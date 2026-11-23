#include "uart_standby.h"

#ifdef USE_XR806_STANDBY_CONTROLLER

#include <xr806_pm.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.uart_standby");

// Quiet time after the last UART activity before the UART allows standby.
static constexpr uint32_t UART_IDLE_GUARD_MS = 200;

void XR806UARTStandby::setup() {
  lowpower_policy().set_uart_idle(false);
  const int8_t port = this->uart_->get_hw_serial_number();
  if (port < 0) {
    this->mark_failed(LOG_STR("Standby needs a hardware UART"));
    return;
  }
  this->port_ = port;
  this->lock_ = xr806_wakelock_create("esphome.uart");
  this->set_busy_();
}

void XR806UARTStandby::set_busy_() {
  // A vote in the policy alone cannot stop a PM path that has already started to suspend.
  if (!this->lock_held_) {
    if (!xr806_wakelock_acquire(this->lock_)) {
      ESP_LOGE(TAG, "UART PM wakelock refused; standby blocked");
      this->status_set_error();
      return;
    }
    this->lock_held_ = true;
  }
  lowpower_policy().set_uart_idle(false);
  this->idle_guard_.reset();
}

void XR806UARTStandby::loop() {
  const uint32_t activity = xr806_uart_activity(this->port_);
  // The hardware check also counts line errors, so it goes first.
  const bool hardware_idle =
      xr806_uart_hw_idle(this->port_) && this->uart_->available() == 0 && activity == this->activity_;
  this->activity_ = activity;
  if (!this->idle_guard_.ready(hardware_idle, App.get_loop_component_start_time(), UART_IDLE_GUARD_MS)) {
    if (!hardware_idle || !this->lock_held_)
      this->set_busy_();
    return;
  }
  if (this->lock_held_) {
    if (!xr806_wakelock_release(this->lock_)) {
      ESP_LOGE(TAG, "UART PM wakelock release failed; standby blocked");
      this->status_set_error();
      return;
    }
    this->lock_held_ = false;
  }
  lowpower_policy().set_uart_idle(true);
}

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_CONTROLLER
