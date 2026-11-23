#include <gtest/gtest.h>
#include "esphome/components/xr806/tuya_wake_lines.h"

namespace esphome::xr806::testing {

TEST(TuyaWakeLines, WxuNumbering) {
  // HC32 firmware 1.3.1, written for the WXU: PA20 in, PB4 out.
  const TuyaWakeLines lines = map_tuya_wake_lines(20, 28);
  EXPECT_EQ(lines.wake_input, 20);
  EXPECT_EQ(lines.mcu_wake, 36);
  EXPECT_EQ(map_tuya_wake_lines(11, 5).mcu_wake, 5);
  EXPECT_EQ(map_tuya_wake_lines(19, -1).mcu_wake, -1);
}

TEST(TuyaWakeLines, T3uPadsOfTheSameHousing) {
  // MCU 1.0.9, written for the T3-U: P32 is pad 9 (PA20), P16 is pad 2 (PB4).
  const TuyaWakeLines lines = map_tuya_wake_lines(32, 16);
  EXPECT_EQ(lines.wake_input, 20);
  EXPECT_EQ(lines.mcu_wake, 36);
}

TEST(TuyaWakeLines, UnknownPinsStayUnmapped) {
  const TuyaWakeLines lines = map_tuya_wake_lines(7, 9);
  EXPECT_EQ(lines.wake_input, -1);
  EXPECT_EQ(lines.mcu_wake, -1);
  EXPECT_EQ(map_tuya_wake_lines(-1, -1).wake_input, -1);
}

}  // namespace esphome::xr806::testing
