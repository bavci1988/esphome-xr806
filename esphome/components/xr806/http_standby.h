#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_XR806_HTTP_STANDBY) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

extern "C" {
#include "xr_config.h"
#include "pm/pm.h"
}

namespace esphome::xr806 {

/// Owns HTTP/SSE sockets and their native PM wakelock during guarded standby.
/// No listener or outstanding HTTP transaction may survive an accepted sleep, except that
/// with stay_connected the listener stays open while no request is in progress.
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
  void standby_cancelled() override;
  void standby_resumed(const XR806StandbyResume &event) override;

 protected:
  bool claim_lock_();
  bool has_captive_dns_() const;
  void restore_();
  void cancel_restore_retry_();

  struct wakelock lock_{};
  bool lock_held_{false};
  bool parked_{false};
  bool shutting_down_{false};
  bool restore_retry_armed_{false};
};

}  // namespace esphome::xr806
#endif
