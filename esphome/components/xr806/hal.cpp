#ifdef USE_XR806

#include "esphome/core/hal.h"
#include "esphome/core/main_task.h"

// Haupttask wird später durch Application::setup() registriert.
extern "C" {
TaskHandle_t esphome_main_task_handle = nullptr;
}

namespace esphome {

void arch_init() {
  // OpenXR806 platform_init() wurde bereits aufgerufen.
  // Keine erneute Hardware- oder Watchdog-Initialisierung.
}

// Vollständiger Systemneustart über den XR806-SDK-Watchdog
[[noreturn]] void arch_restart() {
  xr806_reboot();
  while (true) {
  }
}

}  // namespace esphome

#endif  // USE_XR806
