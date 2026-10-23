# Pages → 本地 HTTPS iframe（测试入口）

这是与 BLE 配网页面独立的实验入口：[`./index.html`](./index.html)。需要设备先通过其他受支持方式安装支持 HTTPS 和下列消息协议的测试固件。首次安装不在本页执行。此文是测试流程/边界说明，不是实机验收或已部署声明。

## 手动操作

1. 设备进入 LAN 管理，并与浏览器处于可互访网络。输入屏幕上的 HTTPS origin；测试设备默认端口为 **443**。接受 IP、localhost、本地 hostname、用户自有 DNS 域名、IPv6 和自定义端口，不按 hostname 判断是否属于 LAN，仅允许根 origin（可有末尾 `/`）。名称必须由你的环境正确解析；本页不探测 IP 或网段。禁止 HTTP、URL 用户名密码、任意路径、query、fragment。
2. 点击 **“在新窗口打开设备，完成证书信任与登录”**。自行核对设备身份、证书指纹/名称，按浏览器提示处理证书及 BasicAuth。设备密码只输入设备的新窗口，不输入父 Pages。
3. 回到 Pages，点击 **“连接iframe”**。只有经过 source、origin、协议和 nonce 校验的设备 `ready` 才算连接成功；iframe `load` 不算。每次手动重连新建 iframe；iframe 自己发生第二次加载时立即使旧会话失效，必须手动点击连接再重建，不自动重连，也不向已绑定的文档发送不同 nonce。
4. 点击 **“检查更新”** 只读取本 Pages 同源 `../firmware/manifest.json`，不下载 app、不写设备。显示 source commit、版本和大小；相同版本/下一槽容量不足会阻止安装。
5. 确认是所选设备、可靠供电后，点击 **“安装已检查的固件”** 并确认警告。父页才下载不可变 app，限制长度，完整校验大小及 WebCrypto SHA-256，再用 transferable `ArrayBuffer` 交给设备 iframe。设备独立验证 manifest、芯片/项目/版本/容量/镜像，自己同源调用已有 OTA API；SHA 由设备 OTA 核心再次校验。父页不直接 fetch 任何设备 API。
6. 固件转交设备、安装 pending 后，父页禁用地址修改、新窗口、连接及断开，避免移除 iframe 或增加 TLS 连接压力；下载尚未转交时仍可换址并取消下载。父页设置 `beforeunload` 离页保护，提醒安装及重启时保持可靠供电、不要关页。浏览器通常只显示自己的通用确认文字，且不保证在所有平台或强制关闭时弹窗，不能把该提示当供电保障。收到结果、错误或会话超时后恢复控件；安装终态使父会话失效，须手动重新连接获取新的设备版本摘要，不能复用升级前的 ready。检查/安装/打开/连接/断开入口拒绝 synthetic click，必须由真实用户操作触发。
7. `rebooting` 仅表示上传已接受，不表示新固件已启动。仅 `success` 才显示设备报告成功；`rollback` / `unconfirmed` / 超时需直接访问设备核对。不会自动重新下载、安装或重放请求。切换地址/重连/关闭页面不会取消已交给设备的 OTA。

**一次顶层证书例外不保证 iframe 可用**；顶层 BasicAuth 缓存也可能因浏览器、会话或隐私隔离无法被 iframe 复用。20 秒未收到 ready 时提示证书、BasicAuth、CSP、本地网络访问（LNA）权限及网络差异。可始终使用新窗口直接管理作为兜底，不要关闭浏览器安全检查。安装结果等待上限为 180 秒，之后使会话失效，勿把超时当失败且盲目再装。

设备使用持久自签证书而非公共 CA；首次生成的 IP SAN 不会因 DHCP 变化自动重签，地址变化可能造成名称不匹配。建议稳定 DHCP 地址，或自行正确解析证书里的稳定名称；名称存在于证书不表示自动具备 mDNS 服务。浏览器仍会检查证书有效期和名称。

## 消息协议 v1

所有消息为对象，公共字段：

```js
{ protocol: 'epaper-admin-v1', type: '…', nonce: '<32位小写hex>' }
```

- 父端为每个新 iframe 生成 128-bit 随机 nonce。只向用户输入的精确 HTTPS origin 发送。
- 设备只绑定来自实际 `window.parent` 且 origin 为 `https://billxc.github.io` 的首个合法 `hello`；同 nonce 重试允许。父页在首次加载后每 1.5 秒重发同 nonce hello，ready 后停止。
- 父端只接收 `event.source === iframe.contentWindow`、精确设备 origin、匹配协议及当前 nonce 的消息；不接受 sibling、null origin 或旧会话。更改输入立即销毁 iframe、取消父端下载、清除 manifest/requestId；每次重连新建 iframe。后续 iframe 文档加载使会话失效，待用户手动重连再重建整个元素。

| 方向 | type | 附加字段 / 含义 |
| --- | --- | --- |
| 父 → 设备 | `hello` | 无；不续设备活动时间 |
| 设备 → 父 | `ready` | `version`, `project:'epaper-clock'`, `chip:9`, `next_slot_bytes`, `capabilities:['sha256','transfer-install']`；必须已完成鉴权的设备同源状态获取 |
| 父 → 设备 | `install` | `requestId` 为另一独立随机 32 小写 hex，`confirmed:true`, `manifest`, `buffer:ArrayBuffer`，以 `[buffer]` 转移所有权；仅真实点击及确认后发送 |
| 设备 → 父 | `result` | 匹配当前 `requestId`，`state:'rebooting' / 'success' / 'rollback' / 'unconfirmed'` |
| 设备 → 父 | `error` | `requestId?`, `code`, `message`；父端只显示固定白名单 code 的解释，不渲染设备任意 message。安装中必须匹配 requestId |

v1 无泛用 RPC 或 `status` 消息；未知类型被忽略。不接收密码、SSID、token、boot_id 或原始状态对象。设备消费 requestId 防重放；父端不自动安装。设备可仅在新接受的明确确认安装时调用一次同源 `/api/activity`，hello/状态轮询不续空闲管理时间。

父端 manifest 只接受现有 `tools/firmware_manifest.py` 的 schema 1：项目 `epaper-clock`、chip 9、完整 40 hex commit、版本等于 commit 前 7 位、288 B–16 MiB、64 hex SHA、文件名严格 `app-<commit>-<sha256>.bin`。只转发 schema 固定字段。SHA 是完整性校验，不是发布者签名。manifest 和 bin 仅同源请求，`credentials:'omit'`、`redirect:'error'`、`referrerPolicy:'no-referrer'`。固件 blob 不经过设备的跨源 fetch。

## 数据及浏览器限制

- 用户输入的一个地址仅在当前页面内存/表单中；不进入 URL、localStorage、日志、analytics 或服务器请求。断开/pagehide 清空输入；JavaScript 内存不保证可安全擦除。
- GitHub Pages 仍接收页面和固件请求；浏览器加载 iframe/新窗口会直接连接所选设备。父页 CSP `connect-src 'self'` 另行阻止跨源 API fetch；`frame-src https:` 仅允许 HTTPS iframe。无外部脚本/字体/analytics，不改既有 BLE 模块或 CSP。
- 父页面不收设备秘密，iframe 中的输入仅由设备页面处理。只输入你信任的设备 origin；父页面不鉴别设备证书身份，也不能读取浏览器证书例外状态。
- 设备 `frame-ancestors` 必须允许发布的父 origin。仅把这份父页复制到其他域名不足以让真实设备接受该父页。独立 mock 使用保留测试域名，不是修改设备 allowlist。
- 浏览器的证书例外、BasicAuth、第三方隔离、CSP 和 LNA 行为存在版本/策略差异，不保证所有平台可用。不请求浏览器绕过权限或全局安全设置。

## 本地回归（不接触设备）

已有 Node.js、`@playwright/test` 及 Chromium 时：

```sh
NODE_PATH="$(npm root -g)" node tools/test_lan_pages.cjs
NODE_PATH="$(npm root -g)" node tools/test_ble_feedback.cjs
```

第一项读取真实 `docs/lan/` 源码，使用全量拦截的 HTTPS `.test` / localhost route fixture；不访问真实服务器，不发送 OTA、不存测试凭据或私钥。显式 `chromiumSandbox:true`，不关闭 TLS/Web 安全，不修改用户证书信任。覆盖：

- origin/schema 拒绝规则；load 与 ready 区分；协议/nonce/summary 校验。
- 真实跨窗口 `postMessage` 的错误 origin、同 origin 错误 source、旧 nonce 与 requestId 拒绝。
- 用户检查/确认/取消；synthetic click 不请求或转交；仅真实确认后下载，大小/SHA 拒绝，逐字节 transfer 及发送端 buffer detached。
- pending 控件禁用及父页原生 beforeunload 对话框（取消离页保留 iframe）；结果/超时解除保护，下载失败不锁地址。
- 手动重连新 iframe；文档重载使旧会话失效、不自动重绑；换址中止下载；ready 超时 fail-closed。
- 父页无设备 API 请求，无 storage、认证头、cookie 或 referrer 外传。

这不是 TLS、证书例外、BasicAuth 登录缓存、真实公网 Pages → LAN LNA、ESP 镜像校验、设备持久化或实机 OTA 验收。先前纯本地双 HTTPS PoC 仅在受测 Chromium 中验证受信证书及顶层预登录 auth cache 可复用；未测试用户手动证书例外，也未验证真实公网到 LAN。正式设备的证书例外和局域网许可仍需用户后续测试；发布测试入口或固件不等于已刷板或实机验收。
