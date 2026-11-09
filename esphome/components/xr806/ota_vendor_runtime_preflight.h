#pragma once

// Runtime admission for the XR806 vendor OTA geometry. This is read-only:
// nothing in this file calls ota_push_start(), the SPI flash driver, or FDCM.
// The image_get_ota_param() caller must provide a LIVE snapshot separately.
// The vendor has two image sequences and compression / pingpong policies.
#include <cstdint>
#include "ota_flash_plan.h"

namespace esphome::xr806 {

struct XR806OTARuntimeGeometry {
  uint32_t flash_bytes{0};
  uint32_t metadata_addr{0};
  uint32_t metadata_bytes{0};
  uint32_t image_addr[2]{};
  uint8_t image_flash[2]{};
  uint8_t metadata_flash{0};
  uint32_t image_max_kib{0};
  uint32_t compressed_max_kib{0};
  uint32_t bootloader_bytes{0};
  uint32_t requested_image_bytes{0};
  uint8_t running_seq{0xff};
  uint8_t fdcm_selected_seq{0xff};  // Derived from actual FDCM, not guessed.
  bool compression_policy{false};
  bool runtime_snapshot_observed{false};
  bool fdcm_state_verified{false};
};

enum class XR806OTARuntimeError : uint8_t {
  OK,
  SNAPSHOT_NOT_OBSERVED,
  INVALID_SEQUENCE,
  SELECTION_MISMATCH,
  FDCM_UNVERIFIED,
  FLASH_ID_UNSUPPORTED,
  INVALID_GEOMETRY,
  EXCEEDS_SLOT,
  RUNNING_IMAGE_OVERLAP,
  METADATA_OVERLAP,
  IMAGE_FORMAT_UNVERIFIED,
  BOOT_RECOVERY_UNVERIFIED,
};

struct XR806OTARuntimeProof {
  // External evidence from matching target firmware, hardware, and recovery.
  bool vendor_image_format_verified{false};
  bool boot_recovery_verified{false};
};

struct XR806OTARuntimeResult {
  XR806OTARuntimeError error{XR806OTARuntimeError::SNAPSHOT_NOT_OBSERVED};
  uint8_t candidate_seq{0xff};
  XR806OTAFlashRange candidate{};
  XR806OTAFlashRange running{};
  XR806OTAFlashRange metadata{};
  bool can_erase{false};  // ALWAYS false here; no write authorization API.
};

constexpr XR806OTARuntimeResult xr806_preflight_runtime_ota(const XR806OTARuntimeGeometry &g,
                                                            const XR806OTARuntimeProof &proof) {
  XR806OTARuntimeResult out{};
  if (!g.runtime_snapshot_observed)
    return out;

  if (g.running_seq >= 2 || g.fdcm_selected_seq >= 2) {
    out.error = XR806OTARuntimeError::INVALID_SEQUENCE;
    return out;
  }
  if (g.running_seq != g.fdcm_selected_seq) {
    out.error = XR806OTARuntimeError::SELECTION_MISMATCH;
    return out;
  }
  if (!g.fdcm_state_verified) {
    out.error = XR806OTARuntimeError::FDCM_UNVERIFIED;
    return out;
  }

  const uint8_t candidate = g.running_seq ^ 1u;
  out.candidate_seq = candidate;
  // A single flash device is the only verified case; do not assume external
  // SPI flash IDs or address spaces are interchangeable.
  if (g.image_flash[g.running_seq] != g.image_flash[candidate] || g.image_flash[g.running_seq] != g.metadata_flash) {
    out.error = XR806OTARuntimeError::FLASH_ID_UNSUPPORTED;
    return out;
  }

  const uint32_t candidate_kib = g.compression_policy ? g.compressed_max_kib : g.image_max_kib;
  if (!g.flash_bytes || !g.metadata_bytes || !candidate_kib || !g.image_max_kib || candidate_kib > UINT32_MAX / 1024U ||
      g.image_max_kib > UINT32_MAX / 1024U) {
    out.error = XR806OTARuntimeError::INVALID_GEOMETRY;
    return out;
  }
  out.metadata = {g.metadata_addr, g.metadata_bytes};
  out.candidate = {g.image_addr[candidate], candidate_kib * 1024U};
  out.running = {g.image_addr[g.running_seq], g.image_max_kib * 1024U};

  constexpr uint32_t sector = 4096;
  if (!xr806_ota_range_valid(out.metadata, g.flash_bytes) || !xr806_ota_range_valid(out.candidate, g.flash_bytes) ||
      !xr806_ota_range_valid(out.running, g.flash_bytes) || (out.metadata.offset % sector) ||
      (out.metadata.length % sector) || (out.candidate.offset % sector) || (out.candidate.length % sector) ||
      (out.running.offset % sector) || (g.bootloader_bytes == 0) || (out.running.offset < g.bootloader_bytes)) {
    out.error = XR806OTARuntimeError::INVALID_GEOMETRY;
    return out;
  }
  if (g.requested_image_bytes == 0 || g.requested_image_bytes > out.candidate.length) {
    out.error = XR806OTARuntimeError::EXCEEDS_SLOT;
    return out;
  }
  if (xr806_ota_overlap(out.running, out.candidate) || xr806_ota_overlap(out.running, out.metadata)) {
    out.error = XR806OTARuntimeError::RUNNING_IMAGE_OVERLAP;
    return out;
  }
  if (xr806_ota_overlap(out.candidate, out.metadata)) {
    out.error = XR806OTARuntimeError::METADATA_OVERLAP;
    return out;
  }
  if (!proof.vendor_image_format_verified) {
    out.error = XR806OTARuntimeError::IMAGE_FORMAT_UNVERIFIED;
    return out;
  }
  if (!proof.boot_recovery_verified) {
    out.error = XR806OTARuntimeError::BOOT_RECOVERY_UNVERIFIED;
    return out;
  }
  // A successful geometric preflight is not a flash-write permission.
  out.error = XR806OTARuntimeError::OK;
  return out;
}

}  // namespace esphome::xr806
