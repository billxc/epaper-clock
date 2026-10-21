#!/usr/bin/env python3
"""Real exFAT/FatFs integration tests; no root, devices, loop mounts or PC prep.

Usage: python3 tests/fat_frame_cache_image_test.py /path/to/fat-frame-cache-test
The executable must be a native Linux ELF built with the SDK's original ff.c
and ffunicode.c (see fat_frame_cache_test.c). No SDK source is vendored here;
its original ChaN license applies to those external sources, not this test.
Requires existing mkfs.exfat/fsck.exfat. No compiler or installation needed here.
Only private mktemp-created, inode-checked regular images reach either tool.
FatFs/the MCU cache code creates the cache itself; mkfs only creates test volumes.

These are clean-close persistence and application CRC tests, NOT filesystem
power-loss safety tests. No write interruption/fuzzing is performed: interrupting
FatFs disk_write can also interrupt allocation/directory/bitmap metadata, which
application header/frame CRCs cannot protect or repair.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

SECTOR = 512
MIB = 1024 * 1024
PARTITION_OFFSET = 16 * MIB
FRAME_SIZE = 48000
CACHE_SIZE = 65536
CACHE = "0:/epaper-cache.bin"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Suite:
    def __init__(self, directory, harness, tools):
        self.directory = directory
        self.harness = harness
        self.tools = tools
        self.images = {}
        self.env = os.environ.copy()
        self.env.pop("FAIL_AFTER_WRITES", None)
        self.env["LC_ALL"] = "C"
        self.frame_a = os.urandom(FRAME_SIZE)
        self.frame_b = os.urandom(FRAME_SIZE)
        self.input_a = directory / "frame-a.bin"
        self.input_b = directory / "frame-b.bin"
        self.input_a.write_bytes(self.frame_a)
        self.input_b.write_bytes(self.frame_b)
        self.unrelated_data = os.urandom(17123)
        self.unrelated = directory / "unrelated-input.bin"
        self.unrelated.write_bytes(self.unrelated_data)
        self.output = directory / "output.bin"

    def run(self, args, success=True):
        result = subprocess.run(
            [str(arg) for arg in args], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=self.env, timeout=120,
        )
        if success:
            require(result.returncode == 0,
                    f"command failed ({result.returncode}): {args}\n"
                    f"{result.stdout}\n{result.stderr}")
        return result

    def create(self, name, size):
        path = self.directory / name
        with path.open("xb") as stream:
            stream.truncate(size)
        st = path.lstat()
        require(stat.S_ISREG(st.st_mode), "new image is not regular")
        self.images[path] = (st.st_dev, st.st_ino)
        return path

    def checked(self, path):
        require(path.parent == self.directory and path in self.images,
                "tool target was not created in this script's private directory")
        st = path.lstat()
        require(stat.S_ISREG(st.st_mode) and
                (st.st_dev, st.st_ino) == self.images[path],
                "image is not the original regular file (or became a symlink)")
        return path

    def restore(self, source, target):
        self.checked(source)
        self.checked(target)
        require(source != target, "cannot copy image onto itself")
        with source.open("rb") as src, target.open("r+b") as dst:
            shutil.copyfileobj(src, dst)
            dst.truncate()

    def clone(self, source, name):
        self.checked(source)
        target = self.create(name, source.stat().st_size)
        self.restore(source, target)
        return target

    def native(self, image, command, *args, success=True):
        self.checked(image)
        result = self.run([self.harness, image, command, *args], success)
        if result.returncode == 0:
            require(re.search(r"\bfs_type=4\b", result.stdout),
                    "harness did not mount real exFAT: " + result.stdout)
        return result

    def dump(self, image, fat_path):
        self.output.unlink(missing_ok=True)
        self.native(image, "dump", fat_path, self.output)
        return self.output.read_bytes()

    def fsck(self, image, offset=0):
        self.checked(image)
        target = image
        if offset:
            # fsck.exfat does not auto-discover MBR partitions. Extract only the
            # volume to another registered regular file; never use loop devices.
            name = image.name + ".fsck-volume"
            target = self.directory / name
            if target not in self.images:
                self.create(name, image.stat().st_size - offset)
            self.checked(target)
            with image.open("rb") as src, target.open("r+b") as dst:
                src.seek(offset)
                shutil.copyfileobj(src, dst)
                dst.truncate()
        self.run([self.tools["fsck.exfat"], "-n", self.checked(target)])

    def format(self, name, size, cluster):
        image = self.create(name, size)
        # -c is BYTES for exfatprogs, not sectors. Check help before any format,
        # then independently verify both shifts in the resulting boot sector.
        self.run([self.tools["mkfs.exfat"], "-K", "-s", str(SECTOR),
                  "-c", str(cluster), "-L", "CACHE-TEST", self.checked(image)])
        with image.open("rb") as stream:
            boot = stream.read(SECTOR)
        require(boot[3:11] == b"EXFAT   " and boot[108] == 9 and boot[110] == 1 and
                (1 << (boot[108] + boot[109])) == cluster,
                "mkfs -c units/512-byte sector/single FAT mismatch")
        self.fsck(image)
        return image

    def check_unrelated(self, image):
        dumped = self.dump(image, "0:/unrelated.bin")
        require(hashlib.sha256(dumped).digest() ==
                hashlib.sha256(self.unrelated_data).digest(),
                "unrelated file SHA256 changed")
        listing = self.native(image, "list").stdout
        require(re.search(r"^ENTRY f 17123 unrelated\.bin$", listing, re.M),
                "unrelated directory entry missing or changed: " + listing)
        require(re.search(r"^ENTRY d \d+ keep-dir$", listing, re.M),
                "unrelated directory disappeared: " + listing)
        require(hashlib.sha256(self.dump(image, "0:/keep-dir/retained.bin")).digest() ==
                hashlib.sha256(self.unrelated_data).digest(),
                "nested unrelated file SHA256 changed")
        return listing

    def read_equals(self, image, expected):
        # Each command is a separate process/open/mount/close/unmount, so this
        # explicitly checks persistence, not only a cached FIL sector buffer.
        self.output.unlink(missing_ok=True)
        result = self.native(image, "cache-read", self.output)
        require(self.output.read_bytes() == expected, "48000-byte round trip mismatch")
        return result

    def rejected(self, image, codes=(4,)):
        self.output.unlink(missing_ok=True)
        result = self.native(image, "cache-read", self.output, success=False)
        require(result.returncode in codes,
                f"expected rejection {codes}, got {result.returncode}: {result.stderr}")
        require("OPEN_FAIL" in result.stderr or "CACHE_READ_FAIL" in result.stderr,
                "missing rejection diagnostic: " + result.stderr)
        require(not self.output.exists(), "rejected frame was exported")

    def container(self, image, valid, frame=None):
        data = self.dump(image, CACHE)
        require(len(data) == CACHE_SIZE, "cache is not exactly 64KiB")
        magic, version, length, flag, frame_crc, header_crc = struct.unpack_from(
            "<8sIIIII", data)
        require((magic, version, length, flag) ==
                (b"EPDCACHE", 1, FRAME_SIZE, valid), "invalid cache header fields")
        require(header_crc == zlib.crc32(data[:24]) & 0xFFFFFFFF,
                "invalid header CRC")
        require(data[28:SECTOR] == bytes(SECTOR - 28), "nonzero header reserved bytes")
        if frame is not None:
            require(data[SECTOR:SECTOR + FRAME_SIZE] == frame and
                    frame_crc == zlib.crc32(frame) & 0xFFFFFFFF,
                    "payload offset or frame CRC mismatch")
        elif valid == 0:
            # Initial creation and explicit invalidate must not claim a frame.
            require(flag == 0, "invalidated cache claims a valid frame")
        return data

    def wrap_mbr(self, volume):
        self.checked(volume)
        image = self.create("mbr-128k.img", PARTITION_OFFSET + volume.stat().st_size)
        mbr = bytearray(SECTOR)
        struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0" * 3, 0x07, b"\0" * 3,
                         PARTITION_OFFSET // SECTOR, volume.stat().st_size // SECTOR)
        mbr[510:512] = b"\x55\xaa"
        with volume.open("rb") as src, image.open("r+b") as dst:
            dst.write(mbr)
            dst.seek(PARTITION_OFFSET)
            shutil.copyfileobj(src, dst)
            # Make both exFAT boot regions describe a real partition offset,
            # including their checksums (not merely an offset-zero superfloppy
            # copied behind an MBR). No FatFs file/metadata patching is involved.
            for relative in (0, 12 * SECTOR):
                dst.seek(PARTITION_OFFSET + relative)
                boot = bytearray(dst.read(11 * SECTOR))
                require(len(boot) == 11 * SECTOR, "short exFAT boot region")
                struct.pack_into("<Q", boot, 64, PARTITION_OFFSET // SECTOR)
                checksum = 0
                for index, byte in enumerate(boot):
                    if index not in (106, 107, 112):
                        checksum = (((checksum & 1) << 31) + (checksum >> 1) + byte) & 0xFFFFFFFF
                dst.seek(PARTITION_OFFSET + relative)
                dst.write(boot)
                dst.write(struct.pack("<I", checksum) * (SECTOR // 4))
        self.fsck(image, PARTITION_OFFSET)
        return image

    def exercise(self, fresh, label, offset=0):
        # Pre-populate unrelated files/a directory only, all via real FatFs.
        # There is no cache template and no dependency on old PC prep tools.
        self.native(fresh, "put", self.unrelated)
        self.native(fresh, "mkdir", "0:/keep-dir")
        self.native(fresh, "put", self.unrelated, "0:/keep-dir/retained.bin")
        self.check_unrelated(fresh)
        listing = self.native(fresh, "list").stdout
        require("epaper-cache.bin" not in listing, "cache was unexpectedly pre-created")
        self.fsck(fresh, offset)
        work = self.clone(fresh, label + "-work.img")
        self.rejected(work)  # actual cache_open creates absent valid=0 container
        self.container(work, 0)
        self.fsck(work, offset)
        result = self.native(work, "cache-write", self.input_a)
        print(f"{label} first write: {result.stdout.strip()}")
        result = self.read_equals(work, self.frame_a)
        print(f"{label} remounted read: {result.stdout.strip()}")
        self.container(work, 1, self.frame_a)
        listing = self.check_unrelated(work)
        require(re.search(r"^ENTRY f 65536 epaper-cache\.bin$", listing, re.M),
                "cache directory entry missing or wrong size")
        self.fsck(work, offset)
        committed = self.clone(work, label + "-committed.img")

        self.native(work, "cache-invalidate")
        self.rejected(work)
        self.container(work, 0)
        self.native(work, "cache-write", self.input_b)
        self.read_equals(work, self.frame_b)
        self.container(work, 1, self.frame_b)
        self.check_unrelated(work)
        self.fsck(work, offset)
        print(f"PASS {label}: auto-create, empty rejection, 48K persistent IO, invalidate/rewrite")

        for corruption, index in (("header CRC", 24), ("frame CRC", SECTOR + 123)):
            self.restore(committed, work)
            original = self.dump(work, CACHE)
            self.native(work, "overwrite-cache-byte", index, original[index] ^ 0x80)
            expected = bytearray(original)
            expected[index] ^= 0x80
            require(self.dump(work, CACHE) == expected, "corruption changed more than one file byte")
            # Header validity may be checked by open or read; neither may export.
            self.rejected(work, (2, 4) if index < SECTOR else (4,))
            self.check_unrelated(work)
            self.fsck(work, offset)  # app CRC failure while filesystem remains healthy
            self.native(work, "cache-write", self.input_b)
            self.read_equals(work, self.frame_b)
            self.container(work, 1, self.frame_b)
            self.check_unrelated(work)
            self.fsck(work, offset)
            print(f"PASS {label}: {corruption} rejected and rewritten; filesystem clean")

        # Exercise creation through cache-write too, without an earlier read or
        # any PC-created container/template.
        self.restore(fresh, work)
        self.native(work, "cache-write", self.input_b)
        self.read_equals(work, self.frame_b)
        self.container(work, 1, self.frame_b)
        self.check_unrelated(work)
        self.fsck(work, offset)
        print(f"PASS {label}: first cache-write auto-creates absent container")

        for size in (123, CACHE_SIZE):
            self.restore(fresh, work)
            foreign = self.directory / "foreign.bin"
            foreign_data = b"FOREIGN!" + os.urandom(size - 8)  # definitely not cache magic
            foreign.write_bytes(foreign_data)
            self.native(work, "put", foreign, CACHE)
            self.rejected(work, (2,))
            for operation, args in (("cache-write", (self.input_b,)),
                                    ("cache-invalidate", ())):
                result = self.native(work, operation, *args, success=False)
                require(result.returncode == 2 and "OPEN_FAIL" in result.stderr,
                        "foreign file accepted: " + result.stderr)
                require(self.dump(work, CACHE) == foreign_data,
                        "same-name foreign file was truncated or modified")
            self.check_unrelated(work)
            self.fsck(work, offset)
        print(f"PASS {label}: short and 64KiB same-name foreign files preserved")

    def all_tests(self):
        help_result = self.run([self.tools["mkfs.exfat"], "--help"], success=False)
        help_text = help_result.stdout + help_result.stderr
        require("--cluster-size" in help_text and
                re.search(r"-c\b[^\n]*--cluster-size[^\n]*size", help_text),
                "cannot confirm mkfs.exfat -c size semantics from --help")
        print("mkfs.exfat -c confirmed as cluster size in bytes; boot shifts also checked")
        plain = self.format("superfloppy-64m.img", 64 * MIB, 4096)
        self.exercise(plain, "superfloppy")
        volume = self.format("partition-128m.img", 128 * MIB, 128 * 1024)
        mbr = self.wrap_mbr(volume)
        self.exercise(mbr, "mbr-16m-offset-128k-clusters", PARTITION_OFFSET)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("harness", type=Path, help="already-built native SDK FatFs ELF")
    args = parser.parse_args()
    try:
        harness = args.harness.resolve(strict=True)
        require(harness.is_file() and os.access(harness, os.X_OK),
                "harness is not an executable regular file")
        with harness.open("rb") as stream:
            require(stream.read(4) == b"\x7fELF", "harness must be a native ELF, not a mock script")
        tools = {name: shutil.which(name) for name in ("mkfs.exfat", "fsck.exfat")}
        require(all(tools.values()), "requires installed mkfs.exfat and fsck.exfat; nothing installed")
        # Never accept a caller-provided image; all tool targets are owned here.
        with tempfile.TemporaryDirectory(prefix="fat-frame-cache-test-", dir="/tmp") as name:
            Suite(Path(name), harness, tools).all_tests()
    except (AssertionError, OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("All real exFAT cache tests passed (clean-close/CRC, not power-loss safety).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
