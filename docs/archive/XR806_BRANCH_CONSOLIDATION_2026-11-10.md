# XR806 legacy branch reconciliation — 2026-11-10

Default branch: `dev`. Keep `Convert-tuya2esphome-ota` separate.

The older native HTTP/SoftAP branch is retained in the **Git commit ancestry**, not overlaid over the newer implementation. Its design is superseded by PR #16's standard `web_server:` and `captive_portal:` support. The historical documentation is in this folder.

## Historical branch

- Original head: [`67c4d11a5da339ab295151dce30c9d5507cedbfc`](https://github.com/bavci1988/esphome-xr806/commit/67c4d11a5da339ab295151dce30c9d5507cedbfc) — `feature/xr806-native-http-softap-dns`.
- Original draft: [PR #15](https://github.com/bavci1988/esphome-xr806/pull/15), closed as superseded.
- Current standard backend: [PR #16](https://github.com/bavci1988/esphome-xr806/pull/16) merged to `dev`.
- Reconciliation merge uses Git's **ours** resolution for overlapping runtime files so an earlier `xr806.http_server` cannot silently replace `web_server_xr806`.

## Work intentionally deferred, preserved in ancestry

1. `esphome/components/xr806/preferences.cpp`, `preferences.h`, `preference_backend.h`, `preference_snapshot.h` and associated `core/preferences.h`, `core/preference_backend.h` changes implement a dual-sector CRC/FDCM preference journal in the former branch. This code **is not enabled in `dev`**. The verified-erased 1972–1984 KiB tail window and foreign-data checks are firmware-specific; a hardware-backed write/erase/recovery test on a spare WXU is required before activating it.
2. The old `esphome/components/xr806/http_portal.*`, `http_request_parser.h` and `captive_dns_packet.h` implement a parallel diagnostic HTTP portal. These are superseded, not reintroduced.
3. Old standalone tests and the dedicated native-build workflow remain recoverable through the legacy merge parent, but have not been enabled as active `dev` checks. The canonical XR806 ARM-Web workflow and multipart tests are already on `dev`.

## Branch cleanup constraints

Other feature heads that are fully behind `dev` are already integrated. Branch refs can be deleted after confirming a zero-ahead comparison with `dev`. This repository's `delete_branch_on_merge` setting is disabled. The available GitHub integration cannot delete refs, so deletion requires repository-owner access in GitHub or an authenticated local `git push origin --delete` operation.

The protected exception `Convert-tuya2esphome-ota` must **not** be deleted or force-updated.
