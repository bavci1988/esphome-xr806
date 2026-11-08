# XR806 Stage 7b – production-side suspend contract

**Status 2026-11-08:** code and ARM link validation in progress; **not cleared
for flashing a functioning TRV16**. The single available WXU is installed
in a real thermostat. The protocol branch is independently developed at
`feature/tuya-low-power-v0`; do not copy its command parser into XR806.

## Ownership boundaries

| Operation | Owning layer |
|---|---|
| Allwinner `PM_MODE_STANDBY`, SRAM retention and low-level PM calls | `xr806` |
| Hardware wake timer, PA20 EINT, wake status bitmap and GPIO mux restore | `xr806` |
| Shared blockers for UART/WLAN/API/OTA/mDNS/optional MCU; quiet interval | `xr806` |
| UART0 baud reconfiguration via `HAL_UART_SetConfig`, RX drain, TX flush, GPIO bus pins | `uart` |
| Scan 9600/115200 using valid CMD01 product-info frames | `tuya` (`low_power_v0`) |
| Tuya packet parsing, command routing, framing and checksum | `tuya` |
| CMD00, CMD01, CMD02, CMD05, CMD06, CMD09, CMD12, CMD38 | `tuya` |
| MCU `wk`/`mwk` decoding, physical PB04 MCU-wake pulse, `sm` policy | `tuya` or the optional MCU-specific adapter |
| DP state adoption, 17-DP status series, eight-DP refresh, time validity | `tuya` |
| Sleep request and post-resume protocol exchange | MCU adapter via generic `XR806StandbyParticipant` |

The XR806 component must **never** construct or inspect `55 AA` UART
frames, assume a Tuya MCU exists, interpret `sm` as a raw PM mode, or
automatically reply to `CMD38`.

## Configuration: coordinator versus bench trial

The legacy `xr806.standby_trial` is a one-shot, offline, hardware-bench test.
It is excluded from the real device configuration. The new opt-in
`xr806.standby` exposes only platform PM settings:

```yaml
xr806:
  board: wxu
  lowpower: true
  standby:
    enabled: true
    wake_after_ms: 3000
    quiet_ms: 250
    min_awake_ms: 5000
    wake_pin: PA20
    wake_edge: falling
    require_external_sync: true
```

This **does not automatically request a sleep**. A platform without a
connected MCU also remains awake until an application explicitly asks.
An unavailable/active transport remains blocked by default. With a
configured UART, at least one registered lifecycle participant is
required before the coordinator will attempt standby. If Wi-Fi,
Native API, OTA or mDNS is configured, their owner must explicitly
clear the corresponding idle blocker. These votes are **not yet**
integrated for all of ESPHome's network transports. Therefore the
present TRV16 software stack is intentionally not yet eligible for
battery-production standby.

The radio's `power_save`/DTIM feature is independent of CPU standby.

## Contract for the separately developed Tuya MCU driver

The platform API is in `standby_controller.h`:

```cpp
struct XR806StandbyResume {
  uint32_t event_bitmap;
  int pm_return;
  bool expected_source;
  bool wake_gpio;
  bool wake_timer;
};
class XR806StandbyParticipant {
 public:
  virtual bool prepare_for_standby() = 0;
  virtual void standby_cancelled() = 0;
  virtual void standby_resumed(const XR806StandbyResume &) = 0;
};
XR806StandbyController *standby_controller();
```

Suggested interaction *inside the optional MCU driver*, not inside
`xr806`:

1. On setup register one lifecycle participant and call
   `lowpower_policy().require_external_mcu(true)`; keep the MCU
   synchronization blocker until a checksum-valid CMD01 and the
   corresponding status/transaction state are reconciled.
2. On CMD38 calculate the **Tuya-specific** response flags (network
   and MCU-OTA state), frame the reply and keep the UART blocker.
   Do not turn a `00 00` reply into unconditional MCU readiness.
   A pending write, an unconfirmed CMD05 report or an RX/TX burst
   must still block standby.
3. Once the device-level conditions permit a suspend request, call
   `standby_controller()->request_standby(true)`. Its caller must
   also ensure the selected MCU `sm` policy is compatible with a
   *retained* Standby. A request that requires Hibernation must be
   rejected until the separate Hibernation/boot path exists.
4. `prepare_for_standby()` checks the *live* UART flush, pending
   packet parsing, CMD09 adoption and status-series state.
   To send CMD12/01 or an MCU wake pulse, do so in the MCU
   component and **return false for that iteration**; the platform
   waits another full quiet interval after UART activity.
   Do not wait for HC32 ACK to CMD12 or CMD38 (the supplied MCU
   handler does not send one).
5. `standby_resumed()` sees the raw hardware event. For a retained
   **GPIO** wake and a previously marked sleeping application,
   the MCU-specific handler may send CMD12/00, clear its logical
   sleep state and maintain at least a 5-second awake interval.
   A timer/WLAN wake does not automatically imply CMD12/00.
   Restore ownership of any MCU IO interrupts after the
   platform restores the PA20 mux.
6. Hibernation, if ever enabled, requires a *new* CMD01 autobaud
   sequence and full application initialization, not a
   retained callback. An MCU reset during a retained WXU session
   also follows its distinct CMD00 handling.

### Technical blockers intentionally kept

- The old `pm_enter_mode_timeout(..., 500)` is **not globally bounded**
  by 500 ms. The SDK waits indefinitely on a PM semaphore after
  wakelock processing. No software-only test can guarantee recovery.
- The vendor PM path may downgrade the requested sleep mode based
  on its runtime platform mask and driver vetoes. `pm_return==0`
  plus a timer/PA20 event is only a software-level resume indication.
- GPIO input edge, Wi-Fi/Native API suspension semantics, GPIO
  retention, current draw and 24-hour stability have no physical
  evidence from this one-WXU setup.
- The present generic ESPHome logger has no XR806 backend. Do not
  put logger traffic on the TRV16's MCU UART0.
- Every asynchronous writer/IRQ owner must additionally honor
  the SDK's native PM wakelock semantics. A C++ atomic idle vote
  alone cannot close the race with independent RTOS threads.

## Acceptance gates before describing a real TRV16 as production-ready

1. Passing MCU-neutral host tests, Stage 7b production AWIH build
   and merged-branch Tuya-autobaud AWIH build.
2. Registered MCU participant and explicit UART TX/RX/pending-write
   gates; real network/API/OTA lifecycle owners for any active transport.
3. Wake edge, wake event, retained MCU protocol state and
   measured current verified on actual board. No silent fallback
   from Hibernation into Standby.
4. Recoverable rollback/flash plan before touching the only
   functional TRV16. Neither an offline bench image nor a build
   artifact is a safe production image by itself.

See the private `xr806-lab` research under
`reverse-engineering/uart-protocol/trv16-boot-wake`,
`reverse-engineering/uart-protocol/cmd12-cmd38` and
`reverse-engineering/xr806/deep-sleep-disassembly`.
