#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_API) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

namespace esphome::xr806 {

/// Standby owner of the Native API, using only the public APIServer interface.
///
/// The API votes idle only while standby is being prepared and no client is
/// connected. A client that connects later flips the vote back to busy, which aborts
/// the attempt; once the WLAN link is parked no new client can arrive.
class XR806APIStandby final : public Component, public XR806StandbyParticipant {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  // Reboot or shutdown is new work: veto PM before any component tears down.
  void on_shutdown() override;

  void standby_preflight(bool requested) override;
  bool may_quiesce() const override;
  uint32_t owned_services() const override { return XR806LowPowerPolicy::API_BUSY; }
  bool prepare_for_standby() override { return this->may_quiesce(); }
  void standby_cancelled() override;
  void standby_resumed(const XR806StandbyResume & /*event*/) override { this->standby_cancelled(); }

 protected:
  bool shutting_down_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_API && USE_XR806_STANDBY_CONTROLLER
