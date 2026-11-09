#include "wifi_component.h"

#ifdef USE_WIFI
#ifdef USE_XR806

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

#include "esphome/components/xr806/network_status.h"
#include "esphome/components/xr806/wifi_power_save_retry.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

extern "C" {
// The WLAN declarations depend on CONFIG_WLAN and PRJCONF_NET_EN from the project configuration.
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "common/framework/net_ctrl.h"
#include "common/framework/sys_ctrl/sys_ctrl.h"
#include "net/wlan/wlan.h"
#include "net/wlan/wlan_ext_req.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
}

namespace esphome::wifi {

// Radio power settings from xr806: wlan:, used for power_save_mode LIGHT and HIGH.
#ifdef XR806_WLAN_DTIM
static constexpr uint8_t WLAN_DTIM = XR806_WLAN_DTIM;
static constexpr uint8_t WLAN_LISTEN_INTERVAL = XR806_WLAN_LISTEN_INTERVAL;
#else
static constexpr uint8_t WLAN_DTIM = 1;
static constexpr uint8_t WLAN_LISTEN_INTERVAL = 0;
#endif

ESPHOME_LOG_TAG(TAG, "wifi_xr806");

// The vendor platform_init() has already started lwIP and the WLAN core in station mode.
// Connection state is polled in wifi_loop_(); the SDK event observer only reports what
// polling cannot see: scan completion and failed connection attempts.
static constexpr size_t MAX_SCAN_RESULTS = 32;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
// Written from the SDK system-control task, read in the main loop.
static std::atomic<bool> s_scan_finished{false};
static std::atomic<bool> s_scan_failed{false};
static std::atomic<bool> s_connect_failed{false};
// Main loop only.
static bool s_observer_attached = false;
static bool s_sta_enabled = false;
static bool s_connect_requested = false;
static bool s_sta_was_connected = false;
static bool s_power_save_requested = false;
static xr806::XR806WiFiPowerSaveRetry s_power_save_retry;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

static void network_event_cb(uint32_t event, uint32_t /*data*/, void * /*arg*/) {
  switch (EVENT_SUBTYPE(event)) {
    case NET_CTRL_MSG_WLAN_SCAN_SUCCESS:
      s_scan_finished.store(true, std::memory_order_release);
      break;
    case NET_CTRL_MSG_WLAN_SCAN_FAILED:
      s_scan_failed.store(true, std::memory_order_relaxed);
      s_scan_finished.store(true, std::memory_order_release);
      break;
    case NET_CTRL_MSG_WLAN_CONNECT_FAILED:
    case NET_CTRL_MSG_WLAN_4WAY_HANDSHAKE_FAILED:
      s_connect_failed.store(true, std::memory_order_release);
      break;
    default:
      break;
  }
}

static struct netif *sta_netif() { return wlan_netif_get(WLAN_MODE_STA); }

static bool sta_ready() { return xr806::read_xr806_network_snapshot(sta_netif()).ready(); }

static bool sta_enable() {
  if (s_sta_enabled)
    return true;
  if (wlan_sta_enable() != 0)
    return false;
  s_sta_enabled = true;
  return true;
}

void WiFiComponent::wifi_pre_setup_() {
  if (!s_observer_attached) {
    observer_base *observer =
        sys_callback_observer_create(CTRL_MSG_TYPE_NETWORK, NET_CTRL_MSG_ALL, network_event_cb, nullptr);
    s_observer_attached = observer != nullptr && sys_ctrl_attach(observer) == 0;
    if (!s_observer_attached) {
      ESP_LOGW(TAG, "Cannot attach WLAN event observer; scans and failures rely on timeouts");
    }
  }
  if (net_switch_mode(WLAN_MODE_STA) != 0) {
    ESP_LOGW(TAG, "Cannot switch WLAN to station mode");
  }
}

bool WiFiComponent::wifi_mode_(optional<bool> sta, optional<bool> ap) {
  if (ap.has_value() && ap.value())
    return false;  // Validation rejects ap: on XR806; the radio runs one interface at a time.
  if (sta.has_value()) {
    if (sta.value())
      return sta_enable();
    s_connect_requested = false;
    if (s_sta_enabled && wlan_sta_disable() != 0)
      return false;
    s_sta_enabled = false;
  }
  return true;
}

bool WiFiComponent::wifi_sta_pre_setup_() { return this->wifi_mode_(true, {}); }

bool WiFiComponent::wifi_apply_output_power_(float /*output_power*/) { return true; }

bool WiFiComponent::wifi_apply_power_save_() {
  // DTIM and the listen interval can only be set while associated, so the request is
  // applied from wifi_loop_() once the station has an address.
  s_power_save_requested = this->power_save_ != WIFI_POWER_SAVE_NONE;
  s_power_save_retry.reset();
  return true;
}

static bool apply_power_save_now(bool enable) {
  struct netif *nif = sta_netif();
  if (nif == nullptr)
    return false;
  if (enable) {
    if (wlan_ext_request(nif, WLAN_EXT_CMD_SET_PM_DTIM, WLAN_DTIM) != 0)
      return false;
    if (wlan_ext_request(nif, WLAN_EXT_CMD_SET_LISTEN_INTERVAL, WLAN_LISTEN_INTERVAL) != 0)
      return false;
  }
  return wlan_set_ps_mode(nif, enable ? 1 : 0) == 0;
}

bool WiFiComponent::wifi_sta_ip_config_(const optional<ManualIP> &manual_ip) {
  // Validation rejects manual_ip on XR806; the vendor network control always runs DHCP.
  return !manual_ip.has_value();
}

bool WiFiComponent::wifi_apply_hostname_() {
  struct netif *nif = sta_netif();
  if (nif == nullptr)
    return false;
  netif_set_hostname(nif, App.get_name().c_str());
  return true;
}

bool WiFiComponent::wifi_sta_connect_(const WiFiAP &ap) {
#ifdef USE_WIFI_MANUAL_IP
  if (!this->wifi_sta_ip_config_(ap.get_manual_ip()))
    return false;
#endif
  this->wifi_apply_hostname_();

  // wlan_sta_set() takes mutable buffers; keep local copies.
  uint8_t ssid[WLAN_SSID_MAX_LEN]{};
  uint8_t psk[65]{};
  const size_t ssid_len = std::min(ap.ssid_.size(), sizeof(ssid));
  std::memcpy(ssid, ap.ssid_.c_str(), ssid_len);
  const size_t psk_len = std::min(ap.password_.size(), sizeof(psk) - 1);
  std::memcpy(psk, ap.password_.c_str(), psk_len);

  if (s_sta_enabled && wlan_sta_disable() != 0)
    return false;
  s_sta_enabled = false;
  if (wlan_sta_set(ssid, static_cast<uint8_t>(ssid_len), psk_len == 0 ? nullptr : psk) != 0)
    return false;
  s_connect_failed.store(false, std::memory_order_relaxed);
  if (!sta_enable())
    return false;
  s_connect_requested = true;
  return true;
}

const char *get_auth_mode_str(uint8_t /*mode*/) { return "UNKNOWN"; }
const char *get_disconnect_reason_str(uint8_t /*reason*/) { return "UNKNOWN"; }

WiFiSTAConnectStatus WiFiComponent::wifi_sta_connect_status_() const {
  if (sta_ready())
    return WiFiSTAConnectStatus::CONNECTED;
  if (s_connect_failed.load(std::memory_order_acquire))
    return WiFiSTAConnectStatus::ERROR_CONNECT_FAILED;
  return s_connect_requested ? WiFiSTAConnectStatus::CONNECTING : WiFiSTAConnectStatus::IDLE;
}

bool WiFiComponent::wifi_scan_start_(bool passive) {
  {
    ScanResultsLock lock(this);
    this->scan_result_.clear();
  }
  this->scan_done_ = false;
  if (!sta_enable())
    return false;
  s_scan_failed.store(false, std::memory_order_relaxed);
  s_scan_finished.store(false, std::memory_order_release);
  wlan_sta_scan_param_t param{};
  param.scan_passive = passive ? 1 : 0;
  param.scan_ssid = this->all_networks_hidden_() ? 1 : 0;
  return wlan_sta_scan(&param) == 0;
}

void WiFiComponent::wifi_scan_done_callback_() {
  ScanResultsLock lock(this);
  this->scan_result_.clear();
  this->scan_done_ = true;
  if (s_scan_failed.load(std::memory_order_relaxed)) {
    ESP_LOGW(TAG, "Scan failed");
    return;
  }
  int found = 0;
  if (wlan_sta_get_scan_result_num(&found) != 0 || found <= 0)
    return;
  const size_t capacity = std::min(static_cast<size_t>(found), MAX_SCAN_RESULTS);
  // Freed at the end of this function; scans are rare and short-lived.
  auto aps = std::make_unique<wlan_sta_ap_t[]>(capacity);
  wlan_sta_scan_results_t results{};
  results.ap = aps.get();
  results.size = static_cast<int>(capacity);
  if (wlan_sta_scan_result(&results) != 0)
    return;
  const size_t num = std::min(static_cast<size_t>(std::max(results.num, 0)), capacity);

  const bool needs_full = this->needs_full_scan_results_();
  char ssid[SSID_BUFFER_SIZE];
  auto copy_ssid = [&ssid](const wlan_sta_ap_t &ap) -> size_t {
    const size_t len = std::min<size_t>(ap.ssid.ssid_len, SSID_BUFFER_SIZE - 1);
    std::memcpy(ssid, ap.ssid.ssid, len);
    ssid[len] = '\0';
    return len;
  };
  size_t count = 0;
  for (size_t i = 0; i < num; i++) {
    const auto &ap = aps[i];
    copy_ssid(ap);
    if (needs_full || this->matches_configured_network_(ssid, ap.bssid)) {
      count++;
    } else {
      this->log_discarded_scan_result_(ssid, ap.bssid, static_cast<int8_t>(ap.level), ap.channel);
    }
  }
  if (!this->scan_result_.try_init(count)) {
    ESP_LOGW(TAG, "No memory for %zu scan results", count);
    return;
  }
  for (size_t i = 0; i < num; i++) {
    const auto &ap = aps[i];
    const size_t len = copy_ssid(ap);
    if (!needs_full && !this->matches_configured_network_(ssid, ap.bssid))
      continue;
    bssid_t bssid;
    std::copy(ap.bssid, ap.bssid + bssid.size(), bssid.begin());
    const bool with_auth = ap.wpa_flags != 0;
    this->scan_result_.emplace_back(bssid, ssid, len, ap.channel, static_cast<int8_t>(ap.level), with_auth, len == 0);
  }
  ESP_LOGV(TAG, "Scan complete: %zu found, %zu stored", num, this->scan_result_.size());
}

bool WiFiComponent::wifi_disconnect_() {
  s_connect_requested = false;
  return wlan_sta_disconnect() == 0;
}

bssid_t WiFiComponent::wifi_bssid() {
  bssid_t bssid{};
  wlan_sta_ap_t ap{};
  if (wlan_sta_ap_info(&ap) == 0)
    std::copy(ap.bssid, ap.bssid + bssid.size(), bssid.begin());
  return bssid;
}

const char *WiFiComponent::wifi_ssid_to(std::span<char, SSID_BUFFER_SIZE> buffer) {
  buffer[0] = '\0';
  wlan_sta_ap_t ap{};
  if (wlan_sta_ap_info(&ap) == 0) {
    const size_t len = std::min<size_t>(ap.ssid.ssid_len, SSID_BUFFER_SIZE - 1);
    std::memcpy(buffer.data(), ap.ssid.ssid, len);
    buffer[len] = '\0';
  }
  return buffer.data();
}

int8_t WiFiComponent::wifi_rssi() {
  if (!this->is_connected_())
    return WIFI_RSSI_DISCONNECTED;
  wlan_sta_ap_t ap{};
  return wlan_sta_ap_info(&ap) == 0 ? static_cast<int8_t>(ap.level) : WIFI_RSSI_DISCONNECTED;
}

int32_t WiFiComponent::get_wifi_channel() {
  wlan_sta_ap_t ap{};
  return wlan_sta_ap_info(&ap) == 0 ? ap.channel : 0;
}

network::IPAddresses WiFiComponent::wifi_sta_ip_addresses() {
  network::IPAddresses addresses;
  struct netif *nif = sta_netif();
  if (nif != nullptr && !ip4_addr_isany(netif_ip4_addr(nif)))
    addresses[0] = network::IPAddress(netif_ip_addr4(nif));
  return addresses;
}

network::IPAddress WiFiComponent::wifi_subnet_mask_() {
  struct netif *nif = sta_netif();
  return nif == nullptr ? network::IPAddress() : network::IPAddress(netif_ip_netmask4(nif));
}

network::IPAddress WiFiComponent::wifi_gateway_ip_() {
  struct netif *nif = sta_netif();
  return nif == nullptr ? network::IPAddress() : network::IPAddress(netif_ip_gw4(nif));
}

network::IPAddress WiFiComponent::wifi_dns_ip_(int num) { return network::IPAddress(dns_getserver(num)); }

bool WiFiComponent::wifi_loop_() {
  if (this->state_ == WIFI_COMPONENT_STATE_STA_SCANNING && s_scan_finished.load(std::memory_order_acquire)) {
    s_scan_finished.store(false, std::memory_order_relaxed);
    this->wifi_scan_done_callback_();
#ifdef USE_WIFI_SCAN_RESULTS_LISTENERS
    this->notify_scan_results_listeners_();
#endif
  }

  const bool connected = sta_ready();
  if (connected && !s_sta_was_connected) {
    s_sta_was_connected = true;
    s_power_save_retry.reset();
    ESP_LOGV(TAG, "Connected");
#ifdef USE_WIFI_CONNECT_STATE_LISTENERS
    // Deferred until the state machine reaches STA_CONNECTED.
    this->pending_.connect_state = true;
#endif
#ifdef USE_WIFI_IP_STATE_LISTENERS
    this->notify_ip_state_listeners_();
#endif
  } else if (!connected && s_sta_was_connected) {
    s_sta_was_connected = false;
    ESP_LOGV(TAG, "Disconnected");
    this->update_connected_state_();
#ifdef USE_WIFI_CONNECT_STATE_LISTENERS
    this->notify_disconnect_state_listeners_();
#endif
  }

  // Driver power save is separate from CPU standby; a failed request is retried every 30 s.
  const uint32_t now = App.get_loop_component_start_time();
  if (connected && s_power_save_retry.due(now)) {
    const bool ok = apply_power_save_now(s_power_save_requested);
    s_power_save_retry.record(now, ok);
    if (!ok) {
      ESP_LOGW(TAG, "Cannot apply WLAN power save; will retry");
    }
#ifdef USE_WIFI_POWER_SAVE_LISTENERS
    if (ok) {
      for (auto *listener : this->power_save_listeners_)
        listener->on_wifi_power_save(this->power_save_);
    }
#endif
  }
  return true;
}

}  // namespace esphome::wifi

#endif  // USE_XR806
#endif  // USE_WIFI
