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

### Decoded original AWIH OTA geometry (further RE)

The beginning of the **actual** 2-MiB WXU flash image is an AWIH BOOT section with SDK-compatible 64-byte header. The boot section private words decode (per `include/image/image.h` and `project/image_cfg/readme.md`) as:

- `priv[0] = 0x001000FF`: OTA metadata size **4096 bytes**; flash-ID field 0xFF (meaning not yet established).
- `priv[1] = 0x00177000`: OTA metadata address **`0x177000`**, *not* the template's `0x100000`.
- `priv[2] = 0x019005D8`: maximum image 1496 KiB, compressed image maximum 400 KiB.

At `0x177000` the dump begins `A5 5A 5A A5` and contains 21 non-FF bytes in the sector. A second AWIH sequence starts at `0x178000`; its APP, APP_XIP, WLAN_BL, WLAN_FW and SYS_SDD sections occur at `0x178000`, `0x17F800`, `0x1BE000`, `0x1BF000`, `0x1D0800`. A *possible* 400-KiB compressed slot would occupy `0x178000..0x1DC000`. These values are more specific than the example config, but **do not prove which slot is active or that writing is safe**.

`tools/xr806/inspect_ota_geometry.py` reports these fields and image headers directly from a local dump without writing any flash or uploading the image.

### OTA FDCM state reconstructed from vendor ROM and dump

The ROM source `src/rom/rom_bin/src/image/image.c:879-1000` defines `image_raw_cfg_t` (two 16-bit values), sequences `0x5555` = 0 and `0xAAAA` = 1, and `0x9669` = VERIFIED. The companion `src/rom/rom_bin/src/image/fdcm.c:43-77, 175-225` reads the newest append-only record selected by an inverted bitmap.

At original WXU flash offset `0x177000`, the actual FDCM header is `A55A5AA5`, bitmap length 124 bytes, record size 4 bytes, bitmap beginning `0xF8` (three occupied records). The records are:

| Flash offset | raw_seq | raw_state | Decoded |
| --- | --- | --- | --- |
| `0x177084` | `0x5555` | `0x9669` | seq 0, VERIFIED |
| `0x177088` | `0xAAAA` | `0x9669` | seq 1, VERIFIED |
| `0x17708C` | `0x5555` | `0x9669` | seq 0, VERIFIED |

**Offline conclusion:** The latest encoded boot selection is **sequence 0 / VERIFIED**, and the stored history shows a prior selection of sequence 1. With the matching ROM behavior, the update sequence would ordinarily be the other one. This is stronger than just observing two AWIH image sections, but still **not** evidence of power-loss-safe rollback, actual running runtime state or compatibility with newly built ESPHome images.

The updated read-only `inspect_ota_geometry.py` decodes this selection with strict FDCM bounds validation; its synthetic fixtures verify all three records. No `fdcm_write`, `image_set_cfg` or flash erase/write is invoked.

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
