/*
 * ESP-IDF full-frame API for the Waveshare 3.97-inch panel.
 * Implementation source and original Waveshare permission notice: epaper_port.c.
 * Pin assignments and ALDO3 power routing are board hardware facts.
 */
#ifndef EPAPER_PORT_H
#define EPAPER_PORT_H

#include <stdint.h>
#include "esp_err.h"

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

// All display APIs now consume packed 2bpp, 200 bytes/row, MSB pixel first.
// Shades: 0 black, 1 dark gray, 2 light gray, 3 white; full frames only.
// Base/Partial internally threshold 0/1 to black and 2/3 to white.
#define EPD_WIDTH       800
#define EPD_HEIGHT      480
#define EPD_SIZE_MONO   48000
#define EPD_SIZE_2BPP   96000

#ifdef __cplusplus
extern "C" {
#endif

// SPI/GPIO failures retain ESP_ERROR_CHECK; BUSY waits return ESP_ERR_TIMEOUT
// after 20 seconds. Display APIs reject NULL (and unsupported partial windows).
esp_err_t epaper_port_init(void);
esp_err_t EPD_Init(void);
esp_err_t EPD_Init_4GRAY(void);
esp_err_t EPD_Display_Base(const UBYTE *image2bpp);
// Partial waveform, not a subwindow: coordinates must be (0, 0, 800, 480).
// One call is still one partial refresh; caller retains its two-call sequence.
esp_err_t EPD_Display_Partial(const UBYTE *image2bpp, UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend);
// Call EPD_Init_4GRAY before gray display; return to EPD_Init for Mono mode.
esp_err_t EPD_Display_4Gray(const UBYTE *image2bpp);
void EPD_Sleep(void);

#ifdef __cplusplus
}
#endif

#endif // EPAPER_PORT_H
