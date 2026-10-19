#ifndef SHTC3_BSP_H
#define SHTC3_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize after the shared I2C bus is ready; leave the sensor asleep. */
void i2c_shtc3_init(void);

/**
 * Wake, measure and sleep. Call from task context, not concurrently.
 * Outputs: uncalibrated Celsius and relative humidity (%).
 * Returns ESP_OK or an ESP-IDF error code; outputs remain unchanged on failure.
 */
int SHTC3_GetEnvTemperatureHumidity(float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif

#endif
