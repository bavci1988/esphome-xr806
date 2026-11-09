#pragma once
// Deliberately read-only OTA layout admission. This does NOT erase/write flash,
// verify image signatures or guarantee bootloader rollback.
#include <cstdint>

namespace esphome::xr806 {

struct XR806OTAFlashRange {
  uint32_t offset{0};
  uint32_t length{0};
};

struct XR806OTAFlashPlan {
  uint32_t flash_size{0};
  XR806OTAFlashRange running_image{};
  XR806OTAFlashRange candidate_image{};  // Inactive OTA staging region
  XR806OTAFlashRange ota_metadata{};     // Boot selection/config region
  uint32_t image_bytes{0};

  // All three are external proofs from matching hardware/ROM/SDK, not
  // inferred merely because an SDK example contains a similar image.cfg.
  bool layout_verified{false};
  bool image_format_verified{false};
  bool boot_recovery_verified{false};
};

enum class XR806OTAFlashPlanError : uint8_t {
  OK,
  INVALID_GEOMETRY,
  INVALID_IMAGE_SIZE,
  ACTIVE_IMAGE_OVERLAP,
  METADATA_OVERLAP,
  UNVERIFIED_LAYOUT,
};

constexpr bool xr806_ota_range_valid(XR806OTAFlashRange r, uint32_t flash_size) {
  return r.length != 0 && r.offset < flash_size && static_cast<uint64_t>(r.offset) + r.length <= flash_size;
}

constexpr bool xr806_ota_overlap(XR806OTAFlashRange a, XR806OTAFlashRange b) {
  return static_cast<uint64_t>(a.offset) < static_cast<uint64_t>(b.offset) + b.length &&
         static_cast<uint64_t>(b.offset) < static_cast<uint64_t>(a.offset) + a.length;
}

constexpr XR806OTAFlashPlanError xr806_validate_ota_flash_plan(const XR806OTAFlashPlan &p) {
  constexpr uint32_t erase_sector = 4096;
  if (p.flash_size == 0 || !xr806_ota_range_valid(p.running_image, p.flash_size) ||
      !xr806_ota_range_valid(p.candidate_image, p.flash_size) || !xr806_ota_range_valid(p.ota_metadata, p.flash_size) ||
      (p.candidate_image.offset % erase_sector) != 0 || (p.candidate_image.length % erase_sector) != 0 ||
      (p.ota_metadata.offset % erase_sector) != 0 || (p.ota_metadata.length % erase_sector) != 0)
    return XR806OTAFlashPlanError::INVALID_GEOMETRY;
  if (p.image_bytes == 0 || p.image_bytes > p.candidate_image.length)
    return XR806OTAFlashPlanError::INVALID_IMAGE_SIZE;
  if (xr806_ota_overlap(p.running_image, p.candidate_image) || xr806_ota_overlap(p.running_image, p.ota_metadata))
    return XR806OTAFlashPlanError::ACTIVE_IMAGE_OVERLAP;
  if (xr806_ota_overlap(p.candidate_image, p.ota_metadata))
    return XR806OTAFlashPlanError::METADATA_OVERLAP;
  if (!p.layout_verified || !p.image_format_verified || !p.boot_recovery_verified)
    return XR806OTAFlashPlanError::UNVERIFIED_LAYOUT;
  return XR806OTAFlashPlanError::OK;
}

}  // namespace esphome::xr806
