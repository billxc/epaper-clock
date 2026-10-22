#!/usr/bin/env python3
"""Publish only a CI app image, never factory/NVS/zip, beside fresh Pages docs.

The caller must select a successful Firmware run on main and supply its head SHA.
SHA256 is integrity metadata, not a signature. No network or device operations.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct


def app_manifest(app: bytes, commit: str) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("expected full source commit SHA")
    if not 288 <= len(app) <= 16 * 1024 * 1024:
        raise ValueError("invalid application size")
    if app[0] != 0xE9 or struct.unpack_from("<I", app, 32)[0] != 0xABCD5432:
        raise ValueError("not a standalone ESP application")

    def text(offset, length):
        raw = app[offset:offset + length]
        if b"\0" not in raw:
            raise ValueError("unterminated app descriptor")
        value = raw.split(b"\0", 1)[0].decode("ascii")
        if not re.fullmatch(r"[\x21-\x7e]+", value):
            raise ValueError("invalid app descriptor text")
        return value

    version, project = text(48, 32), text(80, 32)
    chip = struct.unpack_from("<H", app, 12)[0]
    if project != "epaper-clock" or chip != 9 or version != commit[:7]:
        raise ValueError("project/chip/version does not match CI ESP32-S3 source")
    # Verify ESP segment bounds, SDK image checksum and appended SHA256 too.
    # CI's successful build remains mandatory; this is not a substitute for it.
    offset, checksum = 24, 0xEF
    segments = app[1]
    if not 1 <= segments <= 16 or app[23] != 1:
        raise ValueError("expected hashed ESP app image")
    for _ in range(segments):
        if offset + 8 > len(app):
            raise ValueError("truncated segment header")
        length = struct.unpack_from("<I", app, offset + 4)[0]
        offset += 8
        end = offset + length
        if end > len(app):
            raise ValueError("truncated segment")
        for byte in app[offset:end]:
            checksum ^= byte
        offset = end
    checksum_end = (offset + 16) // 16 * 16
    if checksum_end + 32 != len(app) or app[checksum_end - 1] != checksum:
        raise ValueError("invalid SDK image checksum/size (signed/padded images unsupported)")
    if hashlib.sha256(app[:checksum_end]).digest() != app[checksum_end:]:
        raise ValueError("invalid SDK appended image digest")
    sha = hashlib.sha256(app).hexdigest()
    return dict(schema=1, version=version, commit=commit, size=len(app), sha256=sha,
                project=project, chip=chip, url=f"app-{commit}-{sha}.bin")


def publish(app_path: Path, commit: str, site: Path, licenses: Path):
    required = {"LICENSE", "THIRD_PARTY.md", "OFL-NotoSansSC.txt",
                "BOARD-LIBRARY-NOTICES.txt", "ESP-IDF-LICENSE", "FATFS-SDMMC-NOTICES.txt"}
    if not licenses.is_dir() or any(not (licenses / name).is_file() for name in required):
        raise ValueError("same-package license notices missing")
    if any(p.is_symlink() for p in licenses.rglob("*")):
        raise ValueError("license symlinks are not permitted")
    app = app_path.read_bytes()
    manifest = app_manifest(app, commit)
    target = site / "firmware"
    # Existing docs must not carry firmware; deploy artifacts are assembled fresh.
    if target.exists():
        raise ValueError("firmware directory already exists; use fresh docs staging")
    target.mkdir(parents=True)
    shutil.copytree(licenses, target / "licenses")
    (target / manifest["url"]).write_bytes(app)
    (target / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--licenses", type=Path, required=True)
    args = parser.parse_args()
    publish(args.app, args.commit, args.site, args.licenses)


if __name__ == "__main__":
    main()
