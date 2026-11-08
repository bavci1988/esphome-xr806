#include "esphome/core/helpers.h"

#ifdef USE_XR806

#include "esphome/components/xr806/hal.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include "driver/chip/hal_trng.h"
}

namespace esphome {

bool random_bytes(uint8_t *data, size_t length) {
  uint8_t *const start = data;
  const size_t original_length = length;
  while (length > 0) {
    uint32_t random_words[4] = {};
    bool success = false;

    // Gleiche maximale Versuchszahl wie beim SDK-PRNG-Seeding.
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (HAL_TRNG_Extract(0, random_words) == HAL_OK) {
        success = true;
        break;
      }
    }

    if (!success) {
      // Bei Fehlern keine unvollständigen Zufallsdaten zurückgeben.
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

}  // namespace esphome

#endif  // USE_XR806
