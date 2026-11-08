// XR806 production-side suspend coordinator: no MCU protocol is handled here.
// Hardware entry remains opt-in and fail-closed until every transport owner
// independently votes idle AND a registered participant prepares the IO.
#include "esphome/core/defines.h"

#ifdef USE_XR806_STANDBY_CONTROLLER

extern "C" {
#include "xr_config.h"
}

#if !defined(CONFIG_PM) || !defined(CONFIG_PM_WAKELOCKS) || !defined(CONFIG_STANDBY_RET_SRAM_ALL)
#error "XR806 standby requires PM, PM wakelocks and full SRAM retention"
#endif
#if !defined(CONFIG_CHIP_ARCH_VER) || CONFIG_CHIP_ARCH_VER != 3
#error "XR806 standby wake GPIO mapping requires architecture v3"
#endif

#include "standby_controller.h"
#include "lowpower_policy.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

extern "C" {
#include "driver/chip/hal_gpio.h"
#include "driver/chip/hal_prcm.h"
#include "driver/chip/hal_wakeup.h"
#include "pm/pm.h"
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.standby");

static XR806StandbyController *active_controller = nullptr;

XR806StandbyController *standby_controller() { return active_controller; }

bool XR806StandbyController::register_participant(XR806StandbyParticipant *participant) {
  if (participant == nullptr || this->busy_ || this->participant_count_ >= MAX_PARTICIPANTS)
    return false;
  for (size_t i = 0; i < this->participant_count_; ++i) {
    if (this->participants_[i] == participant)
      return false;
  }
  this->participants_[this->participant_count_++] = participant;
  return true;
}

void XR806StandbyController::request_standby(bool requested) {
  // This method is invoked by the requesting application or MCU driver,
  // never by a periodic background timer in the XR806 platform.
  lowpower_policy().request_standby(requested);
}

void XR806StandbyController::setup() {
  if (active_controller != nullptr) {
    this->mark_failed();
    return;
  }
  active_controller = this;
  auto &p = lowpower_policy();
  p.request_standby(false);
  p.set_scheduler_idle(false);
  p.set_wake_source_ready(false);
  // These votes are cleared only for components absent from this build.
  // When a transport exists, its owner must vote idle; no generic
  // "everything is idle" shortcut is permitted.
  if (this->no_uart_)
    p.set_uart_idle(true);
  if (this->no_network_)
    p.set_network_idle(true);
  if (this->no_api_)
    p.set_api_idle(true);
  if (this->no_ota_)
    p.set_ota_idle(true);
  if (this->no_mdns_)
    p.set_mdns_idle(true);
  if (this->external_sync_required_)
    p.require_external_mcu(true);
  this->state_.configure(this->quiet_ms_, this->min_awake_ms_);
  this->state_.boot(millis());
}

void XR806StandbyController::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 guarded standby coordinator:");
  ESP_LOGCONFIG(TAG, "  Trigger: explicit application request only");
  ESP_LOGCONFIG(TAG, "  Timer: %u ms; quiet: %u ms; min awake: %u ms", static_cast<unsigned>(this->wake_after_ms_),
                static_cast<unsigned>(this->quiet_ms_), static_cast<unsigned>(this->min_awake_ms_));
  ESP_LOGCONFIG(TAG, "  External sync: %s; required UART participant: %s",
                this->external_sync_required_ ? LOG_STR_LITERAL("yes") : LOG_STR_LITERAL("no"),
                this->require_uart_participant_ ? LOG_STR_LITERAL("yes") : LOG_STR_LITERAL("no"));
}

bool XR806StandbyController::is_any_transport_unowned_() const {
  return this->require_uart_participant_ && this->participant_count_ == 0;
}

void XR806StandbyController::loop() {
  if (this->busy_ || this->state_.disabled() || this->is_any_transport_unowned_())
    return;

  auto &p = lowpower_policy();
  const uint32_t blockers = p.blockers();
  constexpr uint32_t OWNED_BY_COORDINATOR =
      XR806LowPowerPolicy::SCHEDULER_BUSY | XR806LowPowerPolicy::WAKE_NOT_READY | XR806LowPowerPolicy::NOT_REQUESTED;
  const bool all_owners_idle = (blockers & ~OWNED_BY_COORDINATOR) == 0;
  if (this->state_.due(millis(), p.generation(), p.standby_requested(), all_owners_idle))
    this->attempt_standby_();
}

void XR806StandbyController::cancel_prepared_(size_t prepared) {
  while (prepared > 0) {
    --prepared;
    this->participants_[prepared]->standby_cancelled();
  }
}

void XR806StandbyController::attempt_standby_() {
  if (this->busy_)
    return;
  this->busy_ = true;
  auto &p = lowpower_policy();
  p.set_scheduler_idle(false);
  p.set_wake_source_ready(false);

  const uint32_t before_clients = p.generation();
  size_t prepared = 0;
  for (size_t i = 0; i < this->participant_count_; ++i) {
    // A protocol participant may queue its own final notification, then
    // return false until UART TX and pending status processing have drained.
    ++prepared;
    if (!this->participants_[i]->prepare_for_standby()) {
      this->cancel_prepared_(prepared);
      this->state_.postpone();
      this->busy_ = false;
      return;
    }
  }

  // If a client sent UART bytes or changed any PM vote while preparing,
  // give the buses another full quiet interval, rather than racing PM entry.
  if (before_clients != p.generation() || !p.standby_requested()) {
    this->cancel_prepared_(prepared);
    this->state_.postpone();
    this->busy_ = false;
    return;
  }

  const int wake_io = this->has_wake_pin_ ? static_cast<int>(this->wake_pin_) - 10 : -1;
  GPIO_InitParam previous_gpio{};
  bool gpio_armed = false;
  bool timer_armed = false;
  bool pm_called = false;
  int result = -1;
  uint32_t event = 0;

  // Always retain a hardware wake timer as fallback, including on PA20 wake.
  if (HAL_Wakeup_SetTimer_mS(this->wake_after_ms_) != 0) {
    ESP_LOGE(TAG, "No hardware timer wake source; standby refused");
    goto abort;
  }
  timer_armed = true;

  if (this->has_wake_pin_) {
    const auto pin = static_cast<GPIO_Pin>(this->wake_pin_);
    HAL_GPIO_GetConfig(GPIO_PORT_A, pin, &previous_gpio);
    GPIO_InitParam eint = previous_gpio;
    eint.mode = GPIOx_Pn_F6_EINT;
    eint.pull = this->wake_falling_ ? GPIO_PULL_UP : GPIO_PULL_DOWN;
    HAL_GPIO_Init(GPIO_PORT_A, pin, &eint);
    gpio_armed = true;
    HAL_Wakeup_SetIO(static_cast<uint32_t>(wake_io),
                     this->wake_falling_ ? WKUPIO_WK_MODE_FALLING_EDGE : WKUPIO_WK_MODE_RISING_EDGE, eint.pull);
    HAL_PRCM_SetWakeupDebClk0(0);
    HAL_PRCM_SetWakeupIOxDebSrc(static_cast<uint32_t>(wake_io), 0);
    HAL_PRCM_SetWakeupIOxDebounce(static_cast<uint32_t>(wake_io), 1);
  }

  if (!HAL_Wakeup_CheckIOMode()) {
    ESP_LOGE(TAG, "Wake GPIO not in EINT mode; standby refused");
    goto abort;
  }

  p.set_wake_source_ready(true);
  p.set_scheduler_idle(true);
  // The last software check is deliberately adjacent to the PM request.
  // Platform/transport owners must ALSO hold SDK PM wakelocks while busy.
  if (!p.can_enter_standby()) {
    goto abort;
  }

  HAL_Wakeup_SetEvent(0);
  pm_called = true;
  // '500' is the SDK wakelock-touch deadline, NOT an upper bound on the
  // total call! pm_enter_mode_timeout waits on a PM semaphore indefinitely
  // in this SDK; physical recovery needs independent hardware testing.
  result = pm_enter_mode_timeout(PM_MODE_STANDBY, 500);
  event = HAL_Wakeup_GetEvent();

abort:
  p.set_scheduler_idle(false);
  p.set_wake_source_ready(false);
  p.request_standby(false);
  if (timer_armed)
    (void) HAL_Wakeup_ClrTimer();
  if (gpio_armed) {
    HAL_Wakeup_ClrIO(static_cast<uint32_t>(wake_io));
    HAL_PRCM_SetWakeupIOxDebounce(static_cast<uint32_t>(wake_io), 0);
    // Do not deinitialize an MCU-owned pin: restore its original mux.
    HAL_GPIO_Init(GPIO_PORT_A, static_cast<GPIO_Pin>(this->wake_pin_), &previous_gpio);
  }

  if (!pm_called) {
    this->cancel_prepared_(prepared);
    this->state_.finished(millis(), false);
  } else {
    const bool expected = XR806StandbyCoordinatorState::expected_wake(result, event, wake_io);
    this->state_.finished(millis(), expected);
    // Every participant must assess the wake/abort independently.
    // In particular the MCU decides whether to send CMD12/00.
    if (!this->no_uart_)
      p.set_uart_idle(false);
    if (this->external_sync_required_)
      p.set_mcu_synchronized(false);
    if (!this->no_network_)
      p.set_network_idle(false);
    if (!this->no_api_)
      p.set_api_idle(false);
    if (!this->no_ota_)
      p.set_ota_idle(false);
    if (!this->no_mdns_)
      p.set_mdns_idle(false);

    XR806StandbyResume resume{};
    resume.event_bitmap = event;
    resume.pm_return = result;
    resume.expected_source = expected;
    resume.wake_timer = (event & XR806StandbyCoordinatorState::WAKE_TIMER) != 0;
    resume.wake_gpio = wake_io >= 0 && (event & (1u << wake_io)) != 0;
    for (size_t i = 0; i < this->participant_count_; ++i)
      this->participants_[i]->standby_resumed(resume);
    ESP_LOGI(TAG, "Standby PM return=%d event=0x%08lx expected=%d; hardware current not verified", result,
             static_cast<unsigned long>(event), static_cast<int>(expected));
  }

  if (this->state_.disabled()) {
    ESP_LOGE(TAG, "PM permanently disabled after three unsuccessful attempts");
    this->mark_failed();
  }
  this->busy_ = false;
}

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_CONTROLLER
