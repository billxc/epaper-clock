#!/usr/bin/env python3
import hashlib
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("manifest", Path(__file__).resolve().parents[1] / "tools/firmware_manifest.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
COMMIT = "a" * 40


def image():
    app = bytearray(288)
    app[0], app[1], app[23] = 0xe9, 1, 1
    struct.pack_into("<H", app, 12, 9)
    struct.pack_into("<II", app, 24, 0x3c000000, 256)
    struct.pack_into("<I", app, 32, 0xabcd5432)
    app[48:56] = b"aaaaaaa\0"
    app[80:93] = b"epaper-clock\0"
    checksum = 0xef
    for byte in app[32:]:
        checksum ^= byte
    app.extend(b"\0" * 15 + bytes([checksum]))
    app.extend(hashlib.sha256(app).digest())
    return bytes(app)


class ManifestTest(unittest.TestCase):
    def test_app_and_atomic_directory(self):
        app = image()
        manifest = module.app_manifest(app, COMMIT)
        self.assertEqual(manifest["sha256"], hashlib.sha256(app).hexdigest())
        self.assertEqual(manifest["version"], "aaaaaaa")
        self.assertEqual(manifest["chip"], 9)
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            (root / "app.bin").write_bytes(app)
            licenses = root / "licenses"
            licenses.mkdir()
            for name in ["LICENSE", "THIRD_PARTY.md", "OFL-NotoSansSC.txt", "BOARD-LIBRARY-NOTICES.txt",
                         "ESP-IDF-LICENSE", "FATFS-SDMMC-NOTICES.txt"]:
                (licenses / name).write_text("test fixture")
            module.publish(root / "app.bin", COMMIT, root / "site", licenses)
            files = list((root / "site/firmware").iterdir())
            self.assertEqual(len(files), 3)
            self.assertEqual((root / "site/firmware/licenses/OFL-NotoSansSC.txt").read_text(), "test fixture")
            self.assertEqual((root / "site/firmware" / manifest["url"]).read_bytes(), app)
            with self.assertRaises(ValueError):
                module.publish(root / "app.bin", COMMIT, root / "site", licenses)
            (licenses / "FATFS-SDMMC-NOTICES.txt").unlink()
            with self.assertRaises(ValueError):
                module.publish(root / "app.bin", COMMIT, root / "site2", licenses)

    def test_reject_non_app_wrong_source_and_corruption(self):
        app = image()
        for candidate, commit in [(b"PK" + app, COMMIT), (b"\xff" * 24 + app, COMMIT),
                                  (app, "b" * 40), (app, "../main"), (app[:-1], COMMIT)]:
            with self.subTest(commit=commit, size=len(candidate)), self.assertRaises(ValueError):
                module.app_manifest(candidate, commit)
        for offset in [0, 12, 32, 48, 80, 160, len(app) - 1]:
            broken = bytearray(app)
            broken[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                module.app_manifest(bytes(broken), COMMIT)


if __name__ == "__main__":
    unittest.main()
