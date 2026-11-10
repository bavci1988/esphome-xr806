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
  if (!parked_)
    return;
  auto &policy = lowpower_policy();
  // Claim the policy blocker before touching sockets or the PM wakelock.
  policy.set_http_idle(false);
  if (!this->claim_lock_()) {
    ESP_LOGE(TAG, "HTTP PM wakelock re-acquire failed; refuse resume");
    this->mark_failed();
    return;
  }
  auto *base = web_server_base::global_web_server_base;
  if (base == nullptr || base->get_server() == nullptr) {
    ESP_LOGE(TAG, "Cannot restore native HTTP listener");
    this->mark_failed();
    return;
  }
  base->get_server()->begin();
  parked_ = false;
}

void XR806HTTPStandby::standby_preflight(bool requested) {
  if (!requested) {
    this->restore_();
    return;
  }
  if (!this->may_quiesce())
    return;
  auto *base = web_server_base::global_web_server_base;
  if (base == nullptr || base->get_server() == nullptr)
    return;  // Fail closed until the shared server actually exists.
  auto *server = base->get_server();
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
  return parked_ && !lock_held_ && !this->has_captive_dns_() && base != nullptr && base->get_server() != nullptr &&
         !base->get_server()->has_active_connections();
}

void XR806HTTPStandby::standby_cancelled() { this->restore_(); }

void XR806HTTPStandby::standby_resumed(const XR806StandbyResume & /*event*/) { this->restore_(); }

}  // namespace esphome::xr806

#endif
