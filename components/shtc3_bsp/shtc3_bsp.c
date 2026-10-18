// Derived from Waveshare's ESP32-S3 e-Paper 3.97 SHTC3 BSP.
#include "shtc3_bsp.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "SHTC3";

#define SHTC3_ADDRESS 0x70
#define CRC_POLYNOMIAL 0x31
#define SHTC3_WAKE_RESET_US 240
#define SHTC3_MEASUREMENT_MS 13
#define SHTC3_MAX_ATTEMPTS 4

typedef enum {
    NO_ERROR = 0,
    ACK_ERROR = 1,
    CHECKSUM_ERROR = 2,
} etError;

typedef enum {
    SOFT_RESET = 0x805D,
    SLEEP = 0xB098,
    WAKEUP = 0x3517,
    MEAS_T_RH_POLLING = 0x7866, // Normal mode, T first, no clock stretching.
} etCommands;

static etError SHTC3_CheckCrc(const uint8_t *data, uint8_t checksum)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (crc << 1) ^ CRC_POLYNOMIAL : crc << 1;
        }
    }
    return crc == checksum ? NO_ERROR : CHECKSUM_ERROR;
}

static etError SHTC3_SendCommand(etCommands command)
{
    // SHTC3 uses a 16-bit command, NOT an 8-bit register address.
    uint8_t bytes[2] = {command >> 8, command & 0xFF};
    esp_err_t err = i2c_write_bytes(SHTC3_ADDRESS, bytes, sizeof(bytes));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Command 0x%04X failed: %d", (unsigned)command, err);
        return ACK_ERROR;
    }
    return NO_ERROR;
}

static etError SHTC3_Wakeup(void)
{
    etError error = SHTC3_SendCommand(WAKEUP);
    // Datasheet: allow up to 240 us before sending another command.
    if (error == NO_ERROR) {
        esp_rom_delay_us(SHTC3_WAKE_RESET_US);
    }
    return error;
}

static etError SHTC3_GetTempAndHumiPolling(float *temperature, float *humidity)
{
    etError error = SHTC3_SendCommand(MEAS_T_RH_POLLING);
    if (error != NO_ERROR) {
        return error;
    }

    // Datasheet normal-mode measurement maximum: 12.1 ms. Round 13 ms up
    // to ticks, plus one tick for the unknown phase of the current tick.
    const TickType_t ticks =
        (SHTC3_MEASUREMENT_MS * configTICK_RATE_HZ + 999) / 1000;
    vTaskDelay(ticks + 1);

    uint8_t bytes[6];
    // Read only: prepending a register/command here would corrupt the protocol.
    if (i2c_read_bytes(SHTC3_ADDRESS, bytes, sizeof(bytes)) != ESP_OK) {
        return ACK_ERROR;
    }
    if (SHTC3_CheckCrc(bytes, bytes[2]) != NO_ERROR ||
        SHTC3_CheckCrc(bytes + 3, bytes[5]) != NO_ERROR) {
        return CHECKSUM_ERROR;
    }

    uint16_t raw_temperature = (bytes[0] << 8) | bytes[1];
    uint16_t raw_humidity = (bytes[3] << 8) | bytes[4];
    *temperature = 175.0f * raw_temperature / 65536.0f - 45.0f;
    *humidity = 100.0f * raw_humidity / 65536.0f;
    return NO_ERROR;
}

int SHTC3_GetEnvTemperatureHumidity(float *temperature, float *humidity)
{
    if (!temperature || !humidity) {
        return -1;
    }

    float measured_temperature = 0;
    float measured_humidity = 0;
    etError error = SHTC3_Wakeup();
    if (error == NO_ERROR) {
        // Preserve the upstream maximum of four attempts, without the
        // unrelated 100 ms delay before every measurement.
        for (int attempt = 0; attempt < SHTC3_MAX_ATTEMPTS; ++attempt) {
            error = SHTC3_GetTempAndHumiPolling(&measured_temperature, &measured_humidity);
            if (error == NO_ERROR) {
                break;
            }
        }
    }

    // Also attempt sleep on wakeup/measurement failure; preserve the first error.
    etError sleep_error = SHTC3_SendCommand(SLEEP);
    if (error != NO_ERROR) {
        return error;
    }
    if (sleep_error != NO_ERROR) {
        return sleep_error;
    }

    // Report the sensor result; any enclosure-specific offset needs calibration.
    *temperature = measured_temperature;
    *humidity = measured_humidity;
    return NO_ERROR;
}

void i2c_shtc3_init(void)
{
    etError error = SHTC3_Wakeup();
    if (error == NO_ERROR) {
        error = SHTC3_SendCommand(SOFT_RESET);
        if (error == NO_ERROR) {
            esp_rom_delay_us(SHTC3_WAKE_RESET_US);
        }
    }
    etError sleep_error = SHTC3_SendCommand(SLEEP);
    if (error != NO_ERROR || sleep_error != NO_ERROR) {
        ESP_LOGE(TAG, "Initialization failed: reset=%d, sleep=%d", error, sleep_error);
    }
}
