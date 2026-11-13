#pragma once

#include <array>

#include "esphome/components/tuya/tuya.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "trv16_schedule.h"

#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_DATETIME_TIME
#include "esphome/components/datetime/time_entity.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome::tuya_trv16 {

#ifdef USE_DATETIME_TIME
class TuyaTRV16PointTime;
#endif

/// Heating program of the TRV16 radiator thermostat, for both known MCU boards. It recognizes the program
/// format by the datapoint the MCU reports, shows the points of one chosen day for editing and writes them back.
class TuyaTRV16 : public Component {
 public:
  explicit TuyaTRV16(tuya::Tuya *tuya) : tuya_(tuya) {}

  void setup() override;
  void dump_config() override;

#ifdef USE_SELECT
  void set_day_select(select::Select *day) { this->day_select_ = day; }
#endif
#ifdef USE_NUMBER
  void set_point_temperature(size_t index, number::Number *number) { this->point_temperatures_[index] = number; }
#endif
#ifdef USE_DATETIME_TIME
  void set_point_time(size_t index, TuyaTRV16PointTime *time) { this->point_times_[index] = time; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_day_text_sensor(size_t day, text_sensor::TextSensor *sensor) { this->day_text_sensors_[day] = sensor; }
#endif

#ifdef USE_BINARY_SENSOR
  /// On after the batteries were changed: the MCU stays off with target 0 until the button on the device is pressed.
  void set_awaiting_confirmation_binary_sensor(binary_sensor::BinarySensor *sensor) {
    this->awaiting_confirmation_binary_sensor_ = sensor;
  }
#endif

  /// Loads the points of the chosen day into the edit fields.
  void load_day();
  /// Sends the edit fields as the program of the days in `day_mask` (bit 0 Monday .. bit 6 Sunday).
  void save(uint8_t day_mask);

 protected:
  void on_program_(const tuya::TuyaDatapoint &datapoint);
  void publish_days_();
  void publish_awaiting_confirmation_();
  size_t selected_day_() const;

  tuya::Tuya *tuya_;
  Schedule schedule_;
  int8_t power_{-1};                // DP1, -1 until reported
  int32_t target_temperature_{-1};  // DP3 times ten, -1 until reported
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *awaiting_confirmation_binary_sensor_{nullptr};
#endif
#ifdef USE_SELECT
  select::Select *day_select_{nullptr};
#endif
#ifdef USE_NUMBER
  std::array<number::Number *, POINTS_PER_DAY> point_temperatures_{};
#endif
#ifdef USE_DATETIME_TIME
  std::array<TuyaTRV16PointTime *, POINTS_PER_DAY> point_times_{};
#endif
#ifdef USE_TEXT_SENSOR
  std::array<text_sensor::TextSensor *, DAYS> day_text_sensors_{};
#endif
};

#ifdef USE_SELECT
class TuyaTRV16DaySelect : public select::Select, public Parented<TuyaTRV16> {
 protected:
  void control(size_t index) override {
    this->publish_state(index);
    this->parent_->load_day();
  }
};
#endif

#ifdef USE_NUMBER
class TuyaTRV16PointTemperature : public number::Number {
 protected:
  void control(float value) override { this->publish_state(value); }
};
#endif

#ifdef USE_DATETIME_TIME
class TuyaTRV16PointTime : public datetime::TimeEntity {
 public:
  void set(uint8_t hour, uint8_t minute) {
    this->hour_ = hour;
    this->minute_ = minute;
    this->second_ = 0;
    this->publish_state();
  }

 protected:
  void control(const datetime::TimeCall &call) override {
    this->set(call.get_hour().value_or(this->hour_), call.get_minute().value_or(this->minute_));
  }
};
#endif

#ifdef USE_BUTTON
class TuyaTRV16SaveButton : public button::Button, public Parented<TuyaTRV16> {
 public:
  explicit TuyaTRV16SaveButton(uint8_t day_mask) : day_mask_(day_mask) {}

 protected:
  void press_action() override { this->parent_->save(this->day_mask_); }
  uint8_t day_mask_;
};
#endif

}  // namespace esphome::tuya_trv16
