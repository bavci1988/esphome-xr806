#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_MQTT) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

namespace esphome::xr806 {

/// Standby owner of the MQTT broker connection.
///
/// While standby is being prepared the client is disabled, which sends a DISCONNECT, so the broker keeps the
/// device's birth message and does not publish the will. MQTT votes idle only once the socket is closed. On wake,
/// or when the attempt is cancelled, the client is enabled again unless it was already disabled before.
class XR806MQTTStandby final : public Component, public XR806StandbyParticipant {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  // Reboot or shutdown is new work: veto PM before any component tears down.
  void on_shutdown() override;

  uint32_t owned_services() const override { return XR806LowPowerPolicy::MQTT_BUSY; }
  bool may_quiesce() const override { return !this->shutting_down_; }
  void standby_preflight(bool requested) override;
  bool prepare_for_standby() override;
  void standby_cancelled() override { this->restore_(); }
  void standby_resumed(const XR806StandbyResume & /*event*/) override { this->restore_(); }

 protected:
  void restore_();

  bool parked_{false};
  /// The client was enabled when it was parked, so it is enabled again afterwards
  bool reenable_{false};
  bool shutting_down_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_MQTT && USE_XR806_STANDBY_CONTROLLER
