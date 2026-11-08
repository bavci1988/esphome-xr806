#pragma once

// XR806 platform compatibility for ESPHome's network/IPAddress.
//
// ESPHome network/ip_address.h includes lwip/ip_addr.h
// only for its currently recognized lwIP platforms.
// XR806 also uses lwIP, so provide the types before
// network/ip_address.h is parsed.
//
// Keep this outside the generic ESPHome Core.

#if defined(USE_XR806) && defined(USE_NETWORK)
#include "lwip/ip_addr.h"
#endif
