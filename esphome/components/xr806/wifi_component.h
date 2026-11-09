#pragma once

#ifdef USE_XR806_WLAN_SMOKE

#include <cstdint>
#include <string>

#include "esphome/core/component.h"
#include "esphome/components/xr806/wlan_reconnect.h"
#include "esphome/components/xr806/wifi_power_save_retry.h"
#ifdef USE_XR806_STANDBY_CONTROLLER
#include "esphome/components/xr806/standby_controller.h"
extern "C" {
#include "xr_config.h"
#include "pm/pm.h"
}
#endif

namespace esphome::xr806 {

// XR806 STA/DTIM backend for standard ESPHome wifi: and legacy probes.
// WLAN is initialized by the vendor platform_init() before ESPHome setup().
class XR806WiFiComponent final : public Component
#ifdef USE_XR806_STANDBY_CONTROLLER
    , public XR806StandbyParticipant
#endif
{
 public:
  void set_ssid(const std::string &ssid) { this->ssid_ = ssid; }
  void set_password(const std::string &password) { this->password_ = password; }
  void set_power_save(bool enabled) { this->power_save_ = enabled; }
#ifdef USE_XR806_STANDBY_CONTROLLER
  void set_network_suspend(bool enabled) { this->network_suspend_opt_in_ = enabled; }
  void standby_preflight(bool requested) override;
  bool may_quiesce() const override { return this->network_suspend_opt_in_; }
  bool prepare_for_standby() override;
  void standby_cancelled() override;
  void standby_resumed(const XR806StandbyResume &event) override;
#endif
  void set_socket_probe(bool enabled) { this->socket_probe_enabled_ = enabled; }
  void set_socket_facade_probe(bool enabled) { this->socket_facade_probe_enabled_ = enabled; }
  void set_dtim(uint8_t dtim) { this->dtim_ = dtim; }
  void set_listen_interval(uint8_t listen_interval) { this->listen_interval_ = listen_interval; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  bool is_connected() const { return this->connected_; }
  const std::string &get_ip_address() const { return this->ip_address_; }
  bool has_rssi() const { return this->rssi_valid_; }
  int rssi_dbm() const { return this->rssi_dbm_; }
  const std::string &ip_address() const { return this->ip_address_; }
  uint32_t reconnect_attempts() const { return this->reconnect_.attempts(); }

 protected:
  void apply_power_save_(void *netif, uint32_t now);
  void probe_socket_();
  void probe_socket_facade_();

  std::string ssid_;
  std::string password_;
  uint32_t last_poll_ms_{0};
  uint8_t dtim_{1};
  uint8_t listen_interval_{0};
  bool power_save_{false};
  bool connected_{false};
#ifdef USE_XR806_STANDBY_CONTROLLER
  struct wakelock network_pm_lock_{};
  bool network_pm_lock_held_{false};
  bool network_suspend_opt_in_{false};
  bool network_suspending_{false};
  bool network_parked_{false};
  bool claim_network_lock_();
  void restore_network_();
#endif
  XR806WiFiPowerSaveRetry power_save_retry_{};
  bool socket_probe_enabled_{false};
  bool socket_probe_done_{false};
  bool socket_facade_probe_enabled_{false};
  bool socket_facade_probe_done_{false};
  bool rssi_valid_{false};
  int rssi_dbm_{0};
  std::string ip_address_;
  uint32_t last_rssi_poll_ms_{0};
  XR806WLANReconnect reconnect_;
};

#if defined(USE_XR806) && defined(USE_XR806_NETWORK_BRIDGE)
extern XR806WiFiComponent *global_xr806_wifi;
#endif

}  // namespace esphome::xr806

#endif  // USE_XR806_WLAN_SMOKE
