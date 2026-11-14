#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_API) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"

#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
namespace esphome::api {
class APIConnection;
}  // namespace esphome::api
#endif

namespace esphome::xr806 {

/// Standby owner of the Native API, using only the public APIServer interface.
///
/// The API votes idle only while standby is being prepared and no client is
/// connected. A client that connects later flips the vote back to busy, which aborts
/// the attempt; once the WLAN link is parked no new client can arrive. With
/// stay_connected the clients stay connected and the vote is idle while none of
/// them has data to read or write.
class XR806APIStandby final : public Component, public XR806StandbyParticipant {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  // Reboot or shutdown is new work: veto PM before any component tears down.
  void on_shutdown() override;

  void standby_preflight(bool requested) override;
  bool may_quiesce() const override;
  uint32_t owned_services() const override { return XR806LowPowerPolicy::API_BUSY; }
  bool prepare_for_standby() override;
  void standby_cancelled() override;
  void standby_resumed(const XR806StandbyResume &event) override;
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  /// Experiment: while the CPU sleeps, the WLAN firmware sends one pre-encrypted PingResponse every period_s.
  void set_keepalive_offload(uint8_t period_s, bool host_sleep, bool wake_on_peer) {
    this->offload_period_s_ = period_s;
    this->offload_host_sleep_ = host_sleep;
    this->offload_wake_on_peer_ = wake_on_peer;
  }
#endif

 protected:
  bool clients_idle_() const;
#ifdef USE_XR806_API_KEEPALIVE_OFFLOAD
  void arm_offload_();
  void disarm_offload_(const XR806StandbyResume *event);

  api::APIConnection *offload_client_{nullptr};
  uint32_t offload_armed_ms_{0};
  uint32_t offload_snd_nxt_{0};
  uint32_t offload_rcv_nxt_{0};
  int offload_fd_{-1};
  uint8_t offload_period_s_{0};
  bool offload_host_sleep_{false};
  bool offload_wake_on_peer_{false};
#endif

  bool shutting_down_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_API && USE_XR806_STANDBY_CONTROLLER
