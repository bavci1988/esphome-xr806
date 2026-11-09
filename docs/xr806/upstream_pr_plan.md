# XR806: Plan für die Einreichung bei ESPHome

Stand: 2026-11-09. Basis: `esphome/esphome@cb57120` (dev vom 2026-11-07).

Upstream verlangt, dass Änderungen an geteiltem Code (`esphome/core/`, Bus- und
Netzwerkkomponenten) nicht in einem Komponenten-PR versteckt werden. Ein einziger
PR mit Plattform, Tuya-Low-Power, Standby und Kern-Hooks würde abgelehnt. Die
folgende Reihenfolge ergibt kleine PRs, die jeweils für sich bauen.

## Reihenfolge

| # | Titel | Inhalt | Hängt ab von |
|---|---|---|---|
| 1 | `[xr806] Add XR806 platform` | `components/xr806/` ohne Standby (Plattform, Boards, GPIO, HAL, Helpers, Preferences, Build-Adapter), `Platform.XR806`/`Framework.XRADIO`/`PlatformFramework.XR806_XRADIO`, `CORE.is_xr806`, die `#elif defined(USE_XR806)`-Zeilen in `core/` (hal, gpio, gpio_pin, main_task, wake, helpers, application), `wake_freertos.cpp`-Filter, `ci-custom.py`-Ausnahmen für `hal.cpp`/`helpers.cpp` | – |
| 2 | `[uart] Add XR806 UART backend` | `uart_component_xr806.*`, Klasse/Filter in `uart/__init__.py`; ohne die zwei XR806-Methoden in `uart_component.h` | 1 |
| 3 | `[wifi] Add XR806 WiFi backend` | `wifi_component_xr806.cpp`, Validierung/Filter in `wifi/__init__.py`, Deklaration in `wifi_component.h`, lwIP-Include in `network/ip_address.h`, `socket`-/`api`-Standardwerte, `socket_compat.*`, `network_status.*` | 1 |
| 4 | `[mdns] Add XR806 mDNS backend` | `mdns_xr806.cpp` (ohne Standby), Plattform-TXT in `mdns_component.cpp`, Filter | 3 |
| 5 | `[xr806] Add standby controller` | `lowpower_policy.h`, `standby_*`, `wifi_standby.*`, `api_standby.*`, mDNS-Standby-Teilnehmer, OTA-Guard, Host-Tests | 1–4 |
| 6 | `[tuya] Add low_power_v0 protocol` | `tuya_low_power_*`, Änderungen an `tuya.*`/`__init__.py`, Tests; die XR806-Teile (`set_baud_probe_mode`, PB04) als eigener Folge-PR | 2, 5 |

Die zwei XR806-Methoden in `uart/uart_component.h` (`reconfigure_baud_rate`,
`set_baud_probe_mode`) gehören in PR 6 oder müssen vorher durch die Upstream-API
`set_baud_rate()` + `load_settings()` ersetzt werden. Dafür bräuchte
`load_settings()` einen Rückgabewert, was eine Kernänderung wäre.

## Was Upstream zusätzlich verlangen wird

- **Öffentliches, reproduzierbares SDK.** Der Build hängt am privaten
  `bavci1988/xr806-sdk` und am Make-System des Herstellers. Upstream baut mit
  PlatformIO oder ESP-IDF/CMake. Ohne öffentlich verfügbares SDK (oder ein
  PlatformIO-Paket) kann die Upstream-CI die Plattform nicht bauen. Das ist
  der größte Brocken.
- **Lizenzklärung** der Hersteller-SDK-Teile (siehe Hinweis im xr806-lab).
- **Komponententests** nach `tests/components/<komponente>/test.xr806-xradio.yaml`
  und eine Basis-Konfiguration `tests/test_build_components/build_components_base.xr806-xradio.yaml`.
- **Dokumentation** auf esphome.io (Plattformseite, Board WXU, Einschränkungen).
- **Hardwarenachweis** für Boot, WLAN, UART und Standby; bisher ist alles nur
  kompiliert und gelinkt.
- `CODEOWNERS`-Einträge für die neuen Dateien.

## Bereits bereinigt (2026-11-09)

- Tuya-Low-Power nur mit `USE_TUYA_LOW_POWER_V0`; Standard-Tuya-Builds
  unverändert.
- Standard-`WiFiComponent` statt eigener Klasse.
- Keine XR806-Logik mehr in `api_server.*`, `mdns_component.h`,
  `network/util.*`, `gpio/switch`, `gpio_binary_sensor.cpp`, `uart.h`.
- UART-Pin-Schema wieder wie Upstream; XR806-Regeln in der abschließenden
  Validierung der Plattform.
- `ci-custom` grün, Kommentare englisch, Scheduler-IDs numerisch.
