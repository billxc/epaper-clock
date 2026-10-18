#include "display.h"
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
                frame[py * (CLOCK_WIDTH / 8) + px / 8] &= ~(0x80 >> (px % 8));
            }
        }
    }
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
        text(frame, 70, 45, 3, "SET TIME VIA USB");
        strcpy(value, "--:--");
    }
    text(frame, 70, 130, 22, value);
    text(frame, 70, 335, 3, "TEMP");
    text(frame, 430, 335, 3, "HUMIDITY");
    if (isfinite(temperature)) snprintf(value, sizeof(value), "%.1f C", temperature);
    else strcpy(value, "--.- C");
    text(frame, 70, 380, 5, value);
    if (isfinite(humidity)) snprintf(value, sizeof(value), "%.1f%%", humidity);
    else strcpy(value, "--.-%");
    text(frame, 430, 380, 5, value);
    if (battery >= 0) {
        snprintf(value, sizeof(value), "BAT %d%%", battery);
        text(frame, 570, 50, 3, value);
    }
}
