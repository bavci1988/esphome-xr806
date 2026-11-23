#pragma once

#include "esphome/core/defines.h"

#if defined(USE_MQTT) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

namespace esphome::mqtt {
class MQTTClientComponent;
}  // namespace esphome::mqtt

namespace esphome::xr806 {

/// Standby owner of the MQTT broker connection.
///
/// While standby is being prepared the client is disabled, which sends a DISCONNECT, so the broker can keep the
/// device's birth message instead of publishing the will (best effort: confirmed on a host broker, not yet on WXU
/// hardware). The client sends the DISCONNECT and closes the socket on its own task, so MQTT votes idle a fixed time
/// after it was disabled. On wake, or when the attempt is cancelled, the client is enabled again unless it was
/// already disabled before.
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
  /// The client is off and had the time to close its connection
  bool quiet_(const mqtt::MQTTClientComponent *client) const;

  uint32_t parked_ms_{0};
  bool parked_{false};
  /// The client was enabled when it was parked, so it is enabled again afterwards
  bool reenable_{false};
  bool shutting_down_{false};
};

}  // namespace esphome::xr806

#endif  // USE_MQTT && USE_XR806_STANDBY_CONTROLLER
