#pragma once
#include <cstdint>

namespace esphome::xr806 {

// Platform-neutral retry state for XR806 WLAN DTIM/PS driver calls.
// A failed request is retried at most every 30 seconds while associated.
// Reset on disconnect so a fresh association is configured immediately.
class XR806WiFiPowerSaveRetry {
 public:
  static constexpr uint32_t RETRY_INTERVAL_MS = 30000U;

  bool due(uint32_t now) const {
    return !applied_ && (!attempted_ || static_cast<uint32_t>(now - last_attempt_ms_) >= RETRY_INTERVAL_MS);
  }

  void record(uint32_t now, bool success) {
    attempted_ = true;
    last_attempt_ms_ = now;
    applied_ = success;
  }

  void reset() {
    applied_ = false;
    attempted_ = false;
    last_attempt_ms_ = 0;
  }

  bool applied() const { return applied_; }

 private:
  uint32_t last_attempt_ms_{0};
  bool attempted_{false};
  bool applied_{false};
};

}  // namespace esphome::xr806
