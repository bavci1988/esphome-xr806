#include "tuya.h"

#ifdef USE_TUYA_LOW_POWER_V0

#include <cctype>
#include <cstring>
#include <utility>

#include "esphome/core/log.h"
#include "esphome/core/util.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif
#ifdef USE_XR806_LOWPOWER
#include "esphome/components/xr806/lowpower_policy.h"
#endif

namespace esphome::tuya {

static const char *const LP_TAG = "tuya.low_power_v0";
static constexpr uint32_t LP_RX_TIMEOUT_MS = 300;
static constexpr uint32_t LP_COMMAND_DELAY_MS = 10;

// Interval id shared with the standard heartbeat slot in tuya.cpp; a hub runs one protocol only.
static constexpr uint32_t LP_NETWORK_STATUS_INTERVAL_ID = 1;

void Tuya::setup_low_power_() {
  this->init_state_ = TuyaInitState::INIT_PRODUCT;
  if (this->mcu_wake_pin_ != nullptr) {
    this->mcu_wake_pin_->setup();
    this->mcu_wake_pin_->digital_write(false);  // Inactive logical level.
  }
#ifdef USE_XR806
  if (this->trv16_pb04_requested_ && !this->trv16_pb04_.claim(this->trv16_pb04_active_high_)) {
    ESP_LOGE(LP_TAG, "PB04 is owned by another peripheral/flash mux; MCU wake pin refused");
    this->mark_failed();
    return;
  }
#endif
#ifdef USE_XR806_LOWPOWER
  esphome::xr806::lowpower_policy().require_external_mcu(true);
  this->update_low_power_sleep_lock_();
#endif
#ifdef USE_XR806_STANDBY_CONTROLLER
  // Registration is a protocol-specific opt-in. The generic platform PM
  // driver never sends Tuya packets or infers the MCU's 'sm' selection.
  if (auto *controller = esphome::xr806::standby_controller(); controller != nullptr) {
    this->lp_bridge_registered_ = controller->register_participant(this);
    if (!this->lp_bridge_registered_) {
      ESP_LOGE(LP_TAG, "Cannot register Tuya MCU standby participant; PM remains blocked");
      this->mark_failed();
    }
  }
#endif  // USE_XR806_STANDBY_CONTROLLER
  // The original WXU probes CMD01 at each candidate baud, without a heartbeat.
  // With a fixed rate the existing queued CMD01 request is sufficient.
  if (this->auto_baud_) {
#ifdef USE_XR806
    // A wrong candidate rate produces expected LSR framing errors. The
    // physical UART driver must not latch a permanent fault during probes.
    this->parent_->set_baud_probe_mode(true);
#endif
    this->lp_autobaud_.start(this->parent_->get_baud_rate());
  } else {
    this->send_low_power_product_query_(false);
  }
  this->set_interval(LP_NETWORK_STATUS_INTERVAL_ID, 1000, [this] { this->send_low_power_network_(); });
}

void Tuya::loop_low_power_() {
  this->poll_low_power_sleep_preflight_();
#ifdef USE_XR806_STANDBY_CONTROLLER
  if (this->lp_standby_.requested()) {
    const auto &policy = esphome::xr806::lowpower_policy();
    const bool ota_busy = (policy.blockers() & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY) != 0;
    auto *controller = esphome::xr806::standby_controller();
    if (!policy.standby_requested() || ota_busy || controller == nullptr || !controller->may_quiesce() ||
        !this->low_power_ready_for_sleep_()) {
      // Withdraw the HC32 logical-sleep signal on any new protocol,
      // network, OTA or application work, before PM has been entered.
      this->cancel_low_power_standby_();
    }
  }
#endif
  if (this->parent_ == nullptr || !this->parent_->is_connected()) {
    if (!this->lp_standby_.recovery_required()) {
      this->lp_standby_.transport_fault();
      this->lp_v0_.command_timeout();
      this->lp_wire_.transport_fault();
      this->lp_network_status_ = 0xFF;
      ESP_LOGE(LP_TAG, "XR806 UART unhealthy: MCU transactions cancelled, standby blocked until recovery");
    }
#ifdef USE_XR806_STANDBY_CONTROLLER
    if (auto *controller = esphome::xr806::standby_controller(); controller != nullptr)
      controller->request_standby(false);
#endif
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
  if (this->lp_v0_.expire(millis())) {
    ESP_LOGW(LP_TAG, "Low-power status/write report timeout; requesting state resynchronization");
    this->lp_network_status_ = 0xFF;
  }
  this->update_low_power_sleep_lock_();
  this->process_low_power_queue_();
}

void Tuya::dump_low_power_config_() {
  ESP_LOGCONFIG(LP_TAG, "  Protocol: low_power_v0");
  ESP_LOGCONFIG(LP_TAG, "  Auto baud: %s", YESNO(this->auto_baud_));
  if (this->auto_baud_ && this->lp_autobaud_.locked()) {
    ESP_LOGCONFIG(LP_TAG, "  Detected baud: %u", static_cast<unsigned>(this->lp_autobaud_.baud()));
  }
  // The standard "Product" line stays empty: low_power_v0 keeps CMD01 in a fixed buffer.
  ESP_LOGCONFIG(LP_TAG, "  MCU product: '%s'", this->lp_product_text_.data());
}

void Tuya::on_low_power_rx_byte_() {
  // An incoming byte before CMD38 consent invalidates a pending network
  // preflight. The MCU must see a busy response, not a delayed grant.
  if (this->lp_cmd38_pending_) {
    this->cancel_low_power_preflight_(true);
    this->lp_cmd38_cooldown_until_ = millis() + 500;
  }
  // The first arriving byte, not only a fully checksummed frame, must
  // preempt a provisional CMD38 -> CMD12 -> CPU standby transaction.
  // Otherwise partial/noisy UART RX can race the final PM admission check.
  if (this->lp_standby_.requested())
    this->cancel_low_power_standby_();
}

void Tuya::preempt_low_power_sleep_() {
  if (this->lp_cmd38_pending_)
    this->cancel_low_power_preflight_(true);
  if (this->lp_standby_.requested())
    this->cancel_low_power_standby_();
}

void Tuya::handle_low_power_frame_(uint8_t command, uint8_t version, const uint8_t *buffer, size_t len) {
  if (this->auto_baud_ && this->lp_autobaud_.searching()) {
    // validate_message_ already checked the full checksum. Keep the scanner
    // independent so bad/wrong frames cannot unlock the boot state machine.
    if (!this->lp_autobaud_.accept_frame(this->rx_message_.data(), this->rx_message_.size(), millis()))
      return;
#ifdef USE_XR806
    // The accepted CMD01 packet was checksum-validated and matches this rate.
    // From now on, any hardware framing/overrun error is a real sticky fault.
    this->parent_->set_baud_probe_mode(false);
#endif
    ESP_LOGI(LP_TAG, "Tuya low_power_v0 baud locked: %u", static_cast<unsigned>(this->lp_autobaud_.baud()));
  }

  TuyaCommandType command_type = (TuyaCommandType) command;

  // CMD02/CMD09 are only acknowledged by an *empty* reply. CMD05 reports
  // never consume a queued command, and any unsolicited matching command ID
  // must not release an unrelated transaction.
  // A matching reply delivered after its deadline but before the periodic
  // timeout sweep must not complete an expired transaction.
  if (this->lp_wire_.overdue(millis()) && this->lp_wire_.accepts(command, len)) {
    ESP_LOGW(LP_TAG, "Discarding overdue MCU ACK CMD 0x%02X; recovery required", command);
    return;
  }
  if (this->lp_wire_.complete(command, len) && this->expected_response_.has_value() &&
      this->expected_response_ == command_type) {
    this->expected_response_.reset();
    if (!this->command_queue_.empty())
      this->command_queue_.erase(this->command_queue_.begin());
    this->init_retries_ = 0;
  }
  this->handle_low_power_command_(command, version, buffer, len);
}

void Tuya::send_low_power_raw_command_(const TuyaCommand &command) {
  this->preempt_low_power_sleep_();
  this->last_command_timestamp_ = millis();
  if (!this->begin_low_power_command_(command))
    return;
  ESP_LOGV(LP_TAG, "Sending MCU CMD=0x%02X len=%u", static_cast<uint8_t>(command.cmd),
           static_cast<unsigned>(command.payload.size()));
  // Same frame as the standard path: 55 AA, version 0, command, length, payload, checksum.
  this->send_low_power_direct_(static_cast<uint8_t>(command.cmd), command.payload);
}

bool Tuya::begin_low_power_command_(const TuyaCommand &command) {
  const auto cmd = static_cast<uint8_t>(command.cmd);
  if (cmd == 0x01 || cmd == 0x02 || cmd == 0x09) {
    if (!this->lp_wire_.begin(cmd, millis())) {
      ESP_LOGE(LP_TAG, "Refusing overlapping/ambiguous low-power MCU wire transaction");
      this->lp_v0_.command_timeout();
      this->lp_wire_.transport_fault();
      return false;
    }
    this->expected_response_ = command.cmd;
  }
  if (cmd == 0x02 && command.payload.size() == 1 && command.payload[0] == 0x04)
    this->lp_v0_.begin_full_status(millis());
  if (cmd == 0x09 && command.payload.size() >= 4) {
    const auto &p = command.payload;
    const size_t size = (static_cast<size_t>(p[2]) << 8) | p[3];
    if (p.size() == size + 4)
      this->lp_v0_.begin_write(p[0], p[1], p.data() + 4, size, millis());
  }
  this->update_low_power_sleep_lock_();
  return true;
}

void Tuya::send_low_power_datapoint_(uint8_t datapoint_id, TuyaDatapointType datapoint_type,
                                     const std::vector<uint8_t> &data) {
  if (!this->lp_v0_.ready() || !this->lp_wire_.idle() || this->lp_standby_.requested()) {
    ESP_LOGW(LP_TAG, "Refusing DP%u write before MCU synchronization or while another transaction owns UART",
             static_cast<unsigned>(datapoint_id));
    return;
  }
  // The original MCU DP102 handler blindly reads four bytes even for
  // RAW1 and can consume checksum/buffer memory. Reject unsafe writes
  // BEFORE serialization, regardless of whether a higher-level entity
  // calls a forced setter or sends an unknown datapoint.
  if (!TuyaLowPowerSleepWindow::valid_datapoint(datapoint_id, static_cast<uint8_t>(datapoint_type), data.data(),
                                                data.size())) {
    ESP_LOGE(LP_TAG, "Refusing malformed TRV16 DP%u (type=%u, len=%u)", datapoint_id,
             static_cast<unsigned>(datapoint_type), static_cast<unsigned>(data.size()));
    return;
  }
  if (datapoint_id == TuyaLowPowerSleepWindow::ENABLE_DP || datapoint_id == TuyaLowPowerSleepWindow::WINDOW_DP) {
    this->lp_sleep_window_.on_write(datapoint_id);
    this->invalidate_low_power_mode_epoch_();
  }
  std::vector<uint8_t> buffer;
  buffer.reserve(data.size() + 4);
  buffer.push_back(datapoint_id);
  buffer.push_back(static_cast<uint8_t>(datapoint_type));
  buffer.push_back(data.size() >> 8);
  buffer.push_back(data.size() >> 0);
  buffer.insert(buffer.end(), data.begin(), data.end());
  // The HC32 low-power firmware expects DP writes as CMD09, not CMD06.
  this->send_command_(TuyaCommand{.cmd = static_cast<TuyaCommandType>(0x09), .payload = buffer});
}

void Tuya::send_low_power_product_query_(bool direct_probe) {
  // The original WXU calls its MCU-wake helper *before* CMD01. We pulse
  // only when a wake GPIO is explicitly configured and its polarity verified.
  // The GPIO configuration supports inverted pins for active-low wiring.
  bool have_wake_output = this->mcu_wake_pin_ != nullptr;
#ifdef USE_XR806
  have_wake_output = have_wake_output || this->trv16_pb04_.claimed();
#endif
  if (have_wake_output) {
    if (this->mcu_wake_pin_ != nullptr) {
      this->mcu_wake_pin_->digital_write(true);
    }
#ifdef USE_XR806
    if (this->trv16_pb04_.claimed()) {
      this->trv16_pb04_.drive(true);
    }
#endif
    this->set_timeout("tuya_mcu_preprobe_wake", 10, [this, direct_probe]() {
      if (this->mcu_wake_pin_ != nullptr) {
        this->mcu_wake_pin_->digital_write(false);
      }
#ifdef USE_XR806
      this->trv16_pb04_.drive(false);
#endif
      if (direct_probe) {
        this->send_low_power_direct_(0x01, {});
      } else {
        this->send_empty_command_(TuyaCommandType::PRODUCT_QUERY);
      }
    });
    return;
  }
  if (direct_probe) {
    this->send_low_power_direct_(0x01, {});
  } else {
    this->send_empty_command_(TuyaCommandType::PRODUCT_QUERY);
  }
}

void Tuya::poll_low_power_autobaud_() {
  const uint32_t now = millis();
  if (!this->lp_autobaud_.probe_due(now)) {
    return;
  }

#ifdef USE_XR806
  const uint32_t baud = this->lp_autobaud_.baud();
  // This is a synchronous, bounded HAL reconfiguration, not a busy-wait
  // through the 500 ms window. The UART backend drains TX and clears stale RX.
  if (this->parent_ == nullptr || !this->parent_->reconfigure_baud_rate(baud)) {
    ESP_LOGE(LP_TAG, "Unable to configure MCU UART for %u baud", static_cast<unsigned>(baud));
    this->lp_autobaud_.stop();
    this->init_failed_ = true;
    this->mark_failed();
    return;
  }
  this->rx_message_.clear();  // Drop a partial frame from the previous rate.
  this->lp_autobaud_.probe_sent(millis());
  this->send_low_power_product_query_(true);
  if (this->lp_autobaud_.attempts() % 8 == 0) {
    ESP_LOGW(LP_TAG, "Still scanning MCU UART (9600/115200), %u probes",
             static_cast<unsigned>(this->lp_autobaud_.attempts()));
  } else {
    ESP_LOGD(LP_TAG, "Autobaud CMD01 probe at %u", static_cast<unsigned>(baud));
  }
#else
  // Schema validation rejects auto_baud on non-XR806 targets.
  this->lp_autobaud_.stop();
  this->init_failed_ = true;
  this->mark_failed();
#endif
}

void Tuya::invalidate_low_power_mode_epoch_() {
  // The HC32's 'sm' is computed by its RTC task, not by the WXU. A write or
  // changed report for DP101/102 makes the last product-info selection stale.
  this->lp_sleep_window_.invalidate_product();
  if (this->lp_v0_.has_product()) {
    this->lp_mode_requery_pending_ = true;
    this->lp_mode_requery_after_ms_ = millis() + 500;
  }
  this->update_low_power_sleep_lock_();
}

void Tuya::poll_low_power_product_recovery_() {
  if (this->init_failed_) {
    return;
  }
  const uint32_t now = millis();
  // Never inject an autonomous CMD01 during a report burst or a write ACK.
  // After an ambiguous ACK timeout, queued writes must not block the
  // direct CMD01 recovery probe used to re-establish the MCU identity.
  const bool idle = !this->lp_v0_.busy() && !this->lp_wire_.active() &&
                    (this->lp_wire_.faulted() || this->command_queue_.empty()) &&
                    !this->expected_response_.has_value() && this->rx_message_.empty();
  if (this->lp_product_retry_.active()) {
    if (!this->lp_product_retry_.due(now) || !idle) {
      return;
    }
    if (this->lp_product_retry_.exhausted()) {
      this->init_failed_ = true;
      this->lp_mode_requery_pending_ = false;
      this->lp_v0_.command_timeout();
      ESP_LOGE(LP_TAG, "Malformed MCU product info: bounded 500ms CMD01 recovery exhausted");
      this->mark_failed();
      this->lp_product_retry_.success();
      return;
    }
    this->lp_product_retry_.query_sent(now);
    this->send_low_power_product_query_(true);
    return;
  }

  if (this->lp_mode_requery_pending_ && idle && static_cast<int32_t>(now - this->lp_mode_requery_after_ms_) >= 0) {
    this->lp_mode_requery_pending_ = false;
    // Unlike original cached sm, our MCU component can revalidate a new
    // product epoch without resetting the generic XR806 platform.
    this->send_low_power_product_query_(false);
  }
}

bool Tuya::low_power_ready_for_sleep_(bool in_cmd38_handler) const {
  // A validated CMD38 frame is still present in rx_message_ until the
  // parser callback returns. Only that specific frame can be disregarded.
  const bool guard_elapsed = static_cast<int32_t>(millis() - this->lp_wake_guard_until_) >= 0;
  const bool baud_locked = !this->auto_baud_ || this->lp_autobaud_.locked();
  return this->lp_v0_.ready() && this->lp_wire_.idle() && this->lp_sleep_window_.standby_mode_stable() &&
         !this->lp_product_retry_.active() && guard_elapsed && baud_locked && this->parent_ != nullptr &&
         this->parent_->is_connected() && !this->lp_standby_.recovery_required() && this->command_queue_.empty() &&
         !this->expected_response_.has_value() && (in_cmd38_handler || this->rx_message_.empty());
}

void Tuya::update_low_power_sleep_lock_() {
#ifdef USE_XR806_LOWPOWER
  esphome::xr806::lowpower_policy().set_mcu_synchronized(this->low_power_ready_for_sleep_());
#endif
}

bool Tuya::request_low_power_refresh() {
  if (this->protocol_ != TuyaProtocol::LOW_POWER_V0 || !this->lp_v0_.ready() || !this->lp_wire_.idle() ||
      this->lp_standby_.requested() || this->expected_response_.has_value())
    return false;
  this->send_datapoint_command_(106, TuyaDatapointType::BOOLEAN, {0x01});
  return true;
}

void Tuya::send_low_power_awake_notification_() {
  // This is a Tuya/HC32 command, not an XR806 hardware-wake operation.
  if (this->parent_ == nullptr || !this->parent_->is_connected()) {
    this->lp_standby_.transport_fault();
  } else {
    this->send_low_power_direct_(0x12, {0x00});
    if (this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS)
      this->lp_standby_.transport_fault();
  }
  this->lp_wake_guard_until_ = millis() + 5000;
  this->update_low_power_sleep_lock_();
}

void Tuya::cancel_low_power_preflight_(bool notify_mcu) {
  if (!this->lp_cmd38_pending_)
    return;
  this->lp_cmd38_pending_ = false;
#ifdef USE_XR806_STANDBY_CONTROLLER
  if (auto *controller = esphome::xr806::standby_controller(); controller != nullptr)
    controller->request_preflight(false);
#endif
  if (notify_mcu)
    this->send_low_power_direct_(0x38, {0x00, 0x01});
}

void Tuya::poll_low_power_sleep_preflight_() {
  if (!this->lp_cmd38_pending_)
    return;
#ifdef USE_XR806_STANDBY_CONTROLLER
  auto *controller = esphome::xr806::standby_controller();
  const auto &policy = esphome::xr806::lowpower_policy();
  const bool ota_busy = (policy.blockers() & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY) != 0;
  if (controller == nullptr || !controller->preflight_requested() ||
      static_cast<int32_t>(millis() - this->lp_cmd38_deadline_ms_) >= 0 || ota_busy || !controller->may_quiesce() ||
      !this->low_power_ready_for_sleep_()) {
    this->cancel_low_power_preflight_(true);
    this->lp_cmd38_cooldown_until_ = millis() + 500;
    return;
  }
  if (!controller->services_quiesced())
    return;
  if (this->available() != 0 || this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS ||
      !this->lp_standby_.accept_request(this->lp_product_.sleep_mode == 1, true, true, true)) {
    this->cancel_low_power_preflight_(true);
    return;
  }
  // Never grant CMD38 while network owners still have live sockets, OTA,
  // mDNS callbacks or a WLAN wakelock. The MCU receives its ACK only NOW.
  this->lp_cmd38_pending_ = false;
  this->send_low_power_direct_(0x38, {0x00, 0x00});
  if (this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
    controller->request_standby(true);
  } else {
    this->lp_standby_.transport_fault();
    this->update_low_power_sleep_lock_();
  }
  controller->request_preflight(false);
#else
  this->cancel_low_power_preflight_(true);
#endif
}

void Tuya::cancel_low_power_standby_() {
  const bool compensate = this->lp_standby_.force_cancel();
#ifdef USE_XR806_STANDBY_CONTROLLER
  if (auto *controller = esphome::xr806::standby_controller(); controller != nullptr)
    controller->request_standby(false);
#endif
  if (compensate)
    this->send_low_power_awake_notification_();
  this->update_low_power_sleep_lock_();
}

bool Tuya::notify_low_power_transition(bool sleeping) {
  if (this->protocol_ != TuyaProtocol::LOW_POWER_V0 || !this->lp_v0_.has_product())
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

#ifdef USE_XR806_STANDBY_CONTROLLER
bool Tuya::prepare_for_standby() {
  if (!this->lp_bridge_registered_ || !this->lp_standby_.requested() || this->lp_product_.sleep_mode != 1 ||
      !this->low_power_ready_for_sleep_() || this->available() != 0)
    return false;
  if (this->lp_standby_.needs_notification()) {
    // Deliberately postpone platform PM; CMD12/01 was transmitted first.
    // standby_cancelled() consumes the intentional first deferral.
    (void) this->notify_low_power_transition(true);
    return false;
  }
  return this->lp_standby_.ready_for_pm() && this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
}

void Tuya::standby_cancelled() {
  const bool compensate = this->lp_standby_.cancelled();
  if (!this->lp_standby_.requested()) {
    if (auto *controller = esphome::xr806::standby_controller(); controller != nullptr)
      controller->request_standby(false);
  }
  if (compensate)
    this->send_low_power_awake_notification_();
  this->update_low_power_sleep_lock_();
}

void Tuya::standby_resumed(const xr806::XR806StandbyResume &event) {
  const auto action = this->lp_standby_.resumed(event.expected_source, event.wake_gpio);
  this->lp_wake_guard_until_ = millis() + 5000;
  // A retained GPIO wake may receive CMD12/00 only if we previously sent
  // CMD12/01. Timer-only wake must not impersonate the original GPIO path.
  if (action == TuyaLowPowerStandbyState::ResumeAction::SEND_CMD12_WAKE) {
    this->send_low_power_awake_notification_();
  } else if (action == TuyaLowPowerStandbyState::ResumeAction::REQUIRE_RESYNC) {
    this->lp_v0_.command_timeout();
    ESP_LOGE(LP_TAG, "Unexpected/timer-only MCU resume: withholding CMD12/00 and further standby");
  }
  this->update_low_power_sleep_lock_();
}
#endif

void Tuya::process_low_power_queue_() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - this->last_rx_char_timestamp_) > LP_RX_TIMEOUT_MS)
    this->rx_message_.clear();

  // Standard Tuya's 300 ms retry logic is incompatible with the HC32's
  // delayed CMD05 report. ACK and report have separate timeouts.
  // The session gate, not an optional ESPHome command field, owns the
  // physical in-flight transaction. It can time out after a reset or an
  // asynchronous recovery callback has already cleared expected_response_.
  const bool product_in_flight = this->lp_wire_.kind() == TuyaLowPowerWireTransactions::Kind::PRODUCT;
  if (this->lp_wire_.expire(now)) {
    const bool product_query = product_in_flight;
    this->expected_response_.reset();
    if (!this->command_queue_.empty())
      this->command_queue_.erase(this->command_queue_.begin());
    this->lp_v0_.command_timeout();
    this->lp_network_status_ = 0xFF;
    this->invalidate_low_power_mode_epoch_();
    if (product_query) {
      // A first CMD01 can time out before any product epoch exists. The
      // existing invalidate helper only schedules re-query once a product
      // was previously received; explicitly start bounded bootstrap retry.
      this->lp_product_retry_.begin(now);
    }
    ESP_LOGW(LP_TAG, "MCU command ACK timeout (product=%s); quarantining writes until full resync",
             YESNO(product_query));
    // A late ACK cannot be distinguished from one for a subsequent write.
    // Do not silently replay queued commands after an ambiguous timeout.
    const size_t dropped = this->command_queue_.size();
    this->command_queue_.clear();
    if (dropped > 0) {
      ESP_LOGW(LP_TAG, "Discarded %u queued MCU commands after ambiguous ACK timeout", static_cast<unsigned>(dropped));
    }
  }

  if (this->lp_v0_.busy() || this->lp_wire_.active() || this->lp_wire_.faulted() ||
      this->expected_response_.has_value() || !this->rx_message_.empty() || this->command_queue_.empty() ||
      static_cast<uint32_t>(now - this->last_command_timestamp_) <= LP_COMMAND_DELAY_MS)
    return;
  if (!this->lp_v0_.has_product() && this->command_queue_.front().cmd != TuyaCommandType::PRODUCT_QUERY)
    return;

  this->send_raw_command_(this->command_queue_.front());
  if (!this->expected_response_.has_value())
    this->command_queue_.erase(this->command_queue_.begin());
}

void Tuya::send_low_power_direct_(uint8_t command, const std::vector<uint8_t> &payload) {
  // ACKs and time replies must not wait behind a pending CMD09 write.
  if (payload.size() > 65535)
    return;
  const uint8_t header[] = {
      0x55, 0xAA, 0x00, command, static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size()),
  };
  uint8_t checksum = 0;
  for (uint8_t b : header)
    checksum += b;
  for (uint8_t b : payload)
    checksum += b;
  this->write_array(header, sizeof(header));
  if (!payload.empty())
    this->write_array(payload.data(), payload.size());
  this->write_byte(checksum);
}

void Tuya::send_low_power_network_() {
  if (!this->lp_v0_.has_product())
    return;
#ifdef USE_XR806_STANDBY_CONTROLLER
  // An intentional, pending XR806 WLAN teardown is not a new logical
  // Tuya network epoch: defer CMD02 until abort/resume, when the station
  // is restored. External/unplanned link loss remains visible as before.
  const auto *controller = esphome::xr806::standby_controller();
  if (esphome::xr806::lowpower_policy().network_quiescing() &&
      (esphome::xr806::lowpower_policy().standby_requested() ||
       (controller != nullptr && controller->preflight_requested())))
    return;
#endif
  uint8_t status = 0x02;
#ifdef USE_NETWORK
  if (network::is_connected())
    status = remote_is_connected() ? 0x04 : 0x03;
#endif
  if (status == this->lp_network_status_)
    return;
  this->lp_network_status_ = status;
  this->lp_v0_.reset_status();
  if (status == 0x04)
    this->lp_wake_guard_until_ = millis() + (this->lp_product_.sleep_mode == 0 ? 10000 : 5000);
  this->update_low_power_sleep_lock_();
  this->send_command_({.cmd = static_cast<TuyaCommandType>(0x02), .payload = {status}});
}

void Tuya::send_low_power_time_() {
  // CMD06 carries local calendar time. No duplicate timezone/DST adjustment.
  std::vector<uint8_t> payload(8, 0);
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
  this->send_low_power_direct_(0x06, payload);
}

void Tuya::handle_low_power_command_(uint8_t command, uint8_t version, const uint8_t *data, size_t len) {
  (void) version;
  // Once CMD38 is provisionally accepted, any new MCU traffic invalidates
  // the quiet interval. Do not race a report burst against CPU standby.
  if (command != 0x38 && this->lp_standby_.requested())
    this->cancel_low_power_standby_();
  switch (command) {
    case 0x00:  // First MCU heartbeat after reset is 00; subsequent is 01.
      if (len != 1 || data[0] != 0)
        return;
      this->lp_standby_.reset_on_mcu_boot();
      // The HC32 may have a new RTC sleep-window marker (sm), including
      // after a PA3 pulse or local UI change; force a fresh product epoch.
      if (this->lp_v0_.has_product()) {
        this->invalidate_low_power_mode_epoch_();
        this->lp_sleep_window_.reset_reports();
      }
#ifdef USE_XR806_STANDBY_CONTROLLER
      if (auto *controller = esphome::xr806::standby_controller(); controller != nullptr)
        controller->request_standby(false);
#endif
      if (!this->lp_v0_.has_product()) {
        this->send_empty_command_(TuyaCommandType::PRODUCT_QUERY);
      } else {
        this->lp_v0_.reset_status();
        this->lp_network_status_ = 0xFF;
        this->update_low_power_sleep_lock_();
        this->send_low_power_network_();
        this->send_empty_command_(static_cast<TuyaCommandType>(0x13));
      }
      return;

    case 0x01: {  // Product-info response.
      TuyaLowPowerProductInfo incoming{};
      if (!incoming.parse(data, len)) {
        // Original WXU calls CMD01 again after 500 ms for missing descriptors,
        // allocation/JSON failures or absent p/v. Retry only malformed data;
        // never treat its failed initialization marker as a working MCU.
        this->invalidate_low_power_mode_epoch_();
        this->lp_v0_.command_timeout();
        this->lp_product_retry_.begin(millis());
        ESP_LOGW(LP_TAG, "Malformed CMD01 product info: retrying at 500ms intervals (bounded)");
        return;
      }
      if (!incoming.is_verified_trv16()) {
        ESP_LOGE(LP_TAG, "MCU PID/wake wiring mismatch; refusing unsupported product");
        this->product_invalid_ = true;
        this->init_failed_ = true;
        this->lp_sleep_window_.invalidate_product();
#ifdef USE_XR806
        this->trv16_pb04_.release();
#endif
        this->mark_failed();
        return;
      }
      // parse() limits len to 512, so the raw descriptor fits our fixed
      // diagnostics buffer without an extra allocation and without an
      // implementation-dependent nothrow-new/abort path.
      std::memcpy(this->lp_product_text_.data(), data, len);
      this->lp_product_text_[len] = '\0';
      const bool had_product = this->lp_v0_.has_product();
      this->lp_product_ = std::move(incoming);
      this->lp_product_retry_.success();
      this->lp_mode_requery_pending_ = false;
      this->lp_sleep_window_.product_validated();
      if (had_product) {
        // HC32 RTC/DP101/DP102 may have changed the requested PM mode.
        // Subsequent CMD01 never proves MCU status is still synchronized.
        this->lp_v0_.reset_status();
        this->lp_sleep_window_.reset_reports();
        this->lp_network_status_ = 0xFF;
      }
      this->lp_v0_.product_received();
      // Revalidated product identity clears the wire quarantine, but not
      // the MCU full-status gate (17 new CMD05 reports are still required).
      if (this->lp_wire_.faulted())
        this->lp_wire_.recovered();
      this->init_state_ = TuyaInitState::INIT_DATAPOINT;
      ESP_LOGI(LP_TAG, "TRV16 PID %s version %s wk=%d mwk=%d sm=%d", this->lp_product_.pid.c_str(),
               this->lp_product_.version.c_str(), this->lp_product_.wake_gpio, this->lp_product_.mcu_wake_gpio,
               this->lp_product_.sleep_mode);
      this->send_low_power_network_();
      return;
    }

    case 0x02:  // Network-state ACK; generic queue handler clears expected CMD.
      return;

    case 0x05: {  // MCU datapoint reports; acknowledge each DP separately.
      if (len == 2 && data[0] == 0)
        return;  // ACK, not a report: don't ACK an ACK.
      if (len < 5)
        return;
      // First validate the complete list; never partially ACK malformed frames.
      size_t offset = 0;
      while (offset < len) {
        if (len - offset < 4)
          return;
        const size_t size = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
        const uint8_t type = data[offset + 1];
        if (size > len - offset - 4 || type > 5 || ((type == 1 || type == 4) && size != 1) ||
            (type == 2 && size != 4) || (type == 1 && data[offset + 4] > 1) ||
            !TuyaLowPowerSleepWindow::valid_datapoint(data[offset], type, data + offset + 4, size)) {
          ESP_LOGW(LP_TAG, "Rejecting malformed MCU DP report; no partial CMD05 acknowledgements");
          return;
        }
        offset += 4 + size;
      }
      offset = 0;
      while (offset < len) {
        const size_t size = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
        this->send_low_power_direct_(0x05, {0x00, data[offset]});
        const bool previous_product_epoch = this->lp_sleep_window_.product_epoch_valid();
        this->lp_sleep_window_.observe_report(data[offset], data[offset + 1], data + offset + 4, size);
        if (previous_product_epoch && !this->lp_sleep_window_.product_epoch_valid())
          this->invalidate_low_power_mode_epoch_();
        this->lp_v0_.observe_dp(data[offset], data[offset + 1], data + offset + 4, size);
        offset += 4 + size;
      }
      this->handle_datapoints_(data, len);
      if (this->init_state_ != TuyaInitState::INIT_DONE && this->lp_v0_.synchronized()) {
        this->init_state_ = TuyaInitState::INIT_DONE;
        if (!this->lp_initialized_callback_called_) {
          this->lp_initialized_callback_called_ = true;
          this->initialized_callback_.call();
        }
        ESP_LOGI(LP_TAG, "MCU status synchronized for current product epoch");
      }
      this->update_low_power_sleep_lock_();
      return;
    }

    case 0x06:  // Local time query.
      if (len == 0)
        this->send_low_power_time_();
      return;

    case 0x09:  // ACK alone never means that a new setpoint was adopted.
      if (len == 0) {
        this->lp_v0_.write_ack();
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
        this->lp_cmd38_cooldown_until_ = millis() + 500;
        return;
      }
      if (this->lp_standby_.requested() || static_cast<int32_t>(millis() - this->lp_cmd38_cooldown_until_) < 0) {
        this->cancel_low_power_standby_();
        this->send_low_power_direct_(0x38, {0x00, 0x01});
        return;
      }
      uint8_t flags = 0x01;  // Default is *busy*, not a provisional grant.
#ifdef USE_XR806_STANDBY_CONTROLLER
      auto *controller = esphome::xr806::standby_controller();
      const auto blockers = esphome::xr806::lowpower_policy().blockers();
      const bool ota_veto = (blockers & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY) != 0;
      const bool supported = controller != nullptr && this->lp_bridge_registered_ && controller->has_wake_pin() &&
                             controller->requires_external_sync() &&
                             controller->wake_pin() == this->lp_product_.wake_gpio &&
                             this->lp_product_.sleep_mode == 1 && controller->may_quiesce();
      const bool transport_ready = this->low_power_ready_for_sleep_(true) && this->available() == 0;
      if (supported && !ota_veto && transport_ready) {
        if (!controller->services_quiesced()) {
          // Start *service* preparation without requesting CPU PM. Do not
          // tell HC32 that WXU accepted sleep before every network owner
          // has actually closed/drained its resources.
          this->lp_cmd38_pending_ = true;
          this->lp_cmd38_deadline_ms_ = millis() + 1800;
          controller->request_preflight(true);
          return;  // A bounded later busy/accept response will be sent.
        }
        const bool accepted = this->lp_standby_.accept_request(true, true, true, true);
        if (accepted) {
          this->send_low_power_direct_(0x38, {0x00, 0x00});
          if (this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS)
            controller->request_standby(true);
          else
            this->lp_standby_.transport_fault();
          this->update_low_power_sleep_lock_();
          return;
        }
      }
      if (ota_veto)
        flags |= 0x02;
#elif defined(USE_XR806_LOWPOWER)
      if (esphome::xr806::lowpower_policy().blockers() & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY)
        flags |= 0x02;
#endif
      this->send_low_power_direct_(0x38, {0x00, flags});
      return;
    }

    default:
      ESP_LOGV(LP_TAG, "Unhandled low-power MCU CMD 0x%02X", command);
      return;
  }
}

}  // namespace esphome::tuya

#endif  // USE_TUYA_LOW_POWER_V0
