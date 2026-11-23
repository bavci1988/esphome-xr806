#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806_STANDBY_CONTROLLER) && defined(USE_OTA_STATE_LISTENER)

#include "esphome/components/ota/ota_backend.h"
#include "esphome/core/component.h"
#include "standby_controller.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

/// Standby owner of OTA updates.
///
/// A running update keeps the chip awake, and so does a committed one until the reboot that
/// installs it. The native OTA listener is a socket of its own: only with stay_connected, where
/// a connecting client wakes the chip and the OTA loop, may it stay open during standby. Web
/// server uploads share the HTTP listener, so they follow the HTTP vote.
class XR806OTAStandbyGuard final : public Component,
                                   public XR806StandbyParticipant,
                                   public ota::OTAGlobalStateListener {
 public:
  void set_http_only(bool value) { this->http_only_ = value; }
  void setup() override {
    lowpower_policy().set_ota_idle(false);
    ota::get_global_ota_callback()->add_global_state_listener(this);
    auto *controller = standby_controller();
    if (controller == nullptr || !controller->register_participant(this))
      this->mark_failed(LOG_STR("Cannot register OTA standby participant"));
  }
  void on_ota_global_state(ota::OTAState state, float /*progress*/, uint8_t /*error*/,
                           ota::OTAComponent * /*component*/) override {
    switch (state) {
      case ota::OTA_STARTED:
      case ota::OTA_IN_PROGRESS:
        update_busy_ = true;
        if (auto *controller = standby_controller(); controller != nullptr)
          controller->request_standby(false);
        break;
      case ota::OTA_COMPLETED:
        committed_ = true;  // The update stays staged until the reboot.
        break;
      case ota::OTA_ABORT:
      case ota::OTA_ERROR:
        update_busy_ = committed_;
        break;
    }
    lowpower_policy().set_ota_idle(false);
  }
  /// An update is being written or waits for its reboot.
  static bool update_busy() { return update_busy_; }

  uint32_t owned_services() const override { return XR806LowPowerPolicy::OTA_BUSY; }
  bool may_quiesce() const override {
    auto *controller = standby_controller();
    const bool listener_may_stay = controller != nullptr && controller->stay_connected();
    return (this->http_only_ || listener_may_stay) && !update_busy_;
  }
  void standby_preflight(bool requested) override {
    lowpower_policy().set_ota_idle(requested && this->prepare_for_standby());
  }
  bool prepare_for_standby() override {
    return this->may_quiesce() && !(lowpower_policy().blockers() & XR806LowPowerPolicy::HTTP_BUSY);
  }
  void standby_cancelled() override { lowpower_policy().set_ota_idle(false); }
  void standby_resumed(const XR806StandbyResume & /*event*/) override { lowpower_policy().set_ota_idle(false); }

 private:
  static inline bool update_busy_{false};
  bool committed_{false};
  bool http_only_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806_STANDBY_CONTROLLER && USE_OTA_STATE_LISTENER
