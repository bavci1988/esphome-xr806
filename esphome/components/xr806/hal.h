#pragma once

#ifdef USE_XR806

#include <cstdint>
#include "esphome/core/main_task.h"


// XR806: ISR-relevanten Code im SRAM platzieren
#ifndef IRAM_ATTR
#define IRAM_ATTR __attribute__((section(".sram_text")))
#endif



// Implementiert im OpenXR806-SDK
extern "C" uint32_t xr806_millis(void);
extern "C" uint64_t xr806_micros64(void);
extern "C" void xr806_delay(uint32_t ms);
extern "C" void xr806_delay_us(uint32_t us);
extern "C" void xr806_wdt_feed(void);
extern "C" void xr806_reboot(void);
extern "C" uint32_t xr806_cpu_freq_hz(void);

namespace esphome {
// Prüft, ob wir im registrierten ESPHome-Haupttask laufen.
__attribute__((always_inline)) inline bool is_main_loop_thread() {
  return esphome_main_task_handle != nullptr &&
         xTaskGetCurrentTaskHandle() == esphome_main_task_handle;
}



void arch_init();

// Cortex-M33: Interrupt- und Exception-Kontext erkennen
__attribute__((always_inline)) inline bool in_isr_context() {
  uint32_t ipsr;
  __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
  return ipsr != 0;
}

inline uint64_t micros_64() {
  return xr806_micros64();
}

inline uint32_t micros() {
  return static_cast<uint32_t>(xr806_micros64());
}

inline uint64_t millis_64() {
  return xr806_micros64() / 1000ULL;
}

inline uint32_t millis() {
  return static_cast<uint32_t>(millis_64());
}

inline void delay(uint32_t ms) {
  xr806_delay(ms);
}

inline void delayMicroseconds(uint32_t us) {
  xr806_delay_us(us);
}

inline uint32_t arch_get_cpu_freq_hz() {
  return xr806_cpu_freq_hz();
}

inline void arch_feed_wdt() {
  xr806_wdt_feed();
}

inline void yield() {
  xr806_delay(0);
}

}  // namespace esphome

#endif  // USE_XR806
