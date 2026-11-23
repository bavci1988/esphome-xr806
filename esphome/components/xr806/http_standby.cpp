#include "http_standby.h"

#if defined(USE_XR806_HTTP_STANDBY) && defined(USE_XR806_STANDBY_CONTROLLER)

#include <xr806_pm.h>

#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.http_standby");

// Time for requests already accepted to finish after the listener closed.
static constexpr uint32_t CLIENT_DRAIN_MS = 2000;

static AsyncWebServer *http_server() {
  auto *base = web_server_base::global_web_server_base;
  return base != nullptr ? base->get_server() : nullptr;
}

bool XR806HTTPStandby::claim_lock_() {
  if (this->lock_held_)
    return true;
  if (!xr806_wakelock_acquire(this->lock_))
    return false;
  this->lock_held_ = true;
  return true;
}

bool XR806HTTPStandby::release_lock_() {
  if (!this->lock_held_)
    return true;
  if (!xr806_wakelock_release(this->lock_)) {
    ESP_LOGE(TAG, "HTTP PM wakelock release failed");
    return false;
  }
  this->lock_held_ = false;
  return true;
}

bool XR806HTTPStandby::has_captive_portal_() const {
#ifdef USE_CAPTIVE_PORTAL
  return captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active();
#else
  return false;
#endif
}

void XR806HTTPStandby::setup() {
  this->lock_ = xr806_wakelock_create("esphome.xr806.http");
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

void XR806HTTPStandby::dump_config() { ESP_LOGCONFIG(TAG, "XR806 HTTP standby: listener and captive portal guarded"); }

void XR806HTTPStandby::on_shutdown() {
  this->shutting_down_ = true;
  lowpower_policy().set_http_idle(false);
  if (auto *controller = standby_controller(); controller != nullptr) {
    controller->request_standby(false);
    controller->request_preflight(false);
  }
}

bool XR806HTTPStandby::may_quiesce() const {
  // No sleeping while the captive portal serves the setup access point.
  return !this->shutting_down_ && !this->has_captive_portal_();
}

void XR806HTTPStandby::restore_() {
  // Claim the policy blocker before the listener and the wakelock come back, so a failed
  // restore can never let the device sleep.
  lowpower_policy().set_http_idle(false);
  if (!this->claim_lock_())
    this->status_set_warning();
  if (!this->parked_)
    return;
  if (auto *server = http_server(); server != nullptr)
    server->begin();
  this->parked_ = false;
}

void XR806HTTPStandby::standby_preflight(bool requested) {
  if (!requested) {
    this->restore_();
    return;
  }
  auto *server = http_server();
  if (!this->may_quiesce() || server == nullptr)
    return;  // Fail closed until the shared server actually exists.
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->stay_connected()) {
    if (!this->parked_) {
      // Stop new requests; the accepted ones get CLIENT_DRAIN_MS to finish.
      server->end();
      this->parked_ = true;
      this->parked_ms_ = App.get_loop_component_start_time();
    }
    if (App.get_loop_component_start_time() - this->parked_ms_ < CLIENT_DRAIN_MS)
      return;
  }
  if (this->release_lock_())
    lowpower_policy().set_http_idle(true);
}

bool XR806HTTPStandby::prepare_for_standby() {
  auto *controller = standby_controller();
  const bool listener_ready = this->parked_ || (controller != nullptr && controller->stay_connected());
  return listener_ready && !this->lock_held_ && this->may_quiesce() && http_server() != nullptr;
}

}  // namespace esphome::xr806

#endif  // USE_XR806_HTTP_STANDBY && USE_XR806_STANDBY_CONTROLLER
