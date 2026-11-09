# XR806/WXU Stage-8 / Part-V consolidation (2026-11-09)

## Integration decision

**Canonical integration branch:** `dev`. Keep the XR806 platform MCU-agnostic. The shipped ESPHome driver remains `esphome/components/tuya/tuya_low_power_v0.cpp`; the UART transaction gate, wake-guarded standby controller and service participants belong in their existing locations. **Do not include a second independent transaction/sleep engine in firmware.**

The standalone `XR806_ESPHome_PartV.zip` host prototype was compared as a design/test reference (SHA-256 `7c249c1a84aef45d0fa4983bf428c747d17796c3d4610284d8f981bb6c2e307d`). Its vendor PM-bridge and raw-UART glue are not proven in an XR806 target build; importing them verbatim would duplicate state owners and weaken validation. Its 720 permutations/1,024 parser-noise cases were host-level simulations, **not hardware validation or a formal exhaustiveness proof**.

## Feature mapping

| Part-V concern | Canonical ESPHome implementation | Current qualification |
| --- | --- | --- |
| Serialize CMD01/02/09 and quarantine timeouts | `tuya_low_power_wire_transactions.h` and `tuya_low_power_v0.cpp` | No V0 wire sequence number exists; cannot attribute arbitrarily late same-command replies absolutely. |
| Late ACK at/after deadline before timer sweep | `TuyaLowPowerWireTransactions::overdue()` + `Tuya::handle_command_()` | Forward-ported from old Stage8 race branch with host regression including millis rollover. |
| CMD38 busy while writes/status/owners are active | `tuya_low_power_v0.cpp` with service preflight in XR806 controller | Fail-closed, no synthetic HC32 CMD12/CMD38 ACK. |
| PM admission and concurrent veto | `xr806/lowpower_policy.h`, `standby_controller.cpp`, `standby_async_epoch.h` | Newer monotonic abort-epoch scheme from Stage7d supersedes old `snapshot()`-only variant. Hardware IRQ/PM atomicity still requires board tests. |
| WLAN/Native API/mDNS reversible preflight | `wifi_component.*`, `api_server.*`, `mdns.cpp`, `standby_network_lifecycle.h` | Epoch-aware async mDNS callback protection; do not substitute older lifecycle revisions. |
| OTA suspend/recovery | `xr806/standby_ota_guard.h` | **Intentionally vetoes PM while OTA transport lifecycle is unproven**, including listener drain and flash-write ownership; complete reversible OTA listener and all backend handling before lifting veto. |
| Hardware validation | XR806-SDK pinned cross-build workflows | Host and cross-build successes do not demonstrate real wake lines, UART pulse shape, current draw, safe OTA resume or interrupt priority. |

## Reconciliation of divergent development branches

- `feature/xr806-stage8-race-hardening` (audit PR #10): ACK-deadline fix is unique and was forward-ported to `dev`. Its mDNS implementation is **older** than the generation-aware Stage7d work merged in PR #9 and was not copied back.
- `feature/xr806-stage8-concurrency-lifecycle` (audit PR #11): contains an earlier parallel service-owner architecture and OTA listener teardown ideas. The branch diverges significantly and introduces separate coordinator/service lifecycle classes that overlap newer implementations. Whole-branch merging risks regression, duplicated ownership and unverified resume behavior. Its experimental code is retained in an immutable archive tag for future comparison rather than enabled on production firmware.
- Other merged/stale feature branches can be replaced by archival tags and deleted, leaving only the default `dev` branch.
- In `xr806-sdk`, Stage7b cross-build PR #3 was merged after confirming successful combined build in workflow run 42620978196. Dependabot PR #2's two `actions/upload-artifact@v7.0.2` updates were applied directly to `main` to avoid a stale-branch conflict.

## Release/bench restrictions

**Not certified for direct flash on the only live TRV16/WXU.** Required next evidence: real PA20/PB04 signal capture, SDK PM suspend/noirq boundary audit, Wi-Fi/Native API/OTA/mDNS resume with active traffic, power consumption, repeated battery cycles, corrupted settings and interrupted OTA recovery. Preserve the vendor dump and a known-good flash/recovery method. Do not present positive CMD38 reply or successful SDK PM return as proof of physical Standby.

## Audit trail

PRs: ESPHome [#8](https://github.com/bavci1988/esphome-xr806/pull/8), [#9](https://github.com/bavci1988/esphome-xr806/pull/9), [#10](https://github.com/bavci1988/esphome-xr806/pull/10), [#11](https://github.com/bavci1988/esphome-xr806/pull/11); SDK [#2](https://github.com/bavci1988/xr806-sdk/pull/2) and [#3](https://github.com/bavci1988/xr806-sdk/pull/3).
