# XR806 / WXU – ESPHome OTA Teil VI (UART-Erstflash, dann OTA)

Status: **schreibfähiger OTA-Backend-Code, erfolgreiche Hosttests; UART- und OTA-Hardwaretests noch offen.**
Aktive Branches: [ESPHome PR #14](https://github.com/bavci1988/esphome-xr806/pull/14) und [SDK PR #5](https://github.com/bavci1988/xr806-sdk/pull/5).

## Scope: ausschließlich ESPHome → ESPHome OTA

**Erster Flash:** `esphome compile` erzeugt `firmware.bin` im XRadio-AWIH-Format. Das vollständige Boot+App-Image wird **kabelgebunden über den XR806-UART-Downloadmodus (RX/TX)** installiert. Das konkrete UART-Flash-Tool muss die AWIH-Containerstruktur und Flash-Offsets korrekt unterstützen; `firmware.bin` darf nicht ungeprüft als rohes Image an Adresse 0 geschrieben werden.

**Danach:** `esphome upload ... --device <ip>` wählt durch den `ota_image`-Hook die separat gebaute `firmware.ota.bin`. ESPHome Native OTA authentifiziert den Upload. Das XR806-Backend ermittelt ROM- und JEDEC-Geometrie und schreibt über `ota_push_start`, `ota_push_data`, `ota_push_finish` real in den XZ-Staging-Slot. Vor dem Commit entpackt das Backend den Slot einmal komplett mit dem ROM-XZ-Decoder (gleicher Decoder und gleiche 8-KiB-Dictionary-Grenze wie der Bootloader) und prüft die entpackten Bytes Sektion für Sektion: AWIH-Kopf, Kopf- und Datenprüfsumme, `next_addr`-Kette und Größe ≤ aktives Fenster. Erst danach ruft es `ota_push_finish()` auf. Diese SDK-Funktion schreibt über `ota_verify_image()` selbst **FDCM seq1 = VERIFIED** – das ist der eigentliche Commit; das Backend liest den Eintrag nur zurück. Ab diesem Punkt bleiben Update-Sperre und Wakelock bis zum Neustart gehalten, auch wenn der Readback fehlschlägt. Beim Neustart übernimmt der XRadio-Bootloader die Installation.

**Warum die Probe-Entpackung nötig ist:** Der Bootloader (`bl_xz_image`) löscht seq0 vollständig, bevor er entpackt. Bei einem XZ-Fehler bricht er nur die Schleife ab, prüft danach lediglich den komprimierten Wrapper in seq1 und setzt seq0 trotzdem auf VERIFIED. Ein Datenstrom, der den Wrapper-Check besteht, aber nicht sauber entpackt, würde so ein halbes Image starten; dann hilft nur noch der UART-Flash. Ein Stromausfall *während* des Entpackens ist dagegen unkritisch: FDCM zeigt weiter auf seq1, und der Bootloader beginnt beim nächsten Start von vorn.

**Neue serielle Erstinstallation:** Nach einem UART-Flash ist der FDCM-Sektor leer. `image_get_cfg()` meldet dann einen Fehler, und ROM sowie Bootloader starten seq0. Das Backend behandelt das als „seq0 verifiziert“ und schreibt vorab **nichts** in FDCM; der erste erfolgreiche `ota_push_finish()` legt den FDCM-Kopf selbst an. Ein vorhandener, aber anderer Eintrag (z. B. seq1 wartet noch auf den Bootloader) blockiert jeden weiteren Upload. Hierzu ist kein Tuya-Firmwaredump und kein zweites Migrationsimage nötig.

**Layoutänderungen nur per UART:** Die Geometrie (FDCM-Adresse, Slotgrößen) steht im Boot-Header. Ein OTA-Update tauscht nur die App aus, nie den Bootloader; ein geändertes `image.cfg` muss deshalb per UART geflasht werden.

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
- `stamp_ota_header.py` – setzt ROM-kompatible OTA-Felder in den Boot-Header des frisch gebauten AWIH-Images. Unpassende Adressen oder eine Überschneidung des reservierten Tail-Bereichs führen zum Fehler. Der Make-Adapter lässt `mkimage` in eine Zwischendatei schreiben und erzeugt `firmware.bin` erst nach erfolgreichem Stempeln; ein abgebrochener Build hinterlässt kein ungestempeltes Image.
- `ota_writer_image.h` – Prüfung des 70-Byte-Vorspanns vor dem Löschen des Slots (`validate_xz_preface`) und Sektionsprüfung des entpackten Images (`DecompressedImageCheck`); beide mit Host-Tests in `tests/xr806/ota_writer_part_vi_test.cpp`.
- `make_ota_xz.py` – konstruiert den XZ-Stream mit CRC32, prüft AWIH-Prüfsummen, die vollständige APP-Segmentkette und das physische Slotlimit.
- **Ein fehlgeschlagenes OTA-Paket lässt den `esphome compile`-Vorgang bei aktiviertem OTA inzwischen fehlschlagen**, statt bloß eine Warnung auszugeben.
- SDK-CI kompiliert ARM-Beispielfirmwares und verlangt zusätzlich ein reproduzierbares `firmware.ota.bin` aus dem OTA-Smoke-Build. Die Host-CI prüft Paket-/Header-/Geometriefehler.

## Noch nicht nachgewiesen

Der Flash und der anschließende Boot über RX/TX sowie ein echter OTA-Zyklus auf einer physischen WXU-MCU sind noch nicht getestet. Es gibt **kein atomisches A/B-Rollback**: Bei der Kompressionspolicy wird das aktive APP beim Neustart aus dem XZ-Slot überschrieben. Ein Stromausfall während der Bootloader-Dekompression führt zu einem erneuten Entpackversuch; ein Defekt des XZ-Slots *nach* der Probe-Entpackung (Flash-Fehler) würde dagegen ein halbes Image starten. Hardware-Readback, Recovery, PM/Wake, WLAN und Tuya-MCU-UART sind deshalb vor einer produktiven Freigabe zu prüfen.
