#!/usr/bin/env python3
"""Native ext4-cache integration tests; no sudo, mounts, loop devices or SDK.

Usage: python3 tests/ext4_cache_image_test.py /path/to/ext4_cache_test
Requires Linux-compatible native harness and existing mke2fs/debugfs/e2fsck.
Only private, script-created regular images are passed to filesystem tools.
Filesystem metadata checksums are checked by e2fsck, independently of the
header/frame CRCs checked by the core and by Python below.
"""

import argparse
import hashlib
import importlib.util
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

PREPARE_PATH = Path(__file__).resolve().parents[1] / "tools" / "prepare_ext4_cache.py"
SPEC = importlib.util.spec_from_file_location("prepare_ext4_cache", PREPARE_PATH)
PREPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PREPARE)

BLOCK = 4096
SECTOR = 512
IMAGE_SIZE = 64 * 1024 * 1024
PARTITION_OFFSET = 1024 * 1024
FRAME_SIZE = 48000
FILE_SIZE = 65536
CACHE_NAME = "/epaper-cache.bin"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Suite:
    def __init__(self, directory, harness, tools):
        self.directory = directory
        self.harness = harness
        self.tools = tools
        self.images = {}  # absolute path -> (device, inode), before any mkfs
        self.environment = os.environ.copy()
        self.environment.pop("FAIL_AFTER_WRITES", None)
        self.environment["LC_ALL"] = "C"
        self.frame_a = hashlib.shake_256(b"ext4-cache frame A").digest(FRAME_SIZE)
        self.frame_b = hashlib.shake_256(b"ext4-cache frame B").digest(FRAME_SIZE)
        self.input_a = directory / "frame-a.bin"
        self.input_b = directory / "frame-b.bin"
        self.input_a.write_bytes(self.frame_a)
        self.input_b.write_bytes(self.frame_b)
        self.output = directory / "read-back.bin"
        self.blocks = []

    def run(self, command, success=True, env=None):
        result = subprocess.run(
            [str(arg) for arg in command], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, env=env or self.environment,
            timeout=120,
        )
        if success:
            require(result.returncode == 0,
                    "command failed ({}) {}\n{}\n{}".format(
                        result.returncode, command, result.stdout, result.stderr))
        return result

    def create_image(self, name, size):
        path = self.directory / name
        with path.open("xb") as stream:
            stream.truncate(size)
        st = path.lstat()
        require(stat.S_ISREG(st.st_mode), "new image is not regular")
        self.images[path] = (st.st_dev, st.st_ino)
        return path

    def checked_image(self, path):
        # Reject devices, symlinks, replacements and arbitrary caller paths.
        require(path.parent == self.directory and path in self.images,
                "filesystem tool target was not created by this script")
        st = path.lstat()
        require(stat.S_ISREG(st.st_mode)
                and (st.st_dev, st.st_ino) == self.images[path],
                "filesystem tool target is not the original regular image")
        return path

    def copy_image(self, source, name):
        self.checked_image(source)
        target = self.create_image(name, source.stat().st_size)
        self.restore(source, target)
        return target

    def restore(self, source, target):
        self.checked_image(source)
        self.checked_image(target)
        require(source != target, "cannot restore an image onto itself")
        # Preserve the registered target inode.
        with source.open("rb") as src, target.open("r+b") as dst:
            dst.seek(0)
            shutil.copyfileobj(src, dst)
            dst.truncate()

    def debugfs(self, image, command, write=False):
        self.checked_image(image)
        args = [self.tools["debugfs"]]
        if write:
            args.append("-w")
        result = self.run(args + ["-R", command, image])
        # debugfs can exit zero even on command errors. Mutations also have
        # explicit on-disk postconditions in their callers.
        error_words = ("File not found", "Command not found", "Usage:",
                       "Filesystem not open", "Could not", "Invalid argument",
                       "Unknown", "unsupported", "No such file", "while ")
        require(not any(word in result.stderr for word in error_words),
                "debugfs error: {}\n{}".format(command, result.stderr))
        return result.stdout

    def fsck(self, image):
        self.checked_image(image)
        self.run([self.tools["e2fsck"], "-f", "-n", image])

    def native(self, image, operation, frame=None, fail_after=None, success=True):
        self.checked_image(image)
        command = [self.harness, image, operation]
        if frame is not None:
            command.append(frame)
        env = self.environment.copy()
        if fail_after is not None:
            env["FAIL_AFTER_WRITES"] = str(fail_after)
        return self.run(command, success=success, env=env)

    def read_equals(self, image, expected):
        self.output.unlink(missing_ok=True)
        result = self.native(image, "read", self.output)
        require(self.output.read_bytes() == expected, "native frame round-trip mismatch")
        return result

    def read_rejected(self, image, open_failure=False):
        self.output.unlink(missing_ok=True)
        result = self.native(image, "read", self.output, success=False)
        require(result.returncode == (2 if open_failure else 4),
                "expected {}, got {}: {}".format(
                    "OPEN_FAIL" if open_failure else "CACHE_READ_FAIL",
                    result.returncode, result.stderr))
        require(("OPEN_FAIL" if open_failure else "CACHE_READ_FAIL") in result.stderr,
                "missing diagnostic: " + result.stderr)
        require(not self.output.exists(), "failed read exported a frame")

    def block_numbers(self, image):
        output = self.debugfs(image, "blocks " + CACHE_NAME)
        require(re.fullmatch(r"\s*(?:\d+\s*)+", output) is not None,
                "unexpected debugfs blocks output: " + output)
        return [int(item) for item in output.split()]

    def patch(self, image, offset, data):
        self.checked_image(image)
        require(0 <= offset and offset + len(data) <= image.stat().st_size,
                "patch outside regular image")
        with image.open("r+b") as stream:
            stream.seek(offset)
            require(stream.write(data) == len(data), "short raw patch")

    def read_at(self, image, offset, count):
        self.checked_image(image)
        with image.open("rb") as stream:
            stream.seek(offset)
            data = stream.read(count)
        require(len(data) == count, "short raw image read")
        return data

    def only_container_changed(self, before, after, partition_offset=0):
        """Compare ALL bytes: permits only the container's allocated data blocks.

        This includes all superblocks, group descriptors, bitmaps, inode tables,
        directory blocks, journal and unrelated data, not just known metadata.
        """
        self.checked_image(before)
        self.checked_image(after)
        require(before.stat().st_size == after.stat().st_size, "image size changed")
        allowed = {partition_offset + block * BLOCK for block in self.blocks}
        changed = 0
        with before.open("rb") as left, after.open("rb") as right:
            offset = 0
            while True:
                a, b = left.read(BLOCK), right.read(BLOCK)
                require(len(a) == len(b), "raw image length mismatch")
                if not a:
                    break
                if a != b:
                    require(offset in allowed,
                            "non-container bytes changed at image block {}".format(offset // BLOCK))
                    changed += 1
                offset += len(a)
        return changed

    def verify_committed_crc(self, image, expected, partition_offset=0):
        start = partition_offset + self.blocks[0] * BLOCK
        data = self.read_at(image, start, FILE_SIZE)
        fields = struct.unpack_from("<8sIIIII", data)
        require(fields[:5] == (b"EPDCACHE", 1, FRAME_SIZE, 1,
                              zlib.crc32(expected) & 0xFFFFFFFF),
                "committed header fields/frame CRC mismatch")
        require(fields[5] == zlib.crc32(data[:24]) & 0xFFFFFFFF,
                "committed header CRC mismatch")
        require(data[SECTOR:SECTOR + FRAME_SIZE] == expected,
                "payload is not at the header-defined offset")

    def build(self):
        base = self.create_image("pristine.img", IMAGE_SIZE)
        # This is the ONLY mke2fs invocation. The path was created with xb,
        # registered and checked as a private regular file immediately before it.
        self.checked_image(base)
        self.run([self.tools["mke2fs"], "-q", "-t", "ext4", "-b", str(BLOCK),
                  "-I", "256", "-F", "-E", "lazy_itable_init=0,lazy_journal_init=0", base])
        initial = PREPARE.initial_cache_bytes()
        require(len(initial) == FILE_SIZE, "initial container size")
        require(struct.unpack_from("<8sIIIII", initial) ==
                (b"EPDCACHE", 1, FRAME_SIZE, 0, 0, zlib.crc32(initial[:24]) & 0xFFFFFFFF),
                "initial header format/CRC mismatch")
        require(initial[28:SECTOR] == bytes(SECTOR - 28), "initial header padding must be zero")
        require(initial[SECTOR:] == b"\xA5" * (FILE_SIZE - SECTOR),
                "initial payload/reserved area must prevent zero-block sparsification")
        self.initial_file = self.directory / "initial.bin"
        self.initial_file.write_bytes(initial)
        # Deliberately sparse source for the separate rejection fixture only.
        self.sparse_file = self.directory / "sparse-container.bin"
        self.sparse_file.write_bytes(initial[:SECTOR] + bytes(FILE_SIZE - SECTOR))
        unrelated = self.directory / "unrelated.bin"
        self.unrelated_data = hashlib.shake_256(b"unrelated file").digest(11003)
        unrelated.write_bytes(self.unrelated_data)
        self.debugfs(base, "write {} {}".format(self.initial_file, CACHE_NAME), write=True)
        self.debugfs(base, "write {} /unrelated.bin".format(unrelated), write=True)
        self.blocks = self.block_numbers(base)
        require(len(self.blocks) == FILE_SIZE // BLOCK
                and self.blocks == list(range(self.blocks[0], self.blocks[0] + FILE_SIZE // BLOCK)),
                "default fixture did not allocate one contiguous 64KiB extent")
        require(self.read_at(base, self.blocks[0] * BLOCK, FILE_SIZE) == initial,
                "debugfs container differs from the preparation template")
        self.fsck(base)
        return base

    def check_unrelated(self, image):
        dumped = self.directory / "unrelated-dump.bin"
        dumped.unlink(missing_ok=True)
        self.debugfs(image, "dump /unrelated.bin {}".format(dumped))
        require(dumped.read_bytes() == self.unrelated_data, "unrelated file changed")

    def exercise(self):
        base = self.build()
        work = self.copy_image(base, "work.img")
        self.read_rejected(work)
        result = self.native(work, "write", self.input_a)
        print("I/O first write: " + result.stdout.strip())
        result = self.read_equals(work, self.frame_a)
        print("I/O first read: " + result.stdout.strip())
        self.verify_committed_crc(work, self.frame_a)
        require(self.only_container_changed(base, work) > 0, "write changed no data")
        self.check_unrelated(work)
        self.fsck(work)
        committed = self.copy_image(work, "committed.img")
        print("PASS empty/read, write/read, raw data-only diff, metadata, unrelated file, fsck")

        self.native(work, "invalidate")
        self.read_rejected(work)
        self.native(work, "write", self.input_b)
        self.read_equals(work, self.frame_b)
        self.verify_committed_crc(work, self.frame_b)
        self.only_container_changed(base, work)
        self.fsck(work)
        print("PASS invalidate and write recovery")

        for label, relative_offset in (("header CRC", 24), ("frame CRC", SECTOR + 123)):
            self.restore(committed, work)
            offset = self.blocks[0] * BLOCK + relative_offset
            old = self.read_at(work, offset, 1)[0]
            self.patch(work, offset, bytes([old ^ 0x80]))
            self.read_rejected(work)
            # Cache CRC failure is independent of ext4 metadata checksums.
            self.only_container_changed(base, work)
            self.fsck(work)
            self.native(work, "write", self.input_b)
            self.read_equals(work, self.frame_b)
            self.verify_committed_crc(work, self.frame_b)
            self.only_container_changed(base, work)
            self.fsck(work)
            print("PASS {} rejection and recovery".format(label))

        self.restore(base, work)
        self.debugfs(work, "set_inode_field {} size 49152".format(CACHE_NAME), write=True)
        self.read_rejected(work, open_failure=True)
        self.restore(base, work)
        self.debugfs(work, "rm " + CACHE_NAME, write=True)
        self.debugfs(work, "write {} {}".format(self.sparse_file, CACHE_NAME), write=True)
        require(len(self.block_numbers(work)) < FILE_SIZE // BLOCK,
                "debugfs did not produce the requested sparse fixture")
        self.read_rejected(work, open_failure=True)
        self.restore(base, work)
        empty = self.directory / "empty.bin"
        empty.write_bytes(b"")
        self.debugfs(work, "rm " + CACHE_NAME, write=True)
        self.debugfs(work, "write {} {}".format(empty, CACHE_NAME), write=True)
        self.debugfs(work, "fallocate {} 0 15".format(CACHE_NAME), write=True)
        self.debugfs(work, "set_inode_field {} size 65536".format(CACHE_NAME), write=True)
        require("[u]" in self.debugfs(work, "stat " + CACHE_NAME),
                "debugfs did not create an unwritten extent")
        self.read_rejected(work, open_failure=True)
        self.restore(base, work)
        self.debugfs(work, "feature needs_recovery", write=True)
        incompat = struct.unpack("<I", self.read_at(work, 1024 + 96, 4))[0]
        require(incompat & 0x4, "RECOVER fixture flag was not set")
        self.read_rejected(work, open_failure=True)
        print("PASS wrong-size, sparse, unwritten and dirty RECOVER layout rejection")

        mbr_base = self.create_image("mbr-pristine.img", PARTITION_OFFSET + IMAGE_SIZE)
        mbr = bytearray(SECTOR)
        struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\0" * 3, 0x83, b"\0" * 3,
                         PARTITION_OFFSET // SECTOR, IMAGE_SIZE // SECTOR)
        mbr[510:512] = b"\x55\xaa"
        self.patch(mbr_base, 0, mbr)
        with base.open("rb") as src, mbr_base.open("r+b") as dst:
            dst.seek(PARTITION_OFFSET)
            shutil.copyfileobj(src, dst)
        mbr_work = self.copy_image(mbr_base, "mbr-work.img")
        self.read_rejected(mbr_work)
        result = self.native(mbr_work, "write", self.input_b)
        print("I/O MBR write: " + result.stdout.strip())
        result = self.read_equals(mbr_work, self.frame_b)
        print("I/O MBR read: " + result.stdout.strip())
        self.verify_committed_crc(mbr_work, self.frame_b, PARTITION_OFFSET)
        require(self.only_container_changed(mbr_base, mbr_work, PARTITION_OFFSET) > 0,
                "partition write changed no data")
        # e2fsck does not discover an MBR partition: extract a regular-file view.
        self.restore(base, work)
        with mbr_work.open("rb") as src, work.open("r+b") as dst:
            src.seek(PARTITION_OFFSET)
            shutil.copyfileobj(src, dst)
            dst.truncate()
        self.check_unrelated(work)
        self.fsck(work)
        print("PASS MBR 1MiB offset, raw diff and partition fsck")

        # Measure a committed A -> B transition, the same starting state used
        # below (an empty -> B write could legitimately use a different count).
        self.restore(committed, work)
        result = self.native(work, "write", self.input_b)
        match = re.search(r"\bwrites=(\d+)", result.stdout)
        require(match is not None, "harness must report callback count")
        write_count = int(match.group(1))
        require(write_count > 2, "implausible frame write callback count")
        self.read_equals(work, self.frame_b)
        # A few representative request boundaries, not an exhaustive/fuzz run.
        # Failure BEFORE the first write may legitimately retain the old frame.
        for fail_at in sorted({1, 2, write_count // 2, write_count}):
            self.restore(committed, work)
            result = self.native(work, "write", self.input_b,
                                 fail_after=fail_at, success=False)
            require(result.returncode == 3 and "IO_FAIL" in result.stderr
                    and "injected" in result.stderr,
                    "expected injected IO_FAIL, got: " + result.stderr)
            self.output.unlink(missing_ok=True)
            result = self.native(work, "read", self.output, success=False)
            if result.returncode == 0:
                require(self.output.read_bytes() in (self.frame_a, self.frame_b),
                        "interrupted write exported a valid mixed frame")
            else:
                require(result.returncode == 4 and "CACHE_READ_FAIL" in result.stderr,
                        "interruption damaged discovery/IO instead of invalidating frame: " + result.stderr)
                require(not self.output.exists(), "failed interrupted read exported a frame")
            self.only_container_changed(base, work)
            self.fsck(work)
            self.native(work, "write", self.input_b)
            self.read_equals(work, self.frame_b)
            self.verify_committed_crc(work, self.frame_b)
            self.only_container_changed(base, work)
        self.check_unrelated(work)
        self.fsck(work)
        print("PASS interrupted writes at callbacks {}; no mixed frame; rewrite recovery".format(
            sorted({1, 2, write_count // 2, write_count})))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("harness", type=Path, help="already-built native C harness executable")
    args = parser.parse_args()
    try:
        harness = args.harness.resolve(strict=True)
        require(harness.is_file() and os.access(harness, os.X_OK),
                "harness is not an executable regular file")
        tools = {name: shutil.which(name) for name in ("mke2fs", "debugfs", "e2fsck")}
        require(all(tools.values()), "requires installed mke2fs, debugfs and e2fsck (no installation attempted)")
        # No caller-supplied image paths: every test target lives in this private
        # directory. ASCII paths also keep debugfs's own command parser simple.
        with tempfile.TemporaryDirectory(prefix="ext4-cache-test-", dir="/tmp") as directory:
            Suite(Path(directory), harness, tools).exercise()
    except (AssertionError, OSError, subprocess.SubprocessError, ValueError) as error:
        print("FAIL: {}".format(error), file=sys.stderr)
        return 1
    print("All ext4 cache image tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
