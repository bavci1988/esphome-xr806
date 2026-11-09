#!/usr/bin/env python3
"""No-dump fixture tests for the read-only original-image geometry decoder."""
from pathlib import Path
import importlib.util
import struct

src = Path(__file__).resolve().parents[2] / "tools/xr806/inspect_ota_geometry.py"
spec = importlib.util.spec_from_file_location("xr806_ota_geometry", src)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

flash = bytearray(b"\xff") * (2 * 1024 * 1024)
boot = struct.pack(
    "<IIHH" + "I" * 13,
    0x48495741, 5, 0, 0, 23364, 0, 0, 23364, 1, 0x8000, 0xA5FF5A00,
    0x001000FF, 0x00177000, 0x019005D8,
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
)
flash[:len(boot)] = boot
flash[0x177000:0x177008] = struct.pack("<IHH", 0xA55A5AA5, 124, 4)
flash[0x177008] = 0xF8  # three committed FDCM records
flash[0x177084:0x177090] = (
    struct.pack("<HH", 0x5555, 0x9669)
    + struct.pack("<HH", 0xAAAA, 0x9669)
    + struct.pack("<HH", 0x5555, 0x9669)
)
image = bytearray(flash)
report = module.parse(image)
assert report["flash_bytes"] == 2097152
assert report["boot_header"]["ota_metadata_flash_id"] == "0xff"
assert report["boot_header"]["ota_metadata_start"] == "0x177000"
assert report["boot_header"]["ota_metadata_bytes"] == 4096
assert report["boot_header"]["image_max_kib"] == 1496
assert report["boot_header"]["xz_max_kib"] == 400
assert report["evidence"]["proposed_compressed_slot_start"] == "0x178000"
assert report["evidence"]["proposed_compressed_slot_end"] == "0x1dc000"
assert report["fdcm_selection"]["record_count"] == 3
assert report["fdcm_selection"]["latest_selected_seq"] == 0
assert [r["seq"] for r in report["fdcm_selection"]["records"]] == [0, 1, 0]
assert not report["write_authorized"]

try:
    module.parse(b"invalid")
    assert False, "short input must be rejected"
except ValueError:
    pass
flash[0:4] = b"BAD!"
try:
    module.parse(flash)
    assert False, "invalid AWIH magic must be rejected"
except ValueError:
    pass
print("PASS AWIH OTA geometry parser and no-write safety")
