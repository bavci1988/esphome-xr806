// XR806 mDNS platform backend: use the vendor SDK's lwIP mDNS responder.
// All lwIP raw-API operations run through tcpip_callback() on the TCP/IP thread.
#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_MDNS)

#include "mdns_component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <array>
#include <atomic>
#include <cstdint>
#ifdef USE_XR806_STANDBY_CONTROLLER
#include "esphome/components/xr806/lowpower_policy.h"
#include "esphome/components/xr806/standby_async_epoch.h"
#include "esphome/components/xr806/standby_controller.h"
#endif
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "net/wlan/wlan.h"
#include "lwip/apps/mdns.h"
#include "lwip/tcpip.h"
#include "lwip/ip4_addr.h"
}

#if !LWIP_MDNS_RESPONDER || !LWIP_IGMP || !LWIP_IPV4
#error "XR806 mDNS needs LWIP_MDNS_RESPONDER, LWIP_IGMP and LWIP_IPV4"
#endif

namespace esphome::mdns {

ESPHOME_LOG_TAG(TAG, "xr806.mdns");

// The SDK's lwipopts.h sets how many services one netif can announce.
static_assert(MDNS_SERVICE_COUNT <= MDNS_MAX_SERVICES,
              "More mDNS services than MDNS_MAX_SERVICES in the XR806 SDK's lwipopts.h");

namespace {
constexpr uint32_t MDNS_TTL_SECONDS = 120;
constexpr size_t MAX_TXT_WIRE_BYTES = 255;  // SDK mdns.c has a 256-byte TXT buffer.

struct Service {
  std::string type;
  enum mdns_sd_proto protocol { DNSSD_PROTO_TCP };
  uint16_t port{0};
  std::vector<std::string> txt;
};

struct Backend {
  // Published by setup() before any tcpip_callback runs; immutable afterward.
  std::string hostname;
  std::array<Service, MDNS_SERVICE_COUNT> services{};
  size_t service_count{0};
  bool configured{false};
#ifdef USE_XR806_STANDBY_CONTROLLER
  xr806::XR806StandbyAsyncEpoch suspend_epoch{};
  std::atomic<bool> suspended{false};
#endif

  // Owned exclusively by the SDK TCP/IP thread.
  bool initialized{false};
  struct netif *registered_netif{nullptr};
  uint32_t last_ipv4{0};
  unsigned failures{0};
};

Backend &backend() {
  static Backend state;
  return state;
}

void append_txt(struct mdns_service *service, void *arg) {
  const auto *entry_service = static_cast<const Service *>(arg);
  for (const auto &entry : entry_service->txt) {
    const err_t err = mdns_resp_add_service_txtitem(service, entry.data(), static_cast<u8_t>(entry.size()));
    if (err != ERR_OK) {
      ESP_LOGE(TAG, "Failed to build mDNS TXT entry (error %d)", static_cast<int>(err));
      break;
    }
  }
}

void remove_netif(Backend *state) {
  if (state->registered_netif == nullptr)
    return;
  mdns_resp_remove_netif(state->registered_netif);
  state->registered_netif = nullptr;
  state->last_ipv4 = 0;
}

// Runs only in lwIP's TCP/IP thread. No raw lwIP or mDNS API use from the
// ESPHome event loop, even during link loss / DHCP address changes.
void refresh_mdns(void *context) {
  auto *state = static_cast<Backend *>(context);
  if (!state->configured)
    return;
#ifdef USE_XR806_STANDBY_CONTROLLER
  const uint32_t requested_epoch = state->suspend_epoch.epoch();
  if (state->suspend_epoch.requested()) {
    // The lwIP thread owns the actual responder. A callback from an older
    // suspend epoch must never publish an idle vote for a newer request.
    remove_netif(state);
    if (!state->suspend_epoch.current(requested_epoch)) {
      state->suspended.store(false, std::memory_order_release);
      xr806::lowpower_policy().set_mdns_idle(false);
      return;
    }
    state->suspended.store(true, std::memory_order_release);
    xr806::lowpower_policy().set_mdns_idle(true);
    // A cancelling producer can race exactly between the check and vote.
    // Recheck AFTER publishing; it will either restore BUSY here or its
    // own preflight(false) will publish BUSY after this callback.
    if (!state->suspend_epoch.current(requested_epoch)) {
      state->suspended.store(false, std::memory_order_release);
      xr806::lowpower_policy().set_mdns_idle(false);
    }
    return;
  }
  state->suspended.store(false, std::memory_order_release);
  xr806::lowpower_policy().set_mdns_idle(false);
#endif

  if (!state->initialized) {
    mdns_resp_init();
    state->initialized = true;
  }

  struct netif *iface = wlan_netif_get(WLAN_MODE_STA);
  const bool connected =
      iface != nullptr && netif_is_up(iface) && netif_is_link_up(iface) && !ip4_addr_isany_val(*netif_ip4_addr(iface));

  if (!connected) {
    remove_netif(state);
    return;
  }

  if (state->registered_netif != nullptr && state->registered_netif != iface)
    remove_netif(state);

  const uint32_t ipv4 = netif_ip4_addr(iface)->addr;
  if (state->registered_netif == iface) {
    if (ipv4 != state->last_ipv4) {
      state->last_ipv4 = ipv4;
      mdns_resp_restart(iface);
      ESP_LOGD(TAG, "mDNS restarted after IPv4 change");
    }
    return;
  }

  const err_t err = mdns_resp_add_netif(iface, state->hostname.c_str(), MDNS_TTL_SECONDS);
  if (err != ERR_OK) {
    if ((state->failures++ % 30) == 0) {
      ESP_LOGW(TAG, "mDNS netif registration failed (%d)", static_cast<int>(err));
    }
    return;
  }

  for (size_t i = 0; i < state->service_count; i++) {
    Service &entry = state->services[i];
    const s8_t slot = mdns_resp_add_service(iface, state->hostname.c_str(), entry.type.c_str(), entry.protocol,
                                            entry.port, MDNS_TTL_SECONDS, append_txt, &entry);
    if (slot < 0) {
      ESP_LOGE(TAG, "mDNS service %s registration failed (%d)", entry.type.c_str(), static_cast<int>(slot));
      mdns_resp_remove_netif(iface);
      return;
    }
  }

  state->registered_netif = iface;
  state->last_ipv4 = ipv4;
  state->failures = 0;
  ESP_LOGI(TAG, "mDNS: %s.local registered with %u services", state->hostname.c_str(),
           static_cast<unsigned>(state->service_count));
}

void shutdown_mdns(void *context) { remove_netif(static_cast<Backend *>(context)); }

void request_refresh() {
  const err_t err = tcpip_callback(refresh_mdns, &backend());
  if (err != ERR_OK) {
    ESP_LOGW(TAG, "Cannot schedule mDNS refresh (%d)", static_cast<int>(err));
  }
}

/// Copies one ESPHome service into out; false if the lwIP responder cannot announce it.
bool convert_service(const MDNSService &entry, Service &out) {
  const char *service_name = MDNS_STR_ARG(entry.service_type);
  const char *protocol_name = MDNS_STR_ARG(entry.proto);
  if (service_name == nullptr || service_name[0] != '_' || std::strlen(service_name) > MDNS_LABEL_MAXLEN) {
    ESP_LOGE(TAG, "Invalid mDNS service type");
    return false;
  }
  if (std::strcmp(protocol_name, "_tcp") == 0) {
    out.protocol = DNSSD_PROTO_TCP;
  } else if (std::strcmp(protocol_name, "_udp") == 0) {
    out.protocol = DNSSD_PROTO_UDP;
  } else {
    ESP_LOGE(TAG, "Unsupported mDNS service protocol %s", protocol_name);
    return false;
  }
  out.type = service_name;
  out.port = entry.port.value();
  out.txt.clear();

  size_t txt_bytes = 0;
  for (const auto &record : entry.txt_records) {
    const char *key = MDNS_STR_ARG(record.key);
    const char *value = MDNS_STR_ARG(record.value);
    if (key == nullptr || value == nullptr) {
      ESP_LOGE(TAG, "Invalid empty mDNS TXT key/value pointer");
      return false;
    }
    std::string txt = std::string(key) + "=" + value;
    if (txt.size() > MAX_TXT_WIRE_BYTES || txt_bytes + 1 + txt.size() > MAX_TXT_WIRE_BYTES) {
      ESP_LOGE(TAG, "mDNS TXT records of %s exceed the 256-byte lwIP buffer", service_name);
      return false;
    }
    txt_bytes += 1 + txt.size();
    out.txt.push_back(std::move(txt));
  }
  return true;
}

void register_xr806(MDNSComponent *component, StaticVector<MDNSService, MDNS_SERVICE_COUNT> &services) {
  Backend &state = backend();
  const auto &hostname = App.get_name();
  if (hostname.empty() || hostname.size() > MDNS_LABEL_MAXLEN) {
    component->mark_failed(LOG_STR("Invalid mDNS hostname"));
    return;
  }
  state.hostname = hostname;
  state.service_count = 0;
  for (const auto &entry : services) {
    if (!convert_service(entry, state.services[state.service_count])) {
      component->mark_failed(LOG_STR("Invalid mDNS service"));
      return;
    }
    state.service_count++;
  }
  state.configured = true;
}

#ifdef USE_XR806_STANDBY_CONTROLLER
// Standby owner of the mDNS responder; kept here so MDNSComponent needs no XR806 base class.
class XR806MDNSStandby final : public xr806::XR806StandbyParticipant {
 public:
  void standby_preflight(bool requested) override {
    auto &state = backend();
    if (!state.configured)
      return;
    if (state.suspend_epoch.requested() == requested)
      return;
    if (!requested) {
      // Busy must be asserted before scheduling a new multicast responder.
      xr806::lowpower_policy().set_mdns_idle(false);
      state.suspended.store(false, std::memory_order_release);
    }
    state.suspend_epoch.request(requested);
    request_refresh();
  }
  uint32_t owned_services() const override { return xr806::XR806LowPowerPolicy::MDNS_BUSY; }
  bool prepare_for_standby() override {
    return backend().suspend_epoch.requested() && backend().suspended.load(std::memory_order_acquire);
  }
  void standby_cancelled() override { this->standby_preflight(false); }
  void standby_resumed(const xr806::XR806StandbyResume & /*event*/) override { this->standby_preflight(false); }
};

XR806MDNSStandby &mdns_standby() {
  static XR806MDNSStandby participant;
  return participant;
}
#endif
}  // namespace

static constexpr uint32_t REFRESH_INTERVAL_ID = 0;

void MDNSComponent::setup() {
  this->setup_buffers_and_register_(register_xr806);
  if (!backend().configured)
    return;
#ifdef USE_XR806_STANDBY_CONTROLLER
  if (auto *controller = xr806::standby_controller(); controller != nullptr) {
    if (!controller->register_participant(&mdns_standby())) {
      this->mark_failed(LOG_STR("Cannot register mDNS standby participant"));
      return;
    }
    xr806::lowpower_policy().set_mdns_idle(false);
  }
#endif

  request_refresh();
  this->set_interval(REFRESH_INTERVAL_ID, 1000, []() { request_refresh(); });
}

void MDNSComponent::on_shutdown() {
  this->cancel_interval(REFRESH_INTERVAL_ID);
  const err_t err = tcpip_callback(shutdown_mdns, &backend());
  if (err != ERR_OK) {
    ESP_LOGW(TAG, "Cannot schedule mDNS shutdown (%d)", static_cast<int>(err));
  }
}

}  // namespace esphome::mdns

#endif  // USE_XR806 && USE_MDNS
