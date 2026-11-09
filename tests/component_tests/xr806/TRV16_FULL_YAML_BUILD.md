# TRV16 WXU full YAML firmware test

Test branch: `test/trv16-wxu-full-build-20261109` (do not merge until XR806 supports all requested components).

## Source and credentials

- Public platform source: `bavci1988/esphome-xr806`, branch `test/trv16-wxu-full-build-20261109` (base: `dev`).
- Complete unchanged user YAML including Wi-Fi, Home Assistant API encryption and OTA password is kept **only in the private** `bavci1988/xr806-sdk` repository, on the corresponding test branch, at `ci/trv16-wxu-full.yaml`.
- Private build workflow: `bavci1988/xr806-sdk/.github/workflows/trv16-wxu-full-build.yml`. It checks out the public source branch, validates the full YAML, then tries to cross-compile an AWIH wired image and OTA XZ image. It never silently removes requested components or substitutes dummy credentials.
- Do **not** move the full YAML into the public port repository or upload logs with secrets to public issues.

## Blocking platform gaps (as checked on the dev source)

1. `web_server` is guarded by `cv.only_on([...])`, excluding XR806. More than a schema change is needed: `web_server_base.h` uses ESP-IDF's server on ESP32, and `ESPAsyncWebServer.h` on the other platforms. Neither backend is implemented for native XRadio.
2. `captive_portal` is also blocked by `cv.only_on([...])`. Its HTTP/captive DNS implementation depends on the missing server backend.
3. The given YAML does not enable `wifi.ap`. Moreover the XR806 Wi-Fi implementation explicitly rejects access-point mode; `wifi_component_xr806.cpp` is currently STA-only. A functional captive portal therefore also requires a real XR806 SoftAP backend.
4. The YAML includes `xr806.standby.wake_pin: PA20` with `wake_edge: falling`. This is **not yet confirmed electrically** on the TRV16 device. PB04 MCU wake is intentionally not activated.
5. Some DP semantics/enum mappings remain hypotheses; a compiler cannot validate hardware correctness.

A bare branch/SHA or an ESPHome YAML parse pass is **not** evidence of a flashable product firmware. Require successful code generation, native ARM link, a validated AWIH binary, an OTA image and real hardware tests. Do not bypass platform guards to claim feature support.

## Full YAML domains

`esphome`, `xr806`, `web_server`, `captive_portal`, `mdns`, `api`, `ota`, `wifi`, `time`, `uart`, `tuya`, `climate`, `switch`, `number`, `select`, `sensor`, `binary_sensor`, `text_sensor`, `button`.

Configured DPs: 1, 2, 3, 4, 7, 10, 11, 16, 17, 19, 20, 101, 102, 104, 105, 107, 108, plus the `tuya.refresh` trigger for DP106.
