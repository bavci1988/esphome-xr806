# TRV16: RTC-Schlaffenster, Produktinfo-Recovery und PM-Abbruch

Stand: 2026-11-09. Implementierungsvertrag für die neuen Befunde im
privaten `bavci1988/xr806-lab`:
- `reverse-engineering/uart-protocol/trv16-sleep-window/README.md`
- `reverse-engineering/xr806/pm-veto-paths/README.md`
- `docs/trv16-re-evidence-gaps.md`

**Wichtig:** Dump-/Unicorn-Ausführung der Originalroutinen, SDK-Vergleich
und ESPHome-Ersatzimplementierung sind unterschiedliche Evidenzstufen.
Alle neuen Tests sind Softwaretests; es liegt weiterhin keine elektrische
Wake-, Stromverbrauchs- oder Resetmessung an der TRV16-Platine vor.

## 1. MCU-Zuständigkeit: DP101/102 und dynamisches sm

| DP | Format | HC32-Wirkung | Ersatzimplementierung |
|---|---|---|---|
| 101 | BOOL, genau 1 Byte mit 0/1 | Dormant-/Schlafzeitfenster aktivieren | Typ, Wert und Länge vor CMD09 sowie CMD05 prüfen |
| 102 | RAW, genau 4 Byte `[Start-H, Start-M, Ende-H, Ende-M]` | Tägliches Schlaffenster; kein Heizprogramm | Länge 4, Stunde 0..23, Minute 0..59 vor TX/RX prüfen |
| 107 | RAW, genau 32 Byte; achtmal `[H, M, Temperatur_BE16]` | Separates Heiz-Wochenprogramm | Länge und Zeitfelder validieren; **keine unbelegte Temperaturspanne** |

Die vier DP102-Bytes sind binär, nicht BCD; `16 1E 06 2D` bedeutet
22:30–06:45. Der HC32-RTC-Service verwendet
`Start <= aktuelle Zeit < Ende`; für ein Mitternacht überschreitendes
Fenster `Zeit >= Start || Zeit < Ende`. Gleiche Grenzen bedeuten ein
leeres Fenster. Ungültige RTC-Zeit lässt den bisherigen MCU-Merker
unverändert. Der pure Host-Test führt 3 × 1440 Minuten durch.

Der **HC32**-RTC-Pfad berechnet den aktiven RAM-Merker `0x2000003B`
und verwendet dabei die gültige Zeit in `0x2000003C`. Die frisch
erzeugte CMD01-Produktinfo meldet `sm=0` nur bei beiden Merkern
ungleich null, sonst `sm=1`. **Der WXU berechnet sm nicht selbst.**

Die Originalroutine verändert den aktiven Zustand nicht schon beim
DP101/102-Write, sondern erst bei späteren RTC-Ereignissen. Bei einem
Wechsel erzeugt die HC32 einen PA3-Low/Delay/High-Aufruf und setzt
ihren Netzwerkstatus auf FF. Die elektrische Wirkung von PA3
auf WXU-CEN/Reset ist ungeklärt; der Ersatz nimmt **keinen sicheren
Reset** an.

### Bewusste Sicherheitsentscheidung im ESPHome-Ersatz

`TuyaLowPowerSleepWindow` speichert die *beobachteten* MCU-Reports,
nicht einen spekulativ berechneten RTC-Merker. Nach DP101-/DP102-Schreibzugriff,
geändertem Report oder neuem MCU-Heartbeat ist der zwischengespeicherte
CMD01-`sm`-Wert als möglicherweise veraltet markiert. Nach Abschluss
der offenen UART-/DP-Transaktionen fordert der MCU-Treiber CMD01 erneut
an, verlangt danach erneut die 17-DP-Statusserie und blockiert
zwischenzeitlich Standby.

**Solange DP101 aktiviert ist, wird retained Standby vorsorglich
nicht freigegeben**, auch falls die zuletzt empfangene Produktinfo
`sm=1` meldet. Die HC32 kann den Zeitfenstermerker minutenabhängig
ändern, ohne dass wir einen garantiert anliegenden Hardware-Reset
nachweisen können. Das ist eine *konservative Ersatzentscheidung*,
keine behauptete originale WXU-Regel. Nach deaktiviertem DP101,
neuer gültiger CMD01-Produktinfo und vollständiger Synchronisation
kann die normale Standby-Policy wieder greifen. `sm=0` bleibt als
nicht implementierte Hibernation gesperrt.

## 2. Fehlerzweige von CMD01

Original-WXU-Produktinfo-Handler `0x50DE54`:
Null-Descriptor (-2), Allokation (-3), cJSON-Fehler (-8), fehlendes
`p`/`v` (-2) rufen die 500-ms-Verzögerung und eine neue
CMD01-Abfrage auf. Das Original prüft dagegen die unmittelbaren
Rückgaberegister von LP-/IoT-Initialisierung nicht; ein gesetztes
Init-Merkmal beweist also **keine** erfolgreiche Inbetriebnahme.

Der Ersatz trennt:
- **malformed/recoverable:** Null/fehlerhafte Produktmetadaten
  → nonblocking CMD01-Retry alle 500 ms, **höchstens fünf Anfragen**
  (lokales Sicherheitslimit, nicht aus dem Original als globaler
  Höchstwert bewiesen). Die rohe Produktinfo wird in einem festen
  513-Byte-Diagnosepuffer abgelegt; eine *zusätzliche* dynamische
  Deskriptorallokation samt Fehlerrisiko entfällt. Das schließt
  nicht alle möglichen Heapfehler im allgemeinen JSON-/String-Parser aus.
- **fremdes Produkt/wrong pins:** JSON syntaktisch gültig,
  aber PID/`wk`/`mwk` abweichend → endgültige Ablehnung.
- **gültiges Produkt:** separate UART-Gesundheit, MCU-Statusserie und
  Netzwerk-/API-/OTA-Sperren bleiben erforderlich. Der Ersatz
  erkennt eine gültige Produktinfo nicht als alleinigen LP-/IoT-Erfolg
  an.

Die Original-RTC-Routine setzt nach dem **3601. bedienten** Ereignis
bei Netzwerkstatus 4 erneut einen Zeit-Anforderungsmerker. Im
inaktiven Zeitfenster führt dies zu CMD06; im aktiven kann der
Merker ohne Anforderung gelöscht werden. Der WXU beantwortet
wie bisher **jede tatsächlich empfangene** CMD06-Anfrage; er
erfindet keinen stündlichen Timer aus der Ereigniszahl.

## 3. PM-Masken und Rückabwicklung

Aus den 54 Original-WXU-Task-Szenarien:
- Eine Plattformmaske kann eine Anforderung nur **abwärts**
  herabstufen (HIBERNATION3 → STANDBY2 → SLEEP1 → ON0).
- Kein zulässiger Modus, ungeeignete EINT-Wake-Konfiguration oder
  ein negativer Treiber-Suspend führt zum Abbruch. Es gibt im
  geprüften Originaltask **keinen automatischen flacheren Retry**
  nach negativem Suspend.
- `PMA: appos enter mode`, ein CMD12/01 und selbst ein
  `pm_enter_mode_timeout()==0` sind **keine Hardware-Sleep-Beweise**.
- Ein Pending-Wake im Vergleichs-ROM kann HIBERNATION sogar vor
  dem eigentlichen Eintritt rebooten. Ein Reboot beweist daher
  keine vorherige Hibernation.
- Der originale UART-Suspend-Callback wartet nicht selbst auf
  abgeschlossenes TX; der ESPHome-UART0-TEMT-Schutz und die
  native PM-Wakelock-/Transaktions-Sperre bleiben nötig.

Die Plattformimplementierung bleibt absichtlich fail-closed:
`PM_MODE_STANDBY` wird explizit angefordert, ein negativer Rückgabewert
nicht stillschweigend in SLEEP umgedeutet. Die aktive Hardwaremaske,
proprietären Treibervetos und der tatsächlich erreichte Modus sind
auf dem realen Modul noch **nicht** festgestellt; `pm_get_mode()`
belegt laut SDK-Vergleich nur den internen PM-Zustand und ist kein
Messgerät für den erreichten Energiezustand.

## 4. Nachweise und offene Punkte

- `tests/tuya_low_power_sleep_window_test.cpp`: 4320 RTC-Minuten,
  Grenzen, Mitternacht, ungültige Uhr, DP101/102 und DP107,
  Produkt-Epochen-Sperren sowie 500-ms-Retry-/Überlaufgrenzen.
- `tests/tuya_low_power_v0_session_test.cpp`: zusätzliche
  Produktinfo-Fehler (fehlendes p/v, Typfehler, sm2, null).
- `.github/workflows/tuya-low-power-v0-ci.yml`: Host- und
  XR806-YAML-Codegen-Regression.
- Private SDK-CI: echtes ARM-Linken/AWIH-Paket aller Profile
  erforderlich; Testresultat ist kein UART-Pin-/Sleep-Nachweis.

Offen: vollständiges UART-/Netzwerk-/Sleep-Interleaving,
interne LP-/IoT-Teilfehler, Flash-Persistenz und beschädigte
Records, tatsächliche PA3/PA20/PB04-Leitungen, Wake-Pegel,
SoC-Wakelocks mit Funklast und durchgehender Sleep-/Wake-Batterietest.
**Kein Flashen des einzigen funktionsfähigen WXU nur aufgrund eines
erfolgreichen Software-Builds.**
