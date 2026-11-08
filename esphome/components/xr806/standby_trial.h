#pragma once

#ifdef USE_XR806_STANDBY_TRIAL

#include <cstdint>
#include "esphome/core/component.h"
#include "standby_trial_state.h"

namespace esphome::xr806 {

// Explicit, OFFLINE bench-only Stage 7b path. This is not the production
// ESPHome WiFi/API/OTA power scheduler. No MCU type is assumed.
class XR806StandbyTrial final : public Component {
 public:
  void set_entry_delay_ms(uint32_t ms) { entry_delay_ms_ = ms; }
  void set_wake_after_ms(uint32_t ms) { wake_after_ms_ = ms; }
  void set_wake_pin(uint8_t pa_pin) { wake_pin_ = pa_pin; has_wake_pin_ = true; }
  void set_wake_falling(bool falling) { wake_falling_ = falling; }

  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  void attempt_standby_();

  XR806StandbyTrialState state_{};
  uint32_t entry_delay_ms_{10000};
  uint32_t wake_after_ms_{3000};
  uint8_t wake_pin_{0};
  bool has_wake_pin_{false};
  bool wake_falling_{true};
};

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_TRIAL
