/*
 * ESP-IDF full-frame port derived from Waveshare team's EPD_3in97.cpp:
 * https://github.com/waveshareteam/e-Paper/blob/master/E-paper_Separate_Program/3in97_e-Paper/Arduino_R4/EPD_3in97.cpp
 * Baseline: /tmp/epaper397-licensed-driver.cpp, SHA-256:
 * c66943c810f9b0b421f28dd084ee24c66e9a7275a159e58160024d804f2fa898
 * Board adaptation: SPI3/20MHz, ALDO3, fixed full-frame transfers, descending
 * partial Y window, reset 50/2/50ms, busy 100ms + 20ms polling, power timing.
 * Upstream permission notice (including its original spelling) follows.
 * Author: Waveshare team
 *
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
 */
#include "epaper_port.h"
#include "frame_codec.h"
#include "axp_prot.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static spi_device_handle_t panel_spi;
#define DELAY_MS(ms) vTaskDelay(pdMS_TO_TICKS(ms))
#define LE16(v) (uint8_t)((v) & 0xff), (uint8_t)(((v) >> 8) & 0xff)
#define REG(cmd, ...) write_register(cmd, (const uint8_t[]){__VA_ARGS__}, \
                                    sizeof((const uint8_t[]){__VA_ARGS__}))

esp_err_t epaper_port_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = EPD_MOSI_PIN, .miso_io_num = -1,
        .sclk_io_num = EPD_SCLK_PIN, .quadwp_io_num = -1,
        .quadhd_io_num = -1, .max_transfer_sz = 65536,
    };
    const spi_device_interface_config_t device = {
        .clock_speed_hz = 20000000, .mode = 0,
        .spics_io_num = -1, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &panel_spi));
    gpio_config_t pins = {
        .pin_bit_mask = (1ULL << EPD_CS_PIN) | (1ULL << EPD_DC_PIN) |
                        (1ULL << EPD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pins));
    pins.pin_bit_mask = 1ULL << EPD_BUSY_PIN;
    pins.mode = GPIO_MODE_INPUT;
    ESP_ERROR_CHECK(gpio_config(&pins));
    gpio_set_level(EPD_CS_PIN, 0); /* Dedicated bus: keep CS asserted. */
    gpio_set_level(EPD_DC_PIN, 0);
    gpio_set_level(EPD_RST_PIN, 1);
    return ESP_OK;
}

/* Bulk SPI replaces upstream byte writes and per-row delays. */
static void transfer(int data, const uint8_t *bytes, size_t size)
{
    gpio_set_level(EPD_DC_PIN, data);
    while (size) {
        size_t count = size > 4096 ? 4096 : size;
        spi_transaction_t transaction = {.length = count * 8, .tx_buffer = bytes};
        ESP_ERROR_CHECK(spi_device_polling_transmit(panel_spi, &transaction));
        bytes += count;
        size -= count;
    }
}

static void command(uint8_t value)
{
    transfer(0, &value, 1);
}

static void write_register(uint8_t address, const uint8_t *values, size_t size)
{
    command(address);
    transfer(1, values, size);
}

static void reset(void)
{
    gpio_set_level(EPD_RST_PIN, 1);
    DELAY_MS(50);
    gpio_set_level(EPD_RST_PIN, 0);
    DELAY_MS(2);
    gpio_set_level(EPD_RST_PIN, 1);
    DELAY_MS(50);
}

static esp_err_t wait_idle(void)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(20000);
    DELAY_MS(100);
    while (gpio_get_level(EPD_BUSY_PIN)) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) {
            return ESP_ERR_TIMEOUT;
        }
        DELAY_MS(20);
    }
    return ESP_OK;
}

static esp_err_t refresh(uint8_t mode)
{
    REG(0x22, mode);
    command(0x20);
    return wait_idle();
}

/* Keep encoding workspace small; reuse the same bulk SPI transport. */
static void transfer_plane(const uint8_t *packed, epd_plane_t plane)
{
    uint8_t chunk[256];
    for (size_t offset = 0; offset < EPD_SIZE_MONO;) {
        size_t count = EPD_SIZE_MONO - offset;
        if (count > sizeof(chunk)) {
            count = sizeof(chunk);
        }
        for (size_t i = 0; i < count; ++i) {
            size_t input = 2 * (offset + i);
            chunk[i] = epd_encode_plane_byte(packed[input], packed[input + 1], plane);
        }
        transfer(1, chunk, count);
        offset += count;
    }
}

esp_err_t EPD_Init(void)
{
    enapwrstate(ALDO3);
    DELAY_MS(10);
    reset();
    esp_err_t err = wait_idle();
    if (err != ESP_OK) return err;
    command(0x12);
    err = wait_idle();
    if (err != ESP_OK) return err;
    REG(0x18, 0x80);
    REG(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80);
    REG(0x01, LE16(EPD_HEIGHT - 1), 0x02);
    REG(0x3C, 0x01);
    REG(0x11, 0x01);
    REG(0x44, LE16(0), LE16(EPD_WIDTH - 1));
    REG(0x45, LE16(EPD_HEIGHT - 1), LE16(0));
    REG(0x4E, LE16(0));
    REG(0x4F, LE16(0));
    return wait_idle();
}

/* Manufacturer Init4GRAY sequence: controller-resident waveform, no LUT upload. */
esp_err_t EPD_Init_4GRAY(void)
{
    enapwrstate(ALDO3);
    DELAY_MS(10);
    reset();
    esp_err_t err = wait_idle();
    if (err != ESP_OK) return err;
    command(0x12);
    err = wait_idle();
    if (err != ESP_OK) return err;
    REG(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80);
    REG(0x01, LE16(EPD_HEIGHT - 1), 0x02);
    REG(0x11, 0x01);
    REG(0x44, LE16(0), LE16(EPD_WIDTH - 1));
    REG(0x45, LE16(EPD_HEIGHT - 1), LE16(0));
    REG(0x4E, LE16(0));
    REG(0x4F, LE16(0));
    err = wait_idle();
    if (err != ESP_OK) return err;
    REG(0x3C, 0x01);
    REG(0x18, 0x80);
    REG(0x1A, 0x5A);
    return ESP_OK;
}

esp_err_t EPD_Display_Base(const UBYTE *Image)
{
    if (!Image) return ESP_ERR_INVALID_ARG;
    command(0x24);
    transfer_plane(Image, EPD_PLANE_MONO);
    command(0x26);
    transfer_plane(Image, EPD_PLANE_MONO);
    return refresh(0xF7);
}

esp_err_t EPD_Display_4Gray(const UBYTE *image2bpp)
{
    if (!image2bpp) return ESP_ERR_INVALID_ARG;
    command(0x24);
    transfer_plane(image2bpp, EPD_PLANE_GRAY_LOW);
    DELAY_MS(1); /* Manufacturer inter-plane delay. */
    command(0x26);
    transfer_plane(image2bpp, EPD_PLANE_GRAY_HIGH);
    return refresh(0xD7);
}

esp_err_t EPD_Display_Partial(const UBYTE *Image, UWORD Xstart, UWORD Ystart,
                            UWORD Xend, UWORD Yend)
{
    /* Legacy signature only; subwindows are outside this port's contract. */
    if (!Image || Xstart != 0 || Ystart != 0 || Xend != EPD_WIDTH ||
        Yend != EPD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }
    reset();
    REG(0x18, 0x80);
    REG(0x3C, 0x80);
    /* Upstream encodes the last byte's first pixel: 792, not 799. */
    REG(0x44, LE16(0), LE16(EPD_WIDTH - 8));
    /* Board scan direction: reverse upstream partial Y endpoints. */
    REG(0x45, LE16(EPD_HEIGHT - 1), LE16(0));
    REG(0x4E, LE16(0));
    REG(0x4F, LE16(0));
    command(0x24);
    transfer_plane(Image, EPD_PLANE_MONO);
    return refresh(0xFF);
}

void EPD_Sleep(void)
{
    REG(0x10, 0x01);
    DELAY_MS(10);
    gpio_set_level(EPD_RST_PIN, 0);
    gpio_set_level(EPD_CS_PIN, 0);
    gpio_set_level(EPD_DC_PIN, 0);
    disapwrstate(ALDO3);
    DELAY_MS(10);
}
