# SHTC3

MIT-licensed I²C driver implementing the public Sensirion SHTC3 datasheet:
https://sensirion.com/products/catalog/SHTC3

- Wake/reset: wait 240 μs.
- Normal-mode measurement `0x7866`: maximum 12.1 ms; wait at least 13 ms,
  rounding to FreeRTOS ticks with a phase allowance.
- Check both CRC-8 words, polynomial `0x31`, initial value `0xff`.
- Send sleep `0xb098` after initialization and every measurement attempt.
- Report datasheet temperature/humidity conversions without an arbitrary offset.
- One attempt per sampling cycle; failures leave outputs unchanged.

The caller supplies valid output pointers and serializes access to this sensor.
Use `i2c_write_bytes`/`i2c_read_bytes`: commands are 16-bit, not register addresses.
