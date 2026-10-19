#include "display.h"
#include "ui_font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// Original 5x7 bitmap glyphs; all assets are embedded, no external font files.
static const struct { char character; uint8_t rows[7]; } glyphs[] = {
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {':',{0,4,4,0,4,4,0}}, {'-',{0,0,0,31,0,0,0}},
    {'.',{0,0,0,0,0,12,12}}, {'%',{17,2,4,8,16,17,0}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'P',{30,17,17,30,16,16,16}},
    {'R',{30,17,17,30,20,18,17}}, {'S',{15,16,16,14,1,1,30}},
    {'T',{31,4,4,4,4,4,4}}, {'U',{17,17,17,17,17,17,14}},
    {'V',{17,17,17,17,17,10,4}}, {'Y',{17,17,10,4,4,4,4}},
    {'F',{31,16,16,30,16,16,16}}, {'G',{14,17,16,23,17,17,15}},
    {'N',{17,25,25,21,19,19,17}}, {'O',{14,17,17,17,17,17,14}},
    {'W',{17,17,17,21,21,27,17}}, {'X',{17,17,10,4,10,17,17}},
    {'a',{0,0,14,1,15,17,15}}, {'b',{16,16,30,17,17,17,30}},
    {'c',{0,0,14,17,16,17,14}}, {'d',{1,1,15,17,17,17,15}},
    {'e',{0,0,14,17,31,16,14}}, {'f',{6,9,8,28,8,8,8}},
    {'i',{4,0,12,4,4,4,14}}, {'m',{0,0,26,21,21,21,21}},
    {'n',{0,0,30,17,17,17,17}}, {'p',{0,0,30,17,30,16,16}},
    {'r',{0,0,22,25,16,16,16}}, {'/',{1,2,2,4,8,8,16}},
    {'K',{17,18,20,24,20,18,17}}, {'_',{0,0,0,0,0,0,31}},
};

static void text(uint8_t *frame, int x, int y, int scale, const char *value)
{
    for (; *value; ++value, x += 6 * scale) {
        const uint8_t *rows = NULL;
        for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i)
            if (glyphs[i].character == *value) { rows = glyphs[i].rows; break; }
        if (!rows) continue; // Space.
        for (int r = 0; r < 7; ++r) for (int c = 0; c < 5; ++c) {
            if (!(rows[r] & (1 << (4 - c)))) continue;
            for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx) {
                int px = x + c * scale + dx, py = y + r * scale + dy;
                if (px >= 0 && px < CLOCK_WIDTH && py >= 0 && py < CLOCK_HEIGHT)
                    frame[py * (CLOCK_WIDTH / 8) + px / 8] &= ~(0x80 >> (px % 8));
            }
        }
    }
}

/* Strict UTF-8 decoder: malformed bytes make visible placeholders rather than
 * indexing outside the font or reading past a truncated string. */
static uint32_t next_codepoint(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    uint32_t cp = *p++;
    *cursor = (const char *)p;
    if (cp < 0x80) return cp;
    unsigned remaining;
    uint32_t minimum;
    if (cp >= 0xc2 && cp <= 0xdf) { remaining = 1; minimum = 0x80; cp &= 0x1f; }
    else if (cp >= 0xe0 && cp <= 0xef) { remaining = 2; minimum = 0x800; cp &= 0x0f; }
    else if (cp >= 0xf0 && cp <= 0xf4) { remaining = 3; minimum = 0x10000; cp &= 7; }
    else return 0xfffd;
    for (unsigned i = 0; i < remaining; ++i) {
        if ((p[i] & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    *cursor = (const char *)(p + remaining);
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0xfffd;
    return cp;
}

static void pixel(uint8_t *frame, int x, int y, bool black)
{
    if (x < 0 || x >= CLOCK_WIDTH || y < 0 || y >= CLOCK_HEIGHT) return;
    uint8_t mask = 0x80u >> (x & 7);
    uint8_t *p = &frame[y * (CLOCK_WIDTH / 8) + x / 8];
    if (black) *p &= (uint8_t)~mask;
    else *p |= mask;
}

static void rectangle(uint8_t *frame, int x, int y, int w, int h)
{
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) pixel(frame, x + c, y + r, true);
}

static void ui_text(uint8_t *frame, int x, int y, const char *value, bool inverse)
{
    if (!value) return;
    while (*value) {
        uint32_t cp = next_codepoint(&value);
        int advance = cp >= 32 && cp < 127 ? UI_FONT_ASCII_ADVANCE : UI_FONT_WIDTH;
        if (x + advance > CLOCK_WIDTH - 32) break;
        const uint8_t *glyph = ui_font_glyph(cp);
        for (int r = 0; r < UI_FONT_HEIGHT; ++r)
            for (int c = 0; c < advance; ++c)
                if (glyph[r * 3 + c / 8] & (0x80u >> (c & 7)))
                    pixel(frame, x + c, y + r, !inverse);
        x += advance;
    }
}

void screen_render(uint8_t *frame, const char *title,
                   const char *const *lines, size_t count, int selected)
{
    if (!frame) return;
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    ui_text(frame, 32, 24, title, false);
    rectangle(frame, 32, 64, CLOCK_WIDTH - 64, 2);
    if (!lines) count = 0;
    size_t first = selected >= 8 && (size_t)selected < count ? (size_t)selected - 7 : 0;
    for (size_t row = 0; row < 8 && first + row < count; ++row) {
        size_t i = first + row;
        bool highlight = selected >= 0 && (size_t)selected == i;
        int y = 88 + (int)row * 40;
        if (highlight) rectangle(frame, 32, y - 5, CLOCK_WIDTH - 64, 34);
        ui_text(frame, 48, y, lines[i], highlight);
    }
    ui_text(frame, 32, 440, selected >= 0 ? "上／下移动  中键选择" : "长按中键打开选单", false);
}

void management_render(uint8_t *frame, const char *password, const char *ip)
{
    char address[96], secret[128];
    snprintf(address, sizeof(address), "地址：http://%s", ip ? ip : "--");
    snprintf(secret, sizeof(secret), "密码：%s", password ? password : "");
    const char *lines[] = {address, "用户名：admin", secret,
        "使用已保存的 Wi-Fi 连接局域网", "连续五分钟无操作后返回时钟"};
    screen_render(frame, "LAN 管理模式", lines, sizeof(lines) / sizeof(lines[0]), -1);
}

void provision_render(uint8_t *frame, const char *service_name, const char *pop, bool initial)
{
    char name[96], code[64];
    snprintf(name, sizeof(name), "蓝牙名称：%s", service_name ? service_name : "--");
    snprintf(code, sizeof(code), "校验码：%s", pop ? pop : "--");
    const char *lines[] = {name, code, "手机打开 GitHub Pages 页面",
        "选择连接设备，输入校验码", "可设置 Wi-Fi 或手动设置时间",
        "等待暂停后，短按中键重新开启"};
    screen_render(frame, initial ? "初始化／配网" : "蓝牙管理模式", lines, sizeof(lines) / sizeof(lines[0]), -1);
}

void clock_render(uint8_t *frame, const struct tm *time, bool valid_time,
                  float temperature, float humidity, int battery)
{
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    char value[32];
    if (valid_time) {
        strftime(value, sizeof(value), "%Y-%m-%d", time);
        text(frame, 70, 45, 4, value);
        strftime(value, sizeof(value), "%H:%M", time);
    } else {
        ui_text(frame, 70, 45, "请通过蓝牙或 USB 设置时间", false);
        strcpy(value, "--:--");
    }
    text(frame, 70, 130, 22, value);
    ui_text(frame, 70, 335, "温度", false);
    ui_text(frame, 430, 335, "湿度", false);
    if (isfinite(temperature)) snprintf(value, sizeof(value), "%.1f C", temperature);
    else strcpy(value, "--.- C");
    text(frame, 70, 380, 5, value);
    if (isfinite(humidity)) snprintf(value, sizeof(value), "%.1f%%", humidity);
    else strcpy(value, "--.-%");
    text(frame, 430, 380, 5, value);
    if (battery >= 0) {
        snprintf(value, sizeof(value), "电量 %d%%", battery);
        ui_text(frame, 570, 50, value, false);
    }
    ui_text(frame, 70, 440, "PWR 开机后长按中键打开选单", false);
}
