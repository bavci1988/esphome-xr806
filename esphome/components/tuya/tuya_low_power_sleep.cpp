#include "esphome/core/defines.h"

#ifdef USE_TUYA_LOW_POWER

#include "tuya_uart.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

// The sleep handshake of protocol low_power. Without a TuyaLowPowerHost every sleep request is answered with busy.
namespace esphome::tuya {

ESPHOME_LOG_TAG(LP_TAG, "tuya.low_power");
static constexpr uint8_t LP_V0_SLEEP_NOTICE_CMD = 0x12;  // 01: the module sleeps now, 00: it is awake
static constexpr uint8_t LP_V0_BUSY = 0x01;
static constexpr uint8_t LP_V0_BUSY_UPGRADE = 0x03;
static constexpr uint8_t LP_V3_SLEEP_OFFER = 0x00;
static constexpr uint8_t LP_V3_SLEEP_WAKE = 0x01;
static constexpr uint8_t LP_V3_SLEEP_REQUEST = 0x02;
// Answer flags to the MCU's own version-3 sleep request, as in the original module firmware
static constexpr uint8_t LP_V3_BUSY_NETWORK = 0x02;
static constexpr uint8_t LP_V3_BUSY_UPGRADE = 0x04;
// How long network services may take to stop before a sleep request is answered with busy
static constexpr uint32_t LP_PREFLIGHT_TIMEOUT_MS = 1800;
// After a busy answer or MCU traffic during a preflight, no grant for this long
static constexpr uint32_t LP_SLEEP_COOLDOWN_MS = 500;
// Pause between attempts to grant an earlier request while the network services stay busy
static constexpr uint32_t LP_DEFERRED_SLEEP_RETRY_MS = 10000;
// Version 3: quiet time before the module offers sleep, as in the original module firmware, and the answer timeout
static constexpr uint32_t LP_V3_SLEEP_IDLE_MS = 5000;
static constexpr uint32_t LP_V3_OFFER_TIMEOUT_MS = 1000;

void TuyaUART::setup_low_power_sleep_() {
  // Not nullptr, which means "allowed": the first check logs its outcome either way
  this->lp_sleep_blocked_ = LOG_STR("not checked yet");
  // No cooldown and no pause between deferred grants at the start
  const uint32_t now = App.get_loop_component_start_time();
  this->lp_cmd38_cooldown_ms_ = now - LP_SLEEP_COOLDOWN_MS;
  this->lp_cmd38_retry_ms_ = now - LP_DEFERRED_SLEEP_RETRY_MS;
  if (this->lp_host_ != nullptr && this->mcu_wake_pin_ == nullptr) {
    // Learned from an earlier product info: a sleeping MCU answers CMD01 only after a wake pulse
    this->mcu_wake_pin_ = this->lp_host_->mcu_wake_output();
    this->lp_mcu_wake_from_host_ = this->mcu_wake_pin_ != nullptr;
  }
}

void TuyaUART::poll_low_power_sleep_() {
  this->poll_low_power_sleep_preflight_();
  this->poll_low_power_deferred_sleep_();
  // Any new work ends the consent before the platform has entered standby
  if (this->lp_standby_.requested() &&
      (!this->lp_host_->standby_requested() || this->low_power_sleep_blocker_() != nullptr))
    this->cancel_low_power_standby_();
  this->poll_low_power_v3_sleep_();
  this->update_low_power_sleep_lock_();
}

void TuyaUART::on_low_power_rx_byte_() {
  const uint32_t now = App.get_loop_component_start_time();
  this->lp_v3_idle_since_ms_ = now;
  // Any byte, not only a whole frame, ends a pending grant or consent
  if (this->lp_cmd38_pending_) {
    this->cancel_low_power_preflight_(true);
    this->lp_cmd38_cooldown_ms_ = now;
  }
  if (this->lp_standby_.requested())
    this->cancel_low_power_standby_();
}

void TuyaUART::on_low_power_frame_(uint8_t command, const uint8_t *data, size_t len) {
  this->lp_mcu_wake_fresh_ = false;  // The MCU answered; a version-0 MCU may now fall asleep again
  if (command == 0x00 && len == 1 && data[0] == 0) {
    // A restarted MCU ends the old handshake
    this->lp_standby_.reset_on_mcu_boot();
    if (this->lp_host_ != nullptr)
      this->lp_host_->request_standby(false);
  }
  // The MCU is active again and asks for sleep itself once it is done
  if (command != LP_V0_SLEEP_CMD)
    this->lp_cmd38_deferred_ = false;
}

// The MCU UART failed: no frame can be sent, so the consent ends without a notice.
void TuyaUART::stop_low_power_sleep_() {
  this->lp_standby_.transport_fault();
  if (this->lp_cmd38_pending_)
    this->cancel_low_power_preflight_(false);
  if (this->lp_host_ != nullptr)
    this->lp_host_->request_standby(false);
  this->update_low_power_sleep_lock_();
}

void TuyaUART::preempt_low_power_sleep_() {
  if (this->lp_cmd38_pending_)
    this->cancel_low_power_preflight_(true);
  if (this->lp_standby_.requested())
    this->cancel_low_power_standby_();
}

// The platform takes the network down for a standby: that is no new network state for the MCU.
bool TuyaUART::low_power_network_held_() const {
  return this->lp_host_ != nullptr && this->lp_host_->network_quiescing() &&
         (this->lp_host_->standby_requested() || this->lp_host_->preflight_requested());
}

void TuyaUART::configure_low_power_wake_lines_() {
  if (this->lp_wake_lines_configured_ || this->lp_host_ == nullptr)
    return;
  this->lp_wake_lines_configured_ = true;
  // The product info names both wake lines, so the configuration does not have to
  const bool create_output = this->mcu_wake_pin_ == nullptr || this->lp_mcu_wake_from_host_;
  const int wake_input =
      this->lp_host_->configure_wake_lines(this->lp_product_.wake_gpio, this->lp_product_.mcu_wake_gpio,
                                           this->lp_product_.mcu_wake_active_high, create_output);
  this->lp_wake_input_ = static_cast<int8_t>(wake_input < 0 || wake_input > 127 ? -1 : wake_input);
  ESP_LOGD(LP_TAG, "MCU wake lines wk=%d mwk=%d, sleep mode sm=%d, wake input %d", this->lp_product_.wake_gpio,
           this->lp_product_.mcu_wake_gpio, this->lp_product_.sleep_mode, this->lp_wake_input_);
  if (!create_output)
    return;
  this->mcu_wake_pin_ = this->lp_host_->mcu_wake_output();
  if (this->mcu_wake_pin_ != nullptr) {
    this->lp_mcu_wake_from_host_ = true;
    setup_output_pin(this->mcu_wake_pin_, false);
  }
}

// The first condition of the platform or the MCU link that keeps the module awake, nullptr when none does.
const LogString *TuyaUART::low_power_sleep_blocker_(bool in_frame_handler) const {
  const LogString *blocker = this->low_power_standby_blocker_();
  return blocker != nullptr ? blocker : this->low_power_link_blocker_(in_frame_handler);
}

// The first condition of the MCU link that keeps the module awake, nullptr when none does.
const LogString *TuyaUART::low_power_link_blocker_(bool in_frame_handler) const {
  if (!this->lp_session_.ready())
    return LOG_STR("MCU state not synchronized");
  if (!this->lp_wire_.idle())
    return LOG_STR("command waiting for its answer");
  // The DP101/DP102 sleep window exists only on the HC32 firmware (version 0 frames)
  if (!this->lp_v3_ && !this->lp_sleep_window_.standby_mode_stable())
    return LOG_STR("sleep window changing");
  if (this->lp_product_retry_.active())
    return LOG_STR("product query retry");
  if (App.get_loop_component_start_time() - this->lp_wake_guard_ms_ < LP_WAKE_GUARD_MS)
    return LOG_STR("wake guard");
  if (this->auto_baud_ && !this->lp_autobaud_.locked())
    return LOG_STR("baud rate not found");
  if (!this->parent_->is_connected() || !this->lp_uart_rx_errors_.unchanged(this->low_power_uart_rx_error_count_()))
    return LOG_STR("UART error");
  if (this->lp_standby_.recovery_required())
    return LOG_STR("standby recovery");
  if (!this->command_queue_.empty() || this->expected_response_.has_value())
    return LOG_STR("commands queued");
  if (this->lp_mcu_wake_pulse_active_ || this->lp_awake_notification_pending_)
    return LOG_STR("waking the MCU");
  if (this->lp_mcu_update_ != McuUpdate::MCU_UPDATE_IDLE)
    return LOG_STR("MCU update");
  if (this->parent_->available() != 0)
    return LOG_STR("bytes waiting");
  // In the frame handler, the frame being handled is not one still coming in
  if (!in_frame_handler && this->reader_.receiving())
    return LOG_STR("frame coming in");
  return nullptr;
}

// What on the platform's side keeps the module from a standby the MCU agrees to, nullptr when nothing does.
const LogString *TuyaUART::low_power_standby_blocker_() const {
  auto *host = this->lp_host_;
  if (host == nullptr || !host->standby_supported())
    return LOG_STR("no standby controller");
  // Version 0 sleeps on its own request (sm=1), version 3 on the module's offer (sm=2)
  if (this->lp_product_.sleep_mode != (this->lp_v3_ ? 2 : 1))
    return LOG_STR("MCU sleep mode");
  if (this->lp_wake_input_ < 0 || host->standby_wake_pin() != this->lp_wake_input_)
    return LOG_STR("wake input differs from the MCU's");
  if (!host->standby_requires_external_sync())
    return LOG_STR("standby does not wait for the MCU");
  if (!host->standby_may_quiesce())
    return LOG_STR("services busy");
  if (host->ota_busy())
    return LOG_STR("update running");
  return nullptr;
}

// Whether the frames sent so far have left the UART; a platform that cannot tell assumes they have.
bool TuyaUART::flush_low_power_() {
  const auto result = this->flush();
  return result == uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS ||
         result == uart::UARTFlushResult::UART_FLUSH_RESULT_ASSUMED_SUCCESS;
}

void TuyaUART::update_low_power_sleep_lock_() {
  if (this->lp_host_ != nullptr)
    this->lp_host_->set_mcu_synchronized(this->low_power_link_blocker_() == nullptr);
}

// Logs why sleep is refused, or that it is allowed again, once per change. Without a host only at VERBOSE.
void TuyaUART::log_low_power_sleep_blocker_(const LogString *blocker) {
  if (blocker == this->lp_sleep_blocked_)
    return;
  this->lp_sleep_blocked_ = blocker;
  if (blocker == nullptr) {
    ESP_LOGD(LP_TAG, "Sleep allowed");
  } else if (this->lp_host_ == nullptr) {
    ESP_LOGV(LP_TAG, "Sleep blocked: %s", LOG_STR_ARG(blocker));
  } else {
    ESP_LOGD(LP_TAG, "Sleep blocked: %s (platform blockers 0x%04X)", LOG_STR_ARG(blocker),
             static_cast<unsigned>(this->lp_host_->standby_blockers()));
  }
}

// A frame may not have left the UART: the MCU's sleep state is unknown until its full status is read again,
// which also ends the recovery.
void TuyaUART::fail_low_power_transport_(const LogString *reason) {
  ESP_LOGW(LP_TAG, "%s: no sleep until the MCU state is read again", LOG_STR_ARG(reason));
  this->lp_standby_.transport_fault();
  this->lp_session_.command_timeout();
  this->lp_network_status_ = 0xFF;      // Version 0: CMD02 0x04 asks for the full status again
  this->lp_v3_report_pending_ = false;  // A report from before the fault does not end the recovery
}

void TuyaUART::handle_low_power_sleep_request_(const uint8_t *data, size_t len) {
  if (len != 1 || data[0] != 0)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (!this->lp_v3_) {
    // A version-0 MCU sleeps right after asking, ignores a busy answer and asks again only after its next
    // activity: a request that cannot be granted now is granted later. A network state that crossed the
    // request is lost, so it is sent again.
    this->lp_cmd38_deferred_ = this->lp_host_ != nullptr;
    if (this->lp_wire_.abandon_network()) {
      ESP_LOGD(LP_TAG, "MCU went to sleep before answering CMD02; sending it again");
      this->expected_response_.reset();
      this->lp_session_.reset_status();
      this->lp_network_status_ = 0xFF;
    }
  }
  // The frame's first byte already ended a consent or a preflight; just after a preflight it is busy
  if (now - this->lp_cmd38_cooldown_ms_ < LP_SLEEP_COOLDOWN_MS) {
    this->send_low_power_direct_(LP_V0_SLEEP_CMD, {0x00, LP_V0_BUSY});
    return;
  }
  const uint8_t flags = this->grant_low_power_sleep_(true);
  if (flags != 0)
    this->send_low_power_direct_(LP_V0_SLEEP_CMD, {0x00, flags});
}

// Grants a version-0 sleep request, or starts stopping the network services for it (0); else the busy flags.
uint8_t TuyaUART::grant_low_power_sleep_(bool in_frame_handler) {
  auto *host = this->lp_host_;
  const LogString *blocker = this->low_power_sleep_blocker_(in_frame_handler);
  this->log_low_power_sleep_blocker_(blocker);
  if (blocker != nullptr)
    return host != nullptr && host->ota_busy() ? LP_V0_BUSY_UPGRADE : LP_V0_BUSY;
  if (!host->standby_services_quiesced()) {
    // The MCU learns of the grant only once every network service has stopped
    this->lp_cmd38_pending_ = true;
    this->lp_cmd38_preflight_ms_ = App.get_loop_component_start_time();
    host->request_preflight(true);
    return 0;
  }
  if (!this->lp_standby_.accept_request())
    return LP_V0_BUSY;
  this->send_low_power_consent_(LP_V0_SLEEP_CMD, 0x00);
  return 0;
}

// Grants a sleep request (38 00 00 or 91 02 00); the platform may enter standby once it has left the UART.
void TuyaUART::send_low_power_consent_(uint8_t command, uint8_t subcommand) {
  if (this->lp_cmd38_deferred_) {
    ESP_LOGD(LP_TAG, "Granting the MCU's earlier sleep request");
  }
  this->lp_cmd38_deferred_ = false;
  this->send_low_power_direct_(command, {subcommand, 0x00});
  if (this->flush_low_power_()) {
    this->lp_host_->request_standby(true);
  } else {
    this->fail_low_power_transport_(LOG_STR("MCU UART flush failed"));
  }
  this->update_low_power_sleep_lock_();
}

void TuyaUART::poll_low_power_sleep_preflight_() {
  if (!this->lp_cmd38_pending_)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  const bool timed_out = now - this->lp_cmd38_preflight_ms_ >= LP_PREFLIGHT_TIMEOUT_MS;
  if (timed_out || !this->lp_host_->preflight_requested() || this->low_power_sleep_blocker_() != nullptr) {
    if (timed_out) {
      ESP_LOGD(LP_TAG, "Network services did not stop in time; sleep request answered busy");
    }
    this->cancel_low_power_preflight_(true);
    this->lp_cmd38_cooldown_ms_ = now;
    return;
  }
  if (!this->lp_host_->standby_services_quiesced())
    return;
  if (!this->lp_standby_.accept_request()) {
    this->cancel_low_power_preflight_(true);
    return;
  }
  this->lp_cmd38_pending_ = false;
  this->send_low_power_consent_(LP_V0_SLEEP_CMD, 0x00);
  this->lp_host_->request_preflight(false);
}

void TuyaUART::cancel_low_power_preflight_(bool notify_mcu) {
  this->lp_cmd38_pending_ = false;
  this->lp_host_->request_preflight(false);
  if (notify_mcu)
    this->send_low_power_direct_(LP_V0_SLEEP_CMD, {0x00, LP_V0_BUSY});
}

void TuyaUART::poll_low_power_deferred_sleep_() {
  const uint32_t now = App.get_loop_component_start_time();
  if (!this->lp_cmd38_deferred_ || this->lp_cmd38_pending_ || this->lp_standby_.requested() ||
      now - this->lp_cmd38_cooldown_ms_ < LP_SLEEP_COOLDOWN_MS ||
      now - this->lp_cmd38_retry_ms_ < LP_DEFERRED_SLEEP_RETRY_MS || this->grant_low_power_sleep_(false) != 0)
    return;
  // Stopping the network services may fail (an open connection, for example): the next attempt waits
  if (this->lp_cmd38_pending_)
    this->lp_cmd38_retry_ms_ = now;
}

// Only with a host: a consent needs one.
void TuyaUART::cancel_low_power_standby_() {
  const bool compensate = this->lp_standby_.force_cancel();
  this->lp_host_->request_standby(false);
  if (compensate)
    this->send_low_power_awake_notification_();
  this->update_low_power_sleep_lock_();
}

void TuyaUART::send_low_power_awake_notification_() {
  if (this->lp_v3_ && this->mcu_wake_pin_ != nullptr) {
    // A version-3 MCU sleeps after its consent and listens to the UART only after a wake pulse
    this->begin_mcu_wake_pulse_();
    if (this->lp_mcu_wake_pulse_active_) {
      this->lp_awake_notification_pending_ = true;
      this->lp_wake_guard_ms_ = App.get_loop_component_start_time();
      this->update_low_power_sleep_lock_();
      return;
    }
  }
  this->send_low_power_awake_frame_();
}

void TuyaUART::send_low_power_awake_frame_() {
  if (!this->parent_->is_connected()) {
    this->fail_low_power_transport_(LOG_STR("MCU UART not connected"));
  } else {
    if (this->lp_v3_) {
      this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_WAKE, 0x01});
    } else {
      this->send_low_power_direct_(LP_V0_SLEEP_NOTICE_CMD, {0x00});
    }
    if (!this->flush_low_power_())
      this->fail_low_power_transport_(LOG_STR("MCU UART flush failed"));
  }
  this->lp_wake_guard_ms_ = App.get_loop_component_start_time();
  this->update_low_power_sleep_lock_();
}

bool TuyaUART::low_power_prepare_for_standby() {
  if (!this->lp_standby_.requested() || this->low_power_sleep_blocker_() != nullptr)
    return false;
  if (this->lp_standby_.needs_notification()) {
    // CMD12/01 goes out first; this attempt is declined, so the UART drains before the next one
    if (!this->flush_low_power_()) {
      ESP_LOGD(LP_TAG, "MCU UART flush failed before CMD12/01; standby declined");
      return false;
    }
    this->send_low_power_direct_(LP_V0_SLEEP_NOTICE_CMD, {0x01});
    if (!this->flush_low_power_()) {
      this->fail_low_power_transport_(LOG_STR("MCU UART flush failed"));
    } else {
      this->lp_standby_.notification_sent();
    }
    this->update_low_power_sleep_lock_();
    return false;
  }
  return this->lp_standby_.ready_for_pm() && this->flush_low_power_();
}

void TuyaUART::low_power_standby_cancelled() {
  const bool compensate = this->lp_standby_.cancelled();
  if (!this->lp_standby_.requested())
    this->lp_host_->request_standby(false);
  if (compensate)
    this->send_low_power_awake_notification_();
  this->update_low_power_sleep_lock_();
}

void TuyaUART::low_power_standby_resumed(bool expected_source, bool wake_gpio, bool wake_timer) {
  const bool keep = this->lp_host_->standby_keeps_network();
  // A version-3 MCU is told about every wake that ends its consent, not only about one it caused itself
  const auto action = this->lp_standby_.resumed(expected_source, wake_gpio || (this->lp_v3_ && !keep), keep);
  if (action == TuyaLowPowerStandbyState::ResumeAction::RESUME_ACTION_KEEP_CONSENT) {
    // The MCU's own changes can be lost while the module sleeps, so its state is read on every timer wake
    if (wake_timer && !this->lp_v3_) {
      ESP_LOGD(LP_TAG, "Timer wake: reading the MCU state");
      if (this->request_low_power_refresh())
        return;
    }
    // The MCU did not wake the module and still sleeps: no CMD12/00, no wake guard, sleep again once idle
    this->lp_host_->request_standby(true);
    this->update_low_power_sleep_lock_();
    return;
  }
  this->lp_wake_guard_ms_ = App.get_loop_component_start_time();
  if (action == TuyaLowPowerStandbyState::ResumeAction::RESUME_ACTION_SEND_WAKE) {
    this->send_low_power_awake_notification_();
  } else if (action == TuyaLowPowerStandbyState::ResumeAction::RESUME_ACTION_REQUIRE_RESYNC) {
    // Only a wake by the MCU's own line proves that the MCU expects CMD12/00
    this->fail_low_power_transport_(LOG_STR("Unexpected wake, no CMD12/00"));
    // A version-3 MCU sends no boot heartbeat: wake it; the status poll then reads its state
    if (this->lp_v3_)
      this->send_low_power_awake_notification_();
  }
  this->update_low_power_sleep_lock_();
}

// Wakes the MCU for a command of the module's own; when it sleeps under its consent, the consent ends first and
// the wake notice follows the pulse.
void TuyaUART::wake_low_power_mcu_() {
  this->lp_cmd38_deferred_ = false;
  if (this->lp_standby_.requested()) {
    const bool compensate = this->lp_standby_.force_cancel();
    this->lp_host_->request_standby(false);
    this->begin_mcu_wake_pulse_();
    if (compensate) {
      if (this->lp_mcu_wake_pulse_active_) {
        this->lp_awake_notification_pending_ = true;
      } else {
        this->send_low_power_awake_notification_();
      }
    }
  } else {
    this->begin_mcu_wake_pulse_();
  }
  this->update_low_power_sleep_lock_();
}

void TuyaUART::poll_low_power_v3_sleep_() {
  if (!this->lp_v3_ || this->lp_product_.sleep_mode != 2)
    return;
  const uint32_t now = App.get_loop_component_start_time();
  if (this->lp_v3_offer_pending_) {
    if (now - this->lp_v3_offer_ms_ >= LP_V3_OFFER_TIMEOUT_MS) {
      this->lp_v3_offer_pending_ = false;
      this->lp_v3_idle_since_ms_ = now;
      ESP_LOGD(LP_TAG, "MCU did not answer the sleep offer");
    }
    return;
  }
  if (this->lp_standby_.requested() || now - this->lp_v3_idle_since_ms_ < LP_V3_SLEEP_IDLE_MS)
    return;
  const LogString *blocked = this->low_power_standby_blocker_();
  if (blocked == nullptr) {
    // The usual short pause after a wake: not worth a DEBUG line
    const LogString *link_busy = this->low_power_link_blocker_();
    if (link_busy != nullptr) {
      ESP_LOGV(LP_TAG, "No sleep offer: MCU link not idle (%s)", LOG_STR_ARG(link_busy));
      return;
    }
  }
  this->log_low_power_sleep_blocker_(blocked);
  if (blocked != nullptr)
    return;
  ESP_LOGV(LP_TAG, "Offering sleep to the MCU");
  this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_OFFER});
  this->lp_v3_offer_pending_ = true;
  this->lp_v3_offer_ms_ = now;
}

void TuyaUART::handle_low_power_v3_sleep_(const uint8_t *data, size_t len) {
  if (!this->lp_v3_ || len < 1)
    return;
  switch (data[0]) {
    case LP_V3_SLEEP_OFFER: {  // The answer to our offer: 01 sleep, 00 stay awake
      if (!this->lp_v3_offer_pending_ || len < 2)
        return;
      this->lp_v3_offer_pending_ = false;
      this->lp_v3_idle_since_ms_ = App.get_loop_component_start_time();
      if (data[1] != 0x01) {
        ESP_LOGV(LP_TAG, "MCU declines sleep (answer %u)", data[1]);
        return;
      }
      if (this->low_power_sleep_blocker_(true) != nullptr || !this->lp_standby_.accept_offer()) {
        // The MCU already sleeps and expects the module to sleep too
        this->send_low_power_awake_notification_();
        return;
      }
      ESP_LOGV(LP_TAG, "MCU agreed to sleep");
      this->lp_host_->request_standby(true);
      this->update_low_power_sleep_lock_();
      return;
    }
    case LP_V3_SLEEP_REQUEST: {  // The MCU asks to sleep; 00 grants it
      const LogString *blocker = this->low_power_sleep_blocker_(true);
      this->log_low_power_sleep_blocker_(blocker);
      uint8_t flags = 0;
      if (this->lp_host_ != nullptr && this->lp_host_->ota_busy()) {
        flags = LP_V3_BUSY_UPGRADE;
      } else if (blocker != nullptr || !this->lp_standby_.accept_offer()) {
        flags = LP_V3_BUSY_NETWORK;
      }
      if (flags != 0) {
        this->send_low_power_direct_(LP_V3_SLEEP_CMD, {LP_V3_SLEEP_REQUEST, flags});
        return;
      }
      ESP_LOGV(LP_TAG, "MCU asked to sleep");
      this->send_low_power_consent_(LP_V3_SLEEP_CMD, LP_V3_SLEEP_REQUEST);
      return;
    }
    default:  // 01: the answer to our wake notice
      return;
  }
}

}  // namespace esphome::tuya

#endif  // USE_TUYA_LOW_POWER
