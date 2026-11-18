#pragma once

// Diagnostics for WLAN core hangs during standby (xr806: diagnose_wlan_sleep: true). Before every standby and after
// every wake, the WLAN core's timer list, clock and event words are read through the firmware (MIB 9) and printed
// together with the wake result, the time asleep and the driver's frame counters, so one long run holds everything.

#include "esphome/core/defines.h"

#ifdef USE_XR806_WLAN_SLEEP_DIAG

#include <cinttypes>
#include <cstdint>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "wlan_mib.h"

extern "C" {
#include "kernel/os/os_timer.h"
extern volatile int skb_rx_cnt;  // libxrwireless: frames handed up by the WLAN driver
extern volatile int skb_tx_cnt;
}

namespace esphome::xr806::wlan_sleep_diag {

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

inline void take(Snapshot &s) {
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
inline void print(const char *label, const Snapshot &s) {
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
inline void before_standby() {
  auto &c = cycle();
  ++c.count;
  c.rx = skb_rx_cnt;
  c.tx = skb_tx_cnt;
  take(c.before);
  c.start_ms = millis();
}

inline void after_wake(int result, uint32_t event) {
  static const char *const TAG = "xr806.wlan_diag";
  auto &c = cycle();
  const uint32_t slept = millis() - c.start_ms;
  Snapshot after;
  take(after);
  // Driver state behind a refused suspend (xr_wlan_suspend): its stay_awake timer and bh_error.
  auto *timer = reinterpret_cast<OS_Timer_t *>(xr_drv_common + 0x1C0);
  ESP_LOGI(TAG,
           "#%" PRIu32 " PM return %d event %08" PRIX32 " slept %" PRIu32
           " ms, driver rx +%d tx +%d, stay_awake %d bh_error %d",
           c.count, result, event, slept, skb_rx_cnt - c.rx, skb_tx_cnt - c.tx, OS_TimerIsActive(timer) ? 1 : 0,
           wlan_driver_failed() ? 1 : 0);
  print("before", c.before);
  print("after ", after);
}

}  // namespace esphome::xr806::wlan_sleep_diag

#endif  // USE_XR806_WLAN_SLEEP_DIAG
