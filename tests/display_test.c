/* Native/off-device test, no ESP-IDF required:
 * cc -std=c11 -Wall -Wextra -Werror -Imain tests/display_test.c main/display.c \
 *    main/ui_font.c main/clock_font.c main/render_mode.c -lm -o display_test
 * mkdir -p OUTPUT; ./display_test OUTPUT [OLD_1BPP_PBM_DIRECTORY]
 * Optional baseline directory contains the original test's five PBMs. Each
 * mono output is compared pixel-for-pixel (including inverse menu text).
 */
#include "display.h"
#include "render_mode.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert(CLOCK_FRAME_SIZE == 96000, "packed 2bpp frame size");
_Static_assert(CLOCK_MONO_FRAME_SIZE == 48000, "legacy mono frame size");
static unsigned char guarded[CLOCK_FRAME_SIZE + 32];
static unsigned char *const frame = guarded + 16;
static unsigned char mono[7][CLOCK_MONO_FRAME_SIZE];
static const char *const names[] = {"menu", "initialize", "clock", "clock-extreme", "clock-unset", "lan", "provision"};
static const char *const modes[] = {"mono", "linear", "ink-match"};

static unsigned shade(size_t pixel)
{
    return (frame[pixel / 4] >> (6 - 2 * (pixel & 3))) & 3;
}

static void save(const char *directory, const char *baseline, unsigned scene, render_mode_t mode)
{
    for (int i = 0; i < 16; ++i) {
        assert(guarded[i] == 0x5a);
        assert(guarded[16 + CLOCK_FRAME_SIZE + i] == 0x5a);
    }
    unsigned gray_pixels = 0;
    for (size_t p = 0; p < CLOCK_WIDTH * CLOCK_HEIGHT; ++p) {
        unsigned s = shade(p);
        gray_pixels += s == 1 || s == 2;
        unsigned char mask = 0x80u >> (p & 7);
        if (mode == RENDER_MONO) {
            assert(s == 0 || s == 3);
            if (!s) mono[scene][p / 8] |= mask;
        } else if (mode == RENDER_INK_MATCH) {
            // Ink-match must retain every original mono contour, including
            // the UI alpha>=96 mask and clock alpha>=128 mask.
            assert((s < 2) == !!(mono[scene][p / 8] & mask));
        }
    }
    if (mode != RENDER_MONO) assert(gray_pixels > 0);
    char path[512];
    if (baseline && mode == RENDER_MONO && scene < 5) {
        snprintf(path, sizeof(path), "%s/%s.pbm", baseline, names[scene]);
        FILE *old = fopen(path, "rb");
        assert(old);
        char header[3];
        int width, height;
        assert(fscanf(old, "%2s%d%d", header, &width, &height) == 3);
        assert(strcmp(header, "P4") == 0 && width == CLOCK_WIDTH && height == CLOCK_HEIGHT);
        assert(fgetc(old) == '\n');
        for (size_t i = 0; i < CLOCK_MONO_FRAME_SIZE; ++i)
            assert(fgetc(old) == mono[scene][i]);
        assert(fgetc(old) == EOF);
        assert(fclose(old) == 0);
    }
    snprintf(path, sizeof(path), "%s/%s-%s.%s", directory, modes[mode], names[scene],
             mode == RENDER_MONO ? "pbm" : "pgm");
    FILE *file = fopen(path, "wb");
    assert(file);
    if (mode == RENDER_MONO) {
        fprintf(file, "P4\n%d %d\n", CLOCK_WIDTH, CLOCK_HEIGHT);
        assert(fwrite(mono[scene], 1, CLOCK_MONO_FRAME_SIZE, file) == CLOCK_MONO_FRAME_SIZE);
    } else {
        fprintf(file, "P5\n%d %d\n255\n", CLOCK_WIDTH, CLOCK_HEIGHT);
        for (size_t p = 0; p < CLOCK_WIDTH * CLOCK_HEIGHT; ++p) assert(fputc(shade(p) * 85, file) != EOF);
    }
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    assert(render_mode_get() == RENDER_MONO);
    memset(guarded, 0x5a, sizeof(guarded));
    const char *baseline = argc == 3 ? argv[2] : NULL;
    const char *menu[] = {"时钟模式", "LAN 管理模式", "蓝牙管理模式", "本机自动调试", "立刻自动对时", "初始化／配网"};
    const char *confirm[] = {"取消，返回选单", "确认，进入初始化／配网"};
    struct tm time = {.tm_year = 126, .tm_mon = 9, .tm_mday = 20, .tm_hour = 23, .tm_min = 59};
    for (render_mode_t mode = RENDER_MONO; mode <= RENDER_INK_MATCH; ++mode) {
        render_mode_set(mode);
        assert(render_mode_get() == mode);
        screen_render(frame, "模式选单", menu, 6, 0);
        save(argv[1], baseline, 0, mode);
        screen_render(frame, "是否进入初始化／配网？", confirm, 2, 0);
        save(argv[1], baseline, 1, mode);
        clock_render(frame, &time, true, 23.6f, 58.2f, 85);
        save(argv[1], baseline, 2, mode);
        clock_render(frame, &time, true, -40.0f, 100.0f, 100);
        save(argv[1], baseline, 3, mode);
        clock_render(frame, NULL, false, NAN, NAN, -1);
        save(argv[1], baseline, 4, mode);
        management_render(frame, "demo-only-1234567", "192.0.2.10");
        save(argv[1], baseline, 5, mode);
        provision_render(frame, "PROV_EPAPER_A1B2C3", "012345", true);
        save(argv[1], baseline, 6, mode);
    }
    render_mode_set(RENDER_MONO);
    puts("PASS: three render modes, 21 previews, mono contours and framebuffer boundaries");
    if (baseline) puts("PASS: five monochrome scenes match original 1bpp C baseline byte-for-byte");
    return 0;
}
