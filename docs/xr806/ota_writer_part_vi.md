# XR806 / WXU: OTA-Part VI – Writable XRadio updater

**Status:** Real flash-writing implementation submitted on feature branch. Only host-side checks available so far; ARM build and hardware flash are NOT confirmed. Do not deploy to the only TRV16.

## Implemented

- Replaces the unconditional no-write OTA backend. Successful admission calls real vendor ota_push_init(), ota_push_start() (flash ERASE), ota_push_data() (WRITE), and ota_push_finish() (flash-read validation and boot selection).
- Reads image_get_ota_param(), image_get_running_seq(), image_get_cfg(), and probed FlashChip capacity at runtime. Requires verified active seq0, coherent flash-device IDs and disjoint bootloader, active, FDCM and inactive staging ranges.
- Accumulates and validates 70 bytes of AWIH APP/XZ wrapper before any erase. Rejects invalid format, wrong length, over-sized stream, unverified boot state and unsupported OTA type.
- Uses native XR806 PM wakelock plus ESPHome global UpdateLock until reboot after commit. Parses the ESPHome transport MD5 and checks it before selecting image.
- Produces separate firmware.ota.bin with 8 KiB XZ dictionary and XRadio section checksums. Normal firmware.bin remains the full AWIH image for wired recovery.
- Registers the ESPHome ota_image hook: network OTA selects firmware.ota.bin automatically; no extra flash write lock. Build prints an explicit error when an OTA package does not fit.

## 2 MiB layout in matching SDK branch

| Address range | Meaning |
|---|---|
| 0x000000–0x007FFF | Bootloader; first APP starts at 32 KiB |
| 0x008000–0x13EFFF | Active APP, max 1244 KiB |
| 0x13F000–0x13FFFF | FDCM metadata (4 KiB) |
| 0x140000–0x1FFFFF | Compressed OTA staging, 768 KiB |

The active SDK .config and project defconfig select CONFIG_OTA_POLICY_IMAGE_COMPRESSION, not PINGPONG. The default image.cfg was customized for this test device. Runtime checks do not blindly trust this table.

## Original TRV16 limitation

The original WXU dump has FDCM at 0x177000 and compressed slot at 0x178000 with only 400 KiB. Packing its full APP chain with the 8 KiB bootloader dictionary gives 754,784 bytes, which does not fit. This firmware requires a wired first migration with matching bootloader/layout, or a different migration scheme. It cannot install the new layout safely through the legacy OTA partition.

## Build, OTA and risk

Use ordinary ESPHome YAML with xr806 board wxu, wifi, and ota platform esphome with authentication/encryption. Compile using esphome compile. For network update, use esphome upload YOUR.yaml --device TARGET_IP. The platform hook sends firmware.ota.bin. If the package cannot be built, OTA upload is refused before any flash erase. Normal firmware.bin must not be sent as OTA payload.

**Not atomic A/B:** the XRadio compression bootloader reads seq1 from staging, then erases and expands into ACTIVE seq0 on restart. Power failure during expansion can destroy the previous working image. A valid FDCM selector is not an independent rollback partition. Hardware supply and recovery must be validated.

Outstanding: ARM CI build; target image-size proof, runtime PM/wakelock, actual OTA to a separate WXU, readback, controlled interruptions before and after commit, bootloader XZ decompression with WLAN/HC32 resynchronization, and a tested physical recovery procedure.
