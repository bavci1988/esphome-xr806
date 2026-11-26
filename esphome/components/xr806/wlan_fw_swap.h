#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_XR806_API_KEEPALIVE_OFFLOAD)

#include <cstdint>

#include "esphome/components/wifi/wifi_component.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

namespace esphome::xr806 {

/// True while the WLAN core runs the patched firmware with the keepalive frame list.
bool wlan_fw_patched_active();

/// Starts a restart of the WLAN core after it hung in standby, with the firmware it ran before; the caller must have
/// brought the core's power domain back (wlan_power::revive_domain). False when there is no firmware swap component
/// or a recent restart of the core did not keep it working; the caller then restarts the device.
bool wlan_fw_recover_after_hang();

/// Loads the patched WLAN firmware a while after boot, so a firmware that breaks the WLAN still leaves time for an
/// OTA update at every start. The flash image keeps the stock wlan_fw.bin, which the driver loads at boot. Once the
/// station has been connected for SWAP_AFTER_MS, the WLAN core is stopped and started again (net_sys_onoff), and the
/// driver's firmware reads return the patched copy held in the application. If the station does not connect again
/// within VERIFY_MS, the stock firmware is loaded back and kept until the next boot.
/// With load_at_boot (USE_XR806_WLAN_FW_LOAD_AT_BOOT) the driver gets the patched firmware already at boot; the same
/// check and fallback apply, and after BOOT_ATTEMPTS starts in a row without a confirmed connection the stock
/// firmware is used until the next firmware update.
class XR806WlanFirmwareSwap final : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool request_recovery();

 protected:
  void restart_wlan_(bool patched);
  void fall_back_(wifi::WiFiComponent *wifi);

  enum class Phase : uint8_t { WAIT, RESTART, VERIFY, FALLBACK, ACTIVE, STOCK };
  Phase phase_{Phase::WAIT};
  uint32_t since_ms_{0};
  uint32_t recovered_ms_{0};
  bool recovering_{false};  // a restart after a hang that has not been confirmed by a connection yet
  bool restart_patched_{true};
#ifdef USE_XR806_WLAN_FW_LOAD_AT_BOOT
  ESPPreferenceObject attempts_pref_;
#endif
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_XR806_API_KEEPALIVE_OFFLOAD
