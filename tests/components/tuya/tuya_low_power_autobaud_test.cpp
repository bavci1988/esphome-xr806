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

  // The MCU wakes up and reports on its own: wait for a pause, then probe again at the same rate.
  scan.probe_again(800);
  EXPECT_FALSE(scan.probe_due(899));
  ASSERT_TRUE(scan.probe_due(900));
  EXPECT_EQ(scan.baud(), 115200u);
  scan.probe_sent(900);

  auto product = make_frame(0x01, "{\"p\":\"x\"}");
  EXPECT_TRUE(scan.accept_frame(product.data(), product.size(), 950));
  EXPECT_EQ(scan.baud(), 115200u);
}

TEST(TuyaLowPowerAutoBaud, BurstRightAfterProbeKeepsRate) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  ASSERT_TRUE(scan.probe_due(0));
  scan.probe_sent(0);
  // The MCU ignores the probe and reports from 30 to 200 ms.
  for (uint32_t t = 30; t <= 200; t += 20) {
    scan.probe_again(t);
    EXPECT_FALSE(scan.probe_due(t));
  }
  // After the burst the probe goes out again at 9600, never switching to 115200.
  ASSERT_TRUE(scan.probe_due(290));
  EXPECT_EQ(scan.baud(), 9600u);
  scan.probe_sent(290);
  EXPECT_FALSE(scan.probe_due(789));
  ASSERT_TRUE(scan.probe_due(790));  // A silent MCU after that: the search goes on.
  EXPECT_EQ(scan.baud(), 115200u);
}

TEST(TuyaLowPowerAutoBaud, SteadyTrafficStillProbes) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  ASSERT_TRUE(scan.probe_due(0));
  scan.probe_sent(0);
  uint32_t t = 10;
  for (; t < TuyaLowPowerAutoBaud::PROBE_TIMEOUT_MS; t += 50) {
    scan.probe_again(t);
    EXPECT_FALSE(scan.probe_due(t));
  }
  scan.probe_again(t);
  ASSERT_TRUE(scan.probe_due(t));
  EXPECT_EQ(scan.baud(), 9600u);
}

TEST(TuyaLowPowerAutoBaud, ProductInfoAfterReportInSameRead) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  ASSERT_TRUE(scan.probe_due(0));
  scan.probe_sent(0);
  scan.probe_again(120);
  auto product = make_frame(0x01, "{}");
  EXPECT_TRUE(scan.accept_frame(product.data(), product.size(), 120));
  EXPECT_TRUE(scan.locked());
}

TEST(TuyaLowPowerAutoBaud, AwakeMcuRestoresFastPace) {
  TuyaLowPowerAutoBaud scan;
  scan.start(9600);
  uint32_t now = 0;
  for (uint32_t i = 0; i <= TuyaLowPowerAutoBaud::FAST_PROBE_ATTEMPTS; ++i) {
    ASSERT_TRUE(scan.probe_due(now));
    scan.probe_sent(now);
    now += TuyaLowPowerAutoBaud::SLOW_PROBE_TIMEOUT_MS;
  }
  const uint32_t attempts = scan.attempts();
  scan.probe_again(now);
  // The warning count goes on, only the pace is reset.
  EXPECT_EQ(scan.attempts(), attempts);
  ASSERT_TRUE(scan.probe_due(now + TuyaLowPowerAutoBaud::MIN_PROBE_GAP_MS));
  scan.probe_sent(now + TuyaLowPowerAutoBaud::MIN_PROBE_GAP_MS);
  // Back to the fast pace.
  EXPECT_TRUE(scan.probe_due(now + TuyaLowPowerAutoBaud::MIN_PROBE_GAP_MS + TuyaLowPowerAutoBaud::PROBE_TIMEOUT_MS));
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
