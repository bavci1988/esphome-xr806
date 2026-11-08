#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "uart_component_xr806.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_XR806_LOWPOWER
#include "esphome/components/xr806/lowpower_policy.h"
#endif

namespace esphome::uart {

static const char *const TAG = "xr806.uart";

void XR806UARTComponent::setup() {
  if (this->uart_id_ != 0 || this->baud_rate_ == 0 ||
      this->data_bits_ != 8 || this->stop_bits_ != 1 ||
      this->parity_ != UART_CONFIG_PARITY_NONE) {
    ESP_LOGE(TAG, "Unsupported UART configuration");
    this->mark_failed();
    return;
  }

  if (this->rx_buffer_size_ == 0 ||
      this->rx_buffer_size_ > 4096) {
    ESP_LOGE(TAG, "Invalid RX buffer size");
    this->mark_failed();
    return;
  }

  this->rx_buffer_.resize(this->rx_buffer_size_);

  UART_InitParam config{};
  config.baudRate = this->baud_rate_;
  config.parity = UART_PARITY_NONE;
  config.stopBits = UART_STOP_BITS_1;
  config.dataBits = UART_DATA_BITS_8;
  config.isAutoHwFlowCtrl = 0;

  HAL_Status result = HAL_UART_Init(static_cast<UART_ID>(this->uart_id_), &config);

  if (result != HAL_OK) {
    ESP_LOGE(TAG, "HAL_UART_Init failed: %d",
             static_cast<int>(result));
    this->mark_failed();
    return;
  }

  this->uart_ = HAL_UART_GetInstance(static_cast<UART_ID>(this->uart_id_));

  if (this->uart_ == nullptr) {
    ESP_LOGE(TAG, "UART0 instance unavailable");
    HAL_UART_DeInit(static_cast<UART_ID>(this->uart_id_));
    this->mark_failed();
    return;
  }

  this->initialized_ = true;

#ifdef USE_XR806_LOWPOWER
  this->note_uart_activity_();
#endif
  ESP_LOGI(TAG, "UART0 initialized: %u baud",
           static_cast<unsigned>(this->baud_rate_));
}

void XR806UARTComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 UART0:");
  ESP_LOGCONFIG(TAG, "  Baud rate: %u",
                static_cast<unsigned>(this->baud_rate_));
  ESP_LOGCONFIG(TAG, "  RX buffer: %u bytes",
                static_cast<unsigned>(this->rx_buffer_.size()));
  ESP_LOGCONFIG(TAG, "  Dropped RX bytes: %u",
                static_cast<unsigned>(this->rx_overflows_));
}

void XR806UARTComponent::check_logger_conflict() {
  // The SDK console uses UART2; this backend owns UART0 only.
}


#ifdef USE_XR806_LOWPOWER

void XR806UARTComponent::note_uart_activity_() {
  this->last_uart_activity_ms_ = millis();
  esphome::xr806::lowpower_policy().set_uart_idle(false);
}

void XR806UARTComponent::update_uart_idle_() {
  if (!this->initialized_ || this->uart_ == nullptr) {
    esphome::xr806::lowpower_policy().set_uart_idle(false);
    return;
  }

  const bool buffer_empty = this->rx_count_ == 0;
  const bool rx_fifo_empty = !HAL_UART_IsRxReady(this->uart_);
  const bool tx_fifo_empty = HAL_UART_IsTxEmpty(this->uart_);

  const uint32_t elapsed =
      static_cast<uint32_t>(
          millis() - this->last_uart_activity_ms_);

  const bool idle =
      buffer_empty &&
      rx_fifo_empty &&
      tx_fifo_empty &&
      elapsed >= UART_IDLE_GUARD_MS;

  esphome::xr806::lowpower_policy().set_uart_idle(idle);
}

#endif  // USE_XR806_LOWPOWER

void XR806UARTComponent::fill_rx_() {
  if (!this->initialized_ || this->uart_ == nullptr ||
      this->rx_buffer_.empty()) {
    return;
  }

#ifdef USE_XR806_LOWPOWER
  bool saw_rx = false;
#endif

  // Bound work per call to avoid starving the ESPHome main loop.
  for (size_t i = 0;
       i < 512 && HAL_UART_IsRxReady(this->uart_);
       ++i) {
    uint8_t byte = HAL_UART_GetRxData(this->uart_);

#ifdef USE_XR806_LOWPOWER
    saw_rx = true;
#endif

    if (this->rx_count_ == this->rx_buffer_.size()) {
      ++this->rx_overflows_;
      continue;
    }

    this->rx_buffer_[this->rx_tail_] = byte;
    this->rx_tail_ =
        (this->rx_tail_ + 1) % this->rx_buffer_.size();
    ++this->rx_count_;
  }

#ifdef USE_XR806_LOWPOWER
  if (saw_rx)
    this->note_uart_activity_();
#endif
}

void XR806UARTComponent::loop() {
  this->fill_rx_();

#ifdef USE_XR806_LOWPOWER
  this->update_uart_idle_();
#endif
}

size_t XR806UARTComponent::available() {
  this->fill_rx_();
  return this->rx_count_;
}

bool XR806UARTComponent::peek_byte(uint8_t *data) {
  if (data == nullptr)
    return false;

  this->fill_rx_();

  if (this->rx_count_ == 0)
    return false;

  *data = this->rx_buffer_[this->rx_head_];
  return true;
}

bool XR806UARTComponent::read_array(uint8_t *data, size_t len) {
  if (len == 0)
    return true;

  if (data == nullptr)
    return false;

  this->fill_rx_();

  // Do not consume a partial frame on insufficient data.
  if (this->rx_count_ < len)
    return false;

  for (size_t i = 0; i < len; ++i) {
    data[i] = this->rx_buffer_[this->rx_head_];
    this->rx_head_ =
        (this->rx_head_ + 1) % this->rx_buffer_.size();
    --this->rx_count_;
  }

  return true;
}

void XR806UARTComponent::write_array(
    const uint8_t *data, size_t len) {
  if (!this->initialized_ || this->uart_ == nullptr ||
      data == nullptr) {
    return;
  }

#ifdef USE_XR806_LOWPOWER
  if (len != 0)
    this->note_uart_activity_();
#endif

  for (size_t i = 0; i < len; ++i) {
    const uint32_t start = millis();

    while (!HAL_UART_IsTxReady(this->uart_)) {
      if (static_cast<uint32_t>(millis() - start) >= 20) {
        ESP_LOGE(TAG, "UART0 TX FIFO timeout");
        return;
      }
    }

    HAL_UART_PutTxData(this->uart_, data[i]);
  }
}

UARTFlushResult XR806UARTComponent::flush() {
  if (!this->initialized_ || this->uart_ == nullptr)
    return UARTFlushResult::UART_FLUSH_RESULT_FAILED;

  const uint32_t start = millis();

  while (!HAL_UART_IsTxEmpty(this->uart_)) {
    if (static_cast<uint32_t>(millis() - start) >= 100)
      return UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
  }

  return UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
}

}  // namespace esphome::uart

#endif  // USE_XR806
