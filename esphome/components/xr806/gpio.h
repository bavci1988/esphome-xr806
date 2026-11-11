#pragma once

#ifdef USE_XR806

#include "esphome/core/gpio_pin.h"

namespace esphome::xr806 {

/// Native XR806 GPIO. The pin number uses the encoding from gpio.py: PAx = x, PBx = 32 + x.
class XR806GPIOPin final : public GPIOPin {
 public:
  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  ESPHOME_ALWAYS_INLINE void set_flags(gpio::Flags flags) { this->flags_ = flags; }

  void setup() override { this->pin_mode(this->flags_); }
  void pin_mode(gpio::Flags flags) override;
  bool digital_read() override;
  void digital_write(bool value) override;
  size_t dump_summary(char *buffer, size_t len) const override;
  void detach_interrupt() const;
  bool interrupt_attached() const { return this->interrupt_attached_; }
  template<typename T> void attach_interrupt(void (*func)(T *), T *arg, gpio::InterruptType type) const {
    this->attach_interrupt_(reinterpret_cast<void (*)(void *)>(func), arg, type);
  }
  ISRInternalGPIOPin to_isr() const;
  ESPHOME_ALWAYS_INLINE uint8_t get_pin() const { return this->pin_; }
  gpio::Flags get_flags() const override { return this->flags_; }
  ESPHOME_ALWAYS_INLINE bool is_inverted() const { return this->inverted_; }
  bool is_internal() override { return true; }

 protected:
  void attach_interrupt_(void (*func)(void *), void *arg, gpio::InterruptType type) const;

  uint8_t pin_{};
  bool inverted_{};
  gpio::Flags flags_{};
  mutable bool interrupt_attached_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806
