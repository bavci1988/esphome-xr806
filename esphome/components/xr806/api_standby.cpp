#include "api_standby.h"

#if defined(USE_XR806) && defined(USE_API) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/components/api/api_server.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

void XR806APIStandby::setup() {
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->register_participant(this)) {
    this->mark_failed(LOG_STR("Cannot register Native API standby participant"));
    return;
  }
  lowpower_policy().set_api_idle(false);
}

void XR806APIStandby::on_shutdown() {
  this->shutting_down_ = true;
  lowpower_policy().set_api_idle(false);
  if (auto *controller = standby_controller(); controller != nullptr) {
    controller->request_standby(false);
    controller->request_preflight(false);
  }
}

bool XR806APIStandby::may_quiesce() const {
#ifdef USE_API_OUTGOING_CONNECTION
  // Dial-out can start network work without an inbound connection.
  return false;
#else
  if (this->shutting_down_)
    return false;
  // With the link kept up a connected client stays connected; its next packet wakes the chip.
  auto *controller = standby_controller();
  if (controller != nullptr && controller->stay_connected())
    return true;
  return !api::global_api_server->is_connected();
#endif
}

#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
void XR806APIStandby::standby_committing() {
  auto *controller = standby_controller();
  if (controller != nullptr)
    this->offload_.arm(controller->wake_after_ms());
}
#endif

bool XR806APIStandby::clients_idle_() const {
  for (const auto &client : api::global_api_server->active_clients()) {
    if (!client->is_idle())
      return false;
  }
  return true;
}

void XR806APIStandby::standby_preflight(bool requested) {
  lowpower_policy().set_api_idle(requested && this->may_quiesce() && this->clients_idle_());
}

bool XR806APIStandby::prepare_for_standby() {
  // With the keepalive experiment the connection is handed to the WLAN firmware in standby_committing(),
  // once nothing can refuse standby any more.
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  if (this->may_quiesce() && this->clients_idle_() && !this->offload_.ready_for_standby())
    return false;
#endif
  return this->may_quiesce() && this->clients_idle_();
}

void XR806APIStandby::standby_cancelled() {
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  this->offload_.disarm(nullptr);
#endif
  lowpower_policy().set_api_idle(false);
}

void XR806APIStandby::standby_resumed(const XR806StandbyResume &event) {
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  this->offload_.disarm(&event);
#endif
  lowpower_policy().set_api_idle(false);
}

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_API && USE_XR806_STANDBY_CONTROLLER
