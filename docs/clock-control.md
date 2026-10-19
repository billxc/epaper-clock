# BLE 初始化／管理共用协议

本文件描述 `main/provision.c/.h` 与 Pages 前端的协议，主流程已接入；上层交互见
[interaction.md](interaction.md)。手动设时不依赖 LAN、HTTP、NTP 或手机当前 Wi-Fi。
固件 `b420e0f` 已通过完整构建及软件测试；真实 BLE／板端联调仍待验收。

## 传输与安全

- 继续使用 ESP-IDF 5.5.2 provisioning BLE、Security1 和每次开启新生成的六位数字 PoP。
  `000123` 是完整字符串，不能转换为数字 `123`。错误 PoP、缺少自定义 endpoint 都失败关闭；没有 Security0／明文兼容路径。
- manager init 后、start 前调用 `wifi_prov_mgr_disable_auto_stop(1000)` 和
  `wifi_prov_mgr_endpoint_create("clock-control")`；start Security1 后才 endpoint register。
- 所有 endpoint 用特征的 **0x2901 User Description** 名称发现，不能假设特征 UUID。
  页面必需 `prov-session`、`prov-config`、`clock-control`；`prov-scan` 可选。
- `clock-control` 是普通 protocomm endpoint，复用已建立的 Security1 **同一条双向 CTR 字节流**，
  和 `prov-config`、`prov-scan` 请求串行交错。只有握手走 `prov-session`。
  不得使用裸 `transport.exchange()` 发送 JSON，也不能独立初始化另一条 CTR 流。
- 请求是 UTF-8 JSON 对象，1–128 字节，不含尾 NUL。严格限定字段数和类型，不接受重复字段、
  额外字段、嵌入 NUL 或转义字符串；操作名不需要转义。返回不含 SSID／已存 Wi-Fi 密码／PoP。
- Pages 仅本地模块，无外部脚本、上传、localStorage。CSP 保持 `connect-src 'none'` 等限制。
  GitHub Pages 只提供静态网页，仍会接收常规页面资源请求。

## 请求与响应

### 只读状态（不续期）

```json
{"op":"status"}
```

示例（本次第 2 次提交已保存）：

```json
{
  "ok": true,
  "wifi_configured": true,
  "wifi_saved": true,
  "wifi_state": "saved",
  "wifi_ready": false,
  "wifi_attempt": 2,
  "rtc_valid": true,
  "rtc_time": "2026-07-01 12:34:56",
  "epoch": 1782880496,
  "time_set": false
}
```

- `wifi_configured`：设备已有应用层配置，包括进入前就有的配置。不是本次成功证明。
- `wifi_saved`：**当前 attempt** 的候选已通过 GOT_IP 验证，且服务主循环的
  `network_save_config()` 已返回成功。仅 `wifi_state == "saved"` 时为 true。
  重新 prepare 开始新 attempt 后变为 false，但不删除先前成功保存的配置。
- `wifi_attempt`：本次 BLE 服务从 0 开始，准备完成时递增；提交前记录，保存 ACK 必须同号。
- `wifi_ready`：SDK 已完成准备且可接收凭据，仅 `ready` 时为 true。不是收到 prepare 请求就立即承诺就绪。
- `rtc_valid`／`rtc_time`：PCF RTC 有效性及**设备本地**日历字符串；无效时 `rtc_time` 为空、`epoch` 为 null。
  额外检查本地年份、mktime 规范化往返，避免把无效日历归一化后误报有效。
- `epoch`：将 RTC 本地日历按设备 TZ 转回 UTC 的整数秒；不是浏览器时区。
- `time_set`：此次 BLE 服务曾执行手动 RTC 写入（累积），不是旧 RTC 有效标志。
- `wifi_state`：`idle`、`preparing`、`ready`、`connecting`、`verifying`、`saved`、
  `failed`、`save_error`、`prepare_error`。

### 准备下一次 Wi-Fi 提交

```json
{"op":"prepare_wifi"}
```

返回同一状态结构。第一次可能是 `preparing`，页面只轮询 `status`，最多等 10 秒；
**只在 `wifi_ready:true` 时**发送官方 `SetConfig` / `ApplyConfig` protobuf。
处于 `preparing`／`ready` 时重复 prepare 幂等，不增加 attempt。
处于连接／待保存阶段返回 `wifi_busy`，不抢占正在进行的提交。

主循环处理的状态重置：

1. 服务初始直接为 `ready`，attempt=1，兼容标准乐鑫客户端首次 Wi-Fi 提交；Pages 的首次 prepare 幂等返回同一就绪状态，无需 SDK reset。
2. `failed` → `wifi_prov_mgr_reset_sm_state_on_failure()` → `ready`。
3. 已保存／保存失败／准备失败 → `wifi_prov_mgr_reset_sm_state_for_reprovision()` → `ready`。
4. 重置失败 → `prepare_error`，不继续发送凭据；下一次明确用户操作可重试。

endpoint 回调只投递请求、立即返回，不等待主循环，不调用 manager API。
因此不会持着 transport／manager mutex 等待另一线程反向取得该锁。
**绝不能在 SDK Connected／CRED_SUCCESS 到达时立即 reset**：客户端还需要读取保存 ACK。
标准乐鑫 App 可直接进行首次 Wi-Fi 提交，但没有本项目手动设时和应用保存 ACK 界面；失败后可在设备重开配网窗口再试，原生 App 的实际行为仍需验收。
页面扫描 Wi-Fi 前也调用 prepare，作为主动 Wi-Fi 操作续期并复用已准备的 attempt；
如果之前已保存，这表示开始下一次修改的准备，不会删除应用配置。

候选只由 `WIFI_PROV_CRED_RECV` 复制，`CRED_SUCCESS` 仅标记为 `verifying`；
服务主循环调用 `network_save_config` 后才置 `saved`，失败置 `save_error`。
不会在回调、清理时保存，也不会用 Wi-Fi SDK NVS 中的候选当成应用配置。
前端不再把 `prov-config` 的 Connected 或 BLE 断开解释成“保存成功”。
提交后的保存轮询最多 90 秒，超时显示**结果未知**，可读取状态，不自动重发。

### 独立手动设时

```json
{"op":"set_time","epoch":1782880496}
```

- `epoch` 必须是 JSON number 表示的整数秒（不是字符串、毫秒、浮点小数），
  范围 **946684800..4102444799**（UTC 2000-01-01..2099-12-31）。
- 固件再以已经配置的设备 TZ 执行 `localtime_r`，要求本地年份也在 **2000..2099**，
  以免 PCF85063 两位年溢出。例如 UTC 边界值在某些设备时区仍会被拒绝。
- endpoint 线程直接调用受共享 RTC mutex 保护的 `rtc_set(epoch)`。
  无需 Wi-Fi，不调用 NTP，也不擅自修改设备 TZ／系统联网校时状态。
- 写入后 atomic 标志通知主 UI 线程；实际 `app_ui_message` 由服务主循环调用。
  返回同一状态结构，`time_set:true`，页面同时要求 `rtc_valid:true` 才显示写入成功。
- 当前 `rtc_set` 是 void，I2C 错误由其 `ESP_ERROR_CHECK` 终止，而非可恢复的成功返回。
  如以后改成可恢复错误 API，应只有返回成功才置位，错误返回 `ok:false`。

页面输入 `datetime-local` 加明确固定 UTC 偏移 `±HH:MM`，严格校验真实日历日期与闰日，
手动计算 `Date.UTC(...) - offset`。不依赖浏览器隐式日期解析／时区推断。
“手机当前时间”仅填表，保留秒和手机此刻的 UTC 偏移，不自动发送；夏令时日期需要用户核对。

### 错误

```json
{"ok":false,"error":"epoch_range"}
```

错误码：`invalid_request`、`session_ending`、`wifi_busy`、`epoch_range`、`local_year_range`。
内存不足则返回 ESP_ERR_NO_MEM，由安全传输层失败关闭；不重放已消费的 CTR 包。

## 生命周期与默认超时

- **initial=true**：295 秒无活动停止 BLE，但仍留在初始化业务模式。
  停止后显示“仍在初始化／中键重试／长按中键选单”，`app_poll` 继续运行；
  中键 select 重开服务、重新生成 PoP。错误也进入同一等待，不无限重启 SDK。
  旧 SSID 或旧有效 RTC 不能完成强制初始化。
- 本次成功保存 Wi-Fi，或已有 Wi-Fi 配置且本次手动设时，才自动请求返回 `APP_CLOCK`。未配置 Wi-Fi 时先设时间会继续留在同一会话等待配网；用户仍可通过选单主动选择离线时钟。保持 BLE 供页面读取 ACK：
  主循环观察到成功响应已生成后至少 2 秒，或本次完成后最多 15 秒。
  生成响应不等于已送达；未收到 ACK 的页面必须显示结果未知，不能把断开说成成功。
- **initial=false**：保存／设时不自动切业务模式，可继续设时、重复修改 Wi-Fi。
  默认连续 **300 秒无用户操作**返回 `APP_CLOCK`。按键、认证后的 prepare、
  凭据接收、成功设时续期，后台 status 不续期；没有额外总时长上限。
- 页面自己的 5 分钟无主动设置操作定时器只断开网页，不决定设备模式；
  手机填表或只读状态不续期。设备按键操作可以延长设备窗口，未必延长网页窗口。
- 两种模式都循环 `app_poll(&activity, &selected)`，任何非 `APP_NONE` action 在清理后原样返回。
- 手动 manager deinit 加入所有 transport 用户后，才释放栈上会话和 PoP。
  不永久 free BT memory，不使用无期限 `wifi_prov_mgr_wait()`。
- IDF start／configure_sta 强制 FLASH storage，因此清理仍需 `esp_wifi_restore()` 清 SDK Wi-Fi keys，
  然后恢复 `WIFI_STORAGE_RAM` 并 `network_finish_provisioning()`；不能 erase NVS 或 clock_net。
  清理出错记录日志，不把已经成功保存的应用配置 ACK 改说成回滚。

## 固件模块接口

```c
typedef struct {
    app_action_t next;
    bool wifi_saved;
    bool time_set;
} provision_result_t;
provision_result_t provision_run(bool initial);
```

1. 取代原 `bool provision_run(void)`：初始化／强制重配调用 `provision_run(true)`，
   菜单蓝牙管理调用 `provision_run(false)`。flags 是本次调用的累积事件，不代表旧 SSID／RTC。
2. supervisor 先 `app_ui_set_mode(APP_INITIALIZE / APP_BLE)`。初始化返回 `APP_CLOCK` 后，
   根据 `wifi_saved` 决定是否尝试联网校时；按父策略确认 RTC 有效后才真正进入时钟。
   **只有手动设时时不要强行依赖联网**，否则失去离线设时用途。
   管理模式默认保存、设时都留在 BLE，直到菜单或闲置返回。
3. 提供 `app_runtime.h` 的 `app_poll(bool *activity, bool *selected)`，在 UI 线程泵菜单、确认与短按。
   非 NONE action 优先于服务成功／闲置切换；在 supervisor 完成清理前保留 pending action。
4. `app_ui_present(frame,true)` 同步复制／保留背景；服务之后会擦除释放临时 frame。
   `app_ui_message` 也需同步消费／复制字符串；菜单打开时应只更新背景、不遮盖菜单。
   返回后父强制完整重绘时钟，不能沿用旧局部刷新缓存。
5. `rtc_read`、`rtc_set` 使用**同一个 mutex**；I2C 底层事务安全仍不足以保护多事务 STOP／写日历／START。
   应在全模块范围保护完整 RTC 操作（含其它 RTC 入口），设时前配置 TZ。
   endpoint 可调用 RTC，但 SDK event callback 不做 RTC/NVS/屏幕/managerAPI。
6. `network_prepare_provisioning` 初始化 STA，不主动连接、无竞争重连；
   `network_finish_provisioning` 即使部分失败也停止并反初始化、排空事件。
   `network_save_config` 必须保持失败不覆盖旧 clock_net 的事务语义，且不能要求先停 manager。
7. 链接 cJSON／provisioning／BLE 支持，不在其它回调重复停止 manager。
   NVS、network、RTC、epaper/UI init 均应在服务入口前完成；入口需 Wi-Fi/BT 已停、无并发 ADC/RF 熵源使用。

## 测试与实机验收

无需安装工具：

```sh
node --test docs/*.test.mjs
node --check docs/app.mjs
```

原有 13 项测试未改，新增 8 项覆盖：六位 PoP leading zero、JSON 类型及 epoch 范围、
UTC 偏移／闰日／无效日期、手机时间填充、状态及禁止密码字段、attempt 保存 ACK、
与 prov-config 共用 Security1 CTR 流、0x2901 自定义特征发现及缺失拒绝。
Node 测试不代替 ESP-IDF 编译／板端验证：

- 错误 PoP／旧固件／缺少描述符均不降级；`000123` 实机完成握手。
- 在管理模式首次保存、收到 ACK 后再次 prepare、SDK ready ACK、再次提交均成功；
  错误密码和 NVS 写失败保留旧配置，Connected 后不提前 reset。
- 无 Wi-Fi 时手动设时后读回 RTC；测试 UTC 范围及设备本地年份跨界、RTC mutex 并发。
- 强制初始化在已有配置和有效 RTC 时仍等待本次事件；295 秒暂停、中键重试、菜单退出。
- 两种模式在 connecting／verifying／准备重置等状态可长按菜单，清理后按选中 action 跳转。
- 保存后继续设时不离开管理；status 轮询不续期，主动操作与按键续期，300 秒闲置关闭服务。
- 初始化 ACK 窗口正常可读；人为断开／丢包不被网页误报成功。反复进出 BLE 不释放 BT memory。
- 测试目标手机／浏览器对大于 MTU 的响应长读兼容性（状态 JSON 约 230–300 字节）。
- 模拟 SDK key 清理错误，确认日志可见且 clock_net 不被误删；设备掉电在 cleanup 前仍是 SDK FLASH 持久化的剩余风险。
