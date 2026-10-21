#!/usr/bin/env python3
"""Create a cache container in an already-mounted Linux ext4 filesystem.

This tool never creates a filesystem or opens a device. Existing containers are
never overwritten. Run it with the mount root, not a directory below that root.
Requires 4KiB blocks and a non-indexed, non-encrypted root directory. After
fsync, FIEMAP checks allocation only; no physical address is saved or used for
raw writes. A newly created file is left in place if a later check fails.
"""

import argparse
import fcntl
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import zlib

FILE_NAME = "epaper-cache.bin"
FILE_SIZE = 65536
HEADER_SIZE = 512
FRAME_SIZE = 48000


def initial_cache_bytes():
    """Return a fully allocated-image template with a valid, empty v1 header.

    Pure function: no filesystem or mount dependencies. `valid=0` means that
    there is not yet a readable frame, even though the header CRC is correct.
    """
    # Nonzero payload/reserved bytes also prevent debugfs/copy tools from
    # optimizing zero blocks into holes. Only the 512-byte header is zero-padded.
    data = bytearray(b"\xA5" * FILE_SIZE)
    data[:HEADER_SIZE] = bytes(HEADER_SIZE)
    struct.pack_into("<8sIIII", data, 0, b"EPDCACHE", 1, FRAME_SIZE, 0, 0)
    struct.pack_into("<I", data, 24, zlib.crc32(data[:24]) & 0xFFFFFFFF)
    return bytes(data)


def mounted_ext4_root(directory):
    """Resolve and check an exact mountpoint using Linux util-linux findmnt."""
    root = Path(directory).resolve(strict=True)
    if not root.is_dir():
        raise ValueError("not a directory: {}".format(root))
    result = subprocess.run(
        ["findmnt", "--json", "--mountpoint", str(root),
         "--output", "TARGET,FSTYPE,FSROOT"],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if result.returncode:
        raise ValueError("not a mount root (or findmnt failed): {}".format(root))
    mounts = json.loads(result.stdout).get("filesystems", [])
    if len(mounts) != 1:
        raise ValueError("cannot identify one exact mount root: {}".format(root))
    mount = mounts[0]
    if (Path(mount.get("target", "")).resolve() != root
            or mount.get("fstype") != "ext4"
            or mount.get("fsroot") != "/"):
        raise ValueError("requires the root of a mounted ext4 filesystem (not a bind subdirectory)")
    return root


def check_directory_profile(directory_fd):
    """Linux FS_IOC_GETFLAGS: the MCU does not traverse htree/encrypted roots."""
    # The ioctl number encodes sizeof(long), but Linux returns an unsigned int.
    flags_buffer = bytearray(struct.calcsize("l"))
    getflags = 0x80006601 | (len(flags_buffer) << 16)
    fcntl.ioctl(directory_fd, getflags, flags_buffer, True)
    flags = struct.unpack_from("=I", flags_buffer)[0]
    if flags & 0x1000:  # FS_INDEX_FL
        raise ValueError("ext4 root uses an htree index (FS_INDEX_FL); unsupported")
    if flags & 0x800:  # FS_ENCRYPT_FL
        raise ValueError("encrypted ext4 root directory is unsupported")


def check_initialized_single_extent(file_fd):
    """Read-only Linux FIEMAP check; physical addresses are never persisted.

    Ask for two extents over the whole mapping, so fragmentation cannot be
    mistaken for a truncated one-extent result. Only FIEMAP_EXTENT_LAST is
    allowed: UNKNOWN, DELALLOC, UNWRITTEN, encoded/encrypted/inline/tail data,
    unaligned, merged, shared and any future special flags are all rejected.
    """
    fiemap_header = struct.Struct("=QQIIII")
    fiemap_extent = struct.Struct("=QQQQQIIII")
    buffer = bytearray(fiemap_header.size + 2 * fiemap_extent.size)
    # fm_start, fm_length, fm_flags (SYNC), mapped_extents, extent_count, reserved
    fiemap_header.pack_into(buffer, 0, 0, (1 << 64) - 1, 1, 0, 2, 0)
    fcntl.ioctl(file_fd, 0xC020660B, buffer, True)  # FS_IOC_FIEMAP
    mapped = fiemap_header.unpack_from(buffer)[3]
    if mapped != 1:
        raise ValueError("new cache file is fragmented or unallocated; requires one extent (file retained)")
    extent = fiemap_extent.unpack_from(buffer, fiemap_header.size)
    logical, length, flags = extent[0], extent[2], extent[5]
    if logical != 0 or length != FILE_SIZE or flags != 1:  # FIEMAP_EXTENT_LAST
        raise ValueError("new cache file is not one fully initialized plain-data extent (file retained)")


def prepare(directory):
    root = mounted_ext4_root(directory)
    fs = os.statvfs(root)
    if fs.f_bsize != 4096 or fs.f_frsize != 4096:
        raise ValueError("requires an ext4 filesystem with 4KiB blocks")
    target = root / FILE_NAME
    directory_fd = os.open(str(root), os.O_RDONLY | os.O_DIRECTORY)
    try:
        check_directory_profile(directory_fd)
        # Write every byte, rather than truncate/fallocate: the core requires
        # an initialized extent, not sparse holes or unwritten extents.
        with target.open("xb") as stream:
            data = initial_cache_bytes()
            if stream.write(data) != len(data):
                raise OSError("short cache-container write")
            stream.flush()
            os.fsync(stream.fileno())
            check_initialized_single_extent(stream.fileno())
        # Make the new directory entry durable too. On failure leave the file
        # in place; never remove or replace a possibly useful container.
        os.fsync(directory_fd)
        check_directory_profile(directory_fd)
    finally:
        os.close(directory_fd)
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mount_root", help="existing ext4 mount root; never a block device")
    args = parser.parse_args()
    try:
        target = prepare(args.mount_root)
    except (OSError, ValueError) as error:
        print("prepare_ext4_cache: {}".format(error), file=sys.stderr)
        return 1
    print("Created {} ({} bytes, no valid frame)".format(target, FILE_SIZE))
    print("Verified 4KiB blocks and one initialized extent; unmount cleanly before MCU use.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
