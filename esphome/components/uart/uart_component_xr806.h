#pragma once

#include "esphome/core/defines.h"

#ifdef USE_XR806

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/uart/uart_component.h"

extern "C" {
#include "driver/chip/hal_uart.h"
}

namespace esphome::uart {

class XR806UARTComponent final : public UARTComponent, public Component {
 public:
  using UARTComponent::write_array;

  void set_uart_id(uint8_t id) { this->uart_id_ = id; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  float get_setup_priority() const override {
    return setup_priority::BUS;
  }

  void write_array(const uint8_t *data, size_t len) override;
  bool peek_byte(uint8_t *data) override;
  bool read_array(uint8_t *data, size_t len) override;
  size_t available() override;
  UARTFlushResult flush() override;

 protected:
  void check_logger_conflict() override;

 private:
  void fill_rx_();

#ifdef USE_XR806_LOWPOWER
  void note_uart_activity_();
  void update_uart_idle_();
#endif

  uint8_t uart_id_{0};
  UART_T *uart_{nullptr};
  bool initialized_{false};

  std::vector<uint8_t> rx_buffer_;
  size_t rx_head_{0};
  size_t rx_tail_{0};
  size_t rx_count_{0};
  uint32_t rx_overflows_{0};

#ifdef USE_XR806_LOWPOWER
  uint32_t last_uart_activity_ms_{0};
  static constexpr uint32_t UART_IDLE_GUARD_MS = 200;
#endif
};

}  // namespace esphome::uart

#endif  // USE_XR806
