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
 * Behaviour is the vendor's, with these additions:
 * - a record of the last entry (sleep_record), which the host reads after the core stopped answering, and a count of
 *   the warm starts that reached the firmware's reset code (sleep_warm_start). The record replaces what could be
 *   read from the vendor code's stack frames after a hang;
 * - two checks of the arithmetic, which the real values never fail: a lead time longer than the sleep no longer
 *   wraps around to the longest sleep (20 s), and the wake compare is at least SLEEP_MIN_TICKS ahead of the counter;
 * - timers are never started with less than 4 us (sleep_timer_start, installed in the firmware's function table):
 *   the ROM writes such a delay straight into the hardware timer, while its own timer processing never programs
 *   less than 4 us. The handler below restarts its paused timers with "expiry - now", which is 0 or negative when
 *   the timer became due in between.
 * - the idle hook (sleep_idle, for the function table instead of the ROM's 0x19223) notes the state before every
 *   wait for an interrupt, and tells the power controller to stay awake while a reason to stay awake exists. See
 *   sleep_idle for the "overslept" failure this is for.
 *
 * In the hangs recorded so far the wake compare was about 300 ms ahead and neither it nor the host's wake request
 * restarted the core, so none of these checks is expected to end the hangs; the record is there to find the cause.
 */

#include <stdint.h>

#include "rom.h"

#ifndef PATCH_STATE
#error "PATCH_STATE: address of the firmware's sleep state word (02.132: 0xFFF0AFD0, 02.138: 0xFFF0AF00)"
#endif
#ifndef PATCH_BUSY_CHECK
#error "PATCH_BUSY_CHECK: Thumb address of the firmware's busy check (02.132: 0xFFF0959D, 02.138: 0xFFF095E5)"
#endif
#ifndef PATCH_TIMER_START
#error "PATCH_TIMER_START: Thumb address of the firmware's timer start (02.132: 0xFFF09041, 02.138: 0xFFF09089)"
#endif

#ifndef PATCH_SET_TIMING
#error "PATCH_SET_TIMING: Thumb address of the firmware's wake timing function (02.132: 0x0900B53D, 02.138: 0x0900AE41)"
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

/* Clock data: +0x14 period of the low-frequency clock in 1/2560 us (rewritten before every sleep by the ROM, from the
   hardware calibration or, in one firmware state, the host's value), +0x18 lead time for the wake in us, +0x1C flag
   for a clock 64 times faster (always 0), +0x20 longest sleep in us (20 s). */
#define CLOCK_DATA 0xFFF05274U
#define LF_PERIOD REG(CLOCK_DATA + 0x14)
#define WAKE_LEAD_US REG(CLOCK_DATA + 0x18)
#define LF_FAST BYTE(CLOCK_DATA + 0x1C)
#define SLEEP_MAX_US REG(CLOCK_DATA + 0x20)

#define TIMER_COUNTER REG(0x0AC00004U)  /* us */
#define LF_CALIBRATION REG(0x0AC80084U) /* bit 0: valid; the measured period above bit 13 */
#define IRQ_ENABLE REG(0x0AC80058U)
#define IRQ_STATUS REG(0x0AC80098U)
#define STAY_AWAKE REG(0x0AC80034U) /* 1: a wake request is being served */
#define SLEEP_GO REG(0x0AC80048U)
#define LF_COUNTER REG(0x0AC8006CU) /* 24 bits */
#define WAKE_COMPARE REG(0x0AC80070U)
#define POWER_CONTROL REG(0x0AA80018U)

#define SLEEP_MIN_US 24080U
#define SLEEP_MIN_TICKS 64U /* about 2 ms */
#define LF_COUNTER_MASK 0x00FFFFFFU

#define SLEEP_RECORD_MAGIC 0x31504C53U /* "SLP1" */

/* Read by the host through the WLAN RAM window after a hang. */
struct sleep_record {
  uint32_t magic;
  uint32_t entries;        /* times "go" was written */
  uint32_t warm_starts;    /* times the reset code ran after a sleep; one less than entries: the CPU never restarted */
  uint32_t wakes_at_entry; /* the ROM's wake counter at the entry; it counts a little later in the warm start */
  uint32_t sleep_us;       /* time to the next timer */
  uint32_t slept_us;       /* the same after lead time and limit */
  uint32_t lead_us;
  uint32_t ticks;
  uint32_t counter; /* low-frequency counter at entry */
  uint32_t compare; /* wake compare written */
  uint32_t lf_period;
  uint32_t lf_calibration;
  uint32_t irq_status; /* bit 1: a wake request was pending at the entry */
  uint32_t power_control;
  uint32_t irq_flags;    /* interrupt mask the handler saved */
  uint32_t remaining;    /* of the paused beacon timer */
  uint16_t short_sleeps; /* entries where the compare had to be moved ahead */
  uint16_t lead_wraps;   /* entries with a lead time longer than the sleep */
  /* the state before the last wait for an interrupt in the idle loop */
  uint32_t idle_count;
  uint32_t idle_awake;
  uint32_t idle_timer_us;
  uint32_t idle_next_us;    /* time to the next timer */
  uint32_t idle_stay_awake; /* as found, before sleep_idle set it */
  uint32_t idle_power_control;
};

volatile struct sleep_record sleep_record;

/* Called once when the firmware starts, instead of the vendor keepalive's initialisation (0x0900076C in 02.132):
   the data of both modules is not part of the firmware image. */
void keepalive_init(void);
void module_init(void) {
  keepalive_init();
  volatile uint32_t *word = (volatile uint32_t *) &sleep_record;
  for (uint32_t i = 0; i < sizeof(sleep_record) / 4; ++i)
    word[i] = 0;
  sleep_record.magic = SLEEP_RECORD_MAGIC;
}

/* For the firmware's function table instead of its timer start. */
void sleep_timer_start(void *node, int32_t us) {
  if (us < 4)
    us = 4;
  ROM_FN(PATCH_TIMER_START, void (*)(void *, int32_t))(node, us);
}

/*
 * For the firmware's function table instead of the ROM's idle hook (0x19223), which only makes sure that waiting for
 * an interrupt may put the domain to sleep.
 *
 * "The core overslept": the core answers, but its clock moved only a fraction of the standby (18 ms in 180 s in one
 * dump) and its timers did not fire. In every dump of it the core was not in deep sleep but in this idle wait, a
 * reason to stay awake was set (0x40000: the 802.11 power save state machine is not in doze, set by ROM 0x73EC and
 * 0x10EDA, cleared by 0x9AA and 0x10FE6; often also "timer pending"), the next timer was 1 to 20 ms away, less than
 * the 24 ms the deep sleep needs, and a keepalive frame had been sent 2 to 4 ms before, as the core woke for a
 * beacon. So its microsecond clock does not run in this wait while the host sleeps; with the host awake it does (it
 * continued when the host woke). What makes the hardware keep the clock is not in the code: the only control the
 * firmware has is STAY_AWAKE, which the vendor code sets when the host asks for the core and when the sleep handler
 * gives up an attempt, but not after a wake by the compare. It is set here whenever a reason to stay awake exists;
 * the record shows afterwards what it was.
 */
void sleep_idle(void) {
  uint32_t awake = AWAKE;
  sleep_record.idle_count++;
  sleep_record.idle_awake = awake;
  sleep_record.idle_timer_us = TIMER_COUNTER;
  sleep_record.idle_next_us = rom_next_timer_us();
  sleep_record.idle_stay_awake = STAY_AWAKE;
  sleep_record.idle_power_control = POWER_CONTROL;
#ifndef SLEEP_IDLE_OBSERVE_ONLY
  if (awake != 0)
    STAY_AWAKE = 1;
#endif
  ROM_FN(0x00019223, void (*)(void))();
}

/*
 * For the firmware's function table instead of its wake timing function (slot 0xFFF00448).
 *
 * The power controller switches the domain on in steps and waits between them: first for the supply, then for the
 * crystal oscillator and its PLL. Both waits come from the WLAN device data file (element 0xC3 of sys_sdd_*.bin:
 * 125 us and 563 us), which the ROM stores as two halfwords; the vendor function turns them into ticks of the
 * low-frequency clock for the power controller (0x0AC800B4, 0x0AC80004) and adds them to the lead time of the wake.
 *
 * On the device the core now and then does not come back from a sleep: the record shows that "go" was written and
 * the reset code never ran again, with the domain's power switch off. Nothing in the core's software runs in
 * between, so the waits are the only part of that path the firmware decides. They are raised here to see whether the
 * vendor's values are too tight; the lead time follows by itself. Costs: the core is powered that much earlier
 * before every wake.
 */
#ifndef SLEEP_SUPPLY_WAIT_US
#define SLEEP_SUPPLY_WAIT_US 500U
#endif
#ifndef SLEEP_CLOCK_WAIT_US
#define SLEEP_CLOCK_WAIT_US 2000U
#endif
#define WAKE_TIMING_MODE HALF(0xFFF01C0CU + 0x3A) /* 2: the two waits below are used */
#define WAKE_SUPPLY_WAIT_US HALF(0xFFF01C0CU + 0x3C)
#define WAKE_CLOCK_WAIT_US HALF(0xFFF01C0CU + 0x3E)

void sleep_set_timing(uint32_t arg) {
  if (WAKE_TIMING_MODE == 2) {
    if (WAKE_SUPPLY_WAIT_US < SLEEP_SUPPLY_WAIT_US)
      WAKE_SUPPLY_WAIT_US = SLEEP_SUPPLY_WAIT_US;
    if (WAKE_CLOCK_WAIT_US < SLEEP_CLOCK_WAIT_US)
      WAKE_CLOCK_WAIT_US = SLEEP_CLOCK_WAIT_US;
  }
  ROM_FN(PATCH_SET_TIMING, void (*)(uint32_t))(arg);
}

/* For the reset code instead of the ROM's warm start. */
void sleep_warm_start(void) {
  sleep_record.warm_starts++;
  ROM_FN(0x000054E7, void (*)(void))();
}

/* The vendor's 0x0900B5AC and the ROM's 0x52D6 in one: never returns. */
static void sleep_enter(uint32_t us) __attribute__((noreturn, noinline));
static void sleep_enter(uint32_t us) {
  SLEEP_CORRECTION_A = 0;
  SLEEP_CORRECTION_B = 0;
  uint32_t period = LF_PERIOD;
  us += rom_udiv64_32(period << 2, period >> 30, 0xA00, 0); /* four periods of the clock */
  uint32_t lead = WAKE_LEAD_US;
  if (us > lead) {
    us -= lead;
  } else {
    us = 0; /* the vendor code wraps around here and sleeps the longest time */
    sleep_record.lead_wraps++;
  }
  if (us > SLEEP_MAX_US)
    us = SLEEP_MAX_US;
  SLEEP_TOTAL_US += us;
  uint8_t fast = LF_FAST;
  uint64_t scaled = rom_umul64(us, fast ? 0x1E000U : 0xA00U);
  uint32_t ticks = rom_udiv64_32((uint32_t) scaled, (uint32_t) (scaled >> 32), period, 0);
  if (fast)
    ticks >>= 6;
  sleep_record.entries++;
  sleep_record.wakes_at_entry = WAKE_COUNT;
  sleep_record.slept_us = us;
  sleep_record.lead_us = lead;
  sleep_record.ticks = ticks;
  sleep_record.lf_period = period;
  sleep_record.lf_calibration = LF_CALIBRATION;
  sleep_record.irq_status = IRQ_STATUS;
  sleep_record.power_control = POWER_CONTROL;

  uint32_t counter = LF_COUNTER;
  uint32_t compare = ((counter + ticks) & LF_COUNTER_MASK) - SLEEP_CORRECTION_A - SLEEP_CORRECTION_B - 4;
  if (ticks < SLEEP_MIN_TICKS + 4) {
    compare = (counter + SLEEP_MIN_TICKS) & LF_COUNTER_MASK;
    sleep_record.short_sleeps++;
  }
  sleep_record.counter = counter;
  sleep_record.compare = compare;

  WAKE_COMPARE = compare;
  IRQ_ENABLE = 0x90; /* only the wake interrupts */
  POWER_CONTROL &= ~0x40000U;
  STAY_AWAKE = 0;
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
    uint32_t now = rom_now_us();
    remaining = *(uint32_t *) (beacon + 0x18) - now;
    beacon[0x0F] = 2;
    rom_timer_stop(beacon_timer);
  }
  if ((other[3] >> 1) & 3)
    rom_timer_stop(other_timer);

  uint32_t sleep_us = rom_next_timer_us();
  if (sleep_us >= SLEEP_MIN_US) {
    uint32_t delay;
    uint32_t counter = TIMER_COUNTER;
    uint32_t calibration = LF_CALIBRATION;
    if (counter < 2000 && !(calibration & 1) && HALF(PATCH_STATE) != 2) {
      delay = 2010 - counter; /* shortly after a start, clock not measured yet */
    } else if (ROM_FN(PATCH_BUSY_CHECK, int (*)(void))()) {
      delay = 20000;
    } else {
      uint32_t flags = rom_irq_lock();
      if (AWAKE == 0)
        STAY_AWAKE = 0;
      rom_irq_unlock(flags);
      flags = rom_irq_lock();
      if (EVENTS <= 4 && AWAKE == 0) {
        SLEEPING_FLAG |= 1;
        sleep_record.sleep_us = sleep_us;
        sleep_record.irq_flags = flags;
        sleep_record.remaining = remaining;
        if (rom_sleep_prepare()) {
          rom_sleep_save_time();
          sleep_enter(sleep_us);
        }
        SLEEPING_FLAG &= ~1;
      }
      STAY_AWAKE = 1;
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
  if ((other[3] >> 1) & 3) {
    uint32_t now = rom_now_us();
    rom_timer_start_exact(other_timer, *(uint32_t *) (other + 0x80) + *(uint32_t *) (other + 0x14) - now);
  }
}
