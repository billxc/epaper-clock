# SSD1677 旧帧 RAM 恢复与单次局刷调研

## 判定

**控制器/驱动机制有明确支持依据：可以把旧帧、新帧分别写入 RAM，最后只触发一次差分刷新。当前两次局刷不是已经证实不可减少的硬件要求。**

但这不等于已验证“本板每分钟 PMU 真断电后恢复旧帧，仅一次局刷仍可靠”。找到的成熟驱动默认要求首次全刷，公开资料没有给出该板这条完整恢复路径的可靠性实测。因此当前结论是：**值得优先做单刷候选验证，不应未经验证直接删除双刷回退。**

本次只研究，没有修改驱动、烧录或测量功耗。

## 1. 屏幕控制器已从手册确认

Waveshare《3.97inch e-Paper User Manual》V1.0，2025-04-24，印刷第 4 页：

> Driver IC: SSD1677

同一手册写有 3.97 英寸、800×480、片内显示 RAM 等信息。这比仅用分辨率猜控制器更可靠；但没有据此认定随板玻璃就是 Good Display 的 GDEM0397T81。

原文入口来自[官方资料页](https://docs.waveshare.net/ESP32-S3-ePaper-3.97/Resources-And-Documents)，其原始 PDF 链接为 `https://www.waveshare.net/w/upload/f/fa/3.97inch_e-Paper.pdf`。直接下载遇到验证，因此本次读取了公开归档的同名厂商手册：

[手册归档，固定提交](https://github.com/developing-today/hardware-doc/blob/8f06024fee9a39545a9e1f47437d92904f4ef8d4/components/waveshare/3.97inch-e-paper/artifacts/3.97inch-e-paper-manual-v1.0-2025-04-24.pdf)。未绕过验证码或 TLS 校验。

## 2. SSD1677 手册：写 RAM 和驱动屏幕是分开的

读取的原始手册为 SSD1677 Rev 1.0，Nov 2018。按文档印刷页码：

| 命令 | 含义 | 位置 |
| --- | --- | --- |
| `0x24` | 写 BW RAM，数据写入本身不等于驱动屏幕 | 第 29 页 |
| `0x26` | 写另一平面，原文称 RED RAM；在相应黑白差分驱动中用作 previous | 第 29 页；previous 用法另见驱动证据 |
| `0x21` | 控制显示更新时 RAM 正常使用、旁路或反相 | 第 27 页 |
| `0x22` | 选择显示更新序列、Display Mode、时钟/模拟供电等 | 第 28 页 |
| `0x20` | Master Activation，执行所选更新序列 | 第 27 页 |

手册还列出 Display Mode 2 的 RAM ping-pong 选项。**不能仅因存在两块 RAM，就自行认定任意 OTP/波形下 0x26 都具有相同的旧帧语义**；需要结合面板对应驱动和更新设置。

手册操作流程也分开列出“写图到 RAM”和“驱动显示”。所以无需通过一次可见刷新才能完成每一笔 RAM 写入。

[SSD1677 手册归档，固定提交](https://github.com/developing-today/hardware-doc/blob/8f06024fee9a39545a9e1f47437d92904f4ef8d4/components/solomon-systech/ssd1677/artifacts/ssd1677-rev1.0-nov2018-m5stack-mirror.pdf)。这是厂商文档的公开镜像，不是本板实测报告。

## 3. GxEPD2：3.97 英寸专用驱动的明确实现

固定提交：`de82887e77a78528ea386e68bba2cb0291d2c319`，GxEPD2 1.6.9。

[头文件](https://github.com/ZinggJM/GxEPD2/blob/de82887e77a78528ea386e68bba2cb0291d2c319/src/gdem/GxEPD2_397_GDEM0397T81.h)明确标注：GDEM0397T81、SSD1677、800×480、单色、支持快速局刷。

[实现](https://github.com/ZinggJM/GxEPD2/blob/de82887e77a78528ea386e68bba2cb0291d2c319/src/gdem/GxEPD2_397_GDEM0397T81.cpp)：

- `writeImage()`：写 `0x24`，注释为 current。
- `writeImageToPrevious()`：写 `0x26`，注释为 previous。
- `writeImageAgain()`：把同一图写入两块 RAM，同步基线；它不是“再次触发显示刷新”。
- `drawImage()`：写新图 → `refresh()` → `writeImageAgain()`。最后一步是 RAM 同步，不是第二次可见刷新。
- `_Update_Part()`：设置 `0x21 = 00,00`、`0x22 = FC`，然后 `0x20` 激活。

注意：当前 Waveshare 路径使用 `0x22 = FF`，不能在未核对时把整个 GxEPD2 初始化序列或 `FC` 常量直接搬过来。应保留本板的电源、复位、扫描方向、窗口、位极性、温度/OTP 设置，并确认更新结束时的供电状态。

### 冷启动限制不能略过

GxEPD2 有初始全刷保护：

- 首次写图可能通过 `clearScreen()` 隐式全刷。
- 首次局刷请求可能被升级成全刷。
- `initial=false` 可以绕过部分保护，但[公开说明](https://github.com/ZinggJM/GxEPD2/blob/de82887e77a78528ea386e68bba2cb0291d2c319/src/GxEPD2_BW.h#L317-L325)将其用于主控深睡后、**屏幕电源仍保留**的场景，并警告掉电后省略初刷会出现 garbage。

这条警告没有专门验证“外部保存了完全准确的旧帧，再手动恢复两块 RAM”这一实验，因此不能据此证明本方案绝对不行；但它明确阻止我们把绕过首次全刷当作已经可靠的断电恢复方案。

## 4. 同板独立实现的交叉证据

[Free-Ink SDK 本板支持说明](https://github.com/Free-Ink/freeink-sdk/blob/3fe3936d62e65274453b96589bcdac57105e8dd0/docs/waveshare-epaper-397-support.md)报告了本板移植及实机使用。

[SSD1677 驱动](https://github.com/Free-Ink/freeink-sdk/blob/3fe3936d62e65274453b96589bcdac57105e8dd0/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp)的正常差分路径分别写 `0x24=new`、`0x26=previous`，`seedPreviousFrame()` 仅写旧 RAM、不刷新。本板配置也使用 F7/FF。

但该实现仍将启动后的首次绘制升级成全刷。它是“本板正常差分可以单次激活”的额外证据，**不是 PMU 断电后恢复旧帧的成功证明**。

## 5. 当前代码为什么实际刷了两次

[本项目 epaper_port.c](../components/epaper_port/epaper_port.c) 的 `EPD_Display_Partial()` 同时包含：

```text
复位 / 窗口设置 → 0x24 写图 → 0x22=FF → 0x20 → 等 BUSY
```

调用方先用旧图调用一次，再用新图调用一次，因此确实进行了两次显示更新。第一步不只是“写缓存”。

候选优化为：

```text
上电、完成本板初始化和窗口配置
→ 旧图写入 previous RAM（不激活显示）
→ 新图写入 current RAM（不激活显示）
→ 配置正确差分更新模式
→ 仅一次显示更新激活，等待完成
→ 屏幕休眠、关闭 ALDO3，随后按原流程 PMU 关机
```

写每个平面前都要设置正确的 RAM 地址指针；不能依赖写完第一平面后地址恰好仍在起点。也不能在两次写 RAM 之间切断屏幕电源。

### 能省什么、不能省什么

- 目标是省掉一次驱动波形及相应等待，可能同时减少重复复位开销。
- 旧图仍需与实际玻璃上的画面完全一致；TF 读写仍在。
- 新旧两帧各 48000 字节，候选方案仍传两帧到控制器，**不是 SPI 数据量减半**。
- 因此即使成功，也不能承诺整机耗电减半或续航翻倍。

## 6. 最小实机验证边界

先只验证整屏范围的黑白差分，不同时改窗口、灰度、TF 或 PMU 策略：

1. 先形成已知画面并保存准确旧帧，然后真正切断 ALDO3 / 经 PMU 冷启动。
2. 重新初始化、恢复旧/新 RAM，仅一次显示更新；检查黑→白、白→黑、相同像素保持。
3. 连续几轮分钟唤醒看残影；旧缓存失效或刚显示过管理画面时仍回退全刷。
4. 确认没有初始清屏、隐式全刷；对比 BUSY 时长和完整周期电量。

本次没有执行这些实机步骤，也没有修改当前可靠性边界。
