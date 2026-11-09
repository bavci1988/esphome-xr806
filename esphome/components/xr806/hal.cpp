#ifdef USE_XR806

#include "esphome/core/hal.h"
#include "esphome/core/main_task.h"

// Set by Application::setup() once the main loop task is known.
extern "C" {
TaskHandle_t esphome_main_task_handle = nullptr;
}

namespace esphome {

void arch_init() {
  // The vendor platform_init() has already set up the hardware and the watchdog.
}

// Full system restart through the vendor SDK watchdog.
[[noreturn]] void arch_restart() {
  xr806_reboot();
  while (true) {
  }
}

}  // namespace esphome

#endif  // USE_XR806
