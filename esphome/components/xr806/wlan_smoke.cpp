// Optional, non-blocking XR806 WLAN station and DTIM smoke probe.
// This is NOT the native ESPHome wifi: or API integration and does not sleep the CPU.
#include "esphome/core/defines.h"

#ifdef USE_XR806_WLAN_SMOKE

#include "esphome/components/xr806/wlan_smoke.h"

#include <cstdint>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

extern "C" {
// Match the vendor build: the WLAN declarations are conditional on CONFIG_WLAN
// and PRJCONF_NET_EN, both selected by this project's actual SDK configuration.
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "common/framework/net_ctrl.h"
#include "net/wlan/wlan.h"
#include "net/wlan/wlan_ext_req.h"
#include "lwip/ip4_addr.h"
}

namespace esphome::xr806 {
static const char *const TAG = "xr806.wlan_smoke";

void XR806WLANSmoke::setup() {
  // Vendor platform_init() has already initialized lwIP and the WLAN core.
  if (this->ssid_.empty() || this->ssid_.size() > 32 ||
      (!this->password_.empty() && (this->password_.size() < 8 || this->password_.size() > 63))) {
    ESP_LOGE(TAG, "Invalid SSID or PSK length");
    this->mark_failed();
    return;
  }

  if (net_switch_mode(WLAN_MODE_STA) != 0) {
    ESP_LOGE(TAG, "Failed to switch WLAN to station mode");
    this->mark_failed();
    return;
  }

  if (wlan_sta_set(reinterpret_cast<uint8_t *>(this->ssid_.data()),
                   static_cast<uint8_t>(this->ssid_.size()),
                   this->password_.empty() ? nullptr : reinterpret_cast<uint8_t *>(this->password_.data())) != 0) {
    ESP_LOGE(TAG, "Could not configure station credentials");
    this->mark_failed();
    return;
  }

  if (wlan_sta_enable() != 0) {
    ESP_LOGE(TAG, "Could not start WLAN station");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "Station connection requested; waiting for DHCP");
}

void XR806WLANSmoke::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 WLAN station smoke test:");
  ESP_LOGCONFIG(TAG, "  SSID: %s", this->ssid_.c_str());
  ESP_LOGCONFIG(TAG, "  Power save requested: %s", this->power_save_ ? "yes" : "no");
  if (this->power_save_) {
    ESP_LOGCONFIG(TAG, "  DTIM: %u; listen interval: %u",
                  static_cast<unsigned>(this->dtim_),
                  static_cast<unsigned>(this->listen_interval_));
  }
  ESP_LOGCONFIG(TAG, "  ESPHome Native API: not implemented");
  ESP_LOGCONFIG(TAG, "  CPU standby: not enabled");
}

void XR806WLANSmoke::apply_power_save_(void *opaque_netif) {
  auto *nif = static_cast<struct netif *>(opaque_netif);
  // Avoid reconfiguring PS on each loop; retry only after a new association.
  this->power_save_attempted_ = true;

  // Do not enable power saving until association AND a DHCP address exist.
  // Vendor example project/example/wlan_low_power/main.c uses these commands.
  if (wlan_ext_request(nif, WLAN_EXT_CMD_SET_PM_DTIM, this->dtim_) != 0) {
    ESP_LOGW(TAG, "Failed to set WLAN DTIM");
    return;
  }

  if (wlan_ext_request(nif, WLAN_EXT_CMD_SET_LISTEN_INTERVAL, this->listen_interval_) != 0) {
    ESP_LOGW(TAG, "Failed to set WLAN listen interval");
    return;
  }

  if (wlan_set_ps_mode(nif, 1) != 0) {
    ESP_LOGW(TAG, "Failed to enable WLAN power save");
    return;
  }
  ESP_LOGI(TAG, "WLAN power save configured: DTIM=%u, listen interval=%u",
           static_cast<unsigned>(this->dtim_),
           static_cast<unsigned>(this->listen_interval_));
}

void XR806WLANSmoke::loop() {
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - this->last_poll_ms_) < 1000U)
    return;
  this->last_poll_ms_ = now;

  wlan_sta_states_t state = WLAN_STA_STATE_DISCONNECTED;
  auto *nif = wlan_netif_get(WLAN_MODE_STA);
  const bool ready = (wlan_sta_state(&state) == 0 &&
                      state == WLAN_STA_STATE_CONNECTED &&
                      nif != nullptr && NETIF_IS_AVAILABLE(nif));

  if (ready != this->connected_) {
    this->connected_ = ready;
    this->power_save_attempted_ = false;
    if (ready) {
      ESP_LOGI(TAG, "WLAN connected, DHCP IP: %s", ip4addr_ntoa(netif_ip4_addr(nif)));
    } else {
      ESP_LOGW(TAG, "WLAN disconnected / IP unavailable");
    }
  }

  if (ready && this->power_save_ && !this->power_save_attempted_)
    this->apply_power_save_(nif);

  // Intentionally do not set lowpower_policy().set_network_idle(true):
  // network/API/OTA work is not yet tracked and CPU standby must remain blocked.
}

}  // namespace esphome::xr806

#endif  // USE_XR806_WLAN_SMOKE
