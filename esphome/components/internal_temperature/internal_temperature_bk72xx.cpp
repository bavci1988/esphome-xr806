#ifdef USE_BK72XX

#include "esphome/core/log.h"
#include "internal_temperature.h"

extern "C" {
#ifdef USE_LIBRETINY_VARIANT_BK7236
// ARMINO SDK, components/sensor.h; returns BK_OK (0) on success
int bk_sensor_get_current_temperature(float *temperature);
#else
uint32_t temp_single_get_current_temperature(uint32_t *temp_value);
#endif
}

namespace esphome::internal_temperature {

ESPHOME_LOG_TAG(TAG, "internal_temperature");

void InternalTemperatureSensor::update() {
  float temperature = NAN;
  bool success = false;

#ifdef USE_LIBRETINY_VARIANT_BK7236
  success = bk_sensor_get_current_temperature(&temperature) == 0;
#else
  uint32_t raw, result;
  result = temp_single_get_current_temperature(&raw);
  success = (result == 0);
#if defined(USE_LIBRETINY_VARIANT_BK7231N)
  temperature = raw * -0.38f + 156.0f;
#else   // USE_LIBRETINY_VARIANT
  temperature = raw * 0.128f;
#endif  // USE_LIBRETINY_VARIANT
#endif  // USE_LIBRETINY_VARIANT_BK7236

  if (success && std::isfinite(temperature)) {
    this->publish_state(temperature);
  } else {
    ESP_LOGD(TAG, "Ignoring invalid temperature (success=%d, value=%.1f)", success, temperature);
    if (!this->has_state()) {
      this->publish_state(NAN);
    }
  }
}

}  // namespace esphome::internal_temperature

#endif  // USE_BK72XX
