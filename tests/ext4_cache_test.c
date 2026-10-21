/* Native regular-image harness, not an ESP/SDK board simulation.
 * cc -std=c11 -Wall -Wextra -Werror tests/ext4_cache_test.c \
 *    components/sdcard_bsp/ext4_cache.c -o /tmp/ext4_cache_test
 * Usage: ext4_cache_test IMAGE read|write|invalidate [FRAME_FILE]
 *        [--fail-after-writes N]
 * FAIL_AFTER_WRITES=N also fails the Nth and all later write callbacks (N>=1).
 * stdout reports callback request attempts: open_reads (discovery/metadata),
 * data_reads/data_writes (operation), plus total writes for fault injection.
 * open_read_sectors/data_read_sectors/data_write_sectors count requested
 * 512-byte sectors, including a failed/injected request, not completed I/O.
 * Exit codes: 1 arguments/frame file, 2 core OPEN_FAIL, 3 sector IO_FAIL,
 *             4 CACHE_READ_FAIL, 5 CACHE_WRITE_FAIL, 6 CACHE_INVALIDATE_FAIL.
 */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "../components/sdcard_bsp/ext4_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint64_t sectors;
    uint64_t reads;
    uint64_t writes;
    uint64_t read_sectors;
    uint64_t write_sectors;
    uint64_t fail_after;
    bool io_failed;
} image_t;

/* Check before open as well as after: never intentionally open a device or
 * FIFO. O_NOFOLLOW also rejects final-component symlinks on supported hosts. */
static int open_regular(const char *path, int flags, bool create)
{
    struct stat before, after;
    bool existed = lstat(path, &before) == 0;
    if ((!existed && (errno != ENOENT || !create)) ||
        (existed && !S_ISREG(before.st_mode))) {
        fprintf(stderr, "regular file required: %s\n", path);
        return -1;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    if (!existed)
        flags |= O_CREAT | O_EXCL;
    int fd = open(path, flags, 0600);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    if (fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
        (existed && (before.st_dev != after.st_dev || before.st_ino != after.st_ino))) {
        fprintf(stderr, "regular file changed during open: %s\n", path);
        close(fd);
        return -1;
    }
    return fd;
}

static bool sectors_read(void *context, uint64_t lba, size_t count, void *data)
{
    image_t *image = context;
    ++image->reads;
    image->read_sectors += count;
    if (count == 0 || count > EXT4_CACHE_IO_SECTORS ||
        lba >= image->sectors || count > image->sectors - lba) {
        image->io_failed = true;
        fprintf(stderr, "IO_FAIL read LBA=%" PRIu64 " count=%zu out of bounds\n", lba, count);
        return false;
    }
    size_t bytes = count * EXT4_CACHE_SECTOR_SIZE;
    ssize_t n;
    do {
        n = pread(image->fd, data, bytes,
                  (off_t)(lba * EXT4_CACHE_SECTOR_SIZE));
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)bytes) {
        image->io_failed = true;
        fprintf(stderr, "IO_FAIL read LBA=%" PRIu64 " count=%zu bytes=%zd\n", lba, count, n);
        return false;
    }
    return true;
}

static bool sectors_write(void *context, uint64_t lba, size_t count, const void *data)
{
    image_t *image = context;
    ++image->writes;
    image->write_sectors += count;
    if ((image->fail_after && image->writes >= image->fail_after) ||
        count == 0 || count > EXT4_CACHE_IO_SECTORS ||
        lba >= image->sectors || count > image->sectors - lba) {
        image->io_failed = true;
        fprintf(stderr, "IO_FAIL write LBA=%" PRIu64 " count=%zu callback=%" PRIu64 "%s\n",
                lba, count, image->writes,
                image->fail_after && image->writes >= image->fail_after ? " injected" : " out of bounds");
        return false;
    }
    size_t bytes = count * EXT4_CACHE_SECTOR_SIZE;
    ssize_t n;
    do {
        n = pwrite(image->fd, data, bytes,
                   (off_t)(lba * EXT4_CACHE_SECTOR_SIZE));
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)bytes) {
        image->io_failed = true;
        fprintf(stderr, "IO_FAIL write LBA=%" PRIu64 " count=%zu bytes=%zd\n", lba, count, n);
        return false;
    }
    return true;
}

static bool positive_number(const char *text, uint64_t *value)
{
    if (!text || !*text)
        return false;
    for (const char *p = text; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    errno = 0;
    unsigned long long n = strtoull(text, NULL, 10);
    if (errno || !n || n > UINT64_MAX)
        return false;
    *value = (uint64_t)n;
    return true;
}

static bool load_frame(const char *path, uint8_t *frame)
{
    int fd = open_regular(path, O_RDONLY, false);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && st.st_size == EXT4_CACHE_FRAME_SIZE;
    size_t done = 0;
    while (ok && done < EXT4_CACHE_FRAME_SIZE) {
        ssize_t n = read(fd, frame + done, EXT4_CACHE_FRAME_SIZE - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        done += (size_t)n;
    }
    /* Also reject a file that grew after the size check. */
    uint8_t extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    ok = ok && n == 0;
    close(fd);
    if (!ok)
        fprintf(stderr, "FRAME_FAIL input must contain exactly %u bytes\n", EXT4_CACHE_FRAME_SIZE);
    return ok;
}

static bool save_frame(const char *path, const uint8_t *frame, int image_fd)
{
    /* Do not truncate until we know this is not the image (including hardlinks). */
    int fd = open_regular(path, O_WRONLY, true);
    if (fd < 0)
        return false;
    struct stat output, image;
    bool ok = fstat(fd, &output) == 0 && fstat(image_fd, &image) == 0;
    if (ok && output.st_dev == image.st_dev && output.st_ino == image.st_ino)
        ok = false;
    if (ok)
        ok = ftruncate(fd, 0) == 0;
    size_t done = 0;
    while (ok && done < EXT4_CACHE_FRAME_SIZE) {
        ssize_t n = write(fd, frame + done, EXT4_CACHE_FRAME_SIZE - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        done += (size_t)n;
    }
    if (ok)
        ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (!ok)
        fprintf(stderr, "FRAME_FAIL output (must not alias IMAGE)\n");
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s IMAGE read|write|invalidate [FRAME_FILE] [--fail-after-writes N]\n", argv[0]);
        return 1;
    }
    bool reading = strcmp(argv[2], "read") == 0;
    bool writing = strcmp(argv[2], "write") == 0;
    bool invalidating = strcmp(argv[2], "invalidate") == 0;
    if (!reading && !writing && !invalidating) {
        fprintf(stderr, "unknown command: %s\n", argv[2]);
        return 1;
    }
    image_t image = {0};
    const char *failure = getenv("FAIL_AFTER_WRITES");
    if (failure && !positive_number(failure, &image.fail_after)) {
        fprintf(stderr, "FAIL_AFTER_WRITES must be a positive integer\n");
        return 1;
    }
    int next = 3;
    const char *frame_path = NULL;
    if (!invalidating) {
        if (next >= argc) {
            fprintf(stderr, "FRAME_FILE required\n");
            return 1;
        }
        frame_path = argv[next++];
    }
    if (next < argc) {
        if (argc - next != 2 || strcmp(argv[next], "--fail-after-writes") != 0 ||
            !positive_number(argv[next + 1], &image.fail_after)) {
            fprintf(stderr, "expected --fail-after-writes N (N>=1)\n");
            return 1;
        }
    }
    image.fd = open_regular(argv[1], reading ? O_RDONLY : O_RDWR, false);
    if (image.fd < 0)
        return 1;
    struct stat st;
    if (fstat(image.fd, &st) != 0 || st.st_size <= 0 ||
        st.st_size % EXT4_CACHE_SECTOR_SIZE != 0) {
        fprintf(stderr, "IMAGE must have a positive, sector-aligned size\n");
        close(image.fd);
        return 1;
    }
    image.sectors = (uint64_t)st.st_size / EXT4_CACHE_SECTOR_SIZE;
    uint8_t *frame = malloc(EXT4_CACHE_FRAME_SIZE);
    if (!frame) {
        close(image.fd);
        return 1;
    }
    int code = 0;
    uint64_t open_reads = 0, open_writes = 0;
    uint64_t open_read_sectors = 0, open_write_sectors = 0;
    if (writing && !load_frame(frame_path, frame)) {
        code = 1;
        goto done;
    }
    ext4_cache_t cache = {0};
    bool opened = ext4_cache_open(&cache, sectors_read, sectors_write, &image, image.sectors);
    open_reads = image.reads;
    open_writes = image.writes;
    open_read_sectors = image.read_sectors;
    open_write_sectors = image.write_sectors;
    if (!opened) {
        fprintf(stderr, "OPEN_FAIL%s\n", image.io_failed ? " IO_FAIL" : "");
        code = image.io_failed ? 3 : 2;
        goto done;
    }
    bool ok;
    if (reading) {
        ok = ext4_cache_read(&cache, frame, EXT4_CACHE_FRAME_SIZE);
        if (!ok) {
            fprintf(stderr, "CACHE_READ_FAIL%s\n", image.io_failed ? " IO_FAIL" : "");
            code = image.io_failed ? 3 : 4;
        } else if (!save_frame(frame_path, frame, image.fd)) {
            code = 1;
        }
    } else {
        ok = writing ? ext4_cache_write(&cache, frame, EXT4_CACHE_FRAME_SIZE)
                     : ext4_cache_invalidate(&cache);
        if (!ok) {
            fprintf(stderr, "%s%s\n", writing ? "CACHE_WRITE_FAIL" : "CACHE_INVALIDATE_FAIL",
                    image.io_failed ? " IO_FAIL" : "");
            code = image.io_failed ? 3 : (writing ? 5 : 6);
        }
    }

done:
    /* Preserve completed sectors even when a later callback was interrupted. */
    if (image.writes && fsync(image.fd) != 0) {
        perror("IO_FAIL fsync");
        code = 3;
    }
    if (close(image.fd) != 0) {
        perror("IO_FAIL close");
        code = 3;
    }
    free(frame);
    printf("%s %s code=%d open_reads=%" PRIu64 " data_reads=%" PRIu64
           " data_writes=%" PRIu64 " open_read_sectors=%" PRIu64
           " data_read_sectors=%" PRIu64 " data_write_sectors=%" PRIu64
           " writes=%" PRIu64 "\n",
           code == 0 ? "OK" : "FAIL", argv[2], code, open_reads,
           image.reads - open_reads, image.writes - open_writes, open_read_sectors,
           image.read_sectors - open_read_sectors,
           image.write_sectors - open_write_sectors, image.writes);
    return code;
}
