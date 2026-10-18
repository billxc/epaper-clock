#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define AXP2101Addr 0x34
#define SHTC3Addr 0x70
#define PCF85063Addr 0x51
void i2c_master_init(void);
esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);
esp_err_t i2c_write_bytes(uint8_t addr, uint8_t *data, size_t len);
esp_err_t i2c_read_bytes(uint8_t addr, uint8_t *data, size_t len);
esp_err_t i2c_master_write_read_device_compat(uint8_t addr, const uint8_t *wbuf,
                                           size_t wlen, uint8_t *rbuf, size_t rlen);
#ifdef __cplusplus
}
#endif
