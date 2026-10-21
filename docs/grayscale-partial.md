# 四灰局刷的证据与待验证边界

**当前结论：本板四灰整屏显示有厂家实现；“局部更新且窗口外灰阶保持”及四灰差分尚未证实，不接入正式分钟刷新策略。**这不是断言 SSD1677 硬件绝对不支持。

## 已核实

- 厂家同板四灰路径向 `0x24`、`0x26` 各发送 48,000 字节，再用 `0x22=0xD7`、`0x20` 激活。两 plane 共同编码目标灰阶，不是上一张与下一张灰图。
- 当前黑白 partial 路径使用 `0xFF`，以整屏地址范围分别激活旧、新两帧。它不等于四灰局刷，也不是矩形 ROI 更新。
- 手册的 `0x44/0x45` 是 RAM 写窗口。缩小写窗口不能证明波形也只作用于窗口；当前整帧传输器若不改跨度直接缩窗，还有地址回绕风险。
- FreeInk 同板灰阶成功的报告没有证明真正四灰局刷。固定版本的窗口接口在灰阶状态下转为整帧清理；分条写 RAM 也不等于分条激活。
- ESPHome 同控制器的另一块 Sticky 面板有维护者实测报告：局刷窗口外灰阶被迫变成单色。这是反例线索，不是本板硬件不支持的定论。
- 保存 96,000 字节旧四灰图本身可行，但不能自动补出灰阶差分波形。也不能仅凭 RAM 面数推断所有多阶段算法都不可能。

## 最小实机验证

先区分三组：黑白差分基线、厂家四灰整屏参考、得到有效命令／波形依据后的四灰窗口候选。使用固定四阶块和 16 种旧灰→新灰转换 patch，窗口外保留四阶色块、细线及文字；分别观察空更新 A→A 和 A↔B。

连续供电与断电必须分开：当前显示 API 每帧都会初始化、休眠并关闭 ALDO3，因此“USB 插着”并不是保持面板 warm。每分钟 PMU 关机后的控制器 RAM／内部状态恢复也需独立验证，不能用连续翻页成功替代。

记录窗口内灰阶、窗口外退化、残影／边缘污染、全屏闪动以及分阶段耗时。软件 BUSY 轮询与 SPI 字节量不是精确波形、功耗或视觉证据。任一异常即停止候选，以已知厂家整屏路径恢复；不能每次候选前偷偷全刷再宣称避免了全刷。

本轮仅核对资料和代码，没有实机测试。未复制外部 LUT、修改模拟电压、引入 FreeInk SDK 或迁移框架；不把编译、命令模拟或 PNG 视作实屏验收。

## 固定依据

- [Waveshare 同板官方驱动 9b12d407](https://github.com/waveshareteam/ESP32-S3-ePaper-3.97/blob/9b12d40731a80213b927ee8a421cae4082952819/Arduino/examples/02_E-Paper_Example/EPD_3in97.cpp)
- [SSD1677 Rev 1.0 手册固定归档](https://github.com/developing-today/hardware-doc/blob/8f06024fee9a39545a9e1f47437d92904f4ef8d4/components/solomon-systech/ssd1677/artifacts/ssd1677-rev1.0-nov2018-m5stack-mirror.pdf)：更新控制与 RAM 命令见印刷第 27–29 页，RAM 窗口见第 39 页。
- [FreeInk 固定版本 f9cc1bed](https://github.com/Free-Ink/freeink-sdk/tree/f9cc1beda45f4d2d3dade69fc444165727424f53)
- [ESPHome SSD1677 gray4 固定实现](https://github.com/esphome/esphome/blob/7784210ec4861a6d7d76f0be5b60fdd500efb680/esphome/components/epaper_spi/epaper_spi_ssd1677_gray4.h)
- [ESPHome 维护者窗口外变单色报告](https://github.com/esphome/esphome/pull/19213#issuecomment-5710373668)
