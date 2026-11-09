#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>
#include "esphome/components/tuya/tuya_low_power_v0.h"
#include "esphome/components/tuya/tuya_low_power_autobaud.h"
#include "esphome/components/tuya/tuya_low_power_product.h"
#include <vector>
#include <string>

namespace esphome::tuya::testing {

TEST(TuyaLowPowerV0Session, FullStatusAndWrites) {
  using esphome::tuya::TuyaLowPowerV0Session;
  TuyaLowPowerV0Session state;
  EXPECT_FALSE(state.ready());
  state.product_received();
  state.begin_full_status(100);

  for (size_t i = 0; i + 1 < TuyaLowPowerV0Session::FULL_STATUS_DPS.size(); ++i) {
    uint8_t id = TuyaLowPowerV0Session::FULL_STATUS_DPS[i];
    state.observe_dp(id, 1, &id, 1);
  }
  EXPECT_TRUE(state.busy() && !state.ready());
  uint8_t last = TuyaLowPowerV0Session::FULL_STATUS_DPS.back();
  state.observe_dp(last, 1, &last, 1);
  EXPECT_TRUE(state.ready());

  const uint8_t wanted[] = {0, 0, 0, 240};
  const uint8_t wrong[] = {0, 0, 0, 210};
  state.begin_write(3, 2, wanted, sizeof(wanted), 200);
  state.write_ack();
  EXPECT_FALSE(state.ready());  // Empty CMD09 ACK is not state confirmation.
  state.observe_dp(3, 2, wrong, sizeof(wrong));
  EXPECT_FALSE(state.ready());
  state.observe_dp(3, 2, wanted, sizeof(wanted));
  EXPECT_TRUE(state.ready());

  state.begin_write(3, 2, wanted, sizeof(wanted), 300);
  state.observe_dp(3, 2, wanted, sizeof(wanted));
  EXPECT_FALSE(state.ready());  // Report may precede the ACK.
  state.write_ack();
  EXPECT_TRUE(state.ready());

  state.begin_write(3, 2, wanted, sizeof(wanted), 400);
  EXPECT_FALSE(state.expire(401, 12000));
  EXPECT_TRUE(state.expire(13000, 12000));
  EXPECT_FALSE(state.ready());  // Failure locks sleep.
  state.begin_full_status(14000);
  for (uint8_t id : TuyaLowPowerV0Session::FULL_STATUS_DPS)
    state.observe_dp(id, 1, &id, 1);
  EXPECT_TRUE(state.ready());  // Full re-synchronization releases the fault.
  state.reset_status();
  EXPECT_FALSE(state.ready());
  // Autobaud: begin at user-configured speed, alternate every 500 ms.
  using esphome::tuya::TuyaLowPowerAutoBaud;
  TuyaLowPowerAutoBaud scan;
  scan.start(115200);
  EXPECT_TRUE(scan.searching() && scan.probe_due(0) && scan.baud() == 115200);
  scan.probe_sent(10);
  EXPECT_FALSE(scan.probe_due(509));

  auto frame = [](uint8_t command, const std::string &payload) {
    std::vector<uint8_t> b = {
        0x55, 0xAA, 0x00, command, static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size())};
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
  EXPECT_FALSE(scan.accept_frame(wrong_command.data(), wrong_command.size(), 100));
  EXPECT_FALSE(scan.accept_frame(bad_checksum.data(), bad_checksum.size(), 100));
  EXPECT_TRUE(scan.searching());
  EXPECT_TRUE(scan.probe_due(510) && scan.baud() == 9600);
  scan.probe_sent(511);
  EXPECT_FALSE(scan.accept_frame(valid.data(), valid.size(), 1011));  // Expired.
  EXPECT_TRUE(scan.probe_due(1011) && scan.baud() == 115200);
  scan.probe_sent(1012);
  EXPECT_TRUE(scan.accept_frame(valid.data(), valid.size(), 1013));
  EXPECT_TRUE(scan.locked() && !scan.searching() && !scan.probe_due(2000));
  EXPECT_FALSE(scan.accept_frame(valid.data(), valid.size(), 2000));  // No repeat.

  // Default to 9600 unless configured to one of the supported candidates.
  scan.start(57600);
  EXPECT_TRUE(scan.baud() == 9600 && scan.probe_due(0));
  scan.probe_sent(0xFFFFFFF0u);
  EXPECT_FALSE(scan.probe_due(0x00000010u));  // millis() wraparound safe.
  EXPECT_TRUE(scan.probe_due(static_cast<uint32_t>(0xFFFFFFF0u + 500u)));
  EXPECT_TRUE(scan.baud() == 115200);
  // The refresh request DP106 never echoes DP106; it returns exactly
  // eight other DPs. An empty CMD09 ACK alone must not release the write.
  state.begin_full_status(15000);
  for (uint8_t id : TuyaLowPowerV0Session::FULL_STATUS_DPS)
    state.observe_dp(id, 1, &id, 1);
  EXPECT_TRUE(state.ready());
  const uint8_t one = 1;
  state.begin_write(106, 1, &one, 1, 16000);
  state.write_ack();
  EXPECT_TRUE(state.refresh_pending() && !state.ready());
  for (size_t i = 0; i + 1 < TuyaLowPowerV0Session::REFRESH_DPS.size(); ++i) {
    uint8_t dp = TuyaLowPowerV0Session::REFRESH_DPS[i];
    state.observe_dp(dp, 1, &dp, 1);
  }
  EXPECT_TRUE(state.write_pending());
  uint8_t end_dp = TuyaLowPowerV0Session::REFRESH_DPS.back();
  state.observe_dp(end_dp, 1, &end_dp, 1);
  EXPECT_TRUE(state.ready());
  EXPECT_TRUE(state.last_write_result() == TuyaLowPowerV0Session::WriteResult::CONFIRMED);

  // Report-before-ACK also works for the eight-DP refresh.
  state.begin_write(106, 1, &one, 1, 17000);
  for (uint8_t dp : TuyaLowPowerV0Session::REFRESH_DPS)
    state.observe_dp(dp, 1, &dp, 1);
  EXPECT_TRUE(state.write_pending());
  state.write_ack();
  EXPECT_TRUE(state.ready());

  // Missing reports are a fault, not an implicit ACK success.
  state.begin_write(106, 1, &one, 1, 18000);
  state.write_ack();
  state.observe_dp(1, 1, &one, 1);
  EXPECT_TRUE(state.expire(30000));
  EXPECT_TRUE(state.last_write_result() == TuyaLowPowerV0Session::WriteResult::TIMED_OUT);
  EXPECT_FALSE(state.ready());
  state.begin_full_status(31000);
  for (uint8_t dp : TuyaLowPowerV0Session::FULL_STATUS_DPS)
    state.observe_dp(dp, 1, &dp, 1);
  EXPECT_TRUE(state.ready());

  // The verified TRV16 product controls wake pins and standby-vs-hibernation.
  using esphome::tuya::TuyaLowPowerProductInfo;
  TuyaLowPowerProductInfo info;
  const std::string product_json = R"({"p":"ihedujbuli6hsx9b","v":"1.3.1","wk":20,"mwk":28,"sm":1})";
  EXPECT_TRUE(info.parse(reinterpret_cast<const uint8_t *>(product_json.data()), product_json.size()));
  EXPECT_TRUE(info.is_verified_trv16());
  EXPECT_TRUE(info.wake_gpio == 20 && info.mcu_wake_gpio == 28 && info.sleep_mode == 1);
  const std::string hib_json = R"({"p":"ihedujbuli6hsx9b","v":"1.3.1","wk":20,"mwk":28,"sm":0})";
  EXPECT_TRUE(info.parse(reinterpret_cast<const uint8_t *>(hib_json.data()), hib_json.size()));
  EXPECT_TRUE(info.is_verified_trv16() && info.sleep_mode == 0);
  const std::string wrong_pid = R"({"p":"another_product","v":"1.3.1","wk":20,"mwk":28,"sm":1})";
  EXPECT_TRUE(info.parse(reinterpret_cast<const uint8_t *>(wrong_pid.data()), wrong_pid.size()));
  EXPECT_FALSE(info.is_verified_trv16());
  const std::string missing_pin = R"({"p":"ihedujbuli6hsx9b","v":"1.3.1","wk":20,"sm":1})";
  EXPECT_FALSE(info.parse(reinterpret_cast<const uint8_t *>(missing_pin.data()), missing_pin.size()));
  EXPECT_FALSE(info.parsed);
  const std::string missing_p = R"({"v":"1.3.1","wk":20,"mwk":28,"sm":1})";
  const std::string missing_v = R"({"p":"ihedujbuli6hsx9b","wk":20,"mwk":28,"sm":1})";
  const std::string bad_sm = R"({"p":"ihedujbuli6hsx9b","v":"1.3.1","wk":20,"mwk":28,"sm":2})";
  const std::string typed_p = R"({"p":42,"v":"1.3.1","wk":20,"mwk":28,"sm":1})";
  EXPECT_FALSE(info.parse(reinterpret_cast<const uint8_t *>(missing_p.data()), missing_p.size()));
  EXPECT_FALSE(info.parse(reinterpret_cast<const uint8_t *>(missing_v.data()), missing_v.size()));
  EXPECT_FALSE(info.parse(reinterpret_cast<const uint8_t *>(bad_sm.data()), bad_sm.size()));
  EXPECT_FALSE(info.parse(reinterpret_cast<const uint8_t *>(typed_p.data()), typed_p.size()));
  EXPECT_FALSE(info.parse(nullptr, 0));
  EXPECT_FALSE(info.parsed);
}

}  // namespace esphome::tuya::testing
