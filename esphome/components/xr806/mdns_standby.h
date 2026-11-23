#pragma once

#include "esphome/core/defines.h"

#if defined(USE_MDNS) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/core/component.h"
#include "standby_controller.h"
#include "lowpower_policy.h"

namespace esphome::xr806 {

/// Standby owner of the mDNS responder.
///
/// The LibreTiny responder runs in the lwIP thread and answers only received queries: with the
/// link kept up a query wakes the chip, and with network_suspend the responder goes down with the
/// link. So it votes idle whenever standby is being prepared.
class XR806MDNSStandby final : public Component, public XR806StandbyParticipant {
 public:
  void setup() override {
    auto *controller = standby_controller();
    if (controller == nullptr || !controller->register_participant(this)) {
      this->mark_failed(LOG_STR("Cannot register mDNS standby participant"));
      return;
    }
    lowpower_policy().set_mdns_idle(false);
  }
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void standby_preflight(bool requested) override { lowpower_policy().set_mdns_idle(requested); }
  uint32_t owned_services() const override { return XR806LowPowerPolicy::MDNS_BUSY; }
  bool prepare_for_standby() override { return true; }
  void standby_cancelled() override { lowpower_policy().set_mdns_idle(false); }
  void standby_resumed(const XR806StandbyResume & /*event*/) override { lowpower_policy().set_mdns_idle(false); }
};

}  // namespace esphome::xr806

#endif  // USE_MDNS && USE_XR806_STANDBY_CONTROLLER
