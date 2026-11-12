#include "http_standby.h"

#if defined(USE_XR806) && defined(USE_XR806_HTTP_STANDBY) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif
#ifdef USE_WEBSERVER_CAPTIVE
#include "esphome/components/web_server/web_server.h"
#endif
#include "esphome/core/log.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.http_standby");

static constexpr uint32_t RESTORE_RETRY_ID = 0;
static constexpr uint32_t RESTORE_RETRY_MS = 1000;

bool XR806HTTPStandby::claim_lock_() {
  if (lock_held_)
    return true;
  if (pm_wake_lock(&lock_) != 0)
    return false;
  lock_held_ = true;
  return true;
}

bool XR806HTTPStandby::has_captive_dns_() const {
#ifdef USE_CAPTIVE_PORTAL
  if (captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active())
    return true;
#endif
#ifdef USE_WEBSERVER_CAPTIVE
  if (web_server::global_web_server != nullptr && web_server::global_web_server->is_captive())
    return true;
#endif
  return false;
}

void XR806HTTPStandby::setup() {
  this->lock_.name = const_cast<char *>("esphome.xr806.http");
  if (!this->claim_lock_()) {
    this->mark_failed(LOG_STR("HTTP PM wakelock unavailable"));
    return;
  }
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->register_participant(this)) {
    this->mark_failed(LOG_STR("HTTP standby participant registration failed"));
    return;
  }
  lowpower_policy().set_http_idle(false);
}

void XR806HTTPStandby::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 HTTP standby: listener/clients/SSE and captive DNS guarded");
}

void XR806HTTPStandby::on_shutdown() {
  this->shutting_down_ = true;
  lowpower_policy().set_http_idle(false);
  if (auto *controller = standby_controller(); controller != nullptr) {
    controller->request_standby(false);
    controller->request_preflight(false);
  }
}

bool XR806HTTPStandby::may_quiesce() const {
  // No sleeping while captive AP/DNS is serving clients.
  return !this->shutting_down_ && !this->has_captive_dns_();
}

void XR806HTTPStandby::restore_() {
  if (!this->parked_) {
    this->cancel_restore_retry_();
    // With stay_connected the listener stays open and only the wakelock was let go.
    if (!this->lock_held_) {
      lowpower_policy().set_http_idle(false);
      if (!this->claim_lock_())
        this->status_set_warning();
    }
    return;
  }
  // Claim the policy blocker before touching sockets or the PM wakelock, so a failed
  // restore can never let the device sleep.
  lowpower_policy().set_http_idle(false);
  auto *base = web_server_base::global_web_server_base;
  auto *server = base != nullptr ? base->get_server() : nullptr;
  if (!this->claim_lock_() || server == nullptr || !server->begin()) {
    // Stay parked and try again; the listener must come back for the device to stay reachable.
    ESP_LOGW(TAG, "Cannot reopen the HTTP listener; retrying");
    this->status_set_warning();
    if (!this->restore_retry_armed_) {
      this->restore_retry_armed_ = true;
      this->set_interval(RESTORE_RETRY_ID, RESTORE_RETRY_MS, [this]() { this->restore_(); });
    }
    return;
  }
  this->parked_ = false;
  this->cancel_restore_retry_();
  this->status_clear_warning();
}

void XR806HTTPStandby::cancel_restore_retry_() {
  if (this->restore_retry_armed_) {
    this->cancel_interval(RESTORE_RETRY_ID);
    this->restore_retry_armed_ = false;
  }
}

void XR806HTTPStandby::standby_preflight(bool requested) {
  if (!requested) {
    this->restore_();
    return;
  }
  if (!this->may_quiesce())
    return;
  // A new standby attempt parks the listener on purpose; stop reopening it.
  this->cancel_restore_retry_();
  auto *base = web_server_base::global_web_server_base;
  if (base == nullptr || base->get_server() == nullptr)
    return;  // Fail closed until the shared server actually exists.
  auto *server = base->get_server();
  auto *controller = standby_controller();
  if (controller != nullptr && controller->stay_connected()) {
    // The listener stays open: a new request wakes the chip, and an open request or event
    // stream keeps it awake.
    if (server->has_active_connections()) {
      this->restore_();
      return;
    }
    if (lock_held_) {
      if (pm_wake_unlock(&lock_) != 0) {
        ESP_LOGE(TAG, "HTTP PM wakelock release failed");
        return;
      }
      lock_held_ = false;
    }
    lowpower_policy().set_http_idle(true);
    return;
  }
  if (!parked_) {
    // Stop new requests, but do not kill an accepted upload or live SSE.
    server->stop_accepting();
    parked_ = true;
  }
  if (server->has_active_connections())
    return;
  if (lock_held_) {
    if (pm_wake_unlock(&lock_) != 0) {
      ESP_LOGE(TAG, "HTTP PM wakelock release failed");
      return;
    }
    lock_held_ = false;
  }
  lowpower_policy().set_http_idle(true);
}

bool XR806HTTPStandby::prepare_for_standby() {
  auto *base = web_server_base::global_web_server_base;
  auto *controller = standby_controller();
  const bool listener_ready = parked_ || (controller != nullptr && controller->stay_connected());
  return listener_ready && !lock_held_ && !this->has_captive_dns_() && base != nullptr &&
         base->get_server() != nullptr && !base->get_server()->has_active_connections();
}

void XR806HTTPStandby::standby_cancelled() { this->restore_(); }

void XR806HTTPStandby::standby_resumed(const XR806StandbyResume & /*event*/) { this->restore_(); }

}  // namespace esphome::xr806

#endif
