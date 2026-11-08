#pragma once

#include <cstdint>

namespace esphome::xr806 {

class XR806WLANReconnect {
 public:
  static constexpr uint32_t INITIAL_TIMEOUT_MS = 30000;
  static constexpr uint32_t DISCONNECT_GRACE_MS = 10000;
  static constexpr uint32_t FIRST_RETRY_GAP_MS = 15000;
  static constexpr uint32_t MAX_RETRY_GAP_MS = 120000;

  void begin(uint32_t now) {
    online_ = false;
    attempts_ = 0;
    next_retry_ms_ = now + INITIAL_TIMEOUT_MS;
  }

  bool poll(uint32_t now, bool ready) {
    if (ready) {
      online_ = true;
      attempts_ = 0;
      return false;
    }

    if (online_) {
      online_ = false;
      attempts_ = 0;
      next_retry_ms_ = now + DISCONNECT_GRACE_MS;
    }

    if (static_cast<int32_t>(now - next_retry_ms_) < 0)
      return false;

    const uint32_t shift = attempts_ < 3 ? attempts_ : 3;
    const uint32_t gap = FIRST_RETRY_GAP_MS << shift;

    if (attempts_ < 255)
      ++attempts_;

    next_retry_ms_ = now +
        (gap < MAX_RETRY_GAP_MS ? gap : MAX_RETRY_GAP_MS);

    return true;
  }

  uint32_t attempts() const { return attempts_; }
  bool is_online() const { return online_; }

 private:
  uint32_t next_retry_ms_{0};
  uint32_t attempts_{0};
  bool online_{false};
};

}  // namespace esphome::xr806
