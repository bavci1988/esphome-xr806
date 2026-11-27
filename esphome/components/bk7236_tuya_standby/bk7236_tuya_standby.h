#pragma once

#include "esphome/core/defines.h"

#if defined(USE_LIBRETINY_VARIANT_BK7236) && defined(USE_TUYA_LOW_POWER)

#include "esphome/components/libretiny/gpio_arduino.h"
#include "esphome/components/tuya/tuya_low_power_host.h"
#include "esphome/components/tuya/tuya_uart.h"
#include "esphome/components/uart/uart_component_libretiny.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::bk7236_tuya_standby {

/// Standby of a BK7236 module under a Tuya low_power MCU, without blocking the main loop.
///
/// While the MCU has agreed to sleep, the module allows ARMINO's low-voltage sleep: the CPU sleeps between
/// beacons and the Wi-Fi link stays up. The MCU's wake line (an interrupt, so also a wake source) ends the
/// standby; the Tuya hub then tells the MCU that the module is awake.
class BK7236TuyaStandby final : public Component, public tuya::TuyaLowPowerHost {
 public:
  BK7236TuyaStandby(tuya::TuyaUART *tuya, uart::LibreTinyUARTComponent *uart, uint32_t boot_hold_ms)
      : tuya_(tuya), uart_(uart), boot_hold_ms_(boot_hold_ms) {
    // Before the hub's setup(), which already asks for the host
    tuya->set_low_power_host(this);
  }

  void set_default_mcu_wake(int8_t pin) { this->default_mcu_wake_ = pin; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  // tuya::TuyaLowPowerHost
  void set_mcu_synchronized(bool synchronized) override {}
  bool ota_busy() const override;
  bool standby_supported() const override { return this->wake_input_ >= 0; }
  int standby_wake_pin() const override { return this->wake_input_; }
  bool standby_requires_external_sync() const override { return true; }
  bool standby_may_quiesce() const override;
  bool standby_services_quiesced() const override { return true; }
  void request_preflight(bool requested) override { this->preflight_requested_ = requested; }
  bool preflight_requested() const override { return this->preflight_requested_; }
  void request_standby(bool requested) override;
  bool standby_requested() const override { return this->standby_requested_; }
  bool network_quiescing() const override { return false; }
  bool standby_keeps_network() const override { return true; }
  int configure_wake_lines(int wake_gpio, int mcu_wake_gpio, bool mcu_wake_active_high,
                           bool create_mcu_wake_output) override;
  GPIOPin *mcu_wake_output() override { return this->mcu_wake_created_ ? &this->mcu_wake_ : nullptr; }

 protected:
  struct WakeLinesPref {
    int8_t mcu_wake{-1};
    uint8_t active_high{0};
  };

  static void wake_isr(BK7236TuyaStandby *arg);
  /// The MCU's TX line carries noise while the MCU sleeps; the receiver is off from the MCU's consent until a wake.
  void set_mcu_rx_(bool enable);
  /// The product info names pins of the module the MCU firmware was written for: T3-U pins as they are,
  /// WXU (XR806) pins mapped to the T3-U pads in the same place. -1 when the pin is unknown.
  static int map_wake_line(int gpio);
  void set_mcu_wake_(int8_t pin, bool active_high);
  void enter_();
  void leave_();

  tuya::TuyaUART *tuya_;
  uart::LibreTinyUARTComponent *uart_;
  uint32_t boot_hold_ms_;
  uint32_t next_attempt_ms_{0};
  libretiny::ArduinoInternalGPIOPin wake_in_;
  libretiny::ArduinoInternalGPIOPin mcu_wake_;
  ESPPreferenceObject wake_pref_;
  WakeLinesPref saved_{};
  int8_t wake_input_{-1};
  int8_t default_mcu_wake_{-1};
  volatile bool wake_requested_{false};
  bool mcu_wake_created_{false};
  bool preflight_requested_{false};
  bool standby_requested_{false};
  bool sleeping_{false};
  volatile bool rx_suspended_{false};
  bool wake_held_logged_{false};
};

}  // namespace esphome::bk7236_tuya_standby

#endif  // USE_LIBRETINY_VARIANT_BK7236 && USE_TUYA_LOW_POWER
