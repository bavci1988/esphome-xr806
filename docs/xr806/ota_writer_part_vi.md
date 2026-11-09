# XR806 / WXU – ESPHome OTA Teil VI (UART-Erstflash, dann OTA)

Status: **schreibfähiger OTA-Backend-Code, erfolgreiche Hosttests; UART- und OTA-Hardwaretests noch offen.**
Aktive Branches: [ESPHome PR #14](https://github.com/bavci1988/esphome-xr806/pull/14) und [SDK PR #5](https://github.com/bavci1988/xr806-sdk/pull/5).

## Scope: ausschließlich ESPHome → ESPHome OTA

**Erster Flash:** `esphome compile` erzeugt `firmware.bin` im XRadio-AWIH-Format. Das vollständige Boot+App-Image wird **kabelgebunden über den XR806-UART-Downloadmodus (RX/TX)** installiert. Das konkrete UART-Flash-Tool muss die AWIH-Containerstruktur und Flash-Offsets korrekt unterstützen; `firmware.bin` darf nicht ungeprüft als rohes Image an Adresse 0 geschrieben werden.

**Danach:** `esphome upload ... --device <ip>` wählt durch den `ota_image`-Hook die separat gebaute `firmware.ota.bin`. ESPHome Native OTA authentifiziert den Upload. Das XR806-Backend ermittelt ROM- und JEDEC-Geometrie und schreibt über `ota_push_start`, `ota_push_data`, `ota_push_finish` real in den XZ-Staging-Slot. Danach erfolgt eine **explizite `image_set_cfg(seq1, VERIFIED)`-Aktualisierung inklusive Readback**. Beim Neustart übernimmt der XRadio-Bootloader die Installation.

**Neue serielle Erstinstallation:** Eine seriell geflashte AWIH-Containerdatei enthält möglicherweise noch keine gültige FDCM-Bootauswahl. Beim *ersten authentifizierten ESPHome-OTA-Vorgang* wird deshalb nach bestätigter Flash-Geometrie und aktivem PM-Wakelock über `image_set_cfg(seq0, VERIFIED)` der neue FDCM-Bereich initialisiert und rückgelesen. Hierzu ist kein Tuya-Firmwaredump und kein zweites Migrationsimage nötig.

Eine **direkte Tuya→ESPHome-OTA-Migration** wird *nicht implementiert und nicht getestet*. Sie ist ausdrücklich außerhalb des Umfangs dieses PR-Paares.

## Testlayout für 2 MiB WXU

| Bereich | Offset | Größe |
|---|---|---:|
| Bootloader | 0x000000–0x007FFF | 32 KiB |
| Aktives APP-Image | 0x008000–0x13EFFF | 1244 KiB |
| FDCM Bootstatus | 0x13F000–0x13FFFF | 4 KiB |
| XZ OTA-Staging | 0x140000–0x1DFFFF | 640 KiB |
| Vorläufig reservierter NV-/RF-Tail | 0x1E0000–0x1FFFFF | 128 KiB |

Die Tail-Reservierung schützt bisher unvollständig identifizierte Daten aus dem originalen WXU-Dump und wird *nicht* für eine Tuya-In-Place-Migration benötigt. Die Reserve bleibt bis zur Klärung der Datenbelegung erhalten. Die Firmware muss aus dem 640-KiB-OTA-Slot passen und darf das aktive 1244-KiB-Fenster nicht überschreiten. Diese Adressen sind konfiguriert; der Runtime-Writer prüft sie anhand der tatsächlichen Hardware.

## Artefakte und Validierung

- `firmware.bin` – vollständige XRadio-AWIH-Firmware für den kabelgebundenen UART-Erstflash.
- `firmware.ota.bin` – gültiges AWIH/XZ-OTA-Image mit 8-KiB-LZMA2-Dictionary; darf nur über OTA übertragen werden.
- `stamp_ota_header.py` – setzt ROM-kompatible OTA-Felder in den Boot-Header des frisch gebauten AWIH-Images. Unpassende Adressen oder eine Überschneidung des reservierten Tail-Bereichs führen zum Fehler.
- `make_ota_xz.py` – konstruiert den XZ-Stream mit CRC32, prüft AWIH-Prüfsummen, die vollständige APP-Segmentkette und das physische Slotlimit.
- **Ein fehlgeschlagenes OTA-Paket lässt den `esphome compile`-Vorgang bei aktiviertem OTA inzwischen fehlschlagen**, statt bloß eine Warnung auszugeben.
- SDK-CI kompiliert ARM-Beispielfirmwares und verlangt zusätzlich ein reproduzierbares `firmware.ota.bin` aus dem OTA-Smoke-Build. Die Host-CI prüft Paket-/Header-/Geometriefehler.

## Noch nicht nachgewiesen

Der Flash und der anschließende Boot über RX/TX sowie ein echter OTA-Zyklus auf einer physischen WXU-MCU sind noch nicht getestet. Es gibt **kein atomisches A/B-Rollback**: Bei der Kompressionspolicy wird das aktive APP beim Neustart aus dem XZ-Slot überschrieben. Stromausfall während der Bootloader-Dekompression kann die Firmware zerstören. Hardware-Readback, Recovery, PM/Wake, WLAN und Tuya-MCU-UART sind deshalb vor einer produktiven Freigabe zu prüfen.
