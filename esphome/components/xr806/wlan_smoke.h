#pragma once

#ifdef USE_XR806_WLAN_SMOKE

#include <cstdint>
#include <string>

#include "esphome/core/component.h"

namespace esphome::xr806 {

// Isolated STA/DTIM integration probe, not an ESPHome wifi:/Native-API backend.
// WLAN is initialized by the vendor platform_init() before ESPHome setup().
class XR806WLANSmoke final : public Component {
 public:
  void set_ssid(const std::string &ssid) { this->ssid_ = ssid; }
  void set_password(const std::string &password) { this->password_ = password; }
  void set_power_save(bool enabled) { this->power_save_ = enabled; }
  void set_dtim(uint8_t dtim) { this->dtim_ = dtim; }
  void set_listen_interval(uint8_t listen_interval) { this->listen_interval_ = listen_interval; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  bool is_connected() const { return this->connected_; }

 protected:
  void apply_power_save_(void *netif);

  std::string ssid_;
  std::string password_;
  uint32_t last_poll_ms_{0};
  uint8_t dtim_{1};
  uint8_t listen_interval_{0};
  bool power_save_{false};
  bool connected_{false};
  bool power_save_attempted_{false};
};

}  // namespace esphome::xr806

#endif  // USE_XR806_WLAN_SMOKE
