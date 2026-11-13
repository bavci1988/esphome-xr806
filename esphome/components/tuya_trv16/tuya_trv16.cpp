#include "tuya_trv16.h"

#include "esphome/core/log.h"

namespace esphome::tuya_trv16 {

ESPHOME_LOG_TAG(TAG, "tuya_trv16");

// Gives up when the MCU keeps reporting its weekly program, so a wrong value cannot cause endless writes.
static constexpr uint8_t MAX_MODE_ATTEMPTS = 6;

void TuyaTRV16::setup() {
  this->tuya_->register_listener(POWER_DP, [this](const tuya::TuyaDatapoint &dp) {
    if (dp.type != tuya::TuyaDatapointType::BOOLEAN)
      return;
    this->power_ = dp.value_bool ? 1 : 0;
    this->publish_awaiting_confirmation_();
  });
  this->tuya_->register_listener(TARGET_TEMPERATURE_DP, [this](const tuya::TuyaDatapoint &dp) {
    if (dp.type != tuya::TuyaDatapointType::INTEGER)
      return;
    this->target_temperature_ = dp.value_int;
    this->publish_awaiting_confirmation_();
  });
  if (!this->force_manual_mode_)
    return;
  this->tuya_->register_listener(MODE_DP, [this](const tuya::TuyaDatapoint &dp) {
    if (dp.type != tuya::TuyaDatapointType::ENUM)
      return;
    this->mode_ = dp.value_enum;
    this->check_mode_();
  });
  // The HC32 reports DP2 inside its status burst, before it accepts writes; try again once it is synchronized.
  this->tuya_->add_on_initialized_callback([this]() { this->check_mode_(); });
}

void TuyaTRV16::dump_config() {
  ESP_LOGCONFIG(TAG, "TRV16:\n  Force manual mode: %s", YESNO(this->force_manual_mode_));
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Awaiting confirmation", this->awaiting_confirmation_binary_sensor_);
#endif
}

bool TuyaTRV16::mcu_reports_changes_() const {
#ifdef USE_TUYA_LOW_POWER_V0
  return this->tuya_->low_power_mcu_reports_changes();
#else
  return true;
#endif
}

// Switches to manual mode after a start, repeating until the MCU reports changes on its own (when its status
// poll ends it fully accepts commands) and has confirmed manual mode. Afterwards the mode is free to change.
void TuyaTRV16::check_mode_() {
  if (this->mode_settled_ || this->mode_ < 0)
    return;
  if (this->mode_ == MODE_MANUAL) {
    if (this->mcu_reports_changes_())
      this->mode_settled_ = true;
    return;
  }
  if (this->mode_attempts_ >= MAX_MODE_ATTEMPTS) {
    ESP_LOGW(TAG, "The MCU stays in its weekly program; giving up");
    this->mode_settled_ = true;
    return;
  }
  this->mode_attempts_++;
  ESP_LOGI(TAG, "Weekly program off: switching to manual mode (attempt %u)", this->mode_attempts_);
  this->tuya_->set_enum_datapoint_value(MODE_DP, MODE_MANUAL);
}

void TuyaTRV16::publish_awaiting_confirmation_() {
#ifdef USE_BINARY_SENSOR
  // A target of 0 is reported only while the MCU waits for the button after a reset; switched off by the user,
  // it keeps its target temperature.
  if (this->awaiting_confirmation_binary_sensor_ != nullptr && this->power_ >= 0 && this->target_temperature_ >= 0)
    this->awaiting_confirmation_binary_sensor_->publish_state(this->power_ == 0 && this->target_temperature_ == 0);
#endif
}

}  // namespace esphome::tuya_trv16
