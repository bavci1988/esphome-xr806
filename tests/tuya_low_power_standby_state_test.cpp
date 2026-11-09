#include <cassert>
#include "esphome/components/tuya/tuya_low_power_standby_state.h"

int main() {
  using esphome::tuya::TuyaLowPowerStandbyState;
  using State = TuyaLowPowerStandbyState;
  using Action = State::ResumeAction;

  State s;
  assert(!s.accept_request(false, true, true, true));  // sm=0 => hibernation, not retained standby
  assert(!s.accept_request(true, false, true, true));  // unconfirmed DP write/status
  assert(!s.accept_request(true, true, false, true));  // UART transport fault
  assert(!s.accept_request(true, true, true, false));  // WLAN/API/OTA veto
  assert(s.accept_request(true, true, true, true));
  assert(!s.accept_request(true, true, true, true));  // repeated CMD38 cannot restart request
  assert(s.needs_notification());
  assert(!s.ready_for_pm());
  assert(!s.cancelled());  // request without CMD12 needs no compensation
  assert(!s.requested());

  assert(s.accept_request(true, true, true, true));
  s.notification_sent();
  assert(!s.ready_for_pm());
  assert(!s.cancelled());  // intentional retry after physical UART drain
  assert(s.ready_for_pm());
  assert(s.notified());
  assert(s.cancelled());  // veto after CMD12: must send compensating CMD12/00
  assert(!s.requested() && !s.notified());

  assert(s.accept_request(true, true, true, true));
  s.notification_sent();
  (void) s.cancelled();
  assert(s.resumed(true, true) == Action::SEND_CMD12_WAKE);
  assert(!s.recovery_required());
  assert(s.accept_request(true, true, true, true));  // new handshake after GPIO wake

  s.notification_sent();
  (void) s.cancelled();
  assert(s.resumed(true, false) == Action::REQUIRE_RESYNC);  // timer alone never fakes GPIO wake
  assert(s.recovery_required());
  assert(!s.accept_request(true, true, true, true));
  s.reset_on_mcu_boot();
  assert(!s.recovery_required());
  assert(s.accept_request(true, true, true, true));

  s.notification_sent();
  (void) s.cancelled();
  assert(s.resumed(false, true) == Action::REQUIRE_RESYNC);
  s.reset_on_mcu_boot();
  assert(s.accept_request(true, true, true, true));
  s.transport_fault();
  assert(s.recovery_required() && !s.requested());
  assert(!s.accept_request(true, true, true, true));

  State empty;
  assert(empty.resumed(true, true) == Action::NONE);
  return 0;
}
