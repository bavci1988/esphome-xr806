# Stage 7b — Physical XR806 standby and wake-up (bench gate)

Status: **implementation branch / hardware UNVERIFIED**. Not enabled by
`xr806.lowpower: true` alone. Stage 7a WLAN DTIM and native API remain
unchanged. This is deliberately a one-shot, offline lab test; it is not
a battery-production idle scheduler.

## Pinned SDK evidence

The exact pinned `bavci1988/xr806-sdk` revision
`963f6f74030bc2fdbf430e66092ca96f6a0c7a6c` has
`CONFIG_PM=y`, `CONFIG_PM_WAKELOCKS=y`,
`CONFIG_STANDBY_RET_SRAM_ALL=y`. The default `PRJCONF_PM_MODE`
enables `PM_SUPPORT_STANDBY`. `platform_init_level0()` calls
`pm_start()`. The implementation calls
`pm_enter_mode_timeout(PM_MODE_STANDBY, 500)`; **not**
`PM_MODE_HIBERNATION` or Tuya's CPU deep-sleep enum.

On XR806 architecture v3, GPIO `PA10..PA23` map to hardware wake
indices `0..13`. WXU only exposes some of them; the trial allows
`PA11`, `PA12`, `PA13`, `PA19`, `PA20` and excludes
UART2/boot pins PA21..PA23. PA20 = index 10. PB04 is **not** a
hardware wake IO. A GPIO is set to `GPIOx_Pn_F6_EINT` before calling
`HAL_Wakeup_SetIO`, because the vendor PM implementation checks
`HAL_Wakeup_CheckIOMode()` before suspending.

The timer is always enabled and limited to 100..5000 ms as a
recovery safeguard. The PM wakelock wait is also bounded (500 ms);
an SDK software timeout is not counted as a successful wake-up.

## Minimal offline test YAML

```yaml
esphome:
  name: xr806-stage7b

xr806:
  board: wxu
  lowpower: true
  standby_trial:
    confirm_offline: true
    entry_after_ms: 10000
    wake_after_ms: 3000
    # Optional PA wake source; omit for timer-only test.
    wake_pin: PA20
    wake_edge: falling

logger:
  level: DEBUG
```

Do **not** combine this trial with top-level `wifi:`, `api:`,
`ota:`, `uart:`, `mqtt:`, `mdns:`, `network:` or XR806's
`wifi:` / `wlan:` / `wlan_smoke:` suboptions.
The YAML validator rejects these combinations. Run on a spare WXU /
isolated test setup; **do not flash the live TRV16 yet**. The
UART0 pins may carry HC32 data on a production TRV; any SDK / logger
diagnostics need their own safe path.

To test the PA20 wake pin: the **falling** configuration expects
an idle-high signal, then a controlled, debounced high-to-low edge.
Confirm the voltage and physical module pad before wiring. The timer
provides a second wake-up source. The pin must not also be owned by
a binary sensor or another component while the trial runs.

## What should happen

1. After the configured delay, the trial arms the wake timer, optionally
   configures PA20 EINT/pull-up/debounce, and tests the wake-IO mode.
2. Stage 7a policy must have **no blockers**; any optional
   `require_external_mcu(true)` remains a blocker until that MCU
   driver signals synchronization. The generic WXU has no required MCU.
3. The backend calls the **real SDK** `pm_enter_mode_timeout(PM_MODE_STANDBY, 500)`.
4. After PM returns, read `HAL_Wakeup_GetEvent()`.
   Timer = bit 16 (`0x00010000`); PA20 = bit 10
   (`0x00000400`). A return code of zero with one of these
   event bits is an encouraging **runtime indication**, not
   current-draw proof. Timeout/software bit 27 is **not** success.
5. Clear the timer and the `HAL_Wakeup_ClrIO()` hardware wake
   registration, release EINT setup and revoke the one-shot request.
   Do not enter standby again before reboot.

## Required bring-up evidence

- SDK build and AWIH generation from the pinned dependency.
- Hardware log with PM result, event bitmap and post-resume operation.
- Measurement of current before/after, with logger/LED loads accounted
  for. Prove retention by observing execution continuing after the
  standby call, rather than a cold boot.
- GPIO wake on PA20 **and** timed wake (independently).
- Repeated cold-boot tests; verify no watchdog resets/hangs.
- UART0 remains free of HC32 protocol corruption on real TRV hardware.

## Out of scope of this safe first Stage 7b slice

- WLAN/API/mDNS/OTA-aware standby and pending-message draining.
- GPIO wake from an MCU-specific signal: the XR806 hardware layer stays
  MCU-neutral; HC32 sync/wakeup and PB04 handling belong in an optional
  device-level driver.
- Background tickless-idle control, periodic production sleep, and
  proof that the original TRV16 firmware uses identical PM settings.

**Do not merge into a production profile** until physical standby
current and wake reliability have been measured.
