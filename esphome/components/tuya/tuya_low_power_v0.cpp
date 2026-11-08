#include "tuya.h"

#include <cctype>
#include <cstring>

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

void Tuya::poll_low_power_autobaud_() {
  const uint32_t now = millis();
  if (!this->lp_autobaud_.probe_due(now))
    return;

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
  this->send_low_power_direct_(0x01, {});
  if (this->lp_autobaud_.attempts() % 8 == 0)
    ESP_LOGW(LP_TAG, "Still scanning MCU UART (9600/115200), %u probes",
             static_cast<unsigned>(this->lp_autobaud_.attempts()));
  else
    ESP_LOGD(LP_TAG, "Autobaud CMD01 probe at %u", static_cast<unsigned>(baud));
#else
  // Schema validation rejects auto_baud on non-XR806 targets.
  this->lp_autobaud_.stop();
  this->init_failed_ = true;
  this->mark_failed();
#endif
}

bool Tuya::low_power_ready_for_sleep_() const {
  const bool guard_elapsed = static_cast<int32_t>(millis() - this->lp_wake_guard_until_) >= 0;
  return this->lp_v0_.ready() && guard_elapsed && this->command_queue_.empty() &&
         !this->expected_response_.has_value() && this->rx_message_.empty();
}

void Tuya::update_low_power_sleep_lock_() {
#ifdef USE_XR806_LOWPOWER
  esphome::xr806::lowpower_policy().set_mcu_synchronized(this->low_power_ready_for_sleep_());
#endif
}

bool Tuya::request_low_power_refresh() {
  if (this->protocol_ != TuyaProtocol::LOW_POWER_V0 || !this->lp_v0_.has_product() ||
      this->lp_v0_.busy() || this->expected_response_.has_value())
    return false;
  this->send_datapoint_command_(106, TuyaDatapointType::BOOLEAN, {0x01});
  return true;
}

bool Tuya::notify_low_power_transition(bool sleeping) {
  // The PM bridge must invoke this only at an actual suspend/resume boundary.
  // A CMD38 request is NOT evidence that the CPU entered sleep.
  if (this->protocol_ != TuyaProtocol::LOW_POWER_V0 || !this->lp_v0_.has_product())
    return false;
  if (sleeping) {
    if (this->lp_sleep_notified_ || !this->low_power_ready_for_sleep_())
      return false;
#ifdef USE_XR806_LOWPOWER
    if (!esphome::xr806::lowpower_policy().can_enter_standby())
      return false;
#else
    return false;  // Never advertise sleep without XR806 PM support.
#endif
    if (this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS)
      return false;
    this->send_low_power_direct_(0x12, {0x01});
    if (this->flush() != uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS)
      return false;
    this->lp_sleep_notified_ = true;
    return true;
  }

  if (!this->lp_sleep_notified_)
    return false;  // GPIO wake alone does not always imply CMD12/00.
  this->lp_sleep_notified_ = false;
  this->lp_wake_guard_until_ = millis() + 5000;
  this->send_low_power_direct_(0x12, {0x00});
  this->update_low_power_sleep_lock_();
  return true;
}

void Tuya::process_low_power_queue_() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - this->last_rx_char_timestamp_) > RECEIVE_TIMEOUT)
    this->rx_message_.clear();

  // Standard Tuya's 300 ms retry logic is incompatible with the HC32's
  // delayed CMD05 report. ACK and report have separate timeouts.
  if (this->expected_response_.has_value() &&
      static_cast<uint32_t>(now - this->last_command_timestamp_) >= 1500) {
    const bool product_query = *this->expected_response_ == TuyaCommandType::PRODUCT_QUERY;
    this->expected_response_.reset();
    if (!this->command_queue_.empty())
      this->command_queue_.erase(this->command_queue_.begin());
    this->lp_v0_.command_timeout();
    this->lp_network_status_ = 0xFF;
    ESP_LOGW(LP_TAG, "MCU command ACK timeout (product=%s); scheduling resync", YESNO(product_query));
    if (product_query && !this->auto_baud_) {
      if (++this->init_retries_ >= MAX_RETRIES) {
        this->init_failed_ = true;
        ESP_LOGE(LP_TAG, "MCU product query failed after %d attempts", this->init_retries_);
      } else {
        this->command_queue_.insert(this->command_queue_.begin(),
                                    {.cmd = TuyaCommandType::PRODUCT_QUERY, .payload = {}});
      }
    }
  }

  if (this->lp_v0_.busy() || this->expected_response_.has_value() ||
      !this->rx_message_.empty() || this->command_queue_.empty() ||
      static_cast<uint32_t>(now - this->last_command_timestamp_) <= COMMAND_DELAY)
    return;
  if (!this->lp_v0_.has_product() &&
      this->command_queue_.front().cmd != TuyaCommandType::PRODUCT_QUERY)
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
      0x55, 0xAA, 0x00, command,
      static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size()),
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
          1, static_cast<uint8_t>(now.year - 2000), static_cast<uint8_t>(now.month),
          static_cast<uint8_t>(now.day_of_month), static_cast<uint8_t>(now.hour),
          static_cast<uint8_t>(now.minute), static_cast<uint8_t>(now.second), weekday,
      };
    }
  }
#endif
  this->send_low_power_direct_(0x06, payload);
}

void Tuya::handle_low_power_command_(uint8_t command, uint8_t version, const uint8_t *data, size_t len) {
  (void) version;
  switch (command) {
    case 0x00:  // First MCU heartbeat after reset is 00; subsequent is 01.
      if (len != 1 || data[0] != 0)
        return;
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
      if (!this->lp_product_.parse(data, len) || !this->lp_product_.is_verified_trv16()) {
        ESP_LOGE(LP_TAG, "Unsupported/malformed MCU product info; refusing initialization");
        this->product_invalid_ = true;
        this->init_failed_ = true;
        this->mark_failed();
        return;
      }
      this->product_ = std::make_unique<char[]>(len + 1);
      std::memcpy(this->product_.get(), data, len);
      this->product_[len] = '\0';
      this->lp_v0_.product_received();
      this->init_state_ = TuyaInitState::INIT_DATAPOINT;
      ESP_LOGI(LP_TAG, "TRV16 PID %s version %s wk=%d mwk=%d sm=%d", this->lp_product_.pid.c_str(),
               this->lp_product_.version.c_str(), this->lp_product_.wake_gpio,
               this->lp_product_.mcu_wake_gpio, this->lp_product_.sleep_mode);
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
        if (size > len - offset - 4 || type > 5 ||
            ((type == 1 || type == 4) && size != 1) ||
            (type == 2 && size != 4) ||
            (type == 1 && data[offset + 4] > 1))
          return;
        offset += 4 + size;
      }
      offset = 0;
      while (offset < len) {
        const size_t size = (static_cast<size_t>(data[offset + 2]) << 8) | data[offset + 3];
        this->send_low_power_direct_(0x05, {0x00, data[offset]});
        this->lp_v0_.observe_dp(data[offset], data[offset + 1], data + offset + 4, size);
        offset += 4 + size;
      }
      this->handle_datapoints_(data, len);
      if (this->init_state_ != TuyaInitState::INIT_DONE && this->lp_v0_.synchronized()) {
        this->init_state_ = TuyaInitState::INIT_DONE;
        this->initialized_callback_.call();
        ESP_LOGI(LP_TAG, "Initial MCU status synchronized");
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

    case 0x38: {  // MCU asks whether it may enter low-power.
      if (len != 1 || data[0] != 0)
        return;
      uint8_t flags = 0x01;  // Fail closed: actual XR806 PM entry is not integrated.
#ifdef USE_XR806_LOWPOWER
      const uint32_t blockers = esphome::xr806::lowpower_policy().blockers();
      if (blockers & esphome::xr806::XR806LowPowerPolicy::OTA_BUSY)
        flags |= 0x02;
#endif
      if (!this->low_power_ready_for_sleep_())
        flags |= 0x01;
      this->send_low_power_direct_(0x38, {0x00, flags});
      return;
    }

    default:
      ESP_LOGV(LP_TAG, "Unhandled low-power MCU CMD 0x%02X", command);
      return;
  }
}

}  // namespace esphome::tuya
