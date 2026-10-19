// SPDX-License-Identifier: MIT
// SHTC3 commands, timing and conversion formulas follow Sensirion's datasheet.
#include <stdbool.h>
#include "shtc3_bsp.h"
#include "i2c_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static esp_err_t command(uint16_t value)
{
    uint8_t bytes[] = {value >> 8, value & 0xff};
    return i2c_write_bytes(SHTC3Addr, bytes, sizeof(bytes));
}

static esp_err_t wake(void)
{
    esp_err_t result = command(0x3517);
    esp_rom_delay_us(240);
    return result;
}

static bool crc_valid(const uint8_t *word)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; ++i) {
        crc ^= word[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
    }
    return crc == word[2];
}

void i2c_shtc3_init(void)
{
    esp_err_t result = wake();
    if (result == ESP_OK) result = command(0x805d);
    esp_rom_delay_us(240);
    esp_err_t sleeping = command(0xb098);
    if (result != ESP_OK || sleeping != ESP_OK)
        ESP_LOGW("SHTC3", "Sensor initialization failed");
}

int SHTC3_GetEnvTemperatureHumidity(float *temperature, float *humidity)
{
    esp_err_t result = wake();
    if (result == ESP_OK) result = command(0x7866); // Normal mode, no clock stretching.
    uint8_t data[6];
    if (result == ESP_OK) {
        // Normal measurement max 12.1 ms; round up and allow for tick phase.
        vTaskDelay((13 * configTICK_RATE_HZ + 999) / 1000 + 1);
        result = i2c_read_bytes(SHTC3Addr, data, sizeof(data));
    }
    esp_err_t sleeping = command(0xb098);
    if (result != ESP_OK) return result;
    if (sleeping != ESP_OK) return sleeping;
    if (!crc_valid(data) || !crc_valid(data + 3)) return ESP_ERR_INVALID_CRC;
    *temperature = -45.0f + 175.0f * ((data[0] << 8) | data[1]) / 65536.0f;
    *humidity = 100.0f * ((data[3] << 8) | data[4]) / 65536.0f;
    return ESP_OK;
}
