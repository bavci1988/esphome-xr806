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
  return !this->shutting_down_ && !api::global_api_server->is_connected();
#endif
}

void XR806APIStandby::standby_preflight(bool requested) {
  lowpower_policy().set_api_idle(requested && this->may_quiesce());
}

void XR806APIStandby::standby_cancelled() { lowpower_policy().set_api_idle(false); }

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_API && USE_XR806_STANDBY_CONTROLLER
