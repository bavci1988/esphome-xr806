// XR806-owned implementation of the two guarded ESPHome network callbacks.
#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_XR806_WLAN_SMOKE) && \
    defined(USE_XR806_NETWORK_BRIDGE) && defined(USE_NETWORK)

#include "esphome/components/xr806/wifi_component.h"
#include "esphome/components/network/util.h"

namespace esphome::network {

bool xr806_platform_is_connected() {
  const auto *wifi = xr806::global_xr806_wifi;
  return wifi != nullptr && wifi->is_connected();
}

IPAddresses xr806_platform_get_ip_addresses() {
  IPAddresses out{};
  const auto *wifi = xr806::global_xr806_wifi;
  if (wifi == nullptr || !wifi->is_connected())
    return out;

  const auto &ip = wifi->get_ip_address();
  if (!ip.empty())
    out[0] = IPAddress(ip);
  return out;
}

}  // namespace esphome::network

#endif  // USE_XR806 && USE_XR806_NETWORK_BRIDGE
