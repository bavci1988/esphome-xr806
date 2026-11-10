#ifdef USE_XR806
#include "logger.h"

#include "esphome/core/hal.h"

extern "C" {
#include "driver/chip/hal_uart.h"
}

namespace esphome::logger {

ESPHOME_LOG_TAG(TAG, "logger");

// A stuck TX FIFO drops the rest of the line instead of stalling the main loop.
static constexpr uint32_t TX_READY_TIMEOUT_MS = 20;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static UART_T *log_uart = nullptr;

void Logger::pre_setup() {
  // The WXU console (UART2 on pads 6 and 7) is the only logger UART; UART0 belongs to the uart component.
  if (this->baud_rate_ > 0) {
    UART_InitParam param{};
    param.baudRate = this->baud_rate_;
    param.parity = UART_PARITY_NONE;
    param.stopBits = UART_STOP_BITS_1;
    param.dataBits = UART_DATA_BITS_8;
    param.isAutoHwFlowCtrl = 0;
    if (HAL_UART_Init(UART2_ID, &param) == HAL_OK)
      log_uart = HAL_UART_GetInstance(UART2_ID);
  }
  global_logger = this;
  ESP_LOGI(TAG, "Log initialized");
}

void HOT Logger::write_msg_(const char *msg, uint16_t len) {
  if (log_uart == nullptr)
    return;
  for (uint16_t i = 0; i < len; i++) {
    if (!HAL_UART_IsTxReady(log_uart)) {
      const uint32_t start = millis();
      while (!HAL_UART_IsTxReady(log_uart)) {
        if (millis() - start >= TX_READY_TIMEOUT_MS)
          return;
      }
    }
    HAL_UART_PutTxData(log_uart, static_cast<uint8_t>(msg[i]));
  }
}

// Config validation allows only UART2, the WXU console.
const LogString *Logger::get_uart_selection_() { return LOG_STR("UART2"); }

}  // namespace esphome::logger

#endif  // USE_XR806
