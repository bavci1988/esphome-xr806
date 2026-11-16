#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_XR806_API_KEEPALIVE_OFFLOAD)

#include <cstdint>

#include "esphome/core/component.h"

namespace esphome::xr806 {

/// True while the WLAN core runs the patched firmware with the keepalive frame list.
bool wlan_fw_patched_active();

/// Loads the patched WLAN firmware a while after boot, so a firmware that breaks the WLAN still leaves time for an
/// OTA update at every start. The flash image keeps the stock wlan_fw.bin, which the driver loads at boot. Once the
/// station has been connected for SWAP_AFTER_MS, the WLAN core is stopped and started again (net_sys_onoff), and the
/// driver's firmware reads return the patched copy held in the application. If the station does not connect again
/// within VERIFY_MS, the stock firmware is loaded back and kept until the next boot.
class XR806WlanFirmwareSwap final : public Component {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  void restart_wlan_(bool patched);

  enum class Phase : uint8_t { WAIT, RESTART, VERIFY, ACTIVE, STOCK };
  Phase phase_{Phase::WAIT};
  uint32_t since_ms_{0};
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_XR806_API_KEEPALIVE_OFFLOAD
