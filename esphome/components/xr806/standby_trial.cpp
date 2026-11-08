// Stage 7b: real XR806 PM_MODE_STANDBY + timer / optional PA wake-up.
// Must be explicitly enabled, offline and one-shot; never silently sleep
// while Native API, MQTT, OTA, mDNS, WLAN or MCU UART may be active.
#include "esphome/core/defines.h"

#ifdef USE_XR806_STANDBY_TRIAL

// CONFIG_PM is emitted by the SDK's generated xr_config.h (not defines.h).
extern "C" {
#include "xr_config.h"
}

#if !defined(CONFIG_PM) || !defined(CONFIG_PM_WAKELOCKS) || !defined(CONFIG_STANDBY_RET_SRAM_ALL)
#error "XR806 Stage 7b requires PM, PM wakelocks and full standby SRAM retention"
#endif
#if !defined(CONFIG_CHIP_ARCH_VER) || CONFIG_CHIP_ARCH_VER != 3
#error "XR806 Stage 7b PA10..PA23 wake-index mapping needs XR806 architecture v3"
#endif

#include "esphome/components/xr806/standby_trial.h"
#include "esphome/components/xr806/lowpower_policy.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

extern "C" {
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_wakeup.h"
#include "pm/pm.h"
}

namespace esphome::xr806 {

static const char *const TAG = "xr806.standby";

void XR806StandbyTrial::setup() {
  this->state_.arm(millis());
  ESP_LOGW(TAG, "Stage 7b ONE-SHOT bench trial armed: no WiFi/API/OTA/UART clients");
}

void XR806StandbyTrial::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 physical standby trial (offline only):");
  ESP_LOGCONFIG(TAG, "  Enter after: %u ms; wake timer: %u ms",
                static_cast<unsigned>(this->entry_delay_ms_),
                static_cast<unsigned>(this->wake_after_ms_));
  if (this->has_wake_pin_)
    ESP_LOGCONFIG(TAG, "  Wake PA%u / IO%u: %s edge",
                  static_cast<unsigned>(this->wake_pin_),
                  static_cast<unsigned>(this->wake_pin_ - 10),
                  this->wake_falling_ ? "falling" : "rising");
  ESP_LOGCONFIG(TAG, "  Mode: PM_MODE_STANDBY (SRAM retention), NOT hibernation");
}

void XR806StandbyTrial::loop() {
  if (!this->state_.claim(millis(), this->entry_delay_ms_))
    return;
  this->attempt_standby_();
}

void XR806StandbyTrial::attempt_standby_() {
  auto &p = lowpower_policy();

  // The Python schema refuses all network/UART transports for this bench
  // path. The optional external MCU gate is NOT bypassed by the platform.
  // A future scheduler will instead obtain each idle vote from its owner.
  p.request_standby(true);
  p.set_uart_idle(true);
  p.set_network_idle(true);
  p.set_ota_idle(true);
  p.set_api_idle(true);
  p.set_mdns_idle(true);
  p.set_scheduler_idle(true);

  const int wake_index = this->has_wake_pin_ ? static_cast<int>(this->wake_pin_) - 10 : -1;
  bool pin_armed = false;

  // Always arm a short timer as a recovery path, even if a GPIO wakes first.
  // HAL_Wakeup_SetTimer_mS returns negative when the timer is invalid.
  if (HAL_Wakeup_SetTimer_mS(this->wake_after_ms_) != 0) {
    ESP_LOGE(TAG, "Cannot arm hardware wake timer; standby cancelled");
    goto cleanup;
  }

  if (this->has_wake_pin_) {
    GPIO_InitParam gpio{};
    gpio.mode = GPIOx_Pn_F6_EINT;  // Required by HAL_Wakeup_CheckIOMode().
    gpio.driving = GPIO_DRIVING_LEVEL_1;
    gpio.pull = this->wake_falling_ ? GPIO_PULL_UP : GPIO_PULL_DOWN;
    const auto pin = static_cast<GPIO_Pin>(this->wake_pin_);
    HAL_GPIO_Init(GPIO_PORT_A, pin, &gpio);
    HAL_Wakeup_SetIO(static_cast<uint32_t>(wake_index),
                     this->wake_falling_ ? WKUPIO_WK_MODE_FALLING_EDGE
                                         : WKUPIO_WK_MODE_RISING_EDGE,
                     gpio.pull);
    pin_armed = true;
    HAL_PRCM_SetWakeupDebClk0(0);
    HAL_PRCM_SetWakeupIOxDebSrc(static_cast<uint32_t>(wake_index), 0);
    HAL_PRCM_SetWakeupIOxDebounce(static_cast<uint32_t>(wake_index), 1);
  }

  if (!HAL_Wakeup_CheckIOMode()) {
    ESP_LOGE(TAG, "Wake IO is not in EINT mode; standby cancelled");
    goto cleanup;
  }
  p.set_wake_source_ready(true);
  if (!p.can_enter_standby()) {
    ESP_LOGW(TAG, "Stage 7b standby refused: blockers=0x%08lx",
             static_cast<unsigned long>(p.blockers()));
    goto cleanup;
  }

  // Calling pm_enter_mode() with OS_WAIT_FOREVER can hang indefinitely on an
  // SDK wakelock, despite a configured hardware wake timer. Bound the wait.
  HAL_Wakeup_SetEvent(0);
  ESP_LOGI(TAG, "Entering PM_MODE_STANDBY (2), timer=%u ms",
           static_cast<unsigned>(this->wake_after_ms_));
  {
    const int result = pm_enter_mode_timeout(PM_MODE_STANDBY, 500);
    const uint32_t event = HAL_Wakeup_GetEvent();
    if (result == 0 && XR806StandbyTrialState::expected_wake_event(event, wake_index)) {
      ESP_LOGI(TAG, "Standby resumed: event=0x%08lx, PM result=%d",
               static_cast<unsigned long>(event), result);
    } else {
      // A return value alone is NOT proof the chip physically entered standby.
      ESP_LOGW(TAG, "Standby not confirmed: event=0x%08lx, PM result=%d",
               static_cast<unsigned long>(event), result);
    }
  }

cleanup:
  p.request_standby(false);
  p.set_wake_source_ready(false);
  p.set_scheduler_idle(false);
  p.set_uart_idle(false);
  p.set_network_idle(false);
  p.set_ota_idle(false);
  p.set_api_idle(false);
  p.set_mdns_idle(false);
  (void) HAL_Wakeup_ClrTimer();
  if (pin_armed) {
    HAL_Wakeup_ClrIO(static_cast<uint32_t>(wake_index));
    HAL_PRCM_SetWakeupIOxDebounce(static_cast<uint32_t>(wake_index), 0);
    HAL_GPIO_DeInit(GPIO_PORT_A, static_cast<GPIO_Pin>(this->wake_pin_));
  }
  ESP_LOGI(TAG, "Stage 7b one-shot completed; automatic re-entry disabled");
}

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_TRIAL
