#include "tuya_trv16.h"

#include <cmath>
#include <cstdio>

#include "esphome/core/log.h"

namespace esphome::tuya_trv16 {

ESPHOME_LOG_TAG(TAG, "tuya_trv16");

void TuyaTRV16::setup() {
#ifdef USE_SELECT
  if (this->day_select_ != nullptr && !this->day_select_->has_state())
    this->day_select_->publish_state(static_cast<size_t>(0));  // Monday
#endif
  this->tuya_->register_listener(WEEK_PROGRAM_DP, [this](const tuya::TuyaDatapoint &dp) { this->on_program_(dp); });
  this->tuya_->register_listener(WORKDAY_PROGRAM_DP, [this](const tuya::TuyaDatapoint &dp) { this->on_program_(dp); });
  this->tuya_->register_listener(WEEK_PROFILE_DP, [this](const tuya::TuyaDatapoint &dp) {
    if (dp.type != tuya::TuyaDatapointType::ENUM || dp.value_enum == this->schedule_.week_profile())
      return;
    this->schedule_.set_week_profile(dp.value_enum);
    if (this->schedule_.layout() == ScheduleLayout::SCHEDULE_LAYOUT_WORKDAY_WEEKEND) {
      this->publish_days_();
      this->load_day();
    }
  });
}

void TuyaTRV16::dump_config() {
  ESP_LOGCONFIG(TAG, "TRV16 heating program:\n  Format: %s",
                this->schedule_.layout() == ScheduleLayout::SCHEDULE_LAYOUT_WEEK
                    ? LOG_STR_LITERAL("7 days x 6 points (DP31)")
                : this->schedule_.layout() == ScheduleLayout::SCHEDULE_LAYOUT_WORKDAY_WEEKEND
                    ? LOG_STR_LITERAL("workdays 6 + weekend 2 points (DP107)")
                    : LOG_STR_LITERAL("not reported yet"));
}

void TuyaTRV16::on_program_(const tuya::TuyaDatapoint &datapoint) {
  if (datapoint.type != tuya::TuyaDatapointType::RAW)
    return;
  const ScheduleLayout previous = this->schedule_.layout();
  if (!this->schedule_.update(datapoint.id, datapoint.value_raw.data(), datapoint.value_raw.size())) {
    ESP_LOGW(TAG, "Ignoring program DP%u of %u bytes", datapoint.id, static_cast<unsigned>(datapoint.value_raw.size()));
    return;
  }
  this->publish_days_();
  // The edit fields follow the MCU only when the format is first known, so a report does not overwrite unsaved edits.
  if (previous != this->schedule_.layout()) {
    ESP_LOGD(TAG, "Heating program format: DP%u", datapoint.id);
    this->load_day();
  }
}

size_t TuyaTRV16::selected_day_() const {
#ifdef USE_SELECT
  if (this->day_select_ != nullptr)
    return this->day_select_->active_index().value_or(0);
#endif
  return 0;
}

void TuyaTRV16::publish_days_() {
#ifdef USE_TEXT_SENSOR
  for (size_t day = 0; day < DAYS; day++) {
    if (this->day_text_sensors_[day] == nullptr)
      continue;
    size_t first, count;
    this->schedule_.day_points(day, first, count);
    char buf[96];
    size_t len = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < count && len < sizeof(buf); i++) {
      const SchedulePoint p = this->schedule_.point(first + i);
      len += snprintf(buf + len, sizeof(buf) - len, "%s%02u:%02u %u.%u", i ? ", " : "", p.hour, p.minute,
                      p.temperature_x10 / 10, p.temperature_x10 % 10);
    }
    this->day_text_sensors_[day]->publish_state(buf);
  }
#endif
}

void TuyaTRV16::load_day() {
  size_t first, count;
  this->schedule_.day_points(this->selected_day_(), first, count);
  for (size_t i = 0; i < count; i++) {
    const SchedulePoint p = this->schedule_.point(first + i);
#ifdef USE_DATETIME_TIME
    if (this->point_times_[i] != nullptr)
      this->point_times_[i]->set(p.hour, p.minute);
#endif
#ifdef USE_NUMBER
    if (this->point_temperatures_[i] != nullptr)
      this->point_temperatures_[i]->publish_state(p.temperature_x10 / 10.0f);
#endif
  }
  if (count == WEEKEND_POINTS)
    ESP_LOGI(TAG, "On weekend days this MCU uses only points 1 and 2");
}

void TuyaTRV16::save(uint8_t day_mask) {
  if (day_mask == 0)
    day_mask = 1u << this->selected_day_();  // The chosen day
  std::array<SchedulePoint, POINTS_PER_DAY> points{};
  for (size_t i = 0; i < POINTS_PER_DAY; i++) {
#ifdef USE_DATETIME_TIME
    if (this->point_times_[i] != nullptr) {
      points[i].hour = this->point_times_[i]->hour;
      points[i].minute = this->point_times_[i]->minute;
    }
#endif
#ifdef USE_NUMBER
    if (this->point_temperatures_[i] != nullptr && !std::isnan(this->point_temperatures_[i]->state))
      points[i].temperature_x10 = static_cast<uint16_t>(lroundf(this->point_temperatures_[i]->state * 10.0f));
#endif
  }
  std::vector<uint8_t> value;
  size_t bad_point = 0;
  const uint8_t datapoint = this->schedule_.encode(day_mask, points.data(), value, bad_point);
  if (datapoint == 0) {
    if (bad_point != 0) {
      ESP_LOGW(TAG, "Point %u is not after the one before it or has no temperature; program not sent",
               static_cast<unsigned>(bad_point));
    } else {
      ESP_LOGW(TAG, "The MCU has not reported its program yet; program not sent");
    }
    return;
  }
  ESP_LOGI(TAG, "Sending heating program DP%u (days 0x%02X)", datapoint, day_mask);
  this->tuya_->set_raw_datapoint_value(datapoint, value);
}

}  // namespace esphome::tuya_trv16
