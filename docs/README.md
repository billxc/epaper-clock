# HTTPS 蓝牙设置页面

静态 Pages 客户端：**仅 Wi-Fi 设置和手动时间**，初始化与蓝牙管理共用。
本版新增 `clock-control` 保存/设时回执，旧固件不支持。配套固件已通过完整构建和软件测试，尚未实机联调；最新下载见项目 README。

公开页面地址 https://billxc.github.io/epaper-clock/ 。是否已经部署本版，以项目根 README / 当前发布状态为准。
GitHub Pages 使用 `main` 分支 `/docs`，无需后端、npm安装、CDN、Service Worker。

## 使用

设备屏幕显示 `PROV_EPAPER_…` 和六位数字码。新版 Android/桌面 Chrome、Edge 打开HTTPS页面，
输入码后选择设备；不需要填写服务UUID，也不会自动读取手机Wi-Fi密码。

- Wi-Fi：可扫描或手填SSID/密码；设备先验证网络，应用配置保存ACK才代表成功。
- 时间：输入日期时间和明确UTC偏移，或填入手机当前时间；加密发送UTC epoch，设备按自己的时区写RTC。
- 初始模式失败仍留初始，窗口暂停可按中键重开；蓝牙管理可重复设置，约五分钟闲置后退时钟。
- 页面断开不等于取消，也不是保存成功证明。已收到的ACK有效，没有ACK时先查看状态，不自动重发。

普通 iPhone/iPad 浏览器通常没有Web Bluetooth。官方App可使用标准Wi-Fi配网，但不包含本项目手动时间界面。

## 文件

| 文件 | 职责 |
| --- | --- |
| `index.html` / `style.css` / `app.mjs` | 连接、Wi-Fi和时间面板、状态与清空 |
| `ble.mjs` | GATT发现、0x2901端点映射、串行写入后读取 |
| `protocol.mjs` | ESP-IDF Wi-Fi/扫描/Security1 protobuf wire codec |
| `security1.mjs` | WebCrypto X25519、PoP、AES-CTR连续流 |
| `clock-control.mjs` | JSON设置协议、时区/epoch转换、应用保存ACK |
| `*.test.mjs` | 无npm依赖的协议及传输模拟测试 |

新协议和固件API：[clock-control.md](clock-control.md)。模式与按钮：[interaction.md](interaction.md)。
LAN网页与OTA不属于此页，不从HTTPS页面跨域调用设备HTTP。

配网、供电、备用电池、TF、局刷及续航的讨论结论汇总见 [本地对话疑问记录](questions.md)。

## Security1 与 BLE 兼容性

使用 ESP-IDF v5.5.2 的标准 `wifi_provisioning` BLE方案和默认服务UUID：
`1775244d-6b43-439b-877c-060f2d9bed07`。
只通过0x2901 User Description匹配 `prov-session`、`prov-config`、`clock-control`，不猜测特征UUID。
缺少自定义端点时明确提示需要新版固件，不静默只做旧版配网。

- X25519共享秘密 XOR SHA256(PoP)得到AES密钥；验证设备握手证明。
- 握手和后续两方向数据共用同一个AES-CTR流；必须保留非16字节包边界的offset。
- 失败不能盲目重发密文，否则双方计数不同步；传输失败关闭会话，业务验证失败则允许重填。
- 只支持Security1，不降级Security0。六位短码方便人工输入，不宣称与长随机码相同抗猜测强度。
- Security1不是AEAD/TLS；用于物理触发、限时、可信近距离配置。

每次请求write-with-response后read，不把单条protobuf拆成多次独立写入。
单次请求保守上限256字节，BLE操作等待20秒；实际OS/GATT必须支持相应MTU或可靠长写/长读。
此限制必须上实机测试，Node mock不能证明硬件兼容。

## 固件配合

SDK回调只复制候选/置标记；应用主循环保存配置，并由加密`clock-control`返回本次attempt的保存ACK。
保存为单个版本化NVS记录，不将SDK Connected当成保存成功。重配由prepare_wifi申请、主循环执行SDK复位，
避免在BLE回调里执行NVS、屏幕或manager操作。

manager禁用原有成功即自动停止，以便管理模式继续手动设时。初始模式完成后留ACK读取窗口再结束BLE；
管理模式按用户设置操作/按键续期，status轮询不续期。退出deinit，不永久释放BT内存，以支持再次进入。
SDK会临时用自己的Wi-Fi FLASH存储，退出时清理SDK层并保留应用配置。掉电中的残留仍需实机验收。

标准GetStatus没有应用保存ACK；本版不再依靠它判成功。客户端也不再宣称拿到LAN IP就等于保存完成。
字段、错误、RTC时间和复用语义以 [clock-control.md](clock-control.md) 为准。

## 隐私

无fetch/XHR/WebSocket上传、统计脚本或凭据持久化；CSP connect-src none。
GitHub仍收到正常静态文件访问请求，但不会收到输入的SSID、密码、时间或验证码。
密码提交后清空，退出清空输入、销毁会话；JS字符串/WebCrypto内存不能保证安全擦除。
NVS未加密，不防物理读取。不要把PoP放进URL。

手动日期按明确UTC偏移转换，不偷偷使用浏览器本地时区；设备再次检查本地年份2000–2099。
允许1–32 UTF-8字节SSID、8–63可打印ASCII密码；不支持WEP/企业EAP。开放网络需显式勾选。
SDK handler不能安全接受64字符原始PSK，页面拒绝而不是截断。

## 测试与开发

```sh
node --test docs/*.test.mjs
python3 -m http.server 8000 --directory docs
```

localhost可开发；其他机器访问普通LAN HTTP不是Web Bluetooth安全上下文，部署应使用HTTPS。
本地21项Node测试已通过，另有浏览器mock覆盖配对、手动设时、保存ACK及断开清空。
ESP-IDF构建及原生C菜单/绘图测试也已通过CI。仍需真实BLE/GATT/MTU、路由器、重复设置、窗口结束及退出功耗验证。

协议实现为依据公开规范独立编写，未打包社区库。曾检查的社区候选有未实现Security1或缺测试的版本，
因此没有声称存在可直接信任的官方浏览器库。项目代码MIT，SDK作为独立Apache-2.0构建依赖。
官方依据（固定v5.5.2）：

- [sec1/session protos](https://github.com/espressif/esp-idf/tree/v5.5.2/components/protocomm/proto)
- [Wi-Fi/scan protos](https://github.com/espressif/esp-idf/tree/v5.5.2/components/wifi_provisioning/proto)
- [Security1 Python参考](https://github.com/espressif/esp-idf/blob/v5.5.2/tools/esp_prov/security/security1.py)
- [BLE scheme](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/src/scheme_ble.c)
- [Security1 C实现](https://github.com/espressif/esp-idf/blob/v5.5.2/components/protocomm/src/security/security1.c)
