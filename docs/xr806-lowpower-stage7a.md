# XR806 stage 7a: MCU-independent WLAN power save and standby policy

## Policy boundary

A WXU/XR806 may have **no external MCU**, an HC32L136, or another
controller. No MCU protocol and no dedicated external wake pin is required
by the XR806 platform. A future optional MCU component calls
`require_external_mcu(true)` during registration, and must explicitly clear
`MCU_UNSYNCED` after its own synchronization. Removing the MCU requirement
immediately removes that specific gate.

`xr806.lowpower: true` exposes the fail-closed system-standby policy. **It
does not enter CPU standby.** The policy is blocked by default until UART,
network, API, OTA, mDNS, wake source and scheduler are independently verified.
No `pm_enter_mode()` or `PM_MODE_HIBERNATION` call is performed in this stage.

## Radio power-save

Opt in independently of CPU standby via the standard ESPHome WiFi backend:

```yaml
xr806:
  board: wxu
  lowpower: true
  wlan:
    power_save: true
    dtim: 3
    listen_interval: 8

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_encryption_key
```

Radio settings are only applied **after WLAN and DHCP are ready**; a failed
driver command is retried after 30 seconds and on a new association. `power_save:
false` remains the safe default. WLAN power-save is separate from CPU
standby/hibernation and may increase API latency.

## What stage 7b must still implement

- Hardware wake source selected via a **board-specific** config, not
  hard-coded HC32/PA20.
- UART TX-complete and optional MCU handshake, if an external MCU exists.
- API/OTA/mDNS network wake-lock and scheduler activity tracking.
- Runtime wake reason, resume validation and measured current.
- No automatic CPU standby on battery-powered WXU until runtime gating is
  instrumented and tested on hardware.
