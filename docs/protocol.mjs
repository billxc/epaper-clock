// Minimal ESP-IDF v5.5.2 protobuf wire codec; see README.md for field sources.
export const utf8 = new TextEncoder();
export const text = new TextDecoder('utf-8', { fatal: true });
export const concat = (...parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const p of parts) { out.set(p, offset); offset += p.length; }
  return out;
};
export function varint(value) {
  let n = BigInt(value);
  if (n < 0n) n = BigInt.asUintN(64, n);
  if (n > 0xffffffffffffffffn) throw new Error('protobuf 整数溢出');
  const out = [];
  do { out.push(Number(n & 127n) | (n > 127n ? 128 : 0)); n >>= 7n; } while (n);
  return Uint8Array.from(out);
}
export const uint = (id, n) => concat(varint(id * 8), varint(n));
export const bytes = (id, b) => concat(varint(id * 8 + 2), varint(b.length), b);
export function decode(data) {
  if (!(data instanceof Uint8Array) || data.length > 65536) throw new Error('protobuf 长度无效');
  const fields = new Map();
  let pos = 0;
  function readVarint() {
    let n = 0n;
    for (let i = 0; i < 10; i++) {
      if (pos >= data.length) throw new Error('protobuf 截断');
      const b = data[pos++];
      if (i === 9 && b > 1) throw new Error('protobuf varint 溢出');
      n |= BigInt(b & 127) << BigInt(i * 7);
      if (!(b & 128)) return n;
    }
    throw new Error('protobuf varint 无效');
  }
  while (pos < data.length) {
    const tag = readVarint(), id = Number(tag >> 3n), wire = Number(tag & 7n);
    if (!id || id > 0x1fffffff) throw new Error('protobuf 字段号无效');
    let value;
    if (wire === 0) value = readVarint();
    else {
      const size = wire === 2 ? Number(readVarint()) : wire === 1 ? 8 : wire === 5 ? 4 : -1;
      if (!Number.isSafeInteger(size) || size < 0 || size > data.length - pos) throw new Error('protobuf 长度/类型无效');
      value = data.slice(pos, pos + size); pos += size;
    }
    if (!fields.has(id)) fields.set(id, []);
    fields.get(id).push({ wire, value });
  }
  return fields;
}
function one(fields, id, wire, required = false) {
  const list = fields.get(id);
  if (!list) {
    if (required) throw new Error(`protobuf 缺少字段 ${id}`);
    return wire === 0 ? 0n : new Uint8Array();
  }
  if (list.length !== 1 || list[0].wire !== wire) throw new Error(`protobuf 字段 ${id} 类型/重复错误`);
  return list[0].value;
}
export const blob = (m, id, required = false) => one(m, id, 2, required);
export function number(m, id) {
  const n = one(m, id, 0);
  if (n > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error('protobuf 数值过大');
  return Number(n);
}
export function success(m, id = 1) {
  const status = number(m, id);
  if (status !== 0) throw new Error(`设备拒绝请求（协议状态 ${status}）`);
}
function payload(data, type, field, candidates) {
  const m = decode(data);
  if (number(m, 1) !== type) throw new Error('设备响应类型不匹配');
  for (const id of candidates) if (id !== field && m.has(id)) throw new Error('设备响应 oneof 冲突');
  return { outer: m, body: decode(blob(m, field, true)) };
}
export const configRequest = (type, body = new Uint8Array()) => concat(type ? uint(1, type) : new Uint8Array(), bytes(10 + type, body));
export function configResponse(data, type) {
  const { body } = payload(data, type, 10 + type, [10, 11, 12, 13, 14, 15]);
  success(body); return body;
}
export function credentials(ssid, password) {
  const s = utf8.encode(ssid), p = utf8.encode(password);
  if (!s.length || s.length > 32 || s.includes(0)) throw new Error('SSID 必须是 1–32 个 UTF-8 字节，不能含 NUL');
  // Personal WPA/WPA2/WPA3 only; WEP and enterprise are deliberately unsupported.
  // IDF v5.5.2 handlers.c uses strlcpy(..., sizeof(sta.password)=64):
  // a 64-character raw PSK would be truncated. Do NOT silently offer it.
  if (p.length && !(p.length >= 8 && p.length <= 63 && /^[\x20-\x7e]+$/.test(password))) {
    throw new Error('密码须为 8–63 个可打印 ASCII 字符；此 IDF handler 不支持 64 位原始 PSK，开放网络请留空');
  }
  return configRequest(2, concat(bytes(1, s), bytes(2, p)));
}
export function wifiStatus(data) {
  const m = configResponse(data, 1), state = number(m, 2);
  if (state > 3) throw new Error('未知 Wi-Fi 状态');
  if ([10, 11, 12].filter(id => m.has(id)).length > 1) throw new Error('Wi-Fi 状态 oneof 冲突');
  let ip = '', reason = '';
  if (state === 0) ip = text.decode(blob(decode(blob(m, 11, true)), 1));
  if (state === 3) {
    const failure = number(m, 10);
    reason = failure === 0 ? '认证失败（检查 Wi-Fi 密码）' : failure === 1 ? '未找到目标网络' : `未知失败原因（${failure}）`;
  }
  if (m.has(12)) reason = `连接重试中，剩余 ${number(decode(blob(m, 12)), 1)} 次`;
  return { state, ip, reason };
}
// Disconnected/idle and failed are terminal for this attempt, not polling states.
// Firmware resets its provisioning failure state to accept a new user submission.
export function wifiDisposition({ state }) {
  if (state === 0) return 'connected';
  if (state === 1) return 'connecting';
  if (state === 2 || state === 3) return 'retry';
  throw new Error('未知 Wi-Fi 状态');
}
export const sessionRequest = (type, body) => concat(uint(2, 1), bytes(11, concat(type ? uint(1, type) : new Uint8Array(), bytes(20 + type, body))));
export function sessionResponse(data, type) {
  const m = decode(data);
  if (number(m, 2) !== 1 || m.has(10) || m.has(12)) throw new Error('仅支持 Security1，拒绝安全降级');
  const { body } = payload(blob(m, 11, true), type, 20 + type, [20, 21, 22, 23]);
  success(body); return body;
}
export const scanRequest = (type, body = new Uint8Array()) => configRequest(type, body);
export function scanResponse(data, type) {
  const { outer, body } = payload(data, type, 10 + type, [10, 11, 12, 13, 14, 15]);
  success(outer, 2); return body;
}
export function scanEntries(data) {
  const m = scanResponse(data, 5);
  return (m.get(1) || []).map(entry => {
    if (entry.wire !== 2) throw new Error('扫描结果类型错误');
    const r = decode(entry.value);
    const ssid = blob(r, 1);
    let name;
    try { name = text.decode(ssid); } catch { return null; } // manual UTF-8 input cannot reproduce non-UTF8 SSIDs
    return { ssid: name, channel: number(r, 2), rssi: Number(BigInt.asIntN(32, one(r, 3, 0))), auth: number(r, 5) };
  }).filter(Boolean);
}
