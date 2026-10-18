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

// Monochrome display resolution and framebuffer size in bytes
#define EPD_WIDTH       800
#define EPD_HEIGHT      480
#define EPD_SIZE_MONO   48000

#ifdef __cplusplus
extern "C" {
#endif

void epaper_port_init(void);
void EPD_Init(void);
void EPD_Display_Base(const UBYTE *Image);
void EPD_Display_Partial(const UBYTE *Image, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend);
void EPD_Sleep(void);

#ifdef __cplusplus
}
#endif

#endif // EPAPER_PORT_H
