#pragma once

// XR806-specific ESPHome Native API compatibility.
// Manufacturer is reported in DeviceInfoResponse.
//
// No effect on other CPUs or builds without the Native API.

#if defined(USE_XR806) && defined(USE_API)

#ifndef ESPHOME_MANUFACTURER
#define ESPHOME_MANUFACTURER "XRadio"
#endif

#endif

#ifdef __cplusplus
// Force-included into every XR806 C++ unit; declares no platform API.
namespace esphome::xr806 {}
#endif
