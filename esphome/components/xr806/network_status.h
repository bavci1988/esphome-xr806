#pragma once

// XR806 network state snapshot.
// Independent of ESPHome Core and SDK headers.

namespace esphome::xr806 {

struct XR806NetworkSnapshot {
  bool associated{false};
  bool interface_up{false};
  bool link_up{false};
  bool ipv4_assigned{false};

  char ipv4[16]{};
  char gateway[16]{};
  char netmask[16]{};

  bool ready() const {
    return associated &&
           interface_up &&
           link_up &&
           ipv4_assigned;
  }
};

}  // namespace esphome::xr806

// Forward declaration; SDK headers stay in the implementation.
struct netif;

namespace esphome::xr806 {

XR806NetworkSnapshot read_xr806_network_snapshot(
    struct netif *interface);

}  // namespace esphome::xr806
