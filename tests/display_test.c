#include "display.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char guarded[CLOCK_FRAME_SIZE + 32];
static unsigned char *const frame = guarded + 16;

static void save(const char *directory, const char *name)
{
    for (int i = 0; i < 16; ++i) {
        assert(guarded[i] == 0x5a);
        assert(guarded[16 + CLOCK_FRAME_SIZE + i] == 0x5a);
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.pbm", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P4\n%d %d\n", CLOCK_WIDTH, CLOCK_HEIGHT);
    for (size_t i = 0; i < CLOCK_FRAME_SIZE; ++i) fputc(frame[i] ^ 0xff, file);
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    memset(guarded, 0x5a, sizeof(guarded));
    const char *menu[] = {"时钟模式", "LAN 管理模式", "蓝牙管理模式", "本机自动调试", "立刻自动对时", "初始化／配网"};
    screen_render(frame, "模式选单", menu, 6, 0);
    save(argv[1], "menu");
    const char *confirm[] = {"取消，返回选单", "确认，进入初始化／配网"};
    screen_render(frame, "是否进入初始化／配网？", confirm, 2, 0);
    save(argv[1], "initialize");
    struct tm time = {.tm_year = 126, .tm_mon = 9, .tm_mday = 20, .tm_hour = 23, .tm_min = 59};
    clock_render(frame, &time, true, -40.0f, 100.0f, 100);
    save(argv[1], "clock");
    puts("PASS: Chinese menu/confirmation/clock rendering and framebuffer boundaries");
    return 0;
}
