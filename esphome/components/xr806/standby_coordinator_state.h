#pragma once

// Platform-only gating for an explicitly requested XR806 CPU standby.
// No Tuya/HC32/DP/UART frame dependencies; host-testable without the SDK.
#include <algorithm>
#include <cstdint>

namespace esphome::xr806 {

class XR806StandbyCoordinatorState {
 public:
  static constexpr uint32_t WAKE_TIMER = 1u << 16;
  static constexpr uint32_t WAKE_WLAN = 1u << 17;
  static constexpr uint32_t WAKE_SOFTWARE = 1u << 27;
  static constexpr uint32_t WAKE_INTERNAL = 0xf0000000u;
  // After an unexpected wake, the wait before the next attempt doubles up to this.
  static constexpr uint32_t MAX_BACKOFF_MS = 10 * 60 * 1000;

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
  }

  // Only the application/protocol owner can request a sleep. The platform
  // never synthesizes an MCU CMD38 request, or turns an ACK into consent.
  bool due(uint32_t now, uint32_t generation, bool requested, bool all_owners_idle) {
    if (!booted_ || !requested || !all_owners_idle) {
      stable_ = false;
      return false;
    }

    if (!stable_ || observed_generation_ != generation) {
      observed_generation_ = generation;
      stable_since_ms_ = now;
      stable_ = true;
      return false;
    }

    return static_cast<uint32_t>(now - last_wake_ms_) >= this->awake_hold_ms() &&
           static_cast<uint32_t>(now - stable_since_ms_) >= quiet_ms_;
  }

  // The minimum awake time, doubled for every unexpected wake in a row, up to MAX_BACKOFF_MS.
  uint32_t awake_hold_ms() const {
    uint32_t hold = awake_ms_;
    for (uint8_t i = 0; i < failures_ && hold < MAX_BACKOFF_MS; ++i)
      hold *= 2;
    return std::min(hold, MAX_BACKOFF_MS);
  }

  // Restart the quiet interval after any failed lifecycle preparation.
  void postpone() { stable_ = false; }

  // Consume one request after the chip went through PM. An unexpected wake lengthens the
  // awake hold before the next attempt; an expected one resets it.
  void finished(uint32_t now, bool expected_resume) {
    stable_ = false;
    last_wake_ms_ = now;
    if (expected_resume) {
      failures_ = 0;
    } else if (failures_ < UINT8_MAX) {
      ++failures_;
    }
  }

  // Consume one request that did not reach the sleep (activity at the last moment, the wake
  // line already active, a wakelock still held). Not a failure.
  void not_entered(uint32_t now) {
    stable_ = false;
    last_wake_ms_ = now;
  }

  uint8_t failures() const { return failures_; }

  // A successful PM return plus a timer/GPIO event is software evidence,
  // not proof that the hardware spent time in PM_MODE_STANDBY.
  // wlan_wake: the WLAN link stayed up, so received network data is an expected source.
  static bool expected_wake(int pm_return, uint32_t events, int wake_io, bool wlan_wake = false) {
    if ((events & (WAKE_SOFTWARE | WAKE_INTERNAL)) != 0)
      return false;
    // With the link kept up, the WLAN driver refuses to suspend while it still has received data
    // to deliver. The chip did not sleep, nothing was lost, and the next attempt may succeed.
    if (pm_return != 0)
      return wlan_wake && (events & WAKE_WLAN) != 0;

    uint32_t expected = WAKE_TIMER | (wlan_wake ? WAKE_WLAN : 0u);
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
};

}  // namespace esphome::xr806
