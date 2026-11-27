#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_TUYA_LOW_POWER)

#include "tuya_low_power_host.h"

#include "esphome/core/log.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.tuya_host");

int XR806TuyaLowPowerHost::configure_wake_lines(int wake_gpio, int mcu_wake_gpio, bool mcu_wake_active_high,
                                                bool create_mcu_wake_output) {
  const TuyaWakeLines lines = map_tuya_wake_lines(wake_gpio, mcu_wake_gpio);
  if (create_mcu_wake_output && lines.mcu_wake >= 0) {
    this->set_mcu_wake_(static_cast<int8_t>(lines.mcu_wake), mcu_wake_active_high);
    char name[24];
    this->mcu_wake_.dump_summary(name, sizeof(name));
    ESP_LOGI(TAG, "MCU wake output from product info: %s", name);
    const WakeLinesPref current{static_cast<int8_t>(lines.mcu_wake), static_cast<uint8_t>(mcu_wake_active_high)};
    if (current.mcu_wake != this->saved_.mcu_wake || current.active_high != this->saved_.active_high) {
      // Written only when the MCU names other lines (another MCU board), not on every start.
      this->saved_ = current;
      this->wake_pref_.save(&current);
    }
  }
#ifdef USE_XR806_STANDBY_CONTROLLER
  auto *controller = standby_controller();
  if (controller != nullptr && !controller->has_wake_pin() && lines.wake_input >= 0) {
    // The MCU pulls its line low to wake the module, as the original firmware expects.
    controller->set_wake_pin(static_cast<uint8_t>(lines.wake_input));
    controller->set_wake_falling(true);
    ESP_LOGI(TAG, "Standby wake input from product info: PA%d", lines.wake_input);
  }
#endif
  if (lines.wake_input < 0) {
    ESP_LOGW(TAG, "MCU wake lines wk=%d mwk=%d have no known pins on this module", wake_gpio, mcu_wake_gpio);
  }
  return lines.wake_input;
}

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_TUYA_LOW_POWER
