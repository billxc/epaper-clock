#include "display.h"
#include "ui_font.h"
#include "clock_font.h"
#include "render_mode.h"
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

/* Always packed 2bpp: 0 black, 1 dark gray, 2 light gray, 3 white.
 * Four pixels per byte, leftmost in bits 7..6. */
static void pixel(uint8_t *frame, int x, int y, unsigned shade)
{
    if (x < 0 || x >= CLOCK_WIDTH || y < 0 || y >= CLOCK_HEIGHT) return;
    unsigned shift = 6 - 2 * (x & 3);
    uint8_t *p = &frame[y * (CLOCK_WIDTH / 4) + x / 4];
    *p = (uint8_t)((*p & ~(3u << shift)) | (shade << shift));
}

/* Coverage comes from ORIGINAL alpha, never a mono mask. Blend foreground
 * with the existing background, including white-on-black. Regular/inverse
 * UI backgrounds are solid white/black, giving exactly the study's shades. */
static void blend(uint8_t *frame, int x, int y, unsigned ink, bool inverse)
{
    if (!ink || x < 0 || x >= CLOCK_WIDTH || y < 0 || y >= CLOCK_HEIGHT) return;
    unsigned background = (frame[y * (CLOCK_WIDTH / 4) + x / 4] >> (6 - 2 * (x & 3))) & 3;
    unsigned foreground = inverse ? 3 : 0;
    pixel(frame, x, y, (background * (3 - ink) + foreground * ink + 1) / 3);
}

static void rectangle(uint8_t *frame, int x, int y, int w, int h)
{
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) pixel(frame, x + c, y + r, 0);
}

static void ui_text(uint8_t *frame, int x, int y, const char *value, bool inverse,
                    render_mode_t mode)
{
    if (!value) return;
    while (*value) {
        uint32_t cp = next_codepoint(&value);
        int advance = cp >= 32 && cp < 127 ? UI_FONT_ASCII_ADVANCE : UI_FONT_WIDTH;
        if (x + advance > CLOCK_WIDTH - 32) break;
        const uint8_t *glyph = ui_font_glyph(cp);
        for (int r = 0; r < UI_FONT_HEIGHT; ++r)
            for (int c = 0; c < advance; ++c) {
                unsigned bit = (unsigned)(r * UI_FONT_WIDTH + c) * 3;
                // Read across bytes without reading beyond the glyph.
                unsigned category = 0;
                for (unsigned b = 0; b < 3; ++b)
                    category = (category << 1) | ((glyph[(bit + b) / 8] >> (7 - ((bit + b) & 7))) & 1);
                blend(frame, x + c, y + r, ui_font_ink[mode][category], inverse);
            }
        x += advance;
    }
}

static int ui_text_width(const char *value)
{
    int width = 0;
    while (*value) {
        uint32_t cp = next_codepoint(&value);
        width += cp >= 32 && cp < 127 ? UI_FONT_ASCII_ADVANCE : UI_FONT_WIDTH;
    }
    return width;
}

/* Public entry points snapshot mode once; private drawing never reads global
 * mode, so a concurrent settings change cannot produce a mixed-mode frame. */
static void screen_draw(uint8_t *frame, const char *title,
                        const char *const *lines, size_t count, int selected,
                        render_mode_t mode)
{
    if (!frame) return;
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    ui_text(frame, 32, 24, title, false, mode);
    rectangle(frame, 32, 64, CLOCK_WIDTH - 64, 2);
    if (!lines) count = 0;
    size_t first = selected >= 8 && (size_t)selected < count ? (size_t)selected - 7 : 0;
    for (size_t row = 0; row < 8 && first + row < count; ++row) {
        size_t i = first + row;
        bool highlight = selected >= 0 && (size_t)selected == i;
        int y = 88 + (int)row * 40;
        if (highlight) rectangle(frame, 32, y - 5, CLOCK_WIDTH - 64, 34);
        ui_text(frame, 48, y, lines[i], highlight, mode);
    }
    ui_text(frame, 32, 440, selected >= 0 ? "上／下移动  中键选择" : "长按中键打开选单", false, mode);
}

void screen_render(uint8_t *frame, const char *title,
                   const char *const *lines, size_t count, int selected)
{
    const render_mode_t mode = render_mode_get();
    screen_draw(frame, title, lines, count, selected, mode);
}

void management_render(uint8_t *frame, const char *password, const char *ip)
{
    const render_mode_t mode = render_mode_get();
    char address[96], secret[128];
    snprintf(address, sizeof(address), "地址：https://%s", ip ? ip : "--");
    snprintf(secret, sizeof(secret), "密码：%s", password ? password : "");
    const char *lines[] = {address, "用户名：admin", secret,
        "自签证书，首次独立打开并信任", "连续五分钟无操作后返回时钟"};
    screen_draw(frame, "LAN 管理模式", lines, sizeof(lines) / sizeof(lines[0]), -1, mode);
}

void provision_render(uint8_t *frame, const char *service_name, const char *pop, bool initial)
{
    const render_mode_t mode = render_mode_get();
    char name[96], code[64];
    snprintf(name, sizeof(name), "蓝牙名称：%s", service_name ? service_name : "--");
    snprintf(code, sizeof(code), "校验码：%s", pop ? pop : "--");
    const char *lines[] = {name, code, "手机打开 GitHub Pages 页面",
        "选择连接设备，输入校验码", "可设置 Wi-Fi 或手动设置时间",
        "等待暂停后，短按中键重新开启"};
    screen_draw(frame, initial ? "初始化／配网" : "蓝牙管理模式", lines, sizeof(lines) / sizeof(lines[0]), -1, mode);
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
                        clock_font_t font, const char *value, render_mode_t mode)
{
    while (*value) {
        const clock_glyph_t *glyph = clock_font_glyph(font, next_codepoint(&value));
        if (!glyph) continue;
        for (unsigned row = 0; row < (unsigned)glyph->height + 2 * CLOCK_FONT_PADDING; ++row)
            for (unsigned col = 0; col < (unsigned)glyph->width + 2 * CLOCK_FONT_PADDING; ++col) {
                unsigned ink = (glyph->bitmap[row * glyph->stride + col / 4] >> (6 - 2 * (col & 3))) & 3;
                if (mode == RENDER_MONO) ink = ink >= 2 ? 3 : 0;
                blend(frame, x + glyph->bearing_x + (int)col - CLOCK_FONT_PADDING,
                      baseline + glyph->bearing_y + (int)row - CLOCK_FONT_PADDING, ink, false);
            }
        x += glyph->advance;
    }
}

void clock_render(uint8_t *frame, const struct tm *time, bool valid_time,
                  float temperature, float humidity, int battery)
{
    const render_mode_t mode = render_mode_get();
    if (!frame) return;
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    // A shared 64px side inset; readings occupy two equal-width columns.
    const int margin = 64;
    const int temperature_center = margin + (CLOCK_WIDTH - 2 * margin) / 4;
    const int humidity_center = CLOCK_WIDTH - temperature_center;
    char value[48];
    if (valid_time) {
        strftime(value, sizeof(value), "%Y年%m月%d日", time);
        ui_text(frame, margin, 48, value, false, mode);
        strftime(value, sizeof(value), "%H:%M", time);
    } else {
        ui_text(frame, margin, 48, "尚未设置时间", false, mode);
        strcpy(value, "--:--");
    }
    number_text(frame, (CLOCK_WIDTH - number_width(CLOCK_FONT_TIME, value)) / 2,
                280, CLOCK_FONT_TIME, value, mode);
    ui_text(frame, temperature_center - ui_text_width("温度") / 2, 344, "温度", false, mode);
    ui_text(frame, humidity_center - ui_text_width("湿度") / 2, 344, "湿度", false, mode);
    if (isfinite(temperature)) snprintf(value, sizeof(value), "%.1f℃", temperature);
    else strcpy(value, "--.-℃");
    number_text(frame, temperature_center - number_width(CLOCK_FONT_VALUE, value) / 2,
                408, CLOCK_FONT_VALUE, value, mode);
    if (isfinite(humidity)) snprintf(value, sizeof(value), "%.1f%%", humidity);
    else strcpy(value, "--.-%");
    number_text(frame, humidity_center - number_width(CLOCK_FONT_VALUE, value) / 2,
                408, CLOCK_FONT_VALUE, value, mode);
    if (battery >= 0) snprintf(value, sizeof(value), "电量 %d%%", battery);
    else strcpy(value, "电量 --");
    ui_text(frame, CLOCK_WIDTH - margin - ui_text_width(value), 48, value, false, mode);
}
