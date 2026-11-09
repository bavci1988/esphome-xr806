# XR806 OTA development – no-write Stage A (2026-11-09)

## Status and why it is fail-closed

This branch is exploratory. **It does not enable ESPHome `ota: - platform: esphome` on XR806.** That platform is rejected in `esphome/components/esphome/ota/__init__.py` until a compatible XR806 `OTABackend` exists. The existing `XR806OTAStandbyGuard` always retains `OTA_BUSY` when an OTA component is configured. It would be unsafe to interpret OTA progress callbacks as proof that listening sockets and flash writes have stopped.

## Evidence from the uploaded XR806 vendor package

- `xr806_os/project/tuya/gcc/defconfig:22-24` has `CONFIG_OTA=y` and `CONFIG_OTA_POLICY_IMAGE_COMPRESSION=y`, **not** `CONFIG_OTA_POLICY_PINGPONG`.
- `xr806_os/project/image_cfg/image.cfg:4-8` proposes OTA config at `1024K` / 4K, image `max_size=1020K`, compressed `xz_max_size=600K`. These are **example configuration defaults**, not verified TRV16 partitions.
- `xr806_os/include/ota/ota.h` declares `ota_push_init/start/data/finish/stop`; `src/ota/ota.c:343-388` shows that `ota_push_start()` performs a flash erase immediately. `ota_push_data()` writes into the selected update sequence; `ota_push_finish()` checks image sections and calls image verification/selection.
- In compression policy, `ota_verify_image()` eventually calls `image_set_cfg()` for the chosen sequence (`src/ota/ota.c:879-965`). This controls a vendor-specific boot flow; it is **not equivalent** to ESPHome's generic OTA write stream or a proven atomic A/B rollback.
- `xr806_os/include/image/image.h` defines `IMAGE_SEQ_NUM=2`, the runtime OTA geometry descriptor `image_ota_param_t`, and `image_get_ota_param()`.
- `src/ota/ota_opt.h` disables vendor HTTP OTA for `__CONFIG_TUYA`. Push-style OTA remains present, but needs proper AWIH/compression and bootloader validation.

## Original WXU 2-MiB dump discrepancy

The provided `WXU_XR806_flash_2MiB.bin` is exactly 2,097,152 bytes. Its bytes at `0x100000` are **not erased** (start `77 8e 4e 00 b7 8f 4e 00`). Scanning the 64-KiB interval `0x100000..0x10FFFF` finds approximately 98.8% bytes different from `0xFF`. Numerous AWIH headers also occur beyond `0x100000`, for example at `0x127C00`, `0x130000`, and `0x178000`. Therefore the vendor *example* 1024-KiB OTA-config location is **not safe to assume** for this original TRV16 image. It may be application or firmware section data; no writable OTA region has been proven.

### Development delivered in this branch

1. `ota_update_pm_gate.h`: portable fail-closed PM wakelock state machine. `ota_backend.cpp` integrates it with the process-wide `UpdateLock` under `USE_XR806_STANDBY_CONTROLLER`: assert OTA busy before flash work, require native `pm_wake_lock()`, keep native lock until `UpdateLock::release()`, and never release the global flash-write lock after a PM-unlock failure. A successful OTA `hold_until_reboot()` retains the lock until reboot by design.
2. `ota_flash_plan.h`: **read-only** runtime geometry admission/check for sector-aligned candidate/metadata regions, overflow and overlap against running image, size and explicit proof flags. A test using the observed TRV16 occupied range fails closed on active-image overlap.
3. `tests/xr806/ota_update_preflight_test.cpp`: host tests for acquired/failed/unreleased PM locks and flash-plan failure modes, with no flash writes.
4. `XR806OTAStandbyGuard` remains a veto. OTA/flash update support has **not** been enabled, and the flash plan is only an input validator for a future vendor backend.

## Required before implementing any flash-writing backend

1. Decode and verify the current TRV16 bootloader's actual `image_get_ota_param()` geometry, running sequence, flash IDs, compressed-image region and update-sequence erase boundaries. Compare runtime values with dump, not a generic project image.cfg.
2. Identify build output format for `ota_push_data()`: AWIH sections, XZ packaging, verify metadata and resulting bootloader behavior. Preserve the original WXU dump and prove recovery on spare hardware.
3. Determine secure boot/anti-rollback/signature requirements and power-loss recovery. MD5/checksum alone is not image authentication; use authenticated upload and independent firmware verification.
4. Provide a real XR806 `OTABackend` that calls the vendor writer only after full preflight, and test abort at every write boundary. Until that is implemented, **keep OTA rejected in YAML**.
5. Only then implement listener suspend/restore for Native OTA and every enabled OTA transport (including web-server OTA) with drain/accept-race proof and SDK PM locks. Run WLAN, OTA and CMD38 races on a spare WXU, with power measurement.

## Safety note

Host tests validate the admission math and PM lock state only. No working ESPHome OTA upgrade or hardware resume is claimed. Do not flash the only TRV16 with any of these experiments.
