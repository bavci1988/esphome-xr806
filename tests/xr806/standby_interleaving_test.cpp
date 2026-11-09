// Stage 7d: deterministic PM/MCU interleaving model using the SAME
// dependency-free state machines and owner policy as the production code.
// Tests no real lwIP socket, PM driver, GPIO voltage or OTA flash backend.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <thread>

#include "esphome/components/tuya/tuya_low_power_v0.h"
#include "esphome/components/tuya/tuya_low_power_wire_transactions.h"
#include "esphome/components/tuya/tuya_low_power_standby_state.h"
#include "esphome/components/xr806/lowpower_policy.h"
#include "esphome/components/xr806/standby_async_epoch.h"
#include "esphome/components/xr806/standby_coordinator_state.h"

namespace {
using Policy = esphome::xr806::XR806LowPowerPolicy;
using Wire = esphome::tuya::TuyaLowPowerWireTransactions;
using Session = esphome::tuya::TuyaLowPowerV0Session;
using Standby = esphome::tuya::TuyaLowPowerStandbyState;

void release_all_owner_votes(Policy &p) {
  p.require_external_mcu(true);
  p.set_uart_idle(true);
  p.set_mcu_synchronized(true);
  p.set_network_idle(true);
  p.set_api_idle(true);
  p.set_mdns_idle(true);
  p.set_ota_idle(true);
  p.set_scheduler_idle(true);
  p.set_wake_source_ready(true);
  p.request_standby(true);
}

void complete_full_status(Session &s, uint32_t now) {
  s.begin_full_status(now);
  for (uint8_t dp : Session::FULL_STATUS_DPS) {
    s.observe_dp(dp, 1, &dp, 1);
  }
  assert(s.ready());
}

void test_uart_ack_and_reports() {
  Session mcu;
  Wire wire;
  mcu.product_received();
  complete_full_status(mcu, 100);

  const uint8_t value[] = {0, 0, 0, 240};
  assert(wire.begin(0x09, 1000));
  mcu.begin_write(3, 2, value, sizeof(value), 1000);
  assert(mcu.busy() && !mcu.ready());
  assert(!wire.begin(0x02, 1001));  // New CMD02 cannot overlap CMD09.
  assert(!wire.complete(0x05, 6));  // A CMD05 is NOT an ACK.
  assert(wire.complete(0x09, 0));
  mcu.write_ack();
  assert(mcu.busy() && !mcu.ready());  // Write adopted only on matching report.
  const uint8_t rejected[] = {0, 0, 0, 210};
  mcu.observe_dp(3, 2, rejected, sizeof(rejected));
  assert(!mcu.ready());
  mcu.observe_dp(3, 2, value, sizeof(value));
  assert(mcu.ready() && wire.idle());

  assert(wire.begin(0x02, 2000));
  mcu.begin_full_status(2000);
  assert(!wire.complete(0x09, 0));  // Late ACK from preceding write ignored.
  assert(wire.complete(0x02, 0));
  assert(mcu.busy());  // Network ACK does not close the 17-DP burst.
  for (size_t i = 0; i + 1 < Session::FULL_STATUS_DPS.size(); ++i) {
    uint8_t dp = Session::FULL_STATUS_DPS[i];
    mcu.observe_dp(dp, 1, &dp, 1);
  }
  assert(mcu.busy());
  uint8_t last = Session::FULL_STATUS_DPS.back();
  mcu.observe_dp(last, 1, &last, 1);
  assert(mcu.ready());

  assert(wire.begin(0x09, 3000));
  assert(wire.expire(4500));
  assert(wire.faulted() && !wire.active());
  assert(!wire.complete(0x09, 0));  // Ambiguous late ACK.
  assert(!wire.begin(0x09, 4501));  // Quarantine until identity/full status.
  wire.recovered();
  assert(wire.idle());
}

void test_wake_and_owner_interleavings() {
  constexpr std::array<uint32_t, 5> owner_bits{Policy::UART_BUSY, Policy::NETWORK_BUSY, Policy::API_BUSY,
                                               Policy::MDNS_BUSY, Policy::OTA_BUSY};
  for (uint32_t bit : owner_bits) {
    Policy policy;
    release_all_owner_votes(policy);
    assert(policy.can_enter_standby());
    const uint32_t epoch = policy.generation();
    assert(policy.begin_commit(epoch));

    // Cross-task owner activity lands after final admission began.
    switch (bit) {
      case Policy::UART_BUSY:
        policy.set_uart_idle(false);
        break;
      case Policy::NETWORK_BUSY:
        policy.set_network_idle(false);
        break;
      case Policy::API_BUSY:
        policy.set_api_idle(false);
        break;
      case Policy::MDNS_BUSY:
        policy.set_mdns_idle(false);
        break;
      case Policy::OTA_BUSY:
        policy.set_ota_idle(false);
        break;
      default:
        assert(false);
        break;
    }
    assert(!policy.commit_valid(epoch));
    assert((policy.blockers() & bit) != 0);
    policy.end_commit();
  }

  // Even a repeated idle vote racing the commit must not erase a genuine
  // abort: non-transitioning callbacks can still represent new work.
  Policy stable;
  release_all_owner_votes(stable);
  const uint32_t epoch = stable.generation();
  assert(stable.begin_commit(epoch));
  stable.set_network_idle(true);  // Repeated vote during commit.
  assert(!stable.commit_valid(epoch));
  stable.end_commit();
  assert(stable.can_enter_standby());

  // All five votes can return idle, yet the OLD admission claim is invalid.
  Policy reopened;
  release_all_owner_votes(reopened);
  const uint32_t first_epoch = reopened.generation();
  assert(reopened.begin_commit(first_epoch));
  reopened.set_api_idle(false);
  reopened.set_api_idle(true);
  assert(reopened.blockers() == 0);
  assert(!reopened.commit_valid(first_epoch));
  reopened.end_commit();
  assert(reopened.begin_commit(reopened.generation()));
  reopened.end_commit();

  // A concurrent producer aborting a live PM admission is visible even
  // if it asserts busy and releases busy before the coordinator wakes.
  Policy concurrent;
  release_all_owner_votes(concurrent);
  const uint32_t concurrent_epoch = concurrent.generation();
  assert(concurrent.begin_commit(concurrent_epoch));
  std::thread producer([&concurrent]() {
    concurrent.set_uart_idle(false);
    concurrent.set_uart_idle(true);
  });
  producer.join();
  assert(concurrent.blockers() == 0);
  assert(!concurrent.commit_valid(concurrent_epoch));
  concurrent.end_commit();

  // CMD38 cannot consent while MCU status or a write remains pending.
  Session mcu;
  Standby handshake;
  mcu.product_received();
  mcu.begin_full_status(0);
  assert(!handshake.accept_request(true, mcu.ready(), true, true));
  complete_full_status(mcu, 100);
  assert(handshake.accept_request(true, mcu.ready(), true, true));
  assert(handshake.needs_notification());
  handshake.notification_sent();
  assert(!handshake.cancelled());  // Intentional first TX-drain deferral.
  assert(handshake.ready_for_pm());
  // A resumed timer-only wake cannot fake the MCU GPIO path.
  assert(handshake.resumed(true, false) == Standby::ResumeAction::REQUIRE_RESYNC);
  assert(!handshake.accept_request(true, true, true, true));
  handshake.reset_on_mcu_boot();
  assert(handshake.accept_request(true, true, true, true));
  assert(!handshake.force_cancel());  // No CMD12/01 yet: no compensation.
}

void test_exhaustive_owner_permutations() {
  std::array<uint8_t, 5> events{{0, 1, 2, 3, 4}};
  size_t permutations = 0;
  do {
    Policy p;
    release_all_owner_votes(p);
    assert(p.can_enter_standby());
    const uint32_t observed = p.generation();
    assert(p.begin_commit(observed));
    for (uint8_t event : events) {
      switch (event) {
        case 0:
          p.set_uart_idle(false);
          p.set_uart_idle(true);
          break;
        case 1:
          p.set_network_idle(false);
          p.set_network_idle(true);
          break;
        case 2:
          p.set_api_idle(false);
          p.set_api_idle(true);
          break;
        case 3:
          p.set_mdns_idle(false);
          p.set_mdns_idle(true);
          break;
        case 4:
          p.set_ota_idle(false);
          p.set_ota_idle(true);
          break;
        default:
          assert(false);
          break;
      }
      assert(!p.commit_valid(observed));  // Releasing work cannot resurrect the old claim.
    }
    p.end_commit();
    assert(p.blockers() == 0);
    assert(p.begin_commit(p.generation()));
    p.end_commit();
    ++permutations;
  } while (std::next_permutation(events.begin(), events.end()));
  assert(permutations == 120);
}

void test_async_lwip_suspend_epochs() {
  using Gate = esphome::xr806::XR806StandbyAsyncEpoch;
  Gate gate;
  assert(!gate.requested() && gate.epoch() == 0);
  assert(!gate.request(false));
  assert(gate.request(true));
  const uint32_t first = gate.epoch();
  assert(gate.current(first));
  assert(!gate.request(true));  // Duplicate callback cannot create a new epoch.
  assert(gate.current(first));
  // A late lwIP callback from a cancelled suspend must be rejected.
  assert(gate.request(false));
  assert(!gate.current(first));
  assert(gate.request(true));
  assert(!gate.current(first));  // ABA: true->false->true is NOT the old request.
  const uint32_t second = gate.epoch();
  assert(gate.current(second));
  // Completing an old callback is never authorization to release MDNS_BUSY.
  Policy owner;
  owner.set_mdns_idle(false);
  if (gate.current(first)) {
    owner.set_mdns_idle(true);
  }
  assert((owner.blockers() & Policy::MDNS_BUSY) != 0);
  if (gate.current(second)) {
    owner.set_mdns_idle(true);
  }
  assert((owner.blockers() & Policy::MDNS_BUSY) == 0);
  gate.request(false);
  owner.set_mdns_idle(false);
  assert(!gate.current(second));
  assert((owner.blockers() & Policy::MDNS_BUSY) != 0);

  // A producer on another task can cancel a pending epoch. Once joined,
  // no stale completion is allowed to authorize the old epoch.
  gate.request(true);
  const uint32_t old = gate.epoch();
  std::thread cancel([&gate]() {
    gate.request(false);
    gate.request(true);
  });
  cancel.join();
  assert(!gate.current(old));
}

void test_quiet_window_and_expiration() {
  using State = esphome::xr806::XR806StandbyCoordinatorState;
  State state;
  state.configure(250, 5000);
  state.boot(0);
  assert(!state.due(5000, 1, true, true));
  assert(!state.due(5249, 1, true, true));
  assert(state.due(5250, 1, true, true));
  assert(!state.due(5251, 2, true, true));  // Owner changed during prepare.
  assert(!state.due(5499, 2, true, true));
  assert(state.due(5501, 2, true, true));
  assert(!state.due(6000, 2, true, false));  // OTA/API veto.
  assert(!state.due(6250, 2, true, true));
  assert(state.due(6501, 2, true, true));
  state.finished(6600, false);  // PM error/unknown wake.
  assert(!state.due(6601, 2, false, true));
  assert(!State::expected_wake(0, State::WAKE_SOFTWARE, 10));
  assert(!State::expected_wake(0, 0, 10));
}
}  // namespace

int main() {
  test_uart_ack_and_reports();
  test_wake_and_owner_interleavings();
  test_exhaustive_owner_permutations();
  test_async_lwip_suspend_epochs();
  test_quiet_window_and_expiration();
  return 0;
}
