#pragma once

#include "esphome/core/defines.h"

#ifdef USE_XR806
extern "C" {
#include "driver/chip/hal_gpio.h"
}

namespace esphome::xr806 {

// Restricted WXU-module-pad-2 wake output. Not a general GPIO exemption.
// Only a separately verified MCU owner may opt in; all other code must
// continue to honor the board's conservative PB04 Flash/SPI reservation.
//
// This claim verifies the *current* mux, not the flash wiring or the
// electrical polarity. Neither can be validated by software alone.
class XR806GuardedWakePB04 {
 public:
  bool claim(bool active_high) {
    if (claimed_)
      return false;
    GPIO_InitParam previous{};
    HAL_GPIO_GetConfig(GPIO_PORT_B, GPIO_PIN_4, &previous);
    if (previous.mode != GPIOx_Pn_F0_INPUT && previous.mode != GPIOx_Pn_F7_DISABLE)
      return false;  // SPI0, any alternative peripheral, IRQ or output owner.

    active_high_ = active_high;
    previous_ = previous;
    GPIO_InitParam output{};
    output.mode = GPIOx_Pn_F1_OUTPUT;
    output.driving = GPIO_DRIVING_LEVEL_1;
    output.pull = GPIO_PULL_NONE;
    // Prime the inactive output latch while the pin is still an input.
    HAL_GPIO_WritePin(GPIO_PORT_B, GPIO_PIN_4, active_high_ ? GPIO_PIN_LOW : GPIO_PIN_HIGH);
    HAL_GPIO_Init(GPIO_PORT_B, GPIO_PIN_4, &output);
    HAL_GPIO_WritePin(GPIO_PORT_B, GPIO_PIN_4, active_high_ ? GPIO_PIN_LOW : GPIO_PIN_HIGH);
    claimed_ = true;
    return true;
  }

  bool claimed() const { return claimed_; }
  void drive(bool active) {
    if (!claimed_)
      return;
    const bool high = active ? active_high_ : !active_high_;
    HAL_GPIO_WritePin(GPIO_PORT_B, GPIO_PIN_4, high ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
  }

  void release() {
    if (!claimed_)
      return;
    drive(false);
    HAL_GPIO_Init(GPIO_PORT_B, GPIO_PIN_4, &previous_);
    claimed_ = false;
  }

 private:
  GPIO_InitParam previous_{};
  bool active_high_{false};
  bool claimed_{false};
};

}  // namespace esphome::xr806
#endif
