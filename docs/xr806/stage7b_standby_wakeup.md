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
    # Do not enable PB05 marker: pin remains conservatively reserved as possible SPI Flash/XIP.

# No logger: yet. The XR806 ESPHome logger backend is not implemented.
# Capture power/GPIO externally on the spare bench WXU.
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
5. The experimental PB05 resume marker is **disabled at YAML validation**. The WXU board map conservatively reserves PB05 for potential SPI Flash/XIP, and the actual TRV16 electrical ownership is not established. Use current measurement, timer/PA20 input observation and a dedicated verified spare pin instead.
6. Clear the timer and the `HAL_Wakeup_ClrIO()` hardware wake
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

## Reverse-engineering alignment (xr806-lab, 2026-11-08)

The current `xr806-lab` reconstruction is based on the actual HC32 1.3.1
and WXU 1.1.4 binaries; the normal boot and GPIO-wake paths have been
executed in isolated Unicorn tests. These are **application-protocol
findings**, not evidence that the physical board already reaches its
requested PM state.

- **Separate cold boot, retained standby resume and hibernation restart.**
  A fresh WXU probes CMD01 (not a mandatory CMD00 heartbeat) at 9600 or
  115200 baud, caches and replays the first valid CMD01 product reply,
  initializes LP before IoT, then reacts to network status. A GPIO-wake
  from retained standby does **not** automatically repeat autobaud,
  CMD01, IoT initialization or DP106. Hibernation, if reached, boots anew.
- **`sm` is MCU-specific and not a platform sleep enum.** In the
  supplied HC32 build, `sm=0` iff both RAM byte `0x2000003B` and the
  time-valid flag `0x2000003C` are nonzero; otherwise `sm=1`.
  The original WXU maps `sm=1` to standby request, `sm=0` to
  hibernation request; SDK mode constants remain 2/3 respectively.
  This offline generic WXU trial intentionally selects standby **without
  inspecting any MCU**.
- **Physical roles:** the TRV16 productinfo `wk=20` is **PA20 /
  WXU module pad 9 / MCU-to-WXU input** (hardware wake index 10).
  `mwk=28` is **PB04 / pad 2 / WXU-to-MCU output**; it cannot be used
  as an XR806 hardware wake IO. The actual electrical edge/pulse
  polarity is not established by firmware-only evidence. The trial's
  `falling` value is an explicit laboratory configuration, not a
  device-specific confirmed property.
- **CMD12 and CMD38:** CMD12/01 is an application sleep-transition
  notification, not proof of physical sleep. The GPIO LP-task emits
  CMD12/00 only if a logical sleeping flag was previously set, then
  clears that flag and extends its *software* awake interval by at
  least 5000 ms. The supplied HC32 dispatcher ignores incoming
  CMD12/38 and issues no ACK. CMD38/00 can cause a low-power request
  on the original WXU when its particular network/MCU-OTA flags are
  clear; this is **not sufficient** to show that UART writes and
  report bursts have drained.
- **TRV16 driver responsibilities (NOT implemented by this MCU-neutral
  bench backend):** serialize write/refresh transactions; distinguish
  empty CMD09 ACK from eventual CMD05 state adoption; keep a sleep
  blocker for UART TX completion, RX processing and pending DP series
  (17 reports after CMD02/04; eight reports on DP106 refresh); guard
  200 ms of stable idle before suspend; honor at least 5000 ms
  software awake after MCU GPIO wake; explicitly classify wake cause
  and cold start versus resume. When connected, CMD02/04 can trigger
  the 17-DP series and a later, conditional CMD06 time request; DP106
  is not a mandatory bootstrap command.
- **Firmware/hash evidence:** HC32 SHA256
  `2e86346a530266770ada2397e5abdabc6c5bf0c31153021a6b3e46567e3d9be4`,
  WXU SHA256 `16b7a9a3dbb722fd51194de04551d38f127b574568453abc7289a0397e86be39`.
  These exact hashes match the working analysis binaries.

Source-of-truth details live in the private lab:
`reverse-engineering/uart-protocol/trv16-boot-wake/README.md`,
`reverse-engineering/uart-protocol/cmd12-cmd38/README.md` and
`reverse-engineering/xr806/deep-sleep-disassembly/README.md`.

## CI and debug restrictions

The ESPHome XR806 platform currently lacks a native `logger:` UART
backend. Adding `logger:` caused the generic ESPHome code generator to
fail in `logger.uart_selection()`; it also risks corrupting HC32 UART0.
Therefore this offline test **rejects logger explicitly** until a
dedicated, non-HC32 debugging transport is implemented. Verify standby
by current draw and the dedicated PA20 wake input under bench conditions. The PB05 debug marker is disabled until flash/pin ownership is verified.
The private `xr806-sdk` repository has a separate cross-build workflow
which checks out the pinned SDK and the Stage 7b ESPHome branch.
