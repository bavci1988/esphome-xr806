// Standalone host test from repository root:
// g++ -std=c++17 -Wall -Wextra -Werror -I. tests/tuya_low_power_v0_session_test.cpp -o /tmp/tuya_lp_test
#include <cassert>
#include <cstddef>
#include <cstdint>
#include "esphome/components/tuya/tuya_low_power_v0.h"

int main() {
  using esphome::tuya::TuyaLowPowerV0Session;
  TuyaLowPowerV0Session state;
  assert(!state.ready());
  state.product_received();
  state.begin_full_status(100);

  for (size_t i = 0; i + 1 < TuyaLowPowerV0Session::FULL_STATUS_DPS.size(); ++i) {
    uint8_t id = TuyaLowPowerV0Session::FULL_STATUS_DPS[i];
    state.observe_dp(id, 1, &id, 1);
  }
  assert(state.busy() && !state.ready());
  uint8_t last = TuyaLowPowerV0Session::FULL_STATUS_DPS.back();
  state.observe_dp(last, 1, &last, 1);
  assert(state.ready());

  const uint8_t wanted[] = {0, 0, 0, 240};
  const uint8_t wrong[] = {0, 0, 0, 210};
  state.begin_write(3, 2, wanted, sizeof(wanted), 200);
  state.write_ack();
  assert(!state.ready());  // Empty CMD09 ACK is not state confirmation.
  state.observe_dp(3, 2, wrong, sizeof(wrong));
  assert(!state.ready());
  state.observe_dp(3, 2, wanted, sizeof(wanted));
  assert(state.ready());

  state.begin_write(3, 2, wanted, sizeof(wanted), 300);
  state.observe_dp(3, 2, wanted, sizeof(wanted));
  assert(!state.ready());  // Report may precede the ACK.
  state.write_ack();
  assert(state.ready());

  state.begin_write(3, 2, wanted, sizeof(wanted), 400);
  assert(!state.expire(401, 12000));
  assert(state.expire(13000, 12000));
  assert(!state.ready());  // Failure locks sleep.
  state.begin_full_status(14000);
  for (uint8_t id : TuyaLowPowerV0Session::FULL_STATUS_DPS)
    state.observe_dp(id, 1, &id, 1);
  assert(state.ready());  // Full re-synchronization releases the fault.
  state.reset_status();
  assert(!state.ready());
  return 0;
}
