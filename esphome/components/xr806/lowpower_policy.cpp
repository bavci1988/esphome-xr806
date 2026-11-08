// MCU-agnostic, fail-closed XR806 system standby policy singleton.
// Stage 7a does not call pm_enter_mode(): hardware suspend is intentionally absent.
#include "esphome/core/defines.h"
#ifdef USE_XR806_LOWPOWER
#include "esphome/components/xr806/lowpower_policy.h"

namespace esphome::xr806 {

XR806LowPowerPolicy &lowpower_policy() {
  static XR806LowPowerPolicy policy;
  return policy;
}

}  // namespace esphome::xr806
#endif
