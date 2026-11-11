#ifdef USE_XR806

#include "gpio.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

extern "C" {
#include "driver/chip/hal_gpio.h"
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806");

// The helpers below run from ISRInternalGPIOPin, which lives in SRAM; force them inline so no call lands in
// flash. The HAL_GPIO_* functions they call are in ROM.

static ESPHOME_ALWAYS_INLINE inline GPIO_Port gpio_port(uint8_t pin) { return pin >= 32 ? GPIO_PORT_B : GPIO_PORT_A; }

static ESPHOME_ALWAYS_INLINE inline GPIO_Pin gpio_pin(uint8_t pin) { return static_cast<GPIO_Pin>(pin & 0x1F); }

static ESPHOME_ALWAYS_INLINE inline GPIO_WorkMode flags_to_mode(gpio::Flags flags) {
  // Reading the data register of an output returns the driven level, so input + output is an output.
  if (flags & gpio::FLAG_OUTPUT)
    return GPIOx_Pn_F1_OUTPUT;
  if (flags & gpio::FLAG_INPUT)
    return GPIOx_Pn_F0_INPUT;
  return GPIOx_Pn_F7_DISABLE;
}

// The pull resistor is independent of the pin function, so this keeps the pull of the configured flags.
static ESPHOME_ALWAYS_INLINE inline void configure(uint8_t pin, GPIO_WorkMode mode, gpio::Flags flags) {
  GPIO_InitParam param{};
  param.mode = mode;
  param.driving = GPIO_DRIVING_LEVEL_1;
  if (flags & gpio::FLAG_PULLUP) {
    param.pull = GPIO_PULL_UP;
  } else if (flags & gpio::FLAG_PULLDOWN) {
    param.pull = GPIO_PULL_DOWN;
  } else {
    param.pull = GPIO_PULL_NONE;
  }
  HAL_GPIO_Init(gpio_port(pin), gpio_pin(pin), &param);
}

static ESPHOME_ALWAYS_INLINE inline bool read_level(uint8_t pin) {
  return HAL_GPIO_ReadPin(gpio_port(pin), gpio_pin(pin)) == GPIO_PIN_HIGH;
}

static ESPHOME_ALWAYS_INLINE inline void write_level(uint8_t pin, bool level) {
  HAL_GPIO_WritePin(gpio_port(pin), gpio_pin(pin), level ? GPIO_PIN_HIGH : GPIO_PIN_LOW);
}

ISRInternalGPIOPin XR806GPIOPin::to_isr() const { return ISRInternalGPIOPin(this); }

void XR806GPIOPin::attach_interrupt_(void (*func)(void *), void *arg, gpio::InterruptType type) const {
  this->interrupt_attached_ = false;
  if (func == nullptr)
    return;
  GPIO_IrqEvent event;
  switch (type) {
    case gpio::INTERRUPT_RISING_EDGE:
      event = this->inverted_ ? GPIO_IRQ_EVT_FALLING_EDGE : GPIO_IRQ_EVT_RISING_EDGE;
      break;
    case gpio::INTERRUPT_FALLING_EDGE:
      event = this->inverted_ ? GPIO_IRQ_EVT_RISING_EDGE : GPIO_IRQ_EVT_FALLING_EDGE;
      break;
    case gpio::INTERRUPT_ANY_EDGE:
      event = GPIO_IRQ_EVT_BOTH_EDGE;
      break;
    case gpio::INTERRUPT_LOW_LEVEL:
      event = this->inverted_ ? GPIO_IRQ_EVT_HIGH_LEVEL : GPIO_IRQ_EVT_LOW_LEVEL;
      break;
    case gpio::INTERRUPT_HIGH_LEVEL:
      event = this->inverted_ ? GPIO_IRQ_EVT_LOW_LEVEL : GPIO_IRQ_EVT_HIGH_LEVEL;
      break;
    default:
      return;
  }

  // The pin must be in its external interrupt function for the controller to see edges and levels.
  configure(this->pin_, GPIOx_Pn_F6_EINT, this->flags_);

  GPIO_IrqParam irq{};
  irq.event = event;
  irq.callback = func;
  irq.arg = arg;
  if (HAL_GPIO_EnableIRQ(gpio_port(this->pin_), gpio_pin(this->pin_), &irq) != HAL_OK) {
    configure(this->pin_, flags_to_mode(this->flags_), this->flags_);
    ESP_LOGE(TAG, "Cannot attach interrupt to pin %u", this->pin_);
    return;
  }
  this->interrupt_attached_ = true;
}

void XR806GPIOPin::detach_interrupt() const {
  HAL_GPIO_DisableIRQ(gpio_port(this->pin_), gpio_pin(this->pin_));
  this->interrupt_attached_ = false;
  configure(this->pin_, flags_to_mode(this->flags_), this->flags_);
}

void XR806GPIOPin::pin_mode(gpio::Flags flags) {
  this->flags_ = flags;
  configure(this->pin_, flags_to_mode(flags), flags);
}

size_t XR806GPIOPin::dump_summary(char *buffer, size_t len) const {
  return buf_append_printf(buffer, len, 0, "P%c%u%s", this->pin_ >= 32 ? 'B' : 'A',
                           static_cast<unsigned>(this->pin_ & 0x1F), this->inverted_ ? " (inverted)" : "");
}

bool XR806GPIOPin::digital_read() { return read_level(this->pin_) != this->inverted_; }

void XR806GPIOPin::digital_write(bool value) { write_level(this->pin_, value != this->inverted_); }

}  // namespace esphome::xr806

namespace esphome {

using namespace xr806;

bool IRAM_ATTR ISRInternalGPIOPin::digital_read() {
  auto *pin = static_cast<XR806GPIOPin *>(this->arg_);
  return read_level(pin->get_pin()) != pin->is_inverted();
}

void IRAM_ATTR ISRInternalGPIOPin::digital_write(bool value) {
  auto *pin = static_cast<XR806GPIOPin *>(this->arg_);
  write_level(pin->get_pin(), value != pin->is_inverted());
}

void IRAM_ATTR ISRInternalGPIOPin::clear_interrupt() {
  // The ROM GPIO interrupt handler clears the pending bit before it calls the callback.
}

void IRAM_ATTR ISRInternalGPIOPin::pin_mode(gpio::Flags flags) {
  auto *pin = static_cast<XR806GPIOPin *>(this->arg_);
  // Share the current mode with normal-context clients, including IRQ detach/restore.
  pin->set_flags(flags);
  configure(pin->get_pin(), flags_to_mode(flags), flags);
}

}  // namespace esphome

#endif  // USE_XR806
