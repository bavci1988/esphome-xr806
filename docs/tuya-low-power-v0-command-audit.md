# TRV16 Tuya MCU low_power_v0: command and SDK coverage audit

Audit date: 2026-11-08. This document distinguishes the original TRV16
firmware, the supplied TuyaOS Lite/XR806 SDK and the in-progress ESPHome driver.
A host test passing is **not** a full XR806 compile or hardware verification.

## Evidence

- Original WXU/XR806 and HC32L136 firmware:
  [boot/wake](https://github.com/bavci1988/xr806-lab/tree/main/reverse-engineering/uart-protocol/trv16-boot-wake),
  [state/time/schedule](https://github.com/bavci1988/xr806-lab/tree/main/reverse-engineering/uart-protocol/trv16-state-time-schedule),
  [CMD12/CMD38](https://github.com/bavci1988/xr806-lab/tree/main/reverse-engineering/uart-protocol/cmd12-cmd38).
- `TuyaOS Lite 3.6.3-beta.14` archive and platform `xr806_0.0.3`:
  vendor SDK source/reference in the lab. The archive's application
  directory `TuyaOS/apps/` is empty; it does **not** contain the TRV16
  `tbl_common` MCU application or its full command table.
- Feature-branch driver:
  `esphome/components/tuya/tuya.cpp` and `tuya_low_power_v0.cpp`.

## Wire-command matrix

| Command / direction | Confirmed TRV16 behavior | Driver coverage and remaining work |
|---|---|---|
| 00 MCU -> WXU | First reset-marker heartbeat 00; later 01. Original WXU handles 00 differently before/after init | Basic branch present; robust recovery/sequence tests pending |
| 01 WXU <-> MCU | Product query/reply, PID, version, wk=20, mwk=28, sm | Query and autobaud present; product JSON/pin/mode interpretation and pre-probe MCU wake missing |
| 02 WXU <-> MCU | Network status; on 04, empty ACK followed by 17 ordered CMD05 DPs | Partial; initial status tracking and state consistency need replay tests |
| 05 MCU -> WXU | Per-DP report; WXU replies [00,DPID] | Basic single-DP report and ACK present; malformed/multi-DP/retransmission handling needs hardening |
| 06 MCU -> WXU -> MCU | Empty MCU time request; WXU returns eight local calendar bytes | Basic response; RTC/DST and resume test still needed |
| 09 WXU -> MCU | DP writes, empty ACK; **matching report not guaranteed** | Basic write + matching report tracker; DP106, clamped/missing reports and recovery missing |
| 12 WXU -> MCU | One-byte 01 entering logical low-power, 00 on conditional GPIO resume; separate two-byte config variant also found | Missing |
| 13 WXU -> MCU | Sent after MCU reboot by original WXU; this HC32 ignores it | Empty command sent in MCU-reboot path; needs trace regression |
| 38 MCU -> WXU | Sleep request; reply [00, flags], bit0 network veto, bit1 MCU-OTA veto | Always returns [00,01] (deliberately busy); dynamic flags and actual PM transition missing |

### Datapoints are *not* UART command numbers

- CMD02/04 triggers 17 reports with IDs:
  `1,3,4,2,17,16,10,11,7,19,20,108,101,102,104,105,107`.
- **DP106 BOOL1** is sent using CMD09; its empty ACK is followed by only:
  `1,3,4,2,17,16,19,20`. There is **no DP106 echo**.
  The current generic pending-write matcher would time out on this request.
- DP3 changed setpoint may report later, may be clamped, or may not be
  applied depending on HC32 conditions. An empty CMD09 ACK does not
  establish acceptance.
- DP104 schedule profile and DP107 eight-entry/32-byte weekly schedule
  behavior are described in the lab but lack a specialized ESPHome
  configuration/entity mapping. Time/date handling must apply DST once.

### Additional MCU dispatcher branches (verified jump-table correction)

The original HC32 switch at `0x618C` uses Thumb helper `0x5FF8`. The
**count byte** is at `0x61A2` (=17) and the **17 halfword-offset bytes**
begin at `0x61A3`, not at `0x61A2`. Reading the latter as index 0
would shift every command and falsely classify CMD10 as ignored.

Directly decoding these 17 bytes from the supplied 65,536-byte HC32 image
gives the following handler entry points (the switch helper computes
`(0x61A3 + 2 * table[cmd]) & ~1`):

| CMD | Handler | Evidence-based classification |
|---|---|---|
| 00 | 61B6 | MCU heartbeat |
| 01 | 61BC | MCU product information |
| 02 | 61C2 | Wi-Fi/network-status handling |
| 03 | 61FA | Sets RAM flag `0x200000FE=1`; likely network/pairing control, exact purpose unproven |
| 04 | 6202 | Sets RAM flag `0x200000FF=1`; exact purpose unproven |
| 05 | 639A | Default (MCU ignores WXU DP-report ACK) |
| 06 | 624C | Local time |
| 07 | 6258 | Reads two bytes, calls `0x76B4`; specialized service, meaning not fully confirmed |
| 08 | 639A | Default |
| 09 | 620A | Datapoint command write |
| 0A | 639A | Default |
| 0B | 626A | Reads two bytes, calls `0x75BE`; specialized service |
| 0C | 627C | Calls `0x7594`, writes a timer; specialized service |
| 0D | 628E | Sets transfer state / expected value; service transfer |
| 0E | 6326 | Checks transfer state, length, sends status CMD0E; service transfer |
| 0F | 639A | Default |
| 10 | 6398 -> 63D2 | Parses a length-bounded nested DP/record list through `0x645C` |

**Scope decision:** ESPHome needs `00,01,02,05,06,09,12,38` for the
observed thermostat protocol. The outbound `CMD12` and inbound `CMD38`
are not inside the same HC32 receive-switch table. CMD13 is accepted as a
no-op by this HC32 firmware and is sent only on the observed reset branch.

Do not emulate `03,04,07,0B,0C,0D,0E,10` without a specific user-facing
feature and a decoded payload contract. Several handlers resemble service
transport / MCU update sequences, but this is **not yet a verified complete
OTA command specification**. Unrecognized messages are logged and ignored;
they must not be replied to with fabricated payloads.

## What the TuyaOS SDK does and does not supply

**Useful, source-confirmed APIs/data:**

- `include/components/svc_wifi/include/tuya_cloud_wifi_defs.h`:
  `STAT_CLOUD_CONN=7` (original Tuya cloud status), distinct from UART
  status `04`; an ESPHome native-API connection is a compatibility mapping.
- `include/components/netmgr/include/lpmgr.h`:
  `TY_LP_UART=16`, `TY_LP_OTA=15`, `TY_LP_MQTT=8`, `TY_LP_DEEP=25`.
  `libnetmgr.a` defines `lpmgr_init/register/unregister/is_registered`,
  `lpmgr_get_cpu_max_sleep_time`, `lpmgr_set_cpu_fix_sleep_time`.
  These model subsystem locks, not TRV16 wire commands.
- `libs/libtal_system.a` contains `tal_cpu_acquire_wakelock`,
  `tal_cpu_release_wakelock`, `tal_cpu_sleep_mode_set`.
- The platform `tuyaos/tuya_os_adapter/src/system/tkl_sleep.c` maps
  `TUYA_CPU_SLEEP` to standby via `aux_pm_select_hib(0)`, and
  `TUYA_CPU_DEEP_SLEEP` to hibernation via `aux_pm_select_hib(1)`.
  **Do not equate this SDK enum with productinfo `sm`.**
- The platform has `tkl_wakeup.c`, XR806 HAL GPIO wake configuration,
  and `tkl_uart.c` UART initialization. PB04 and PA20 interpretation
  is established at the SDK mapping level, not electrically measured.
- `smart_frame.h` and `libsvc_dp.a` implement Tuya **cloud**
  datapoint services. Those are not the serial CMD05/CMD09 HC32 parser.
- SDK includes no complete TRV16 `tbl_common`, `tbl_mcu_ota_trans`
  implementation or exact product UART command dispatcher.

## Next development gates

1. **Complete XR806 firmware compile/CI** with the new Tuya component.
   Host tests cover only the portable session/autobaud helpers.
2. Fix parser limits and protocol transaction queue/recovery; test real
   recorded frames, ACK-before-report and report-before-ACK, malformed
   frames, DP106's eight-report refresh and omitted/clamped DP3 reports.
3. Parse/validate PID/version/wk/mwk/sm and implement conditional MCU
   wake before CMD01 probe, MCU reboot recovery and retained GPIO resume.
4. Map climate mode/setpoint, DP104/107 schedule, and time updates.
5. Implement CMD12 and CMD38 based on safe XR806 network/OTA/UART/API
   gates, with actual PM/wakeup behavior verified **separately** on hardware.
6. Independently classify the HC32's extra UART handlers 03/04/07/0B/0C/0D/0E
   before deciding which belong to an ESPHome replacement. Full Tuya
   cloud provisioning and vendor MCU-OTA need not be emulated without an
   explicit feature requirement.
