# FreeInk 同款板算法参考

## 范围

按用户确定的方向：**只参考算法与硬件行为，不引入 FreeInk SDK、不迁移框架、不复制其驱动代码、LUT 或电压参数。**
现有原生 ESP-IDF、PMU 分钟关机、三个业务模式和通信功能保持不变。当前仅研究，尚未修改固件。

同板证据为已合并的 [FreeInk PR #79](https://github.com/Free-Ink/freeink-sdk/pull/79) 与 [板型实测记录](https://github.com/Free-Ink/freeink-sdk/blob/3fe3936d62e65274453b96589bcdac57105e8dd0/docs/waveshare-epaper-397-support.md)。
本次还核对了主线固定版本 [`f9cc1bed`](https://github.com/Free-Ink/freeink-sdk/tree/f9cc1beda45f4d2d3dade69fc444165727424f53)，没有把旧 PR 的行为当作当前实现。

## 有价值的算法参考

| 参考点 | 对我们的意义 |
| --- | --- |
| 2bpp 像素与双平面组织 | 将四档像素拆成两个位平面；位序、极性和控制器 RAM 含义必须与选用的厂家路径一致，不能把任意两张黑白图直接当四灰 |
| 绝对灰阶与补灰分开 | 完整四灰目标图与“黑白基底＋灰边补偿”是不同流程，不能混用数据编码和刷新顺序 |
| 前帧与控制器基准同步 | 灰阶后两个 RAM 不再自然等于黑白旧帧；灰→黑和断电恢复都需重建正确基准 |
| 电源稳定与刷新时序 | 其外部 LUT 路径会先稳定内部驱动电源，说明淡灰、杂点不一定是字体算法导致；我们的厂家路径是否需要同样处理应验证，不直接追加延时 |
| 刷新性能瓶颈 | 作者报告主要耗时在波形而非 SPI；保留本板 20MHz，不盲目提频 |

直接参考：[SSD1677 驱动](https://github.com/Free-Ink/freeink-sdk/blob/f9cc1beda45f4d2d3dade69fc444165727424f53/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp)。

## 字体算法不等于屏幕驱动

FreeInk 这部分代码的强项是灰阶输出和刷新状态，不提供能直接套给我们 24px 中文的最佳 Gamma 参数。
字体参考可另外查看 [CrossPoint 的 packed 1/2bpp 字模绘制](https://github.com/crosspoint-reader/crosspoint-reader/blob/19985520b8d020f811954975f316f4a9e5679de7/lib/GfxRenderer/GlyphBitmap.h) 与 [四灰抗锯齿字体示例](https://github.com/miklav/GxEPD2_4G_ext)。

后续仍在我们自己的字库生成与 C 绘图代码中完成：

- 从现有 Noto 字体覆盖率生成 2bpp 字模，不对黑白 PNG 做模糊。
- 校准小字的灰度量化曲线和饱满度，不盲用别屏的 Gamma 数字。
- 正确混合前景与背景，既支持黑字白底，也支持菜单反白。
- 研究共享字库输出黑白／四灰，控制 Flash 增量；全局模式覆盖全部电子纸文字。

## 不能从别人数据直接得出的结论

- 该板作者报告的 `BW wait≈407ms` 与 `gray_display≈226ms` 分属不同阶段，不能把 226ms 当作整轮四灰更新耗时。
- 同板四灰成功不等于所有温度、面板批次和我们每分钟 PMU 关机方式都已验证。
- 其当前驱动在冷启动首刷会提升刷新级别；我们不能照搬这一策略而无意变成每分钟全刷，也不能未经验证就删除首刷要求。
- 暂无可直接引用的同板灰度／黑白电池电量对照。实屏效果、响应、残影与耗电仍要 A/B 测量。

这里只记录参考结论；不引入新依赖，也不据此更改厂家波形或驱动电压。
