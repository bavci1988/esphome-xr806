#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_WIFI) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

extern "C" {
#include "xr_config.h"
#include "pm/pm.h"
}

namespace esphome::xr806 {

/// Standby owner of the WLAN link.
///
/// Holds a native SDK PM wakelock while the station is active, so no other PM path can
/// suspend the radio under ESPHome. With network_suspend enabled it parks the standard
/// wifi component (disable()) once API, mDNS and OTA are idle, and enables it again when
/// standby is cancelled or after resume. Without it, it keeps NETWORK_BUSY asserted.
class XR806WiFiStandby final : public Component, public XR806StandbyParticipant {
 public:
  explicit XR806WiFiStandby(bool network_suspend) : network_suspend_(network_suspend) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void standby_preflight(bool requested) override;
  bool may_quiesce() const override { return this->network_suspend_; }
  uint32_t owned_services() const override { return XR806LowPowerPolicy::NETWORK_BUSY; }
  bool prepare_for_standby() override;
  void standby_cancelled() override { this->restore_network_(); }
  void standby_resumed(const XR806StandbyResume & /*event*/) override { this->restore_network_(); }

 protected:
  bool claim_lock_();
  void restore_network_();

  struct wakelock lock_{};
  bool lock_held_{false};
  const bool network_suspend_;
  bool suspending_{false};
  bool parked_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_WIFI && USE_XR806_STANDBY_CONTROLLER
