"""Host verification for the XRadio AWIH/XZ packager, no flash writes."""
import importlib.util
import pathlib
import random
import struct
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FILE = ROOT / "tools/xr806/make_ota_xz.py"
spec = importlib.util.spec_from_file_location("xr_ota_packer", FILE)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def header(section_id: int, body: bytes, next_addr: int, private=None):
    fields = [mod.AWIH_MAGIC, 5, 0, (0xFFFF - mod.checksum16(body)) & 0xFFFF,
              len(body), 0xFFFFFFFF, 0xFFFFFFFF, len(body), 1,
              next_addr, section_id] + (private or [0xFFFFFFFF] * 6)
    data = mod.HEADER.pack(*fields)
    fields[2] = (0xFFFF - mod.checksum16(data)) & 0xFFFF
    return mod.HEADER.pack(*fields) + body


def fixture(*, app_size=1200, xz_kib=64):
    start = 8192
    fdcm = 256 * 1024
    boot = header(mod.BOOT_ID, b"bootrom", start, [
        0x001000FF, fdcm, (xz_kib << 16) | 256, *([0xFFFFFFFF] * 3)
    ])
    app = header(mod.APP_ID, bytes([1, 2, 3]) * (app_size // 3), 0xFFFFFFFF)
    image = bytearray([0xFF] * (start + len(app)))
    image[:len(boot)] = boot
    image[start:start + len(app)] = app
    return bytes(image)


class TestAWIHXZ(unittest.TestCase):
    def test_roundtrip(self):
        raw = fixture()
        image, info = mod.package(raw)
        self.assertTrue(info["fits"])
        self.assertEqual(info["sections"], 1)
        self.assertEqual(image[:4], b"AWIH")
        sh = mod.section(image, 0, mod.APP_ID)
        self.assertEqual(sh[8], 0x11)
        self.assertEqual(sh[9], 0xFFFFFFFF)

    def test_corrupt_input_refused(self):
        raw = bytearray(fixture())
        raw[12] ^= 3
        with self.assertRaises(ValueError):
            mod.package(bytes(raw))

    def test_corrupt_payload_refused(self):
        raw = bytearray(fixture())
        raw[8192 + 70] ^= 3
        with self.assertRaises(ValueError):
            mod.package(bytes(raw))

    def test_oversize_requires_explicit_analysis_override(self):
        rng = random.Random(20261109)
        raw = bytearray(fixture(app_size=65000, xz_kib=1))
        raw[8192 + 64:8192 + 64 + 64998] = rng.randbytes(64998)
        # Repair data checksum in the deliberately mutated source.
        data = bytes(raw[8192 + 64:8192 + 64 + 64998])
        struct.pack_into("<H", raw, 8192 + 10,
                         (0xFFFF - mod.checksum16(data)) & 0xFFFF)
        # data checksum resides in header, so fix header checksum as well.
        struct.pack_into("<H", raw, 8192 + 8, 0)
        struct.pack_into("<H", raw, 8192 + 8,
                         (0xFFFF - mod.checksum16(raw[8192:8192 + 64])) & 0xFFFF)
        with self.assertRaises(ValueError):
            mod.package(bytes(raw))
        output, info = mod.package(bytes(raw), allow_oversize=True)
        self.assertFalse(info["fits"])
        self.assertGreater(len(output), info["ota_slot_bytes"])


if __name__ == "__main__":
    unittest.main()
