#ifndef SHTC3_BSP_H
#define SHTC3_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize after the shared I2C bus is ready; leave the sensor asleep. */
void i2c_shtc3_init(void);

/**
 * Wake, measure and sleep. Call from task context, not concurrently.
 * Outputs: Celsius (upstream -1.5 C board correction), relative humidity (%).
 * Returns 0 on success, -1 for null arguments, 1 for I2C failure,
 * or 2 for CRC mismatch. Outputs remain unchanged on failure.
 */
int SHTC3_GetEnvTemperatureHumidity(float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif

#endif
