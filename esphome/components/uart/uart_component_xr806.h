#pragma once

#include "esphome/core/defines.h"

#ifdef USE_XR806

#include <cstddef>
#include <cstdint>
#include <memory>

#include "esphome/core/component.h"
#include "esphome/components/uart/uart_component.h"
#ifdef USE_XR806_LOWPOWER
#include "esphome/components/xr806/uart_idle_guard.h"
#endif

extern "C" {
#include "driver/chip/hal_uart.h"
#ifdef USE_XR806_LOWPOWER
#include "xr_config.h"
#include "pm/pm.h"
#endif
}

namespace esphome::uart {

/// UART0 (PB0/PB1) of the XR806. An RX interrupt moves received bytes into a software ring
/// buffer, so a frame longer than the 64-byte hardware FIFO survives a slow main loop.
class XR806UARTComponent final : public UARTComponent, public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  /// Applies the current baud rate and frame format to the running UART.
  /// TX data is sent first and RX data received at the old settings is discarded.
  /// On failure the UART reports is_connected() == false.
  void load_settings(bool dump_config) override;
  using UARTComponent::load_settings;

  /// False after a TX timeout or a failed reconfiguration; the fault is not cleared.
  bool is_connected() override { return this->initialized_ && !this->tx_fault_; }
  /// Monotonic count of dropped RX bytes and RX line errors. A change tells a framed
  /// protocol to discard its in-flight state and resynchronize.
  uint32_t get_rx_error_count() const {
    return __atomic_load_n(&this->rx_dropped_, __ATOMIC_RELAXED) +
           __atomic_load_n(&this->rx_line_errors_, __ATOMIC_RELAXED);
  }

  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override;
  UARTFlushResult flush() override;

 protected:
  void check_logger_conflict() override;

  UART_InitParam make_init_param_() const;
  void fill_rx_();
  static void rx_isr_(void *arg);
  void discard_rx_();
  void check_line_status_();
  bool tx_hardware_idle_();
  void set_tx_fault_();

  UART_T *uart_{nullptr};
  std::unique_ptr<uint8_t[]> rx_buffer_;
  // Single producer (the RX interrupt, or fill_rx_() without it) and single consumer (the
  // main loop): the producer owns rx_tail_, the consumer rx_head_, and rx_count_ hands bytes over.
  // Shared fields use __atomic builtins, which always inline: the interrupt must not call a
  // std::atomic member that the linker may have placed in XIP flash.
  size_t rx_head_{0};
  size_t rx_tail_{0};
  size_t rx_count_{0};
  uint32_t rx_dropped_{0};
  uint32_t rx_line_errors_{0};
  bool rx_activity_{false};
  bool rx_irq_{false};
  uint32_t rx_errors_logged_{0};
  uint32_t last_error_log_ms_{0};
  bool initialized_{false};
  bool tx_fault_{false};

#ifdef USE_XR806_LOWPOWER
  void note_uart_activity_();
  void update_uart_idle_();

  struct wakelock uart_pm_wakelock_{};
  bool uart_pm_wakelock_held_{false};
  xr806::UARTIdleGuard idle_guard_{};
#endif
};

}  // namespace esphome::uart

#endif  // USE_XR806
