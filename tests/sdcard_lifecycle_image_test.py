#!/usr/bin/env python3
"""Run real-FatFs SD lifecycle tests on private regular images only.

Usage: python3 tests/sdcard_lifecycle_image_test.py IMAGE_HARNESS LIFECYCLE_HARNESS
Uses existing mkfs.exfat/fsck.exfat, never devices, root, mounts or a real card.
SDMMC lifecycle is a stub; no hardware/hard-removal safety claims.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from fat_frame_cache_image_test import Suite, MIB, require

# Host esp_random stub returns a fixed nonce; candidate 1 is preoccupied below.
CANDIDATE1 = "0:/epaper-cache-test-1234567812345678-00000001.bin"
CANDIDATE2 = "0:/epaper-cache-test-1234567812345678-00000002.bin"


def main():
    require(len(sys.argv) == 3, __doc__)
    harness, lifecycle = (Path(p).resolve(strict=True) for p in sys.argv[1:])
    for executable in (harness, lifecycle):
        require(executable.is_file() and os.access(executable, os.X_OK), "ELF required")
        with executable.open("rb") as stream:
            require(stream.read(4) == b"\x7fELF", "native ELF required")
    tools = {name: shutil.which(name) for name in ("mkfs.exfat", "fsck.exfat")}
    require(all(tools.values()), "existing exfatprogs required; do not install")
    with tempfile.TemporaryDirectory(prefix="sd-lifecycle-test-", dir="/tmp") as name:
        suite = Suite(Path(name), harness, tools)
        for case in ("normal", "collision", "diagnostic-io-failure", "failed-mount"):
            image = (suite.create(case + ".img", 64 * MIB) if case == "failed-mount"
                     else suite.format(case + ".img", 64 * MIB, 128 * 1024))
            if case != "failed-mount":
                suite.native(image, "put", suite.unrelated)
            foreign = b"DO NOT OVERWRITE THIS TEST-NAME FILE"
            if case == "collision":
                source = suite.directory / "foreign.bin"
                source.write_bytes(foreign)
                suite.native(image, "put", source, "0:/epaper-cache-test.bin")
                suite.native(image, "put", source, CANDIDATE1)
            result = suite.run([lifecycle, suite.checked(image), case])
            print(result.stdout, end="")
            if case != "failed-mount":
                require(suite.dump(image, "0:/unrelated.bin") == suite.unrelated_data,
                        "unrelated file changed")
                if case == "collision":
                    require(suite.dump(image, "0:/epaper-cache-test.bin") == foreign,
                            "legacy foreign diagnostic file changed")
                    require(suite.dump(image, CANDIDATE1) == foreign,
                            "colliding unique-name foreign file changed")
                    listing = suite.native(image, "list")
                    require(CANDIDATE2[3:] not in listing.stdout,
                            "successful retry file not cleaned")
                elif case == "diagnostic-io-failure":
                    data = suite.dump(image, CANDIDATE1)
                    require(len(data) == 65536 and data[:8] == b"EPDCACHE",
                            "failed diagnostic's owned container unexpectedly removed")
                    listing = suite.native(image, "list")
                    require(CANDIDATE2[3:] not in listing.stdout,
                            "new diagnostic did not clean only its own successful file")
                else:
                    listing = suite.native(image, "list")
                    require("epaper-cache-test" not in listing.stdout,
                            "owned successful test file not removed")
                suite.fsck(image, 0)
    print("All SD lifecycle image tests passed; SDMMC/EPD/power-loss not validated.")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
