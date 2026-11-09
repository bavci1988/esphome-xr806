#pragma once
// Validate the AWIH/XZ wrapper before ota_push_start() erases the target slot.
#include <cstddef>
#include <cstdint>
namespace esphome::xr806 {
constexpr size_t XR806_OTA_PREFACE_BYTES = 70;  // 64-byte AWIH + 6-byte XZ magic
constexpr uint32_t AWIH_MAGIC = 0x48495741;
constexpr uint32_t AWIH_APP_ID = 0xA5FE5A01;
constexpr uint32_t AWIH_COMPRESS = 1u << 4;
inline uint32_t load_le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint16_t sum16(const uint8_t *p, size_t n) {
  uint16_t sum = 0;
  for (size_t i = 0; i < n; i += 2)
    sum = static_cast<uint16_t>(sum + p[i] + (i + 1 < n ? static_cast<uint16_t>(p[i + 1]) << 8 : 0));
  return sum;
}
struct XZImageInfo { bool valid{false}; uint32_t bytes{0}; };
inline XZImageInfo validate_xz_preface(const uint8_t *p, size_t n, uint32_t slot, uint32_t expected) {
  if (p == nullptr || n < XR806_OTA_PREFACE_BYTES || slot < XR806_OTA_PREFACE_BYTES)
    return {};
  const uint32_t body = load_le32(p + 12);
  const uint32_t flags = load_le32(p + 28);
  if (load_le32(p) != AWIH_MAGIC || load_le32(p + 4) != 5 || sum16(p, 64) != 0xFFFF ||
      load_le32(p + 36) != AWIH_APP_ID || (flags & AWIH_COMPRESS) == 0 ||
      (flags & ((1u << 2) | (1u << 3))) != 0 ||
      load_le32(p + 32) != 0xFFFFFFFFu ||
      body < 6 || load_le32(p + 24) != body || body > slot - 64 ||
      (expected != 0 && expected != body + 64))
    return {};
  constexpr uint8_t XZ_MAGIC[6] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};
  for (size_t i = 0; i < 6; ++i)
    if (p[64 + i] != XZ_MAGIC[i])
      return {};
  return {true, body + 64};
}
}  // namespace esphome::xr806
