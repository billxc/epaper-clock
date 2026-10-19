import { BleTransport, serviceUUID } from './ble.mjs';
import { Security1, cryptoCheck } from './security1.mjs';
import { configRequest, configResponse, credentials, wifiStatus, wifiDisposition, scanRequest, scanResponse, scanEntries, concat, uint, number } from './protocol.mjs';

const $ = id => document.getElementById(id);
let compatible = false, transport = null, session = null, busy = false, epoch = 0, expiry;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const show = message => { $('status').textContent = message; };
function controls() {
  $('connect').disabled = !compatible || busy || !!transport;
  $('disconnect').disabled = !transport && !busy;
  $('pop').disabled = $('service').disabled = busy || !!transport;
  $('wifi-fields').disabled = busy || !session;
  $('scan').disabled = !transport?.endpoints.has('prov-scan');
}
function clearInputs() {
  for (const id of ['pop', 'ssid', 'password']) $(id).value = '';
  $('open-network').checked = false;
  $('networks').replaceChildren(new Option('尚未扫描', ''));
}
function close(message) {
  epoch++; clearTimeout(expiry);
  session?.destroy(); session = null;
  transport?.close(); transport = null;
  busy = false; clearInputs(); controls();
  if (message) show(message);
}
function errorMessage(error) {
  if (error.name === 'NotFoundError') return '未选择设备，或设备不在管理模式/服务 UUID 不匹配。';
  if (error.name === 'NotAllowedError' || error.name === 'SecurityError') return '浏览器/系统拒绝蓝牙权限，请检查 HTTPS、附近设备权限及系统设置。';
  if (error.name === 'NetworkError') return 'BLE 通信失败。可能是 PoP 错误、设备退出管理或系统 MTU/长写不兼容；请重新开启管理并连接。';
  return error.message || '发生未知错误';
}
async function operation(task) {
  if (busy || !session) return;
  const mine = epoch;
  busy = true; controls();
  const current = session;
  try { await task(current, () => { if (mine !== epoch) throw new Error('操作已取消'); }); }
  catch (error) { if (mine === epoch) close(`未确认配网成功：${errorMessage(error)}\n会话已关闭；如已提交，请先查看设备状态，不要把断开视为成功。`); }
  finally { if (mine === epoch) { busy = false; controls(); } }
}
$('connect').addEventListener('click', async () => {
  if (!compatible || busy || transport) return;
  let uuid, pop = $('pop').value;
  try {
    uuid = serviceUUID($('service').value);
    if (!pop || new TextEncoder().encode(pop).length > 256) throw new Error('请填写设备显示的 PoP（最多 256 字节）');
  } catch (error) { show(errorMessage(error)); return; }
  const mine = ++epoch;
  busy = true; $('pop').value = ''; controls(); show('请在浏览器弹窗中明确选择你的设备…');
  try {
    // Keep this before any await: Web Bluetooth requires transient user activation.
    const device = await BleTransport.choose(uuid);
    if (mine !== epoch) return;
    const current = new BleTransport(device, () => {
      if (transport === current) close('BLE 已断开，尚未确认配网成功。请查看设备屏幕；需要时重新开启管理模式。');
    });
    transport = current; controls();
    expiry = setTimeout(() => close('网页会话已满 5 分钟，已断开并清空。设备端仍须自行执行管理模式超时。'), 5 * 60 * 1000);
    show('正在读取 endpoint 描述符并进行 Security1 + PoP 身份验证…');
    await current.open(uuid);
    if (mine !== epoch) return;
    const established = await Security1.establish((...args) => current.exchange(...args), pop);
    if (mine !== epoch) { established.destroy(); return; }
    session = established;
    show('设备 PoP 验证通过。请输入你自己的 2.4 GHz 局域网 SSID 和密码。');
  } catch (error) { if (mine === epoch) close(`连接失败：${errorMessage(error)}\n没有降级到 Security0。`); }
  finally {
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
  operation(async (secure, active) => {
    show('正在通过加密 BLE 发送凭据…');
    try { configResponse(await secure.request('prov-config', packet), 3); }
    finally { packet.fill(0); }
    active();
    configResponse(await secure.request('prov-config', configRequest(4)), 5);
    active(); show('设备已接受配置，正在查询连接结果（不代表已连接成功）…');
    const deadline = Date.now() + 90000;
    while (Date.now() < deadline) {
      active();
      const status = wifiStatus(await secure.request('prov-config', configRequest(0)));
      active();
      const disposition = wifiDisposition(status);
      if (disposition === 'connected') {
        close(`设备报告 Wi-Fi 已连接。\nLAN IPv4：${status.ip || '设备未返回 IP'}\n已断开网页 BLE 会话；请以设备屏幕确认配置保存完成。设备将自动退出管理模式。`);
        return;
      }
      if (disposition === 'retry') {
        show(`本次未连接成功：${status.reason || '设备已断开/空闲（Disconnected / IDLE）'}。\n已停止轮询，BLE 验证会话仍保留。请核对 SSID、重新输入密码并点击提交；如设备已退出管理模式，请重新开启并连接。`);
        return;
      }
      show(`正在连接…${status.reason || '等待 DHCP / 路由器响应'}。`);
      await delay(1000);
    }
    close('90 秒内未确认连接成功，已断开。请查看设备屏幕和路由器状态；凭据可能已保存，不能把超时当作成功。');
  });
});
$('scan').addEventListener('click', () => operation(async (secure, active) => {
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
$('networks').addEventListener('change', () => {
  if ($('networks').value) $('ssid').value = $('networks').value;
});
window.addEventListener('pagehide', () => close());

(async () => {
  try {
    if (!window.isSecureContext || !['https:', 'http:'].includes(location.protocol)) throw new Error('需要 HTTPS（开发可用 localhost），不能直接打开本地文件');
    if (window.top !== window.self) throw new Error('请在独立标签页打开，不支持嵌入 iframe');
    if (!navigator.bluetooth?.requestDevice) throw new Error('此浏览器没有 Web Bluetooth；iPhone 普通浏览器不支持');
    if (navigator.bluetooth.getAvailability && !await navigator.bluetooth.getAvailability()) throw new Error('系统报告蓝牙不可用，请开启蓝牙/检查浏览器与驱动');
    await cryptoCheck();
    compatible = true;
    $('compatibility').textContent = '基础能力检测通过：Web Bluetooth、X25519、SHA-256、AES-CTR 可用。仍需连接实机验证 GATT / MTU 和固件兼容性。';
  } catch (error) { $('compatibility').textContent = `当前不能使用：${errorMessage(error)}。请改用受支持的 Chrome / Edge 与 HTTPS 页面。`; }
  controls();
})();
