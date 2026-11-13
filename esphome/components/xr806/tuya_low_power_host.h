#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_TUYA_LOW_POWER_V0)

#include "esphome/components/tuya/tuya.h"
#include "esphome/components/uart/uart_component_xr806.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "gpio.h"
#include "tuya_wake_lines.h"
#ifdef USE_XR806_LOWPOWER
#include "lowpower_policy.h"
#endif
#ifdef USE_XR806_STANDBY_CONTROLLER
#include "standby_controller.h"
#endif

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TUYA_HOST_TAG, "xr806.tuya_host");

/// Connects a Tuya hub running protocol low_power_v0 to XR806 power management:
/// the MCU decides when the module may sleep, and this bridge carries that to the standby controller.
class XR806TuyaLowPowerHost final : public Component,
                                    public tuya::TuyaLowPowerHost
#ifdef USE_XR806_STANDBY_CONTROLLER
    ,
                                    public XR806StandbyParticipant
#endif
{
 public:
  XR806TuyaLowPowerHost(tuya::Tuya *tuya, uart::XR806UARTComponent *uart) : tuya_(tuya), uart_(uart) {
    // Before Tuya::setup() runs, which already uses the host.
    tuya->set_low_power_host(this);
  }

  void setup() override {
#ifdef USE_XR806_STANDBY_CONTROLLER
    // The generic standby controller never sends Tuya frames or reads the MCU's sleep mode.
    if (auto *controller = standby_controller(); controller != nullptr) {
      this->registered_ = controller->register_participant(this);
      if (!this->registered_)
        this->mark_failed(LOG_STR("Cannot register the Tuya MCU as standby participant"));
    }
#endif
  }

  void require_external_mcu() override {
#ifdef USE_XR806_LOWPOWER
    lowpower_policy().require_external_mcu(true);
#endif
  }
  void set_mcu_synchronized(bool synchronized) override {
#ifdef USE_XR806_LOWPOWER
    lowpower_policy().set_mcu_synchronized(synchronized);
#endif
  }
  int configure_wake_lines(int wake_gpio, int mcu_wake_gpio, bool mcu_wake_active_high,
                           bool create_mcu_wake_output) override {
    const TuyaWakeLines lines = map_tuya_wake_lines(wake_gpio, mcu_wake_gpio);
    if (create_mcu_wake_output && lines.mcu_wake >= 0 && !this->mcu_wake_created_) {
      this->mcu_wake_.set_pin(static_cast<uint8_t>(lines.mcu_wake));
      this->mcu_wake_.set_inverted(!mcu_wake_active_high);
      this->mcu_wake_.set_flags(gpio::FLAG_OUTPUT);
      this->mcu_wake_created_ = true;
      char name[24];
      this->mcu_wake_.dump_summary(name, sizeof(name));
      ESP_LOGI(TUYA_HOST_TAG, "MCU wake output from product info: %s", name);
    }
#ifdef USE_XR806_STANDBY_CONTROLLER
    auto *controller = standby_controller();
    if (controller != nullptr && !controller->has_wake_pin() && lines.wake_input >= 0) {
      // The MCU pulls its line low to wake the module, as the original firmware expects.
      controller->set_wake_pin(static_cast<uint8_t>(lines.wake_input));
      controller->set_wake_falling(true);
      ESP_LOGI(TUYA_HOST_TAG, "Standby wake input from product info: PA%d", lines.wake_input);
    }
#endif
    if (lines.wake_input < 0)
      ESP_LOGW(TUYA_HOST_TAG, "MCU wake lines wk=%d mwk=%d have no known pins on this module", wake_gpio,
               mcu_wake_gpio);
    return lines.wake_input;
  }
  uint32_t standby_blockers() const override {
#ifdef USE_XR806_LOWPOWER
    return lowpower_policy().blockers();
#else
    return 0;
#endif
  }
  GPIOPin *mcu_wake_output() override { return this->mcu_wake_created_ ? &this->mcu_wake_ : nullptr; }

  uint32_t uart_rx_error_count() const override { return this->uart_->get_rx_error_count(); }
  bool ota_busy() const override {
#ifdef USE_XR806_LOWPOWER
    return (lowpower_policy().blockers() & XR806LowPowerPolicy::OTA_BUSY) != 0;
#else
    return false;
#endif
  }
  bool standby_requested() const override {
#ifdef USE_XR806_LOWPOWER
    return lowpower_policy().standby_requested();
#else
    return false;
#endif
  }
  bool network_quiescing() const override {
#ifdef USE_XR806_LOWPOWER
    return lowpower_policy().network_quiescing();
#else
    return false;
#endif
  }

#ifdef USE_XR806_STANDBY_CONTROLLER
  bool standby_supported() const override { return this->registered_ && standby_controller() != nullptr; }
  int standby_wake_pin() const override {
    auto *controller = standby_controller();
    return controller != nullptr && controller->has_wake_pin() ? controller->wake_pin() : -1;
  }
  bool standby_requires_external_sync() const override {
    auto *controller = standby_controller();
    return controller != nullptr && controller->requires_external_sync();
  }
  bool standby_may_quiesce() const override {
    auto *controller = standby_controller();
    return controller != nullptr && controller->may_quiesce();
  }
  bool standby_services_quiesced() const override {
    auto *controller = standby_controller();
    return controller != nullptr && controller->services_quiesced();
  }
  void request_preflight(bool requested) override {
    if (auto *controller = standby_controller(); controller != nullptr)
      controller->request_preflight(requested);
  }
  bool preflight_requested() const override {
    auto *controller = standby_controller();
    return controller != nullptr && controller->preflight_requested();
  }
  void request_standby(bool requested) override {
    if (auto *controller = standby_controller(); controller != nullptr)
      controller->request_standby(requested);
  }
  bool standby_keeps_network() const override {
    auto *controller = standby_controller();
    return controller != nullptr && controller->stay_connected();
  }

  // XR806StandbyParticipant: the MCU UART owner.
  bool owns_uart() const override { return true; }
  bool prepare_for_standby() override { return this->tuya_->low_power_prepare_for_standby(); }
  void standby_cancelled() override { this->tuya_->low_power_standby_cancelled(); }
  void standby_resumed(const XR806StandbyResume &event) override {
    this->tuya_->low_power_standby_resumed(event.expected_source, event.wake_gpio);
  }
#else
  bool standby_supported() const override { return false; }
  int standby_wake_pin() const override { return -1; }
  bool standby_requires_external_sync() const override { return false; }
  bool standby_may_quiesce() const override { return false; }
  bool standby_services_quiesced() const override { return false; }
  void request_preflight(bool requested) override {}
  bool preflight_requested() const override { return false; }
  void request_standby(bool requested) override {}
#endif

 protected:
  tuya::Tuya *tuya_;
  uart::XR806UARTComponent *uart_;
  XR806GPIOPin mcu_wake_;
  bool mcu_wake_created_{false};
#ifdef USE_XR806_STANDBY_CONTROLLER
  bool registered_{false};
#endif
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_TUYA_LOW_POWER_V0
