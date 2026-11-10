#include "debug_component.h"
#ifdef USE_XR806

#include "esphome/core/log.h"

#include <malloc.h>

#include "driver/chip/system_chip.h"

extern "C" void heap_get_space(uint8_t **start, uint8_t **end, uint8_t **current);

namespace esphome::debug {

ESPHOME_LOG_TAG(TAG, "debug");

static const char *get_startup_reason() {
  // SystemInit saves the startup state before clearing the reset registers.
  switch (SysGetStartupState()) {
    case SYS_POWERON:
      return "Power-on reset";
    case SYS_WATCHDOG_CHIP_RST:
      return "Chip watchdog reset";
    case SYS_WATCHDOG_CPU_RST:
      return "CPU watchdog reset";
    case SYS_SLEEP:
      return "Sleep wakeup";
    case SYS_STANDBY:
      return "Standby wakeup";
    case SYS_HIBERNATION:
      return "Hibernation wakeup";
    case SYS_REBOOT:
      return "Software reset";
    case SYS_CPU_RST:
      return "Software CPU reset";
    case SYS_NVIC_RST:
      return "NVIC system reset";
    default:
      return "Unknown";
  }
}

// Preserve the boot reason before a later sleep or standby changes the SDK state.
DebugComponent::DebugComponent() : startup_reason_(get_startup_reason()) {}

const char *DebugComponent::get_reset_reason_(std::span<char, RESET_REASON_BUFFER_SIZE> buffer) {
  return this->startup_reason_;
}

uint32_t DebugComponent::get_free_heap_() {
  uint8_t *start;
  uint8_t *end;
  uint8_t *current;
  heap_get_space(&start, &end, &current);
  const auto capacity = reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(start);
  // Subtract live allocations so freed Newlib chunks and uncommitted SRAM both count as free.
  const auto used = static_cast<uint32_t>(mallinfo().uordblks);
  return used < capacity ? capacity - used : 0;
}

size_t DebugComponent::get_device_info_(std::span<char, DEVICE_INFO_BUFFER_SIZE> buffer, size_t pos) {
  ESP_LOGD(TAG,
           "XR806 debug info:\n"
           "  Board: %s\n"
           "  Reset Reason: %s",
           ESPHOME_BOARD, this->startup_reason_);

  return buf_append_printf(buffer.data(), buffer.size(), pos, "|Chip Name: XR806|Board: %s|Reset Reason: %s",
                           ESPHOME_BOARD, this->startup_reason_);
}

void DebugComponent::update_platform_() {}

}  // namespace esphome::debug
#endif
