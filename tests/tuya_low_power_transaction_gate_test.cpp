// Standalone build: g++ -std=c++17 -Wall -Wextra -Werror -I. tests/tuya_low_power_transaction_gate_test.cpp -o /tmp/txgate && /tmp/txgate
#include <cassert>
#include <cstdint>
#include "esphome/components/tuya/tuya_low_power_transaction_gate.h"
#include "esphome/components/tuya/tuya_low_power_v0.h"

int main() {
  using esphome::tuya::TuyaLowPowerTransactionGate;
  using esphome::tuya::TuyaLowPowerV0Session;
  using Gate = TuyaLowPowerTransactionGate;
  Gate g;
  assert(g.can_send(0));
  assert(g.begin(0x09, 10));
  assert(!g.begin(0x02, 11));  // Cannot overwrite a pending CMD09 with status command.
  assert(g.observe(0x05, 9, 20) == Gate::Reply::WRONG_TYPE);
  assert(g.observe(0x02, 0, 21) == Gate::Reply::WRONG_TYPE);
  assert(g.observe(0x09, 1, 22) == Gate::Reply::WRONG_TYPE);
  assert(g.pending());
  assert(g.observe(0x09, 0, 23) == Gate::Reply::MATCH);
  assert(!g.pending());
  assert(g.begin(0x02, 24));
  assert(g.observe(0x09, 0, 25) == Gate::Reply::WRONG_TYPE);
  assert(g.observe(0x02, 0, 26) == Gate::Reply::MATCH);
  assert(g.begin(0x01, 27));
  assert(g.observe(0x01, 0, 28) == Gate::Reply::WRONG_TYPE);
  assert(g.observe(0x01, 56, 29) == Gate::Reply::MATCH);

  // A timeout quarantines late ACKs and requires a new status epoch.
  assert(g.begin(0x09, 100));
  assert(g.expire(1599) == Gate::Timeout::NONE);
  assert(g.expire(1600) == Gate::Timeout::EXPIRED);
  const auto failed_epoch = g.epoch();
  assert(!g.can_send(1601));
  assert(g.observe(0x09, 0, 1602) == Gate::Reply::STALE);
  assert(g.can_send(1900));
  assert(g.begin(0x02, 1900));
  assert(g.epoch() > failed_epoch);
  assert(g.observe(0x02, 0, 1901) == Gate::Reply::MATCH);

  // Reset invalidates an in-flight transaction, even if the UART is healthy.
  assert(g.begin(0x09, 2000));
  g.reset(2001);
  assert(!g.pending() && g.quarantined(2001));
  assert(g.observe(0x09, 0, 2002) == Gate::Reply::STALE);
  assert(g.can_send(2301));

  // Millis rollover must not accidentally release the quarantine early.
  g.reset(0xFFFFFFF0u);
  assert(g.quarantined(0x00000010u));
  assert(!g.can_send(0x00000010u));
  assert(g.can_send(static_cast<uint32_t>(0xFFFFFFF0u + Gate::REPLY_QUARANTINE_MS)));

  // ACK and the HC32's delayed state report are separate from the wire gate.
  TuyaLowPowerV0Session status;
  status.product_received();
  status.begin_full_status(1000);
  for (auto id : TuyaLowPowerV0Session::FULL_STATUS_DPS) {
    const uint8_t val = 0;
    status.observe_dp(id, 1, &val, 1);
  }
  assert(status.ready());
  const uint8_t wanted[4] = {0, 0, 0, 240};
  status.begin_write(3, 2, wanted, 4, 1500);
  assert(g.begin(0x09, 2400));
  assert(g.observe(0x09, 0, 2405) == Gate::Reply::MATCH);
  status.write_ack();
  assert(status.busy() && !status.ready());  // CMD38 must be busy here.
  status.observe_dp(3, 2, wanted, 4);
  assert(status.ready());
  return 0;
}
