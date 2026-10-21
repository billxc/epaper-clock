import { text } from './protocol.mjs';
import { bleLog, bleFailure } from './ble-log.mjs';
export const DEFAULT_SERVICE = '1775244d-6b43-439b-877c-060f2d9bed07';
export function serviceUUID(value) {
  const uuid = value.trim().toLowerCase();
  if (!/^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$/.test(uuid)) throw new Error('服务 UUID 必须为完整 128 位 UUID');
  return uuid;
}
const valueBytes = view => new Uint8Array(view.buffer, view.byteOffset, view.byteLength).slice();
export class BleTransport {
  // Called directly in click handler: no await before requestDevice.
  static choose(uuid) {
    bleLog('device.choose', { service: uuid });
    return navigator.bluetooth.requestDevice({ filters: [{ services: [uuid] }] });
  }
  constructor(device, onDisconnect, onProgress = () => {}) {
    this.device = device; this.endpoints = new Map(); this.closed = false; this.busy = false;
    this.onProgress = onProgress;
    this.listener = () => { bleLog('gatt.disconnected'); this.close(); onDisconnect(); };
    device.addEventListener('gattserverdisconnected', this.listener);
  }
  async bounded(promise, stage, label) {
    let timer;
    const started = performance.now();
    this.onProgress(label);
    bleLog(`${stage}.start`);
    try {
      const result = await Promise.race([promise, new Promise((_, reject) => {
        timer = setTimeout(() => {
          this.close();
          reject(new Error(`${label}超时（20 秒）；会话已关闭，请重新选择设备`));
        }, 20000);
      })]);
      bleLog(`${stage}.ok`, { elapsed_ms: Math.round(performance.now() - started) });
      return result;
    } catch (error) {
      bleFailure(`${stage}.failed`, error, { elapsed_ms: Math.round(performance.now() - started) });
      throw error;
    } finally { clearTimeout(timer); }
  }
  async open(uuid) {
    try {
      const connecting = this.device.gatt.connect();
      connecting.then(() => { if (this.closed) this.device.gatt.disconnect(); }, () => {});
      const server = await this.bounded(connecting, 'gatt.connect', '连接蓝牙 GATT');
      const service = await this.bounded(server.getPrimaryService(uuid), 'gatt.service', '蓝牙已连接，读取设置服务');
      const characteristics = await this.bounded(service.getCharacteristics(), 'gatt.characteristics', '设置服务已找到，发现特征');
      bleLog('gatt.characteristics.discovered', { count: characteristics.length });
      for (const [index, characteristic] of characteristics.entries()) {
        const progress = `识别设置端点 ${index + 1}/${characteristics.length}`;
        const descriptors = await this.bounded(characteristic.getDescriptors(), 'gatt.descriptors', progress);
        const descriptor = descriptors.find(d => d.uuid === '00002901-0000-1000-8000-00805f9b34fb');
        bleLog('gatt.descriptors.discovered', { characteristic: characteristic.uuid, count: descriptors.length, has_endpoint_name: !!descriptor });
        if (!descriptor) continue;
        const name = text.decode(valueBytes(await this.bounded(descriptor.readValue(), 'gatt.endpoint_name', progress))).toLowerCase();
        if (this.endpoints.has(name)) throw new Error('设备 endpoint 描述符重名');
        this.endpoints.set(name, characteristic);
        bleLog('gatt.endpoint.discovered', { endpoint: name, read: characteristic.properties.read, write: characteristic.properties.write });
      }
      for (const name of ['prov-session', 'prov-config']) this.requireEndpoint(name);
      if (this.closed) throw new Error('BLE 连接已关闭');
      bleLog('gatt.ready', { endpoints: [...this.endpoints.keys()] });
    } catch (error) { this.close(); throw error; }
  }
  requireEndpoint(name) {
    const c = this.endpoints.get(name);
    if (!c || !c.properties.read || !c.properties.write || typeof c.writeValueWithResponse !== 'function') {
      throw new Error(`缺少可读写 endpoint：${name}（需要 0x2901 描述符；不支持旧固件或明文降级）`);
    }
    return c;
  }
  async exchange(name, data) {
    if (this.closed || !this.device.gatt.connected || this.busy) throw new Error('BLE 已断开或请求并发');
    const c = this.endpoints.get(name);
    if (!c) throw new Error(`缺少 endpoint：${name}`);
    // Never split a protobuf into independent writes: each write invokes IDF's handler.
    if (data.length > 256) throw new Error('请求超过保守 GATT 长度上限');
    this.busy = true;
    try {
      bleLog('gatt.write', { endpoint: name, bytes: data.length });
      await this.bounded(c.writeValueWithResponse(data), 'gatt.write', `向 ${name} 发送请求`);
      const result = valueBytes(await this.bounded(c.readValue(), 'gatt.read', `读取 ${name} 响应`));
      bleLog('gatt.read', { endpoint: name, bytes: result.length });
      if (!result.length) throw new Error('设备返回空 BLE 响应');
      return result;
    } catch (error) { this.close(); throw error; }
    finally { this.busy = false; }
  }
  close() {
    if (!this.closed) bleLog('gatt.close');
    this.closed = true; this.endpoints.clear();
    this.device.removeEventListener('gattserverdisconnected', this.listener);
    if (this.device.gatt?.connected) this.device.gatt.disconnect();
  }
}
