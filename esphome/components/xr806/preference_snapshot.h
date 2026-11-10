#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Portable, allocation-free on-flash snapshot format. This header deliberately
// has no dependency on the XRadio SDK and is host-testable.
namespace esphome::xr806 {

struct PreferenceSnapshotHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t used;
  uint32_t generation;
  uint32_t checksum;
};
static_assert(sizeof(PreferenceSnapshotHeader) == 16);

inline uint32_t preference_snapshot_checksum(const uint8_t *data, size_t used) {
  uint32_t crc = 0xFFFFFFFF;
  auto update = [&crc](const uint8_t *bytes, size_t n) {
    for (size_t j = 0; j < n; j++) {
      crc ^= bytes[j];
      for (unsigned bit = 0; bit < 8; bit++)
        crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  };
  update(data, 12);  // 12-byte header prefix; exclude checksum at byte 12.
  update(data + sizeof(PreferenceSnapshotHeader), used);
  return ~crc;
}

inline bool preference_snapshot_valid(const uint8_t *data, size_t capacity, uint32_t magic, uint16_t version,
                                     PreferenceSnapshotHeader *out) {
  if (data == nullptr || out == nullptr || capacity < sizeof(PreferenceSnapshotHeader))
    return false;
  PreferenceSnapshotHeader header;
  memcpy(&header, data, sizeof(header));
  if (header.magic != magic || header.version != version ||
      header.used > capacity - sizeof(PreferenceSnapshotHeader))
    return false;

  size_t cursor = sizeof(PreferenceSnapshotHeader);
  const size_t end = cursor + header.used;
  while (cursor < end) {
    if (end - cursor < 6)
      return false;
    uint16_t length;
    memcpy(&length, data + cursor + 4, sizeof(length));
    if (length == 0 || length > end - cursor - 6)
      return false;
    cursor += 6 + length;
  }
  if (cursor != end || preference_snapshot_checksum(data, header.used) != header.checksum)
    return false;
  *out = header;
  return true;
}

// Monotonic journal generations are compared modulo 2^32; a wrap from
// 0xFFFFFFFF to 0 is considered a newer generation.
inline bool preference_generation_newer(uint32_t candidate, uint32_t current) {
  return static_cast<int32_t>(candidate - current) > 0;
}

}  // namespace esphome::xr806
