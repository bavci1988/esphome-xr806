// XR806 WLAN station backend (legacy smoke probes remain optional).
// Supports ESPHome top-level wifi:; CPU standby remains blocked in Stage 7a.
#include "esphome/core/defines.h"

#ifdef USE_XR806_WLAN_SMOKE

#include "esphome/components/xr806/wifi_component.h"
#include "esphome/components/xr806/network_status.h"
#ifdef USE_XR806_STANDBY_CONTROLLER
#include "esphome/components/xr806/lowpower_policy.h"
#endif
#if defined(USE_XR806) && defined(USE_XR806_NETWORK_BRIDGE)
#include "esphome/components/network/util.h"
#endif

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

// Use explicit lwip_* functions, avoiding POSIX compatibility macros.
#undef LWIP_COMPAT_SOCKETS
#define LWIP_COMPAT_SOCKETS 0
#include "lwip/sockets.h"
}

#ifdef USE_XR806_SOCKET_FACADE_PROBE
#include "esphome/components/xr806/socket_compat.h"
#include "esphome/components/socket/socket.h"
#endif

namespace esphome::xr806 {
#if defined(USE_XR806) && defined(USE_XR806_NETWORK_BRIDGE)
XR806WiFiComponent *global_xr806_wifi = nullptr;
#endif

static const char *const TAG = "xr806.wifi";

void XR806WiFiComponent::setup() {
#if defined(USE_XR806) && defined(USE_XR806_NETWORK_BRIDGE)
  global_xr806_wifi = this;
#endif
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

#ifdef USE_XR806_STANDBY_CONTROLLER
  this->network_pm_lock_.name = const_cast<char *>("esphome.xr806.wlan");
  if (!this->claim_network_lock_()) {
    ESP_LOGE(TAG, "Cannot acquire XR806 native WLAN PM wakelock");
    this->mark_failed();
    return;
  }
  auto *controller = standby_controller();
  if (controller == nullptr || !controller->register_participant(this)) {
    ESP_LOGE(TAG, "Cannot register WLAN PM lifecycle owner");
    this->mark_failed();
    return;
  }
  lowpower_policy().set_network_idle(false);
#endif
  if (wlan_sta_enable() != 0) {
    ESP_LOGE(TAG, "Could not start WLAN station");
    this->mark_failed();
    return;
  }

  this->reconnect_.begin(millis());
  ESP_LOGI(TAG, "Station connection requested; waiting for DHCP");
}

void XR806WiFiComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "XR806 WLAN station:");
  ESP_LOGCONFIG(TAG, "  SSID: %s", this->ssid_.c_str());
  ESP_LOGCONFIG(TAG, "  Power save requested: %s", this->power_save_ ? "yes" : "no");
  if (this->power_save_) {
    ESP_LOGCONFIG(TAG, "  DTIM: %u; listen interval: %u",
                  static_cast<unsigned>(this->dtim_),
                  static_cast<unsigned>(this->listen_interval_));
  }
  ESP_LOGCONFIG(TAG, "  Native API: available when api: is configured");
  ESP_LOGCONFIG(TAG, "  CPU standby: not enabled");
  ESP_LOGCONFIG(TAG, "  TCP socket probe: %s",
                this->socket_probe_enabled_ ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  ESPHome socket facade probe: %s",
                this->socket_facade_probe_enabled_ ? "enabled" : "disabled");
}


#ifdef USE_XR806_STANDBY_CONTROLLER
bool XR806WiFiComponent::claim_network_lock_() {
  if (this->network_pm_lock_held_)
    return true;
  if (pm_wake_lock(&this->network_pm_lock_) != 0)
    return false;
  this->network_pm_lock_held_ = true;
  return true;
}

void XR806WiFiComponent::restore_network_() {
  if (!this->network_suspending_ && !this->network_parked_)
    return;
  auto &policy = lowpower_policy();
  policy.set_network_idle(false);  // Claim CPU before resuming Wi-Fi activity.
  policy.set_network_quiescing(false);
  if (!this->claim_network_lock_()) {
    ESP_LOGE(TAG, "Failed to reacquire WLAN native PM wakelock; recovery blocked");
    this->mark_failed();
    return;
  }
  this->network_suspending_ = false;
  this->network_parked_ = false;
  if (wlan_sta_enable() != 0) {
    ESP_LOGE(TAG, "WLAN station restart after aborted PM failed");
    this->mark_failed();
    return;
  }
  this->reconnect_.begin(millis());
  this->connected_ = false;
  this->ip_address_.clear();
  this->power_save_retry_.reset();
}

void XR806WiFiComponent::standby_preflight(bool requested) {
  if (!requested) {
    this->restore_network_();
    return;
  }
  if (!this->network_suspend_opt_in_)
    return;  // Default retains NETWORK_BUSY forever.
  if (this->network_parked_)
    return;
  auto &policy = lowpower_policy();
  const uint32_t other_services = XR806LowPowerPolicy::API_BUSY | XR806LowPowerPolicy::OTA_BUSY |
                                   XR806LowPowerPolicy::MDNS_BUSY;
  if ((policy.blockers() & other_services) != 0)
    return;  // Never disable Wi-Fi while API/mDNS/OTA still own sockets.
  if (!this->network_suspending_) {
    policy.set_network_quiescing(true);  // Declare intentional link teardown first.
    if (wlan_sta_disable() != 0) {
      policy.set_network_quiescing(false);
      ESP_LOGW(TAG, "WLAN STA disable refused; retaining PM blocker");
      return;
    }
    this->network_suspending_ = true;
  }
  const auto snapshot = read_xr806_network_snapshot(wlan_netif_get(WLAN_MODE_STA));
  if (snapshot.associated || snapshot.link_up)
    return;  // Driver callback has not completed yet.
  if (this->network_pm_lock_held_) {
    if (pm_wake_unlock(&this->network_pm_lock_) != 0) {
      ESP_LOGE(TAG, "WLAN PM wakelock release failed; suspend refused");
      return;
    }
    this->network_pm_lock_held_ = false;
  }
  this->network_parked_ = true;
  policy.set_network_idle(true);
}

bool XR806WiFiComponent::prepare_for_standby() {
  return this->network_suspend_opt_in_ && this->network_parked_ &&
         !this->network_pm_lock_held_;
}

void XR806WiFiComponent::standby_cancelled() { this->restore_network_(); }

void XR806WiFiComponent::standby_resumed(const XR806StandbyResume &event) {
  (void) event;
  this->restore_network_();
}
#endif

void XR806WiFiComponent::probe_socket_() {
  ESP_LOGI(TAG, "Starting lwIP TCP socket probe");

  const int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (fd < 0) {
    ESP_LOGW(TAG, "TCP probe: socket() failed");
    return;
  }

  sockaddr_in addr{};
  addr.sin_len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = INADDR_ANY;

  bool success = true;

  if (lwip_bind(fd, reinterpret_cast<sockaddr *>(&addr),
                sizeof(addr)) != 0) {
    ESP_LOGW(TAG, "TCP probe: bind() failed");
    success = false;
  }

  if (success && lwip_listen(fd, 1) != 0) {
    ESP_LOGW(TAG, "TCP probe: listen() failed");
    success = false;
  }

  if (success) {
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);

    if (lwip_getsockname(
            fd, reinterpret_cast<sockaddr *>(&bound),
            &length) != 0) {
      ESP_LOGW(TAG, "TCP probe: getsockname() failed");
      success = false;
    } else {
      ESP_LOGI(TAG, "TCP probe: listener opened on port %u",
               static_cast<unsigned>(lwip_ntohs(bound.sin_port)));
    }
  }

  if (lwip_close(fd) != 0) {
    ESP_LOGW(TAG, "TCP probe: close() failed");
    success = false;
  }

  if (success)
    ESP_LOGI(TAG, "TCP probe: PASS (socket/bind/listen/close)");
}


#ifdef USE_XR806_SOCKET_FACADE_PROBE
void XR806WiFiComponent::probe_socket_facade_() {
  auto listener = esphome::socket::socket_ip_loop_monitored(
      SOCK_STREAM, IPPROTO_TCP);

  if (!listener) {
    ESP_LOGW(TAG, "ESPHome socket facade: allocation failed");
    return;
  }

  bool ok = listener->setblocking(false) == 0;

  if (!ok)
    ESP_LOGW(TAG, "ESPHome socket facade: nonblocking failed");

  sockaddr_in address{};

  const socklen_t length = esphome::socket::set_sockaddr_any(
      reinterpret_cast<sockaddr *>(&address),
      sizeof(address), 0);

  if (length != sizeof(address)) {
    ok = false;
    ESP_LOGW(TAG, "ESPHome socket facade: address failed");
  } else {
    address.sin_len = sizeof(address);
  }

  if (ok && listener->bind(
          reinterpret_cast<sockaddr *>(&address),
          length) != 0) {
    ok = false;
    ESP_LOGW(TAG, "ESPHome socket facade: bind failed");
  }

  if (ok && listener->listen(1) != 0) {
    ok = false;
    ESP_LOGW(TAG, "ESPHome socket facade: listen failed");
  }

  if (ok) {
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);

    if (listener->getsockname(
            reinterpret_cast<sockaddr *>(&bound),
            &len) != 0) {
      ok = false;
      ESP_LOGW(TAG, "ESPHome socket facade: getsockname failed");
    } else {
      ESP_LOGI(TAG, "ESPHome socket facade: port %u",
               static_cast<unsigned>(
                   lwip_ntohs(bound.sin_port)));
    }
  }

  if (listener->close() != 0) {
    ok = false;
    ESP_LOGW(TAG, "ESPHome socket facade: close failed");
  }

  if (ok)
    ESP_LOGI(TAG, "ESPHome socket facade: PASS");
}
#endif

void XR806WiFiComponent::apply_power_save_(void *opaque_netif, uint32_t now) {
  auto *nif = static_cast<struct netif *>(opaque_netif);

  // Driver power save is distinct from CPU standby. Only configure it after
  // DHCP; on failure, throttle retries rather than hammering the radio or
  // permanently giving up for this association.
  if (wlan_ext_request(nif, WLAN_EXT_CMD_SET_PM_DTIM, this->dtim_) != 0) {
    this->power_save_retry_.record(now, false);
    ESP_LOGW(TAG, "Failed to set WLAN DTIM (will retry)");
    return;
  }

  if (wlan_ext_request(nif, WLAN_EXT_CMD_SET_LISTEN_INTERVAL, this->listen_interval_) != 0) {
    this->power_save_retry_.record(now, false);
    ESP_LOGW(TAG, "Failed to set WLAN listen interval (will retry)");
    return;
  }

  if (wlan_set_ps_mode(nif, 1) != 0) {
    this->power_save_retry_.record(now, false);
    ESP_LOGW(TAG, "Failed to enable WLAN power save (will retry)");
    return;
  }

  this->power_save_retry_.record(now, true);
  ESP_LOGI(TAG, "WLAN power save configured: DTIM=%u, listen interval=%u",
           static_cast<unsigned>(this->dtim_),
           static_cast<unsigned>(this->listen_interval_));
}

void XR806WiFiComponent::loop() {
#ifdef USE_XR806_STANDBY_CONTROLLER
  if (this->network_suspending_ || this->network_parked_)
    return;  // Never launch an automatic reconnect during PM admission.
#endif
  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - this->last_poll_ms_) < 1000U)
    return;
  this->last_poll_ms_ = now;

  auto *nif = wlan_netif_get(WLAN_MODE_STA);
  const auto network = read_xr806_network_snapshot(nif);
  const bool ready = network.ready();

  const bool retry = this->reconnect_.poll(now, ready);

  if (ready != this->connected_) {
    this->connected_ = ready;
    this->power_save_retry_.reset();
    this->socket_probe_done_ = false;
    this->socket_facade_probe_done_ = false;

    if (ready) {
      this->ip_address_ = network.ipv4;
#if defined(USE_XR806) && defined(USE_XR806_NETWORK_BRIDGE)
      const auto esp_ips = ::esphome::network::get_ip_addresses();
      char ipbuf[::esphome::network::IP_ADDRESS_BUFFER_SIZE]{};
      ESP_LOGI(TAG, "ESPHome network bridge: connected=%s ip=%s",
               ::esphome::network::is_connected() ? "yes" : "no",
               esp_ips[0].str_to(ipbuf));
#endif
      this->last_rssi_poll_ms_ = now - 30000U;

      ESP_LOGI(TAG, "WLAN connected: IP=%s GW=%s MASK=%s",
               network.ipv4, network.gateway, network.netmask);
    } else {
      this->ip_address_.clear();
      this->rssi_valid_ = false;
      ESP_LOGW(TAG, "WLAN disconnected / IP unavailable");
    }
  }

  if (ready) {
    if (static_cast<uint32_t>(now - this->last_rssi_poll_ms_) >= 30000U) {
      this->last_rssi_poll_ms_ = now;

      wlan_sta_ap_t ap{};

      if (wlan_sta_ap_info(&ap) == 0) {
        this->rssi_dbm_ = ap.level;
        this->rssi_valid_ = true;

        ESP_LOGD(TAG, "WLAN RSSI: %d dBm, channel: %u",
                 this->rssi_dbm_,
                 static_cast<unsigned>(ap.channel));
      }
    }

    if (this->power_save_ && this->power_save_retry_.due(now))
      this->apply_power_save_(nif, now);

    if (this->socket_probe_enabled_ && !this->socket_probe_done_) {
      this->socket_probe_done_ = true;
      this->probe_socket_();
    }

#ifdef USE_XR806_SOCKET_FACADE_PROBE
    if (this->socket_facade_probe_enabled_ &&
        !this->socket_facade_probe_done_) {
      this->socket_facade_probe_done_ = true;
      this->probe_socket_facade_();
    }
#endif

  } else if (retry) {
    ESP_LOGW(TAG, "WLAN not ready; restarting station (attempt %u)",
             static_cast<unsigned>(this->reconnect_.attempts()));

    if (wlan_sta_disable() != 0)
      ESP_LOGW(TAG, "wlan_sta_disable returned an error");

    if (wlan_sta_enable() != 0)
      ESP_LOGW(TAG, "wlan_sta_enable returned an error");
  }

  // Do not release system-standby blockers here:
  // API/OTA/mDNS and optional MCU activity are not yet fully tracked.
  // WLAN radio power save can operate independently of CPU standby.
}

}  // namespace esphome::xr806

#endif  // USE_XR806_WLAN_SMOKE
