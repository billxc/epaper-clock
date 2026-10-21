/* Real SDK FatFs host harness, not a mock filesystem or an ESP simulation.
 * Build from the repository root (SDK FatFs revision 80286):
 * cc -std=c11 -O2 -Wall -Wextra -Werror \
 *   -include tests/fatfs_host_config.h \
 *   -I$IDF_PATH/components/fatfs/src -I$IDF_PATH/components/fatfs/diskio \
 *   -Icomponents/sdcard_bsp tests/fat_frame_cache_test.c \
 *   components/sdcard_bsp/fat_frame_cache.c \
 *   $IDF_PATH/components/fatfs/src/ff.c \
 *   $IDF_PATH/components/fatfs/src/ffunicode.c -o /out/fat-frame-cache-test
 * SDK sources remain external, with their original ChaN license notices.
 * If a compiler warns in SDK sources, compile those objects separately with
 * only the necessary warning-specific -Wno-error=...; do not patch the SDK or
 * relax warnings for this harness/the production cache module.
 *
 * IMAGE must already be a regular file. No devices, loop mounts or formatting.
 * Commands (paths inside FatFs are simple absolute paths, no dot components):
 *   IMAGE put INPUT [SIMPLEPATH]       default 0:/unrelated.bin; create-new only
 *   IMAGE dump SIMPLEPATH OUTPUT      export any regular FatFs file
 *   IMAGE list                        list root directory
 *   IMAGE mkdir SIMPLEPATH            create a directory preservation fixture
 *   IMAGE cache-read OUTPUT           also auto-creates absent valid=0 cache
 *   IMAGE cache-write INPUT           INPUT must be exactly 48000 bytes
 *   IMAGE cache-invalidate
 *   IMAGE overwrite-cache-byte OFFSET BYTE   decimal values; via f_write
 * Exit: 1 usage/host IO, 2 mount/open, 3 disk IO, 4 cache read,
 *       5 cache write, 6 invalidate, 7 other FatFs API error.
 * Every invocation closes files, syncs and unmounts. f_mount(NULL) alone is
 * NOT a file flush. CRC corruption commands change file data, not FS metadata.
 */
#include "fat_frame_cache.h"
#include "diskio.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_PATH "0:/" FRAME_CACHE_FILE_NAME
#define SECTOR_SIZE 512u

static struct {
    int fd;
    uint64_t sectors, reads, writes, read_sectors, write_sectors;
    bool failed;
} image = {.fd = -1};

void *ff_memalloc(UINT size) { return malloc(size); }
void ff_memfree(void *pointer) { free(pointer); }
DWORD get_fattime(void)
{
    /* 2024-01-02 12:00:00, independent of wall clock/timezone. */
    return ((DWORD)(2024 - 1980) << 25) | ((DWORD)1 << 21) |
           ((DWORD)2 << 16) | ((DWORD)12 << 11);
}

DSTATUS disk_status(BYTE drive)
{
    return drive == 0 && image.fd >= 0 ? 0 : STA_NOINIT;
}
DSTATUS disk_initialize(BYTE drive) { return disk_status(drive); }

static DRESULT transfer(BYTE drive, void *buffer, LBA_t sector, UINT count, bool write_op)
{
    if (write_op) {
        ++image.writes;
        image.write_sectors += count;
    } else {
        ++image.reads;
        image.read_sectors += count;
    }
    if (disk_status(drive) || !buffer || !count ||
        (uint64_t)sector >= image.sectors ||
        (uint64_t)count > image.sectors - (uint64_t)sector ||
        (uint64_t)count * SECTOR_SIZE > (uint64_t)SSIZE_MAX) {
        image.failed = true;
        return RES_PARERR;
    }
    size_t size = (size_t)count * SECTOR_SIZE;
    off_t offset = (off_t)((uint64_t)sector * SECTOR_SIZE);
    ssize_t done;
    do {
        done = write_op ? pwrite(image.fd, buffer, size, offset)
                        : pread(image.fd, buffer, size, offset);
    } while (done < 0 && errno == EINTR);
    /* A short request is an IO error, not a successful sector transfer. */
    if (done != (ssize_t)size) {
        image.failed = true;
        fprintf(stderr, "IO_FAIL %s sector=%" PRIu64 " count=%u bytes=%zd\n",
                write_op ? "write" : "read", (uint64_t)sector, count, done);
        return RES_ERROR;
    }
    return RES_OK;
}
DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
    return transfer(drive, buffer, sector, count, false);
}
DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count)
{
    return transfer(drive, (void *)buffer, sector, count, true);
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
    if (disk_status(drive))
        return RES_NOTRDY;
    switch (command) {
    case CTRL_SYNC:
        if (fsync(image.fd) == 0)
            return RES_OK;
        image.failed = true;
        return RES_ERROR;
    case GET_SECTOR_COUNT:
        if (!buffer) return RES_PARERR;
        *(LBA_t *)buffer = (LBA_t)image.sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        if (!buffer) return RES_PARERR;
        *(WORD *)buffer = SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        if (!buffer) return RES_PARERR;
        *(DWORD *)buffer = 1;
        return RES_OK;
    default:
        return RES_PARERR; /* Never trim/format any backing file. */
    }
}

/* Check before AND after open. Reject symlinks/devices/FIFOs, inode replacement,
 * and host input/output aliases (including hardlinks) of the backing image.
 * No O_TRUNC until the output has passed all of these checks. */
static int regular_open(const char *path, int flags, bool create)
{
    struct stat before, after, backing;
    bool existed = lstat(path, &before) == 0;
    if ((!existed && (errno != ENOENT || !create)) ||
        (existed && !S_ISREG(before.st_mode))) {
        fprintf(stderr, "regular file required: %s\n", path);
        return -1;
    }
    if (!existed) flags |= O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags | O_NONBLOCK, 0600);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    if (fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
        (existed && (before.st_dev != after.st_dev || before.st_ino != after.st_ino)) ||
        (image.fd >= 0 && (fstat(image.fd, &backing) != 0 ||
         (after.st_dev == backing.st_dev && after.st_ino == backing.st_ino)))) {
        fprintf(stderr, "unsafe host file or alias of IMAGE: %s\n", path);
        close(fd);
        return -1;
    }
    return fd;
}

static bool host_write(int fd, const void *data, size_t size)
{
    const BYTE *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static bool save(const char *path, const BYTE *data, size_t size)
{
    int fd = regular_open(path, O_WRONLY, true);
    if (fd < 0) return false;
    bool ok = ftruncate(fd, 0) == 0 && host_write(fd, data, size) && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static bool load_frame(const char *path, BYTE *frame)
{
    int fd = regular_open(path, O_RDONLY, false);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size == FRAME_CACHE_FRAME_SIZE;
    size_t done = 0;
    while (ok && done < FRAME_CACHE_FRAME_SIZE) {
        ssize_t n = read(fd, frame + done, FRAME_CACHE_FRAME_SIZE - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        done += (size_t)n;
    }
    BYTE extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    ok = ok && n == 0;
    if (close(fd) != 0) ok = false;
    if (!ok) fprintf(stderr, "INPUT must contain exactly %u bytes\n", FRAME_CACHE_FRAME_SIZE);
    return ok;
}

static bool simplepath(const char *path)
{
    if (strncmp(path, "0:/", 3) != 0 || !path[3]) return false;
    const char *part = path + 3;
    for (const char *p = part; ; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!c || c == '/') {
            size_t length = (size_t)(p - part);
            if (!length || (length == 1 && part[0] == '.') ||
                (length == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (!c) return true;
            part = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) {
            return false;
        }
    }
}

static bool number(const char *text, uint64_t *value)
{
    if (!*text) return false;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    errno = 0;
    unsigned long long n = strtoull(text, NULL, 10);
    if (errno) return false;
    *value = (uint64_t)n;
    return true;
}

static int copy_file(const char *fat_path, const char *host_path, bool putting)
{
    int fd = regular_open(host_path, putting ? O_RDONLY : O_WRONLY, !putting);
    if (fd < 0) return 1;
    FIL file;
    FRESULT result = f_open(&file, fat_path, putting ? FA_WRITE | FA_CREATE_NEW : FA_READ);
    if (result != FR_OK) {
        fprintf(stderr, "f_open(%s)=%d\n", fat_path, (int)result);
        close(fd);
        return 7;
    }
    bool ok = putting || ftruncate(fd, 0) == 0;
    BYTE buffer[4096];
    while (ok) {
        UINT count = 0;
        if (putting) {
            ssize_t n;
            do { n = read(fd, buffer, sizeof(buffer)); } while (n < 0 && errno == EINTR);
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            result = f_write(&file, buffer, (UINT)n, &count);
            ok = result == FR_OK && count == (UINT)n;
        } else {
            result = f_read(&file, buffer, sizeof(buffer), &count);
            if (result != FR_OK) { ok = false; break; }
            if (!count) break;
            ok = host_write(fd, buffer, count);
        }
    }
    if (putting && f_sync(&file) != FR_OK) ok = false;
    if (f_close(&file) != FR_OK) ok = false;
    if (!putting && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    return ok ? 0 : 7;
}

static int list_root(void)
{
    FF_DIR dir;
    FILINFO info;
    FRESULT result = f_opendir(&dir, "0:/");
    if (result != FR_OK) return 7;
    while ((result = f_readdir(&dir, &info)) == FR_OK && info.fname[0])
        printf("ENTRY %c %" PRIu64 " %s\n", info.fattrib & AM_DIR ? 'd' : 'f',
               (uint64_t)info.fsize, info.fname);
    bool ok = result == FR_OK;
    if (f_closedir(&dir) != FR_OK) ok = false;
    return ok ? 0 : 7;
}

static int overwrite(uint64_t offset, uint64_t value)
{
    FIL file;
    if (f_open(&file, CACHE_PATH, FA_READ | FA_WRITE) != FR_OK) return 7;
    BYTE byte = (BYTE)value;
    UINT count = 0;
    bool ok = f_size(&file) == FRAME_CACHE_FILE_SIZE &&
              f_lseek(&file, (FSIZE_t)offset) == FR_OK &&
              f_write(&file, &byte, 1, &count) == FR_OK && count == 1;
    if (f_sync(&file) != FR_OK) ok = false;
    if (f_close(&file) != FR_OK) ok = false;
    return ok ? 0 : 7;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE subcommand [file]; see source for commands\n", argv[0]);
        return 1;
    }
    const char *cmd = argv[2];
    bool reading = strcmp(cmd, "cache-read") == 0;
    bool writing = strcmp(cmd, "cache-write") == 0;
    bool invalidating = strcmp(cmd, "cache-invalidate") == 0;
    bool putting = strcmp(cmd, "put") == 0;
    bool dumping = strcmp(cmd, "dump") == 0;
    bool listing = strcmp(cmd, "list") == 0;
    bool making_dir = strcmp(cmd, "mkdir") == 0;
    bool patching = strcmp(cmd, "overwrite-cache-byte") == 0;
    const char *fat_path = putting && argc == 5 ? argv[4] : "0:/unrelated.bin";
    uint64_t offset = 0, value = 0;
    bool valid = ((reading || writing) && argc == 4) ||
                 ((invalidating || listing) && argc == 3) ||
                 (putting && (argc == 4 || argc == 5) && simplepath(fat_path)) ||
                 (dumping && argc == 5 && simplepath(argv[3])) ||
                 (making_dir && argc == 4 && simplepath(argv[3])) ||
                 (patching && argc == 5 && number(argv[3], &offset) &&
                  number(argv[4], &value) && offset < FRAME_CACHE_FILE_SIZE && value <= 255);
    if (!valid) {
        fprintf(stderr, "invalid command/arguments/simplepath\n");
        return 1;
    }
    image.fd = regular_open(argv[1], O_RDWR, false);
    if (image.fd < 0) return 1;
    struct stat st;
    if (fstat(image.fd, &st) != 0 || st.st_size <= 0 || st.st_size % SECTOR_SIZE ||
        (uint64_t)st.st_size / SECTOR_SIZE > UINT32_MAX) {
        fprintf(stderr, "IMAGE must be a positive sector-aligned regular file (32-bit LBA)\n");
        close(image.fd);
        return 1;
    }
    image.sectors = (uint64_t)st.st_size / SECTOR_SIZE;
    FATFS fs;
    memset(&fs, 0, sizeof(fs));
    FRESULT result = f_mount(&fs, "0:", 1);
    int code = 0;
    unsigned fs_type = fs.fs_type;
    if (result != FR_OK) {
        fprintf(stderr, "MOUNT_FAIL FatFs=%d\n", (int)result);
        code = 2;
    } else if (putting || dumping) {
        code = copy_file(putting ? fat_path : argv[3], putting ? argv[3] : argv[4], putting);
    } else if (listing) {
        code = list_root();
    } else if (making_dir) {
        code = f_mkdir(argv[3]) == FR_OK ? 0 : 7;
    } else if (patching) {
        code = overwrite(offset, value);
    } else {
        BYTE frame[FRAME_CACHE_FRAME_SIZE];
        fat_frame_cache_t cache = {0};
        if (writing && !load_frame(argv[3], frame)) {
            code = 1;
        } else if (!fat_frame_cache_open(&cache, CACHE_PATH)) {
            fprintf(stderr, "OPEN_FAIL\n");
            code = 2;
        } else {
            if (reading) {
                if (!fat_frame_cache_read(&cache, frame, sizeof(frame))) {
                    fprintf(stderr, "CACHE_READ_FAIL\n");
                    code = 4;
                } else if (!save(argv[3], frame, sizeof(frame))) {
                    code = 1;
                }
            } else if (writing) {
                if (!fat_frame_cache_write(&cache, frame, sizeof(frame))) {
                    fprintf(stderr, "CACHE_WRITE_FAIL\n");
                    code = 5;
                }
            } else if (!fat_frame_cache_invalidate(&cache)) {
                fprintf(stderr, "CACHE_INVALIDATE_FAIL\n");
                code = 6;
            }
            if (!fat_frame_cache_close(&cache) && !code) code = 7;
        }
    }
    if (f_mount(NULL, "0:", 0) != FR_OK && !code) code = 7;
    if (fsync(image.fd) != 0) image.failed = true;
    if (close(image.fd) != 0) image.failed = true;
    image.fd = -1;
    if (image.failed) code = 3;
    printf("%s %s code=%d fs_type=%u reads=%" PRIu64 " writes=%" PRIu64
           " read_sectors=%" PRIu64 " write_sectors=%" PRIu64 "\n",
           code ? "FAIL" : "OK", cmd, code, fs_type, image.reads, image.writes,
           image.read_sectors, image.write_sectors);
    return code;
}
