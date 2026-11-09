# XR806 ↔ Tuya low_power_v0: guarded standby integration

**Status: development / CI validation, not cleared for the only live TRV16.**
The XR806 platform does not know Tuya commands or HC32 datapoints.
The MCU dialect implements its own `XR806StandbyParticipant` and calls
the generic XR806 controller only after protocol-level checks.

## Neueste HC32-/WXU-Rekonstruktion (2026-11-09)

Die neue [Analyse von DP101/102, Produktinfo-Recovery und
PM-Veto-Pfaden](trv16_rtc_sleep_window_recovery.md) wurde im
Tuya-MCU-Teil integriert. DP101/102 können eine RTC-abhängige
`sm`-Neuaushandlung bewirken; der XR806 darf den alten Produktstatus
nicht dauerhaft als stabil ansehen. Ein eigener bounded CMD01-Retry
behandelt malformed/fehlende Produktinfo, ohne fremde PIDs zuzulassen.

Die Plattform bleibt protokollneutral. Weder eine durch SDK-PM-Masken
herabgesetzte Stufe noch ein Wake-Event beweisen physisch erreichten
Standby. Für den angeschlossenen TRV16 bleiben sämtliche
Netzwerk-/API-/Hardware-Sperren unverändert fail-closed.

## Ownership matrix

| Owner | Responsibility |
|---|---|
| XR806 platform | PM_MODE_STANDBY (retained RAM), wake timer, PA20 EINT, native PM callbacks, software sleep gate and resume cause |
| XR806 UART0 | PB0 TX/PB1 RX, RX ring, 9600/115200 baud switching, TX-shifter-empty verification, sticky line errors, **native SDK PM wakelock** |
| XR806 GPIO | Inversion, input/output/interrupts, safe ownership and mux save/restore, optional guarded PB04 output |
| Tuya low_power_v0 | CMD00, CMD01/autobaud, CMD02/05/06/09, CMD38 flags, CMD12/01 before sleep, conditional CMD12/00 on GPIO wake, product `sm`, `wk`, `mwk` |
| ESPHome network/API/OTA/mDNS | Must independently release their standby blockers only after a proven network suspend/drain lifecycle |

## Protocol flow

1. The MCU driver validates CMD01 product metadata. For the known TRV16,
   `wk=20` maps to **PA20** (WXU pad 9) and `mwk=28` maps to **PB04**
   (WXU pad 2). `sm=1` requests *retained standby*; `sm=0` requests
   *hibernation* and is currently **rejected** by this bridge.
2. A valid CMD02/04 → 17-DP CMD05 status series establishes initial
   synchronization. A CMD09 ACK is **not** proof a write has been applied.
   Pending writes, status reports, UART errors and any queued command
   keep the MCU blocker set.
3. MCU CMD38/00 is a *provisional* sleep request. A zero flags response
   is possible only when the correct PA20 wake pin and external sync are
   configured, the protocol and UART are healthy, no OTA/API/WLAN/mDNS
   owner veto remains, and a retained standby request is permissible.
   CMD38 does not prove CPU sleep occurred.
4. The platform's first preparation calls into Tuya; it sends CMD12/01
   and flushes to **hardware TX shift completion**. That preparation
   intentionally returns false. The platform waits for UART/native
   wakelock drain and another quiet interval before retrying.
5. The next preparation can succeed only if nothing changed. The
   platform then arms timer + PA20 wake and calls SDK PM_MODE_STANDBY.
   The vendor `pm_enter_mode_timeout(..., 500)` is *not* a globally
   bounded 500-ms operation and may block on an SDK PM semaphore.
6. If standby preparation aborts after CMD12/01, the MCU driver sends
   a compensating CMD12/00 and waits at least five seconds before
   another attempt. A new MCU report, bad checksum, API write or
   repeated CMD38 also cancels a pending handoff.
7. After a confirmed **GPIO** wake, the MCU driver sends CMD12/00 if
   it had actually sent CMD12/01. A timer-only or unexpected PM return
   does not impersonate GPIO wake; further standby is blocked pending
   verified MCU recovery.

## UART physical baud-scan exception

`tuya.low_power_v0` owns the 9600/115200 CMD01 scanner. On probe
start it asks the XR806 UART0 **transport** to enable
`set_baud_probe_mode(true)`; on a checksum-valid CMD01 lock it
disables this window. While the scanner is active, receiving framing
errors or a wrong-baud RX ring overflow is an **expected candidate
mismatch**, not a permanently faulted UART. Crucially:

- UART and MCU PM blockers remain asserted for the whole scan;
- TX errors, UART clock configuration errors and PM wakelock failures
  are **never** ignored by the probe exception;
- After lock, framing/parity/overrun errors and ring overflow again
  latch the transport fault and block standby;
- A valid CMD01 *packet* locks baud, while product PID/wk/mwk/sm
  validation separately determines whether the MCU is supported.

The original HC32 baud rate and the PA20/PB04 electrical levels are
still not proven by this software-only change.

## PB04 wake output (experimental, off by default)

The board's ordinary GPIO schema conservatively reserves PB02..PB07
as possible SPI Flash/XIP pins. Do **not** configure the real HC32
wake line as PA19 or another spare GPIO. `tuya.mcu_wake_pin` on XR806
is intentionally rejected until hardware ownership is known.

A restricted, optional PB04 output driver is available only with
the **two explicit declarations below**:

```yaml
tuya:
  id: trv16_mcu
  uart_id: uart_bus
  protocol: low_power_v0
  auto_baud: true
  trv16_mcu_wake_pb04: active_high  # choose ONLY after measured physical polarity
  trv16_pb04_confirmed: true      # only after confirming Flash/SPI pin ownership
```

These lines are **an API example, not a safe recommended configuration**.
The mode can be `active_low` or `active_high`. The board must be
electrically inspected and the pin ownership/edge polarity determined
before using either. At runtime, the guarded output refuses ownership
unless PB04 is currently input or disabled, preserving active SPI,
output and interrupt muxes. A current pinmux check alone is not
proof PB04 is electrically disconnected from Flash, so this explicit
flag is a user-provided assertion rather than an automatic verification.
No production TRV16 should use this opt-in yet.

A 10-ms pre-CMD01 pulse is made **only** when the guarded output
has been successfully claimed. An existing GPIO callback is not
silently repurposed.

## Verification and remaining release gates

- Portable host test `tests/tuya_low_power_standby_state_test.cpp`
  covers `sm=0` veto, owner/UART failures, repeated/aborted handoffs,
  deferred CMD12, conditional GPIO resume and timer-only resync.
- Existing XR806 host tests cover policy votes, wake event classification
  and cooldowns. The pinned vendor SDK CI cross-builds standalone
  standby, combined UART+Tuya, and WLAN/API+Tuya firmware.
- Both live hardware electrical wake polarity and **actual current
  consumption** remain unverified. UART retention across PM, precise
  MCU response timings, SDK watchdog interactions and >24-hour
  thermostat reliability are also unverified.
- **Connected WLAN/API is deliberately fail-closed.** No proven driver
  release of `NETWORK_BUSY`, `API_BUSY`, `OTA_BUSY` or
  `MDNS_BUSY` exists yet for a persistent Home Assistant connection.
  This means the current bridge generally cannot physically sleep in
  the actual connected TRV16 profile, even if it cross-builds.
- **Offline MCU synchronization also requires evidence:** the
  observed initial full 17-DP status series follows network status04.
  Without it, the MCU gate remains blocked. Do not bypass this merely
  to get a short idle/sleep measurement.
- The sole original WXU is still installed in the functioning TRV16.
  A validated recovery/flashing plan and a separate bench device are
  needed before hardware sign-off.

Original observations and emulation: private `bavci1988/xr806-lab`
under `reverse-engineering/uart-protocol/trv16-boot-wake`,
`cmd12-cmd38`, and `xr806/deep-sleep-disassembly`.
