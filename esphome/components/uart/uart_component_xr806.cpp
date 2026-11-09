#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "uart_component_xr806.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_XR806_LOWPOWER
#include "esphome/components/xr806/lowpower_policy.h"
#endif

namespace esphome::uart {

ESPHOME_LOG_TAG(TAG, "xr806.uart");

void XR806UARTComponent::setup() {
  if (this->uart_id_ != 0 || this->baud_rate_ == 0 || this->data_bits_ != 8 || this->stop_bits_ != 1 ||
      this->parity_ != UART_CONFIG_PARITY_NONE) {
    ESP_LOGE(TAG, "Unsupported UART configuration");
    this->mark_failed();
    return;
  }

  if (this->rx_buffer_size_ == 0 || this->rx_buffer_size_ > 4096) {
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
    ESP_LOGE(TAG, "HAL_UART_Init failed: %d", static_cast<int>(result));
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
  this->uart_pm_wakelock_.name = const_cast<char *>("esphome.xr806.uart0");
  this->note_uart_activity_();
#endif
  ESP_LOGI(TAG, "UART0 initialized: %u baud", static_cast<unsigned>(this->baud_rate_));
}

// The vendor HAL's HAL_UART_IsTxEmpty() checks only the FIFO. A shift
// register may still be emitting the final UART frame on the physical line.
// XR806 has a 16550-compatible LSR transmitter-empty flag and BUSY status.
bool XR806UARTComponent::tx_hardware_idle_() const {
  return this->initialized_ && this->uart_ != nullptr && HAL_UART_IsTxEmpty(this->uart_) &&
         (this->uart_->LINE_STATUS & UART_TX_EMPTY_BIT) != 0;
}

void XR806UARTComponent::set_baud_probe_mode(bool probing) {
  if (this->baud_probe_mode_ == probing)
    return;
  this->baud_probe_mode_ = probing;
  if (!probing && this->initialized_ && this->uart_ != nullptr) {
    // Read line status to clear wrong-rate receive errors after a
    // checksum-valid CMD01 response. The protocol owns baud selection.
    (void) this->uart_->LINE_STATUS;
  }
#ifdef USE_XR806_LOWPOWER
  this->note_uart_activity_();
#endif
}

bool XR806UARTComponent::reconfigure_baud_rate(uint32_t baud_rate) {
  if (!this->initialized_ || this->uart_ == nullptr || this->uart_id_ != 0 || this->tx_fault_ ||
      this->rx_overflow_fault_)
    return false;
  if (baud_rate != 9600 && baud_rate != 115200)
    return false;
  if (this->flush() != UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
    ESP_LOGE(TAG, "UART0 baud change refused: untransmitted TX data");
    return false;
  }
#ifdef USE_XR806_LOWPOWER
  // Claim the native SDK PM wakelock BEFORE changing a live UART clock.
  this->note_uart_activity_();
  if (this->tx_fault_)
    return false;
#endif
  if (baud_rate != this->baud_rate_) {
    UART_InitParam cfg{};
    cfg.baudRate = baud_rate;
    cfg.parity = UART_PARITY_NONE;
    cfg.stopBits = UART_STOP_BITS_1;
    cfg.dataBits = UART_DATA_BITS_8;
    cfg.isAutoHwFlowCtrl = 0;
    if (HAL_UART_SetConfig(static_cast<UART_ID>(this->uart_id_), &cfg) != HAL_OK) {
      this->tx_fault_ = true;
#ifdef USE_XR806_LOWPOWER
      esphome::xr806::lowpower_policy().set_uart_idle(false);
#endif
      ESP_LOGE(TAG, "UART0 baud reconfiguration failed at %u", static_cast<unsigned>(baud_rate));
      return false;
    }
    this->baud_rate_ = baud_rate;
  }
  // A wrong-rate CMD01 probe can have left partial hardware/software RX.
  this->rx_head_ = this->rx_tail_ = this->rx_count_ = 0;
  for (size_t i = 0; i < 512 && HAL_UART_IsRxReady(this->uart_); ++i)
    (void) HAL_UART_GetRxData(this->uart_);
#ifdef USE_XR806_LOWPOWER
  this->note_uart_activity_();
#endif
  return true;
}

void XR806UARTComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 UART0:");
  ESP_LOGCONFIG(TAG, "  Baud rate: %u", static_cast<unsigned>(this->baud_rate_));
  ESP_LOGCONFIG(TAG, "  RX buffer: %u bytes", static_cast<unsigned>(this->rx_buffer_.size()));
  ESP_LOGCONFIG(TAG, "  Dropped RX bytes: %u", static_cast<unsigned>(this->rx_overflows_));
}

void XR806UARTComponent::check_logger_conflict() {
  // The SDK console uses UART2; this backend owns UART0 only.
}

#ifdef USE_XR806_LOWPOWER

void XR806UARTComponent::note_uart_activity_() {
  // Physical UART traffic must not begin until the native PM driver has
  // acknowledged its wakelock. A software-only idle vote cannot stop a
  // concurrent vendor PM thread that has already started admission.
  if (!this->uart_pm_wakelock_held_) {
    if (pm_wake_lock(&this->uart_pm_wakelock_) != 0) {
      this->tx_fault_ = true;
      esphome::xr806::lowpower_policy().set_uart_idle(false);
      ESP_LOGE(TAG, "Unable to claim native UART0 PM wakelock; standby disabled");
      this->mark_failed();
      return;
    }
    this->uart_pm_wakelock_held_ = true;
  }
  esphome::xr806::lowpower_policy().set_uart_idle(false);
  this->last_uart_activity_ms_ = millis();
}

void XR806UARTComponent::update_uart_idle_() {
  if (!this->initialized_ || this->uart_ == nullptr) {
    esphome::xr806::lowpower_policy().set_uart_idle(false);
    return;
  }

  const bool buffer_empty = this->rx_count_ == 0;
  const bool rx_fifo_empty = !HAL_UART_IsRxReady(this->uart_);
  const bool tx_completed_on_wire = this->tx_hardware_idle_();
  const uint32_t line_status = this->uart_->LINE_STATUS;
  if (line_status & (UART_FIFO_ERROR_BIT | UART_FRAME_ERROR_BIT | UART_PARITY_ERROR_BIT | UART_OVERRUN_ERROR_BIT)) {
    if (this->baud_probe_mode_) {
      ++this->ignored_probe_line_errors_;  // Invalid baud candidates are expected.
    } else if (!this->rx_overflow_fault_) {
      this->rx_overflow_fault_ = true;
      ESP_LOGE(TAG, "UART0 line-status fault 0x%02lx; PM blocked", static_cast<unsigned long>(line_status));
    }
  }
  const bool peripheral_idle = (this->uart_->STATUS & UART_BUSY_BIT) == 0;

  const uint32_t elapsed = static_cast<uint32_t>(millis() - this->last_uart_activity_ms_);

  const bool idle = buffer_empty && rx_fifo_empty && tx_completed_on_wire && peripheral_idle && !this->tx_fault_ &&
                    !this->rx_overflow_fault_ && !this->baud_probe_mode_ && elapsed >= UART_IDLE_GUARD_MS;

  if (idle && this->uart_pm_wakelock_held_) {
    if (pm_wake_unlock(&this->uart_pm_wakelock_) != 0) {
      this->tx_fault_ = true;
      ESP_LOGE(TAG, "UART0 native PM wakelock release failed; PM blocked");
      esphome::xr806::lowpower_policy().set_uart_idle(false);
      return;
    }
    this->uart_pm_wakelock_held_ = false;
  } else if (!idle && !this->uart_pm_wakelock_held_) {
    this->note_uart_activity_();
  }
  esphome::xr806::lowpower_policy().set_uart_idle(idle && !this->tx_fault_);
}

#endif  // USE_XR806_LOWPOWER

void XR806UARTComponent::fill_rx_() {
  if (!this->initialized_ || this->uart_ == nullptr || this->rx_buffer_.empty()) {
    return;
  }

#ifdef USE_XR806_LOWPOWER
  bool saw_rx = false;
#endif

  // Bound work per call to avoid starving the ESPHome main loop.
  for (size_t i = 0; i < 512 && HAL_UART_IsRxReady(this->uart_); ++i) {
#ifdef USE_XR806_LOWPOWER
    if (!saw_rx) {
      // Claim the SDK PM wakelock before consuming even the first RX byte.
      // The HC32 wake GPIO is independent and not proof that UART is idle.
      this->note_uart_activity_();
      if (this->tx_fault_)
        return;
      saw_rx = true;
    }
#endif
    uint8_t byte = HAL_UART_GetRxData(this->uart_);

    if (this->rx_count_ == this->rx_buffer_.size()) {
      ++this->rx_overflows_;
      if (!this->baud_probe_mode_)
        this->rx_overflow_fault_ = true;
      // A truncated packet is never synchronized. During physical baud
      // probing the wrong-rate byte stream can legitimately overflow RX.
      this->rx_head_ = this->rx_tail_ = this->rx_count_ = 0;
      continue;
    }

    this->rx_buffer_[this->rx_tail_] = byte;
    this->rx_tail_ = (this->rx_tail_ + 1) % this->rx_buffer_.size();
    ++this->rx_count_;
  }
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
    this->rx_head_ = (this->rx_head_ + 1) % this->rx_buffer_.size();
    --this->rx_count_;
  }

  return true;
}

void XR806UARTComponent::write_array(const uint8_t *data, size_t len) {
  if (len == 0)
    return;
  if (!this->initialized_ || this->uart_ == nullptr || data == nullptr) {
    this->tx_fault_ = true;
#ifdef USE_XR806_LOWPOWER
    esphome::xr806::lowpower_policy().set_uart_idle(false);
#endif
    return;
  }
  if (this->tx_fault_)
    return;

#ifdef USE_XR806_LOWPOWER
  this->note_uart_activity_();
  // An SDK PM wakelock acquisition failure is a hard transport fault:
  // NEVER push even the first TX byte when the native lock was refused.
  if (this->tx_fault_ || !this->uart_pm_wakelock_held_)
    return;
#endif

  for (size_t i = 0; i < len; ++i) {
    const uint32_t start = millis();

    while (!HAL_UART_IsTxReady(this->uart_)) {
      if (static_cast<uint32_t>(millis() - start) >= 20) {
        this->tx_fault_ = true;
#ifdef USE_XR806_LOWPOWER
        esphome::xr806::lowpower_policy().set_uart_idle(false);
#endif
        ESP_LOGE(TAG, "UART0 TX FIFO timeout (transport unhealthy, PM blocked)");
        return;
      }
    }

    HAL_UART_PutTxData(this->uart_, data[i]);
  }
}

UARTFlushResult XR806UARTComponent::flush() {
  if (!this->initialized_ || this->uart_ == nullptr || this->tx_fault_ || this->rx_overflow_fault_)
    return UARTFlushResult::UART_FLUSH_RESULT_FAILED;

  const uint32_t start = millis();
  while (!this->tx_hardware_idle_()) {
    if (static_cast<uint32_t>(millis() - start) >= 100) {
      this->tx_fault_ = true;
#ifdef USE_XR806_LOWPOWER
      esphome::xr806::lowpower_policy().set_uart_idle(false);
#endif
      ESP_LOGE(TAG, "UART0 flush timed out before TX shifter was empty; PM blocked");
      return UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
    }
  }
  return UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
}

}  // namespace esphome::uart

#endif  // USE_XR806
