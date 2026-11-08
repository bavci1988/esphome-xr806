#include "esphome/core/defines.h"

#ifdef USE_XR806_WLAN_SMOKE

#include "esphome/components/xr806/network_status.h"

extern "C" {
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "common/framework/net_ctrl.h"
#include "net/wlan/wlan.h"
#include "lwip/ip4_addr.h"
}

namespace esphome::xr806 {

XR806NetworkSnapshot read_xr806_network_snapshot(
    struct netif *interface) {

  XR806NetworkSnapshot result{};

  wlan_sta_states_t state = WLAN_STA_STATE_DISCONNECTED;

  result.associated =
      wlan_sta_state(&state) == 0 &&
      state == WLAN_STA_STATE_CONNECTED;

  if (interface == nullptr)
    return result;

  result.interface_up = netif_is_up(interface) != 0;
  result.link_up = netif_is_link_up(interface) != 0;

  const auto *ip = netif_ip4_addr(interface);

  result.ipv4_assigned =
      ip != nullptr && !ip4_addr_isany(ip);

  if (result.ipv4_assigned) {
    ip4addr_ntoa_r(
        ip, result.ipv4, sizeof(result.ipv4));

    ip4addr_ntoa_r(
        netif_ip4_gw(interface),
        result.gateway,
        sizeof(result.gateway));

    ip4addr_ntoa_r(
        netif_ip4_netmask(interface),
        result.netmask,
        sizeof(result.netmask));
  }

  return result;
}

}  // namespace esphome::xr806

#endif  // USE_XR806_WLAN_SMOKE
