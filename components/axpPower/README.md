# AXP2101 board wrapper

来源：Waveshare 官方 `ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/axpPower`。
`src/` 中 XPowersLib 原样保留（含上游版权和 MIT 许可）；仅直接使用 AXP2101，
不引入其他芯片的聚合头，库头文件不再作为组件公共 API 导出。

- 公共接口仅保留 `axp_init/get_battery_power/enapwrstate/disapwrstate/get_usb_connected/axp_pwr_off`。
  屏幕所用的 `ALDO3` 枚举及开关不变；仍禁止关闭 DC1。
- 删除音频依赖、关机音频清理（累计 600 ms 等待）、电源调试任务、状态/电压读写调试 API。
- 所有有效初始化配置调用及顺序保持官方值：VBUS 4.36 V / 1500 mA，系统关机 2.6 V，
  DC1/ALDO1/2/3 3.3 V；按键关机 4 s、开机 1 s；预充 50 mA、恒流 200 mA、
  截止 25 mA / 4.2 V；RTC 充电 3.0 V；低电警告 10%、关机 5%；测量、LED、
  电量学习配置不变。未增加任何电源轨关闭操作。
- 初始化探测失败立即返回，不再继续写配置；删除仅用于注释日志的读回操作。

集成前先初始化共享 I2C 总线。`i2c_bsp` 须提供官方签名的
`i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)` 和
`i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t len)`，返回 `esp_err_t`。
组件依赖仅 `i2c_bsp`、`driver`（ESP-IDF 5.x 新 I2C 驱动）。

关机前由调用者完成屏幕刷新及其他外设收尾。未核对原理图前，不应因删除音频代码而关闭
ALDO1/2 等电源轨。上游 XPowersLib 初始化还会禁用 TS 引脚测量，此行为未修改；
充电安全、轨道归属及真实关机效果仍需硬件验证。
