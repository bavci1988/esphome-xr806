# Tuya MCU Low-Power v0 — experimental feature branch

`tuya.protocol` selects a **dialect**, not the UART frame's version byte.
If omitted, ESPHome retains its existing standard Tuya implementation.

```yaml
uart:
  id: uart_bus
  tx_pin: PB0
  rx_pin: PB1
  baud_rate: 115200  # Change to actual MCU baud rate; auto-baud not ported.

xr806:
  board: wxu
  lowpower: true

tuya:
  id: tuya_mcu
  uart_id: uart_bus
  protocol: low_power_v0

climate:
  - platform: tuya
    name: "TRV16"
    tuya_id: tuya_mcu
    target_temperature_datapoint: 3
    current_temperature_datapoint: 4
    target_temperature_multiplier: 0.1
    current_temperature_multiplier: 0.1
```

## Initial implementation

- `standard` is the implicit default. Existing `tuya.climate`, `tuya.sensor`
  and other consumers continue to use the same `Tuya` parent class.
- On WXU startup, `low_power_v0` sends CMD01 product query directly.
- Handles CMD02 network status, CMD05 reports and immediate `[00, DPID]` ACK,
  CMD06 local time query, CMD09 writes and empty ACK, and CMD00 MCU-reset heartbeat.
- Tracks the 17-DP CMD02/status04 report series and checks write confirmation
  separately from the empty CMD09 ACK; a mismatched value does not confirm a write.
- Integrates with the existing optional XR806 MCU synchronization sleep gate.
- **CMD38 always replies busy (00 01)** until physical standby / wake integration
  is verified. CMD12 sleep/wake signaling and actual CPU standby are not implemented.

## Not finished / hardware safety

This commit is **not** a flash-ready, production-quality TRV16 driver.
It does not implement autobaud (original WXU scans 9600/115200), saved
product-info replay, complete error recovery, RTC retention, GPIO-wake delivery,
or XR806 standby. The current protocol implementation requires separate
hardware testing of UART, timing, status bursts, Wi-Fi, Home Assistant API and
power measurement before any original TRV16 is reflashed.

Time is supplied as **local time** by the ESPHome time source, without a second
timezone/DST conversion. Without valid time the eight-byte CMD06 reply is zero.
`force_time_sync` and `status_pin` are deliberately rejected in this dialect.

The initial host-only transaction test can be executed with:

```sh
g++ -std=c++17 -Wall -Wextra -Werror -I. tests/tuya_low_power_v0_session_test.cpp -o /tmp/tuya_lp_test
/tmp/tuya_lp_test
```

Evidence: `bavci1988/xr806-lab` under
`reverse-engineering/uart-protocol/trv16-boot-wake/` and
`reverse-engineering/uart-protocol/trv16-state-time-schedule/`.
