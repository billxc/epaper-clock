/* Packed framebuffer conversion; independent of ESP-IDF. */
#ifndef FRAME_CODEC_H
#define FRAME_CODEC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum epd_plane_t {
    EPD_PLANE_MONO,
    EPD_PLANE_GRAY_LOW,  /* Controller 0x24: inverted shade bit 0. */
    EPD_PLANE_GRAY_HIGH  /* Controller 0x26: inverted shade bit 1. */
} epd_plane_t;

/* Two packed bytes = eight pixels, most significant pixel first.
 * Shades: 0 black, 1 dark gray, 2 light gray, 3 white.
 * Mono uses shade bit 1 (0/1 black, 2/3 white); gray polarity is inverted.
 */
uint8_t epd_encode_plane_byte(uint8_t first, uint8_t second, epd_plane_t plane);
/* Reads 2 * mono_bytes; writes mono_bytes. Forward in-place compaction is OK. */
void epd_encode_mono(const uint8_t *packed, uint8_t *mono, size_t mono_bytes);
/* Needs capacity 2 * mono_bytes; reverse in-place expansion: 0 black, 3 white. */
void epd_expand_mono(uint8_t *inplace, size_t mono_bytes);

#ifdef __cplusplus
}
#endif
#endif
