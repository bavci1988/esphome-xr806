// XR806 suspend coordinator: no MCU protocol is handled here.
// Hardware entry stays opt-in and fail-closed until every transport owner
// votes idle AND every registered participant has prepared its IO.
#include "esphome/core/defines.h"

#ifdef USE_XR806_STANDBY_CONTROLLER

#include "standby_controller.h"
#include "lowpower_policy.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <xr806_pm.h>

#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.standby");

XR806LowPowerPolicy &lowpower_policy() {
  static XR806LowPowerPolicy policy;
  return policy;
}

static XR806StandbyController *active_controller = nullptr;
// Shortest timer wake, for a period that ran out while the module was awake.
static constexpr uint32_t MIN_TIMER_MS = 50;

#ifdef USE_LOGGER
// PM suspends the UART without waiting for the transmitter, so the last log line would be cut off.
// Bytes that can be queued: the TX FIFO (at most 128) plus the shift register.
static constexpr uint32_t LOG_UART_QUEUED_BYTES_MAX = 129;

/// Waits until the log UART has sent every byte. False if it is still sending after the time
/// those bytes need at the configured baud rate.
static bool drain_log_uart() {
  if (logger::global_logger == nullptr || logger::global_logger->get_baud_rate() == 0)
    return true;
  const uint32_t baud = logger::global_logger->get_baud_rate();
  return xr806_uart_wait_tx_done(logger::global_logger->get_uart() - 1,
                                 LOG_UART_QUEUED_BYTES_MAX * 10 * 1000 / baud + 2);
}
#endif

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

void XR806StandbyController::request_preflight(bool requested) {
  if (requested && !this->preflight_requested_)
    this->preflight_since_ms_ = millis();
  this->preflight_requested_ = requested;
}

bool XR806StandbyController::services_quiesced() const {
  const uint32_t busy = XR806LowPowerPolicy::NETWORK_BUSY | XR806LowPowerPolicy::API_BUSY |
                        XR806LowPowerPolicy::OTA_BUSY | XR806LowPowerPolicy::MDNS_BUSY |
                        XR806LowPowerPolicy::HTTP_BUSY | XR806LowPowerPolicy::MQTT_BUSY;
  return this->may_quiesce() && (lowpower_policy().blockers() & busy) == 0;
}

void XR806StandbyController::request_standby(bool requested) {
  // Only the application or MCU driver may request entry, never a periodic
  // platform timer. A preflight must finish within a bounded 10s interval.
  auto &policy = lowpower_policy();
  if (requested && !policy.standby_requested())
    this->request_since_ms_ = millis();
  policy.request_standby(requested);
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
  if (this->no_http_)
    p.set_http_idle(true);
  if (this->no_mqtt_)
    p.set_mqtt_idle(true);
  if (this->external_sync_required_)
    p.require_external_mcu(true);
  this->state_.configure(this->quiet_ms_, this->min_awake_ms_);
  this->state_.boot(millis());
}

void XR806StandbyController::dump_config() {
  ESP_LOGCONFIG(TAG,
                "XR806 guarded standby coordinator:\n"
                "  Trigger: explicit application request only\n"
                "  Timer: %u ms; quiet: %u ms; min awake: %u ms",
                static_cast<unsigned>(this->wake_after_ms_), static_cast<unsigned>(this->quiet_ms_),
                static_cast<unsigned>(this->min_awake_ms_));
  if (this->boot_hold_ms_ != 0) {
    ESP_LOGCONFIG(TAG, "  No standby before %u s after boot (safe_mode boot check)",
                  static_cast<unsigned>(this->boot_hold_ms_ / 1000));
  }
  ESP_LOGCONFIG(TAG,
                "  Network during standby: %s\n"
                "  External sync: %s; required UART participant: %s",
                this->stay_connected_ ? LOG_STR_LITERAL("stays connected") : LOG_STR_LITERAL("parked"),
                YESNO(this->external_sync_required_), YESNO(this->require_uart_participant_));
}

bool XR806StandbyController::is_any_transport_unowned_() const {
  if (!this->require_uart_participant_)
    return false;
  for (size_t i = 0; i < this->participant_count_; ++i) {
    if (this->participants_[i]->owns_uart())
      return false;
  }
  return true;
}

bool XR806StandbyController::may_quiesce() const {
  if (this->is_any_transport_unowned_())
    return false;
  const uint32_t required =
      (this->no_network_ ? 0u : XR806LowPowerPolicy::NETWORK_BUSY) |
      (this->no_api_ ? 0u : XR806LowPowerPolicy::API_BUSY) | (this->no_ota_ ? 0u : XR806LowPowerPolicy::OTA_BUSY) |
      (this->no_mdns_ ? 0u : XR806LowPowerPolicy::MDNS_BUSY) | (this->no_http_ ? 0u : XR806LowPowerPolicy::HTTP_BUSY) |
      (this->no_mqtt_ ? 0u : XR806LowPowerPolicy::MQTT_BUSY);
  uint32_t registered = 0;
  for (size_t i = 0; i < this->participant_count_; ++i) {
    registered |= this->participants_[i]->owned_services();
    if (!this->participants_[i]->may_quiesce())
      return false;
  }
  // A configured service without a lifecycle owner refuses standby; a missing owner isn't "idle".
  return (registered & required) == required;
}

void XR806StandbyController::loop() {
  if (this->busy_ || this->state_.disabled() || this->is_any_transport_unowned_())
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (this->boot_hold_ms_ != 0) {
    if (now < this->boot_hold_ms_) {
      // A request made meanwhile waits; its timeouts start when the hold ends.
      this->request_since_ms_ = now;
      this->preflight_since_ms_ = now;
      return;
    }
    ESP_LOGD(TAG, "Boot hold of %" PRIu32 " s over; standby allowed", this->boot_hold_ms_ / 1000);
    this->boot_hold_ms_ = 0;
  }

  auto &p = lowpower_policy();
  if (this->preflight_requested_ && now - this->preflight_since_ms_ > HANDSHAKE_PREFLIGHT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "MCU CMD38 service preflight expired without confirmed drain");
    this->request_preflight(false);
  }
  // With the link kept up no service is parked while a request waits, so a long wait costs nothing.
  if (!this->stay_connected_ && p.standby_requested() && now - this->request_since_ms_ > PREFLIGHT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Suspend preflight timed out; releasing service owners and refusing PM entry");
    this->request_standby(false);
    this->state_.postpone();
  }
  // Services cannot release their votes if the preflight runs only after all votes are already idle.
  const bool draining = p.standby_requested() || this->preflight_requested_;
  for (size_t i = 0; i < this->participant_count_; ++i)
    this->participants_[i]->standby_preflight(draining);
  const uint32_t blockers = p.blockers();
  constexpr uint32_t owned_by_coordinator =
      XR806LowPowerPolicy::SCHEDULER_BUSY | XR806LowPowerPolicy::WAKE_NOT_READY | XR806LowPowerPolicy::NOT_REQUESTED;
  const bool all_owners_idle = (blockers & ~owned_by_coordinator) == 0;
  if (this->state_.due(now, p.generation(), p.standby_requested(), all_owners_idle))
    this->attempt_standby_();
}

void XR806StandbyController::cancel_prepared_(size_t prepared) {
  while (prepared > 0) {
    --prepared;
    this->participants_[prepared]->standby_cancelled();
  }
}

bool XR806StandbyController::wake_input_active_() const {
  return this->has_wake_pin_ && xr806_wake_pin_active(this->wake_pin_, this->wake_falling_);
}

// Runs with the wake sources armed, right before PM entry. The lock-free policy fence rejects
// any activity or vote change during the final admission; the PM wakelocks still guard the
// physical producers.
bool XR806StandbyController::commit_(void * /*arg*/) {
  auto &p = lowpower_policy();
  p.set_wake_source_ready(true);
  p.set_scheduler_idle(true);
  const uint32_t epoch = p.generation();
  return p.begin_commit(epoch) && p.commit_valid(epoch);
}

void XR806StandbyController::attempt_standby_() {
  if (this->busy_)
    return;
  // Right after asking to sleep the MCU may still hold its wake line for a moment. Wait one more quiet
  // interval instead of refusing, which would withdraw the MCU's consent until it is active again.
  if (this->wake_input_active_()) {
    this->state_.postpone();
    return;
  }
  this->busy_ = true;
  auto &p = lowpower_policy();
  p.set_scheduler_idle(false);
  p.set_wake_source_ready(false);

  const uint32_t before_clients = p.generation();
  size_t prepared = 0;
  bool ready = true;
  for (size_t i = 0; i < this->participant_count_ && ready; ++i) {
    // A protocol participant may queue its own final notification, then
    // return false until UART TX and pending status processing have drained.
    ++prepared;
    ready = this->participants_[i]->prepare_for_standby();
  }
  // If a client sent UART bytes or changed any PM vote while preparing,
  // give the buses another full quiet interval, rather than racing PM entry.
  ready = ready && before_clients == p.generation() && p.standby_requested();
#ifdef USE_LOGGER
  // Before the wake sources are armed, so a log UART that is still sending postpones the attempt
  // instead of counting as a failed one.
  ready = ready && drain_log_uart();
#endif
  if (!ready) {
    this->cancel_prepared_(prepared);
    this->state_.postpone();
    this->busy_ = false;
    return;
  }

  // The wake timer always runs, also on a pin wake. Its period runs on the RTC counter, which keeps
  // counting in standby, so frequent network wakes do not restart it. Only a timer wake does.
  const uint64_t now_us = xr806_rtc_time_us();
  if (this->timer_deadline_us_ == 0)
    this->timer_deadline_us_ = now_us + static_cast<uint64_t>(this->wake_after_ms_) * 1000;
  const int64_t left_us = static_cast<int64_t>(this->timer_deadline_us_ - now_us);
  const xr806_standby_config_t config{
      .timer_ms = left_us <= static_cast<int64_t>(MIN_TIMER_MS) * 1000
                      ? MIN_TIMER_MS
                      : static_cast<uint32_t>(static_cast<uint64_t>(left_us) / 1000),
      .wake_pin = this->has_wake_pin_ ? static_cast<int8_t>(this->wake_pin_) : static_cast<int8_t>(-1),
      .wake_falling = this->wake_falling_,
      .commit = &XR806StandbyController::commit_,
      .arg = nullptr,
  };
  xr806_standby_result_t result;
  {
    // Time spent asleep is not a main loop stall.
    UnavoidableBlockingScope sleeping;
    result = xr806_standby_enter(&config);
  }
  p.end_commit();
  p.set_scheduler_idle(false);
  p.set_wake_source_ready(false);
  p.request_standby(false);

  switch (result.status) {
    case XR806_STANDBY_ENTERED:
      this->resumed_(result.pm_return, result.event);
      break;
    case XR806_STANDBY_NO_TIMER:
      ESP_LOGE(TAG, "No hardware timer wake source; standby refused");
      break;
    case XR806_STANDBY_PIN_OWNED:
      ESP_LOGE(TAG, "Wake PA%u already owned by IRQ/output client; refusing standby",
               static_cast<unsigned>(this->wake_pin_));
      break;
    case XR806_STANDBY_PIN_ACTIVE:
      ESP_LOGW(TAG, "Wake input already active; standby refused");
      break;
    case XR806_STANDBY_PIN_INVALID:
    case XR806_STANDBY_PIN_MODE:
      ESP_LOGE(TAG, "Wake GPIO not in EINT mode; standby refused");
      break;
    case XR806_STANDBY_REFUSED:
      break;
  }
  if (result.status != XR806_STANDBY_ENTERED) {
    this->cancel_prepared_(prepared);
    this->state_.finished(millis(), false);
  }

  if (this->state_.disabled()) {
    ESP_LOGE(TAG, "PM permanently disabled after three unsuccessful attempts");
    this->mark_failed();
  }
  this->busy_ = false;
}

void XR806StandbyController::resumed_(int pm_return, uint32_t event) {
  auto &p = lowpower_policy();
  const int wake_io = this->has_wake_pin_ ? static_cast<int>(this->wake_pin_) - XR806_WAKE_PIN_FIRST : -1;
  const bool expected = XR806StandbyCoordinatorState::expected_wake(pm_return, event, wake_io, this->stay_connected_);
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
  if (!this->no_http_)
    p.set_http_idle(false);
  if (!this->no_mqtt_)
    p.set_mqtt_idle(false);

  XR806StandbyResume resume{};
  resume.event_bitmap = event;
  resume.pm_return = pm_return;
  resume.expected_source = expected;
  resume.wake_timer = (event & XR806_WAKE_EVENT_TIMER) != 0;
  if (resume.wake_timer)
    this->timer_deadline_us_ = 0;
  resume.wake_gpio = this->has_wake_pin_ && (event & xr806_wake_pin_event(this->wake_pin_)) != 0;
  resume.wake_wlan = (event & XR806_WAKE_EVENT_WLAN) != 0;
  for (size_t i = 0; i < this->participant_count_; ++i)
    this->participants_[i]->standby_resumed(resume);
  // With the link kept up every received packet ends a standby; keep those wakes out of the INFO log.
  if (expected && this->stay_connected_) {
    ESP_LOGV(TAG, "Standby PM return=%d event=0x%08" PRIx32, pm_return, event);
  } else {
    ESP_LOGI(TAG, "Standby PM return=%d event=0x%08" PRIx32 " expected=%d", pm_return, event,
             static_cast<int>(expected));
  }
}

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_CONTROLLER
