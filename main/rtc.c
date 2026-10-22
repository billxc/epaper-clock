// PCF85063 register map and board pins from Waveshare's example.
#include "rtc.h"
#include "i2c_bsp.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static StaticSemaphore_t rtc_lock_storage;
static SemaphoreHandle_t rtc_lock;

static uint8_t read_reg(uint8_t reg)
{
    uint8_t value;
    ESP_ERROR_CHECK(i2c_read_reg(PCF85063Addr, reg, &value, 1));
    return value;
}

static void write_reg(uint8_t reg, uint8_t value)
{
    ESP_ERROR_CHECK(i2c_write_reg(PCF85063Addr, reg, &value, 1));
}

static unsigned from_bcd(uint8_t value) { return (value >> 4) * 10 + (value & 15); }
static uint8_t to_bcd(unsigned value) { return ((value / 10) << 4) | (value % 10); }

bool rtc_alarm_pending(void)
{
    return (read_reg(0x01) & 0x40) != 0;
}

void clock_rtc_init(void)
{
    rtc_lock = xSemaphoreCreateMutexStatic(&rtc_lock_storage);
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << 45) | (1ULL << 1), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    // Release the previous alarm interrupt; do not clear oscillator-stop status.
    write_reg(0x01, read_reg(0x01) & ~(0x80 | 0x40));
}

bool rtc_read(struct tm *time)
{
    uint8_t data[7];
    xSemaphoreTake(rtc_lock, portMAX_DELAY);
    // One burst avoids mixing dates across a seconds rollover; serialize it
    // against the multi-register set operation from BLE/HTTP worker tasks.
    ESP_ERROR_CHECK(i2c_read_reg(PCF85063Addr, 0x04, data, sizeof(data)));
    xSemaphoreGive(rtc_lock);
    *time = (struct tm) {
        .tm_sec = from_bcd(data[0] & 0x7f), .tm_min = from_bcd(data[1] & 0x7f),
        .tm_hour = from_bcd(data[2] & 0x3f), .tm_mday = from_bcd(data[3] & 0x3f),
        .tm_wday = data[4] & 7, .tm_mon = from_bcd(data[5] & 0x1f) - 1,
        .tm_year = 100 + from_bcd(data[6]), .tm_isdst = -1,
    };
    return !(data[0] & 0x80) && time->tm_mon >= 0 && time->tm_mon < 12 &&
           time->tm_mday >= 1 && time->tm_mday <= 31;
}

void rtc_set(time_t epoch)
{
    struct tm time;
    localtime_r(&epoch, &time);
    xSemaphoreTake(rtc_lock, portMAX_DELAY);
    uint8_t control = read_reg(0x00);
    write_reg(0x00, control | 0x20); // STOP while setting the calendar.
    uint8_t data[] = {
        to_bcd(time.tm_sec), to_bcd(time.tm_min), to_bcd(time.tm_hour),
        to_bcd(time.tm_mday), time.tm_wday, to_bcd(time.tm_mon + 1),
        to_bcd(time.tm_year - 100),
    };
    ESP_ERROR_CHECK(i2c_write_reg(PCF85063Addr, 0x04, data, sizeof(data)));
    write_reg(0x00, control & ~0x20);
    xSemaphoreGive(rtc_lock);
}

bool rtc_cancel_alarm(void)
{
    uint8_t control = 0, check = 0;
    uint8_t disabled[] = {0x80, 0x80, 0x80, 0x80, 0x80};
    xSemaphoreTake(rtc_lock, portMAX_DELAY);
    esp_err_t err = i2c_read_reg(PCF85063Addr, 0x01, &control, 1);
    // Disable AIE and clear AF (write-zero-clear); preserve other control bits.
    control &= ~(0x80 | 0x40);
    if (err == ESP_OK) err = i2c_write_reg(PCF85063Addr, 0x01, &control, 1);
    if (err == ESP_OK) err = i2c_write_reg(PCF85063Addr, 0x0b, disabled, sizeof(disabled));
    if (err == ESP_OK) err = i2c_read_reg(PCF85063Addr, 0x01, &check, 1);
    xSemaphoreGive(rtc_lock);
    return err == ESP_OK && !(check & (0x80 | 0x40));
}

void rtc_arm_next_minute(void)
{
    xSemaphoreTake(rtc_lock, portMAX_DELAY);
    unsigned minute = from_bcd(read_reg(0x05) & 0x7f);
    // Match only minute and second. Date/hour rollover then needs no arithmetic.
    uint8_t alarm[] = {0x00, to_bcd((minute + 1) % 60), 0x80, 0x80, 0x80};
    uint8_t control = read_reg(0x01) & ~(0x80 | 0x40);
    write_reg(0x01, control);
    ESP_ERROR_CHECK(i2c_write_reg(PCF85063Addr, 0x0b, alarm, sizeof(alarm)));
    write_reg(0x01, control | 0x80);
    xSemaphoreGive(rtc_lock);
}
