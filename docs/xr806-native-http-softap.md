# Native XR806 HTTP + SoftAP + captive DNS (experimental)

This is an **opt-in bring-up implementation**, not ESPHome's normal
`web_server:` or `captive_portal:` integration. The existing ESPHome
AsyncWebServer backend requires Arduino/ESP-IDF and has not been ported to XR806.

## Firmware example

```yaml
esphome:
  name: xr806-http-lab

xr806:
  board: wxu
  http_server:
    port: 80
    captive_dns: true

wifi:
  ap:
    ssid: "XR806-Lab"
    password: "xr806test123"
```

Avoid enabling `xr806.standby:` with this experimental server. The YAML
validator rejects that combination because socket-drain and PM votes are not
implemented. The AP is exclusive: it **cannot** operate alongside a station
association. In fallback AP mode the WiFi component cycles back to STA to try
new credentials.

## Architecture

- `wifi_component_xr806.cpp`: exclusive STA ↔ SoftAP radio switching through
  XRadio `net_switch_mode()`, `wlan_ap_set()`, `wlan_ap_enable()`,
  `wlan_ap_disable()` and `wlan_ap_sta_num()`.
- XRadio vendor `net_ctrl.c`: DHCP server starts on the AP network-up event.
  Patched `udhcpd` now supplies 5 addresses in the AP subnet (normally
  `192.168.176.101` through `192.168.176.105`). DHCP advertises the AP
  interface itself as the DNS server.
- Vendor DHCP's own DNS listener is disabled to avoid competing for UDP/53.
- `xr806/http_portal.cpp`: bounded, nonblocking native lwIP socket server
  with GET `/health`, an HTML Wi-Fi form and POST `/wifisave`.
  Responds with the form to arbitrary GET paths while SoftAP is up so phone
  OS connectivity checks can discover the captive UI.
- `xr806/captive_dns_packet.h`: minimal RFC1035 A record reply for
  **IN/A** queries, pointing to the actual AP IPv4. Other types return NODATA.
  Compression pointers in questions, malformed DNS and multiple questions
  are deliberately rejected.
- The server is restricted to one concurrent HTTP client, a 1024-byte request,
  2304-byte response, 512-byte form and 5-second per-client deadline.

## Bring-up checks

1. Build from the `feature/xr806-native-http-softap-dns` ESPHome branch.
   The dependency is pinned to XRadio SDK revision
   `495ac30bbb4e8df7699051318e3fb451969a1c61` in the matching SDK fork.
   If an earlier `xr806/sdk-work` directory already exists, check its revision
   before replacing it with the fresh pinned dependency.
2. Confirm on an isolated XR806/WXU test device that the AP is visible and
   the phone receives a DHCP lease, gateway and DNS server.
3. Connect the phone, then request `http://192.168.176.1/health`.
   Expect `{"ok":true,"platform":"xr806"}`.
4. Visit `http://192.168.176.1/`, or make an arbitrary DNS A request
   through the AP resolver. Confirm captive DNS points to the AP IP.
5. POST a valid SSID/password. Check the response is sent before
   fallback AP stops and STA reconnect begins; repeat with interrupted
   uploads, invalid forms and wrong passwords.
6. Measure RAM, file descriptors, task stalls, HTTP retries, AP transitions
   and watchdog behavior. Validate on mobile OS captive probes.

## Known limits / NOT production-ready

- **No persistent Wi-Fi provisioning**: XR806 `preferences.cpp` is currently
  the non-persistent placeholder; credentials are applied for the current
  runtime but not retained over reset. Do not describe this as a complete
  provisioning solution until flash-backed preferences are implemented and
  validated with power interruption tests.
- **No ESPHome standard `web_server:`** and **no standard
  `captive_portal:`** API: the component is configured as
  `xr806.http_server:`. It does not expose ESPHome entities, websockets,
  streaming events, authentication or web OTA.
- AP-only/exclusive network behavior; scanning while the AP is running is
  not supported. The form takes an SSID manually.
- Only IPv4 A captive responses and HTTP (no HTTPS, IPv6, EDNS0 or
  encrypted transport). Use a WPA2-protected AP, not an open commissioning AP.
- DNS is not authoritative for real upstream names; the captive DNS listener
  runs only while SoftAP is up.
- Cannot enter guarded standby with HTTP/DNS active; other PM paths and
  runtime event races require hardware verification.
- Native cross-compile, real XR806 network initialization and smartphone
  portal negotiation **have not yet been verified**.

## Portable DNS smoke test

From repository root:

```sh
g++ -std=c++20 -Wall -Wextra -Werror -I. \
  tests/unit_tests/xr806_captive_dns_smoke.cpp -o /tmp/xr806_dns_test
/tmp/xr806_dns_test
```

The DNS packet helper is host-portable and has no SDK dependencies.
