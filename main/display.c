#include "display.h"
#include "ui_font.h"
#include "clock_font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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

static int number_width(clock_font_t font, const char *value)
{
    int width = 0;
    while (*value) {
        const clock_glyph_t *glyph = clock_font_glyph(font, next_codepoint(&value));
        if (glyph) width += glyph->advance;
    }
    return width;
}

static void number_text(uint8_t *frame, int x, int baseline,
                        clock_font_t font, const char *value)
{
    while (*value) {
        const clock_glyph_t *glyph = clock_font_glyph(font, next_codepoint(&value));
        if (!glyph) continue;
        for (unsigned row = 0; row < glyph->height; ++row)
            for (unsigned col = 0; col < glyph->width; ++col)
                if (glyph->bitmap[row * glyph->stride + col / 8] & (0x80u >> (col & 7)))
                    pixel(frame, x + glyph->bearing_x + (int)col,
                          baseline + glyph->bearing_y + (int)row, true);
        x += glyph->advance;
    }
}

void clock_render(uint8_t *frame, const struct tm *time, bool valid_time,
                  float temperature, float humidity, int battery)
{
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    char value[48];
    if (valid_time) {
        strftime(value, sizeof(value), "%Y年%m月%d日", time);
        ui_text(frame, 52, 30, value, false);
        strftime(value, sizeof(value), "%H:%M", time);
    } else {
        ui_text(frame, 52, 30, "尚未设置时间", false);
        strcpy(value, "--:--");
    }
    number_text(frame, (CLOCK_WIDTH - number_width(CLOCK_FONT_TIME, value)) / 2,
                280, CLOCK_FONT_TIME, value);
    rectangle(frame, 52, 78, 696, 1);
    rectangle(frame, 52, 320, 696, 1);
    ui_text(frame, 56, 338, "温度", false);
    ui_text(frame, 448, 338, "湿度", false);
    if (isfinite(temperature)) snprintf(value, sizeof(value), "%.1f℃", temperature);
    else strcpy(value, "--.-℃");
    number_text(frame, 56, 408, CLOCK_FONT_VALUE, value);
    if (isfinite(humidity)) snprintf(value, sizeof(value), "%.1f%%", humidity);
    else strcpy(value, "--.-%");
    number_text(frame, 448, 408, CLOCK_FONT_VALUE, value);
    if (battery >= 0) snprintf(value, sizeof(value), "电量 %d%%", battery);
    else strcpy(value, "电量 --");
    ui_text(frame, 610, 30, value, false);
}
