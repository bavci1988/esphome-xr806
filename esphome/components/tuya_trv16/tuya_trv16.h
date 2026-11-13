#pragma once

#include "esphome/components/tuya/tuya.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome::tuya_trv16 {

static constexpr uint8_t POWER_DP = 1;
static constexpr uint8_t MODE_DP = 2;
static constexpr uint8_t TARGET_TEMPERATURE_DP = 3;
/// DP2 values, the same on both known MCU boards of the TRV16 (confirmed on MCU 1.0.9).
static constexpr uint8_t MODE_MANUAL = 0;
static constexpr uint8_t MODE_PROGRAM = 1;

/// Device behaviour of the TRV16 radiator thermostat that the generic Tuya entities cannot express.
class TuyaTRV16 : public Component {
 public:
  explicit TuyaTRV16(tuya::Tuya *tuya) : tuya_(tuya) {}

  void setup() override;
  void dump_config() override;

  /// Keeps the thermostat out of its built-in weekly program after a start: the program cannot be edited here,
  /// and its factory points would change the target temperature.
  void set_force_manual_mode(bool force) { this->force_manual_mode_ = force; }
#ifdef USE_BINARY_SENSOR
  /// On after the batteries were changed: the MCU stays off with target 0 until the button on the device is pressed.
  void set_awaiting_confirmation_binary_sensor(binary_sensor::BinarySensor *sensor) {
    this->awaiting_confirmation_binary_sensor_ = sensor;
  }
#endif

 protected:
  void check_mode_();
  void publish_awaiting_confirmation_();
  bool mcu_reports_changes_() const;

  tuya::Tuya *tuya_;
  int8_t power_{-1};                // DP1, -1 until reported
  int32_t target_temperature_{-1};  // DP3 times ten, -1 until reported
  int8_t mode_{-1};                 // DP2, -1 until reported
  uint8_t mode_attempts_{0};
  bool mode_settled_{false};
  bool force_manual_mode_{false};
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *awaiting_confirmation_binary_sensor_{nullptr};
#endif
};

}  // namespace esphome::tuya_trv16
