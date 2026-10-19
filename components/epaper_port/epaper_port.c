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

void epaper_port_init(void)
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

static void wait_idle(void)
{
    DELAY_MS(100);
    while (gpio_get_level(EPD_BUSY_PIN)) {
        DELAY_MS(20);
    }
}

static void refresh(uint8_t mode)
{
    REG(0x22, mode);
    command(0x20);
    wait_idle();
}

void EPD_Init(void)
{
    enapwrstate(ALDO3);
    DELAY_MS(10);
    reset();
    wait_idle();
    command(0x12);
    wait_idle();
    REG(0x18, 0x80);
    REG(0x0C, 0xAE, 0xC7, 0xC3, 0xC0, 0x80);
    REG(0x01, LE16(EPD_HEIGHT - 1), 0x02);
    REG(0x3C, 0x01);
    REG(0x11, 0x01);
    REG(0x44, LE16(0), LE16(EPD_WIDTH - 1));
    REG(0x45, LE16(EPD_HEIGHT - 1), LE16(0));
    REG(0x4E, LE16(0));
    REG(0x4F, LE16(0));
    wait_idle();
}

void EPD_Display_Base(const UBYTE *Image)
{
    command(0x24);
    transfer(1, Image, EPD_SIZE_MONO);
    command(0x26);
    transfer(1, Image, EPD_SIZE_MONO);
    refresh(0xF7);
}

void EPD_Display_Partial(const UBYTE *Image, UWORD Xstart, UWORD Ystart,
                         UWORD Xend, UWORD Yend)
{
    /* Legacy signature only; subwindows are outside this port's contract. */
    ESP_ERROR_CHECK((Xstart == 0 && Ystart == 0 && Xend == EPD_WIDTH &&
                     Yend == EPD_HEIGHT) ? ESP_OK : ESP_ERR_INVALID_ARG);
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
    transfer(1, Image, EPD_SIZE_MONO);
    refresh(0xFF);
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
