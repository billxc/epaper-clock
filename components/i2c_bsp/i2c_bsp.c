// Pin assignments and I2C addresses from the Waveshare board example.
#include "i2c_bsp.h"
#include "driver/i2c_master.h"
#include <string.h>

static i2c_master_dev_handle_t rtc, sensor, pmu;

static i2c_master_dev_handle_t device(uint8_t addr)
{
    switch (addr) {
    case PCF85063Addr: return rtc;
    case SHTC3Addr: return sensor;
    case AXP2101Addr: return pmu;
    default: return NULL;
    }
}

void i2c_master_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t config = {
        .i2c_port = 0, .sda_io_num = 41, .scl_io_num = 42,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
    const uint8_t addresses[] = {PCF85063Addr, SHTC3Addr, AXP2101Addr};
    i2c_master_dev_handle_t *handles[] = {&rtc, &sensor, &pmu};
    for (int i = 0; i < 3; ++i) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[i], .scl_speed_hz = 400000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev, handles[i]));
    }
}

esp_err_t i2c_write_bytes(uint8_t addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit(device(addr), data, len, 1000);
}

esp_err_t i2c_read_bytes(uint8_t addr, uint8_t *data, size_t len)
{
    return i2c_master_receive(device(addr), data, len, 1000);
}

esp_err_t i2c_master_write_read_device_compat(uint8_t addr, const uint8_t *wbuf,
                                           size_t wlen, uint8_t *rbuf, size_t rlen)
{
    return i2c_master_transmit_receive(device(addr), wbuf, wlen, rbuf, rlen, 1000);
}

esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    uint8_t buffer[len + 1];
    buffer[0] = reg;
    memcpy(buffer + 1, data, len);
    return i2c_write_bytes(addr, buffer, len + 1);
}

esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device_compat(addr, &reg, 1, data, len);
}
