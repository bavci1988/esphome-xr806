// XR806 station MAC address for ESPHome Native API device identity.
#include "esphome/core/defines.h"
#ifdef USE_XR806
#include "esphome/core/helpers.h"
#include <cstdint>
#include <cstring>
extern "C" {
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "net/wlan/wlan.h"
}

namespace esphome {
void get_mac_address_raw(uint8_t *mac) {
  if (mac == nullptr)
    return;

  constexpr size_t MAC_LEN = 6;
  std::memset(mac, 0, MAC_LEN);

  struct netif *nif = wlan_netif_get(WLAN_MODE_STA);
  if (nif == nullptr)
    return;

  uint8_t candidate[MAC_LEN]{};

  if (wlan_get_mac_addr(nif, candidate, MAC_LEN) == 0 &&
      mac_address_is_valid(candidate)) {
    std::memcpy(mac, candidate, MAC_LEN);
    return;
  }

  // Fallback to lwIP interface MAC when the WLAN getter is unavailable.
  if (nif->hwaddr_len == MAC_LEN && mac_address_is_valid(nif->hwaddr))
    std::memcpy(mac, nif->hwaddr, MAC_LEN);
}
}  // namespace esphome
#endif  // USE_XR806
