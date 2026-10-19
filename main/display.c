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
                frame[py * (CLOCK_WIDTH / 8) + px / 8] &= ~(0x80 >> (px % 8));
            }
        }
    }
}

void management_render(uint8_t *frame, const char *password, const char *ip)
{
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    text(frame, 48, 36, 5, "LAN HTTP");
    text(frame, 48, 120, 3, "HTTP:");
    text(frame, 174, 120, 3, ip);
    text(frame, 48, 190, 3, "USER: admin");
    text(frame, 48, 260, 3, "PASS:");
    text(frame, 174, 260, 3, password);
    text(frame, 48, 350, 3, "WIFI: ROUTER NETWORK");
    text(frame, 48, 412, 2, "IDLE 5 MIN / MAX 15 MIN");
}

void provision_render(uint8_t *frame, const char *service_name, const char *pop)
{
    memset(frame, 0xff, CLOCK_FRAME_SIZE);
    text(frame, 48, 36, 5, "BLE SETUP");
    text(frame, 48, 112, 3, "NAME:");
    text(frame, 174, 112, 3, service_name);
    text(frame, 48, 180, 3, "CODE:");
    text(frame, 174, 180, 3, pop);
    text(frame, 48, 260, 3, "OPEN PAGES ON PHONE");
    text(frame, 48, 310, 3, "CLICK CONNECT / ENTER CODE");
    text(frame, 48, 360, 3, "NO WIFI HOTSPOT");
    text(frame, 48, 420, 2, "5 MIN TIMEOUT");
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
        text(frame, 70, 45, 3, "SET TIME BLE/USB");
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
