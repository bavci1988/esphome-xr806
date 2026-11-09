#include "wifi_standby.h"

#if defined(USE_XR806) && defined(USE_WIFI) && defined(USE_XR806_STANDBY_CONTROLLER)

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/log.h"
#include "lowpower_policy.h"
#include "network_status.h"

extern "C" {
#include "net/wlan/wlan.h"
}

namespace esphome::xr806 {

ESPHOME_LOG_TAG(TAG, "xr806.wifi_standby");

void XR806WiFiStandby::setup() {
  this->lock_.name = const_cast<char *>("esphome.xr806.wlan");
  if (!this->claim_lock_()) {
    this->mark_failed(LOG_STR("Cannot acquire WLAN PM wakelock"));
    return;
  }
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->register_participant(this)) {
    this->mark_failed(LOG_STR("Cannot register WLAN standby participant"));
    return;
  }
  lowpower_policy().set_network_idle(false);
}

void XR806WiFiStandby::dump_config() {
  ESP_LOGCONFIG(TAG,
                "XR806 WLAN standby:\n"
                "  Network suspend: %s",
                YESNO(this->network_suspend_));
}

bool XR806WiFiStandby::claim_lock_() {
  if (this->lock_held_)
    return true;
  if (pm_wake_lock(&this->lock_) != 0)
    return false;
  this->lock_held_ = true;
  return true;
}

void XR806WiFiStandby::restore_network_() {
  if (!this->suspending_ && !this->parked_)
    return;
  auto &policy = lowpower_policy();
  policy.set_network_idle(false);  // Claim the CPU before Wi-Fi work resumes.
  policy.set_network_quiescing(false);
  if (!this->claim_lock_()) {
    ESP_LOGE(TAG, "Cannot reacquire WLAN PM wakelock; recovery blocked");
    this->status_set_error();
    return;
  }
  this->suspending_ = false;
  this->parked_ = false;
  wifi::global_wifi_component->enable();
}

void XR806WiFiStandby::standby_preflight(bool requested) {
  if (!requested) {
    this->restore_network_();
    return;
  }
  if (!this->network_suspend_ || this->parked_)
    return;  // Without network_suspend NETWORK_BUSY stays asserted.
  auto &policy = lowpower_policy();
  constexpr uint32_t other_services =
      XR806LowPowerPolicy::API_BUSY | XR806LowPowerPolicy::OTA_BUSY | XR806LowPowerPolicy::MDNS_BUSY;
  if ((policy.blockers() & other_services) != 0)
    return;  // Never take Wi-Fi down while API, mDNS or OTA still own sockets.
  if (!this->suspending_) {
    policy.set_network_quiescing(true);  // Declare the intentional link teardown first.
    wifi::global_wifi_component->disable();
    this->suspending_ = true;
  }
  const auto snapshot = read_xr806_network_snapshot(wlan_netif_get(WLAN_MODE_STA));
  if (snapshot.associated || snapshot.link_up)
    return;  // The driver has not finished the teardown yet.
  if (this->lock_held_) {
    if (pm_wake_unlock(&this->lock_) != 0) {
      ESP_LOGE(TAG, "WLAN PM wakelock release failed; suspend refused");
      return;
    }
    this->lock_held_ = false;
  }
  this->parked_ = true;
  policy.set_network_idle(true);
}

bool XR806WiFiStandby::prepare_for_standby() { return this->network_suspend_ && this->parked_ && !this->lock_held_; }

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_WIFI && USE_XR806_STANDBY_CONTROLLER
