#pragma once

// Host-testable transaction state for the Tuya MCU low-power v0 dialect.
// No XR806 or ESPHome runtime dependency.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::tuya {

class TuyaLowPowerV0Session {
 public:
  static constexpr std::array<uint8_t, 17> FULL_STATUS_DPS = {
      1, 3, 4, 2, 17, 16, 10, 11, 7, 19, 20, 108, 101, 102, 104, 105, 107,
  };

  void product_received() { product_ = true; }
  bool has_product() const { return product_; }

  void reset_status() {
    synced_ = false;
    status_mask_ = 0;
    write_pending_ = false;
    write_acked_ = false;
    write_report_seen_ = false;
  }

  void begin_full_status(uint32_t now) {
    status_mask_ = (1u << FULL_STATUS_DPS.size()) - 1;
    synced_ = false;
    status_started_ = now;
  }

  void begin_write(uint8_t id, uint8_t type, const uint8_t *data, size_t len, uint32_t now) {
    write_pending_ = true;
    write_acked_ = false;
    write_report_seen_ = false;
    write_id_ = id;
    write_type_ = type;
    expected_.assign(data, data + len);
    write_started_ = now;
  }

  void write_ack() {
    write_acked_ = true;
    if (write_report_seen_)
      write_pending_ = false;
  }

  void observe_dp(uint8_t id, uint8_t type, const uint8_t *data, size_t len) {
    if (status_mask_ != 0) {
      for (size_t i = 0; i < FULL_STATUS_DPS.size(); ++i) {
        if (FULL_STATUS_DPS[i] == id)
          status_mask_ &= ~(1u << i);
      }
      if (status_mask_ == 0) {
        synced_ = true;
        fault_ = false;
      }
    }

    if (write_pending_ && write_id_ == id && write_type_ == type && expected_.size() == len) {
      bool match = true;
      for (size_t i = 0; i < len; ++i)
        if (expected_[i] != data[i])
          match = false;
      if (match) {
        write_report_seen_ = true;
        if (write_acked_)
          write_pending_ = false;
      }
    }
  }

  bool expire(uint32_t now, uint32_t max_age_ms) {
    bool timed_out = false;
    if (status_mask_ && static_cast<uint32_t>(now - status_started_) > max_age_ms) {
      status_mask_ = 0;
      synced_ = false;
      timed_out = true;
    }
    if (write_pending_ && static_cast<uint32_t>(now - write_started_) > max_age_ms) {
      write_pending_ = false;
      timed_out = true;
    }
    if (timed_out)
      fault_ = true;
    return timed_out;
  }

  bool busy() const { return status_mask_ != 0 || write_pending_; }
  bool ready() const { return product_ && synced_ && !fault_ && !busy(); }

 private:
  bool product_{false};
  bool synced_{false};
  bool fault_{false};
  uint32_t status_mask_{0};
  uint32_t status_started_{0};
  uint32_t write_started_{0};
  bool write_pending_{false};
  bool write_acked_{false};
  bool write_report_seen_{false};
  uint8_t write_id_{0};
  uint8_t write_type_{0};
  std::vector<uint8_t> expected_{};
};

}  // namespace esphome::tuya
