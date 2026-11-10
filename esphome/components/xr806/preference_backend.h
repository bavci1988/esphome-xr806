#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::xr806 {

class XR806Preferences;

// ESPHome preference handles hold this tiny immutable descriptor; the
// flash-backed manager owns the actual records and both FDCM slots.
class XR806PreferenceBackend {
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
