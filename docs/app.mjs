import { BleTransport, serviceUUID } from './ble.mjs';
import { bleLog, bleFailure } from './ble-log.mjs';
import { Security1, cryptoCheck } from './security1.mjs';
import { configRequest, configResponse, credentials, scanRequest, scanResponse, scanEntries, concat, uint, number } from './protocol.mjs';
import { validatePop, localDateEpoch, phoneTime, requestControl, wifiOutcome } from './clock-control.mjs';

const $ = id => document.getElementById(id);
let compatible = false, transport = null, session = null, busy = false, epoch = 0, expiry;
let connectionPhase = '尚未连接设备';
function connectionProgress(message, state = 'busy') {
  connectionPhase = message;
  $('connection-feedback').textContent = message;
  $('connection-feedback').dataset.state = state;
}
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const show = message => { $('status').textContent = message; };
function controls() {
  $('connect').disabled = !compatible || busy || !!transport;
  $('connect').textContent = session ? '设备已验证连接' : busy ? '正在连接并验证…' : '选择蓝牙设备并验证';
  $('disconnect').disabled = !transport && !busy;
  $('pop').disabled = $('service').disabled = busy || !!transport;
  $('wifi-fields').disabled = $('time-fields').disabled = busy || !session;
  $('refresh').disabled = busy || !session;
  $('scan').disabled = busy || !session || !transport?.endpoints.has('prov-scan');
}
function clearInputs() {
  for (const id of ['pop', 'ssid', 'password', 'local-time']) $(id).value = '';
  $('open-network').checked = false;
  $('networks').replaceChildren(new Option('尚未扫描', ''));
}
function close(message) {
  epoch++; clearTimeout(expiry);
  session?.destroy(); session = null;
  transport?.close(); transport = null;
  busy = false; clearInputs(); controls();
  bleLog('ui.session.closed');
  if (message) { show(message); connectionProgress(message, 'closed'); }
}
function errorMessage(error) {
  if (error.name === 'NotFoundError') return '未选择设备，或设备不在管理模式/服务 UUID 不匹配。';
  if (error.name === 'NotAllowedError' || error.name === 'SecurityError') return '浏览器/系统拒绝蓝牙权限，请检查 HTTPS、附近设备权限及系统设置。';
  if (error.name === 'NetworkError') return 'BLE 通信失败。可能是 PoP 错误、设备退出管理或系统 MTU/长写不兼容；请重新开启管理并连接。';
  return error.message || '发生未知错误';
}
function renewExpiry() {
  clearTimeout(expiry);
  expiry = setTimeout(() => close('网页连续 5 分钟无主动设置操作，已断开并清空。状态轮询不延长设备服务；请以设备屏幕为准。'), 5 * 60 * 1000);
}
function deviceSummary(r) {
  return `Wi-Fi：${r.wifi_saved ? '本次设置已保存' : r.wifi_configured ? '已有配置' : '尚未设置'}。\n设备时间：${r.rtc_valid ? `${r.rtc_time}（设备本地时间）` : '尚未设置'}。${r.time_set ? '\n本次手动时间已保存。' : ''}`;
}
async function prepareWifi(secure, active) {
  let ready = await requestControl(secure, 'prepare_wifi'); active();
  const deadline = Date.now() + 10000;
  while (!ready.wifi_ready && ready.wifi_state === 'preparing' && Date.now() < deadline) {
    await delay(250); active();
    ready = await requestControl(secure, 'status'); active();
  }
  return ready;
}
async function operation(task) {
  if (busy || !session) return;
  const mine = epoch;
  busy = true; controls();
  const current = session;
  try { await task(current, () => { if (mine !== epoch) throw new Error('操作已取消'); }); }
  catch (error) { if (mine === epoch) close(`操作未获完整确认：${errorMessage(error)}\n会话已关闭；已收到的保存/设时 ACK 仍然有效，但不能把断开视为其它操作成功。`); }
  finally { if (mine === epoch) { busy = false; controls(); } }
}
$('connect').addEventListener('click', async () => {
  if (!compatible || busy || transport) return;
  let uuid, pop = $('pop').value.trim();
  try {
    uuid = serviceUUID($('service').value);
    pop = validatePop(pop);
  } catch (error) {
    bleFailure('ui.input_rejected', error);
    show(errorMessage(error)); connectionProgress(errorMessage(error), 'error'); return;
  }
  const mine = ++epoch;
  let connecting = true;
  const progress = message => {
    if (mine === epoch && connecting) connectionProgress(message);
  };
  busy = true; $('pop').value = ''; controls();
  show('请在浏览器弹窗中明确选择你的设备…'); progress('等待你在浏览器弹窗中选择设备');
  bleLog('ui.connection.start');
  try {
    // Keep this before any await: Web Bluetooth requires transient user activation.
    const device = await BleTransport.choose(uuid);
    if (mine !== epoch) return;
    bleLog('device.selected');
    progress('设备已选择，正在建立蓝牙连接');
    const current = new BleTransport(device, () => {
      if (transport === current) close(`蓝牙已断开（${connectionPhase}）。已收到的保存/设时 ACK 仍然有效；断开本身不代表其它操作成功。请查看设备屏幕，需要时重新进入蓝牙管理。`);
    }, progress);
    transport = current; controls();
    renewExpiry();
    await current.open(uuid);
    current.requireEndpoint('clock-control');
    if (mine !== epoch) return;
    const established = await Security1.establish((...args) => current.exchange(...args), pop, undefined, progress);
    if (mine !== epoch) { established.destroy(); return; }
    session = established;
    progress('身份验证已通过，正在读取设备状态');
    bleLog('control.initial_status.start');
    const status = await requestControl(established, 'status');
    if (mine !== epoch) return;
    bleLog('control.initial_status.ok');
    connectionProgress('验证完成，可以设置 Wi-Fi 和时间。下方两个设置区已启用。', 'ready');
    show(`设备已连接，可以设置 Wi-Fi 或时间。\n${deviceSummary(status)}`);
  } catch (error) {
    bleFailure('ui.connection.failed', error, { phase: connectionPhase });
    if (mine === epoch) {
      const message = `连接失败（${connectionPhase}）：${errorMessage(error)}\n会话已关闭，没有降级到 Security0。`;
      close(message); connectionProgress(message, 'error');
    }
  } finally {
    connecting = false;
    pop = '';
    if (mine === epoch) { $('pop').value = ''; busy = false; controls(); }
  }
});
$('disconnect').addEventListener('click', () => close('已断开并清空。设备端应在管理超时后关闭蓝牙；网页不能远程强制关闭设备蓝牙。'));
$('wifi').addEventListener('submit', event => {
  event.preventDefault();
  if (busy || !session) return;
  let packet;
  try {
    if (!$('password').value && !$('open-network').checked) throw new Error('密码为空：若确为开放网络，请显式勾选确认');
    if ($('password').value && $('open-network').checked) throw new Error('已勾选开放网络但填写了密码，请核对');
    packet = credentials($('ssid').value, $('password').value);
  } catch (error) { show(errorMessage(error)); return; }
  $('password').value = ''; $('open-network').checked = false;
  renewExpiry();
  operation(async (secure, active) => {
    try {
      show('正在准备 Wi-Fi 设置…');
      const ready = await prepareWifi(secure, active);
      if (!ready.wifi_ready) {
        show(`设备未就绪（${ready.wifi_state}），没有发送凭据；请重试。`);
        return;
      }
      const attempt = ready.wifi_attempt;
      show('正在发送 Wi-Fi 设置…');
      configResponse(await secure.request('prov-config', packet), 3); active();
      packet.fill(0);
      configResponse(await secure.request('prov-config', configRequest(4)), 5); active();
      const deadline = Date.now() + 90000;
      while (Date.now() < deadline) {
        const status = await requestControl(secure, 'status'); active();
        const outcome = wifiOutcome(status, attempt);
        if (outcome === 'saved') {
          show(`Wi-Fi 设置成功。\n${deviceSummary(status)}\n蓝牙管理可继续设置时间；首次初始化完成后，设备会回到时钟。`);
          return;
        }
        if (outcome === 'failed') {
          show(`本次未保存成功（${status.wifi_state}），不覆盖旧配置。请核对后重新提交；安全会话保留。`);
          return;
        }
        show('正在连接网络并保存设置，请稍候…');
        await delay(700); active();
      }
      show('90 秒内未收到保存 ACK，结果未知，不视为成功。可点“读取设备状态”；若仍连接中请等待，不自动重发凭据。');
    } finally { packet.fill(0); }
  });
});
$('scan').addEventListener('click', () => operation(async (secure, active) => {
  // Scanning is an explicit Wi-Fi operation. Prepare once to renew the device
  // activity window, and to avoid scanning in an old SDK success state.
  renewExpiry();
  const ready = await prepareWifi(secure, active);
  if (!ready.wifi_ready) { show(`扫描前设备未就绪（${ready.wifi_state}），请重试。`); return; }
  show('设备正在扫描附近 Wi-Fi…');
  // Nonblocking, active scan, 5-channel groups, 120 ms/channel; official wifi_scan.proto.
  scanResponse(await secure.request('prov-scan', scanRequest(0, concat(uint(3, 5), uint(4, 120)))), 1);
  const deadline = Date.now() + 30000;
  let count = null;
  while (Date.now() < deadline) {
    active();
    const status = scanResponse(await secure.request('prov-scan', scanRequest(2)), 3);
    active();
    if (number(status, 1)) { count = number(status, 2); break; }
    await delay(700);
  }
  if (count === null) throw new Error('Wi-Fi 扫描超时');
  const options = [new Option('请选择或手动输入', '')], seen = new Set();
  // One result per response avoids MTU-sized multi-result payloads.
  for (let i = 0; i < Math.min(count, 30); i++) {
    active();
    const entries = scanEntries(await secure.request('prov-scan', scanRequest(4, concat(uint(1, i), uint(2, 1)))));
    active();
    for (const r of entries) {
      if (!r.ssid || seen.has(r.ssid)) continue;
      seen.add(r.ssid);
      const supported = [0, 2, 3, 4, 6, 7].includes(r.auth) && r.channel <= 14;
      const option = new Option(`${r.ssid} · ${r.rssi} dBm${supported ? '' : ' · 不支持的认证/频段'}`, r.ssid);
      option.disabled = !supported; options.push(option);
    }
  }
  $('networks').replaceChildren(...options);
  show(`扫描完成，设备返回 ${count} 项，读取前 ${Math.min(count, 30)} 项并去重。隐藏网络请手动输入；密码不会自动读取。`);
}));
$('refresh').addEventListener('click', () => operation(async (secure, active) => {
  const status = await requestControl(secure, 'status'); active();
  show(deviceSummary(status)); // Read-only: no idle-window renewal.
}));
$('phone-time').addEventListener('click', () => {
  const time = phoneTime();
  $('local-time').value = time.value;
  $('timezone').value = time.offset;
  show('已填入手机当前日期时间和此刻 UTC 偏移；尚未发送。请核对后点击写入 RTC。');
});
$('time').addEventListener('submit', event => {
  event.preventDefault();
  if (busy || !session) return;
  let time;
  try { time = localDateEpoch($('local-time').value, $('timezone').value); }
  catch (error) { show(errorMessage(error)); return; }
  renewExpiry();
  operation(async (secure, active) => {
    show('正在设置设备时间，不需要连接互联网…');
    const result = await requestControl(secure, 'set_time', time); active();
    if (!result.time_set || !result.rtc_valid) throw new Error('设备未确认 RTC 写入有效');
    show(`时间设置成功。\n${deviceSummary(result)}\n蓝牙管理不会因此自动退出；首次初始化还需要完成 Wi-Fi 设置。`);
  });
});
$('networks').addEventListener('change', () => {
  if ($('networks').value) $('ssid').value = $('networks').value;
});
window.addEventListener('pagehide', () => close());

(async () => {
  bleLog('capability.start');
  try {
    if (!window.isSecureContext || !['https:', 'http:'].includes(location.protocol)) throw new Error('需要 HTTPS（开发可用 localhost），不能直接打开本地文件');
    if (window.top !== window.self) throw new Error('请在独立标签页打开，不支持嵌入 iframe');
    if (!navigator.bluetooth?.requestDevice) throw new Error('此浏览器没有 Web Bluetooth；iPhone 普通浏览器不支持');
    if (navigator.bluetooth.getAvailability && !await navigator.bluetooth.getAvailability()) throw new Error('系统报告蓝牙不可用，请开启蓝牙/检查浏览器与驱动');
    await cryptoCheck();
    compatible = true;
    bleLog('capability.ok');
    $('compatibility').textContent = '基础能力检测通过：Web Bluetooth、X25519、SHA-256、AES-CTR 可用。仍需连接实机验证 GATT / MTU 和固件兼容性。';
  } catch (error) {
    bleFailure('capability.failed', error);
    $('compatibility').textContent = `当前不能使用：${errorMessage(error)}。请改用受支持的 Chrome / Edge 与 HTTPS 页面。`;
    connectionProgress('浏览器能力检测未通过，请查看上方提示。', 'error');
  }
  controls();
})();
