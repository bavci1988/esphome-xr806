# XR806: Standard-`wifi:` statt eigener WLAN-Klasse

Stand: 2026-11-09.

## Was sich geändert hat

Der XR806 implementiert jetzt das normale ESPHome-`WiFiComponent`, genauso wie
ESP8266, LibreTiny oder Pico W. `USE_WIFI` ist gesetzt; Scan, Verbindungsaufbau,
Wiederverbindung, `wifi.connected`, Sensoren und `network::is_connected()`
laufen über den Upstream-Code.

| Datei | Rolle |
|---|---|
| `wifi/wifi_component_xr806.cpp` | Plattform-Hooks (`wifi_sta_connect_`, `wifi_scan_start_`, `wifi_loop_`, ...) auf die XRadio-SDK-API (`wlan_sta_*`, `net_ctrl`-Ereignisse) |
| `xr806/wifi_standby.{h,cpp}` | Nur mit `xr806: standby:`: native PM-Wakelock und `NETWORK_BUSY`; mit `network_suspend` wird das WLAN über `wifi::global_wifi_component->disable()/enable()` geparkt |
| `xr806/network_status.{h,cpp}` | Snapshot von Assoziation/Link/IPv4 aus SDK und lwIP |

Entfernt: `xr806: wifi:`, `xr806: wlan_smoke:`, `XR806WiFiComponent`,
`network_bridge`, die Socket-Probes sowie die XR806-Sonderwege in
`wifi/__init__.py`, `network/util.*` und `mdns_component.cpp`.

## Konfiguration

```yaml
xr806:
  board: wxu
  lowpower: true
  wlan:                  # optional, nur XR806-Funkparameter
    dtim: 1              # gilt bei power_save_mode LIGHT/HIGH
    listen_interval: 0
    network_suspend: true  # erfordert xr806.standby
  standby:
    enabled: true
    # ...

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  power_save_mode: light   # Standard auf XR806: none
```

Alte Konfiguration und ihre Entsprechung:

| Alt (`xr806: wifi:`) | Neu |
|---|---|
| `ssid`, `password` | `wifi: ssid/password` |
| `power_save: true` | `wifi: power_save_mode: light` |
| `dtim`, `listen_interval` | `xr806: wlan: dtim/listen_interval` |
| `network_suspend` | `xr806: wlan: network_suspend` |
| `network_bridge` | entfällt (Standardweg über `USE_WIFI`) |
| `socket_probe`, `socket_facade_probe` | entfällt |

## Noch nicht unterstützt

- `ap:` (Fallback-Access-Point, Captive Portal): Das Funkmodul läuft im SDK
  entweder als Station oder als Access Point. Die Validierung lehnt `ap:` ab.
- `manual_ip`: Die Netzwerksteuerung des SDK startet immer DHCP. Die
  Validierung lehnt es ab.
- Grund eines Verbindungsabbruchs und Auth-Modus im Log: werden als
  `UNKNOWN` gemeldet.

## Nachweisstand

Kompiliert und gelinkt mit dem gepinnten SDK `963f6f7` (alle vier
Stage-7b-Konfigurationen plus eine reine WLAN-Konfiguration). **Auf Hardware
nicht getestet**: Scan-Ergebnisse, Verbindungsaufbau mit `wlan_sta_set()`/
`wlan_sta_enable()`, Fehlererkennung über `NET_CTRL_MSG_WLAN_CONNECT_FAILED`
und die DTIM-Einstellung nach DHCP müssen am Board geprüft werden.
