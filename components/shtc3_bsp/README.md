# SHTC3 board wrapper

来源：Waveshare 官方 `ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/shtc3_bsp`。
仅公开 `i2c_shtc3_init` 和 `SHTC3_GetEnvTemperatureHumidity`；删除调试任务、
未实现的声明及无关的 ID 查询 API，底层命令/CRC/测量辅助函数改为私有。

时序依据 SHTC3 数据手册的 Wakeup / Soft Reset 和 normal-mode measurement：

- 唤醒、复位后分别等待 240 us（官方 100 us 不足以覆盖最大值）。
- `0x7866` 测量最大 12.1 ms，等待 13 ms，按 RTOS tick 向上取整并额外补一个 tick，
  防止调用处于 tick 末尾导致提前读取；实际延迟受 `configTICK_RATE_HZ` 影响。
- 删除每次测量前的额外 100 ms，保留最多四次尝试及两组 CRC 校验。
- 初始化结束、每次读取结束均尝试 Sleep；通信失败时无法保证已真正休眠。
- 返回传感器原始换算温度，不沿用官方固定 -1.5 °C 偏移；外壳和主控自热补偿需实测校准。
  仅在完全成功后更新输出；错误不修改调用者数据。

**I2C 集成约定**：AXP 的 `i2c_read_reg/i2c_write_reg` 使用 8 位寄存器地址，
不能直接替代 SHTC3 的 16 位命令和无地址读取。除这两个寄存器接口外，`i2c_bsp`
仍需提供官方已有的两个原始传输接口，返回 `esp_err_t`：

```c
esp_err_t i2c_write_bytes(uint8_t addr, uint8_t *data, size_t len);
esp_err_t i2c_read_bytes(uint8_t addr, uint8_t *data, size_t len);
```

写入两个命令字节后 STOP；等待测量完成，再单独读六字节（不可额外发送寄存器字节）。
组件依赖仅 `i2c_bsp`、`driver`，不依赖 GUI、音频、`esp_timer`。
共享总线需先初始化；在任务上下文中调用，同一传感器的初始化/读取必须由调用者串行化，
底层 I2C 单笔事务互斥不能代替整个 wake/measure/sleep 流程的串行化。
