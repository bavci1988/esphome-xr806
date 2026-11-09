#pragma once
// Runtime ROM/FDCM geometry validation for writable XRadio compressed OTA.
// This is a physical range check, not an unconditional write-disable flag.
#include <cstdint>
namespace esphome::xr806 {
struct WriterRange {
  uint32_t start{0};
  uint32_t size{0};
};
struct WriterGeometry {
  uint32_t flash_size{0}, metadata_addr{0}, metadata_size{0};
  uint32_t image_addr[2]{}, image_max_kib{0}, compressed_max_kib{0}, bootloader_bytes{0}, upload_bytes{0};
  uint8_t flash_id[2]{}, metadata_flash_id{0}, running_seq{0xff}, selected_seq{0xff};
  bool selected_verified{false};
};
enum class WriterError : uint8_t { OK, SEQUENCE, FLASH, GEOMETRY, OVERLAP, SIZE };
struct WriterPlan {
  WriterError error{WriterError::GEOMETRY};
  uint8_t candidate_seq{0xff};
  WriterRange active{}, candidate{}, metadata{};
};
constexpr bool within(WriterRange r, uint32_t capacity) {
  return r.size && r.start < capacity && static_cast<uint64_t>(r.start) + r.size <= capacity;
}
constexpr bool overlap(WriterRange a, WriterRange b) {
  return static_cast<uint64_t>(a.start) < static_cast<uint64_t>(b.start) + b.size &&
         static_cast<uint64_t>(b.start) < static_cast<uint64_t>(a.start) + a.size;
}
constexpr WriterPlan validate_writer(const WriterGeometry &g) {
  WriterPlan p{};
  // Vendor compression bootloader runs sequence 0. Sequence 1 is the
  // compressed staging area expanded into seq 0 during the next reboot.
  if (g.running_seq != 0 || g.selected_seq != 0 || !g.selected_verified) {
    p.error = WriterError::SEQUENCE;
    return p;
  }
  if (g.flash_id[0] != g.flash_id[1] || g.flash_id[0] != g.metadata_flash_id) {
    p.error = WriterError::FLASH;
    return p;
  }
  constexpr uint32_t sector = 4096;
  if (!g.flash_size || (g.flash_size % sector) || !g.bootloader_bytes || (g.bootloader_bytes % sector) ||
      (g.metadata_addr % sector) || !g.metadata_size || (g.metadata_size % sector) || (g.image_addr[0] % sector) ||
      (g.image_addr[1] % sector) || !g.image_max_kib || !g.compressed_max_kib || g.image_max_kib > UINT32_MAX / 1024u ||
      g.compressed_max_kib > UINT32_MAX / 1024u) {
    p.error = WriterError::GEOMETRY;
    return p;
  }
  p.candidate_seq = 1;
  p.metadata = {g.metadata_addr, g.metadata_size};
  p.active = {g.image_addr[0], g.image_max_kib * 1024u};
  p.candidate = {g.image_addr[1], g.compressed_max_kib * 1024u};
  if (!within(p.metadata, g.flash_size) || !within(p.active, g.flash_size) || !within(p.candidate, g.flash_size) ||
      p.active.start < g.bootloader_bytes || p.candidate.start < g.bootloader_bytes ||
      p.metadata.start < g.bootloader_bytes ||
      static_cast<uint64_t>(p.active.start) + p.active.size > p.metadata.start) {
    p.error = WriterError::GEOMETRY;
    return p;
  }
  if (overlap(p.active, p.candidate) || overlap(p.active, p.metadata) || overlap(p.candidate, p.metadata)) {
    p.error = WriterError::OVERLAP;
    return p;
  }
  if (g.upload_bytes && g.upload_bytes > p.candidate.size) {
    p.error = WriterError::SIZE;
    return p;
  }
  p.error = WriterError::OK;
  return p;
}
}  // namespace esphome::xr806
