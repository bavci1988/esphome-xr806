#include "tuya.h"

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
static constexpr int LP_MAX_RETRIES = 5;

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
         !this->lp_product_retry_.active() &&
         guard_elapsed && baud_locked && this->parent_ != nullptr && this->parent_->is_connected() &&
         !this->lp_standby_.recovery_required() && this->command_queue_.empty() &&
         !this->expected_response_.has_value() && (in_cmd38_handler || this->rx_message_.empty());
}

void Tuya::update_low_power_sleep_lock_() {
#ifdef USE_XR806_LOWPOWER
  esphome::xr806::lowpower_policy().set_mcu_synchronized(this->low_power_ready_for_sleep_());
#endif
}

bool Tuya::request_low_power_refresh() {
  if (this->protocol_ != TuyaProtocol::LOW_POWER_V0 || !this->lp_v0_.has_product() || this->lp_v0_.busy() ||
      this->expected_response_.has_value())
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
  if (this->lp_wire_.expire(now)) {
    const bool product_query = *this->expected_response_ == TuyaCommandType::PRODUCT_QUERY;
    this->expected_response_.reset();
    if (!this->command_queue_.empty())
      this->command_queue_.erase(this->command_queue_.begin());
    this->lp_v0_.command_timeout();
    this->lp_network_status_ = 0xFF;
    this->invalidate_low_power_mode_epoch_();
    ESP_LOGW(LP_TAG, "MCU command ACK timeout (product=%s); quarantining writes until full resync",
             YESNO(product_query));
    // A late ACK cannot be distinguished from one for a subsequent write.
    // Do not silently replay queued commands after an ambiguous timeout.
    const size_t dropped = this->command_queue_.size();
    this->command_queue_.clear();
    if (dropped > 0) {
      ESP_LOGW(LP_TAG, "Discarded %u queued MCU commands after ambiguous ACK timeout",
               static_cast<unsigned>(dropped));
    }
  }

  if (this->lp_v0_.busy() || this->lp_wire_.active() || this->lp_wire_.faulted() ||
      this->expected_response_.has_value() || !this->rx_message_.empty() ||
      this->command_queue_.empty() || static_cast<uint32_t>(now - this->last_command_timestamp_) <= LP_COMMAND_DELAY_MS)
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
      // Repeated CMD38 during a previously accepted handoff may mean the
      // HC32 did not see our ACK. Cancel, respond busy, and resynchronize.
      if (this->lp_standby_.requested()) {
        this->cancel_low_power_standby_();
        this->send_low_power_direct_(0x38, {0x00, 0x01});
        return;
      }
      uint8_t flags = 0x01;  // Default: network/bus not safely suspended.
#ifdef USE_XR806_STANDBY_CONTROLLER
      auto *controller = esphome::xr806::standby_controller();
      const auto blockers = esphome::xr806::lowpower_policy().blockers();
      const bool ota_veto = (blockers & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY) != 0;
      const bool owner_veto =
          (blockers &
           (esphome::xr806::XR806LowPowerPolicy::NETWORK_BUSY | esphome::xr806::XR806LowPowerPolicy::API_BUSY |
            esphome::xr806::XR806LowPowerPolicy::MDNS_BUSY | esphome::xr806::XR806LowPowerPolicy::OTA_BUSY)) != 0;
      const bool wake_matches_mcu = controller != nullptr && controller->has_wake_pin() &&
                                    controller->requires_external_sync() &&
                                    controller->wake_pin() == this->lp_product_.wake_gpio;
      const bool accepted =
          wake_matches_mcu && this->lp_bridge_registered_ &&
          this->lp_standby_.accept_request(this->lp_product_.sleep_mode == 1, this->low_power_ready_for_sleep_(true),
                                           this->parent_ != nullptr && this->parent_->is_connected(), !owner_veto);
      if (accepted) {
        flags = 0x00;
      } else if (ota_veto) {
        flags |= 0x02;
      }
#elif defined(USE_XR806_LOWPOWER)
      if (esphome::xr806::lowpower_policy().blockers() & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY)
        flags |= 0x02;
#endif
      this->send_low_power_direct_(0x38, {0x00, flags});
#ifdef USE_XR806_STANDBY_CONTROLLER
      if (accepted) {
        // CMD38/00 is only a provisional consent. Request the PM controller
        // once the ACK is fully on the wire; it will still verify ALL votes.
        if (this->flush() == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
          controller->request_standby(true);
        } else {
          this->lp_standby_.transport_fault();
          this->update_low_power_sleep_lock_();
        }
      }
#endif
      return;
    }

    default:
      ESP_LOGV(LP_TAG, "Unhandled low-power MCU CMD 0x%02X", command);
      return;
  }
}

}  // namespace esphome::tuya
