// Host-only production PM gating tests; no XR806 SDK or Tuya dependency.
#include <cassert>
#include <cstdint>

#include "esphome/components/xr806/standby_coordinator_state.h"
#include "esphome/components/xr806/lowpower_policy.h"

int main() {
  using esphome::xr806::XR806StandbyCoordinatorState;
  using esphome::xr806::XR806LowPowerPolicy;

  XR806LowPowerPolicy policy;
  const auto initial = policy.generation();
  assert(!policy.standby_requested());
  assert(!policy.can_enter_standby());

  policy.set_uart_idle(true);
  assert(policy.generation() == initial + 1);
  policy.set_uart_idle(true);
  assert(policy.generation() == initial + 1);
  policy.set_uart_idle(false);
  assert(policy.generation() == initial + 2);

  policy.require_external_mcu(true);
  assert((policy.blockers() & XR806LowPowerPolicy::MCU_UNSYNCED) != 0);
  policy.set_mcu_synchronized(true);
  assert((policy.blockers() & XR806LowPowerPolicy::MCU_UNSYNCED) == 0);
  policy.require_external_mcu(false);
  assert((policy.blockers() & XR806LowPowerPolicy::MCU_UNSYNCED) == 0);
  policy.request_standby(true);
  assert(policy.standby_requested());
  const auto requested_epoch = policy.generation();
  policy.request_standby(true);
  assert(policy.generation() == requested_epoch);
  policy.request_standby(false);
  assert(policy.generation() == requested_epoch + 1);

  XR806StandbyCoordinatorState state;
  state.configure(250, 5000);
  state.boot(100);
  assert(!state.due(200, 4, false, true));  // Never autonomously suspend.
  assert(!state.due(300, 4, true, false));  // One busy owner is enough.
  assert(!state.due(400, 4, true, true));   // Begin stable quiet interval.
  assert(!state.due(649, 4, true, true));
  assert(!state.due(900, 5, true, true));  // Any vote transition resets quiet.
  assert(!state.due(1149, 5, true, true));
  assert(!state.due(1500, 5, true, true));  // Still inside initial awake guard.
  assert(state.due(5100, 5, true, true));   // After boot awake guard and quiet.

  state.finished(5200, true);
  assert(state.failures() == 0);
  assert(!state.due(5300, 5, true, true));
  assert(!state.due(6000, 5, true, true));
  assert(state.due(10200, 5, true, true));  // Resume cooldown enforced.

  assert(XR806StandbyCoordinatorState::expected_wake(0, 1u << 16, -1));
  assert(XR806StandbyCoordinatorState::expected_wake(0, 1u << 10, 10));
  assert(!XR806StandbyCoordinatorState::expected_wake(0, 1u << 10, -1));
  assert(!XR806StandbyCoordinatorState::expected_wake(-1, 1u << 16, 10));
  assert(!XR806StandbyCoordinatorState::expected_wake(0, 1u << 27, 10));
  assert(!XR806StandbyCoordinatorState::expected_wake(0, (1u << 16) | (1u << 27), 10));
  assert(!XR806StandbyCoordinatorState::expected_wake(0, (1u << 16) | (1u << 28), 10));
  assert(!XR806StandbyCoordinatorState::expected_wake(0, 0, 10));

  state.finished(10500, false);
  assert(state.failures() == 1);
  state.finished(10600, false);
  assert(state.failures() == 2);
  state.finished(10700, false);
  assert(state.failures() == 3 && state.disabled());
  assert(!state.due(90000, 6, true, true));

  // millis() wraps safely across boot, quiet interval and wake cooldown.
  XR806StandbyCoordinatorState wrapped;
  wrapped.configure(200, 1000);
  wrapped.boot(0xfffffff0u);
  assert(!wrapped.due(0xfffffffeu, 1, true, true));
  assert(!wrapped.due(0x20u, 1, true, true));
  assert(wrapped.due(0x400u, 1, true, true));
  wrapped.finished(0x450u, true);
  assert(!wrapped.due(0x500u, 1, true, true));
  assert(wrapped.due(0x900u, 1, true, true));

  return 0;
}
