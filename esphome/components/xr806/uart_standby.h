#pragma once

#include "esphome/core/defines.h"

#ifdef USE_XR806_STANDBY_CONTROLLER

#include "esphome/components/uart/uart_component_libretiny.h"
#include "esphome/core/component.h"
#include "uart_idle_guard.h"

namespace esphome::xr806 {

/// Standby owner of the UART bus (UART_BUSY).
///
/// Holds a PM wakelock while the UART sends or receives, and votes idle once the hardware has
/// been idle (nothing in the TX FIFO or shift register, nothing received, no line activity) for
/// the guard time.
class XR806UARTStandby final : public Component {
 public:
  explicit XR806UARTStandby(uart::LibreTinyUARTComponent *uart) : uart_(uart) {}

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::BUS - 1.0f; }

 protected:
  void set_busy_();

  uart::LibreTinyUARTComponent *uart_;
  void *lock_{nullptr};
  uint32_t activity_{0};
  UARTIdleGuard idle_guard_{};
  uint8_t port_{0};
  bool lock_held_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_CONTROLLER
