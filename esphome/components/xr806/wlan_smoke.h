#pragma once

// Legacy development alias. New configs use xr806.wifi and XR806WiFiComponent.
#include "esphome/components/xr806/wifi_component.h"

#ifdef USE_XR806_WLAN_SMOKE
namespace esphome::xr806 {
using XR806WLANSmoke = XR806WiFiComponent;
}  // namespace esphome::xr806
#endif
