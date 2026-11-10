#pragma once

#include <cstddef>
#include <cstdint>

#include "esphome/core/preference_backend.h"

extern "C" {
#include "image/fdcm.h"
}

namespace esphome::xr806 {

// Only the WXU 2 MiB variant is supported. The two 4 KiB preference sectors
// are isolated from the SDK sysinfo sector at 2044 KiB and both OTA slots.
// FDCM is used for flash operations; snapshots add record checksums and a
// two-sector generation journal for interrupted-update recovery.
class XR806Preferences final : public PreferencesMixin<XR806Preferences> {
 public:
  using PreferencesMixin<XR806Preferences>::make_preference;

  ESPPreferenceObject make_preference(size_t length, uint32_t type, bool in_flash);
  ESPPreferenceObject make_preference(size_t length, uint32_t type) {
    return this->make_preference(length, type, true);
  }

  bool load_from_key(uint32_t key, uint8_t *dest, size_t len);
  bool save_to_key(uint32_t key, const uint8_t *src, size_t len);
  bool sync();
  bool reset();

 protected:
  friend class XR806PreferenceBackend;

  static constexpr size_t SNAPSHOT_SIZE = 2048;
  static constexpr uint32_t MAGIC = 0x58525046;  // "FPRX", little-endian
  static constexpr uint16_t VERSION = 1;

  struct SnapshotHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t used;
    uint32_t generation;
    uint32_t checksum;
  };
  static_assert(sizeof(SnapshotHeader) == 16);

  bool open_();
  bool valid_snapshot_(const uint8_t *data, SnapshotHeader *header) const;
  bool read_slot_(uint8_t slot, SnapshotHeader *header);
  uint32_t checksum_(const uint8_t *data, size_t used) const;

  fdcm_handle_t *slots_[2]{};
  alignas(4) uint8_t data_[SNAPSHOT_SIZE]{};
  alignas(4) uint8_t scratch_[SNAPSHOT_SIZE]{};
  size_t used_{0};
  uint32_t generation_{0};
  uint8_t active_slot_{1};  // First write goes to slot 0 when both are blank.
  bool initialized_{false};
  bool available_{false};
  bool dirty_{false};
};

}  // namespace esphome::xr806

DECLARE_PREFERENCE_ALIASES(esphome::xr806::XR806Preferences)
