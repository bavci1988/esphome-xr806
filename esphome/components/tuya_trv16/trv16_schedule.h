#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome::tuya_trv16 {

/// Which heating program the MCU reported. The MCU boards of the TRV16 differ only here, so the
/// datapoint they send decides it; nothing has to be configured.
enum class ScheduleLayout : uint8_t {
  SCHEDULE_LAYOUT_UNKNOWN,
  /// MCU 1.0.9: DP31, 7 days with 6 switching points each.
  SCHEDULE_LAYOUT_WEEK,
  /// HC32 1.3.1: DP107, 6 points shared by all workdays and 2 for the weekend days.
  SCHEDULE_LAYOUT_WORKDAY_WEEKEND,
};

static constexpr uint8_t POWER_DP = 1;
static constexpr uint8_t TARGET_TEMPERATURE_DP = 3;
static constexpr uint8_t WEEK_PROGRAM_DP = 31;
static constexpr uint8_t WORKDAY_PROGRAM_DP = 107;
static constexpr uint8_t WEEK_PROFILE_DP = 104;
static constexpr size_t DAYS = 7;
static constexpr size_t POINTS_PER_DAY = 6;
static constexpr size_t WEEKEND_POINTS = 2;
static constexpr size_t POINT_BYTES = 4;
static constexpr size_t WEEK_PROGRAM_BYTES = DAYS * POINTS_PER_DAY * POINT_BYTES;
static constexpr size_t WORKDAY_PROGRAM_BYTES = (POINTS_PER_DAY + WEEKEND_POINTS) * POINT_BYTES;

/// One switching point: from this time of day on, the target temperature applies.
struct SchedulePoint {
  uint8_t hour;
  uint8_t minute;
  uint16_t temperature_x10;
};

/// The heating program as the MCU stores it: per point hour, minute and the temperature times ten (big endian).
/// Days count from Monday (0) to Sunday (6).
class Schedule {
 public:
  /// Takes a program report. Returns true when the report was a heating program of a known size.
  bool update(uint8_t datapoint, const uint8_t *data, size_t len) {
    if (datapoint == WEEK_PROGRAM_DP && len == WEEK_PROGRAM_BYTES) {
      this->layout_ = ScheduleLayout::SCHEDULE_LAYOUT_WEEK;
    } else if (datapoint == WORKDAY_PROGRAM_DP && len == WORKDAY_PROGRAM_BYTES) {
      this->layout_ = ScheduleLayout::SCHEDULE_LAYOUT_WORKDAY_WEEKEND;
    } else {
      return false;
    }
    std::memcpy(this->data_.data(), data, len);
    return true;
  }

  /// DP104 of the HC32: 1 makes Saturday and Sunday the weekend, 2 only Sunday. 0 has no weekend
  /// (all days use the workday points); that reading of 0 is not confirmed yet.
  void set_week_profile(uint8_t profile) { this->week_profile_ = profile; }
  uint8_t week_profile() const { return this->week_profile_; }
  ScheduleLayout layout() const { return this->layout_; }

  bool is_weekend(size_t day) const {
    if (this->layout_ != ScheduleLayout::SCHEDULE_LAYOUT_WORKDAY_WEEKEND || this->week_profile_ == 0)
      return false;
    return day == 6 || (day == 5 && this->week_profile_ != 2);
  }

  /// The points that apply on a day: index of the first one and how many.
  void day_points(size_t day, size_t &first, size_t &count) const {
    if (this->layout_ == ScheduleLayout::SCHEDULE_LAYOUT_WEEK) {
      first = day * POINTS_PER_DAY;
      count = POINTS_PER_DAY;
    } else if (this->is_weekend(day)) {
      first = POINTS_PER_DAY;
      count = WEEKEND_POINTS;
    } else {
      first = 0;
      count = this->layout_ == ScheduleLayout::SCHEDULE_LAYOUT_UNKNOWN ? 0 : POINTS_PER_DAY;
    }
  }

  SchedulePoint point(size_t index) const {
    const uint8_t *p = this->data_.data() + index * POINT_BYTES;
    return {p[0], p[1], static_cast<uint16_t>((p[2] << 8) | p[3])};
  }

  /// Points must be valid times in increasing order with a plausible temperature (0.5 to 99.5 degrees).
  /// Returns the number of the first bad point (1-based), or 0 when all are fine.
  static size_t first_invalid_point(const SchedulePoint *points, size_t count) {
    int previous = -1;
    for (size_t i = 0; i < count; i++) {
      const SchedulePoint &p = points[i];
      const int minutes = p.hour * 60 + p.minute;
      if (p.hour > 23 || p.minute > 59 || minutes <= previous || p.temperature_x10 < 5 || p.temperature_x10 > 995)
        return i + 1;
      previous = minutes;
    }
    return 0;
  }

  /// Builds the program to send when `points` (6 of them) are set for the days in `day_mask` (bit 0 Monday).
  /// For the HC32 a workday writes the shared workday points, a weekend day the first two points as the
  /// weekend points. Returns the datapoint to write, or 0 when nothing is known yet or a point is invalid;
  /// `bad_point` then names the invalid point.
  uint8_t encode(uint8_t day_mask, const SchedulePoint *points, std::vector<uint8_t> &out, size_t &bad_point) const {
    bad_point = 0;
    if (this->layout_ == ScheduleLayout::SCHEDULE_LAYOUT_WEEK) {
      if ((bad_point = first_invalid_point(points, POINTS_PER_DAY)) != 0)
        return 0;
      out.assign(this->data_.begin(), this->data_.begin() + WEEK_PROGRAM_BYTES);
      for (size_t day = 0; day < DAYS; day++) {
        if (day_mask & (1u << day))
          write_points_(out.data() + day * POINTS_PER_DAY * POINT_BYTES, points, POINTS_PER_DAY);
      }
      return WEEK_PROGRAM_DP;
    }
    if (this->layout_ != ScheduleLayout::SCHEDULE_LAYOUT_WORKDAY_WEEKEND)
      return 0;
    bool workdays = false;
    bool weekend = false;
    for (size_t day = 0; day < DAYS; day++) {
      if (day_mask & (1u << day))
        (this->is_weekend(day) ? weekend : workdays) = true;
    }
    const size_t needed = workdays ? POINTS_PER_DAY : WEEKEND_POINTS;
    if ((bad_point = first_invalid_point(points, needed)) != 0)
      return 0;
    out.assign(this->data_.begin(), this->data_.begin() + WORKDAY_PROGRAM_BYTES);
    if (workdays)
      write_points_(out.data(), points, POINTS_PER_DAY);
    if (weekend)
      write_points_(out.data() + POINTS_PER_DAY * POINT_BYTES, points, WEEKEND_POINTS);
    return WORKDAY_PROGRAM_DP;
  }

 protected:
  static void write_points_(uint8_t *out, const SchedulePoint *points, size_t count) {
    for (size_t i = 0; i < count; i++) {
      out[i * POINT_BYTES] = points[i].hour;
      out[i * POINT_BYTES + 1] = points[i].minute;
      out[i * POINT_BYTES + 2] = points[i].temperature_x10 >> 8;
      out[i * POINT_BYTES + 3] = points[i].temperature_x10 & 0xFF;
    }
  }

  std::array<uint8_t, WEEK_PROGRAM_BYTES> data_{};
  ScheduleLayout layout_{ScheduleLayout::SCHEDULE_LAYOUT_UNKNOWN};
  uint8_t week_profile_{1};
};

}  // namespace esphome::tuya_trv16
