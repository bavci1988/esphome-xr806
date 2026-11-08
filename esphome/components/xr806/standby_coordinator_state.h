#pragma once

// Platform-only gating for an explicitly requested XR806 CPU standby.
// No Tuya/HC32/DP/UART frame dependencies; host-testable without the SDK.
#include <cstdint>

namespace esphome::xr806 {

class XR806StandbyCoordinatorState {
 public:
  static constexpr uint32_t WAKE_TIMER = 1u << 16;
  static constexpr uint32_t WAKE_SOFTWARE = 1u << 27;
  static constexpr uint32_t WAKE_INTERNAL = 0xf0000000u;
  static constexpr uint8_t MAX_FAILURES = 3;

  void configure(uint32_t quiet_ms, uint32_t awake_ms) {
    quiet_ms_ = quiet_ms;
    awake_ms_ = awake_ms;
  }

  // A fresh boot gets the same initial awake guard as a physical resume.
  void boot(uint32_t now) {
    booted_ = true;
    last_wake_ms_ = now;
    stable_ = false;
    failures_ = 0;
    permanently_disabled_ = false;
  }

  // Only the application/protocol owner can request a sleep. The platform
  // never synthesizes an MCU CMD38 request, or turns an ACK into consent.
  bool due(uint32_t now, uint32_t generation, bool requested, bool all_owners_idle) {
    if (!booted_ || permanently_disabled_ || !requested || !all_owners_idle) {
      stable_ = false;
      return false;
    }

    if (!stable_ || observed_generation_ != generation) {
      observed_generation_ = generation;
      stable_since_ms_ = now;
      stable_ = true;
      return false;
    }

    return static_cast<uint32_t>(now - last_wake_ms_) >= awake_ms_ &&
           static_cast<uint32_t>(now - stable_since_ms_) >= quiet_ms_;
  }

  // Restart the quiet interval after any failed lifecycle preparation.
  void postpone() { stable_ = false; }

  // Consume one request, even when SDK returns immediately or fails.
  // A later attempt requires a new external request plus a quiet interval.
  void finished(uint32_t now, bool expected_resume) {
    stable_ = false;
    last_wake_ms_ = now;
    if (expected_resume) {
      failures_ = 0;
    } else if (++failures_ >= MAX_FAILURES) {
      permanently_disabled_ = true;
    }
  }

  bool disabled() const { return permanently_disabled_; }
  uint8_t failures() const { return failures_; }

  // A successful PM return plus a timer/GPIO event is software evidence,
  // not proof that the hardware spent time in PM_MODE_STANDBY.
  static bool expected_wake(int pm_return, uint32_t events, int wake_io) {
    if (pm_return != 0 || (events & (WAKE_SOFTWARE | WAKE_INTERNAL)) != 0)
      return false;

    uint32_t expected = WAKE_TIMER;
    if (wake_io >= 0 && wake_io < 14)
      expected |= (1u << wake_io);
    return (events & expected) != 0;
  }

 private:
  uint32_t quiet_ms_{250};
  uint32_t awake_ms_{5000};
  uint32_t last_wake_ms_{0};
  uint32_t stable_since_ms_{0};
  uint32_t observed_generation_{0};
  uint8_t failures_{0};
  bool booted_{false};
  bool stable_{false};
  bool permanently_disabled_{false};
};

}  // namespace esphome::xr806
