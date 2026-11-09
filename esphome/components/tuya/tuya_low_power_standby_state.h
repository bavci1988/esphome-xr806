#pragma once

#include <cstdint>

namespace esphome::tuya {

// Pure MCU-protocol state. The XR806 platform never interprets CMD12/38 or
// the HC32's 'sm' product field. All transitions are host-testable.
class TuyaLowPowerStandbyState {
 public:
  enum class ResumeAction : uint8_t { NONE, SEND_CMD12_WAKE, REQUIRE_RESYNC };

  bool accept_request(bool standby_mode, bool mcu_idle, bool uart_healthy, bool other_owners_idle) {
    if (requested_ || notified_ || recovery_required_ || !standby_mode || !mcu_idle || !uart_healthy ||
        !other_owners_idle)
      return false;
    requested_ = true;
    return true;
  }

  bool requested() const { return requested_; }
  bool notified() const { return notified_; }
  bool recovery_required() const { return recovery_required_; }
  bool needs_notification() const { return requested_ && !notified_; }
  bool ready_for_pm() const { return requested_ && notified_ && !deferred_ && !recovery_required_; }

  // The CMD12/01 frame is sent, fully drained, and the current preparation
  // deliberately returns false so UART/scheduler idle must stabilize again.
  void notification_sent() {
    notified_ = true;
    deferred_ = true;
  }

  // Called when an attempt is declined by the platform. The *first* decline
  // after CMD12/01 is intentional. Any later veto must compensate CMD12/00.
  bool cancelled() {
    if (deferred_) {
      deferred_ = false;
      return false;
    }
    return this->force_cancel();
  }

  // Returns true if a compensating CMD12/00 is necessary. For a failed PM
  // request this is an abort notification, not a GPIO wake notification.
  bool force_cancel() {
    const bool compensate = notified_;
    requested_ = false;
    notified_ = false;
    deferred_ = false;
    return compensate;
  }

  ResumeAction resumed(bool expected_wake, bool gpio_wake) {
    const bool had_notification = notified_;
    requested_ = false;
    notified_ = false;
    deferred_ = false;
    if (!had_notification)
      return ResumeAction::NONE;
    if (expected_wake && gpio_wake)
      return ResumeAction::SEND_CMD12_WAKE;
    // Timer-only, unknown/aborted PM or an unexpected source does not prove
    // the original WXU's conditional CMD12/00 GPIO-wake path occurred.
    recovery_required_ = true;
    return ResumeAction::REQUIRE_RESYNC;
  }

  void transport_fault() {
    recovery_required_ = true;
    requested_ = false;
    notified_ = false;
    deferred_ = false;
  }

  // An observed MCU reset invalidates the old sleep transaction. Higher
  // layers must still re-run CMD01 and the full datapoint state sequence.
  void reset_on_mcu_boot() {
    requested_ = false;
    notified_ = false;
    deferred_ = false;
    recovery_required_ = false;
  }

 private:
  bool requested_{false};
  bool notified_{false};
  bool deferred_{false};
  bool recovery_required_{false};
};

}  // namespace esphome::tuya
