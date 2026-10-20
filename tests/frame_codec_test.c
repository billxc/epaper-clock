/* Host: cc -std=c11 -Wall -Wextra -Werror -Icomponents/epaper_port \
 * tests/frame_codec_test.c components/epaper_port/frame_codec.c -o /tmp/frame_codec_test
 */
#include "frame_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Independent pixel-wise oracle for the manufacturer's Display4Gray mapping. */
static uint8_t reference(uint16_t pixels, epd_plane_t plane)
{
    static const uint8_t mono[] = {0, 0, 1, 1};
    static const uint8_t low[] = {1, 0, 1, 0};  /* 0x24 */
    static const uint8_t high[] = {1, 1, 0, 0}; /* 0x26 */
    const uint8_t *map = plane == EPD_PLANE_MONO ? mono :
                         plane == EPD_PLANE_GRAY_LOW ? low : high;
    uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result = (uint8_t)((result << 1) | map[(pixels >> (14 - 2 * i)) & 3]);
    }
    return result;
}

int main(void)
{
    /* Pixels 0,1,2,3,0,1,2,3: explicitly check ordering and opposite polarity. */
    assert(epd_encode_plane_byte(0x1b, 0x1b, EPD_PLANE_MONO) == 0x33);
    assert(epd_encode_plane_byte(0x1b, 0x1b, EPD_PLANE_GRAY_LOW) == 0xaa);
    assert(epd_encode_plane_byte(0x1b, 0x1b, EPD_PLANE_GRAY_HIGH) == 0xcc);
    for (unsigned pair = 0; pair < 65536; ++pair) {
        for (int p = EPD_PLANE_MONO; p <= EPD_PLANE_GRAY_HIGH; ++p) {
            assert(epd_encode_plane_byte((uint8_t)(pair >> 8), (uint8_t)pair,
                                        (epd_plane_t)p) ==
                   reference((uint16_t)pair, (epd_plane_t)p));
        }
    }

    /* All 256 mono bytes; guarded buffers verify expansion/compaction bounds. */
    uint8_t guarded[514], mono[256];
    guarded[0] = 0xa5;
    guarded[513] = 0x5a;
    uint8_t *packed = guarded + 1;
    for (unsigned i = 0; i < 256; ++i) packed[i] = (uint8_t)i;
    epd_expand_mono(packed, 256);
    for (unsigned i = 0; i < 256; ++i) {
        for (unsigned pixel = 0; pixel < 8; ++pixel) {
            unsigned actual = (packed[i * 2 + pixel / 4] >>
                               (6 - 2 * (pixel % 4))) & 3;
            assert(actual == (((i >> (7 - pixel)) & 1) ? 3u : 0u));
        }
    }
    epd_encode_mono(packed, mono, sizeof(mono));
    for (unsigned i = 0; i < 256; ++i) assert(mono[i] == i);
    epd_encode_mono(packed, packed, sizeof(mono));
    assert(memcmp(packed, mono, sizeof(mono)) == 0);
    assert(guarded[0] == 0xa5 && guarded[513] == 0x5a);

    uint8_t single[2] = {0x81, 0};
    epd_expand_mono(single, 1);
    assert(single[0] == 0xc0 && single[1] == 0x03);
    epd_expand_mono(single, 0);
    epd_encode_mono(single, mono, 0);
    assert(single[0] == 0xc0 && single[1] == 0x03);
    puts("frame_codec_test: OK");
    return 0;
}
