#pragma once
#include <stdint.h>

/* Noto Sans SC / SIL OFL 1.1. Packed 24x24, three bytes/row, MSB first,
 * 1 = ink. Printable ASCII, GB2312 common Han and CJK punctuation.
 * Unsupported Unicode returns a visible replacement box. Static flash data. */
#define UI_FONT_WIDTH 24
#define UI_FONT_HEIGHT 24
#define UI_FONT_ASCII_ADVANCE 14
const uint8_t *ui_font_glyph(uint32_t codepoint);
