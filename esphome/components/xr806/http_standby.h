#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806_HTTP_STANDBY) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

namespace esphome::xr806 {

/// Standby owner of the web server listener and its PM wakelock.
///
/// ESPAsyncWebServer serves its clients on the lwIP thread and cannot report its open
/// connections. With stay_connected the listener stays open: a new request wakes the chip.
/// Otherwise the listener is closed before standby and the clients get a fixed time to finish.
/// While the captive portal serves the setup access point, the chip stays awake.
class XR806HTTPStandby final : public Component, public XR806StandbyParticipant {
 public:
  void setup() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  uint32_t owned_services() const override { return XR806LowPowerPolicy::HTTP_BUSY; }
  bool may_quiesce() const override;
  void standby_preflight(bool requested) override;
  bool prepare_for_standby() override;
  void standby_cancelled() override { this->restore_(); }
  void standby_resumed(const XR806StandbyResume & /*event*/) override { this->restore_(); }

 protected:
  bool claim_lock_();
  bool release_lock_();
  bool has_captive_portal_() const;
  void restore_();

  void *lock_{nullptr};
  uint32_t parked_ms_{0};
  bool lock_held_{false};
  bool parked_{false};
  bool shutting_down_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806_HTTP_STANDBY && USE_XR806_STANDBY_CONTROLLER
