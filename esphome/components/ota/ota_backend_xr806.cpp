#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "ota_backend_xr806.h"

#include "esphome/core/log.h"

#include <memory>

namespace esphome::ota {

ESPHOME_LOG_TAG(TAG, "ota.xr806");

std::unique_ptr<XR806OTABackend> make_ota_backend() { return std::make_unique<XR806OTABackend>(); }

OTAResponseTypes XR806OTABackend::begin(size_t image_size, OTAType ota_type) {
  (void) image_size;
  if (ota_type != OTA_TYPE_UPDATE_APP)
    return OTA_RESPONSE_ERROR_UNSUPPORTED_OTA_TYPE;

  // Reject BEFORE attempting vendor ota_push_start(): that API erases flash.
  // A supported OTA upload must verify the live ROM/FDCM slot, recoverability
  // and AWIH/XZ format, then own a native PM wake lock for the entire write.
  ESP_LOGE(TAG, "XR806 OTA flash writer is not available yet: upload rejected without erasing flash");
  return OTA_RESPONSE_ERROR_NO_UPDATE_PARTITION;
}

void XR806OTABackend::set_update_md5(const char *md5) { (void) md5; }

OTAResponseTypes XR806OTABackend::write(uint8_t *data, size_t len) {
  (void) data;
  (void) len;
  return OTA_RESPONSE_ERROR_WRITING_FLASH;
}

OTAResponseTypes XR806OTABackend::end() { return OTA_RESPONSE_ERROR_UPDATE_END; }

void XR806OTABackend::abort() {}

}  // namespace esphome::ota

#endif  // USE_XR806
