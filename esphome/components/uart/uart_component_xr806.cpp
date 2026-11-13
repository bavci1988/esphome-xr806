#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "uart_component_xr806.h"

#include <cinttypes>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_XR806_LOWPOWER
#include "esphome/components/xr806/lowpower_policy.h"
#endif

namespace esphome::uart {

ESPHOME_LOG_TAG(TAG, "uart.xr806");

// The SDK console uses UART2 and UART1 shares pins with flash, so the backend drives UART0.
static constexpr UART_ID UART_PORT = UART0_ID;
// Bounds the bytes moved from the hardware FIFO per call, so a fast sender cannot starve the main loop.
static constexpr size_t MAX_RX_BYTES_PER_CALL = 512;
static constexpr uint32_t TX_READY_TIMEOUT_MS = 20;
static constexpr uint32_t FLUSH_TIMEOUT_MS = 100;
static constexpr uint32_t ERROR_LOG_INTERVAL_MS = 5000;
#ifdef USE_XR806_LOWPOWER
// Quiet time after the last UART activity before the UART allows standby.
static constexpr uint32_t UART_IDLE_GUARD_MS = 200;
#endif
static constexpr uint32_t LINE_ERROR_BITS =
    UART_FIFO_ERROR_BIT | UART_FRAME_ERROR_BIT | UART_PARITY_ERROR_BIT | UART_OVERRUN_ERROR_BIT;

UART_InitParam XR806UARTComponent::make_init_param_() const {
  UART_InitParam param{};
  param.baudRate = this->baud_rate_;
  switch (this->data_bits_) {
    case 5:
      param.dataBits = UART_DATA_BITS_5;
      break;
    case 6:
      param.dataBits = UART_DATA_BITS_6;
      break;
    case 7:
      param.dataBits = UART_DATA_BITS_7;
      break;
    default:
      param.dataBits = UART_DATA_BITS_8;
      break;
  }
  switch (this->parity_) {
    case UART_CONFIG_PARITY_EVEN:
      param.parity = UART_PARITY_EVEN;
      break;
    case UART_CONFIG_PARITY_ODD:
      param.parity = UART_PARITY_ODD;
      break;
    default:
      param.parity = UART_PARITY_NONE;
      break;
  }
  param.stopBits = this->stop_bits_ == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  param.isAutoHwFlowCtrl = 0;
  return param;
}

void XR806UARTComponent::setup() {
  this->rx_buffer_ = std::make_unique<uint8_t[]>(this->rx_buffer_size_);

  const UART_InitParam param = this->make_init_param_();
  if (HAL_UART_Init(UART_PORT, &param) != HAL_OK) {
    this->mark_failed(LOG_STR("UART0 init failed"));
    return;
  }
  this->uart_ = HAL_UART_GetInstance(UART_PORT);
  if (this->uart_ == nullptr) {
    HAL_UART_DeInit(UART_PORT);
    this->mark_failed(LOG_STR("UART0 instance unavailable"));
    return;
  }
  this->initialized_ = true;
  // Without the interrupt the loop polls the FIFO, which loses bytes once a frame outgrows it.
  this->rx_irq_ = HAL_UART_EnableRxCallback(UART_PORT, &XR806UARTComponent::rx_isr_, this) == HAL_OK;

#ifdef USE_XR806_LOWPOWER
  this->uart_pm_wakelock_.name = const_cast<char *>("esphome.uart0");
  this->note_uart_activity_();
#endif
}

void XR806UARTComponent::load_settings(bool dump_config) {
  if (!this->initialized_)
    return;  // setup() applies the settings.
#ifdef USE_XR806_LOWPOWER
  // Hold the native PM wakelock throughout TX drain and clock reconfiguration.
  this->note_uart_activity_();
  if (this->tx_fault_)
    return;
#endif
  if (this->flush() != UARTFlushResult::UART_FLUSH_RESULT_SUCCESS) {
    ESP_LOGE(TAG, "UART0 settings not applied: TX did not complete");
    return;
  }
  const UART_InitParam param = this->make_init_param_();
  if (HAL_UART_SetConfig(UART_PORT, &param) != HAL_OK) {
    this->set_tx_fault_();
    ESP_LOGE(TAG, "UART0 reconfiguration to %" PRIu32 " baud failed", this->baud_rate_);
    return;
  }
  if (this->rx_irq_)
    (void) HAL_UART_EnableRxCallback(UART_PORT, &XR806UARTComponent::rx_isr_, this);
  // Bytes received at the previous settings are meaningless now.
  this->discard_rx_();
  if (dump_config) {
    ESP_LOGCONFIG(TAG, "Reloaded UART0");
    this->dump_config();
  }
}

void XR806UARTComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "UART Bus 0:");
  LOG_PIN("  TX Pin: ", this->tx_pin_);
  LOG_PIN("  RX Pin: ", this->rx_pin_);
  if (this->rx_pin_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  RX Buffer Size: %u", static_cast<unsigned>(this->rx_buffer_size_));
  }
  ESP_LOGCONFIG(TAG,
                "  Baud Rate: %" PRIu32 " baud\n"
                "  Data Bits: %u\n"
                "  Parity: %s\n"
                "  Stop bits: %u",
                this->baud_rate_, this->data_bits_, LOG_STR_ARG(parity_to_str(this->parity_)), this->stop_bits_);
  this->check_logger_conflict();
}

void XR806UARTComponent::check_logger_conflict() {
  // The logger is fixed to UART2 on XR806, and its validation rejects UART0.
}

void XR806UARTComponent::set_tx_fault_() {
  this->tx_fault_ = true;
#ifdef USE_XR806_LOWPOWER
  esphome::xr806::lowpower_policy().set_uart_idle(false);
#endif
}

// Reading the line status register clears its error bits, so every read goes through here.
void XR806UARTComponent::check_line_status_() {
  if ((this->uart_->LINE_STATUS & LINE_ERROR_BITS) != 0) {
    __atomic_fetch_add(&this->rx_line_errors_, 1, __ATOMIC_RELAXED);
#ifdef USE_XR806_LOWPOWER
    this->note_uart_activity_();  // Corruption/noise is UART activity, even with an empty RX FIFO.
#endif
  }
}

// HAL_UART_IsTxEmpty() checks only the TX FIFO. The transmitter-empty bit also covers
// the shift register, so the last frame has left the TX line.
bool XR806UARTComponent::tx_hardware_idle_() {
  if (!HAL_UART_IsTxEmpty(this->uart_))
    return false;
  const uint32_t line_status = this->uart_->LINE_STATUS;
  if ((line_status & LINE_ERROR_BITS) != 0) {
    __atomic_fetch_add(&this->rx_line_errors_, 1, __ATOMIC_RELAXED);
#ifdef USE_XR806_LOWPOWER
    this->note_uart_activity_();
#endif
  }
  return (line_status & UART_TX_EMPTY_BIT) != 0;
}

// Interrupt context. This file stays out of XIP (see the linker script), so it also runs
// while flash is busy; no division, no logging, no SDK calls besides the register helpers.
void XR806UARTComponent::rx_isr_(void *arg) {
  auto *self = static_cast<XR806UARTComponent *>(arg);
  if ((self->uart_->LINE_STATUS & LINE_ERROR_BITS) != 0)
    __atomic_fetch_add(&self->rx_line_errors_, 1, __ATOMIC_RELAXED);
  uint8_t *const buffer = self->rx_buffer_.get();
  bool received = false;
  while (HAL_UART_IsRxReady(self->uart_)) {
    const uint8_t byte = HAL_UART_GetRxData(self->uart_);
    received = true;
    if (__atomic_load_n(&self->rx_count_, __ATOMIC_ACQUIRE) >= self->rx_buffer_size_) {
      __atomic_fetch_add(&self->rx_dropped_, 1, __ATOMIC_RELAXED);
      continue;
    }
    buffer[self->rx_tail_] = byte;
    if (++self->rx_tail_ == self->rx_buffer_size_)
      self->rx_tail_ = 0;
    __atomic_fetch_add(&self->rx_count_, 1, __ATOMIC_RELEASE);
  }
  if (received)
    __atomic_store_n(&self->rx_activity_, true, __ATOMIC_RELAXED);
}

void XR806UARTComponent::fill_rx_() {
  if (!this->initialized_)
    return;
  if (this->rx_irq_) {
#ifdef USE_XR806_LOWPOWER
    if (__atomic_exchange_n(&this->rx_activity_, false, __ATOMIC_RELAXED))
      this->note_uart_activity_();
#endif
    return;
  }

  this->check_line_status_();
  for (size_t i = 0; i < MAX_RX_BYTES_PER_CALL && HAL_UART_IsRxReady(this->uart_); ++i) {
#ifdef USE_XR806_LOWPOWER
    if (i == 0) {
      // Hold the PM wakelock before the first byte is taken from the FIFO.
      this->note_uart_activity_();
      if (this->tx_fault_)
        return;
    }
#endif
    const uint8_t byte = HAL_UART_GetRxData(this->uart_);
    if (this->rx_count_ == this->rx_buffer_size_) {
      // The ring is full: drop everything, so the reader never joins two unrelated byte runs.
      this->rx_dropped_ += this->rx_count_ + 1;
      this->rx_head_ = this->rx_tail_ = this->rx_count_ = 0;
      continue;
    }
    this->rx_buffer_[this->rx_tail_] = byte;
    this->rx_tail_ = (this->rx_tail_ + 1) % this->rx_buffer_size_;
    ++this->rx_count_;
  }
}

void XR806UARTComponent::discard_rx_() {
  if (this->rx_irq_) {
    // Consume what the interrupt has queued; it keeps owning rx_tail_.
    const size_t queued = __atomic_load_n(&this->rx_count_, __ATOMIC_ACQUIRE);
    this->rx_head_ = (this->rx_head_ + queued) % this->rx_buffer_size_;
    __atomic_fetch_sub(&this->rx_count_, queued, __ATOMIC_ACQ_REL);
    return;
  }
  this->rx_head_ = this->rx_tail_ = this->rx_count_ = 0;
  for (size_t i = 0; i < MAX_RX_BYTES_PER_CALL && HAL_UART_IsRxReady(this->uart_); ++i)
    HAL_UART_GetRxData(this->uart_);
  this->check_line_status_();
}

void XR806UARTComponent::loop() {
  this->fill_rx_();

  const uint32_t errors = this->get_rx_error_count();
  const uint32_t now = App.get_loop_component_start_time();
  if (errors != this->rx_errors_logged_ && now - this->last_error_log_ms_ >= ERROR_LOG_INTERVAL_MS) {
    // Expected while a protocol probes for the baud rate; framed protocols resynchronize.
    ESP_LOGW(TAG, "RX errors: %" PRIu32 " bytes dropped (buffer full), %" PRIu32 " line errors",
             __atomic_load_n(&this->rx_dropped_, __ATOMIC_RELAXED),
             __atomic_load_n(&this->rx_line_errors_, __ATOMIC_RELAXED));
    this->rx_errors_logged_ = errors;
    this->last_error_log_ms_ = now;
  }

#ifdef USE_XR806_LOWPOWER
  this->update_uart_idle_();
#endif
}

size_t XR806UARTComponent::available() {
  this->fill_rx_();
  return __atomic_load_n(&this->rx_count_, __ATOMIC_ACQUIRE);
}

bool XR806UARTComponent::peek_byte(uint8_t *data) {
  if (!this->check_read_timeout_())
    return false;
  *data = this->rx_buffer_[this->rx_head_];
  return true;
}

bool XR806UARTComponent::read_array(uint8_t *data, size_t len) {
  if (!this->check_read_timeout_(len))
    return false;
  for (size_t i = 0; i < len; ++i) {
    data[i] = this->rx_buffer_[this->rx_head_];
    this->rx_head_ = (this->rx_head_ + 1) % this->rx_buffer_size_;
#ifdef USE_UART_DEBUGGER
    this->debug_callback_.call(UART_DIRECTION_RX, data[i]);
#endif
  }
  __atomic_fetch_sub(&this->rx_count_, len, __ATOMIC_ACQ_REL);
  return true;
}

void XR806UARTComponent::write_array(const uint8_t *data, size_t len) {
  if (len == 0 || !this->is_connected())
    return;

#ifdef USE_XR806_LOWPOWER
  // Never start TX without the native PM wakelock.
  this->note_uart_activity_();
  if (this->tx_fault_)
    return;
#endif

  for (size_t i = 0; i < len; ++i) {
    const uint32_t start = millis();
    while (!HAL_UART_IsTxReady(this->uart_)) {
      if (millis() - start >= TX_READY_TIMEOUT_MS) {
        this->set_tx_fault_();
        ESP_LOGE(TAG, "UART0 TX timed out");
        return;
      }
    }
    HAL_UART_PutTxData(this->uart_, data[i]);
#ifdef USE_UART_DEBUGGER
    this->debug_callback_.call(UART_DIRECTION_TX, data[i]);
#endif
  }
}

UARTFlushResult XR806UARTComponent::flush() {
  if (!this->is_connected())
    return UARTFlushResult::UART_FLUSH_RESULT_FAILED;

  const uint32_t start = millis();
  while (!this->tx_hardware_idle_()) {
    if (millis() - start >= FLUSH_TIMEOUT_MS) {
      this->set_tx_fault_();
      ESP_LOGE(TAG, "UART0 flush timed out");
      return UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
    }
  }
  return UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
}

#ifdef USE_XR806_LOWPOWER

void XR806UARTComponent::note_uart_activity_() {
  // UART traffic must not begin until the SDK PM driver holds the wakelock. A vote in the
  // policy alone cannot stop a PM thread that has already started to suspend.
  if (!this->uart_pm_wakelock_held_) {
    if (pm_wake_lock(&this->uart_pm_wakelock_) != 0) {
      this->set_tx_fault_();
      ESP_LOGE(TAG, "UART0 PM wakelock refused; standby blocked");
      return;
    }
    this->uart_pm_wakelock_held_ = true;
  }
  esphome::xr806::lowpower_policy().set_uart_idle(false);
  this->idle_guard_.reset();
}

void XR806UARTComponent::update_uart_idle_() {
  auto &policy = esphome::xr806::lowpower_policy();
  if (!this->is_connected()) {
    policy.set_uart_idle(false);
    return;
  }

  // Start the 200-ms guard only after the last bit has left the UART,
  // the RX queue is drained and the peripheral reports not busy.
  // The previous activity-start timestamp could expire while a long TX was still on the wire.
  const bool hardware_idle = __atomic_load_n(&this->rx_count_, __ATOMIC_ACQUIRE) == 0 &&
                             !HAL_UART_IsRxReady(this->uart_) && this->tx_hardware_idle_() &&
                             (this->uart_->STATUS & UART_BUSY_BIT) == 0;
  const bool idle = this->idle_guard_.ready(hardware_idle, App.get_loop_component_start_time(), UART_IDLE_GUARD_MS);

  if (idle && this->uart_pm_wakelock_held_) {
    if (pm_wake_unlock(&this->uart_pm_wakelock_) != 0) {
      this->set_tx_fault_();
      ESP_LOGE(TAG, "UART0 PM wakelock release failed; standby blocked");
      return;
    }
    this->uart_pm_wakelock_held_ = false;
  } else if (!idle && !this->uart_pm_wakelock_held_) {
    this->note_uart_activity_();
  }
  policy.set_uart_idle(idle && !this->tx_fault_);
}

#endif  // USE_XR806_LOWPOWER

}  // namespace esphome::uart

#endif  // USE_XR806
