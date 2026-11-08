# Tuya MCU Low-Power v0 — experimental feature branch

`tuya.protocol` selects a **dialect**, not the UART frame's version byte.
If omitted, ESPHome retains its existing standard Tuya implementation.

```yaml
uart:
  id: uart_bus
  tx_pin: PB0
  rx_pin: PB1
  baud_rate: 115200  # First candidate; scanner also tries 9600.

xr806:
  board: wxu
  lowpower: true

tuya:
  id: tuya_mcu
  uart_id: uart_bus
  protocol: low_power_v0
  auto_baud: true

climate:
  - platform: tuya
    name: "TRV16"
    tuya_id: tuya_mcu
    target_temperature_datapoint: 3
    current_temperature_datapoint: 4
    target_temperature_multiplier: 0.1
    current_temperature_multiplier: 0.1
```

## Implemented in the feature branch

- `standard` remains the default; existing Tuya climate/sensor classes reuse
  the same Tuya parent class with protocol-dependent UART dispatch.
- `auto_baud: true` on XR806 probes 9600/115200 with CMD01 at 500 ms per
  candidate, retaining only a checksum-valid expected product reply.
- CMD01 product metadata is parsed and restricted to the verified TRV16 PID,
  `wk=20`, `mwk=28` and `sm=0/1`.
- CMD02 network-status reports, CMD05 single/multi-DP reports with a
  `[00, DPID]` acknowledgement for each DP, CMD06 local-time response,
  CMD09 writes, and CMD00 MCU-reset heartbeat.
- The 17-DP status04 report set is tracked independently. CMD09/DP106
  requests an eight-DP refresh set (no DP106 echo). Empty CMD09 ACK,
  actual reported value, missing/clamped report and timeout are distinct
  outcomes. Errors trigger controlled full-status resynchronization.
- MCU synchronization is an XR806 optional sleep blocker; product `sm`
  selects 5000 ms or 10000 ms connected wake-margin.
- `mcu_wake_pin` optionally pulses an explicitly configured output for
  10 ms before CMD01 probes. Pulse polarity is controlled by the pin's
  `inverted` setting. **Do not enable on hardware until pinout and
  polarity are verified.**
- A CMD12/01 and CMD12/00 transition hook is exposed for the future XR806
  PM bridge, but it is **not** invoked by the current platform.
- CMD38 remains fail-closed: the network-blocker bit is set even when the
  MCU is synchronized because no physical PM entry exists yet. OTA status
  from the generic XR806 policy can add bit1, yielding `[00,03]`.

## Hardware and safety limitations

This implementation is not flash-ready for battery-powered operation. The
XR806 PM driver still does not enter verified standby/hibernation, and GPIO
resume, persisted baud, real power consumption and the original RTOS wake
sequencing remain untested. The original physical MCU-wake pulse timing
cannot be inferred just from software disassembly. The safe-sleep hook must
be connected to an actual suspend/resume boundary, with TX completely
drained and wake sources verified.

**Do not assume product field `mwk=28` means `PB4` in ESPHome.** The
WXU/XR806 board schema deliberately reserves PB2..PB7 for Flash/XIP,
including PB4. Attempting to configure PB4 as an output is rejected.
The CI YAML uses PA19 **only as a configuration fixture**, not as a
confirmed MCU-wake GPIO. Use a scope/pin trace to establish electrical wiring
before assigning a real wake output.

The optional `mcu_wake_pin` is absent in the sample above by design.
If the hardware mapping is established, its YAML pin can be set explicitly.

Time is supplied as **local time** by the ESPHome time source, without a second
timezone/DST conversion. Without valid time the eight-byte CMD06 reply is zero.
`force_time_sync` and `status_pin` are deliberately rejected in this dialect.

The initial host-only transaction and autobaud tests can be executed with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I. tests/tuya_low_power_v0_session_test.cpp -o /tmp/tuya_lp_test
/tmp/tuya_lp_test
```

Evidence: `bavci1988/xr806-lab` under
`reverse-engineering/uart-protocol/trv16-boot-wake/` and
`reverse-engineering/uart-protocol/trv16-state-time-schedule/`.
