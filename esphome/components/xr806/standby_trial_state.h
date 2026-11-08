#pragma once

#include <cstdint>

namespace esphome::xr806 {

// Hardware-independent one-shot scheduling and wake-event classification.
// The hardware driver must separately verify actual standby entry.
class XR806StandbyTrialState {
 public:
  void arm(uint32_t now_ms) {
    armed_ = true;
    attempted_ = false;
    armed_at_ms_ = now_ms;
  }

  bool claim(uint32_t now_ms, uint32_t delay_ms) {
    if (!armed_ || attempted_ || static_cast<uint32_t>(now_ms - armed_at_ms_) < delay_ms)
      return false;
    attempted_ = true;  // Never loop on a failed PM entry.
    return true;
  }

  bool attempted() const { return attempted_; }

  static bool resumed_from_expected_source(int pm_result, uint32_t event, int wake_io_index) {
    return pm_result == 0 && expected_wake_event(event, wake_io_index);
  }

  static bool expected_wake_event(uint32_t event, int wake_io_index) {
    constexpr uint32_t wake_timer = 1u << 16;
    uint32_t expected = wake_timer;
    if (wake_io_index >= 0 && wake_io_index <= 13)
      expected |= 1u << wake_io_index;
    constexpr uint32_t software_or_internal = (1u << 27) | 0xf0000000u;
    return (event & software_or_internal) == 0 && (event & expected) != 0;
  }

 private:
  uint32_t armed_at_ms_{0};
  bool armed_{false};
  bool attempted_{false};
};

}  // namespace esphome::xr806
