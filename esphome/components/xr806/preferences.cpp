#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "esphome/core/preferences.h"

namespace esphome::xr806 {

// Initial backend: non-persistent ESPHome fallback.
// No flash writes. Persistent storage will be added separately.
static ESPPreferences xr806_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::xr806

namespace esphome {

// EntityBase and other components require this global manager.
ESPPreferences *global_preferences =  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    &xr806::xr806_preferences;

}  // namespace esphome

#endif  // USE_XR806
