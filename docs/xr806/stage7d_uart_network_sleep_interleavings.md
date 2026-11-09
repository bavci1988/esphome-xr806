# XR806 Stage 7d: UART / WLAN / Native API / mDNS / OTA and PM races

Status: development and **SDK-link verification**, not approval to flash a
working TRV16. The underlying vendor ROM, PA20 wake polarity, PB04 ownership,
PA3 reset wiring and real suspend current have not been measured.

## Existing lifecycle owners retained

The Stage-7d changes build on existing integration in `dev`:

| Actor | Current owner contract | Hard limit |
| --- | --- | --- |
| Tuya MCU | One queued CMD01/CMD02/CMD09 transaction at a time; CMD09 ACK and matching CMD05 status are distinct; CMD02/04 requires the complete 17-DP series | Protocol has no transaction ID; an arbitrarily late same-command ACK is ambiguous, so timeouts quarantine writes |
| XR806 UART0 | Vendor PM wakelock, RX ring/framing health, full TX TEMT, PM idle vote | A hardware TX-empty flag is not an HC32 application ACK |
| XR806 Wi-Fi | Explicit `network_suspend` opt-in, wait for API/mDNS/OTA owner votes, disable STA, confirm association/link down, release vendor PM wakelock only at completion, re-enable on abort/resume | No claim that all proprietary WLAN driver work or real low current have been hardware-verified |
| Native API | Reject drain when clients are connected; close TCP listener before publishing idle; reopen on abort/resume | Outgoing dial-out is not quiesceable |
| mDNS | Execute unregister on lwIP thread, not ESPHome main loop, and publish idle only after callback completion | In-flight stale callbacks are epoch-tagged; no power measurement |
| OTA | `XR806OTAStandbyGuard` tracks OTA activity and holds OTA_BUSY; schema currently rejects unsupported XR806 OTA flash backend | **No real OTA suspend/resume** exists. Never release OTA_BUSY in an OTA-enabled configuration until a complete flash writer + listener lifecycle can be verified |

The generic XR806 platform does not understand Tuya command numbers. The
Tuya driver does not modify physical PM registers and must never fabricate an
HC32 GPIO-wake event after timer-only wake.

## Corrections and improvements in this integration

1. **Wire transaction timeout identity:** detect the expired transaction
   from `TuyaLowPowerWireTransactions::kind()`, not from an optional
   `expected_response_` field that may already have been cleared by a
   reset or asynchronous error. Late same-command ACKs do not release the
   quarantine; identity/product and full-status recovery are required.
2. **Monotonic PM-veto fencing:** `XR806LowPowerPolicy` now samples an
   ever-increasing abort epoch *before* claiming the final commit phase.
   Owner updates, duplicate last-moment votes, new network quiescing or a
   cancellation cannot be erased by clearing a reusable boolean during
   admission. The coordinator also checks owner generation/blockers.
3. **Native API shutdown barrier:** If reboot/shutdown begins while the
   listener is already parked, the API explicitly reasserts API_BUSY and
   withdraws PM/preflight before closing anything. A paused API listener
   therefore cannot silently authorize CPU standby during teardown.
4. **mDNS callback generation:** `XR806StandbyAsyncEpoch` is shared by
   the real lwIP-backed mDNS owner and the host tests. An old callback
   from a cancelled/restarted preflight cannot release the new mDNS
   owner vote. The callback rechecks cancellation after publishing.
5. **No unsafe OTA shortcut:** A state listener can veto standby on OTA
   activity, but is not equivalent to a flash writer, TCP listener and
   pending-write drain. OTA is deliberately fail-closed.

### Ordering and race model

```text
HC32 CMD38
    |
  MCU UART session idle? ACK/report burst complete? checksum good?
    | no -> CMD38 busy
    v yes
  WLAN/API/mDNS/OTA may_quiesce? [OTA if present: NO]
    | no -> CMD38 busy
    v yes
  Preflight drains API listener -> mDNS lwIP unregister -> WLAN STA disconnect
    | any new RX/TX, OTA, client, preflight timeout -> cancel, resume owners
    v
  CMD38/00 only after owner votes are idle and UART is drained
    |
  CMD12/01, hardware TEMT, full quiet interval, native PM locks released
    | any new work or abort-epoch change -> compensate CMD12/00; restore owners
    v
  Platform PM_MODE_STANDBY (vendor may refuse/downgrade)
    |
  Confirm GPIO wake before CMD12/00; unknown/timer-only wake requires resync
    |
  Restore network owners and acquire locks before re-enabling traffic
```

The ordering model deliberately has two independent barriers:
a **software admission fence** (`generation`, veto epoch and blockers)
and **vendor hardware PM wakelocks** held by UART0 and WLAN. It is not
a single atomic transaction with the vendor ROM; do not claim this provides
hardware-atomicity across an unverified RF/ISR producer. A hardware
producer must acquire its own vendor wakelock *before* emitting work.

## Host regression coverage

`tests/xr806/standby_interleaving_test.cpp` compiles with no XR806 SDK and
executes production pure state/policy code. It covers:

- CMD09 ACK vs. delayed CMD05/DP3 adoption; mismatching reports
- CMD02/04 ACK vs. all 17 status reports; unexpected/late CMD09 ACKs
- Expired writes and quarantine; no unconditional ACK replay
- CMD38 rejection while MCU not synchronized, conditional CMD12 wake
- Owner-veto assertions during PM commit, including a second host thread
- All **120 permutations** of UART, WLAN, API, mDNS and OTA busy/idle
  transitions after an admission claim
- mDNS asynchronous callback generation, cancellation/ABA and restart
- PM quiet-interval resets, abort and unknown/timer-only wake handling

These tests establish behavior of the integrated **software policy**
under modeled event orderings; they do not execute the vendor PM task,
real ARM interrupt handlers, lwIP socket teardown or actual UART hardware.
The exact original WXU queue/lock interleavings also remain only partly
reverse-engineered in the lab.

## Outstanding for physical readiness

- Measure real PA20/PA3/PB04 levels, retention and current on a spare WXU.
- Check suspended UART/WLAN with a logic analyzer under simultaneous
  TCP reconnect, mDNS queries, HC32 burst reports and timer/GPIO wake.
- Audit proprietary WLAN PM wakelock producers and the active platform
  PM mode mask: a successful `pm_enter_mode_timeout()` is not proof of
  retention or real sleep.
- Implement and test a real XR806 OTA flash backend and complete OTA
  socket/flash-drain lifecycle before any OTA-enabled standby support.
- Investigate PM semaphore waits that may exceed the nominal SDK 500 ms
  argument, including watchdog/recovery behavior.
