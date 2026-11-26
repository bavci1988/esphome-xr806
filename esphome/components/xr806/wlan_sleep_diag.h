#pragma once

// Diagnostics for WLAN core hangs during standby (xr806: diagnose_wlan_sleep: true). Before every standby and after
// every wake, the WLAN core's timer list, clock and event words are read through the firmware (MIB 9) and printed
// together with the wake result, the time asleep and the driver's frame counters, so one long run holds everything.

#include "esphome/core/defines.h"

#ifdef USE_XR806_WLAN_SLEEP_DIAG

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "wlan_mib.h"
#include "wlan_power.h"

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

using wlan_power::cpu_register;
using wlan_power::POWER_CONTROL_BASE;
using wlan_power::POWER_CONTROL_SIZE;
using wlan_power::POWER_SYS3_STATUS;
using wlan_power::POWER_WLAN_HIF_OVERRIDE;
using wlan_power::wlan_alive;
static constexpr uint32_t POWER_WORDS = POWER_CONTROL_SIZE / 4;

XR806_DIAG_FLASH inline void read_power_control(uint32_t *out) {
  for (uint32_t i = 0; i < POWER_WORDS; ++i)
    out[i] = cpu_register(POWER_CONTROL_BASE + 4 * i);
}

// Prints the power control block in rows of eight words: from words, or read now when words is null. With
// reference, only the rows that differ from it.
XR806_DIAG_FLASH inline void print_power_control(const char *label, const uint32_t *words,
                                                 const uint32_t *reference = nullptr) {
  static const char *const TAG = "xr806.wlan_diag";
  for (uint32_t row = 0; row < POWER_WORDS; row += 8) {
    uint32_t live[8];
    const uint32_t *w = words != nullptr ? words + row : live;
    if (words == nullptr) {
      for (uint8_t i = 0; i < 8; ++i)
        live[i] = cpu_register(POWER_CONTROL_BASE + 4 * (row + i));
    }
    if (reference != nullptr && memcmp(w, reference + row, 8 * sizeof(uint32_t)) == 0)
      continue;
    const uint32_t address = POWER_CONTROL_BASE + 4 * row;
    ESP_LOGE(TAG,
             "%s %03" PRIX32 ": %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %08" PRIX32
             " %08" PRIX32 " %08" PRIX32,
             label, address - POWER_CONTROL_BASE, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    delay(3);
  }
}

XR806_DIAG_FLASH inline void dump_power_control(const char *label) { print_power_control(label, nullptr); }

// Power words that a hang changes: LDO and power switch status (0x004, bit 14: switch 5), power control config
// (0x074, bits 6-7: when the 32 kHz calibration starts), the WLAN domain's control and status (0x090, 0x094), the
// block from DCDC parameters to the WLAN override (0x200-0x22C), the 32 kHz clock sources (0x23C), the wake-up
// source clocks (0x240) and the RC oscillator calibration control (0x260, 0x264).
static constexpr uint16_t POWER_LINE_OFFSETS[] = {0x004, 0x074, 0x090, 0x094, 0x200, 0x204, 0x208, 0x20C, 0x210, 0x214,
                                                  0x218, 0x21C, 0x220, 0x224, 0x228, 0x22C, 0x23C, 0x240, 0x260, 0x264};
static constexpr size_t POWER_LINE_WORDS = sizeof(POWER_LINE_OFFSETS) / sizeof(POWER_LINE_OFFSETS[0]);

XR806_DIAG_FLASH inline void read_power_line(uint32_t *out) {
  for (size_t i = 0; i < POWER_LINE_WORDS; ++i)
    out[i] = cpu_register(POWER_CONTROL_BASE + POWER_LINE_OFFSETS[i]);
}

// One line, only when the words differ from the last line printed with the same label slot.
XR806_DIAG_FLASH inline void print_power_line(const char *label, const uint32_t *words, uint32_t *last) {
  static const char *const TAG = "xr806.wlan_diag";
  if (memcmp(words, last, POWER_LINE_WORDS * sizeof(uint32_t)) == 0)
    return;
  memcpy(last, words, POWER_LINE_WORDS * sizeof(uint32_t));
  char text[POWER_LINE_WORDS * 9 + 1];
  size_t pos = 0;
  for (size_t i = 0; i < POWER_LINE_WORDS; ++i)
    pos += snprintf(text + pos, sizeof(text) - pos, " %08" PRIX32, words[i]);
  ESP_LOGI(TAG, "power %s (004 074 090 094 200..22C 23C 240 260 264, printed on change):%s", label, text);
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
static constexpr uint32_t MEMORY_WORDS = 40 + 16 + 16 + 32 + 72 + 1024;
// Registers of the WLAN domain's own power control (its sleep-on-WFI register is 0x0AA800A0) and timer, read
// one by one: only those the WLAN firmware itself reads. 0x0AA800C4 and up are hardware spinlocks, where a read
// takes the lock, so a plain dump of the block leaves the core spinning. The CPU reaches them only through the
// domain's bus, so a hung core is read directly only while its power switch is on.
static constexpr uint32_t WLAN_SIDE[] = {0x0AA80018, 0x0AA80040, 0x0AA80044, 0x0AA80048, 0x0AA80080,
                                         0x0AA80084, 0x0AA80090, 0x0AA80094, 0x0AA80098, 0x0AA8009C,
                                         0x0AA800A0, 0x0AC00004, 0x0AC00014, 0x0AC0001C};
static constexpr uint32_t WLAN_SIDE_WORDS = sizeof(WLAN_SIDE) / sizeof(WLAN_SIDE[0]);
// The buffer also holds the power control block as found, before the wake request or the reset changes it, and
// the WLAN side as found and after the wake request.
static constexpr uint32_t POWER_OFFSET = MEMORY_WORDS;
static constexpr uint32_t SIDE_FOUND_OFFSET = POWER_OFFSET + POWER_WORDS;
static constexpr uint32_t SIDE_WOKEN_OFFSET = SIDE_FOUND_OFFSET + WLAN_SIDE_WORDS;
static constexpr uint32_t CAPTURE_WORDS = SIDE_WOKEN_OFFSET + WLAN_SIDE_WORDS;

// Power switch 5 (bit 14 of 0x004) is on while the WLAN domain is powered, in the hang too.
inline bool wlan_switch_on() { return (cpu_register(POWER_CONTROL_BASE + 0x004) & (1U << 14)) != 0; }

XR806_DIAG_FLASH inline void read_wlan_side(uint32_t *out) {
  for (uint32_t i = 0; i < WLAN_SIDE_WORDS; ++i)
    out[i] = *app_address(WLAN_SIDE[i]);
}

// One line: address=value for each WLAN-side register; with reference, only the ones that differ from it.
XR806_DIAG_FLASH inline void print_wlan_side(const char *label, const uint32_t *words,
                                             const uint32_t *reference = nullptr) {
  static const char *const TAG = "xr806.wlan_diag";
  char text[WLAN_SIDE_WORDS * 18 + 1];
  size_t pos = 0;
  for (uint32_t i = 0; i < WLAN_SIDE_WORDS; ++i) {
    if (reference != nullptr && words[i] == reference[i])
      continue;
    pos += snprintf(text + pos, sizeof(text) - pos, " %05" PRIX32 "=%08" PRIX32, WLAN_SIDE[i] & 0xFFFFF, words[i]);
  }
  ESP_LOGE(TAG, "WLAN side %s:%s", label, pos == 0 ? LOG_STR_LITERAL(" unchanged") : text);
}

struct Capture {
  uint32_t status_first{0}, hif_first{0}, status_last{0}, hif_last{0};
  uint32_t waited_ms{0};
  bool copied{false};
  bool domain_reset{false};
  bool side_found{false};  // the WLAN side was read as found
  bool side_woken{false};  // and again after the wake request
  uint32_t *words{nullptr};
};

// Runs first after a wake that left the driver failed, with the scheduler stopped, so that the driver's recovery
// task cannot reload the core's firmware before its memory is copied. The memory can only be read while the
// core's power domain reports alive; reading it otherwise blocks the CPU. When it is not alive, the wake request
// is set once more and the domain gets 300 ms; after that the domain is reset with the WLAN processor held.
XR806_DIAG_FLASH inline void capture(Capture &c) {
  c.words = static_cast<uint32_t *>(malloc(CAPTURE_WORDS * sizeof(uint32_t)));
  volatile uint32_t &hif = cpu_register(POWER_WLAN_HIF_OVERRIDE);
  OS_ThreadSuspendScheduler();
  if (c.words != nullptr)
    read_power_control(c.words + POWER_OFFSET);
  c.status_first = cpu_register(POWER_SYS3_STATUS);
  c.hif_first = hif;
  if (!wlan_alive() && wlan_switch_on() && c.words != nullptr) {
    read_wlan_side(c.words + SIDE_FOUND_OFFSET);
    c.side_found = true;
  }
  if (!wlan_alive()) {
    hif = c.hif_first | wlan_power::HIF_WAKE;
    wlan_power::wait_alive(300, &c.waited_ms);
    if (!wlan_alive() && c.side_found && wlan_switch_on()) {
      read_wlan_side(c.words + SIDE_WOKEN_OFFSET);
      c.side_woken = true;
    }
  }
  if (!wlan_alive()) {
    // The domain is down and the wake request does nothing.
    c.domain_reset = true;
    wlan_power::reset_domain(true);
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
  if (c.domain_reset)
    hif = hif & ~wlan_power::HIF_HOLD_CPU;
  OS_ThreadResumeScheduler();
}

XR806_DIAG_FLASH inline void print_capture(Capture &c) {
  static const char *const TAG = "xr806.wlan_diag";
  ESP_LOGE(TAG,
           "WLAN core state: power status %08" PRIX32 " wake control %08" PRIX32 ", after %" PRIu32
           " ms with the wake request set%s: %08" PRIX32 " / %08" PRIX32 ", memory %s",
           c.status_first, c.hif_first, c.waited_ms,
           c.domain_reset ? LOG_STR_LITERAL(" and a reset of the domain") : LOG_STR_LITERAL(""), c.status_last,
           c.hif_last, c.copied ? LOG_STR_LITERAL("copied") : LOG_STR_LITERAL("not readable"));
  if (c.words != nullptr) {
    // As found, then what the wake request and the reset changed.
    print_power_control("prcm", c.words + POWER_OFFSET);
    print_power_control("prcm-now", nullptr, c.words + POWER_OFFSET);
  }
  if (c.side_found)
    print_wlan_side("found", c.words + SIDE_FOUND_OFFSET);
  if (c.side_woken)
    print_wlan_side("after wake request", c.words + SIDE_WOKEN_OFFSET, c.words + SIDE_FOUND_OFFSET);
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
  uint32_t power_before[POWER_LINE_WORDS]{};
  uint32_t monitor_first{0};
  uint32_t monitor_awake{0};            // as found before standby: what the time awake left
  uint32_t monitor_printed_awake{~0U};  // last printed values, so that unchanged ones are not repeated
  uint32_t monitor_printed_standby{~0U};
  uint32_t printed_before[POWER_LINE_WORDS]{};
  uint32_t printed_after[POWER_LINE_WORDS]{};
};

inline Cycle &cycle() {
  static Cycle c;
  return c;
}

// Right before the PM call: read only, print after the wake so no UART traffic delays standby.
// The 32 kHz clock of the WLAN core comes from the RC oscillator calibration unit (no 32 kHz crystal on this
// module). Its monitor flags a glitch or an over-long level on that clock; it is cleared before every standby.
static constexpr uint32_t RC_CALIBRATION_BASE = 0x40046000;
static constexpr uint32_t RC_CALIBRATION_DCXO_COUNT = RC_CALIBRATION_BASE + 0x08;
static constexpr uint32_t RC_CALIBRATION_MONITOR = RC_CALIBRATION_BASE + 0x0C;
static constexpr uint32_t MONITOR_ENABLE = 1U << 4;

// Experiment: calibrate the 32 kHz clock more often. The SDK sets a base interval of 640 slow clock cycles with
// phase 3 at 24 times that, about every 480 ms once settled; here 320 cycles and 10 times, about every 100 ms.
// Stop, configure and start as HAL_RcoscCali_Config/Start do.
static constexpr uint32_t CALIBRATION_CTRL0 = POWER_CONTROL_BASE + 0x260;  // bit 29 enable, bit 16 timer, 0-12 time
static constexpr uint32_t CALIBRATION_CTRL1 = POWER_CONTROL_BASE + 0x264;  // bits 8-10 phase 3, 4-6 phase 2, 0 mode
static constexpr uint32_t CALIBRATION_ENABLE = (1U << 29) | (1U << 16);
static constexpr uint32_t CALIBRATION_TIME_MASK = 0x1FFFU;
static constexpr uint32_t CALIBRATION_PHASE3_MASK = 7U << 8;
static constexpr uint32_t CALIBRATION_TIME = 320;
static constexpr uint32_t CALIBRATION_PHASE3_10 = 0U << 8;

XR806_DIAG_FLASH inline void calibrate_more_often() {
  static const char *const TAG = "xr806.wlan_diag";
  volatile uint32_t &ctrl0 = cpu_register(CALIBRATION_CTRL0);
  volatile uint32_t &ctrl1 = cpu_register(CALIBRATION_CTRL1);
  const uint32_t before0 = ctrl0, before1 = ctrl1;
  ctrl0 = ctrl0 & ~CALIBRATION_ENABLE;
  ctrl1 = (ctrl1 & ~CALIBRATION_PHASE3_MASK) | CALIBRATION_PHASE3_10;
  ctrl0 = (ctrl0 & ~CALIBRATION_TIME_MASK) | CALIBRATION_TIME;
  ctrl0 = ctrl0 | CALIBRATION_ENABLE;
  delayMicroseconds(200);
  ESP_LOGW(TAG, "Experiment: 32 kHz calibration more often: %08" PRIX32 " %08" PRIX32 " -> %08" PRIX32 " %08" PRIX32,
           before0, before1, static_cast<uint32_t>(ctrl0), static_cast<uint32_t>(ctrl1));
}
static constexpr uint32_t MONITOR_CLEAR = (1U << 5) | (1U << 2);

XR806_DIAG_FLASH inline void print_clock_monitor(const char *label, uint32_t monitor) {
  static const char *const TAG = "xr806.wlan_diag";
  ESP_LOGW(TAG,
           "32 kHz clock monitor %s: %08" PRIX32 ", long level %u (longest %" PRIu32 "), glitch %u (widest %" PRIu32
           "), calibration count %08" PRIX32,
           label, monitor, (monitor >> 6) & 1U, monitor >> 16, (monitor >> 7) & 1U, (monitor >> 8) & 0x7FU,
           cpu_register(RC_CALIBRATION_DCXO_COUNT));
}

XR806_DIAG_FLASH inline void before_standby() {
  auto &c = cycle();
  ++c.count;
  c.rx = skb_rx_cnt;
  c.tx = skb_tx_cnt;
  take(c.before);
  read_power_line(c.power_before);
  volatile uint32_t &monitor = cpu_register(RC_CALIBRATION_MONITOR);
  if (c.count == 1) {
    c.monitor_first = monitor;
    calibrate_more_often();
    delay(20);  // let the line reach the UART before standby
  }
  c.monitor_awake = monitor;
  monitor = MONITOR_ENABLE | MONITOR_CLEAR;
  monitor = MONITOR_ENABLE;
  c.start_ms = millis();
}

XR806_DIAG_FLASH inline void after_wake(int result, uint32_t event) {
  static const char *const TAG = "xr806.wlan_diag";
  auto &c = cycle();
  const uint32_t slept = millis() - c.start_ms;
  uint32_t power_after[POWER_LINE_WORDS];
  read_power_line(power_after);
  const uint32_t monitor = cpu_register(RC_CALIBRATION_MONITOR);
  // Read first: the driver's recovery task clears the flag again within a moment.
  const bool failed = wlan_driver_failed();
  Capture hung;
  if (failed) {
    if (!wlan_alive() && wlan_switch_on()) {
      // Experimental: this read of the hung domain may block the CPU until the watchdog restarts it.
      ESP_LOGW(TAG, "Reading the hung WLAN core's own power registers; a restart right after this line means the "
                    "read blocked");
      delay(20);  // let the line reach the UART
    }
    capture(hung);
  }
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
  print_power_line("before", c.power_before, c.printed_before);
  print_power_line("after ", power_after, c.printed_after);
  if (c.count == 1)
    print_clock_monitor("at boot", c.monitor_first);
  // Printed on change, and always after a hang; "while awake" is what the time before the standby left.
  if (c.count > 1 && c.monitor_awake != c.monitor_printed_awake) {
    c.monitor_printed_awake = c.monitor_awake;
    print_clock_monitor("while awake", c.monitor_awake);
  }
  if (monitor != c.monitor_printed_standby || failed) {
    c.monitor_printed_standby = monitor;
    print_clock_monitor("during standby", monitor);
  }
  if (c.before.ok && after.ok) {
    // The core's clock must advance as far as the CPU's; less means the core slept past its own timers.
    const uint32_t wlan_ms = ((after.counter + after.offset) - (c.before.counter + c.before.offset)) / 1000;
    if (wlan_ms + 2000 < slept) {
      ESP_LOGE(TAG, "WLAN clock advanced only %" PRIu32 " of %" PRIu32 " ms: the core overslept", wlan_ms, slept);
      print_clock_monitor("during this standby", monitor);
      // The core runs again, so its memory can be read: what it queued for the CPU and where it stood.
      capture(hung);
      print_capture(hung);
    } else if (c.count == 2) {
      dump_power_control("prcm-ok");  // reference values from a good cycle
      // The same WLAN-side registers through the running firmware, which is safe while the core answers.
      uint32_t *side = static_cast<uint32_t *>(malloc(WLAN_SIDE_WORDS * sizeof(uint32_t)));
      if (side != nullptr) {
        uint32_t *out = side;
        bool ok = true;
        for (uint32_t i = 0; i < WLAN_SIDE_WORDS && ok; ++i)
          ok = wlan_read_words(WLAN_SIDE[i], out + i, 1) == 0;
        if (ok)
          print_wlan_side("ok", side);
        free(side);
      }
    }
  }
  if (failed)
    print_capture(hung);
}

}  // namespace esphome::xr806::wlan_sleep_diag

#endif  // USE_XR806_WLAN_SLEEP_DIAG
