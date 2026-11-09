#pragma once

#ifdef USE_XR806

// Load the XR806 SDK's lwIP configuration before ESPHome
// includes lwip/sockets.h.
//
// The vendor lwipopts.h unconditionally defines
// LWIP_COMPAT_SOCKETS=1. Loading it first allows ESPHome's
// socket/headers.h to disable those compatibility macros
// without the vendor configuration overriding that setting.

#include "lwip/opt.h"

namespace esphome::xr806 {

// Non-blocking lwIP readiness poll used by the socket hooks below.
bool lwip_fd_readable(int fd);

}  // namespace esphome::xr806

namespace esphome::socket {

bool wake_register_fd(int fd);
void wake_unregister_fd(int fd);
bool socket_ready_fd(int fd, bool loop_monitored);

}  // namespace esphome::socket
#endif
