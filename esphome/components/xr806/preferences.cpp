#include "esphome/core/defines.h"

#ifdef USE_XR806

#include "esphome/core/preferences.h"

namespace esphome {

// XR806_PREFERENCES_V1
// Initial backend: non-persistent ESPHome fallback.
// No flash writes. Persistent storage will be added separately.
namespace {
ESPPreferences xr806_preferences;
}

// EntityBase and other components require this global manager.
ESPPreferences *global_preferences = &xr806_preferences;

}  // namespace esphome

#endif  // USE_XR806
