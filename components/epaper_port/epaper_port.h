/*
 * ESP-IDF full-frame API for the Waveshare 3.97-inch panel.
 * Implementation source and original Waveshare permission notice: epaper_port.c.
 * Pin assignments and ALDO3 power routing are board hardware facts.
 */
#ifndef EPAPER_PORT_H
#define EPAPER_PORT_H

#include <stdint.h>

typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t UDOUBLE;

// GPIO configuration
#define EPD_SCLK_PIN    11
#define EPD_MOSI_PIN    12
#define EPD_CS_PIN      10
#define EPD_DC_PIN      9
#define EPD_RST_PIN     46
#define EPD_BUSY_PIN    3

// Packed 1-bpp, 100 bytes/row; every display call consumes a full frame.
#define EPD_WIDTH       800
#define EPD_HEIGHT      480
#define EPD_SIZE_MONO   48000

#ifdef __cplusplus
extern "C" {
#endif

void epaper_port_init(void);
void EPD_Init(void);
void EPD_Display_Base(const UBYTE *Image);
// Partial waveform, not a subwindow: coordinates must be (0, 0, 800, 480).
void EPD_Display_Partial(const UBYTE *Image, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend);
void EPD_Sleep(void);

#ifdef __cplusplus
}
#endif

#endif // EPAPER_PORT_H
