#pragma once

#ifdef USE_XR806

#include "ota_backend.h"

#include <memory>

namespace esphome::ota {

// XR806 OTA transport integration only. The vendor OTA ROM API erases its
// destination immediately in ota_push_start(), and the live target geometry,
// compressed image packaging and recovery behavior have not been verified.
// Do not silently route OTA uploads into an unverified flash writer.
class XR806OTABackend final {
 public:
  OTAResponseTypes begin(size_t image_size, OTAType ota_type = OTA_TYPE_UPDATE_APP);
  void set_update_md5(const char *md5);
  OTAResponseTypes write(uint8_t *data, size_t len);
  OTAResponseTypes end();
  void abort();
  static constexpr bool supports_compression() { return false; }
};

std::unique_ptr<XR806OTABackend> make_ota_backend();

}  // namespace esphome::ota

#endif  // USE_XR806
