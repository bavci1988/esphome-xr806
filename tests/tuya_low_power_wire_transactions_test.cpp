#include <cassert>
#include <cstdint>
#include "esphome/components/tuya/tuya_low_power_wire_transactions.h"

int main() {
  using State = esphome::tuya::TuyaLowPowerWireTransactions;
  State t;
  assert(t.idle());
  assert(!t.accepts(0x09, 0));
  assert(!t.begin(0x06, 10));  // unsolicited direct time reply is no queued transaction
  assert(t.begin(0x09, 100));
  assert(!t.begin(0x02, 120));   // no overlapping queued network/write transaction
  assert(!t.complete(0x05, 6));  // report cannot satisfy write ACK
  assert(!t.complete(0x09, 2));  // malformed ACK
  assert(!t.complete(0x02, 0));  // wrong command code
  assert(t.active() && !t.expire(1599));
  assert(!t.overdue(1599));
  assert(t.overdue(1600));  // Frame delivered at deadline before queue sweep.
  assert(t.accepts(0x09, 0));  // Type matches, but RX MUST reject overdue ACK.
  assert(t.expire(1600));
  assert(t.faulted() && !t.complete(0x09, 0));
  t.recovered();  // Explicitly model complete identity/status resync.
  assert(t.begin(0x09, 1700));
  assert(t.complete(0x09, 0));
  assert(t.idle());

  assert(t.begin(0x02, 2000));
  assert(!t.complete(0x09, 0));  // late CMD09 ACK may not clear CMD02
  assert(t.complete(0x02, 0));
  assert(t.begin(0x01, 2500));
  assert(!t.complete(0x01, 0));
  assert(t.complete(0x01, 51));
  assert(t.begin(0x09, 3000));
  assert(!t.expire(4499));
  assert(t.expire(4500));
  assert(t.faulted() && !t.active());
  assert(!t.begin(0x09, 5000));  // new write cannot consume late old ACK
  assert(!t.complete(0x09, 0));
  t.recovered();  // caller must validate MCU product AND 17-DP state separately
  assert(t.begin(0x09, 6000));
  t.transport_fault();
  assert(!t.begin(0x02, 7000));
  t.recovered();
  assert(t.idle());
  assert(t.begin(0x01, 0xfffffff0u));
  assert(!t.overdue(0x20u));
  assert(!t.expire(0x20u));
  assert(t.overdue(static_cast<uint32_t>(0xfffffff0u + 1500u)));
  assert(t.expire(static_cast<uint32_t>(0xfffffff0u + 1500u)));
  assert(t.faulted());
  return 0;
}
