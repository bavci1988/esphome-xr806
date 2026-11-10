#pragma once
#ifdef USE_XR806

#include <cstddef>
#include <cstdint>

namespace esphome::xr806 {

class XR806Preferences;

/// Handle held by an ESPPreferenceObject; the manager owns the records and both flash slots.
class XR806PreferenceBackend final {
 public:
  XR806PreferenceBackend(XR806Preferences *owner, uint32_t key, size_t length)
      : owner_(owner), key_(key), length_(length) {}

  bool save(const uint8_t *src, size_t len);
  bool load(uint8_t *dest, size_t len);

 protected:
  XR806Preferences *owner_;
  uint32_t key_;
  size_t length_;
};

}  // namespace esphome::xr806

namespace esphome {
using PreferenceBackend = xr806::XR806PreferenceBackend;
}  // namespace esphome

#endif  // USE_XR806
