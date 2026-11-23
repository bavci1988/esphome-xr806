#pragma once

#include <cstdint>

namespace esphome::xr806 {

/// The wake lines of a Tuya MCU, mapped to XR806 GPIO numbers (PA0..PA31 = 0..31, PB0.. = 32..).
struct TuyaWakeLines {
  int wake_input{-1};  // MCU to module; a standby wake GPIO on port A
  int mcu_wake{-1};    // module to MCU
};

/// Tuya product info names the wake lines in the pin numbering of the module the MCU firmware was
/// written for. A WXU (XR806) numbers PA0..PA23 as 0..23 and PB0.. as 24..; a T3-U (BK7236) in the
/// same housing uses its own pin numbers. The pads both modules share decide the mapping:
/// T3-U P32 is pad 9 (WXU PA20), T3-U P16 is pad 2 (WXU PB4).
inline TuyaWakeLines map_tuya_wake_lines(int wake_gpio, int mcu_wake_gpio) {
  TuyaWakeLines lines;
  const bool xr806_wake = wake_gpio == 11 || wake_gpio == 12 || wake_gpio == 13 || wake_gpio == 19 || wake_gpio == 20;
  if (xr806_wake) {
    lines.wake_input = wake_gpio;
    if (mcu_wake_gpio >= 0 && mcu_wake_gpio < 24) {
      lines.mcu_wake = mcu_wake_gpio;
    } else if (mcu_wake_gpio >= 24 && mcu_wake_gpio < 48) {
      lines.mcu_wake = 32 + (mcu_wake_gpio - 24);
    }
    return lines;
  }
  if (wake_gpio == 32)
    lines.wake_input = 20;
  if (mcu_wake_gpio == 16)
    lines.mcu_wake = 32 + 4;
  return lines;
}

}  // namespace esphome::xr806
