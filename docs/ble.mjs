import { text } from './protocol.mjs';
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
    return navigator.bluetooth.requestDevice({ filters: [{ services: [uuid] }] });
  }
  constructor(device, onDisconnect) {
    this.device = device; this.endpoints = new Map(); this.closed = false; this.busy = false;
    this.listener = () => { this.close(); onDisconnect(); };
    device.addEventListener('gattserverdisconnected', this.listener);
  }
  async bounded(promise) {
    let timer;
    try {
      return await Promise.race([promise, new Promise((_, reject) => {
        timer = setTimeout(() => { this.close(); reject(new Error('BLE 操作超时；会话已关闭，请重新选择设备')); }, 20000);
      })]);
    } finally { clearTimeout(timer); }
  }
  async open(uuid) {
    try {
      const connecting = this.device.gatt.connect();
      connecting.then(() => { if (this.closed) this.device.gatt.disconnect(); }, () => {});
      const server = await this.bounded(connecting);
      const service = await this.bounded(server.getPrimaryService(uuid));
      const characteristics = await this.bounded(service.getCharacteristics());
      for (const characteristic of characteristics) {
        const descriptors = await this.bounded(characteristic.getDescriptors());
        const descriptor = descriptors.find(d => d.uuid === '00002901-0000-1000-8000-00805f9b34fb');
        if (!descriptor) continue;
        const name = text.decode(valueBytes(await this.bounded(descriptor.readValue()))).toLowerCase();
        if (this.endpoints.has(name)) throw new Error('设备 endpoint 描述符重名');
        this.endpoints.set(name, characteristic);
      }
      for (const name of ['prov-session', 'prov-config']) {
        const c = this.endpoints.get(name);
        if (!c || !c.properties.read || !c.properties.write || typeof c.writeValueWithResponse !== 'function') {
          throw new Error(`缺少可读写 endpoint：${name}（需要 0x2901 描述符）`);
        }
      }
      if (this.closed) throw new Error('BLE 连接已关闭');
    } catch (error) { this.close(); throw error; }
  }
  async exchange(name, data) {
    if (this.closed || !this.device.gatt.connected || this.busy) throw new Error('BLE 已断开或请求并发');
    const c = this.endpoints.get(name);
    if (!c) throw new Error(`缺少 endpoint：${name}`);
    // Never split a protobuf into independent writes: each write invokes IDF's handler.
    if (data.length > 256) throw new Error('请求超过保守 GATT 长度上限');
    this.busy = true;
    try {
      await this.bounded(c.writeValueWithResponse(data));
      const result = valueBytes(await this.bounded(c.readValue()));
      if (!result.length) throw new Error('设备返回空 BLE 响应');
      return result;
    } catch (error) { this.close(); throw error; }
    finally { this.busy = false; }
  }
  close() {
    this.closed = true; this.endpoints.clear();
    this.device.removeEventListener('gattserverdisconnected', this.listener);
    if (this.device.gatt?.connected) this.device.gatt.disconnect();
  }
}
