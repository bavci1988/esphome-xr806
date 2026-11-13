#include <gtest/gtest.h>
#include "esphome/components/tuya_trv16/trv16_schedule.h"

namespace esphome::tuya_trv16::testing {

static std::vector<uint8_t> program(size_t points) {
  std::vector<uint8_t> data;
  for (size_t i = 0; i < points; i++) {
    const uint16_t temp = 160 + i;
    data.insert(data.end(),
                {static_cast<uint8_t>(i % 6 * 2), 0, static_cast<uint8_t>(temp >> 8), static_cast<uint8_t>(temp)});
  }
  return data;
}

static const SchedulePoint NEW_POINTS[6] = {{6, 0, 210},  {8, 30, 170}, {12, 0, 200},
                                            {14, 0, 170}, {18, 0, 215}, {22, 0, 160}};

TEST(TuyaTRV16Schedule, UnknownUntilTheMcuReports) {
  Schedule s;
  size_t first = 9, count = 9, bad = 0;
  s.day_points(0, first, count);
  EXPECT_EQ(count, 0u);
  std::vector<uint8_t> out;
  EXPECT_EQ(s.encode(0x7F, NEW_POINTS, out, bad), 0);
  const auto wrong = program(5);
  EXPECT_FALSE(s.update(WEEK_PROGRAM_DP, wrong.data(), wrong.size()));
}

TEST(TuyaTRV16Schedule, WeekProgramOfMcu109) {
  Schedule s;
  const auto data = program(42);
  ASSERT_TRUE(s.update(WEEK_PROGRAM_DP, data.data(), data.size()));
  EXPECT_EQ(s.layout(), ScheduleLayout::SCHEDULE_LAYOUT_WEEK);
  size_t first, count;
  s.day_points(2, first, count);
  EXPECT_EQ(first, 12u);
  EXPECT_EQ(count, 6u);
  EXPECT_EQ(s.point(13).temperature_x10, 173);

  std::vector<uint8_t> out;
  size_t bad = 0;
  ASSERT_EQ(s.encode(1u << 2, NEW_POINTS, out, bad), WEEK_PROGRAM_DP);
  ASSERT_EQ(out.size(), WEEK_PROGRAM_BYTES);
  EXPECT_EQ(out[2 * 24 + 4], 8);  // Wednesday point 2: 08:30
  EXPECT_EQ(out[2 * 24 + 5], 30);
  EXPECT_EQ(out[2 * 24 + 6], 0);  // 17.0 degrees, big endian
  EXPECT_EQ(out[2 * 24 + 7], 170);
  EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 48), std::vector<uint8_t>(data.begin(), data.begin() + 48));
}

TEST(TuyaTRV16Schedule, WorkdayWeekendProgramOfHc32) {
  Schedule s;
  const auto data = program(8);
  ASSERT_TRUE(s.update(WORKDAY_PROGRAM_DP, data.data(), data.size()));
  EXPECT_EQ(s.layout(), ScheduleLayout::SCHEDULE_LAYOUT_WORKDAY_WEEKEND);
  size_t first, count;
  s.day_points(4, first, count);  // Friday
  EXPECT_EQ(first, 0u);
  EXPECT_EQ(count, 6u);
  s.day_points(5, first, count);  // Saturday with profile 1 (5+2)
  EXPECT_EQ(first, 6u);
  EXPECT_EQ(count, 2u);
  s.set_week_profile(2);  // 6+1: only Sunday
  s.day_points(5, first, count);
  EXPECT_EQ(count, 6u);
  EXPECT_TRUE(s.is_weekend(6));
  s.set_week_profile(0);
  EXPECT_FALSE(s.is_weekend(6));
  s.set_week_profile(1);

  std::vector<uint8_t> out;
  size_t bad = 0;
  ASSERT_EQ(s.encode(1u << 6, NEW_POINTS, out, bad), WORKDAY_PROGRAM_DP);  // Sunday: weekend points only
  ASSERT_EQ(out.size(), WORKDAY_PROGRAM_BYTES);
  EXPECT_EQ(std::vector<uint8_t>(out.begin(), out.begin() + 24), std::vector<uint8_t>(data.begin(), data.begin() + 24));
  EXPECT_EQ(out[24], 6);
  EXPECT_EQ(out[28], 8);
  ASSERT_EQ(s.encode(0x1F, NEW_POINTS, out, bad), WORKDAY_PROGRAM_DP);  // Monday-Friday: workday points
  EXPECT_EQ(out[20], 22);
  EXPECT_EQ(std::vector<uint8_t>(out.begin() + 24, out.end()), std::vector<uint8_t>(data.begin() + 24, data.end()));
}

TEST(TuyaTRV16Schedule, RejectsPointsOutOfOrder) {
  Schedule s;
  const auto data = program(42);
  s.update(WEEK_PROGRAM_DP, data.data(), data.size());
  SchedulePoint points[6];
  std::copy(std::begin(NEW_POINTS), std::end(NEW_POINTS), points);
  points[3] = {11, 0, 170};  // before point 3
  std::vector<uint8_t> out;
  size_t bad = 0;
  EXPECT_EQ(s.encode(0x7F, points, out, bad), 0);
  EXPECT_EQ(bad, 4u);
  points[3] = {14, 0, 0};  // no temperature
  EXPECT_EQ(s.encode(0x7F, points, out, bad), 0);
  EXPECT_EQ(bad, 4u);
}

}  // namespace esphome::tuya_trv16::testing
