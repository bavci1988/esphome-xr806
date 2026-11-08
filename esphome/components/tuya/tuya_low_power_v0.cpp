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

void Tuya::update_low_power_sleep_lock_() {
#ifdef USE_XR806_LOWPOWER
  // The generic XR806 PM gate remains closed during incomplete MCU sync.
  esphome::xr806::lowpower_policy().set_mcu_synchronized(this->lp_v0_.ready());
#endif
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
      if (!len)
        return;
      for (size_t i = 0; i < len; ++i)
        if (!std::isprint(data[i]))
          return;
      this->product_ = std::make_unique<char[]>(len + 1);
      std::memcpy(this->product_.get(), data, len);
      this->product_[len] = '\0';
      this->lp_v0_.product_received();
      this->init_state_ = TuyaInitState::INIT_DATAPOINT;
      this->send_low_power_network_();
      return;
    }

    case 0x02:  // Network-state ACK; generic queue handler clears expected CMD.
      return;

    case 0x05: {  // MCU datapoint report: ACK 00 + DPID.
      if (len < 5)
        return;
      const size_t size = (static_cast<size_t>(data[2]) << 8) | data[3];
      if (size + 4 != len)
        return;
      this->send_low_power_direct_(0x05, {0x00, data[0]});
      this->lp_v0_.observe_dp(data[0], data[1], data + 4, size);
      this->handle_datapoints_(data, len);
      if (this->init_state_ != TuyaInitState::INIT_DONE && this->lp_v0_.ready()) {
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

    case 0x38:  // Explicit fail-closed until XR806 hardware PM is integrated.
      if (len == 1 && data[0] == 0)
        this->send_low_power_direct_(0x38, {0x00, 0x01});
      return;

    default:
      ESP_LOGV(LP_TAG, "Unhandled low-power MCU CMD 0x%02X", command);
      return;
  }
}

}  // namespace esphome::tuya
