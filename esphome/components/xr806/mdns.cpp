// XR806 mDNS platform backend: use the vendor SDK's lwIP mDNS responder.
// All lwIP raw-API operations run through tcpip_callback() on the TCP/IP thread.
#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_MDNS)

#include "esphome/components/mdns/mdns_component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cstdint>
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

// The pinned XR806 vendor SDK compiles its liblwip.a with exactly one mDNS
// service slot. Do not silently drop a YAML-configured extra service.
static_assert(MDNS_SERVICE_COUNT <= MDNS_MAX_SERVICES,
              "XR806 vendor lwIP mDNS supports one service; extra mDNS services need an SDK rebuild");

namespace {
constexpr uint32_t MDNS_TTL_SECONDS = 120;
constexpr size_t MAX_TXT_WIRE_BYTES = 255;  // SDK mdns.c has a 256-byte TXT buffer.

struct Backend {
  // Published by setup() before any tcpip_callback runs; immutable afterward.
  std::string hostname;
  std::string service_type;
  enum mdns_sd_proto protocol{DNSSD_PROTO_TCP};
  uint16_t port{0};
  std::vector<std::string> txt;
  bool configured{false};

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
  const auto *state = static_cast<const Backend *>(arg);
  for (const auto &entry : state->txt) {
    const err_t err = mdns_resp_add_service_txtitem(
        service, entry.data(), static_cast<u8_t>(entry.size()));
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

  if (!state->initialized) {
    mdns_resp_init();
    state->initialized = true;
  }

  struct netif *iface = wlan_netif_get(WLAN_MODE_STA);
  const bool connected =
      iface != nullptr && netif_is_up(iface) && netif_is_link_up(iface) &&
      !ip4_addr_isany_val(*netif_ip4_addr(iface));

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
    if ((state->failures++ % 30) == 0)
      ESP_LOGW(TAG, "mDNS netif registration failed (%d)", static_cast<int>(err));
    return;
  }

  const s8_t service = mdns_resp_add_service(
      iface, state->hostname.c_str(), state->service_type.c_str(),
      state->protocol, state->port, MDNS_TTL_SECONDS, append_txt, state);
  if (service < 0) {
    ESP_LOGE(TAG, "mDNS service registration failed (%d)", static_cast<int>(service));
    mdns_resp_remove_netif(iface);
    return;
  }

  state->registered_netif = iface;
  state->last_ipv4 = ipv4;
  state->failures = 0;
  ESP_LOGI(TAG, "mDNS: %s.local and %s registered on port %u",
           state->hostname.c_str(), state->service_type.c_str(), state->port);
}

void shutdown_mdns(void *context) {
  remove_netif(static_cast<Backend *>(context));
}

void request_refresh() {
  const err_t err = tcpip_callback(refresh_mdns, &backend());
  if (err != ERR_OK)
    ESP_LOGW(TAG, "Cannot schedule mDNS refresh (%d)", static_cast<int>(err));
}

void register_xr806(MDNSComponent *component,
                    StaticVector<MDNSService, MDNS_SERVICE_COUNT> &services) {
  Backend &state = backend();
  if (services.size() != 1) {
    ESP_LOGE(TAG, "XR806 mDNS expects one service, got %u",
             static_cast<unsigned>(services.size()));
    component->mark_failed();
    return;
  }

  const auto &entry = services[0];
  const char *service_name = MDNS_STR_ARG(entry.service_type);
  const char *protocol_name = MDNS_STR_ARG(entry.proto);
  const auto &hostname = App.get_name();

  if (hostname.empty() || hostname.size() > MDNS_LABEL_MAXLEN ||
      service_name == nullptr || service_name[0] != '_' ||
      std::strlen(service_name) > MDNS_LABEL_MAXLEN) {
    ESP_LOGE(TAG, "Invalid XR806 mDNS hostname or service type");
    component->mark_failed();
    return;
  }

  enum mdns_sd_proto protocol;
  if (std::strcmp(protocol_name, "_tcp") == 0) {
    protocol = DNSSD_PROTO_TCP;
  } else if (std::strcmp(protocol_name, "_udp") == 0) {
    protocol = DNSSD_PROTO_UDP;
  } else {
    ESP_LOGE(TAG, "Unsupported XR806 mDNS service protocol");
    component->mark_failed();
    return;
  }

  state.hostname = hostname;
  state.service_type = service_name;
  state.protocol = protocol;
  state.port = entry.port.value();
  state.txt.clear();

  size_t txt_bytes = 0;
  for (const auto &record : entry.txt_records) {
    const char *key = MDNS_STR_ARG(record.key);
    const char *value = MDNS_STR_ARG(record.value);
    if (key == nullptr || value == nullptr) {
      ESP_LOGE(TAG, "Invalid empty mDNS TXT key/value pointer");
      component->mark_failed();
      return;
    }

    std::string txt = std::string(key) + "=" + value;
    if (txt.size() > MAX_TXT_WIRE_BYTES ||
        txt_bytes + 1 + txt.size() > MAX_TXT_WIRE_BYTES) {
      ESP_LOGE(TAG, "XR806 mDNS TXT exceeds vendor 256-byte buffer");
      component->mark_failed();
      return;
    }
    txt_bytes += 1 + txt.size();
    state.txt.push_back(std::move(txt));
  }

  state.configured = true;
}
}  // namespace

void MDNSComponent::setup() {
  this->setup_buffers_and_register_(register_xr806);
  if (!backend().configured)
    return;

  request_refresh();
  this->set_interval("xr806_mdns_refresh", 1000, []() { request_refresh(); });
}

void MDNSComponent::on_shutdown() {
  this->cancel_interval("xr806_mdns_refresh");
  const err_t err = tcpip_callback(shutdown_mdns, &backend());
  if (err != ERR_OK)
    ESP_LOGW(TAG, "Cannot schedule mDNS shutdown (%d)", static_cast<int>(err));
}

}  // namespace esphome::mdns

#endif  // USE_XR806 && USE_MDNS
