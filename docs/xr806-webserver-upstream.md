# XR806: upstream-compatible `web_server:` / `captive_portal:` roadmap

Status: implementation in progress on [ESPHome PR #15](https://github.com/bavci1988/esphome-xr806/pull/15) and [SDK PR #6](https://github.com/bavci1988/xr806-sdk/pull/6). Neither is production tested.

## Compatibility rules

1. **Do not fork public ESPHome YAML syntax.** The target configuration is ordinary
   `wifi.ap:`, `captive_portal:`, `web_server:` and (separately) `ota:`.
   The existing `xr806.http_server:` must remain temporary and be removed
   before claiming normal `web_server:` support.
2. **Do not expose a reduced HTTP status page as `web_server:`.** The standard
   component provides entity REST routes, configurable index resources,
   authentication, event-source updates and shared web-server ownership.
3. Preserve portable C++ paths, feature-gate optional code and keep vendor APIs
   behind an XR806-only backend. In particular, do not introduce XRadio headers
   into generic `web_server`, `captive_portal`, `wifi` or `socket` files.
4. `web_server_base` must remain a *single* owner of HTTP port 80 shared by
   `web_server` and `captive_portal`, with a correct init/deinit refcount.
   The temporary native captive implementation does not yet support concurrent
   `web_server:`, and must be replaced when the shared backend is ready.
5. Do not silently disable normal security semantics. Authentication, allowed
   origins, HTTP method checks, request-size limits, credential redaction,
   cancellation and upload denial must have explicit support or validation errors.
6. No sleep while network sockets are live or unowned. Existing validation rejects
   guarded standby with either XR806 HTTP backend.

## Transport-backend contract to implement next

A new platform-specific `web_server_xr806` backend should implement the
AsyncWebServer-compatible interfaces used by `web_server_base`:

- Asynchronous/nonblocking TCP listener: accept, read, incremental header/body
  parsing, keep-alive policy, bounded max clients, idle timeout and shutdown.
- Request and response APIs: `method()`, `url()`, `arg()`, `hasArg()`,
  `getHeader()`, `beginResponse()`, `beginResponseStream()`, `send()`,
  `redirect()`, binary data and HTTP headers.
- Handler registration and dispatch with `addHandler`, `onNotFound`,
  authentication and origin guard via the **existing** `web_server_base`.
- Server-sent events `/events`: nonblocking, bounded per-session queues,
  initial configuration ping, entity enumeration, state updates and close handling.
- Captive DNS via the shared `web_server_base::CaptiveDNS` abstraction (not
  another UDP/53 listener). Existing `xr806/captive_dns_packet.h` is reusable.
- OTA web upload support must be a separately gated feature once the XR806
  OTA writer, multipart handling, preflight and standby gate are proven.
  Native ESPHome OTA on its own port must remain unaffected.

Upstream files that should require **small platform-gated additions only**:
`web_server_base/__init__.py`, `web_server_base/web_server_base.h`,
`web_server_base/captive_dns.h`, `web_server/__init__.py`,
`web_server/web_server.h`, `web_server/list_entities.h`, and
`captive_portal/__init__.py`. Shared REST entity handlers should be left
unmodified. The XR806-specific implementation belongs entirely in
`web_server_xr806/` and `xr806/`.

## Verification gates

| Area | Gate |
|---|---|
| YAML | Standard blocks validate; unsupported HTTP features produce explicit errors |
| Build | Generated ARM C++20 firmware links the exact pinned SDK without Arduino/ESP-IDF |
| HTTP | GET/POST/OPTIONS, request parsing, large binary response, multiple probes and backpressure |
| API | REST entity read/control, SSE initial entity list, state changes and reconnections |
| SoftAP | AP association, DHCP offer/ack/router/DNS, iOS/Android captive detection |
| Credentials | Flash-backed preferences with reset/power-loss/corruption recovery |
| PM | AP, web sockets, OTA and network votes prevent standby until quiesced |
| Security | Auth/origin checks, fuzzed DNS/HTTP inputs, no secrets in logs |
| Upstream CI | clang-format, ruff, pylint, platform compile tests, portable unit tests |
| Hardware | Tested on separate WXU module before installation in the TRV16 |

## Current implemented subset

The standard `captive_portal:` YAML is wired to a native XR806 HTTP/DNS
implementation, serves the upstream compressed captive portal index, responds
to `/config.json` and accepts `/wifisave`. XR806 SoftAP is exclusive
with STA, so scan-result listing currently returns no AP entries; users must
enter SSIDs manually. WLAN credentials are **not yet persistent**. The normal
`web_server:` component remains disabled on XR806 until the shared backend
and SSE/REST contract above are implemented and verified.
