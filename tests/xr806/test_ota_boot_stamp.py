"""Host-only tests for UART-installed AWIH header stamping and OTA capacity."""
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/xr806"))
from make_ota_xz import AWIH_MAGIC, APP_ID, BOOT_ID, HEADER, checksum16, package
from stamp_ota_header import stamp


def awih(sec_id, body, next_addr, priv=None):
    v = [
        AWIH_MAGIC, 5, 0, (0xFFFF - checksum16(body)) & 0xFFFF,
        len(body), 0xFFFFFFFF, 0xFFFFFFFF, len(body), 1,
        next_addr, sec_id, *(priv or [0xFFFFFFFF] * 6),
    ]
    v[2] = (0xFFFF - checksum16(HEADER.pack(*v))) & 0xFFFF
    return HEADER.pack(*v) + body


CFG = json.dumps({
    "magic": "AWIH", "version": "0.5",
    "OTA": {"addr": "1276K", "size": "4K"},
    "image": {"max_size": "1276K", "xz_max_size": "640K"},
})


def generated_boot_and_app():
    boot = awih(BOOT_ID, b"original vendor boot binary", 0x8000, [
        0xFFFFFFFF, 0xFFFFFFFF, (640 << 16) | 1276,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    ])
    app = awih(APP_ID, b"dummy SDK application bytes" * 64, 0xFFFFFFFF)
    return boot + b"\xFF" * (0x8000 - len(boot)) + app


class TestSerialInstallHeader(unittest.TestCase):
    def test_stamping_real_rom_fields(self):
        raw = generated_boot_and_app()
        result = stamp(raw, CFG)
        self.assertEqual(result[64:], raw[64:])
        head = HEADER.unpack_from(result)
        self.assertEqual(head[11], 0x1000FF)
        self.assertEqual(head[12], 0x13F000)
        self.assertEqual(head[13], (640 << 16) | 1276)
        self.assertEqual(checksum16(result[:64]), 0xFFFF)
        pkg, info = package(result)
        self.assertEqual(info["ota_slot_bytes"], 640 * 1024)
        self.assertTrue(info["fits"])
        self.assertEqual(len(pkg), info["ota_bytes"])

    def test_overlapping_factory_tail_rejected(self):
        bad = CFG.replace('"640K"', '"768K"')
        with self.assertRaises(ValueError):
            stamp(generated_boot_and_app(), bad)

    def test_unrecognized_source_header_rejected(self):
        bad = bytearray(generated_boot_and_app())
        struct.pack_into("<I", bad, 44, 0x1337000)
        with self.assertRaises(ValueError):
            stamp(bytes(bad), CFG)



if __name__ == "__main__":
    unittest.main()
