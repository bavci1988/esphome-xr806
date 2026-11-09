#pragma once
// SDK-neutral state for an OTA flash-write PM lock.
// Call only after acquiring the process-wide OTA UpdateLock. OTA remains a
// separate fail-closed standby owner; this gate NEVER votes OTA idle.
#include <cstdint>

namespace esphome::xr806 {

class XR806OTAUpdatePMGate {
 public:
  using Veto = void (*)(void *);
  using PMCallback = bool (*)(void *);

  enum class State : uint8_t { IDLE, HELD, FAULTED };

  bool acquire(Veto veto, PMCallback lock, void *context) {
    if (veto == nullptr || lock == nullptr || state_ != State::IDLE)
      return false;
    veto(context);  // Make the software veto visible before beginning flash work.
    if (!lock(context)) {
      state_ = State::FAULTED;  // Fail closed for the remainder of this boot.
      return false;
    }
    state_ = State::HELD;
    return true;
  }

  bool release(Veto veto, PMCallback unlock, void *context) {
    if (veto == nullptr || unlock == nullptr || state_ != State::HELD)
      return false;
    veto(context);  // Never publish OTA idle merely because writing has ended.
    if (!unlock(context)) {
      state_ = State::FAULTED;  // Caller must retain the global update lock.
      return false;
    }
    state_ = State::IDLE;
    return true;
  }

  State state() const { return state_; }
  bool held() const { return state_ == State::HELD; }
  bool faulted() const { return state_ == State::FAULTED; }

 private:
  State state_{State::IDLE};
};

}  // namespace esphome::xr806
