#pragma once

// The WLAN core's power domain (SYS3) as seen from the CPU's power control block.

#include <cstdint>

#include "esphome/core/hal.h"

namespace esphome::xr806::wlan_power {

// Used only around a hang, so the code stays in flash and leaves the RAM to the heap.
#define XR806_WLAN_POWER_FLASH __attribute__((noinline, section(".xip_text.wlan_power")))

static constexpr uint32_t POWER_CONTROL_BASE = 0x40040000;
static constexpr uint32_t POWER_CONTROL_SIZE = 0x270;                     // up to the RC oscillator calibration control
static constexpr uint32_t POWER_SYS3_CTRL = POWER_CONTROL_BASE + 0x90;    // bit 0 clear: domain held in reset
static constexpr uint32_t POWER_SYS3_STATUS = POWER_CONTROL_BASE + 0x94;  // bit 0: alive
static constexpr uint32_t POWER_WLAN_HIF_OVERRIDE = POWER_CONTROL_BASE + 0x21C;  // bit 1: hold CPU, bit 2: wake
static constexpr uint32_t HIF_HOLD_CPU = 0x2;
static constexpr uint32_t HIF_WAKE = 0x4;

inline volatile uint32_t &cpu_register(uint32_t address) { return *reinterpret_cast<volatile uint32_t *>(address); }
inline bool wlan_alive() { return (cpu_register(POWER_SYS3_STATUS) & 1) != 0; }

XR806_WLAN_POWER_FLASH inline bool wait_alive(uint32_t ms, uint32_t *waited = nullptr) {
  uint32_t i = 0;
  for (; i < ms && !wlan_alive(); ++i)
    delayMicroseconds(1000);
  if (waited != nullptr)
    *waited += i;
  return wlan_alive();
}

/// Takes the domain through its reset, as the driver does when it starts the WLAN. With hold_cpu the WLAN processor
/// stays in reset afterwards (the caller releases it), so that it cannot run and overwrite its memory.
XR806_WLAN_POWER_FLASH inline bool reset_domain(bool hold_cpu) {
  volatile uint32_t &hif = cpu_register(POWER_WLAN_HIF_OVERRIDE);
  volatile uint32_t &sys3 = cpu_register(POWER_SYS3_CTRL);
  hif = hif | HIF_HOLD_CPU;
  sys3 = sys3 & ~0x1U;
  delayMicroseconds(2000);
  sys3 = sys3 | 0x1U;
  const bool alive = wait_alive(300);
  if (!hold_cpu)
    hif = hif & ~HIF_HOLD_CPU;
  return alive;
}

/// After a hang in standby the domain stays down and ignores the wake request; reading the core's memory or a
/// driver request then blocks the CPU. Brings the domain back: the wake request first, then a reset of the domain.
/// Returns 0 if it was alive, 1 after a wake, 2 after a reset, 3 when it stays down.
XR806_WLAN_POWER_FLASH inline uint8_t revive_domain() {
  if (wlan_alive())
    return 0;
  volatile uint32_t &hif = cpu_register(POWER_WLAN_HIF_OVERRIDE);
  hif = hif | HIF_WAKE;
  if (wait_alive(300))
    return 1;
  return reset_domain(false) ? 2 : 3;
}

}  // namespace esphome::xr806::wlan_power
