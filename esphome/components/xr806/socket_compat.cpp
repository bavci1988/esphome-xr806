#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_SOCKET_IMPL_LWIP_SOCKETS)

#include "esphome/components/xr806/socket_compat.h"
#include "esphome/components/socket/headers.h"

namespace esphome::socket {

// Transitional implementation: no event-driven FD wake yet.
bool wake_register_fd(int /*fd*/) {
  return false;
}

void wake_unregister_fd(int /*fd*/) {}

// Poll readiness without blocking the ESPHome main loop.
bool socket_ready_fd(int fd, bool /*loop_monitored*/) {
  if (fd < 0 || fd >= FD_SETSIZE)
    return true;

  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);

  struct timeval timeout{};

  const int result = lwip_select(
      fd + 1, &fds, nullptr, nullptr, &timeout);

  return result != 0;
}

}  // namespace esphome::socket

#endif
