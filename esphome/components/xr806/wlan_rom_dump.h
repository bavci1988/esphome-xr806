#pragma once

#include "esphome/core/defines.h"

#if defined(USE_XR806) && defined(USE_XR806_WLAN_ROM_DUMP)

#include <cstdint>

#include "esphome/core/component.h"

namespace esphome::xr806 {

/// Diagnostics: reads the ROM of the WLAN core through the stock firmware's register debug request (MIB 9) and
/// prints it to the log. Read only; the firmware is not changed. Starts a while after the station connected,
/// first checks that the request returns known patch RAM, then dumps the ROM and a CRC-32.
class XR806WlanRomDump final : public Component {
 public:
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  bool read_words_(uint32_t address, uint32_t *words, uint32_t count);
  void check_();
  void dump_step_();

  enum class Phase : uint8_t { WAIT, CHECK, DUMP, DONE };
  Phase phase_{Phase::WAIT};
  uint32_t connected_ms_{0};
  uint32_t next_address_{0};
  uint32_t crc_{0xFFFFFFFF};
};

}  // namespace esphome::xr806

#endif  // USE_XR806 && USE_XR806_WLAN_ROM_DUMP
