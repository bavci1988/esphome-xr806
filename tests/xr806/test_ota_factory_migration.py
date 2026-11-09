"""Host-only tests for ROM boot geometry stamping and safe wired migration."""
import importlib.util
import json
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/xr806"))
from make_ota_xz import AWIH_MAGIC, APP_ID, BOOT_ID, HEADER, checksum16, package
from stamp_ota_header import stamp, read_geometry
from make_migration_image import build, validate_fdcm_sector, FACTORY_TAIL


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


def old_dump():
    boot = awih(BOOT_ID, b"old boot", 0x8000, [
        0x001000FF, 0x177000, (400 << 16) | 1496,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    ])
    full = bytearray(b"\xFF" * 0x200000)
    full[:len(boot)] = boot
    full[FACTORY_TAIL:] = b"F" * (0x200000 - FACTORY_TAIL)
    return bytes(full)


class TestFactoryMigration(unittest.TestCase):
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

    def test_migration_preserves_factory_tail_and_initializes_selector(self):
        original = old_dump()
        new_image = stamp(generated_boot_and_app(), CFG)
        composite, manifest = build(original, new_image)
        self.assertEqual(len(composite), 0x200000)
        self.assertEqual(composite[:len(new_image)], new_image)
        self.assertTrue(validate_fdcm_sector(composite[0x13F000:0x140000]))
        self.assertEqual(composite[0x140000:FACTORY_TAIL],
                         b"\xFF" * (FACTORY_TAIL - 0x140000))
        self.assertEqual(composite[FACTORY_TAIL:], original[FACTORY_TAIL:])
        self.assertEqual(manifest["ota_package_bytes"], package(new_image)[1]["ota_bytes"])

    def test_unknown_dump_rejected(self):
        invalid = bytearray(old_dump())
        invalid[0] = 0
        with self.assertRaises(ValueError):
            build(bytes(invalid), stamp(generated_boot_and_app(), CFG))


if __name__ == "__main__":
    unittest.main()
