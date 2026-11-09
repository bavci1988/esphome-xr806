#include "esphome/core/helpers.h"

#ifdef USE_XR806

#include "hal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include "driver/chip/hal_trng.h"
#include "xr_config.h"
#include "demo/trv16_port/prj_config.h"
#include "net/wlan/wlan.h"
}

namespace esphome {

bool random_bytes(uint8_t *data, size_t length) {
  uint8_t *const start = data;
  const size_t original_length = length;
  while (length > 0) {
    uint32_t random_words[4] = {};
    bool success = false;

    // Same retry limit as the SDK PRNG seeding.
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (HAL_TRNG_Extract(0, random_words) == HAL_OK) {
        success = true;
        break;
      }
    }

    if (!success) {
      // Never return partially filled random data.
      std::memset(start, 0, original_length);
      return false;
    }

    constexpr size_t BLOCK_SIZE = sizeof(random_words);
    const size_t count = std::min(length, BLOCK_SIZE);

    std::memcpy(data, random_words, count);

    data += count;
    length -= count;
  }
  return true;
}

// Station MAC address, used as the Native API device identity.
void get_mac_address_raw(uint8_t *mac) {
  if (mac == nullptr)
    return;

  constexpr size_t MAC_LEN = 6;
  std::memset(mac, 0, MAC_LEN);

  struct netif *nif = wlan_netif_get(WLAN_MODE_STA);
  if (nif == nullptr)
    return;

  uint8_t candidate[MAC_LEN]{};

  if (wlan_get_mac_addr(nif, candidate, MAC_LEN) == 0 && mac_address_is_valid(candidate)) {
    std::memcpy(mac, candidate, MAC_LEN);
    return;
  }

  // Fallback to lwIP interface MAC when the WLAN getter is unavailable.
  if (nif->hwaddr_len == MAC_LEN && mac_address_is_valid(nif->hwaddr))
    std::memcpy(mac, nif->hwaddr, MAC_LEN);
}

}  // namespace esphome

#endif  // USE_XR806
