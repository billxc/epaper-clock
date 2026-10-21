/*
 * Independent, allocation-free ext4 cache reader/writer (MIT project).
 * Format references: Linux kernel documentation, filesystems/ext4/{super,
 * group_descr,inodes,ifork,directory,checksums}.rst; no filesystem code is
 * incorporated. All disk integers are decoded explicitly, never cast.
 *
 * This is deliberately not a filesystem driver: no allocation, journal replay,
 * extent trees, indexed directories, or filesystem metadata writes. The caller
 * must provide exclusive access to a clean, offline filesystem, and callbacks
 * whose successful writes are ordered (including any device-cache flushes).
 */
#include "ext4_cache.h"

#include <string.h>

#define BLOCK_SIZE 4096u
#define SECTORS_PER_BLOCK 8u
#define INODE_SIZE 256u
#define INCOMPAT_FILETYPE 0x00000002u
#define INCOMPAT_64BIT 0x00000080u
#define INCOMPAT_CSUM_SEED 0x00002000u
#define RO_METADATA_CSUM 0x00000400u
#define INODE_EXTENTS 0x00080000u

/* Only these structural features have been considered here. In particular,
 * reject recovery, meta_bg, bigalloc, GDT_CSUM, MMP, fast_commit, encryption,
 * inline_data, casefold, verity and ORPHAN_PRESENT, rather than guessing. */
#define COMPAT_SUPPORTED 0x0000123cu /* journal, xattr, resize, index, sparse2, orphan_file */
#define INCOMPAT_SUPPORTED 0x000022c2u /* filetype, extents, 64bit, flex_bg, csum_seed */
#define RO_SUPPORTED 0x0000046bu /* sparse, large/huge files, dir_nlink, extra_isize, csum */
#define INODE_FLAGS_SUPPORTED 0x000b00c8u /* extents, sync, nodump, noatime, dirsync, topdir */

typedef struct {
    ext4_cache_t *cache;
    uint64_t start;
    uint64_t sectors;
    uint64_t blocks;
    uint64_t groups;
    uint32_t inodes;
    uint32_t inodes_per_group;
    uint32_t incompat;
    uint32_t checksum_seed;
    uint16_t descriptor_size;
    bool metadata_csum;
} filesystem_t;

typedef struct {
    uint64_t block;
    uint64_t size;
    uint32_t length;
    uint32_t generation;
} extent_t;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/* Reflected CRC, with no implicit initial/final complement. ext4 CRC32C uses
 * ~0 initially and no final xor; the cache uses the usual zlib IEEE convention. */
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t size,
                           uint32_t polynomial)
{
    /* Four-bit tables avoid eight polynomial steps per frame byte, using
     * only 128 bytes of read-only constants for both checksum algorithms. */
    static const uint32_t ieee[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    static const uint32_t castagnoli[16] = {
        0x00000000u, 0x105ec76fu, 0x20bd8edeu, 0x30e349b1u,
        0x417b1dbcu, 0x5125dad3u, 0x61c69362u, 0x7198540du,
        0x82f63b78u, 0x92a8fc17u, 0xa24bb5a6u, 0xb21572c9u,
        0xc38d26c4u, 0xd3d3e1abu, 0xe330a81au, 0xf36e6f75u,
    };
    const uint32_t *table = polynomial == 0x82f63b78u ? castagnoli : ieee;
    while (size-- != 0u) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 15u];
        crc = (crc >> 4) ^ table[crc & 15u];
    }
    return crc;
}

static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t size)
{
    return crc_update(crc, p, size, 0x82f63b78u);
}

static uint32_t crc_ieee(uint32_t crc, const uint8_t *p, size_t size)
{
    return crc_update(crc, p, size, 0xedb88320u);
}

static uint32_t crc_number(uint32_t crc, uint32_t value)
{
    uint8_t bytes[4];
    put32(bytes, value);
    return crc32c(crc, bytes, sizeof(bytes));
}

static bool device_read(ext4_cache_t *cache, uint64_t lba)
{
    return lba < cache->sectors &&
           cache->read(cache->context, lba, 1u, cache->sector);
}

static bool fs_read(const filesystem_t *fs, uint64_t relative)
{
    return relative < fs->sectors &&
           device_read(fs->cache, fs->start + relative);
}

static bool block_range(const filesystem_t *fs, uint64_t block, uint64_t count)
{
    /* Block zero contains the superblock, not inode data. */
    return block != 0u && count != 0u && block < fs->blocks &&
           count <= fs->blocks - block;
}

static bool read_super(filesystem_t *fs)
{
    uint8_t *s = fs->cache->sector;
    uint32_t incompat, ro, compat, crc, uuid_seed;
    uint32_t blocks_low, blocks_high, inodes, blocks_per_group;
    uint32_t inodes_per_group;
    uint16_t descriptor_size;

    if (fs->sectors < SECTORS_PER_BLOCK || !fs_read(fs, 2u))
        return false;
    compat = get32(s + 92);
    incompat = get32(s + 96);
    ro = get32(s + 100);
    if (get16(s + 56) != 0xef53u || get32(s + 76) != 1u ||
        get32(s + 72) != 0u || /* Linux inode/checksum layout */
        get32(s + 20) != 0u || get32(s + 24) != 2u ||
        get32(s + 28) != 2u || get16(s + 88) != INODE_SIZE ||
        get16(s + 58) != 1u || get32(s + 232) != 0u ||
        (compat & ~COMPAT_SUPPORTED) != 0u ||
        (incompat & ~INCOMPAT_SUPPORTED) != 0u ||
        (ro & ~RO_SUPPORTED) != 0u ||
        (incompat & 0x40u) == 0u)
        return false;

    fs->metadata_csum = (ro & RO_METADATA_CSUM) != 0u;
    if (((incompat & INCOMPAT_CSUM_SEED) != 0u && !fs->metadata_csum) ||
        (fs->metadata_csum && s[373] != 1u))
        return false;
    blocks_low = get32(s + 4);
    blocks_high = get32(s + 336);
    if ((incompat & INCOMPAT_64BIT) == 0u && blocks_high != 0u)
        return false;
    fs->blocks = (uint64_t)blocks_low | ((uint64_t)blocks_high << 32);
    inodes = get32(s);
    blocks_per_group = get32(s + 32);
    inodes_per_group = get32(s + 40);
    descriptor_size = (incompat & INCOMPAT_64BIT) != 0u ? get16(s + 254) : 32u;
    if (fs->blocks < 2u || fs->blocks > fs->sectors / SECTORS_PER_BLOCK ||
        inodes < 2u || blocks_per_group == 0u || blocks_per_group > 32768u ||
        get32(s + 36) != blocks_per_group ||
        inodes_per_group == 0u || inodes_per_group > 32768u ||
        inodes_per_group % (BLOCK_SIZE / INODE_SIZE) != 0u ||
        ((incompat & INCOMPAT_64BIT) != 0u && descriptor_size != 64u))
        return false;
    fs->groups = (fs->blocks - 1u) / blocks_per_group + 1u;
    if (fs->groups > UINT32_MAX ||
        (uint64_t)inodes > fs->groups * inodes_per_group ||
        (fs->groups * descriptor_size + BLOCK_SIZE - 1u) / BLOCK_SIZE >
            fs->blocks - 1u)
        return false;
    fs->inodes = inodes;
    fs->inodes_per_group = inodes_per_group;
    fs->descriptor_size = descriptor_size;
    fs->incompat = incompat;
    uuid_seed = crc32c(~(uint32_t)0, s + 104, 16u);
    crc = crc32c(~(uint32_t)0, s, EXT4_CACHE_SECTOR_SIZE);
    if (!fs_read(fs, 3u))
        return false;
    fs->checksum_seed = (incompat & INCOMPAT_CSUM_SEED) != 0u ?
                        get32(s + (624u - 512u)) : uuid_seed;
    return !fs->metadata_csum ||
           crc32c(crc, s, 508u) == get32(s + 508);
}

static bool read_inode(const filesystem_t *fs, uint32_t number,
                       uint8_t inode[INODE_SIZE])
{
    uint8_t *s = fs->cache->sector;
    uint64_t group, descriptor_byte, table, table_blocks, inode_byte;
    uint32_t crc, expected;
    uint16_t extra;
    size_t offset;

    if (number == 0u || number > fs->inodes)
        return false;
    group = ((uint64_t)number - 1u) / fs->inodes_per_group;
    if (group >= fs->groups)
        return false;
    descriptor_byte = BLOCK_SIZE + group * fs->descriptor_size;
    if (!fs_read(fs, descriptor_byte / EXT4_CACHE_SECTOR_SIZE))
        return false;
    offset = (size_t)(descriptor_byte % EXT4_CACHE_SECTOR_SIZE);
    table = get32(s + offset + 8u);
    if ((fs->incompat & INCOMPAT_64BIT) != 0u)
        table |= (uint64_t)get32(s + offset + 40u) << 32;
    if (fs->metadata_csum) {
        expected = get16(s + offset + 30u);
        s[offset + 30u] = 0;
        s[offset + 31u] = 0;
        crc = crc_number(fs->checksum_seed, (uint32_t)group);
        crc = crc32c(crc, s + offset, fs->descriptor_size);
        if ((uint16_t)crc != expected)
            return false;
    }
    table_blocks = (uint64_t)fs->inodes_per_group * INODE_SIZE / BLOCK_SIZE;
    if (!block_range(fs, table, table_blocks))
        return false;
    inode_byte = table * BLOCK_SIZE +
                 (((uint64_t)number - 1u) % fs->inodes_per_group) * INODE_SIZE;
    if (!fs_read(fs, inode_byte / EXT4_CACHE_SECTOR_SIZE))
        return false;
    memcpy(inode, s + (size_t)(inode_byte % EXT4_CACHE_SECTOR_SIZE), INODE_SIZE);
    extra = get16(inode + 128);
    if (extra > INODE_SIZE - 128u || extra % 4u != 0u)
        return false;
    if (!fs->metadata_csum)
        return true;
    expected = get16(inode + 124);
    inode[124] = 0;
    inode[125] = 0;
    if (extra >= 4u) {
        expected |= (uint32_t)get16(inode + 130) << 16;
        inode[130] = 0;
        inode[131] = 0;
    }
    crc = crc_number(fs->checksum_seed, number);
    crc = crc_number(crc, get32(inode + 100));
    crc = crc32c(crc, inode, INODE_SIZE);
    return (extra >= 4u ? crc : (uint32_t)(uint16_t)crc) == expected;
}

static bool inode_extent(const filesystem_t *fs, const uint8_t *inode,
                         bool directory, extent_t *extent)
{
    uint32_t flags = get32(inode + 32);
    uint16_t length = get16(inode + 56);
    uint64_t used_blocks;

    if ((get16(inode) & 0xf000u) != (directory ? 0x4000u : 0x8000u) ||
        get16(inode + 26) == 0u || get32(inode + 20) != 0u ||
        (flags & INODE_EXTENTS) == 0u ||
        (flags & ~INODE_FLAGS_SUPPORTED) != 0u ||
        get16(inode + 40) != 0xf30au || get16(inode + 42) != 1u ||
        get16(inode + 44) != 4u || get16(inode + 46) != 0u ||
        get32(inode + 52) != 0u || length == 0u || length > 32768u)
        return false;
    /* ee_len == 32768 is initialized; values greater than it are unwritten. */
    extent->size = (uint64_t)get32(inode + 4) |
                   ((uint64_t)get32(inode + 108) << 32);
    extent->block = (uint64_t)get32(inode + 60) |
                    ((uint64_t)get16(inode + 58) << 32);
    extent->length = length;
    extent->generation = get32(inode + 100);
    if (!block_range(fs, extent->block, length) || extent->size == 0u ||
        extent->size > (uint64_t)length * BLOCK_SIZE)
        return false;
    used_blocks = (extent->size - 1u) / BLOCK_SIZE + 1u;
    if (directory)
        return extent->size % BLOCK_SIZE == 0u && used_blocks <= length;
    return extent->size == EXT4_CACHE_FILE_SIZE && length == 16u;
}

static bool directory_checksum(const filesystem_t *fs, uint64_t block,
                                uint32_t generation)
{
    uint32_t crc = crc_number(fs->checksum_seed, 2u);
    uint8_t *s = fs->cache->sector;
    crc = crc_number(crc, generation);
    for (unsigned int part = 0; part < SECTORS_PER_BLOCK; ++part) {
        if (!fs_read(fs, block * SECTORS_PER_BLOCK + part))
            return false;
        if (part != SECTORS_PER_BLOCK - 1u) {
            crc = crc32c(crc, s, EXT4_CACHE_SECTOR_SIZE);
        } else {
            const uint8_t *tail = s + EXT4_CACHE_SECTOR_SIZE - 12u;
            if (get32(tail) != 0u || get16(tail + 4) != 12u ||
                tail[6] != 0u || tail[7] != 0xdeu)
                return false;
            crc = crc32c(crc, s, EXT4_CACHE_SECTOR_SIZE - 12u);
            if (crc != get32(tail + 8))
                return false;
        }
    }
    return true;
}

/* Small reads may straddle sectors; only the aligned cache scratch is DMA'd. */
static bool directory_bytes(const filesystem_t *fs, uint64_t block,
                             uint32_t offset, uint8_t *out, size_t size,
                             uint64_t *loaded)
{
    while (size != 0u) {
        uint64_t lba = block * SECTORS_PER_BLOCK + offset / EXT4_CACHE_SECTOR_SIZE;
        size_t in_sector = offset % EXT4_CACHE_SECTOR_SIZE;
        size_t count = EXT4_CACHE_SECTOR_SIZE - in_sector;
        if (count > size)
            count = size;
        if (*loaded != lba) {
            if (!fs_read(fs, lba))
                return false;
            *loaded = lba;
        }
        memcpy(out, fs->cache->sector + in_sector, count);
        out += count;
        size -= count;
        offset += (uint32_t)count;
    }
    return true;
}

static bool find_file(const filesystem_t *fs, const extent_t *root,
                       uint32_t *number)
{
    const uint32_t end = BLOCK_SIZE - (fs->metadata_csum ? 12u : 0u);
    uint8_t entry[8], name[sizeof(EXT4_CACHE_FILE_NAME) - 1u];
    uint32_t found = 0;
    for (uint64_t index = 0; index < root->size / BLOCK_SIZE; ++index) {
        uint64_t block = root->block + index;
        uint64_t loaded = UINT64_MAX;
        uint32_t offset = 0;
        if (fs->metadata_csum && !directory_checksum(fs, block, root->generation))
            return false;
        while (offset < end) {
            uint32_t ino;
            uint16_t record_length, name_length;
            uint8_t type = 0;
            if (end - offset < sizeof(entry) ||
                !directory_bytes(fs, block, offset, entry, sizeof(entry), &loaded))
                return false;
            ino = get32(entry);
            record_length = get16(entry + 4);
            if ((fs->incompat & INCOMPAT_FILETYPE) != 0u) {
                name_length = entry[6];
                type = entry[7];
            } else {
                name_length = get16(entry + 6);
            }
            if (record_length < 8u || record_length % 4u != 0u ||
                record_length > end - offset || name_length > 255u ||
                name_length > record_length - 8u || type > 7u ||
                ino > fs->inodes)
                return false;
            if (ino != 0u && name_length == sizeof(name)) {
                if (!directory_bytes(fs, block, offset + 8u, name, sizeof(name), &loaded))
                    return false;
                if (memcmp(name, EXT4_CACHE_FILE_NAME, sizeof(name)) == 0) {
                    if (found != 0u || (type != 0u && type != 1u))
                        return false;
                    found = ino;
                }
            }
            offset += record_length;
        }
    }
    *number = found;
    return found != 0u;
}

static bool open_filesystem(ext4_cache_t *cache, uint64_t start, uint64_t sectors)
{
    filesystem_t fs;
    extent_t extent;
    uint8_t inode[INODE_SIZE];
    uint32_t number;
    if (start >= cache->sectors || sectors > cache->sectors - start)
        return false;
    memset(&fs, 0, sizeof(fs));
    fs.cache = cache;
    fs.start = start;
    fs.sectors = sectors;
    if (!read_super(&fs) || !read_inode(&fs, 2u, inode) ||
        !inode_extent(&fs, inode, true, &extent) ||
        !find_file(&fs, &extent, &number) ||
        !read_inode(&fs, number, inode) ||
        !inode_extent(&fs, inode, false, &extent))
        return false;
    cache->file_lba = start + extent.block * SECTORS_PER_BLOCK;
    cache->opened = true;
    return true;
}

bool ext4_cache_open(ext4_cache_t *cache, ext4_cache_read_sectors_t read,
                     ext4_cache_write_sectors_t write, void *context,
                     uint64_t sectors)
{
    if (cache == NULL)
        return false;
    cache->opened = false;
    cache->file_lba = 0;
    cache->read = read;
    cache->write = write;
    cache->context = context;
    cache->sectors = sectors;
    if (read == NULL || sectors == 0u)
        return false;
    if (open_filesystem(cache, 0u, sectors))
        return true;
    /* Primary MBR Linux partitions only; no EBR traversal or GPT probing.
     * Reread the MBR because filesystem probing reuses the same scratch. */
    for (unsigned int part = 0; part < 4u; ++part) {
        const uint8_t *entry;
        uint64_t start, count;
        if (!device_read(cache, 0u) || get16(cache->sector + 510) != 0xaa55u)
            return false;
        entry = cache->sector + 446u + part * 16u;
        if (entry[4] != 0x83u)
            continue;
        start = get32(entry + 8);
        count = get32(entry + 12);
        if (start != 0u && count != 0u && open_filesystem(cache, start, count))
            return true;
    }
    return false;
}

static bool cache_ready(const ext4_cache_t *cache)
{
    return cache != NULL && cache->opened && cache->read != NULL &&
           cache->file_lba < cache->sectors &&
           EXT4_CACHE_FILE_SIZE / EXT4_CACHE_SECTOR_SIZE <=
               cache->sectors - cache->file_lba;
}

static void make_header(ext4_cache_t *cache, bool valid, uint32_t frame_crc)
{
    uint8_t *s = cache->sector;
    memset(s, 0, EXT4_CACHE_SECTOR_SIZE);
    memcpy(s, "EPDCACHE", 8u);
    put32(s + 8, 1u);
    put32(s + 12, EXT4_CACHE_FRAME_SIZE);
    put32(s + 16, valid ? 1u : 0u);
    put32(s + 20, frame_crc);
    put32(s + 24, ~crc_ieee(~(uint32_t)0, s, 24u));
}

bool ext4_cache_read(ext4_cache_t *cache, uint8_t *frame, size_t size)
{
    uint32_t expected, crc = ~(uint32_t)0;
    size_t remaining = EXT4_CACHE_FRAME_SIZE;
    uint64_t lba;
    if (!cache_ready(cache) || frame == NULL || size != EXT4_CACHE_FRAME_SIZE ||
        !device_read(cache, cache->file_lba))
        return false;
    if (memcmp(cache->sector, "EPDCACHE", 8u) != 0 ||
        get32(cache->sector + 8) != 1u ||
        get32(cache->sector + 12) != EXT4_CACHE_FRAME_SIZE ||
        get32(cache->sector + 16) != 1u ||
        get32(cache->sector + 24) != ~crc_ieee(~(uint32_t)0, cache->sector, 24u))
        return false;
    for (size_t i = 28u; i < EXT4_CACHE_SECTOR_SIZE; ++i)
        if (cache->sector[i] != 0u)
            return false;
    expected = get32(cache->sector + 20);
    lba = cache->file_lba + 1u;
    while (remaining != 0u) {
        const size_t capacity = EXT4_CACHE_IO_SECTORS * EXT4_CACHE_SECTOR_SIZE;
        size_t count = remaining < capacity ? remaining : capacity;
        size_t sectors = (count + EXT4_CACHE_SECTOR_SIZE - 1u) / EXT4_CACHE_SECTOR_SIZE;
        /* cache_ready bounds the entire contiguous file, including padding. */
        if (!cache->read(cache->context, lba, sectors, cache->sector))
            return false;
        crc = crc_ieee(crc, cache->sector, count);
        memcpy(frame, cache->sector, count);
        lba += sectors;
        frame += count;
        remaining -= count;
    }
    /* On failure the caller must discard the output, which can be partial. */
    return ~crc == expected;
}

bool ext4_cache_invalidate(ext4_cache_t *cache)
{
    if (!cache_ready(cache) || cache->write == NULL)
        return false;
    make_header(cache, false, 0u);
    return cache->write(cache->context, cache->file_lba, 1u, cache->sector);
}

bool ext4_cache_write(ext4_cache_t *cache, const uint8_t *frame, size_t size)
{
    uint32_t crc = ~(uint32_t)0;
    size_t remaining = EXT4_CACHE_FRAME_SIZE;
    uint64_t lba;
    if (!cache_ready(cache) || cache->write == NULL || frame == NULL ||
        size != EXT4_CACHE_FRAME_SIZE)
        return false;
    /* Invalidate the old commit first; any interrupted payload is unusable. */
    if (!ext4_cache_invalidate(cache))
        return false;
    lba = cache->file_lba + 1u;
    while (remaining != 0u) {
        const size_t capacity = EXT4_CACHE_IO_SECTORS * EXT4_CACHE_SECTOR_SIZE;
        size_t count = remaining < capacity ? remaining : capacity;
        size_t sectors = (count + EXT4_CACHE_SECTOR_SIZE - 1u) / EXT4_CACHE_SECTOR_SIZE;
        /* Only the last transfer needs padding; CRC excludes reserved bytes. */
        if (count % EXT4_CACHE_SECTOR_SIZE != 0u)
            memset(cache->sector + count, 0, sectors * EXT4_CACHE_SECTOR_SIZE - count);
        memcpy(cache->sector, frame, count);
        crc = crc_ieee(crc, cache->sector, count);
        if (!cache->write(cache->context, lba, sectors, cache->sector))
            return false;
        lba += sectors;
        frame += count;
        remaining -= count;
    }
    make_header(cache, true, ~crc);
    return cache->write(cache->context, cache->file_lba, 1u, cache->sector);
}
