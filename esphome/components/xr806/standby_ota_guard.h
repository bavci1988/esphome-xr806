#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806_STANDBY_CONTROLLER) && defined(USE_OTA_STATE_LISTENER)
#include "esphome/core/component.h"
#include "esphome/components/ota/ota_backend.h"
#include "esphome/components/ota/ota_backend_xr806.h"
#include "standby_controller.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

// OTA is a separate socket/flash writer, not automatically covered by
// Native API listener shutdown. Until every installed OTA transport exports
// a proven listener/flash-queue quiesce callback, keep OTA_BUSY asserted.
// Lifecycle events still cancel provisional CMD38 immediately.
class XR806OTAStandbyGuard final : public Component,
                                   public XR806StandbyParticipant,
                                   public ota::OTAGlobalStateListener {
 public:
  void set_http_only(bool value) { this->http_only_ = value; }

  void setup() override {
    lowpower_policy().set_ota_idle(false);
    ota::get_global_ota_callback()->add_global_state_listener(this);
    if (auto *controller = standby_controller(); controller != nullptr) {
      if (!controller->register_participant(this))
        this->mark_failed();
    } else {
      this->mark_failed();
    }
  }

  void on_ota_global_state(ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *component) override {
    (void) progress;
    (void) error;
    (void) component;
    switch (state) {
      case ota::OTA_STARTED:
      case ota::OTA_IN_PROGRESS:
        active_ = true;
        if (auto *controller = standby_controller(); controller != nullptr)
          controller->request_standby(false);
        break;
      case ota::OTA_COMPLETED:
        active_ = false;
        committed_ = true;  // Staging selection remains locked until reboot.
        break;
      case ota::OTA_ABORT:
      case ota::OTA_ERROR:
        active_ = false;
        break;
    }
    // Never infer listener closure or flash-drain completion from an
    // OTA progress/finish callback. An unverified OTA backend stays busy.
    lowpower_policy().set_ota_idle(false);
  }

  uint32_t owned_services() const override { return XR806LowPowerPolicy::OTA_BUSY; }
  // The native ESPHome OTA socket has no verified suspend lifecycle yet.
  // Only HTTP-only OTA may quiesce, and only after its shared HTTP listener
  // has stopped accepting and all clients/flash writers have drained.
  bool may_quiesce() const override { return http_only_ && !active_ && !committed_; }
  void standby_preflight(bool requested) override {
    bool ready = requested && this->may_quiesce() && !ota::XR806OTABackend::flash_busy() &&
                 !(lowpower_policy().blockers() & XR806LowPowerPolicy::HTTP_BUSY);
    lowpower_policy().set_ota_idle(ready);
  }
  bool prepare_for_standby() override {
    return this->may_quiesce() && !ota::XR806OTABackend::flash_busy() &&
           !(lowpower_policy().blockers() & XR806LowPowerPolicy::HTTP_BUSY);
  }
  void standby_cancelled() override { lowpower_policy().set_ota_idle(false); }
  void standby_resumed(const XR806StandbyResume &event) override {
    (void) event;
    lowpower_policy().set_ota_idle(false);
  }
  bool active() const { return active_; }

 private:
  bool active_{false};
  bool committed_{false};
  bool http_only_{false};
};

}  // namespace esphome::xr806
#endif
