#pragma once

#include <cstdint>

namespace esphome::tuya {

// The version-0 sleep request (CMD38) and the version-3 sleep handshake (0x91)
static constexpr uint8_t LP_V0_SLEEP_CMD = 0x38;
static constexpr uint8_t LP_V3_SLEEP_CMD = 0x91;
// No sleep for this long after a wake or a new network state: the MCU reports first
static constexpr uint32_t LP_WAKE_GUARD_MS = 5000;

/// The MCU's consent to a standby of the module, testable on the host without a platform.
///
/// Version 0 (sm=1): the MCU asks with CMD38, the module grants it (38 00 00), tells the MCU with CMD12/01 that it
/// sleeps now and with CMD12/00 that it woke. Version 3 (sm=2): the module offers sleep with 0x91 and the MCU's
/// answer is the whole handshake.
class TuyaLowPowerStandbyState {
 public:
  enum class ResumeAction : uint8_t {
    RESUME_ACTION_NONE,
    RESUME_ACTION_SEND_WAKE,
    RESUME_ACTION_REQUIRE_RESYNC,
    RESUME_ACTION_KEEP_CONSENT,
  };

  /// The caller has checked that the MCU link and the platform are idle.
  bool accept_request() {
    if (this->requested_ || this->notified_ || this->recovery_required_)
      return false;
    this->requested_ = true;
    return true;
  }

  /// A version-3 MCU sleeps right after its answer, so its consent counts as notified.
  bool accept_offer() {
    if (!this->accept_request())
      return false;
    this->notified_ = true;
    return true;
  }

  /// A fresh full status ends a recovery.
  void recovered() { this->recovery_required_ = false; }

  bool requested() const { return this->requested_; }
  bool recovery_required() const { return this->recovery_required_; }
  bool needs_notification() const { return this->requested_ && !this->notified_; }
  bool ready_for_pm() const {
    return this->requested_ && this->notified_ && !this->deferred_ && !this->recovery_required_;
  }

  /// CMD12/01 was sent. The standby attempt that sent it is declined on purpose, so the UART can drain first.
  void notification_sent() {
    this->notified_ = true;
    this->deferred_ = true;
  }

  /// The platform declined a standby attempt. The first decline after CMD12/01 is the intended one; true when a
  /// later one needs CMD12/00 to tell the MCU that the module stays awake.
  bool cancelled() {
    if (this->deferred_) {
      this->deferred_ = false;
      return false;
    }
    return this->force_cancel();
  }

  /// Ends the consent; true when CMD12/00 must tell the MCU that the module stays awake.
  bool force_cancel() {
    const bool compensate = this->notified_;
    this->requested_ = false;
    this->notified_ = false;
    this->deferred_ = false;
    return compensate;
  }

  /// Only an expected wake by the MCU's own wake line proves the path where the MCU expects CMD12/00. Any other
  /// wake after CMD12/01 leaves the MCU's state unknown until a full status.
  /// keep_consent: the network stays connected during standby and the module woke on its own (timer or network
  /// data) while the MCU still sleeps; the consent and the sent CMD12/01 stay valid, so the module may sleep again
  /// without a new request.
  ResumeAction resumed(bool expected_wake, bool gpio_wake, bool keep_consent = false) {
    if (keep_consent && this->notified_ && expected_wake && !gpio_wake && !this->recovery_required_) {
      this->deferred_ = false;
      return ResumeAction::RESUME_ACTION_KEEP_CONSENT;
    }
    const bool had_notification = this->notified_;
    this->requested_ = false;
    this->notified_ = false;
    this->deferred_ = false;
    if (!had_notification)
      return ResumeAction::RESUME_ACTION_NONE;
    if (expected_wake && gpio_wake)
      return ResumeAction::RESUME_ACTION_SEND_WAKE;
    this->recovery_required_ = true;
    return ResumeAction::RESUME_ACTION_REQUIRE_RESYNC;
  }

  /// A frame may not have left the UART: no consent until a full status.
  void transport_fault() {
    this->recovery_required_ = true;
    this->requested_ = false;
    this->notified_ = false;
    this->deferred_ = false;
  }

  /// A restarted MCU ends the old handshake.
  void reset_on_mcu_boot() {
    this->requested_ = false;
    this->notified_ = false;
    this->deferred_ = false;
    this->recovery_required_ = false;
  }

 protected:
  bool requested_{false};
  bool notified_{false};
  bool deferred_{false};
  bool recovery_required_{false};
};

}  // namespace esphome::tuya
