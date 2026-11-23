#pragma once

#include <cstdint>

namespace esphome::xr806 {

// Hardware-idle quiet interval used before releasing the XR806 PM wakelock.
// No timestamp from the beginning of TX is allowed to shorten the interval.
class UARTIdleGuard {
 public:
  void reset() { this->observing_idle_ = false; }

  bool ready(bool hardware_idle, uint32_t now_ms, uint32_t guard_ms) {
    if (!hardware_idle) {
      this->reset();
      return false;
    }
    if (!this->observing_idle_) {
      this->idle_since_ms_ = now_ms;
      this->observing_idle_ = true;
      return false;
    }
    return now_ms - this->idle_since_ms_ >= guard_ms;
  }

 private:
  uint32_t idle_since_ms_{0};
  bool observing_idle_{false};
};

}  // namespace esphome::xr806
