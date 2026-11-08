// ESPHome runtime bridge for the Allwinner/XRadio XR806.
//
// Called from the main FreeRTOS task created by the vendor SDK.
// The SDK must complete platform_init() before entering this function.

extern void setup();
extern void loop();

// XR806_LOWPOWER_POLICY_V1
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

extern "C" void xr806_esphome_run(void) {
  setup();

  while (true) {
    loop();
  }
}
