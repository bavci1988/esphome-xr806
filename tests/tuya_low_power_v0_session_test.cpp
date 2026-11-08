// Standalone host test from repository root:
// g++ -std=c++17 -Wall -Wextra -Werror -I. tests/tuya_low_power_v0_session_test.cpp -o /tmp/tuya_lp_test
#include <cassert>
#include <cstddef>
#include <cstdint>
#include "esphome/components/tuya/tuya_low_power_v0.h"
#include "esphome/components/tuya/tuya_low_power_autobaud.h"
#include <vector>
#include <string>

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
  // Autobaud: begin at user-configured speed, alternate every 500 ms.
  using esphome::tuya::TuyaLowPowerAutoBaud;
  TuyaLowPowerAutoBaud scan;
  scan.start(115200);
  assert(scan.searching() && scan.probe_due(0) && scan.baud() == 115200);
  scan.probe_sent(10);
  assert(!scan.probe_due(509));

  auto frame = [](uint8_t command, const std::string &payload) {
    std::vector<uint8_t> b = {0x55, 0xAA, 0x00, command,
        static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size())};
    b.insert(b.end(), payload.begin(), payload.end());
    uint8_t sum = 0;
    for (uint8_t x : b)
      sum += x;
    b.push_back(sum);
    return b;
  };
  const std::string product = R"({"p":"ihedujbuli6hsx9b","v":"1.3.1","wk":20,"mwk":28,"sm":1})";
  auto valid = frame(0x01, product);
  auto wrong_command = frame(0x05, product);
  auto bad_checksum = valid;
  bad_checksum.back() ^= 0x01;
  assert(!scan.accept_frame(wrong_command.data(), wrong_command.size(), 100));
  assert(!scan.accept_frame(bad_checksum.data(), bad_checksum.size(), 100));
  assert(scan.searching());
  assert(scan.probe_due(510) && scan.baud() == 9600);
  scan.probe_sent(511);
  assert(!scan.accept_frame(valid.data(), valid.size(), 1011));  // Expired.
  assert(scan.probe_due(1011) && scan.baud() == 115200);
  scan.probe_sent(1012);
  assert(scan.accept_frame(valid.data(), valid.size(), 1013));
  assert(scan.locked() && !scan.searching() && !scan.probe_due(2000));
  assert(!scan.accept_frame(valid.data(), valid.size(), 2000));  // No repeat.

  // Default to 9600 unless configured to one of the supported candidates.
  scan.start(57600);
  assert(scan.baud() == 9600 && scan.probe_due(0));
  scan.probe_sent(0xFFFFFFF0u);
  assert(!scan.probe_due(0x00000010u));  // millis() wraparound safe.
  assert(scan.probe_due(static_cast<uint32_t>(0xFFFFFFF0u + 500u)));
  assert(scan.baud() == 115200);
  return 0;
}
