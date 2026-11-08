#include <cassert>
#include <cstdint>
#include "esphome/components/xr806/standby_trial_state.h"
#include "esphome/components/xr806/lowpower_policy.h"

int main() {
  using esphome::xr806::XR806StandbyTrialState;
  using esphome::xr806::XR806LowPowerPolicy;
  XR806StandbyTrialState state;
  assert(!state.claim(10000, 5000));
  state.arm(0);
  assert(!state.claim(4999, 5000));
  assert(state.claim(5000, 5000));
  assert(state.attempted());
  assert(!state.claim(999999, 5000));
  state.arm(0xfffffff0u);
  assert(!state.claim(0x10u, 100));
  assert(state.claim(0xfffffff0u + 100u, 100));
  assert(XR806StandbyTrialState::expected_wake_event(1u << 16, -1));
  assert(XR806StandbyTrialState::expected_wake_event(1u << 10, 10));
  assert(!XR806StandbyTrialState::expected_wake_event(1u << 10, -1));
  assert(!XR806StandbyTrialState::expected_wake_event(1u << 27, 10)); // software PM abort
  assert(!XR806StandbyTrialState::expected_wake_event(0, 10));

  XR806LowPowerPolicy p;
  p.request_standby(true);
  p.set_uart_idle(true);
  p.set_network_idle(true);
  p.set_ota_idle(true);
  p.set_api_idle(true);
  p.set_mdns_idle(true);
  p.set_scheduler_idle(true);
  assert(!p.can_enter_standby()); // Wake source not armed.
  p.set_wake_source_ready(true);
  assert(p.can_enter_standby());
  p.require_external_mcu(true);
  assert(!p.can_enter_standby()); // The optional MCU gate is respected.
  p.set_mcu_synchronized(true);
  assert(p.can_enter_standby());
  p.request_standby(false);
  assert(!p.can_enter_standby()); // A one-shot does not repeat.
  return 0;
}
