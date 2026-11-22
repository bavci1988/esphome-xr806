#pragma once

// Diagnostics for WLAN core hangs during standby (xr806: diagnose_wlan_sleep: true). Before every standby and after
// every wake, the WLAN core's timer list, clock and event words are read through the firmware (MIB 9) and printed
// together with the wake result, the time asleep and the driver's frame counters, so one long run holds everything.

#include "esphome/core/defines.h"

#ifdef USE_XR806_WLAN_SLEEP_DIAG

#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "wlan_mib.h"

extern "C" {
#include "kernel/os/os_thread.h"
#include "kernel/os/os_timer.h"
#include "sys/sys_heap.h"
extern volatile int skb_rx_cnt;  // libxrwireless: frames handed up by the WLAN driver
extern volatile int skb_tx_cnt;
void heap_get_space(uint8_t **start, uint8_t **end, uint8_t **current);
}

namespace esphome::xr806::wlan_sleep_diag {

// The standby controller runs from RAM; the diagnostics are only called before and after standby, so their code
// goes to flash and leaves the RAM to the heap.
#define XR806_DIAG_FLASH __attribute__((noinline, section(".xip_text.wlan_diag")))

// ROM timer list (see the lab notes): head at 0xFFF01C6C, nodes {next, prev, expiry, callback, argument}; time is
// [0x0AC00004] + [0xFFF0560C] in microseconds; event words at 0xFFF01C0C (+4: reasons to stay awake).
static constexpr uint32_t TIMER_HEAD = 0xFFF01C6C;
static constexpr uint32_t CLOCK_COUNTER = 0x0AC00004;
static constexpr uint32_t CLOCK_OFFSET = 0xFFF0560C;
static constexpr uint32_t EVENT_WORD = 0xFFF01C0C;
static constexpr uint32_t AWAKE_WORD = 0xFFF01C10;
static constexpr uint32_t TIMER_HW = 0x0AC00014;  // compare value; +8 control
static constexpr size_t MAX_NODES = 10;

struct Snapshot {
  bool ok{false};
  uint32_t counter{0}, offset{0}, events{0}, awake{0}, busy{0}, hw_compare{0}, hw_ctrl{0};
  uint8_t nodes{0};
  uint32_t node[MAX_NODES]{};
  uint32_t expiry[MAX_NODES]{};
  uint32_t callback[MAX_NODES]{};
};

inline bool read_word(uint32_t address, uint32_t &value) { return wlan_read_words(address, &value, 1) == 0; }

XR806_DIAG_FLASH inline void take(Snapshot &s) {
  s = Snapshot{};
  uint32_t head[6]{};
  if (wlan_read_words(TIMER_HEAD, head, 6) != 0)
    return;
  s.busy = head[5];
  if (!read_word(CLOCK_COUNTER, s.counter) || !read_word(CLOCK_OFFSET, s.offset) || !read_word(EVENT_WORD, s.events) ||
      !read_word(AWAKE_WORD, s.awake))
    return;
  read_word(TIMER_HW, s.hw_compare);
  read_word(TIMER_HW + 8, s.hw_ctrl);
  uint32_t node = head[0];
  while (node != 0 && s.nodes < MAX_NODES) {
    uint32_t t[5]{};
    if (wlan_read_words(node, t, 5) != 0)
      break;
    s.node[s.nodes] = node;
    s.expiry[s.nodes] = t[2];
    s.callback[s.nodes] = t[3];
    ++s.nodes;
    node = t[0];
  }
  s.ok = true;
}

// One line: WLAN time, event words, hardware timer, and every timer as callback@ms-until-expiry (negative: overdue).
XR806_DIAG_FLASH inline void print(const char *label, const Snapshot &s) {
  static const char *const TAG = "xr806.wlan_diag";
  if (!s.ok) {
    ESP_LOGW(TAG, "%s: WLAN core does not answer MIB 9", label);
    return;
  }
  const uint32_t now = s.counter + s.offset;
  char timers[MAX_NODES * 20 + 1]{};
  size_t pos = 0;
  bool overdue = false;
  for (uint8_t i = 0; i < s.nodes && pos < sizeof(timers) - 20; ++i) {
    const int32_t left_ms = static_cast<int32_t>(s.expiry[i] - now) / 1000;
    overdue = overdue || left_ms < -50;
    pos += snprintf(timers + pos, sizeof(timers) - pos, " %05" PRIX32 "@%" PRId32, s.callback[i] & 0xFFFFF, left_ms);
  }
  ESP_LOGI(TAG,
           "%s: now %08" PRIX32 " (%08" PRIX32 "+%08" PRIX32 ") ev %08" PRIX32 " awake %08" PRIX32 " busy %" PRIu32
           " hw %08" PRIX32 "/%08" PRIX32 " timers%s%s",
           label, now, s.counter, s.offset, s.events, s.awake, s.busy, s.hw_compare, s.hw_ctrl, timers,
           overdue ? " OVERDUE" : "");
}

// The WLAN core's memory is mapped into the CPU's address space (FW_ADDR_TO_APP_ADDR in the vendor driver), so it
// can be read even when the core no longer answers requests.
inline const volatile uint32_t *app_address(uint32_t wlan_address) {
  if ((wlan_address & 0xFFF00000) == 0xFFF00000)
    return reinterpret_cast<const volatile uint32_t *>(0x68000000 + (wlan_address & 0xFFFF));
  if ((wlan_address & 0xFF000000) == 0x09000000)
    return reinterpret_cast<const volatile uint32_t *>(0x6800C000 + (wlan_address & 0x3FFFF));
  return reinterpret_cast<const volatile uint32_t *>(wlan_address | 0x60000000);
}

// CPU-side power control block. SYS3 is the WLAN core's power domain: the driver wakes it with the WUP bit and
// waits for the ALIVE bit ("Device cannot wakeup during 500ms" when it stays clear).
static constexpr uint32_t POWER_CONTROL_BASE = 0x40040000;
static constexpr uint32_t POWER_CONTROL_SIZE = 0x240;
static constexpr uint32_t POWER_SYS3_STATUS = POWER_CONTROL_BASE + 0x94;         // bit 0: alive
static constexpr uint32_t POWER_WLAN_HIF_OVERRIDE = POWER_CONTROL_BASE + 0x21C;  // bit 2: wake request

inline volatile uint32_t &cpu_register(uint32_t address) { return *reinterpret_cast<volatile uint32_t *>(address); }
inline bool wlan_alive() { return (cpu_register(POWER_SYS3_STATUS) & 1) != 0; }

XR806_DIAG_FLASH inline void dump_power_control(const char *label) {
  static const char *const TAG = "xr806.wlan_diag";
  for (uint32_t address = POWER_CONTROL_BASE; address < POWER_CONTROL_BASE + POWER_CONTROL_SIZE; address += 32) {
    uint32_t w[8];
    for (uint8_t i = 0; i < 8; ++i)
      w[i] = cpu_register(address + 4 * i);
    ESP_LOGE(TAG,
             "%s %03" PRIX32 ": %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32
             " %08" PRIX32 " %08" PRIX32,
             label, address - POWER_CONTROL_BASE, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    delay(3);
  }
}

// Parts of the WLAN core's memory that show where a hung core stands: scheduler words and timer list head,
// clock, message ring for the CPU (+0xC read, +0x10 write index), receive counters, keepalive state, and all
// stacks, which hold the return addresses and any exception frame.
struct Region {
  const char *label;
  uint32_t start;
  uint32_t words;
};
static constexpr Region REGIONS[] = {
    {"sched", 0xFFF01C00, 40}, {"clock", 0xFFF05600, 16},     {"hostq", 0xFFF02AA0, 16},
    {"rxcnt", 0xFFF02280, 32}, {"keepalive", 0x0900BB80, 72}, {"stack", 0xFFF0B000, 1024},
};
static constexpr uint32_t CAPTURE_WORDS = 40 + 16 + 16 + 32 + 72 + 1024;

struct Capture {
  uint32_t status_first{0}, hif_first{0}, status_last{0}, hif_last{0};
  uint32_t waited_ms{0};
  bool copied{false};
  uint32_t *words{nullptr};
};

// Runs first after a wake that left the driver failed, with the scheduler stopped, so that the driver's recovery
// task cannot reload the core's firmware before its memory is copied. The memory can only be read while the
// core's power domain reports alive; reading it otherwise blocks the CPU. When it is not alive, the wake request
// is set once more and the domain gets 300 ms.
XR806_DIAG_FLASH inline void capture(Capture &c) {
  c.words = static_cast<uint32_t *>(malloc(CAPTURE_WORDS * sizeof(uint32_t)));
  volatile uint32_t &hif = cpu_register(POWER_WLAN_HIF_OVERRIDE);
  OS_ThreadSuspendScheduler();
  c.status_first = cpu_register(POWER_SYS3_STATUS);
  c.hif_first = hif;
  if (!wlan_alive()) {
    hif = c.hif_first | 0x4U;
    while (!wlan_alive() && c.waited_ms < 300) {
      delayMicroseconds(1000);
      ++c.waited_ms;
    }
  }
  if (wlan_alive() && c.words != nullptr) {
    uint32_t *out = c.words;
    for (const auto &region : REGIONS) {
      const volatile uint32_t *in = app_address(region.start);
      for (uint32_t i = 0; i < region.words; ++i)
        *out++ = in[i];
    }
    c.copied = true;
  }
  c.status_last = cpu_register(POWER_SYS3_STATUS);
  c.hif_last = hif;
  OS_ThreadResumeScheduler();
}

XR806_DIAG_FLASH inline void print_capture(Capture &c) {
  static const char *const TAG = "xr806.wlan_diag";
  ESP_LOGE(TAG,
           "WLAN core state: power status %08" PRIX32 " wake control %08" PRIX32 ", after %" PRIu32
           " ms with the wake request set: %08" PRIX32 " / %08" PRIX32 ", memory %s",
           c.status_first, c.hif_first, c.waited_ms, c.status_last, c.hif_last, c.copied ? "copied" : "not readable");
  dump_power_control("prcm");
  if (c.copied) {
    const uint32_t *w = c.words;
    for (const auto &region : REGIONS) {
      for (uint32_t i = 0; i < region.words; i += 8, w += 8) {
        if ((w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]) == 0)
          continue;
        ESP_LOGE(TAG,
                 "%s %08" PRIX32 ": %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32
                 " %08" PRIX32 " %08" PRIX32,
                 region.label, region.start + 4 * i, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        delay(3);
      }
    }
  }
  free(c.words);
  c.words = nullptr;
}

struct Cycle {
  uint32_t count{0};
  uint32_t start_ms{0};
  int rx{0}, tx{0};
  Snapshot before;
};

inline Cycle &cycle() {
  static Cycle c;
  return c;
}

// Right before the PM call: read only, print after the wake so no UART traffic delays standby.
XR806_DIAG_FLASH inline void before_standby() {
  auto &c = cycle();
  ++c.count;
  c.rx = skb_rx_cnt;
  c.tx = skb_tx_cnt;
  take(c.before);
  c.start_ms = millis();
}

XR806_DIAG_FLASH inline void after_wake(int result, uint32_t event) {
  static const char *const TAG = "xr806.wlan_diag";
  auto &c = cycle();
  const uint32_t slept = millis() - c.start_ms;
  // Read first: the driver's recovery task clears the flag again within a moment.
  const bool failed = wlan_driver_failed();
  Capture hung;
  if (failed)
    capture(hung);
  Snapshot after;
  if (!failed)  // a request to a failed driver only runs into its timeout
    take(after);
  // Driver state behind a refused suspend (xr_wlan_suspend): its stay_awake timer and bh_error.
  auto *timer = reinterpret_cast<OS_Timer_t *>(xr_drv_common + 0x1C0);
  ESP_LOGI(TAG,
           "#%" PRIu32 " PM return %d event %08" PRIX32 " slept %" PRIu32
           " ms, driver rx +%d tx +%d, stay_awake %d bh_error %d",
           c.count, result, event, slept, skb_rx_cnt - c.rx, skb_tx_cnt - c.tx, OS_TimerIsActive(timer) ? 1 : 0,
           failed ? 1 : 0);
  // Heap: bytes in use, free bytes inside the claimed part (in pieces) and the part never claimed. "in use" that
  // keeps growing is a leak; a small "never claimed" with a large "free" is fragmentation.
  uint8_t *heap_start, *heap_end, *heap_current;
  malloc_mutex_lock();
  heap_get_space(&heap_start, &heap_end, &heap_current);
  const struct mallinfo heap = mallinfo();
  malloc_mutex_unlock();
  ESP_LOGI(TAG, "heap: in use %u, free in pieces %u, never claimed %u of %u", static_cast<unsigned>(heap.uordblks),
           static_cast<unsigned>(heap.fordblks), static_cast<unsigned>(heap_end - heap_current),
           static_cast<unsigned>(heap_end - heap_start));
  print("before", c.before);
  print("after ", after);
  if (c.before.ok && after.ok) {
    // The core's clock must advance as far as the CPU's; less means the core slept past its own timers.
    const uint32_t wlan_ms = ((after.counter + after.offset) - (c.before.counter + c.before.offset)) / 1000;
    if (wlan_ms + 2000 < slept) {
      ESP_LOGE(TAG, "WLAN clock advanced only %" PRIu32 " of %" PRIu32 " ms: the core overslept", wlan_ms, slept);
      // The core runs again, so its memory can be read: what it queued for the CPU and where it stood.
      capture(hung);
      print_capture(hung);
    } else if (c.count == 2) {
      dump_power_control("prcm-ok");  // reference values from a good cycle
    }
  }
  if (failed)
    print_capture(hung);
}

}  // namespace esphome::xr806::wlan_sleep_diag

#endif  // USE_XR806_WLAN_SLEEP_DIAG
