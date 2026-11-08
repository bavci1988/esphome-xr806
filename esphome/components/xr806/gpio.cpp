#include "esphome/components/xr806/gpio.h"

#ifdef USE_XR806

#include <cstdio>

extern "C" {
#include "driver/chip/hal_gpio.h"
}

namespace esphome::xr806 {

namespace {

// Internal SoC encoding:
// PA0..PA23 = 0..23
// PB0..PB15 = 32..47
//
// These are NOT physical WXU module pad numbers.

bool valid_pin(uint8_t number) {
  return number <= 23 || (number >= 32 && number <= 47);
}

GPIO_Port gpio_port(uint8_t number) {
  return number >= 32 ? GPIO_PORT_B : GPIO_PORT_A;
}

GPIO_Pin gpio_pin(uint8_t number) {
  return static_cast<GPIO_Pin>(number & 0x1F);
}

bool has_flag(gpio::Flags flags, gpio::Flags flag) {
  return (static_cast<unsigned>(flags) &
          static_cast<unsigned>(flag)) != 0U;
}

}  // namespace

void XR806GPIOPin::pin_mode(gpio::Flags flags) {
  this->flags_ = flags;

  if (!valid_pin(this->pin_)) {
    return;
  }

  GPIO_InitParam param{};

  param.mode = has_flag(flags, gpio::FLAG_OUTPUT)
                   ? GPIOx_Pn_F1_OUTPUT
                   : GPIOx_Pn_F0_INPUT;

  param.driving = GPIO_DRIVING_LEVEL_1;

  if (has_flag(flags, gpio::FLAG_PULLUP)) {
    param.pull = GPIO_PULL_UP;
  } else if (has_flag(flags, gpio::FLAG_PULLDOWN)) {
    param.pull = GPIO_PULL_DOWN;
  } else {
    param.pull = GPIO_PULL_NONE;
  }

  HAL_GPIO_Init(
      gpio_port(this->pin_),
      gpio_pin(this->pin_),
      &param
  );
}

bool XR806GPIOPin::digital_read() {
  if (!valid_pin(this->pin_)) {
    return false;
  }

  bool value = HAL_GPIO_ReadPin(
      gpio_port(this->pin_),
      gpio_pin(this->pin_)
  ) == GPIO_PIN_HIGH;

  return value != this->inverted_;
}

void XR806GPIOPin::digital_write(bool value) {
  if (!valid_pin(this->pin_)) {
    return;
  }

  bool physical_value = value != this->inverted_;

  HAL_GPIO_WritePin(
      gpio_port(this->pin_),
      gpio_pin(this->pin_),
      physical_value ? GPIO_PIN_HIGH : GPIO_PIN_LOW
  );
}

size_t XR806GPIOPin::dump_summary(
    char *buffer, size_t len
) const {
  const char port = this->pin_ >= 32 ? 'B' : 'A';
  const unsigned pin = this->pin_ & 0x1F;

  const int written = std::snprintf(
      buffer,
      len,
      "P%c%u%s",
      port,
      pin,
      this->inverted_ ? " (inverted)" : ""
  );

  return written > 0 ? static_cast<size_t>(written) : 0;
}

// GPIO interrupts / ISR support are implemented separately.


// XR806_NATIVE_INTERRUPT_V1

ISRInternalGPIOPin XR806GPIOPin::to_isr() const {
  return ISRInternalGPIOPin(
      const_cast<XR806GPIOPin *>(this)
  );
}

void XR806GPIOPin::detach_interrupt() const {
  this->interrupt_attached_ = false;
  if (!valid_pin(this->pin_)) {
    return;
  }

  const auto port = gpio_port(this->pin_);
  const auto pin = gpio_pin(this->pin_);

  HAL_GPIO_DisableIRQ(port, pin);

  GPIO_InitParam config{};
  config.mode = GPIOx_Pn_F0_INPUT;
  config.driving = GPIO_DRIVING_LEVEL_1;

  if (has_flag(this->flags_, gpio::FLAG_PULLUP)) {
    config.pull = GPIO_PULL_UP;
  } else if (has_flag(this->flags_, gpio::FLAG_PULLDOWN)) {
    config.pull = GPIO_PULL_DOWN;
  } else {
    config.pull = GPIO_PULL_NONE;
  }

  HAL_GPIO_Init(port, pin, &config);
}

void XR806GPIOPin::attach_interrupt_(
    void (*func)(void *),
    void *arg,
    gpio::InterruptType type
) const {
  this->interrupt_attached_ = false;

  if (!valid_pin(this->pin_) || func == nullptr) {
    return;
  }

  GPIO_IrqEvent event;

  switch (type) {
    case gpio::INTERRUPT_RISING_EDGE:
      event = this->inverted_
                  ? GPIO_IRQ_EVT_FALLING_EDGE
                  : GPIO_IRQ_EVT_RISING_EDGE;
      break;

    case gpio::INTERRUPT_FALLING_EDGE:
      event = this->inverted_
                  ? GPIO_IRQ_EVT_RISING_EDGE
                  : GPIO_IRQ_EVT_FALLING_EDGE;
      break;

    case gpio::INTERRUPT_ANY_EDGE:
      event = GPIO_IRQ_EVT_BOTH_EDGE;
      break;

    case gpio::INTERRUPT_LOW_LEVEL:
      event = this->inverted_
                  ? GPIO_IRQ_EVT_HIGH_LEVEL
                  : GPIO_IRQ_EVT_LOW_LEVEL;
      break;

    case gpio::INTERRUPT_HIGH_LEVEL:
      event = this->inverted_
                  ? GPIO_IRQ_EVT_LOW_LEVEL
                  : GPIO_IRQ_EVT_HIGH_LEVEL;
      break;

    default:
      return;
  }

  const auto port = gpio_port(this->pin_);
  const auto pin = gpio_pin(this->pin_);

  GPIO_InitParam config{};
  config.mode = GPIOx_Pn_F6_EINT;
  config.driving = GPIO_DRIVING_LEVEL_1;

  if (has_flag(this->flags_, gpio::FLAG_PULLUP)) {
    config.pull = GPIO_PULL_UP;
  } else if (has_flag(this->flags_, gpio::FLAG_PULLDOWN)) {
    config.pull = GPIO_PULL_DOWN;
  } else {
    config.pull = GPIO_PULL_NONE;
  }

  HAL_GPIO_Init(port, pin, &config);

  GPIO_IrqParam irq{};
  irq.event = event;
  irq.callback = func;
  irq.arg = arg;

  if (HAL_GPIO_EnableIRQ(port, pin, &irq) != HAL_OK) {
    // Restore input mode if IRQ registration failed.
    config.mode = GPIOx_Pn_F0_INPUT;
    HAL_GPIO_Init(port, pin, &config);
    return;
  }

  this->interrupt_attached_ = true;
}

}  // namespace esphome::xr806

namespace esphome {

// ISRInternalGPIOPin stores the address of its owning XR806GPIOPin.
// The owner has static lifetime in ESPHome's generated main.cpp.

bool ISRInternalGPIOPin::digital_read() {
  if (this->arg_ == nullptr) {
    return false;
  }

  const auto *pin =
      static_cast<xr806::XR806GPIOPin *>(this->arg_);

  const uint8_t number = pin->get_pin();

  const GPIO_Port port =
      number >= 32 ? GPIO_PORT_B : GPIO_PORT_A;

  const GPIO_Pin gpio_number =
      static_cast<GPIO_Pin>(number & 0x1F);

  const bool physical =
      HAL_GPIO_ReadPin(port, gpio_number) == GPIO_PIN_HIGH;

  return physical != pin->is_inverted();
}

void ISRInternalGPIOPin::digital_write(bool value) {
  if (this->arg_ == nullptr) {
    return;
  }

  const auto *pin =
      static_cast<xr806::XR806GPIOPin *>(this->arg_);

  const uint8_t number = pin->get_pin();

  const GPIO_Port port =
      number >= 32 ? GPIO_PORT_B : GPIO_PORT_A;

  const GPIO_Pin gpio_number =
      static_cast<GPIO_Pin>(number & 0x1F);

  const bool physical = value != pin->is_inverted();

  HAL_GPIO_WritePin(
      port,
      gpio_number,
      physical ? GPIO_PIN_HIGH : GPIO_PIN_LOW
  );
}

void ISRInternalGPIOPin::clear_interrupt() {
  // XR806 ROM GPIO_IRQHandler clears pending status
  // before invoking the registered callback.
}

void ISRInternalGPIOPin::pin_mode(gpio::Flags flags) {
  if (this->arg_ == nullptr) {
    return;
  }

  auto *pin =
      static_cast<xr806::XR806GPIOPin *>(this->arg_);

  pin->pin_mode(flags);
}

}  // namespace esphome

#endif  // USE_XR806
