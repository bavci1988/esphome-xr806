#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>
#include "esphome/components/tuya/tuya_low_power_autobaud.h"

namespace esphome::tuya::testing {

static std::vector<uint8_t> make_frame(uint8_t command, const std::string &payload) {
  std::vector<uint8_t> frame{
      0x55, 0xAA, 0x00, command, static_cast<uint8_t>(payload.size() >> 8), static_cast<uint8_t>(payload.size())};
  frame.insert(frame.end(), payload.begin(), payload.end());
  uint8_t checksum = 0;
  for (uint8_t byte : frame)
    checksum += byte;
  frame.push_back(checksum);
  return frame;
}

TEST(TuyaLowPowerAutoBaud, TimeoutSwitchesRate) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  ASSERT_TRUE(scan.probe_due(1000));
  EXPECT_EQ(scan.baud(), 9600u);
  scan.probe_sent(1000);
  EXPECT_FALSE(scan.probe_due(1499));
  EXPECT_TRUE(scan.probe_due(1500));
  EXPECT_EQ(scan.baud(), 115200u);
}

TEST(TuyaLowPowerAutoBaud, LocksOnlyOnProductInfo) {
  TuyaLowPowerAutoBaud scan;
  scan.start(115200);
  ASSERT_TRUE(scan.probe_due(0));
  scan.probe_sent(0);
  auto report = make_frame(0x05, "x");
  EXPECT_FALSE(scan.accept_frame(report.data(), report.size(), 50));
  auto product = make_frame(0x01, "{\"p\":\"x\"}");
  product.back() ^= 0xFF;  // Bad checksum.
  EXPECT_FALSE(scan.accept_frame(product.data(), product.size(), 60));
  product.back() ^= 0xFF;
  EXPECT_TRUE(scan.accept_frame(product.data(), product.size(), 70));
  EXPECT_TRUE(scan.locked());
  EXPECT_FALSE(scan.searching());
  EXPECT_EQ(scan.baud(), 115200u);
}

TEST(TuyaLowPowerAutoBaud, OtherValidFrameKeepsRateAndProbesAgain) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  ASSERT_TRUE(scan.probe_due(0));
  scan.probe_sent(0);
  ASSERT_TRUE(scan.probe_due(500));  // No reply at 9600.
  scan.probe_sent(500);
  ASSERT_EQ(scan.baud(), 115200u);

  // The MCU wakes up and reports on its own, late in the probe window.
  scan.probe_again(800);
  ASSERT_TRUE(scan.probe_due(800));
  EXPECT_EQ(scan.baud(), 115200u);
  scan.probe_sent(800);

  // More reports of the same burst: no probe per frame, and no rate change at the old deadline.
  scan.probe_again(820);
  EXPECT_FALSE(scan.probe_due(821));
  EXPECT_FALSE(scan.probe_due(1301));
  EXPECT_EQ(scan.baud(), 115200u);

  auto product = make_frame(0x01, "{\"p\":\"x\"}");
  EXPECT_TRUE(scan.accept_frame(product.data(), product.size(), 900));
  EXPECT_EQ(scan.baud(), 115200u);
}

TEST(TuyaLowPowerAutoBaud, SilentMcuIsProbedLessOften) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  uint32_t now = 0;
  for (uint32_t i = 0; i < TuyaLowPowerAutoBaud::FAST_PROBE_ATTEMPTS; ++i) {
    ASSERT_TRUE(scan.probe_due(now));
    scan.probe_sent(now);
    now += TuyaLowPowerAutoBaud::PROBE_TIMEOUT_MS;
  }
  ASSERT_TRUE(scan.probe_due(now));
  scan.probe_sent(now);
  EXPECT_FALSE(scan.probe_due(now + TuyaLowPowerAutoBaud::SLOW_PROBE_TIMEOUT_MS - 1));
  EXPECT_TRUE(scan.probe_due(now + TuyaLowPowerAutoBaud::SLOW_PROBE_TIMEOUT_MS));
}

TEST(TuyaLowPowerAutoBaud, ProbeAgainIgnoredAfterLock) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  ASSERT_TRUE(scan.probe_due(0));
  scan.probe_sent(0);
  auto product = make_frame(0x01, "{}");
  ASSERT_TRUE(scan.accept_frame(product.data(), product.size(), 10));
  scan.probe_again(500);
  EXPECT_FALSE(scan.probe_due(1000));
}

}  // namespace esphome::tuya::testing
