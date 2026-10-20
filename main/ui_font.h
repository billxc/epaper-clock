#pragma once
#include <stdint.h>

/* Noto Sans SC / SIL OFL 1.1. One 216-byte 24x24 bitmap per glyph.
 * Each row is 9 bytes, MSB-first 3-bit coverage classes (not grayscale).
 * ui_font_ink[render_mode][class] gives quantized coverage: 0..3.
 * Mono coverage is only 0/3; normal shade=3-coverage, inverse=coverage.
 * Printable ASCII, GB2312 Han and CJK punctuation. Unknown -> visible box.
 * Returned bitmaps and lookup table have static lifetime in flash. */
#define UI_FONT_WIDTH 24
#define UI_FONT_HEIGHT 24
#define UI_FONT_ASCII_ADVANCE 14
#define UI_FONT_CLASS_COUNT 7
extern const uint8_t ui_font_ink[3][UI_FONT_CLASS_COUNT];
const uint8_t *ui_font_glyph(uint32_t codepoint);
