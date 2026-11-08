#pragma once

#ifdef USE_XR806

#include "esphome/core/gpio_pin.h"

namespace esphome::xr806 {

class XR806GPIOPin final : public GPIOPin {
 public:
  void set_pin(uint8_t pin) { pin_ = pin; }
  void set_inverted(bool inverted) { inverted_ = inverted; }
  void set_flags(gpio::Flags flags) { flags_ = flags; }

  void setup() override { pin_mode(flags_); }
  void pin_mode(gpio::Flags flags) override;
  gpio::Flags get_flags() const override { return flags_; }

  bool digital_read() override;
  void digital_write(bool value) override;

  size_t dump_summary(char *buffer, size_t len) const override;

  uint8_t get_pin() const { return pin_; }
  bool is_inverted() const { return inverted_; }
  bool is_internal() override { return true; }

  ISRInternalGPIOPin to_isr() const;

  void detach_interrupt() const;

  // XR806_IRQ_FALLBACK_V1
  bool interrupt_attached() const { return interrupt_attached_; }

  template<typename T>
  void attach_interrupt(void (*func)(T *), T *arg,
                        gpio::InterruptType type) const {
    this->attach_interrupt_(
        reinterpret_cast<void (*)(void *)>(func), arg, type);
  }

 protected:
  void attach_interrupt_(void (*func)(void *), void *arg,
                         gpio::InterruptType type) const;

  uint8_t pin_{};
  bool inverted_{};
  gpio::Flags flags_{};
  mutable bool interrupt_attached_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806
