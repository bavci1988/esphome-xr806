#pragma once
#ifdef USE_XR806

#include <cstddef>
#include <cstdint>

#include "esphome/core/preference_backend.h"
#include "preference_journal.h"

namespace esphome::xr806 {

/// Preferences in two reserved 4 KiB FDCM sectors of the WXU 2 MiB flash (1972 and 1976 KiB,
/// next to the SDK sysinfo sector and outside both OTA regions). Each commit alternates sectors.
class XR806Preferences final : public PreferencesMixin<XR806Preferences> {
 public:
  using PreferencesMixin<XR806Preferences>::make_preference;

  // There is no RTC storage, so every preference lives in flash.
  ESPPreferenceObject make_preference(size_t length, uint32_t type, bool /*in_flash*/) {
    return this->make_preference(length, type);
  }
  ESPPreferenceObject make_preference(size_t length, uint32_t type);
  /// One-shot read of a stored preference by key
  bool load_from_key(uint32_t key, uint8_t *dest, size_t len);
  bool save_to_key(uint32_t key, const uint8_t *src, size_t len);
  bool sync();
  bool reset();

  static constexpr size_t SNAPSHOT_SIZE = 2048;

 protected:
  bool open_();

  PreferenceJournal<SNAPSHOT_SIZE> journal_;
  bool initialized_{false};
  bool available_{false};
};

}  // namespace esphome::xr806

DECLARE_PREFERENCE_ALIASES(esphome::xr806::XR806Preferences)

#endif  // USE_XR806
