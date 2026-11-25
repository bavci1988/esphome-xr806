#include "esphome/core/defines.h"

#ifdef USE_TUYA_LOW_POWER

#include "tuya_uart.h"

#include <cctype>
#include <algorithm>
#include <array>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/util.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif

namespace esphome::tuya {

ESPHOME_LOG_TAG(LP_TAG, "tuya.low_power");
// A version-3 MCU (firmware 1.0.9) reports changes made at the device only when asked.
static constexpr uint32_t LP_V3_STATUS_POLL_MS = 60000;
// Until its first report, the status query of a version-3 MCU is repeated when the answer was lost.
static constexpr uint32_t LP_V3_STATUS_RETRY_MS = 3000;
static constexpr uint32_t LP_COMMAND_DELAY_MS = 10;
// The HC32 sleeps within a second of its last byte; after this long without one it is woken before a command.
static constexpr uint32_t LP_MCU_SILENT_MS = 200;
// Pause between attempts to grant an earlier sleep request while the network services stay busy.
static constexpr uint32_t LP_DEFERRED_SLEEP_RETRY_MS = 10000;
// sm=2: quiet time before the module offers sleep with 0x91, and how long it waits for the answer.
// The original T3-U firmware uses the same 5 s.
static constexpr uint32_t LP_V3_SLEEP_IDLE_MS = 5000;
static constexpr uint32_t LP_V3_OFFER_TIMEOUT_MS = 1000;
static constexpr uint8_t LP_V3_SLEEP_CMD = 0x91;
static constexpr uint8_t LP_V3_SLEEP_OFFER = 0x00;
static constexpr uint8_t LP_V3_SLEEP_WAKE = 0x01;
static constexpr uint8_t LP_V3_SLEEP_REQUEST = 0x02;
// Reply flags to the MCU's own sleep request, as in the original firmware.
static constexpr uint8_t LP_V3_BUSY_NETWORK = 0x02;
static constexpr uint8_t LP_V3_BUSY_UPGRADE = 0x04;

// Scheduler ids continue those in tuya_uart.cpp. The network status shares the Wi-Fi status slot
// there, because a hub runs one protocol only.
static constexpr uint32_t LP_NETWORK_STATUS_INTERVAL_ID = 1;
// After the quick CMD01 retries fail, ask again every 30 s until the MCU answers.
static constexpr uint32_t LP_PRODUCT_SLOW_RETRY_MS = 30000;
// Pending writes; a newer write to the same datapoint replaces the queued one.
static constexpr size_t LP_MAX_QUEUED_COMMANDS = 8;

// A queued datapoint write. It is CMD09 for version 0 and becomes the standard CMD06 for version 3 when it is
// sent, because the version is only known from the product info.
static constexpr auto LP_WRITE_CMD = static_cast<TuyaCommandType>(0x09);
static bool is_low_power_write(const TuyaCommand &command) { return command.cmd == LP_WRITE_CMD; }
// The network state (CMD02), queued so it does not cross a write
static constexpr auto LP_NETWORK_CMD = static_cast<TuyaCommandType>(0x02);

// A datapoint list the decoder takes whole: types and sizes as handle_datapoints_() accepts them.
static bool valid_datapoints(const uint8_t *data, size_t len) {
  if (len < 4)
    return false;
  for (size_t offset = 0; offset < len;) {
    if (len - offset < 4)
      return false;
    const size_t size = encode_uint16(data[offset + 2], data[offset + 3]);
    const uint8_t type = data[offset + 1];
    if (size > len - offset - 4 || type > 5 || ((type == 1 || type == 4) && size != 1) || (type == 2 && size != 4) ||
        (type == 5 && size != 1 && size != 2 && size != 4) || (type == 1 && data[offset + 4] > 1))
      return false;
    offset += 4 + size;
  }
  return true;
}

void TuyaUART::setup_low_power_() {
  this->init_state_ = TuyaInitState::INIT_PRODUCT;
  this->lp_uart_rx_errors_.reset(this->low_power_uart_rx_error_count_());
  if (this->mcu_wake_pin_ == nullptr && this->lp_host_ != nullptr) {
    // Learned from an earlier product info: a sleeping MCU answers CMD01 only after a wake pulse.
    this->mcu_wake_pin_ = this->lp_host_->mcu_wake_output();
    this->lp_mcu_wake_from_host_ = this->mcu_wake_pin_ != nullptr;
  }
  if (this->mcu_wake_pin_ != nullptr) {
    this->mcu_wake_pin_->setup();
    this->mcu_wake_pin_->digital_write(false);  // Inactive logical level.
  }
  if (this->lp_host_ != nullptr) {
    this->lp_host_->require_external_mcu();
    this->update_low_power_sleep_lock_();
  }
  // The original WXU probes CMD01 at each candidate baud, without a heartbeat.
  // With a fixed rate the existing queued CMD01 request is sufficient.
  if (this->auto_baud_) {
    this->lp_autobaud_.start(this->parent_->get_baud_rate());
  } else {
    this->send_low_power_product_query_(false);
  }
  this->set_interval(LP_NETWORK_STATUS_INTERVAL_ID, 1000, [this] { this->send_low_power_network_(); });
}

uint32_t TuyaUART::low_power_uart_rx_error_count_() const {
  return this->lp_host_ != nullptr ? this->lp_host_->uart_rx_error_count() : 0;
}

bool TuyaUART::check_low_power_uart_rx_errors_() {
  const uint32_t count = this->low_power_uart_rx_error_count_();
  if (!this->lp_uart_rx_errors_.observe(count, this->auto_baud_ && this->lp_autobaud_.searching()))
    return false;

  // An already-validated MCU report or ACK could have disappeared from RX.
  // Never grant CMD38/standby using a potentially stale 17-DP snapshot.
  ESP_LOGW(LP_TAG, "UART RX corruption/loss (counter=%u): MCU identity and full status required again",
           static_cast<unsigned>(count));
  this->cancel_low_power_preflight_(true);
  this->cancel_low_power_standby_();
  this->reader_.reset();
  this->lp_session_.command_timeout();
  this->lp_wire_.transport_fault();  // Do not replay an ambiguous CMD09/ACK.
  this->expected_response_.reset();
  // As after an ACK timeout: unsent writes wait for the full resync, queued handshake commands are stale.
  auto &queue = this->command_queue_;
  queue.erase(std::remove_if(queue.begin(), queue.end(), [](const TuyaCommand &c) { return !is_low_power_write(c); }),
              queue.end());
  this->lp_network_status_ = 0xFF;
  this->invalidate_low_power_mode_epoch_();
  // The direct CMD01 retry is permitted while the wire is quarantined;
  // a verified product response will unlock it and request full status.
  this->lp_product_retry_.begin(App.get_loop_component_start_time());
  this->update_low_power_sleep_lock_();
  return true;
}

void TuyaUART::loop_low_power_() {
  // An RX error counted while loop() read the frames is handled before anything is sent
  if (this->check_low_power_uart_rx_errors_())
    this->discard_available();
  this->poll_low_power_product_probe_();
  this->poll_low_power_sleep_preflight_();
  this->poll_low_power_deferred_sleep_();
  if (this->lp_standby_.requested()) {
    auto *host = this->lp_host_;
    if (host == nullptr || !host->standby_requested() || host->ota_busy() || !host->standby_supported() ||
        !host->standby_may_quiesce() || !this->low_power_ready_for_sleep_()) {
      // Withdraw the MCU's logical-sleep signal on any new protocol,
      // network, OTA or application work, before standby has been entered.
      this->cancel_low_power_standby_();
    }
  }
  if (!this->parent_->is_connected()) {
    this->mcu_wake_pulse_running_();  // A running wake pulse still ends on time
    this->expected_response_.reset();
    if (!this->lp_standby_.recovery_required()) {
      this->lp_standby_.transport_fault();
      this->lp_session_.command_timeout();
      this->lp_wire_.transport_fault();
      this->lp_network_status_ = 0xFF;
      ESP_LOGE(LP_TAG, "MCU UART unhealthy: MCU transactions cancelled, standby blocked until recovery");
    }
    if (this->lp_host_ != nullptr)
      this->lp_host_->request_standby(false);
    // Once the UART is back, the product query restores the MCU link.
    this->lp_product_retry_.begin(App.get_loop_component_start_time());
    this->update_low_power_sleep_lock_();
    return;  // No further CMD09 or autobaud probes on a failed transport.
  }
  if (this->auto_baud_) {
    if (this->lp_autobaud_.searching())
      this->poll_low_power_autobaud_();
    if (!this->lp_autobaud_.locked())
      return;  // Never send queued MCU writes/status before CMD01 lock.
  }
  this->poll_low_power_product_recovery_();
  if (this->lp_session_.expire(App.get_loop_component_start_time())) {
    ESP_LOGW(LP_TAG, "Low-power status/write report timeout; requesting state resynchronization");
    this->lp_network_status_ = 0xFF;
  }
  this->update_low_power_sleep_lock_();
  this->process_low_power_queue_();
  this->poll_low_power_v3_status_();
  this->poll_low_power_v3_sleep_();
  this->poll_low_power_update_probe_();
  this->poll_low_power_mcu_update_();
}

void TuyaUART::poll_low_power_v3_status_() {
  const bool synced = this->lp_session_.synchronized();
  // Queued writes wait for the status this asks for, so only other queued commands hold it back. Not while the MCU
  // sleeps under its standby consent or its firmware is updated.
  if (!this->lp_v3_ || !this->lp_v3_mode_acked_ || (synced && this->lp_v3_pushes_) || this->lp_session_.busy() ||
      !this->lp_wire_.idle() || this->reader_.receiving() || this->lp_standby_.requested() ||
      this->lp_mcu_update_ != McuUpdate::MCU_UPDATE_IDLE ||
      !std::all_of(this->command_queue_.begin(), this->command_queue_.end(), is_low_power_write))
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (now - this->lp_v3_last_poll_ms_ < (synced ? LP_V3_STATUS_POLL_MS : LP_V3_STATUS_RETRY_MS))
    return;
  this->lp_v3_last_poll_ms_ = now;
  ESP_LOGV(LP_TAG, "Polling MCU status with CMD08");
  // Queued, so a sleeping MCU is woken first
  this->send_empty_command_(static_cast<TuyaCommandType>(0x08));
}

void TuyaUART::dump_low_power_config_() {
  ESP_LOGCONFIG(LP_TAG, "  Protocol: low_power");
  ESP_LOGCONFIG(LP_TAG, "  Auto baud: %s", YESNO(this->auto_baud_));
  if (this->auto_baud_ && this->lp_autobaud_.locked()) {
    ESP_LOGCONFIG(LP_TAG, "  Detected baud: %u", static_cast<unsigned>(this->lp_autobaud_.baud()));
  }
  // The standard "Product" line stays empty: low_power keeps CMD01 in a fixed buffer.
  ESP_LOGCONFIG(LP_TAG, "  MCU product: '%s'", this->lp_product_text_.data());
}

void TuyaUART::on_low_power_rx_byte_() {
  this->lp_v3_idle_since_ms_ = App.get_loop_component_start_time();
  // An incoming byte before CMD38 consent invalidates a pending network
  // preflight. The MCU must see a busy response, not a delayed grant.
  if (this->lp_cmd38_pending_) {
    this->cancel_low_power_preflight_(true);
    this->lp_cmd38_cooldown_until_ = App.get_loop_component_start_time() + 500;
  }
  // The first arriving byte, not only a fully checksummed frame, must
  // preempt a provisional CMD38 -> CMD12 -> CPU standby transaction.
  // Otherwise partial/noisy UART RX can race the final PM admission check.
  if (this->lp_standby_.requested())
    this->cancel_low_power_standby_();
}

void TuyaUART::preempt_low_power_sleep_() {
  if (this->lp_cmd38_pending_)
    this->cancel_low_power_preflight_(true);
  if (this->lp_standby_.requested())
    this->cancel_low_power_standby_();
}

void TuyaUART::handle_low_power_frame_(uint8_t command, uint8_t version, const uint8_t *buffer, size_t len) {
  // Only a valid frame shows that the MCU is awake; noise on the line of a sleeping MCU does not
  this->lp_mcu_heard_ms_ = millis();
  if (this->auto_baud_ && this->lp_autobaud_.searching()) {
    // handle_frame_ already checked the full checksum. Keep the scanner
    // independent so bad/wrong frames cannot unlock the boot state machine.
    // buffer is the payload inside the whole frame: 6 header bytes before it, the checksum after it.
    if (!this->lp_autobaud_.accept_frame(buffer - TuyaLength::KNOWN_AT, len + TuyaLength::KNOWN_AT + 1,
                                         App.get_loop_component_start_time())) {
      // Show what the MCU sent, so a reply the scan does not recognize can be diagnosed.
      char hex[3 * 12 + 1];
      const size_t shown = len < 12 ? len : 12;
      for (size_t i = 0; i < shown; ++i)
        snprintf(hex + 3 * i, 4, "%02X ", buffer[i]);
      hex[3 * shown] = '\0';
      ESP_LOGD(LP_TAG, "Baud scan ignored CMD 0x%02X v%u len %u: %s| %.*s", command, version,
               static_cast<unsigned>(len), hex, static_cast<int>(len > 120 ? 120 : len),
               reinterpret_cast<const char *>(buffer));
      this->lp_autobaud_.probe_again(App.get_loop_component_start_time());
      return;
    }
    ESP_LOGI(LP_TAG, "Tuya low_power baud locked: %u", static_cast<unsigned>(this->lp_autobaud_.baud()));
  }

  // CMD01/CMD02/CMD09 are answered by the matching command; a version-3 write (CMD06) or status query (CMD08) by
  // the CMD07 report. CMD05 reports never consume a queued command.
  // A matching reply delivered after its deadline but before the periodic
  // timeout sweep must not complete an expired transaction.
  if (this->lp_wire_.overdue(App.get_loop_component_start_time()) && this->lp_wire_.accepts(command, len)) {
    ESP_LOGW(LP_TAG, "Discarding overdue MCU ACK CMD 0x%02X; recovery required", command);
    return;
  }
  if (this->lp_wire_.complete(command, len))
    this->expected_response_.reset();
  this->lp_mcu_wake_fresh_ = false;  // The MCU answered; a version-0 MCU may now fall asleep again.
  this->handle_low_power_command_(command, version, buffer, len);
  // A frame shows that the MCU is awake right now: during a recovery, ask for the product info at once instead
  // of waiting for a retry that may reach it asleep.
  const uint32_t now = App.get_loop_component_start_time();
  if (command != 0x01 && this->lp_product_retry_.active() && !this->lp_wire_.active() && !this->lp_session_.busy() &&
      this->lp_probe_pending_ == ProductProbe::PRODUCT_PROBE_NONE &&
      static_cast<int32_t>(now - this->lp_product_probe_ms_) >=
          static_cast<int32_t>(TuyaLowPowerProductRetry::RETRY_MS)) {
    this->lp_product_probe_ms_ = now;
    this->send_product_query_now_(true);
  }
}

void TuyaUART::send_low_power_raw_command_(const TuyaCommand &command) {
  this->preempt_low_power_sleep_();
  this->last_command_timestamp_ = millis();
  if (!this->begin_low_power_command_(command))
    return;
  ESP_LOGV(LP_TAG, "Sending MCU CMD=0x%02X len=%u", static_cast<uint8_t>(command.cmd),
           static_cast<unsigned>(command.payload.size()));
  // Same frame as the standard path: 55 AA, version 0, command, length, payload, checksum.
  this->send_low_power_direct_(static_cast<uint8_t>(command.cmd), command.payload.data(), command.payload.size());
}

bool TuyaUART::begin_low_power_command_(const TuyaCommand &command) {
  const auto cmd = static_cast<uint8_t>(command.cmd);
  if (cmd == 0x01 || cmd == 0x02 || cmd == 0x06 || cmd == 0x08 || cmd == 0x09) {
    if (!this->lp_wire_.begin(cmd, App.get_loop_component_start_time())) {
      ESP_LOGE(LP_TAG, "Refusing overlapping/ambiguous low-power MCU wire transaction");
      this->lp_session_.command_timeout();
      this->lp_wire_.transport_fault();
      this->lp_product_retry_.begin(App.get_loop_component_start_time());
      return false;
    }
    this->expected_response_ = command.cmd;
  }
  if (cmd == 0x02 && command.payload.size() == 1 && command.payload[0] == 0x04 && !this->lp_v3_)
    this->lp_session_.begin_full_status(App.get_loop_component_start_time());
  if (cmd == 0x06 && this->lp_v3_) {
    // Its CMD07 echo is a reply, not a report of its own.
    this->lp_v3_last_poll_ms_ = App.get_loop_component_start_time();
  }
  if (cmd == 0x09 && command.payload.size() >= 4) {
    const auto &p = command.payload;
    const size_t size = (static_cast<size_t>(p[2]) << 8) | p[3];
    if (p.size() == size + 4)
      this->lp_session_.begin_write(p[0], p[1], p.data() + 4, size, App.get_loop_component_start_time());
  }
  this->update_low_power_sleep_lock_();
  return true;
}

void TuyaUART::send_low_power_datapoint_(uint8_t datapoint_id, TuyaDatapointType datapoint_type, const uint8_t *data,
                                         size_t len) {
  if (this->lp_standby_.requested() && this->lp_session_.ready() && this->lp_wire_.idle() &&
      this->lp_host_ != nullptr && this->lp_host_->standby_keeps_network()) {
    // A write from the network while the MCU sleeps under its standby consent: end the consent,
    // wake the MCU, then send CMD12/00 and the write once the wake pulse is over.
    const bool compensate = this->lp_standby_.force_cancel();
    this->lp_host_->request_standby(false);
    this->begin_mcu_wake_pulse_();
    if (compensate) {
      if (this->lp_mcu_wake_pulse_active_) {
        this->lp_awake_notification_pending_ = true;
      } else {
        this->send_low_power_awake_notification_();
      }
    }
    this->update_low_power_sleep_lock_();
  }
  if (this->lp_mcu_update_ != McuUpdate::MCU_UPDATE_IDLE) {
    ESP_LOGW(LP_TAG, "Refusing DP%u write while the MCU firmware is updated", static_cast<unsigned>(datapoint_id));
    return;
  }
  // The original MCU DP102 handler blindly reads four bytes even for
  // RAW1 and can consume checksum/buffer memory. Reject unsafe writes
  // BEFORE serialization, regardless of whether a higher-level entity
  // calls a forced setter or sends an unknown datapoint.
  if (!TuyaLowPowerSleepWindow::valid_datapoint(datapoint_id, static_cast<uint8_t>(datapoint_type), data, len)) {
    ESP_LOGE(LP_TAG, "Refusing malformed MCU DP%u (type=%u, len=%u)", datapoint_id,
             static_cast<unsigned>(datapoint_type), static_cast<unsigned>(len));
    return;
  }
  if (datapoint_id == TuyaLowPowerSleepWindow::ENABLE_DP || datapoint_id == TuyaLowPowerSleepWindow::WINDOW_DP) {
    this->lp_sleep_window_.on_write(datapoint_id);
    this->invalidate_low_power_mode_epoch_();
  }
  std::vector<uint8_t> buffer{datapoint_id, static_cast<uint8_t>(datapoint_type), static_cast<uint8_t>(len >> 8),
                              static_cast<uint8_t>(len)};
  buffer.insert(buffer.end(), data, data + len);
  // Writes wait in the queue until the MCU's state is known; a newer value replaces a queued one.
  for (auto &queued : this->command_queue_) {
    if (is_low_power_write(queued) && queued.payload[0] == datapoint_id) {
      queued.payload = std::move(buffer);
      return;
    }
  }
  if (this->command_queue_.size() >= LP_MAX_QUEUED_COMMANDS) {
    ESP_LOGW(LP_TAG, "Too many pending MCU commands; dropping the DP%u write", static_cast<unsigned>(datapoint_id));
    return;
  }
  this->command_queue_.push_back(TuyaCommand{.cmd = LP_WRITE_CMD, .payload = std::move(buffer)});
  this->update_low_power_sleep_lock_();
}

void TuyaUART::begin_mcu_wake_pulse_() {
  if (this->mcu_wake_pin_ == nullptr || this->lp_mcu_wake_pulse_active_)
    return;
  this->mcu_wake_pin_->digital_write(true);
  // millis(), not the loop time: the pulse may start late in a loop pass and must last its full length.
  this->lp_mcu_wake_pulse_until_ = millis() + this->lp_product_.mcu_wake_pulse_ms;
  this->lp_mcu_wake_pulse_active_ = true;
  this->lp_mcu_asleep_ = false;
}

// Ends a finished wake pulse and sends what waited for it. True while the pulse still runs:
// nothing may go to the MCU before it has woken.
bool TuyaUART::mcu_wake_pulse_running_() {
  if (!this->lp_mcu_wake_pulse_active_)
    return false;
  if (static_cast<int32_t>(millis() - this->lp_mcu_wake_pulse_until_) < 0)
    return true;
  this->mcu_wake_pin_->digital_write(false);
  this->lp_mcu_wake_pulse_active_ = false;
  // Counted from the end of the pulse, so the command goes out before the MCU falls asleep again.
  this->lp_mcu_woken_ms_ = millis();
  this->lp_mcu_wake_fresh_ = true;
  if (this->lp_awake_notification_pending_) {
    this->lp_awake_notification_pending_ = false;
    this->send_low_power_awake_frame_();
  }
  return false;
}

void TuyaUART::send_low_power_product_query_(bool direct_probe) {
  // The original WXU calls its MCU-wake helper *before* CMD01. We pulse
  // only when a wake GPIO is explicitly configured and its polarity verified.
  // The GPIO configuration supports inverted pins for active-low wiring.
  if (this->mcu_wake_pin_ != nullptr) {
    // CMD01 goes out when the pulse is over; the queue uses the same pulse, so the pin has one owner.
    this->lp_probe_pending_ = direct_probe ? ProductProbe::PRODUCT_PROBE_DIRECT : ProductProbe::PRODUCT_PROBE_QUEUED;
    this->begin_mcu_wake_pulse_();
    return;
  }
  this->send_product_query_now_(direct_probe);
}

void TuyaUART::send_product_query_now_(bool direct_probe) {
  if (direct_probe) {
    this->send_low_power_direct_(0x01, {});
  } else {
    this->send_empty_command_(TuyaCommandType::PRODUCT_QUERY);
  }
}

void TuyaUART::poll_low_power_product_probe_() {
  if (this->lp_probe_pending_ == ProductProbe::PRODUCT_PROBE_NONE || this->mcu_wake_pulse_running_())
    return;
  const bool direct_probe = this->lp_probe_pending_ == ProductProbe::PRODUCT_PROBE_DIRECT;
  this->lp_probe_pending_ = ProductProbe::PRODUCT_PROBE_NONE;
  this->send_product_query_now_(direct_probe);
}

void TuyaUART::poll_low_power_autobaud_() {
  const uint32_t now = App.get_loop_component_start_time();
  if (!this->lp_autobaud_.probe_due(now)) {
    return;
  }

  const uint32_t baud = this->lp_autobaud_.baud();
  // Bytes received at a wrong candidate rate fail the frame checksum and are dropped.
  // A repeated probe at the same rate must not restart the UART.
  if (this->parent_->get_baud_rate() != baud) {
    this->parent_->set_baud_rate(baud);
    this->parent_->load_settings(false);
    this->reader_.reset();  // Drop a partial frame from the previous rate.
  }
  if (!this->parent_->is_connected()) {
    ESP_LOGE(LP_TAG, "Unable to configure MCU UART for %u baud", static_cast<unsigned>(baud));
    this->lp_autobaud_.stop();
    this->init_failed_ = true;
    this->status_set_warning(LOG_STR("MCU UART baud rate"));
    return;
  }
  this->lp_autobaud_.probe_sent(App.get_loop_component_start_time());
  this->send_low_power_product_query_(true);
  if (this->lp_autobaud_.attempts() % 8 == 0) {
    ESP_LOGW(LP_TAG, "Still scanning MCU UART (9600/115200), %u probes",
             static_cast<unsigned>(this->lp_autobaud_.attempts()));
  } else {
    ESP_LOGD(LP_TAG, "Autobaud CMD01 probe at %u", static_cast<unsigned>(baud));
  }
}

void TuyaUART::invalidate_low_power_mode_epoch_() {
  // The HC32's 'sm' is computed by its RTC task, not by the WXU. A write or
  // changed report for DP101/102 makes the last product-info selection stale.
  this->lp_sleep_window_.invalidate_product();
  if (this->lp_session_.has_product()) {
    this->lp_mode_requery_pending_ = true;
    this->lp_mode_requery_after_ms_ = App.get_loop_component_start_time() + 500;
  }
  this->update_low_power_sleep_lock_();
}

void TuyaUART::poll_low_power_product_recovery_() {
  if (this->init_failed_) {
    return;
  }
  const uint32_t now = App.get_loop_component_start_time();
  // Never inject an autonomous CMD01 during a report burst or a write ACK.
  // After an ambiguous ACK timeout, queued writes must not block the
  // direct CMD01 recovery probe used to re-establish the MCU identity.
  const bool idle = !this->lp_session_.busy() && !this->lp_wire_.active() &&
                    (this->lp_wire_.faulted() || this->command_queue_.empty()) &&
                    !this->expected_response_.has_value() && !this->reader_.receiving();
  if (this->lp_product_retry_.active()) {
    if (!this->lp_product_retry_.due(now) || !idle) {
      return;
    }
    if (this->lp_product_retry_.exhausted()) {
      // The MCU may sleep without a wake line, or be gone: keep asking, more slowly, and keep listening.
      this->lp_mode_requery_pending_ = false;
      this->lp_session_.command_timeout();
      if (!this->status_has_warning()) {
        ESP_LOGW(LP_TAG, "MCU does not answer the product query; asking again every %u s",
                 static_cast<unsigned>(LP_PRODUCT_SLOW_RETRY_MS / 1000));
      }
      this->status_set_warning(LOG_STR("MCU does not answer"));
      this->lp_product_retry_.success();
      this->lp_product_retry_.begin(now + LP_PRODUCT_SLOW_RETRY_MS - TuyaLowPowerProductRetry::RETRY_MS);
      return;
    }
    this->lp_product_retry_.query_sent(now);
    this->lp_product_probe_ms_ = now;
    this->send_low_power_product_query_(true);
    return;
  }

  if (this->lp_mode_requery_pending_ && idle && static_cast<int32_t>(now - this->lp_mode_requery_after_ms_) >= 0) {
    this->lp_mode_requery_pending_ = false;
    // Unlike original cached sm, our MCU component can revalidate a new
    // product epoch without resetting the platform.
    this->send_low_power_product_query_(false);
  }
}

bool TuyaUART::low_power_ready_for_sleep_(bool in_cmd38_handler) const {
  // Called from the CMD38 handler, the frame being handled is not a frame still coming in.
  const bool guard_elapsed =
      static_cast<int32_t>(App.get_loop_component_start_time() - this->lp_wake_guard_until_) >= 0;
  const bool baud_locked = !this->auto_baud_ || this->lp_autobaud_.locked();
  // The DP101/DP102 sleep window exists only on the HC32 firmware (version 0 frames).
  return this->lp_session_.ready() && this->lp_wire_.idle() &&
         (this->lp_v3_ || this->lp_sleep_window_.standby_mode_stable()) && !this->lp_product_retry_.active() &&
         guard_elapsed && baud_locked && this->parent_->is_connected() &&
         this->lp_uart_rx_errors_.unchanged(this->low_power_uart_rx_error_count_()) &&
         !this->lp_standby_.recovery_required() && this->command_queue_.empty() && !this->lp_mcu_wake_pulse_active_ &&
         !this->lp_awake_notification_pending_ && !this->expected_response_.has_value() &&
         this->lp_mcu_update_ == McuUpdate::MCU_UPDATE_IDLE && (in_cmd38_handler || !this->reader_.receiving());
}

void TuyaUART::update_low_power_sleep_lock_() {
  if (this->lp_host_ != nullptr)
    this->lp_host_->set_mcu_synchronized(this->low_power_ready_for_sleep_());
}

bool TuyaUART::request_low_power_refresh() {
  if (!this->lp_session_.ready() || !this->lp_wire_.idle() || this->expected_response_.has_value())
    return false;
  if (this->lp_standby_.requested()) {
    // A version-0 MCU sleeping under its standby consent is woken first; the refresh waits for the pulse.
    if (this->lp_v3_ || this->lp_host_ == nullptr)
      return false;
    this->wake_low_power_mcu_();
  }
  if (this->lp_v3_) {
    // A version-3 MCU answers the standard status query; DP106 is the HC32's refresh trigger.
    this->lp_v3_last_poll_ms_ = App.get_loop_component_start_time();
    this->send_low_power_direct_(0x08, {});
    return true;
  }
  static constexpr uint8_t REFRESH_VALUE = 0x01;
  this->send_datapoint_command(106, TuyaDatapointType::BOOLEAN, &REFRESH_VALUE, 1);
  return true;
}

bool TuyaUART::low_power_probe_mcu_update(uint32_t image_size) {
  if (!this->lp_session_.has_product()) {
    ESP_LOGW(LP_TAG, "MCU update probe: MCU not identified yet");
    return false;
  }
  this->wake_low_power_mcu_();
  this->lp_update_probe_size_ = image_size;
  this->lp_update_probe_ = UpdateProbe::UPDATE_PROBE_PENDING;
  return true;
}

// Wakes the MCU for a command of our own; when it sleeps under its standby consent, the consent ends first.
void TuyaUART::wake_low_power_mcu_() {
  this->lp_cmd38_deferred_ = false;
  if (this->lp_standby_.requested() && this->lp_host_ != nullptr) {
    const bool compensate = this->lp_standby_.force_cancel();
    this->lp_host_->request_standby(false);
    this->begin_mcu_wake_pulse_();
    if (compensate) {
      if (this->lp_mcu_wake_pulse_active_) {
        this->lp_awake_notification_pending_ = true;
      } else {
        this->send_low_power_awake_notification_();
      }
    }
  } else {
    this->begin_mcu_wake_pulse_();
  }
  this->update_low_power_sleep_lock_();
}

bool TuyaUART::low_power_update_mcu(const uint8_t *image, size_t size) {
  if (!this->lp_session_.has_product() || image == nullptr || size == 0) {
    ESP_LOGW(LP_TAG, "MCU update: MCU not identified yet or no image");
    return false;
  }
  if (this->lp_mcu_update_ != McuUpdate::MCU_UPDATE_IDLE) {
    ESP_LOGW(LP_TAG, "MCU update: already running");
    return false;
  }
  this->lp_mcu_update_image_ = image;
  this->lp_mcu_update_size_ = size;
  this->lp_mcu_update_offset_ = 0;
  this->lp_mcu_update_retries_ = 0;
  this->lp_mcu_update_percent_ = 0;
  this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_START_PENDING;
  this->wake_low_power_mcu_();
  ESP_LOGI(LP_TAG, "MCU update: starting, %u bytes", static_cast<unsigned>(size));
  return true;
}

void TuyaUART::fail_low_power_mcu_update_(const char *reason) {
  // The MCU installs nothing without a complete image with a valid checksum; it keeps running its firmware.
  ESP_LOGE(LP_TAG, "MCU update failed at %u of %u bytes: %s; the MCU keeps its firmware",
           static_cast<unsigned>(this->lp_mcu_update_offset_), static_cast<unsigned>(this->lp_mcu_update_size_),
           reason);
  this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_IDLE;
  this->update_low_power_sleep_lock_();
}

void TuyaUART::send_low_power_update_packet_() {
  const uint32_t offset = this->lp_mcu_update_offset_;
  const uint32_t rest = this->lp_mcu_update_size_ - offset;
  this->lp_mcu_update_chunk_ = rest < this->lp_mcu_update_packet_ ? rest : this->lp_mcu_update_packet_;
  std::vector<uint8_t> payload;
  payload.reserve(4 + this->lp_mcu_update_chunk_);
  payload.push_back(offset >> 24);
  payload.push_back(offset >> 16);
  payload.push_back(offset >> 8);
  payload.push_back(offset);
  payload.insert(payload.end(), this->lp_mcu_update_image_ + offset,
                 this->lp_mcu_update_image_ + offset + this->lp_mcu_update_chunk_);
  this->send_low_power_direct_(this->low_power_update_data_cmd_(), payload.data(), payload.size());
  this->lp_mcu_update_sent_ms_ = App.get_loop_component_start_time();
  this->lp_mcu_update_ =
      this->lp_mcu_update_chunk_ == 0 ? McuUpdate::MCU_UPDATE_FINAL_SENT : McuUpdate::MCU_UPDATE_PACKET_SENT;
}

void TuyaUART::poll_low_power_mcu_update_() {
  const uint32_t now = App.get_loop_component_start_time();
  switch (this->lp_mcu_update_) {
    case McuUpdate::MCU_UPDATE_START_PENDING: {
      if (this->lp_mcu_wake_pulse_active_ || this->lp_awake_notification_pending_ || !this->lp_wire_.idle())
        return;
      const uint32_t size = this->lp_mcu_update_size_;
      this->send_low_power_direct_(this->low_power_update_start_cmd_(),
                                   {static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                                    static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)});
      this->lp_mcu_update_sent_ms_ = now;
      this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_START_SENT;
      return;
    }
    case McuUpdate::MCU_UPDATE_START_SENT:
      if (now - this->lp_mcu_update_sent_ms_ > 3000)
        this->fail_low_power_mcu_update_("no answer to the start");
      return;
    case McuUpdate::MCU_UPDATE_ERASE_WAIT:
      // The erase command has no answer and blocks the MCU for the whole erase, so wait before packets.
      if (now - this->lp_mcu_update_sent_ms_ < LOW_POWER_UPDATE_ERASE_MS)
        return;
      this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_SEND_PACKET;
      return;
    case McuUpdate::MCU_UPDATE_SEND_PACKET:
      if (this->lp_mcu_wake_pulse_active_)
        return;
      this->send_low_power_update_packet_();
      return;
    case McuUpdate::MCU_UPDATE_PACKET_SENT:
      if (now - this->lp_mcu_update_sent_ms_ < 2000)
        return;
      if (++this->lp_mcu_update_retries_ > 3) {
        this->fail_low_power_mcu_update_("packet not acknowledged");
        return;
      }
      ESP_LOGW(LP_TAG, "MCU update: no acknowledgement at %u, sending again",
               static_cast<unsigned>(this->lp_mcu_update_offset_));
      this->begin_mcu_wake_pulse_();  // The MCU may have fallen asleep.
      this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_SEND_PACKET;
      return;
    case McuUpdate::MCU_UPDATE_FINAL_SENT:
      // A good image makes the MCU restart without an answer; an acknowledgement means its check failed.
      if (now - this->lp_mcu_update_sent_ms_ < 10000)
        return;
      ESP_LOGI(LP_TAG, "MCU update: transfer complete, the MCU installs the image and restarts; restarting the module");
      this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_IDLE;
      App.safe_reboot();
      return;
    default:
      return;
  }
}

// Returns true when the frame belonged to the running update.
bool TuyaUART::handle_low_power_mcu_update_(uint8_t command, const uint8_t *data, size_t len) {
  if (this->lp_mcu_update_ == McuUpdate::MCU_UPDATE_START_SENT && command == this->low_power_update_start_cmd_()) {
    // Packet size: 0 = 256, 1 = 512, 2 = 1024 bytes
    const uint8_t code = len >= 1 ? data[0] : 0;
    this->lp_mcu_update_packet_ = code == 2 ? 1024 : code == 1 ? 512 : 256;
    ESP_LOGI(LP_TAG, "MCU update: MCU ready, packets of %u bytes", this->lp_mcu_update_packet_);
    if (this->lp_v3_) {
      // The version-3 start command already erased the staging area.
      this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_SEND_PACKET;
    } else {
      // Erase the staging area first; flash keeps old bits otherwise and the checksum fails.
      ESP_LOGI(LP_TAG, "MCU update: erasing the MCU staging area");
      this->send_low_power_direct_(LOW_POWER_UPDATE_ERASE_CMD, {0x00});
      this->lp_mcu_update_sent_ms_ = App.get_loop_component_start_time();
      this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_ERASE_WAIT;
    }
    return true;
  }
  if (command != this->low_power_update_data_cmd_() || len != 0)
    return false;
  if (this->lp_mcu_update_ == McuUpdate::MCU_UPDATE_PACKET_SENT) {
    this->lp_mcu_update_offset_ += this->lp_mcu_update_chunk_;
    this->lp_mcu_update_retries_ = 0;
    const uint8_t percent = static_cast<uint8_t>(100ull * this->lp_mcu_update_offset_ / this->lp_mcu_update_size_);
    if (percent / 10 != this->lp_mcu_update_percent_ / 10) {
      ESP_LOGI(LP_TAG, "MCU update: %u%%", percent);
    }
    this->lp_mcu_update_percent_ = percent;
    // After the last packet an empty one at offset = size ends the transfer.
    this->lp_mcu_update_ = McuUpdate::MCU_UPDATE_SEND_PACKET;
    return true;
  }
  if (this->lp_mcu_update_ == McuUpdate::MCU_UPDATE_FINAL_SENT) {
    this->fail_low_power_mcu_update_("the MCU rejected the image checksum");
    return true;
  }
  return false;
}

// Sends a pending probe once the MCU is awake, and reports a missing answer.
void TuyaUART::poll_low_power_update_probe_() {
  if (this->lp_update_probe_ == UpdateProbe::UPDATE_PROBE_PENDING) {
    if (this->lp_mcu_wake_pulse_active_ || this->lp_awake_notification_pending_ || !this->lp_wire_.idle())
      return;
    const uint32_t size = this->lp_update_probe_size_;
    const uint8_t cmd = this->low_power_update_start_cmd_();
    this->send_low_power_direct_(cmd, {static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                                       static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)});
    this->lp_update_probe_sent_ms_ = App.get_loop_component_start_time();
    this->lp_update_probe_ = UpdateProbe::UPDATE_PROBE_SENT;
    ESP_LOGI(LP_TAG, "MCU update probe: sent CMD%02X with image size %u (start only, no data)", cmd,
             static_cast<unsigned>(size));
    return;
  }
  if (this->lp_update_probe_ == UpdateProbe::UPDATE_PROBE_SENT &&
      App.get_loop_component_start_time() - this->lp_update_probe_sent_ms_ > 3000) {
    this->lp_update_probe_ = UpdateProbe::UPDATE_PROBE_IDLE;
    ESP_LOGW(LP_TAG, "MCU update probe: no answer to CMD%02X within 3 s", this->low_power_update_start_cmd_());
  }
}

void TuyaUART::send_low_power_awake_notification_() {
  if (this->lp_v3_ && this->mcu_wake_pin_ != nullptr) {
    // A version-3 MCU sleeps after its consent and listens to the UART only after a wake pulse.
    this->begin_mcu_wake_pulse_();
    if (this->lp_mcu_wake_pulse_active_) {
      this->lp_awake_notification_pending_ = true;
      this->lp_wake_guard_until_ = App.get_loop_component_start_time() + 5000;
      this->update_low_power_sleep_lock_();
      return;
    }
  }
  this->send_low_power_awake_frame_();
}

void TuyaUART::send_low_power_awake_frame_() {
  // This is a Tuya MCU command, not a platform wake operation.
  if (!this->parent_->is_connected()) {
    this->lp_standby_.transport_fault();
  } else {
    if (this->lp_v3_) {
      this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_WAKE, 0x01});
      // The MCU pushes its own changes. Only after a fault is its state read again; that report ends the recovery.
      if (this->lp_standby_.recovery_required()) {
        this->lp_v3_last_poll_ms_ = App.get_loop_component_start_time();
        this->send_low_power_direct_(0x08, {});
      }
    } else {
      this->send_low_power_direct_(0x12, {0x00});
    }
    if (this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS)
      this->lp_standby_.transport_fault();
  }
  this->lp_wake_guard_until_ = App.get_loop_component_start_time() + 5000;
  this->update_low_power_sleep_lock_();
}

void TuyaUART::configure_low_power_wake_lines_() {
  if (this->lp_wake_lines_configured_ || this->lp_host_ == nullptr)
    return;
  this->lp_wake_lines_configured_ = true;
  // The product info names both wake lines, so the configuration does not have to.
  const bool create_output = this->mcu_wake_pin_ == nullptr || this->lp_mcu_wake_from_host_;
  this->lp_wake_input_ =
      this->lp_host_->configure_wake_lines(this->lp_product_.wake_gpio, this->lp_product_.mcu_wake_gpio,
                                           this->lp_product_.mcu_wake_active_high, create_output);
  if (create_output)
    this->mcu_wake_pin_ = this->lp_host_->mcu_wake_output();
  if (create_output && this->mcu_wake_pin_ != nullptr) {
    this->lp_mcu_wake_from_host_ = true;
    this->mcu_wake_pin_->setup();
    this->mcu_wake_pin_->digital_write(false);
  }
}

bool TuyaUART::low_power_v3_sleep_supported_() const {
  auto *host = this->lp_host_;
  return this->lp_v3_ && this->lp_product_.sleep_mode == 2 && host != nullptr && host->standby_supported() &&
         this->lp_wake_input_ >= 0 && host->standby_wake_pin() == this->lp_wake_input_ &&
         host->standby_requires_external_sync() && host->standby_may_quiesce();
}

void TuyaUART::poll_low_power_v3_sleep_() {
  if (!this->lp_v3_ || this->lp_product_.sleep_mode != 2)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (this->lp_v3_offer_pending_) {
    if (static_cast<int32_t>(now - this->lp_v3_offer_deadline_ms_) >= 0) {
      this->lp_v3_offer_pending_ = false;
      this->lp_v3_idle_since_ms_ = now;
      ESP_LOGD(LP_TAG, "MCU did not answer the sleep offer");
    }
    return;
  }
  if (this->lp_standby_.requested() || now - this->lp_v3_idle_since_ms_ < LP_V3_SLEEP_IDLE_MS)
    return;
  auto *host = this->lp_host_;
  const LogString *blocked = nullptr;
  bool link_busy = false;  // The usual short pause after a wake: not worth a DEBUG line.
  if (host == nullptr || !host->standby_supported()) {
    blocked = LOG_STR("no standby controller");
  } else if (this->lp_wake_input_ < 0 || host->standby_wake_pin() != this->lp_wake_input_) {
    blocked = LOG_STR("wake input differs from the MCU's");
  } else if (!host->standby_requires_external_sync()) {
    blocked = LOG_STR("require_external_sync is off");
  } else if (!host->standby_may_quiesce()) {
    blocked = LOG_STR("services busy");
  } else if (host->ota_busy()) {
    blocked = LOG_STR("update running");
  } else if (!this->low_power_ready_for_sleep_() || this->available() != 0) {
    link_busy = true;
  }
  if (link_busy) {
    ESP_LOGV(LP_TAG, "No sleep offer: MCU link not idle (ready %d, wire %d, queue %u, guard %d)",
             this->lp_session_.ready(), this->lp_wire_.idle(), static_cast<unsigned>(this->command_queue_.size()),
             static_cast<int32_t>(now - this->lp_wake_guard_until_) >= 0);
    return;
  }
  if (blocked != this->lp_v3_sleep_blocked_) {
    this->lp_v3_sleep_blocked_ = blocked;
    if (blocked != nullptr) {
      ESP_LOGD(LP_TAG, "No sleep offer: %s (blockers 0x%04X)", LOG_STR_ARG(blocked),
               static_cast<unsigned>(host != nullptr ? host->standby_blockers() : 0));
    } else {
      ESP_LOGD(LP_TAG, "Sleep offers to the MCU start");
    }
  }
  if (blocked != nullptr)
    return;
  ESP_LOGV(LP_TAG, "Offering sleep to the MCU");
  this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_OFFER});
  this->lp_v3_offer_pending_ = true;
  this->lp_v3_offer_deadline_ms_ = now + LP_V3_OFFER_TIMEOUT_MS;
}

void TuyaUART::handle_low_power_v3_sleep_(const uint8_t *data, size_t len) {
  if (!this->lp_v3_ || len < 1)
    return;
  auto *host = this->lp_host_;
  switch (data[0]) {
    case LP_V3_SLEEP_OFFER: {  // The MCU's answer to our offer: 01 = sleep, 00 = stay awake.
      if (!this->lp_v3_offer_pending_ || len < 2)
        return;
      this->lp_v3_offer_pending_ = false;
      this->lp_v3_idle_since_ms_ = App.get_loop_component_start_time();
      if (data[1] != this->lp_v3_last_answer_) {
        this->lp_v3_last_answer_ = data[1];
        if (data[1] != 0x01) {
          ESP_LOGD(LP_TAG, "MCU declines sleep (answer %u)", data[1]);
        }
      }
      if (data[1] != 0x01)
        return;
      if (!this->low_power_v3_sleep_supported_() || host->ota_busy() ||
          !this->lp_standby_.accept_offer(this->low_power_ready_for_sleep_(true), this->available() == 0, true)) {
        // The MCU already sleeps and expects the module to sleep too.
        this->send_low_power_awake_notification_();
        return;
      }
      ESP_LOGV(LP_TAG, "MCU agreed to sleep");
      host->request_standby(true);
      this->update_low_power_sleep_lock_();
      return;
    }
    case LP_V3_SLEEP_WAKE:  // The MCU's answer to our wake notice.
      return;
    case LP_V3_SLEEP_REQUEST: {  // The MCU asks to sleep; 00 grants it.
      if (this->lp_standby_.requested()) {
        this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_REQUEST, 0x00});
        return;
      }
      uint8_t flags = 0;
      if (host != nullptr && host->ota_busy())
        flags |= LP_V3_BUSY_UPGRADE;
      if (flags == 0 &&
          (!this->low_power_v3_sleep_supported_() ||
           !this->lp_standby_.accept_offer(this->low_power_ready_for_sleep_(true), this->available() == 0, true)))
        flags |= LP_V3_BUSY_NETWORK;
      this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_REQUEST, flags});
      if (flags != 0)
        return;
      if (this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
        this->lp_standby_.transport_fault();
        this->update_low_power_sleep_lock_();
        return;
      }
      ESP_LOGV(LP_TAG, "MCU asked to sleep");
      host->request_standby(true);
      this->update_low_power_sleep_lock_();
      return;
    }
    default:
      return;
  }
}

void TuyaUART::cancel_low_power_preflight_(bool notify_mcu) {
  if (!this->lp_cmd38_pending_)
    return;
  this->lp_cmd38_pending_ = false;
  if (this->lp_host_ != nullptr)
    this->lp_host_->request_preflight(false);
  if (notify_mcu)
    this->send_low_power_direct_(0x38, {0x00, 0x01});
}

void TuyaUART::poll_low_power_sleep_preflight_() {
  if (!this->lp_cmd38_pending_)
    return;
  auto *host = this->lp_host_;
  if (host == nullptr || !host->standby_supported() || !host->preflight_requested() ||
      static_cast<int32_t>(App.get_loop_component_start_time() - this->lp_cmd38_deadline_ms_) >= 0 ||
      host->ota_busy() || !host->standby_may_quiesce() || !this->low_power_ready_for_sleep_()) {
    this->cancel_low_power_preflight_(true);
    this->lp_cmd38_cooldown_until_ = App.get_loop_component_start_time() + 500;
    return;
  }
  if (!host->standby_services_quiesced())
    return;
  if (this->available() != 0 || this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS ||
      !this->lp_standby_.accept_request(this->lp_product_.sleep_mode == 1, true, true, true)) {
    this->cancel_low_power_preflight_(true);
    return;
  }
  // Never grant CMD38 while network owners still have live sockets, OTA,
  // mDNS callbacks or a WLAN wakelock. The MCU receives its ACK only NOW.
  this->lp_cmd38_pending_ = false;
  if (this->lp_cmd38_deferred_)
    ESP_LOGD(LP_TAG, "Granting the MCU's earlier sleep request");
  this->lp_cmd38_deferred_ = false;
  this->send_low_power_direct_(0x38, {0x00, 0x00});
  if (this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
    host->request_standby(true);
  } else {
    this->lp_standby_.transport_fault();
    this->update_low_power_sleep_lock_();
  }
  host->request_preflight(false);
}

uint8_t TuyaUART::grant_low_power_sleep_() {
  auto *host = this->lp_host_;
  const bool ota_veto = host != nullptr && host->ota_busy();
  const bool supported = host != nullptr && host->standby_supported() && host->standby_wake_pin() >= 0 &&
                         host->standby_requires_external_sync() && host->standby_wake_pin() == this->lp_wake_input_ &&
                         this->lp_product_.sleep_mode == 1 && host->standby_may_quiesce();
  const bool transport_ready = this->low_power_ready_for_sleep_(true) && this->available() == 0;
  if (supported && !ota_veto && transport_ready) {
    if (!host->standby_services_quiesced()) {
      // Start *service* preparation without requesting CPU PM. Do not
      // tell HC32 that WXU accepted sleep before every network owner
      // has actually closed/drained its resources.
      this->lp_cmd38_pending_ = true;
      this->lp_cmd38_deadline_ms_ = App.get_loop_component_start_time() + 1800;
      host->request_preflight(true);
      return 0;  // A bounded later busy/accept response will be sent.
    }
    if (this->lp_standby_.accept_request(true, true, true, true)) {
      if (this->lp_cmd38_deferred_)
        ESP_LOGD(LP_TAG, "Granting the MCU's earlier sleep request");
      this->lp_cmd38_deferred_ = false;
      this->send_low_power_direct_(0x38, {0x00, 0x00});
      if (this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
        host->request_standby(true);
      } else {
        this->lp_standby_.transport_fault();
      }
      this->update_low_power_sleep_lock_();
      return 0;
    }
  }
  return ota_veto ? 0x03 : 0x01;  // Default is *busy*, not a provisional grant.
}

void TuyaUART::poll_low_power_deferred_sleep_() {
  const uint32_t now = App.get_loop_component_start_time();
  if (!this->lp_cmd38_deferred_ || this->lp_cmd38_pending_ || this->lp_standby_.requested() ||
      static_cast<int32_t>(now - this->lp_cmd38_cooldown_until_) < 0 ||
      static_cast<int32_t>(now - this->lp_cmd38_retry_at_) < 0 || this->grant_low_power_sleep_() != 0)
    return;
  // Preparing the network services failed this time (an open connection, for example): try again later.
  if (this->lp_cmd38_pending_)
    this->lp_cmd38_retry_at_ = now + LP_DEFERRED_SLEEP_RETRY_MS;
}

void TuyaUART::cancel_low_power_standby_() {
  const bool compensate = this->lp_standby_.force_cancel();
  if (this->lp_host_ != nullptr)
    this->lp_host_->request_standby(false);
  if (compensate)
    this->send_low_power_awake_notification_();
  this->update_low_power_sleep_lock_();
}

bool TuyaUART::notify_low_power_transition(bool sleeping) {
  if (!this->lp_session_.has_product())
    return false;
  if (sleeping) {
    // The generic coordinator's scheduler/wake votes are intentionally
    // FALSE during prepare: requiring can_enter_standby() here deadlocks.
    if (!this->lp_standby_.needs_notification() || !this->low_power_ready_for_sleep_() || this->available() != 0 ||
        this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS)
      return false;
    this->send_low_power_direct_(0x12, {0x01});
    if (this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
      this->lp_standby_.transport_fault();
      this->update_low_power_sleep_lock_();
      return false;
    }
    this->lp_standby_.notification_sent();
    this->update_low_power_sleep_lock_();
    return true;
  }

  if (!this->lp_standby_.force_cancel())
    return false;
  this->send_low_power_awake_notification_();
  return true;
}

bool TuyaUART::low_power_prepare_for_standby() {
  if (this->lp_host_ == nullptr || !this->lp_standby_.requested() ||
      (this->lp_product_.sleep_mode != 1 && this->lp_product_.sleep_mode != 2) || !this->low_power_ready_for_sleep_() ||
      this->available() != 0)
    return false;
  if (this->lp_standby_.needs_notification()) {
    // Deliberately postpone platform PM; CMD12/01 was transmitted first.
    // standby_cancelled() consumes the intentional first deferral.
    (void) this->notify_low_power_transition(true);
    return false;
  }
  return this->lp_standby_.ready_for_pm() && this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
}

void TuyaUART::low_power_standby_cancelled() {
  const bool compensate = this->lp_standby_.cancelled();
  if (!this->lp_standby_.requested() && this->lp_host_ != nullptr)
    this->lp_host_->request_standby(false);
  if (compensate)
    this->send_low_power_awake_notification_();
  this->update_low_power_sleep_lock_();
}

void TuyaUART::low_power_standby_resumed(bool expected_source, bool wake_gpio, bool wake_timer) {
  const bool keep = this->lp_host_ != nullptr && this->lp_host_->standby_keeps_network();
  // A version-3 MCU is told about every wake that ends its consent (91 01 01 after a wake pulse),
  // not only about one it caused itself; its state is then read again.
  const bool notify_wake = wake_gpio || (this->lp_v3_ && !keep);
  const auto action = this->lp_standby_.resumed(expected_source, notify_wake, keep);
  if (action == TuyaLowPowerStandbyState::ResumeAction::KEEP_CONSENT) {
    if (wake_timer && !this->lp_v3_) {
      // The MCU's own changes can be lost while the module sleeps, so read its state on every timer wake.
      ESP_LOGD(LP_TAG, "Timer wake: reading the MCU state");
      if (this->request_low_power_refresh())
        return;
    }
    // The MCU did not wake us and still sleeps: no CMD12/00, no wake guard, sleep again once idle.
    this->lp_host_->request_standby(true);
    this->update_low_power_sleep_lock_();
    return;
  }
  this->lp_wake_guard_until_ = App.get_loop_component_start_time() + 5000;
  // A retained GPIO wake may receive CMD12/00 only if we previously sent
  // CMD12/01. Timer-only wake must not impersonate the original GPIO path.
  if (action == TuyaLowPowerStandbyState::ResumeAction::SEND_CMD12_WAKE) {
    this->send_low_power_awake_notification_();
  } else if (action == TuyaLowPowerStandbyState::ResumeAction::REQUIRE_RESYNC) {
    this->lp_session_.command_timeout();
    ESP_LOGE(LP_TAG, "Unexpected/timer-only MCU resume: withholding CMD12/00 and further standby");
    // A version-3 MCU sends no boot heartbeat: wake it and read its state to recover.
    if (this->lp_v3_)
      this->send_low_power_awake_notification_();
  }
  this->update_low_power_sleep_lock_();
}

void TuyaUART::process_low_power_queue_() {
  const uint32_t now = App.get_loop_component_start_time();
  if (this->mcu_wake_pulse_running_())
    return;
  // After the pulse check, which may stamp its end; the command timestamp of tuya_uart.cpp comes from millis() too.
  const uint32_t wire_now = millis();

  // Standard Tuya's 300 ms retry logic is incompatible with the HC32's
  // delayed CMD05 report. ACK and report have separate timeouts.
  // The session gate, not an optional ESPHome command field, owns the
  // physical in-flight transaction. It can time out after a reset or an
  // asynchronous recovery callback has already cleared expected_response_.
  const bool product_in_flight = this->lp_wire_.kind() == TuyaLowPowerWireTransactions::Kind::PRODUCT;
  if (this->lp_wire_.expire(now)) {
    const bool product_query = product_in_flight;
    this->expected_response_.reset();
    this->lp_session_.command_timeout();
    this->lp_network_status_ = 0xFF;
    this->invalidate_low_power_mode_epoch_();
    // Recover with the direct CMD01 probe (it wakes the MCU first): the queued re-query of the
    // invalidate helper would wait behind the quarantined queue for good.
    this->lp_product_retry_.begin(now);
    ESP_LOGW(LP_TAG, "MCU command ACK timeout (product=%s); quarantining writes until full resync",
             YESNO(product_query));
    // Unsent writes wait for the full resync, which no late ACK can reach; queued handshake commands are stale.
    auto &queue = this->command_queue_;
    queue.erase(std::remove_if(queue.begin(), queue.end(), [](const TuyaCommand &c) { return !is_low_power_write(c); }),
                queue.end());
  }

  if (this->lp_session_.busy() || this->lp_wire_.active() || this->lp_wire_.faulted() ||
      this->expected_response_.has_value() || this->reader_.receiving() || this->handling_frame_ ||
      static_cast<uint32_t>(wire_now - this->last_command_timestamp_) <= LP_COMMAND_DELAY_MS)
    return;
  // Before the product info only CMD01 may go out. Writes wait until the MCU's state is known, while the MCU
  // sleeps under its standby consent, and during an MCU firmware update.
  // With a wake line, writes also wait until a report series the MCU started on its own is over. Without one,
  // a write must go out while the MCU is still awake from its reports.
  auto next = std::find_if(this->command_queue_.begin(), this->command_queue_.end(), [this, now](const TuyaCommand &c) {
    if (!this->lp_session_.has_product())
      return c.cmd == TuyaCommandType::PRODUCT_QUERY;
    return !is_low_power_write(c) ||
           (this->lp_session_.ready() && (this->mcu_wake_pin_ == nullptr || this->lp_session_.quiet(now)) &&
            !this->lp_standby_.requested() && this->lp_mcu_update_ == McuUpdate::MCU_UPDATE_IDLE);
  });
  if (next == this->command_queue_.end())
    return;
  // A low-power MCU sleeps whenever it is idle and listens only after a pulse on its wake line; the command goes
  // out once the pulse is over. A version-0 MCU can fall asleep right after its own answer, so for it neither a
  // recent byte nor a pulse that it has already answered counts: only a fresh, unanswered pulse.
  const bool awake = this->lp_v3_ ? wire_now - this->lp_mcu_heard_ms_ <= LP_MCU_SILENT_MS ||
                                        wire_now - this->lp_mcu_woken_ms_ <= LP_MCU_SILENT_MS
                                  : this->lp_mcu_wake_fresh_ && wire_now - this->lp_mcu_woken_ms_ <= LP_MCU_SILENT_MS;
  if (this->mcu_wake_pin_ != nullptr && (this->lp_mcu_asleep_ || !awake)) {
    this->begin_mcu_wake_pulse_();
    return;
  }

  TuyaCommand command = std::move(*next);
  this->command_queue_.erase(next);
  if (this->lp_v3_ && is_low_power_write(command))
    command.cmd = static_cast<TuyaCommandType>(0x06);
  // A partial frame kept by an idle flush must not merge into the reply
  this->reader_.drop_kept();
  this->send_low_power_raw_command_(command);
}

void TuyaUART::send_low_power_direct_(uint8_t command, const uint8_t *data, size_t len) {
  // ACKs and time replies must not wait behind a pending CMD09 write.
  if (len > 65535)
    return;
  // A partial frame kept by an idle flush must not merge into the reply
  this->reader_.drop_kept();
  this->lp_v3_idle_since_ms_ = App.get_loop_component_start_time();
  const uint8_t header[] = {
      0x55, 0xAA, 0x00, command, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len),
  };
  this->write_array(header, sizeof(header));
  if (len != 0)
    this->write_array(data, len);
  this->write_byte(sum_bytes(data, len, sum_bytes(header, sizeof(header))));
}

void TuyaUART::send_low_power_network_() {
  if (!this->lp_session_.has_product())
    return;
  // An intentional, pending Wi-Fi teardown for standby is not a new logical
  // Tuya network epoch: defer CMD02 until abort/resume, when the station
  // is restored. External/unplanned link loss remains visible as before.
  if (this->lp_host_ != nullptr && this->lp_host_->network_quiescing() &&
      (this->lp_host_->standby_requested() || this->lp_host_->preflight_requested()))
    return;
  uint8_t status = 0x02;
#ifdef USE_NETWORK
  // 0x04 ("connected to the cloud") only with a client. Each change sends CMD02, also after a standby: the
  // HC32 relies on that traffic after its own wake; a constant 0x04 left its reports unanswered.
  if (network::is_connected())
    status = remote_is_connected() ? 0x04 : 0x03;
#endif
  if (status == this->lp_network_status_)
    return;
  this->lp_network_status_ = status;
  if (this->lp_v3_ && this->lp_v3_mode_acked_) {
    // Standard Tuya framing: the network state is CMD03 (the original T3-U firmware sends it on
    // every change); CMD02 is the working-mode query. The MCU reports changes on its own only
    // after it was told that the module is online. Queued, so a sleeping MCU is woken.
    this->send_command_({.cmd = static_cast<TuyaCommandType>(0x03), .payload = {status}});
    return;
  }
  this->lp_session_.reset_status();
  if (status == 0x04) {
    this->lp_wake_guard_until_ =
        App.get_loop_component_start_time() + (this->lp_product_.sleep_mode == 0 ? 10000 : 5000);
  }
  this->update_low_power_sleep_lock_();
  // A queued CMD02 is updated in place, keeping the order on the wire (version 3: the empty mode query).
  for (auto &queued : this->command_queue_) {
    if (queued.cmd == LP_NETWORK_CMD) {
      if (!queued.payload.empty())
        queued.payload[0] = status;
      return;
    }
  }
  if (this->lp_v3_) {
    this->send_empty_command_(LP_NETWORK_CMD);
  } else {
    this->send_command_({.cmd = LP_NETWORK_CMD, .payload = {status}});
  }
}

void TuyaUART::send_low_power_time_(uint8_t command) {
  // Local calendar time (HC32: CMD06, version-3 MCU: CMD1C). No duplicate timezone/DST adjustment.
  std::array<uint8_t, 8> payload{};
#ifdef USE_TIME
  if (this->time_id_ != nullptr) {
    const auto now = this->time_id_->now();
    if (now.is_valid() && now.year >= 2000 && now.year <= 2255) {
      const uint8_t weekday = static_cast<uint8_t>(now.day_of_week == 1 ? 7 : now.day_of_week - 1);
      payload = {
          1,
          static_cast<uint8_t>(now.year - 2000),
          static_cast<uint8_t>(now.month),
          static_cast<uint8_t>(now.day_of_month),
          static_cast<uint8_t>(now.hour),
          static_cast<uint8_t>(now.minute),
          static_cast<uint8_t>(now.second),
          weekday,
      };
    }
  }
#endif
  if (payload[0] == 0) {
    ESP_LOGW(LP_TAG, "MCU asked for the time before it was known");
  } else {
    ESP_LOGD(LP_TAG, "Sending local time to the MCU (CMD%02X)", command);
  }
  this->send_low_power_direct_(command, payload.data(), payload.size());
}

void TuyaUART::handle_low_power_command_(uint8_t command, uint8_t version, const uint8_t *data, size_t len) {
  (void) version;
  if (this->lp_mcu_update_ != McuUpdate::MCU_UPDATE_IDLE && this->handle_low_power_mcu_update_(command, data, len))
    return;
  if (this->lp_update_probe_ == UpdateProbe::UPDATE_PROBE_SENT && command == this->low_power_update_start_cmd_()) {
    this->lp_update_probe_ = UpdateProbe::UPDATE_PROBE_IDLE;
    char hex[3 * 8 + 1] = "";
    const size_t shown = len < 8 ? len : 8;
    for (size_t i = 0; i < shown; ++i)
      snprintf(hex + 3 * i, 4, "%02X ", data[i]);
    // Standard Tuya answer: one byte, the packet size (0 = 256, 1 = 512, 2 = 1024 bytes).
    ESP_LOGI(LP_TAG, "MCU update probe: answer CMD%02X len %u: %s", command, static_cast<unsigned>(len), hex);
    return;
  }
  // The MCU is active again and asks for sleep itself once it is done.
  if (command != 0x38) {
    this->lp_cmd38_deferred_ = false;
    this->lp_mcu_asleep_ = false;
  }
  // Once CMD38 is provisionally accepted, any new MCU traffic invalidates
  // the quiet interval. Do not race a report burst against CPU standby.
  if (command != 0x38 && command != LP_V3_SLEEP_CMD && this->lp_standby_.requested())
    this->cancel_low_power_standby_();
  switch (command) {
    case 0x00:  // First MCU heartbeat after reset is 00; subsequent is 01.
      if (len != 1 || data[0] != 0)
        return;
      this->lp_standby_.reset_on_mcu_boot();
      // The HC32 may have a new RTC sleep-window marker (sm), including
      // after a PA3 pulse or local UI change; force a fresh product epoch.
      if (this->lp_session_.has_product()) {
        this->invalidate_low_power_mode_epoch_();
        this->lp_sleep_window_.reset_reports();
      }
      if (this->lp_host_ != nullptr)
        this->lp_host_->request_standby(false);
      if (!this->lp_session_.has_product()) {
        // During a recovery the frame handler already sends the product query directly; a queued one would
        // follow its answer and run the full resynchronization a second time.
        if (!this->lp_product_retry_.active())
          this->send_empty_command_(TuyaCommandType::PRODUCT_QUERY);
      } else {
        this->lp_session_.reset_status();
        this->lp_network_status_ = 0xFF;
        this->lp_v3_mode_acked_ = false;  // A restarted version-3 MCU needs the working-mode handshake again
        this->update_low_power_sleep_lock_();
        this->send_low_power_network_();
        this->send_empty_command_(static_cast<TuyaCommandType>(0x13));
      }
      return;

    case 0x01: {  // Product-info response.
      TuyaLowPowerProductInfo incoming{};
      // The descriptor differs between MCU firmware versions; show it once for diagnosis.
      ESP_LOGD(LP_TAG, "MCU product info: %.*s", static_cast<int>(len > 200 ? 200 : len),
               reinterpret_cast<const char *>(data));
      if (!incoming.parse(data, len)) {
        // Original WXU calls CMD01 again after 500 ms for missing descriptors,
        // allocation/JSON failures or absent p/v. Retry only malformed data;
        // never treat its failed initialization marker as a working MCU.
        this->invalidate_low_power_mode_epoch_();
        this->lp_session_.command_timeout();
        this->lp_product_retry_.begin(App.get_loop_component_start_time());
        ESP_LOGW(LP_TAG, "Malformed CMD01 product info: retrying at 500ms intervals (bounded)");
        return;
      }
      // The frame reader limits a frame to MAX_FRAME_SIZE, so the raw descriptor fits our fixed
      // diagnostics buffer without an extra allocation and without an
      // implementation-dependent nothrow-new/abort path.
      std::memcpy(this->lp_product_text_.data(), data, len);
      this->lp_product_text_[len] = '\0';
      const bool had_product = this->lp_session_.has_product();
      this->lp_product_ = incoming;
      this->lp_v3_mode_acked_ = false;
      this->lp_v3_pushes_ = false;
      if (this->lp_v3_ != (version == 3)) {
        this->lp_v3_ = version == 3;
        ESP_LOGI(LP_TAG, "MCU frames with protocol version %u: status %s, writes %s", version,
                 this->lp_v3_ ? LOG_STR_LITERAL("CMD08/CMD07") : LOG_STR_LITERAL("CMD05 burst"),
                 this->lp_v3_ ? LOG_STR_LITERAL("CMD06") : LOG_STR_LITERAL("CMD09"));
      }
      this->lp_product_retry_.success();
      this->status_clear_warning();
      this->lp_mode_requery_pending_ = false;
      this->lp_sleep_window_.product_validated();
      if (had_product) {
        // HC32 RTC/DP101/DP102 may have changed the requested PM mode.
        // Subsequent CMD01 never proves MCU status is still synchronized.
        this->lp_session_.reset_status();
        this->lp_sleep_window_.reset_reports();
        this->lp_network_status_ = 0xFF;
      }
      this->lp_session_.product_received();
      // Revalidated product identity clears the wire quarantine, but not
      // the MCU full-status gate (17 new CMD05 reports are still required).
      if (this->lp_wire_.faulted())
        this->lp_wire_.recovered();
      this->init_state_ = TuyaInitState::INIT_DATAPOINT;
      ESP_LOGI(LP_TAG, "MCU PID %s version %s wk=%d mwk=%d sm=%d", this->lp_product_.pid.data(),
               this->lp_product_.version.data(), this->lp_product_.wake_gpio, this->lp_product_.mcu_wake_gpio,
               this->lp_product_.sleep_mode);
      this->configure_low_power_wake_lines_();
      this->send_low_power_network_();
      return;
    }

    case 0x02:  // Network-state ACK; generic queue handler clears expected CMD.
      if (this->lp_v3_) {
        // For a version-3 MCU CMD02 is the working-mode query. Then, like the original T3-U
        // firmware, report the network state (CMD03) and ask for the status (CMD08, answered by CMD07).
        this->lp_v3_mode_acked_ = true;
        if (this->lp_network_status_ != 0xFF)
          this->send_low_power_direct_(0x03, {this->lp_network_status_});
        ESP_LOGD(LP_TAG, "Querying MCU status with CMD08");
        this->lp_v3_last_poll_ms_ = App.get_loop_component_start_time();
        this->send_low_power_direct_(0x08, {});
      }
      return;

    case 0x03:  // ACK of the network state (version-3 MCU).
      return;

    case LP_V3_SLEEP_CMD:  // Sleep handshake of a version-3 MCU with sm=2.
      this->handle_low_power_v3_sleep_(data, len);
      return;

    case 0x25:  // The original T3-U firmware acknowledges this MCU notice with an empty CMD25.
      if (this->lp_v3_ && len == 0)
        this->send_low_power_direct_(0x25, {});
      return;

    case 0x07: {  // Status report of a version-3 MCU (standard Tuya framing).
      if (!this->lp_v3_)
        return;
      if (!valid_datapoints(data, len)) {
        ESP_LOGW(LP_TAG, "Rejecting malformed MCU CMD07 report");
        return;
      }
      this->lp_session_.observe_report(App.get_loop_component_start_time());
      if (!this->lp_v3_pushes_ && this->lp_session_.synchronized() &&
          App.get_loop_component_start_time() - this->lp_v3_last_poll_ms_ > 3000) {
        this->lp_v3_pushes_ = true;
        ESP_LOGI(LP_TAG, "MCU reports changes on its own; status polling stopped");
      }
      for (size_t offset = 0; offset < len;) {
        const size_t size = encode_uint16(data[offset + 2], data[offset + 3]);
        this->lp_session_.observe_dp(data[offset], data[offset + 1], data + offset + 4, size);
        offset += 4 + size;
      }
      this->handle_datapoints_(data, len);
      // A version-3 MCU sends no boot heartbeat; a complete report proves the link works again.
      this->lp_standby_.recovered();
      if (!this->lp_session_.synchronized()) {
        this->lp_session_.mark_synchronized();
        this->init_state_ = TuyaInitState::INIT_DONE;
        if (!this->lp_initialized_callback_called_) {
          this->lp_initialized_callback_called_ = true;
          this->mark_initialized_();
        }
        ESP_LOGI(LP_TAG, "MCU status synchronized (version-3 MCU)");
      }
      this->notify_mcu_ready_();
      this->update_low_power_sleep_lock_();
      return;
    }

    case 0x05: {  // MCU datapoint reports; acknowledge each DP separately.
      if (len == 2 && data[0] == 0)
        return;  // ACK, not a report: don't ACK an ACK.
      // First validate the complete list; never partially ACK malformed frames.
      bool valid = valid_datapoints(data, len);
      for (size_t offset = 0; valid && offset < len;) {
        const size_t size = encode_uint16(data[offset + 2], data[offset + 3]);
        valid = TuyaLowPowerSleepWindow::valid_datapoint(data[offset], data[offset + 1], data + offset + 4, size);
        offset += 4 + size;
      }
      if (!valid) {
        ESP_LOGW(LP_TAG, "Rejecting malformed MCU DP report; no partial CMD05 acknowledgements");
        return;
      }
      this->lp_session_.observe_report(App.get_loop_component_start_time());
      size_t offset = 0;
      while (offset < len) {
        const size_t size = encode_uint16(data[offset + 2], data[offset + 3]);
        this->send_low_power_direct_(0x05, {0x00, data[offset]});
        const bool previous_product_epoch = this->lp_sleep_window_.product_epoch_valid();
        this->lp_sleep_window_.observe_report(data[offset], data[offset + 1], data + offset + 4, size);
        if (previous_product_epoch && !this->lp_sleep_window_.product_epoch_valid())
          this->invalidate_low_power_mode_epoch_();
        this->lp_session_.observe_dp(data[offset], data[offset + 1], data + offset + 4, size);
        offset += 4 + size;
      }
      this->handle_datapoints_(data, len);
      if (this->init_state_ != TuyaInitState::INIT_DONE && this->lp_session_.synchronized()) {
        this->init_state_ = TuyaInitState::INIT_DONE;
        if (!this->lp_initialized_callback_called_) {
          this->lp_initialized_callback_called_ = true;
          this->mark_initialized_();
        }
        ESP_LOGI(LP_TAG, "MCU status synchronized for current product epoch");
      }
      // The HC32 takes commands once its full status is in.
      if (this->lp_session_.synchronized())
        this->notify_mcu_ready_();
      this->update_low_power_sleep_lock_();
      return;
    }

    case 0x06:  // Local time query.
      if (len == 0)
        this->send_low_power_time_(0x06);
      return;

    case 0x1C:  // Local time query of a version-3 MCU (standard Tuya); its weekly program needs the clock.
      if (this->lp_v3_ && len == 0)
        this->send_low_power_time_(0x1C);
      return;

    case 0x09:  // ACK alone never means that a new setpoint was adopted.
      if (len == 0) {
        this->lp_session_.write_ack();
        this->update_low_power_sleep_lock_();
      }
      return;

    case 0x38: {  // MCU asks whether WXU can enter low-power.
      if (len != 1 || data[0] != 0)
        return;
      // Never confuse a repeated sleep request with a completion ACK.
      if (this->lp_cmd38_pending_) {
        // The previous request is still pending. Send exactly one busy
        // response; never replay a second CMD38 ACK for the same transaction.
        this->cancel_low_power_preflight_(true);
        this->lp_cmd38_cooldown_until_ = App.get_loop_component_start_time() + 500;
        return;
      }
      // A version-0 MCU ignores the answer, sleeps anyway and asks again only after its next activity:
      // a request that cannot be granted now is granted later.
      this->lp_cmd38_deferred_ = !this->lp_v3_;
      if (!this->lp_v3_) {
        // It also sleeps right after asking, so the next command needs a wake pulse, and a network state
        // that crossed the request is lost: send it again instead of resynchronizing everything.
        this->lp_mcu_asleep_ = true;
        if (this->lp_wire_.abandon_network()) {
          ESP_LOGD(LP_TAG, "MCU went to sleep before answering CMD02; sending it again");
          this->expected_response_.reset();
          this->lp_network_status_ = 0xFF;
        }
      }
      if (this->lp_standby_.requested() ||
          static_cast<int32_t>(App.get_loop_component_start_time() - this->lp_cmd38_cooldown_until_) < 0) {
        this->cancel_low_power_standby_();
        this->send_low_power_direct_(0x38, {0x00, 0x01});
        return;
      }
      const uint8_t flags = this->grant_low_power_sleep_();
      if (flags != 0)
        this->send_low_power_direct_(0x38, {0x00, flags});
      return;
    }

    default: {
      // Shown at DEBUG while other MCU firmware versions are being mapped.
      char hex[3 * 24 + 1];
      const size_t shown = len < 24 ? len : 24;
      for (size_t i = 0; i < shown; ++i)
        snprintf(hex + 3 * i, 4, "%02X ", data[i]);
      hex[3 * shown] = '\0';
      ESP_LOGD(LP_TAG, "Unhandled MCU CMD 0x%02X v%u len %u: %s", command, version, static_cast<unsigned>(len), hex);
      return;
    }
  }
}

}  // namespace esphome::tuya

#endif  // USE_TUYA_LOW_POWER
