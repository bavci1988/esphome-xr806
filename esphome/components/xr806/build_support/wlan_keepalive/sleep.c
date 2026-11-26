/*
 * Deep sleep of the XR806 WLAN core, rebuilt in C from the vendor firmware (the handler is the same code in 02.132
 * and 02.138) and the ROM, so that it can be read, instrumented and fixed.
 *
 * How the core sleeps: when nothing keeps it awake any more, the ROM raises event 4. Its handler (sleep_event here)
 * looks at the time until the next ROM timer and, if that is long enough and the radio is not busy, programs the
 * wake compare of the low-frequency counter and tells the power controller to go. The CPU then loses power. A wake
 * (the compare, or a request of the host) restarts the CPU in the firmware's reset code, which sees its marker at
 * 0xFFF00000 and continues in the ROM's warm start (0x54E6); the handler below is never returned to.
 *
 * Behaviour is the vendor's, with two additions:
 * - a record of the last entry (sleep_record), which the host reads after the core stopped answering: with the ROM's
 *   wake counter it tells whether the core never restarted after "go" or stopped during the warm start;
 * - the wake compare is never written closer than SLEEP_MIN_TICKS ahead of the counter. The compare is a match of a
 *   24-bit counter, so a value at or behind the counter would only match after a wrap-around (512 s).
 */

#include <stdint.h>

#include "rom.h"

#ifndef PATCH_STATE
#error "PATCH_STATE: address of the firmware's sleep state word (02.132: 0xFFF0AFD0, 02.138: 0xFFF0AF00)"
#endif
#ifndef PATCH_BUSY_CHECK
#error "PATCH_BUSY_CHECK: Thumb address of the firmware's busy check (02.132: 0xFFF0959D, 02.138: 0xFFF095E5)"
#endif

#define REG(address) (*(volatile uint32_t *) (address))
#define BYTE(address) (*(volatile uint8_t *) (address))
#define HALF(address) (*(volatile uint16_t *) (address))

#define EVENTS REG(0xFFF01C0CU) /* pending events of the ROM's scheduler */
#define AWAKE REG(0xFFF01C10U)  /* reasons to stay awake; event 4 is raised when this becomes 0 */
#define AWAKE_TIMER 0x10U       /* a timer is pending */
#define SLEEP_CORRECTION_A HALF(0xFFF01C0CU + 0x36)
#define SLEEP_CORRECTION_B HALF(0xFFF01C0CU + 0x38)
#define SLEEPING_FLAG BYTE(0xFFF0562CU)
#define RETRY_TIMER ((void *) 0xFFF05610U)
#define WAKE_COUNT REG(0xFFF055F8U + 0x2C) /* counted by the ROM's warm start */
#define SLEEP_TOTAL_US REG(0xFFF055F8U + 0x30)

/* Clock data: +0x14 period of the low-frequency clock in 1/2560 us (from the host), +0x18 lead time for the wake in
   us, +0x1C flag for a clock 64 times faster, +0x20 longest sleep in us. */
#define CLOCK_DATA 0xFFF05274U
#define LF_PERIOD REG(CLOCK_DATA + 0x14)
#define WAKE_LEAD_US REG(CLOCK_DATA + 0x18)
#define LF_FAST BYTE(CLOCK_DATA + 0x1C)
#define SLEEP_MAX_US REG(CLOCK_DATA + 0x20)

#define TIMER_COUNTER REG(0x0AC00004U) /* us */
#define IRQ_PENDING REG(0x0AC80084U)
#define IRQ_ENABLE REG(0x0AC80058U)
#define IRQ_STATUS REG(0x0AC80098U)
#define SLEEP_ALLOW REG(0x0AC80034U)
#define SLEEP_GO REG(0x0AC80048U)
#define LF_COUNTER REG(0x0AC8006CU) /* 24 bits */
#define WAKE_COMPARE REG(0x0AC80070U)
#define POWER_CONTROL REG(0x0AA80018U)

#define SLEEP_MIN_US 24080U
#define SLEEP_MIN_TICKS 64U /* about 2 ms */
#define LF_COUNTER_MASK 0x00FFFFFFU

/* Read by the host through the WLAN RAM window after a hang. */
struct sleep_record {
  uint32_t entries;        /* times "go" was written */
  uint32_t wakes_at_entry; /* the ROM's wake counter then; still the same after a hang: the core never restarted */
  uint32_t sleep_us;       /* time to the next timer */
  uint32_t ticks;          /* the same after lead time and conversion */
  uint32_t counter;        /* low-frequency counter at entry */
  uint32_t compare;        /* wake compare written */
  uint32_t timer_us;       /* microsecond counter at entry */
  uint32_t lf_period;
  uint32_t irq_status;
  uint32_t irq_pending;
  uint32_t short_sleeps; /* entries where the compare had to be moved ahead */
};

struct sleep_record sleep_record = {0};

/* The vendor's 0x0900B5AC and the ROM's 0x52D6 in one: never returns. */
static void sleep_enter(uint32_t us) __attribute__((noreturn, noinline));
static void sleep_enter(uint32_t us) {
  SLEEP_CORRECTION_A = 0;
  SLEEP_CORRECTION_B = 0;
  uint32_t period = LF_PERIOD;
  us += (uint32_t) rom_udiv64((uint64_t) period << 2, 0xA00); /* four periods of the clock */
  us -= WAKE_LEAD_US;
  if (us > SLEEP_MAX_US)
    us = SLEEP_MAX_US;
  SLEEP_TOTAL_US += us;
  uint8_t fast = LF_FAST;
  uint32_t ticks = (uint32_t) rom_udiv64(rom_umul64(us, fast ? 0x1E000U : 0xA00U), period);
  if (fast)
    ticks >>= 6;
  uint32_t counter = LF_COUNTER;
  uint32_t compare = ((counter + ticks) & LF_COUNTER_MASK) - SLEEP_CORRECTION_A - SLEEP_CORRECTION_B - 4;
  if (((compare - counter) & LF_COUNTER_MASK) - SLEEP_MIN_TICKS >= (LF_COUNTER_MASK >> 1)) {
    compare = (counter + SLEEP_MIN_TICKS) & LF_COUNTER_MASK;
    sleep_record.short_sleeps++;
  }
  sleep_record.entries++;
  sleep_record.wakes_at_entry = WAKE_COUNT;
  sleep_record.ticks = ticks;
  sleep_record.counter = counter;
  sleep_record.compare = compare;
  sleep_record.timer_us = TIMER_COUNTER;
  sleep_record.lf_period = period;
  sleep_record.irq_status = IRQ_STATUS;
  sleep_record.irq_pending = IRQ_PENDING;

  WAKE_COMPARE = compare;
  IRQ_ENABLE = 0x90; /* only the wake interrupts */
  POWER_CONTROL &= ~0x40000U;
  SLEEP_ALLOW = 0;
  SLEEP_GO = 1;
  for (;;) {
  }
}

/* Handler of event 4 (the vendor's 0x0900B6DC in 02.132, 0x09009C00 in 02.138). */
void sleep_event(void) {
  uint8_t *beacon = (uint8_t *) 0xFFF08038U; /* a timer the firmware pauses over the sleep: +0xF state, +0x10 node */
  void *beacon_timer = beacon + 0x10;
  uint8_t *other = (uint8_t *) 0xFFF021CCU; /* a second one: +3 bits 1..2 in use, node at +0x18 */
  void *other_timer = other + 0x18;
  uint32_t remaining = 200000;
  if (beacon[0x0F] == 1) {
    remaining = *(uint32_t *) (beacon + 0x18) - rom_now_us();
    beacon[0x0F] = 2;
    rom_timer_stop(beacon_timer);
  }
  if ((other[3] >> 1) & 3)
    rom_timer_stop(other_timer);

  uint32_t sleep_us = rom_next_timer_us();
  if (sleep_us >= SLEEP_MIN_US) {
    uint32_t delay;
    uint32_t now = TIMER_COUNTER;
    if (now < 2000 && !(IRQ_PENDING & 1) && HALF(PATCH_STATE) != 2) {
      delay = 2010 - now; /* shortly after a start: not yet */
    } else if (ROM_FN(PATCH_BUSY_CHECK, int (*)(void))()) {
      delay = 20000;
    } else {
      uint32_t flags = rom_irq_lock();
      if (AWAKE == 0)
        SLEEP_ALLOW = 0;
      rom_irq_unlock(flags);
      flags = rom_irq_lock();
      if (EVENTS <= 4 && AWAKE == 0) {
        SLEEPING_FLAG |= 1;
        sleep_record.sleep_us = sleep_us;
        if (rom_sleep_prepare()) {
          rom_sleep_save_time();
          sleep_enter(sleep_us);
        }
        SLEEPING_FLAG &= ~1;
      }
      SLEEP_ALLOW = 1;
      rom_irq_unlock(flags);
      if (AWAKE != 0)
        goto resume;
      delay = 1000;
    }
    rom_timer_start_exact(RETRY_TIMER, delay);
  }
  rom_set_bits(&AWAKE, AWAKE_TIMER);
resume:
  if (beacon[0x0F] == 2) {
    beacon[0x0F] = 1;
    rom_timer_start_exact(beacon_timer, remaining);
  }
  if ((other[3] >> 1) & 3)
    rom_timer_start_exact(other_timer, *(uint32_t *) (other + 0x80) + *(uint32_t *) (other + 0x14) - rom_now_us());
}
