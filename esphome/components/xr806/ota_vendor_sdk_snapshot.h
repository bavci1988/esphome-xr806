#pragma once
// Optional compile-time XR806 vendor-image introspection. READ ONLY.
// Define USE_XR806_OTA_RUNTIME_PROBE explicitly in a separate diagnostic
// target; normal firmware/ESPHome YAML never turns this on.
// No ota_push_* / image_set_cfg / flash_erase / flash_write is called.
#include "ota_vendor_runtime_preflight.h"

#if defined(USE_XR806) && defined(USE_XR806_OTA_RUNTIME_PROBE)
extern "C" {
#include "image/image.h"
}

namespace esphome::xr806 {

inline bool xr806_capture_vendor_ota_snapshot(uint32_t measured_flash_bytes, uint32_t prospective_upload_bytes,
                                              XR806OTARuntimeGeometry *out) {
  if (out == nullptr)
    return false;
  *out = {};  // Fail closed if the SDK ROM refuses the read.
  const image_ota_param_t *param = image_get_ota_param();
  if (param == nullptr)
    return false;

  // This reads the FDCM records through the ROM's own API; unlike static
  // dump decoding, this is the currently mounted system's configuration.
  image_cfg_t stored{};
  if (image_get_cfg(&stored) != 0)
    return false;

  const image_seq_t live = image_get_running_seq();
  if (live != param->running_seq)
    return false;  // SDK's two running-sequence views disagree.

  out->flash_bytes = measured_flash_bytes;  // Must be externally measured, not guessed.
  out->metadata_addr = param->ota_addr;
  out->metadata_bytes = param->ota_size;
  out->metadata_flash = static_cast<uint8_t>(param->ota_flash);
  out->image_addr[0] = param->addr[0];
  out->image_addr[1] = param->addr[1];
  out->image_flash[0] = param->flash[0];
  out->image_flash[1] = param->flash[1];
  out->image_max_kib = param->img_max_size;
  out->compressed_max_kib = param->img_xz_max_size;
  out->bootloader_bytes = param->bl_size;
  out->running_seq = static_cast<uint8_t>(live);
  out->fdcm_selected_seq = static_cast<uint8_t>(stored.seq);
  out->fdcm_state_verified = stored.state == IMAGE_STATE_VERIFIED;
  out->requested_image_bytes = prospective_upload_bytes;
#if defined(CONFIG_OTA_POLICY_IMAGE_COMPRESSION) && !defined(CONFIG_OTA_POLICY_PINGPONG)
  out->compression_policy = true;
#else
  // Do not pretend the 400-KiB slot is a safe target in an unknown policy.
  out->compression_policy = false;
#endif
  out->runtime_snapshot_observed = true;
  return true;
}

}  // namespace esphome::xr806
#endif
