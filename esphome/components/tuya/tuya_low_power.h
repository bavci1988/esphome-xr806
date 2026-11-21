#pragma once

// MCU transaction bookkeeping, testable on the host without ESPHome or a platform.
// Command 09 ACK and command 05 state reports are distinct events.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::tuya {

class TuyaLowPowerSession {
 public:
  /// Datapoints the HC32 firmware of the TRV16 reports after network state 04, and after a refresh.
  /// They are the defaults; a device with other datapoints sets its own lists.
  static constexpr std::array<uint8_t, 17> FULL_STATUS_DPS = {
      1, 3, 4, 2, 17, 16, 10, 11, 7, 19, 20, 108, 101, 102, 104, 105, 107,
  };
  static constexpr std::array<uint8_t, 8> REFRESH_DPS = {1, 3, 4, 2, 17, 16, 19, 20};
  static constexpr uint8_t REFRESH_TRIGGER_DP = 106;
  /// The pending sets are bit masks, one bit per listed datapoint.
  static constexpr size_t MAX_LISTED_DPS = 32;
  static constexpr uint32_t REPORT_TIMEOUT_MS = 12000;

  enum class WriteResult : uint8_t { NONE, CONFIRMED, TIMED_OUT };

  TuyaLowPowerSession() {
    this->set_status_dps(FULL_STATUS_DPS.data(), FULL_STATUS_DPS.size());
    this->set_refresh_dps(REFRESH_DPS.data(), REFRESH_DPS.size());
  }
  /// Datapoints that must all be reported before the MCU status counts as synchronized.
  void set_status_dps(const uint8_t *ids, size_t count) { status_count_ = copy_list_(status_dps_, ids, count); }
  /// Datapoints the MCU reports after a write of 1 to the refresh trigger.
  void set_refresh_dps(const uint8_t *ids, size_t count) { refresh_count_ = copy_list_(refresh_dps_, ids, count); }
  void set_refresh_trigger_dp(uint8_t id) { refresh_trigger_dp_ = id; }
  uint8_t refresh_trigger_dp() const { return refresh_trigger_dp_; }
  void product_received() { product_ = true; }
  bool has_product() const { return product_; }
  bool synchronized() const { return synced_; }
  bool faulted() const { return fault_; }
  bool write_pending() const { return write_pending_; }
  bool awaiting_ack() const { return write_pending_ && !write_acked_; }
  bool refresh_pending() const { return write_pending_ && write_refresh_; }
  WriteResult last_write_result() const { return last_result_; }

  // MCU or network epoch changed: all previous state is now suspect.
  void reset_status() {
    synced_ = false;
    status_mask_ = 0;
    full_status_active_ = false;
    write_pending_ = false;
    write_acked_ = false;
    write_seen_ = false;
    write_refresh_ = false;
    refresh_mask_ = 0;
    last_result_ = WriteResult::NONE;
  }

  void begin_full_status(uint32_t now) {
    status_mask_ = full_mask_(status_count_);
    synced_ = false;
    full_status_active_ = true;
    status_started_ms_ = now;
  }

  void begin_write(uint8_t id, uint8_t type, const uint8_t *value, size_t len, uint32_t now) {
    write_pending_ = true;
    write_acked_ = false;
    write_seen_ = false;
    write_refresh_ = (id == refresh_trigger_dp_ && type == 1 && len == 1 && value != nullptr && value[0] == 1);
    refresh_mask_ = write_refresh_ ? full_mask_(refresh_count_) : 0;
    last_result_ = WriteResult::NONE;
    write_id_ = id;
    write_type_ = type;
    expected_.clear();
    if (len != 0 && value != nullptr)
      expected_.assign(value, value + len);
    write_started_ms_ = now;
  }

  void write_ack() {
    if (!write_pending_)
      return;
    write_acked_ = true;
    finish_write_();
  }

  void observe_dp(uint8_t id, uint8_t type, const uint8_t *value, size_t len) {
    for (size_t i = 0; i < status_count_; ++i) {
      if (status_mask_ && status_dps_[i] == id) {
        status_mask_ &= ~(1u << i);
      }
    }
    if (status_started_() && status_mask_ == 0) {
      full_status_active_ = false;
      synced_ = true;
      fault_ = false;
    }

    if (!write_pending_) {
      return;
    }
    if (write_refresh_) {
      for (size_t i = 0; i < refresh_count_; ++i) {
        if (refresh_dps_[i] == id) {
          refresh_mask_ &= ~(1u << i);
        }
      }
      if (refresh_mask_ == 0) {
        write_seen_ = true;
      }
    } else if (id == write_id_ && type == write_type_ && len == expected_.size()) {
      bool equal = true;
      for (size_t i = 0; i < len; ++i) {
        if (expected_[i] != value[i]) {
          equal = false;
        }
      }
      // Mismatched/clamped MCU reports are published as *actual* state by
      // Tuya, but are not mistaken for confirmation of the requested value.
      if (equal) {
        write_seen_ = true;
      }
    }
    finish_write_();
  }

  bool expire(uint32_t now, uint32_t max_age_ms = REPORT_TIMEOUT_MS) {
    bool expired = false;
    if (full_status_active_ && static_cast<uint32_t>(now - status_started_ms_) >= max_age_ms) {
      status_mask_ = 0;
      full_status_active_ = false;
      synced_ = false;
      expired = true;
    }
    if (write_pending_ && static_cast<uint32_t>(now - write_started_ms_) >= max_age_ms) {
      write_pending_ = false;
      write_refresh_ = false;
      refresh_mask_ = 0;
      last_result_ = WriteResult::TIMED_OUT;
      expired = true;
    }
    if (expired) {
      synced_ = false;
      fault_ = true;
    }
    return expired;
  }

  void command_timeout() {
    synced_ = false;
    fault_ = true;
    if (write_pending_) {
      write_pending_ = false;
      write_refresh_ = false;
      refresh_mask_ = 0;
      last_result_ = WriteResult::TIMED_OUT;
    }
    status_mask_ = 0;
    full_status_active_ = false;
  }

  // MCUs that frame with version 3 report their status (CMD07) in one go after CMD08 and have no
  // fixed DP list to wait for: their first complete report synchronizes the session.
  void mark_synchronized() {
    status_mask_ = 0;
    full_status_active_ = false;
    synced_ = true;
    fault_ = false;
  }

  bool busy() const { return full_status_active_ || write_pending_; }
  bool ready() const { return product_ && synced_ && !fault_ && !busy(); }

 private:
  bool status_started_() const { return full_status_active_; }
  static uint32_t full_mask_(size_t count) { return count >= 32 ? 0xFFFFFFFFu : (1u << count) - 1u; }
  static uint8_t copy_list_(std::array<uint8_t, MAX_LISTED_DPS> &list, const uint8_t *ids, size_t count) {
    if (ids == nullptr)
      count = 0;
    if (count > MAX_LISTED_DPS)
      count = MAX_LISTED_DPS;
    for (size_t i = 0; i < count; ++i)
      list[i] = ids[i];
    return static_cast<uint8_t>(count);
  }
  void finish_write_() {
    if (!write_pending_ || !write_acked_ || !write_seen_)
      return;
    write_pending_ = false;
    write_refresh_ = false;
    refresh_mask_ = 0;
    last_result_ = WriteResult::CONFIRMED;
  }

  std::array<uint8_t, MAX_LISTED_DPS> status_dps_{};
  std::array<uint8_t, MAX_LISTED_DPS> refresh_dps_{};
  uint8_t status_count_{0};
  uint8_t refresh_count_{0};
  uint8_t refresh_trigger_dp_{REFRESH_TRIGGER_DP};
  bool product_{false};
  bool synced_{false};
  bool fault_{false};
  uint32_t status_mask_{0};
  uint32_t status_started_ms_{0};
  bool full_status_active_{false};
  uint32_t write_started_ms_{0};
  bool write_pending_{false};
  bool write_acked_{false};
  bool write_seen_{false};
  bool write_refresh_{false};
  uint32_t refresh_mask_{0};
  uint8_t write_id_{0};
  uint8_t write_type_{0};
  std::vector<uint8_t> expected_{};
  WriteResult last_result_{WriteResult::NONE};
};

}  // namespace esphome::tuya
