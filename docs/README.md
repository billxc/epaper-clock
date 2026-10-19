# HTTPS Web Bluetooth 配网客户端

**状态：协议实现及 Node 单元测试已完成，尚未通过 ESP32-S3 + 浏览器 BLE 实机联调。**
`index.html` 明示此边界，不应向用户宣传为已验收的整机功能。
此目录提供静态客户端；配套 BLE 固件见 `main/provision.c`。旧版热点固件不能使用本页面。

目标：用户在 HTTPS 网页明确选择附近设备、输入设备 PoP，再把自己局域网的 SSID/密码通过
ESP-IDF **v5.5.2** `wifi_provisioning` BLE Security1 发送给设备。**不需要设备开热点**。

## 文件及运行

- `index.html`、`style.css`：中文页面、能力检测结果、凭据输入和状态显示。
- `app.mjs`：交互、可选 Wi-Fi 扫描、配置/应用/轮询、断开及清空。
- `ble.mjs`：Web Bluetooth、0x2901 描述符发现、串行 write-with-response → read。
- `protocol.mjs`：最小 protobuf 编解码、Security1 / Wi-Fi config / scan 消息。
- `security1.mjs`：原生 WebCrypto X25519、PoP、连续 AES-256-CTR 流及设备证明校验。
- `protocol.test.mjs`、`ble.test.mjs`：无需 npm 包的 Node 测试；后者仅是 transport mock，不是实机测试。

从仓库根目录运行已有的 Node（开发验证使用 v24.21.0）：

```sh
node --test docs/*.test.mjs
```

本地开发可用已有 Python，不需安装工具：

```sh
python3 -m http.server 8000 --directory docs
```

在本机浏览器打开 `http://localhost:8000/`。其他电脑/手机访问普通 HTTP 局域网地址不是安全上下文，
应改用 HTTPS；不能直接双击 `file://` 页面。
公开仓库为 [`billxc/epaper-clock`](https://github.com/billxc/epaper-clock)，已发布页面：
`https://billxc.github.io/epaper-clock/`（强制 HTTPS）。
GitHub Pages 来源为 **`main` 分支的 `/docs`** 目录。所有资源使用相对路径，
适合项目站点子路径，不需要构建、后端、CDN、Service Worker 或远端运行依赖。
仓库公开历史与 Pages 发布状态以项目根目录 README 为准。

## 库调查与许可判断

通过公开 GitHub REST 和 npm Registry API 查询，并阅读仓库 README / package.json / LICENSE，
未假定存在官方成熟 JS 库：

| 候选 | 查证结果 | 决策 |
| --- | --- | --- |
| `espressif/esp-web-provisioning` | `GET https://api.github.com/repos/espressif/esp-web-provisioning` 返回 **404** | 不能假设存在或有可复用许可；404 仅代表该公开 URL 当前不可用 |
| [`esp-idf-provisioning-web`](https://github.com/ho-229/esp-idf-provisioning-web) / npm 0.0.5 | MIT；README 明确 **Security1 / Security2 not implemented**；依赖 protobufjs | 不满足 PoP + Security1，不采用 |
| [`esp-ble-prov`](https://github.com/nikas-belogolov/esp-ble-prov) / npm 2.0.1 | MIT；声明支持 Security0/1/2、扫描和 endpoint discovery；依赖 aes-js / protobufjs；README 明确 Testing（硬件与单元）尚未实现 | 是许可清楚的候选，不能据此称为成熟/已验证；本任务不安装、不引入其 bundle |
| React Native / Capacitor 的 ESP-IDF provisioning 包 | 依赖原生移动端桥接，不是普通 HTTPS 浏览器客户端 | 不适合此目标 |

npm 查询入口：`https://registry.npmjs.org/-/v1/search?text=esp%20provisioning&size=8`。
查询结论只覆盖本次检查的候选，不是断言全网没有其他库；包版本、维护状态和 README 可能变化。

本目录 JS 为依据公开协议独立编写，**没有复制、打包或 vendor 上述社区库代码**。
不因此引入其 MIT 分发代码。下面的 ESP-IDF 源码/客户端作为协议依据（上游源码标注 Apache-2.0）；
本目录未包含其源码副本。若后续改为复制实现/生成代码，应另行保留相应许可证、版权及 NOTICE。

## 官方依据（固定 ESP-IDF v5.5.2）

注意：有些讨论称 `security1.proto`，但本版本实际文件名是 **`sec1.proto`**。
字段不是从第三方示例猜测：

- [`sec1.proto`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/protocomm/proto/sec1.proto)
- [`session.proto`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/protocomm/proto/session.proto)
- [`constants.proto`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/protocomm/proto/constants.proto)
- [`wifi_config.proto`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/proto/wifi_config.proto)
- [`wifi_scan.proto`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/proto/wifi_scan.proto)
- [`wifi_constants.proto`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/proto/wifi_constants.proto)
- [`security/security1.py`](https://github.com/espressif/esp-idf/blob/v5.5.2/tools/esp_prov/security/security1.py)：算法、PoP、同一个 encryptor 的双向流状态。
- [`transport/ble_cli.py`](https://github.com/espressif/esp-idf/blob/v5.5.2/tools/esp_prov/transport/ble_cli.py)：读取 0x2901 endpoint 名称；写入后读取 characteristic。
- [`transport/transport_ble.py`](https://github.com/espressif/esp-idf/blob/v5.5.2/tools/esp_prov/transport/transport_ble.py)：优先发现名称映射。
- [`scheme_ble.c`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/src/scheme_ble.c)：默认服务 UUID、STA 模式。
- [`protocomm_ble.c`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/protocomm/src/transports/protocomm_ble.c)：描述符、请求处理、GATT 长读/prepare-write。
- [`security1.c`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/protocomm/src/security/security1.c)：两方向共用 AES context / nc_off / nonce counter。
- [`handlers.c`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/src/handlers.c)、[`wifi_config.c`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/src/wifi_config.c)、[`manager.c`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/src/manager.c)：状态、IPv4、凭据保存及退出时序。
- [`manager.h`](https://github.com/espressif/esp-idf/blob/v5.5.2/components/wifi_provisioning/include/wifi_provisioning/manager.h)：Security1 参数类型、生命周期 API。

### 字段核对摘要

| 消息 | 本客户端使用的官方字段号 |
| --- | --- |
| SessionData | `sec_ver=2`（值 1），`sec1=11`；拒绝 sec0 / sec2 |
| Sec1Payload | `msg=1`；sc0=20、sr0=21、sc1=22、sr1=23；msg 分别 0/1/2/3 |
| SessionCmd0 / Resp0 | client_pubkey=1；status=1、device_pubkey=2、device_random=3 |
| SessionCmd1 / Resp1 | client_verify_data=2；status=1、device_verify_data=3 |
| WiFiConfigPayload | msg=1；get/status/set/set-response/apply/apply-response 为字段 10..15、类型值 0..5 |
| CmdSetConfig | ssid=1、passphrase=2；不指定 bssid=3 / channel=4 |
| RespGetStatus | status=1、sta_state=2；fail_reason=10、connected=11、attempt_failed=12 |
| WifiConnectedState | ip4_addr=1（string），ssid=3；本页面显示 IPv4，不是自定义端点 |
| WiFiScanPayload | msg=1、**status=2（与 config 不同）**；消息体字段 10..15、类型 0..5 |
| CmdScanStart / Status / Result | blocking=1、passive=2、group_channels=3、period_ms=4；finished=1、count=2；start_index=1、count=2 |
| RespScanResult / WiFiScanResult | repeated entries=1；ssid=1、channel=2、**rssi=3（int32，有符号）**、bssid=4、auth=5 |

遵循 proto3 零值省略：status 缺省为 Success；Connected 状态值为 **0**、Connecting=1、
Disconnected=2、ConnectionFailed=3；AuthError=0、NetworkNotFound=1。
响应必须有正确类型和对应消息体；拒绝截断、无效 wire type、重复单字段及已知 oneof 冲突。
未知可跳过字段不解释，方便兼容；这不是通用 protobuf 库。

### Security1 连续流（重要）

1. 每次会话生成新的原生 X25519 临时密钥。校验设备公钥 32 字节、随机数 16 字节。
2. `K = X25519(client_private, device_public) XOR SHA256(UTF8(PoP))`；拒绝空 PoP、零共享秘密。
3. 设备随机数是 AES-256-CTR 初始 128 位 counter（大端递增）。
4. 同一个流先加密 **设备公钥**生成 client_verify，再处理 device_verify 并核对恢复的**客户端公钥**。
5. 后续加密请求、解密响应继续消费**同一个流**。官方 Python 的 `encrypt_data` 与 `decrypt_data`
   确实都调用 `self.cipher.update()`，不是一条发送流加一条接收流。
6. WebCrypto 是无状态接口，因此 `CtrStream` 保留 counter **和 0..15 的 offset**。
   每次重新生成当前块已用的前缀、丢弃此前缀输出，仅推进实际消费字节；不会把不满 16 字节的包向上取整消耗。
7. 单个会话禁止并发；传输失败不重试原包，立即销毁会话并要求重新选择设备。否则双方流位置可能不一致。

Security1 并非 AEAD，没有每条消息的独立完整性标签，也不是 TLS 或 Security2。
本项目为方便手动输入，使用每次会话重新生成的 **6 位纯数字 PoP**，保留前导零。
短码熵低于长随机码，不能宣称同等抗猜测强度；仅面向物理触发、限时、可信近距离配网，
不适用于对抗主动攻击者的高安全环境。协议仍使用 Security1，不做明文降级。

### BLE 映射和数据长度

默认服务 UUID：`1775244d-6b43-439b-877c-060f2d9bed07`。
页面允许手工指定另一个完整服务 UUID，浏览器选择器按此服务过滤，每次都需要用户选择。

只读取该服务下 characteristic 的 **Characteristic User Description（0x2901）**，
用其 UTF-8 名称建立 `prov-session` / `prov-config` / 可选 `prov-scan` 映射。
缺描述符、重名、缺必要端点或读写属性不匹配即停止。**不按列表顺序、不猜 UUID、不作硬编码 UUID 回退**。
官方 Python 有发现失败后的 UUID fallback；此客户端有意采用更严格的失败策略。
`proto-ver` 可被发现但不必请求；实际 Security1 握手和证明校验才是硬性门槛。

一条请求严格执行 write-with-response → read，不能把一个 protobuf 拆成多次普通写入，
因为每次写入都会触发 IDF 的独立请求处理。
浏览器/系统必须支持当前 ATT MTU 或可靠长写/长读；Web Bluetooth 没有通用的手动协商 MTU 接口。
Security1 首包约 41 字节、最大本页面凭据包约 105 字节，不能假设 20 字节写入就够。
扫描一次只取一个结果，降低响应大小，但仍可能超过默认 23-byte MTU。
若系统不能完成整包写读，页面明确报错并关闭会话，**不会擅自分片或假报成功**。
此项必须在实际目标 Android / Windows / macOS 上验收，Node 测试无法验证。

## 配套固件接口与联调约定

配套实现为 `main/provision.c`，采用以下接口约定。
按当前源码/接口约定，网页无需私有协议端点：

1. 使用默认服务 UUID、设备名 **`PROV_EPAPER_<MAC6HEX>`**、每次会话随机生成的
   **6 位纯数字 PoP**（如 `042731`，保留前导零）。用户以设备屏幕为准核对设备名和校验码。
   采用 `wifi_prov_scheme_ble` + `WIFI_PROV_SECURITY_1`，保留官方 0x2901 描述符。
   页面仍允许更换服务 UUID，兼容开发者自定义固件，但不自动信任设备名或省略 PoP。
2. BLE 配网不需要 SoftAP / HTTP 服务；只在用户主动开启的短时管理模式运行。
   设备端有约 5 分钟会话预算（异常硬件阻塞不保证硬实时），正常分钟刷新/每日校时不运行 BLE。
3. 初次与主动重新配网均可进入。失败后固件调用
   `wifi_prov_mgr_reset_sm_state_on_failure()`，允许用户再次提交，**无需自动降级或重新握手**。
   页面见 `ConnectionFailed`（3）或 `Disconnected`（2，作为空闲/IDLE 处理）即停止当前轮询、
   保留已验证会话并提示重新输入/提交。仅 `Connecting`（1）继续轮询。
   协议本身没有另一个名为 IDLE 的枚举；页面不会凭空增加枚举值。
   若故障导致 BLE 断开，则重新选择设备并验证 PoP。页面不自动重发凭据，不使用 `prov-ctrl` 清除配置。
4. 只有拿到 IP 后固件才允许提交到应用 NVS **`clock_net`**，失败不替换原应用配置。
   固件在 manager 退出后清理 SDK 的临时 Wi-Fi NVS，再保存验证过的配置。
   **标准 GetStatus 仅能确认 Connected/IP，不能确认之后 `clock_net` 的 NVS 写入已成功**；
   页面因此明确显示“设备报告 Wi-Fi 已连接”，并要求以设备最终屏幕确认保存完成，不伪造保存 ACK。
   如需网页确认落盘，应另行设计有界的应用级确认协议；当前不增加私有端点。
5. 保留默认 auto-stop：拿到 IP 后仍允许查询 Connected；成功 GetStatus 会经
   `wifi_prov_mgr_done()` 触发退出，若客户端未查询则默认 30 秒后退出。
   **不要在 CRED_SUCCESS 回调提前停栈**。默认约 1 秒 cleanup delay 的 BLE read 窗口须实机验证；
   若操作系统过慢导致成功响应未读回，应在固件调整退出时序，而不是网页猜测成功。
6. 网页断开/关标签页不等于设备已停栈；设备最迟按硬超时自行 deinit。
   当前固件不使用 FREE_BTDM / FREE_BLE，以免本次启动释放内存后无法再次进入管理，
   而是在退出时停止并 deinit controller/host。此策略仍需以实机电流和第二次进入管理确认。
7. 不记录 PoP/密钥/凭据 DEBUG 日志；PoP 不放 URL 或仓库。NVS 未加密仍有物理读取风险。

固件 `d4cefd5` 已通过 ESP-IDF 5.5.2 CI 完整构建，网页也已在 HTTPS Pages 加载验证。
仍需实机验收：BLE host/controller 的真实行为、设备屏幕最终保存结果、
默认 cleanup delay 是否足够，以及失败重置/硬超时/重复进入管理的硬件验收。

另外发现 v5.5.2 `handlers.c` 的 password 采用 `strlcpy(..., sizeof(sta.password))`，
容量为 64（含结尾 NUL）。因此页面**明确拒绝 64 字符原始十六进制 PSK**，避免被截成 63 字符。
当前支持 8–63 个可打印 ASCII 密码、1–32 UTF-8 字节 SSID（含中文），
不支持非 UTF-8 SSID、含 NUL SSID、WEP、企业 EAP；开放网络必须显式确认。

## 隐私与浏览器兼容性

- 基础检测实际运行 X25519 keygen/import/derive、SHA-256、AES-CTR，不只判断 User-Agent。
  另查 secure context、`requestDevice`、可用时的 `getAvailability()`、顶层页面。
  即使通过，仍不代表 GATT 或 OS 已兼容。
- 面向 Android Chrome / Edge、支持的桌面 Chrome / Edge；iPhone / iPad 普通浏览器不支持。
  Firefox/Safari 通常没有 Web Bluetooth；Linux 的系统/浏览器后端配置不保证可用。
- 无 `fetch`/XHR/WebSocket/analytics/CDN、无 localStorage/IndexedDB/cookie 保存凭据、无日志输出凭据。
  CSP 的 `connect-src 'none'` 阻止网络 API；脚本/样式只能从本站相对路径加载，表单禁止导航提交。
  GitHub Pages 仍收到网页/模块的普通 HTTPS 访问请求，此政策不能消除托管方访问日志。
- PoP 不读 URL 参数，不生成含凭据的链接；密码提交后清空，退出/取消清空输入并销毁会话。
  key import 后擦除可控 raw-key 字节缓冲；JS 字符串、WebCrypto 和浏览器内存不能保证安全擦除。
  浏览器扩展、恶意托管更新及密码管理器不在页面可完全防御的范围，HTTPS 也不是端到端供应链保证。
- 单次 BLE 操作超时 20 秒，扫描等待 30 秒，配网查询 90 秒，网页连接会话最长 5 分钟。
  断开/超时不是成功。只有 config 响应 Success 且 GetStatus 明确 Connected 时显示 LAN IPv4；
  不把这一响应冒充随后应用 NVS 的保存 ACK。Disconnected/失败停止查询并允许用户重试。
  该 IP 不代表固件常驻 LAN HTTP 服务，页面不会尝试用 HTTPS 访问局域网 HTTP 地址。

## 测试结果与待验收项

已有 Node 测试（不安装依赖）：

- protobuf 精确命令字节、UTF-8 长度、proto3 默认零值、扫描重复字段、负 RSSI、IPv4。
- 畸形数据、重复字段、oneof 冲突、错误响应/协议状态拒绝。
- RFC 7748 Alice/Bob 固定 X25519 key + SHA256 PoP XOR 固定 AES key。
- **独立 Python cryptography 46.0.5 向量**：同一个 encryptor，握手双向证明，以及其后
  2/6/20/4 字节的双向包序列；任意拆分（含 1 字节）与一次处理结果一致。
- 完整固定握手 + 交替加密配置请求；错误 PoP、Security0 降级、传输失败后禁用会话。
- Disconnected/IDLE 与 ConnectionFailed 均归类为可重试终态，而非一直轮询。
- BLE mock 用任意 UUID 证明按 0x2901 名称映射；写后读；缺描述符失败；写失败不重试。

**已验证**：13 项协议/传输模拟单测、浏览器能力检测和取消流程、局域网管理 UI 模拟、HTTPS 页面加载、ESP-IDF 5.5.2 编译。
**尚未验证**：真实 BLE/GATT/MTU、完整浏览器配网交互、路由器连接、PoP 屏幕展示、NVS 迁移及退出功耗。
验收前需至少：

1. 编译并烧录本项目 BLE 固件，在 Android Chrome 和至少一种桌面 Chrome/Edge 上完整配网。
2. 测正确/错误 PoP、手动 UUID 不符、取消选择、权限拒绝、关闭蓝牙、连接中断及低 MTU/长写失败。
3. 测中文/隐藏 SSID、开放网络显式确认、错误密码、AP 不存在、DHCP 超时、成功返回 LAN IP。
4. 凭据提交途中取消/关标签页，以及重新进入管理后重试；不能出现旧异步操作恢复新会话状态。
5. 查浏览器 Network/Storage：除静态资源外无网络请求，无凭据持久化；检查屏幕和固件日志不泄漏密码。
6. 成功响应读回后 BLE 停止；失败/取消最迟设备端硬超时停栈；下次校时使用新 SSID。
7. 离开管理恢复节能时钟，确认 AP 不存在，蓝牙不广播、HTTP 服务未启动，并实测电流。
