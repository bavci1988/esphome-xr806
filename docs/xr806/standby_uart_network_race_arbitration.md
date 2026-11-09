# XR806 Stage 8: UART, Netzwerk und Sleep – Interleaving-Vertrag

Stand 2026-11-09. **Development / nur Host- und SDK-Test, keine Flashfreigabe
für den einzigen funktionsfähigen WXU im TRV16.** Die
Plattformkomponenten interpretieren keine Tuya-CMD-/DP-Daten.

## Zuständigkeiten

| Owner | Vorhandene Implementierung | Freigabebedingung |
|---|---|---|
| Tuya MCU low_power_v0 | `tuya_low_power_wire_transactions.h`, `tuya_low_power_v0.cpp` | CMD01/02/09 jeweils maximal eine offene ACK-Transaktion, danach ggf. separate CMD05-Serie, keine offene RX/Write-Schuld, gesunder UART |
| XR806 UART0 | UART-TEMT und native SDK-PM-Wakelock | FIFO + Shiftregister vollständig gesendet, RX abgearbeitet, Ruhefrist, kein sticky fault |
| XR806 PM | `standby_controller.cpp`, `lowpower_policy.h` | Besitzer aller vorhandenen Dienste registriert und inaktiv, unveränderte Generation, atomarer Admission-Fence, Hardware-Wake korrekt eingerichtet |
| Native API | `api_server.cpp` | Kein einziger API-Client; Listener wird geschlossen und bei Abbruch/Resume erneut geöffnet |
| mDNS | `mdns/mdns_xr806.cpp` | Registrierter lwIP-Responder tatsächlich im TCP/IP-Thread abgemeldet; Callback wurde **ausgeführt**, nicht nur eingeplant |
| WLAN | `xr806/wifi_component.cpp` | Explizites `network_suspend: true`, API/mDNS/OTA abgearbeitet, STA deaktiviert und Link/Association inaktiv, native WLAN-PM-Sperre freigegeben |
| OTA | `xr806/standby_ota_guard.h` | **Noch nicht freigabefähig:** OTA-Events werden überwacht, aber keinem bekannten OTA-Backend wird fälschlich ein garantierter Listener-/Flash-Drain zugeschrieben |

**Zusätzliche Build-Grenze:** Der ESPHome-Quellcode in
`esphome/components/esphome/ota/__init__.py` führt XR806
derzeit **nicht** unter den Plattformen mit unterstütztem
`ota: - platform: esphome`-Flash-Backend. Die vorhandene
XR806-OTA-Integration ist daher nur eine vorbereitete,
fail-closed State-Listener-/Veto-Schnittstelle und **keine**
funktionierende XR806-OTA-Update-Implementierung.
Ein später entwickelter XR806-OTA-Backend-Treiber benötigt
vor jeder Schlaf-Freigabe einen eigenen nachgewiesenen
Flash-/Socket-/Listener-Drain.

Die verpflichtende Vollständigkeitsprüfung `XR806StandbyController::may_quiesce()`
verhindert, dass ein vorhandener, aber nicht registrierter Dienst
als *idle* gilt. Es gibt keine generische „alle Netzwerkdienste sind
inaktiv“-Abkürzung. Der konkrete OTA-Guard liefert derzeit
`may_quiesce() == false`; eine Konfiguration mit aktivem `ota:`
blockiert CPU-Standby daher weiterhin.

## MCU-Serialisierung, ACK und Recovery

- CMD01, CMD02 und CMD09 belegen jeweils **einen** exklusiven
  Transaktionsslot; ein CMD05-Report ist kein CMD09-ACK.
- Für CMD02 und CMD09 gilt nur eine leere Antwort der korrekten
  Kommando-ID; für CMD01 ein Produktantwortpaket.
- Nach einem 1.500-ms-ACK-Timeout wird die Transaktion
  **mehrdeutig**. Ohne Sequenznummer im originalen Drahtprotokoll
  kann ein verspätetes ACK nicht vollständig einer neuen, gleichartigen
  Anfrage zugeordnet werden.
- Der Ersatz verwirft deshalb queued Writes statt sie blind erneut
  auszuführen, sperrt die nächste Transaktion und verlangt eine neue,
  gültige CMD01-Produktinfo plus vollständige Statussynchronisation.
- Alle acht DP106-Refresh-Reports und die 17 DP-Reports nach
  CMD02/04 bleiben separate Zustandsbedingungen.
- Schreibzugriffe bei unbekanntem/unsynchronisiertem MCU-Status
  werden **abgelehnt**, nicht unbestätigt für später gespeichert.

Die Serialisierung ersetzt keine Transaktionsnummer, die die
Original-HC32-Firmware nicht besitzt. Auch ein CMD09-ACK allein
beweist weder Sollwertübernahme noch Ventilbewegung.

## Netzwerk-Suspend

1. Die HC32 kann CMD38 senden. Solange UART, MCU-Status, RTC-sm
   oder ein benötigter Netzwerk-Owner unbekannt/busy ist,
   wird `CMD38 [00,01]` zurückgegeben.
2. Mit opt-in `xr806.wifi.network_suspend: true`, passender
   PA20-Konfiguration, validierter MCU-Synchronisation und
   nachweisbar quiesce-fähigen API/mDNS/WLAN-Teilnehmern
   startet ein gesonderter **Service-Preflight**.
3. API schließt seinen Listener ohne einen aktiven Client.
   mDNS wird über `tcpip_callback()` auf dem lwIP-Thread entfernt.
   Erst nachdem API/mDNS (und ein OTA-Owner, sofern überhaupt
   freigabefähig) fertig sind, darf die WLAN-STA herunterfahren.
4. Die Plattform hält den nativen WLAN-Wakelock bis zur bestätigten
   Linktrennung. `NETWORK_BUSY` bleibt vorher gesetzt.
5. **Erst nach vollständigem Drain** kann der Tuya-Treiber
   `CMD38 [00,00]` senden, danach verlangt er physisches
   TX-Draining. CMD12/01 und der eigentliche XR806-PM-Eintritt
   bleiben getrennte, weitere Phasen.
6. Neue UART-RX-Bytes, MCU-Befehle, WLAN-Reconnect, OTA-Arbeit
   oder ein Abort vor dem Eintritt brechen die Freigabe ab und
   stellen API-/mDNS-/WLAN-Ressourcen wieder her.
7. Nach Resume werden Netzwerk-Busy-Votes **vor** Reaktivierung
   gesetzt. UART- und MCU-Synchronisierung müssen erneuert werden.

**Kritische Timing-Grenze:** Der neue CMD38-Preflight wartet
softwareseitig höchstens 1.800 ms auf die Netzwerkfreigabe.
Aus den Dumps ist *nicht* bewiesen, dass die HC32 eine
verzögerte CMD38-Antwort so lange toleriert. Dies erfordert
gezielte RE-/Host-MCU-Emulation oder eine separate elektrische
Validierung, bevor der optionale WLAN-Suspend freigeschaltet
werden darf. Die reine Buildfähigkeit des Flags ist keine
empfohlene Konfiguration für den echten TRV16.

**Aktive Home-Assistant-API-Verbindungen verhindern weiterhin**
den Suspend: der Code trennt keine Nutzerverbindung zwangsweise.
Beim WLAN-Neustart sind DHCP/Cloud/HA-Verbindungen ebenfalls
erneut aufzubauen; deren vollständige real-time Recovery ist
noch nicht auf echter Hardware gemessen.

## PM-Atomizität und Grenzen

`XR806LowPowerPolicy::begin_commit(generation)` beansprucht
genau eine CPU-PM-Admission-Epoche. Jede Owner-Vote-/Request-/
Netzwerk-Quiesce-Änderung bricht diesen Anspruch ab;
`commit_valid(generation)` prüft die unveränderte Generation
vor dem SDK-Aufruf. Der SDK-PM-Call erhält weiterhin nur
`PM_MODE_STANDBY`; Maskenfallback/Treiberfehler dürfen
nicht als erfolgreicher Hardwaremodus ausgegeben werden.

Dies ist eine **atomare Software-Zutrittskontrolle**, keine
globale FreeRTOS/LwIP-IRQ-Transaktion. Asynchrone Hardware-
Producers müssen vor einer neuen Arbeit ihren **eigenen**
nativen SDK-Wakelock halten. UART0 und der optionale WLAN-
Pfad besitzen diese Locks; für die übrigen Netzwerk-/OTA-
und nicht inventarisierten SDK-Producer liegt noch
kein vollständiger Runtime-Nachweis vor.

Daher keine Batterie-Produktionsfreigabe ohne:
Plattformmasken-/IRQ-Trace, UART/WLAN-Wakelock-Traces,
PA20/PB04-Elektrik, DP-/CMD38-Timeout-Emulation,
OTA-Backend-Drain-Nachweis und echte Strom-/Wake-Messungen.

## Regressionsmatrix

`tests/tuya_low_power_wire_transactions_test.cpp`:
CMD01/02/09-Antworten, CMD05/ACK-Interleaving, falscher Typ,
fehlender/verspäteter ACK, 1.500-ms-Timeout, Quarantäne,
Status-Neuinitialisierung und `millis()`-Überlauf.

`tests/xr806/standby_race_network_lifecycle_test.cpp`:
vier Owner-Epochen, echte ACK vs. bloße Quiesce-Anfrage,
Resume/Abort und 120 Permutationen von
API-Abmeldung, mDNS-Drain, WLAN-Stop, UART-RX und finalem
PM-Versuch. Die Tests prüfen das reine Zustandsmodell;
sie simulieren keine physischen lwIP-ISR/SDK-ROM-Treiber.

Das private gepinnte SDK-CI prüft zusätzlich den vollständigen
WLAN/API/mDNS/Tuya-Cross-Build. Ein erfolgreiches ARM-Linken
ist kein Laufzeittest des Hardware-Suspend-Lifecycle.
