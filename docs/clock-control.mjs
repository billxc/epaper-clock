// All packets go through Security1.request('clock-control', ...), never raw GATT.
import { utf8, text } from './protocol.mjs';
export const MIN_EPOCH = 946684800;
export const MAX_EPOCH = 4102444799;
export function validatePop(value) {
  if (typeof value !== 'string' || value.length !== 6 || !/^[0-9]{6}$/.test(value)) throw new Error('请输入屏幕上的 6 位数字校验码（保留开头的 0）');
  return value; // Never parseInt: 000123 is a different PoP from 123.
}
export function validEpoch(epoch) {
  if (!Number.isSafeInteger(epoch) || epoch < MIN_EPOCH || epoch > MAX_EPOCH) throw new Error('UTC epoch 必须为 2000–2099 范围内的整数秒');
  return epoch;
}
export function controlRequest(op, epoch) {
  if (!['status', 'prepare_wifi', 'set_time'].includes(op)) throw new Error('未知 clock-control 操作');
  if (op === 'set_time') return utf8.encode(JSON.stringify({ op, epoch: validEpoch(epoch) }));
  if (epoch !== undefined) throw new Error('此操作不接受 epoch');
  return utf8.encode(JSON.stringify({ op }));
}
const states = ['idle', 'preparing', 'ready', 'connecting', 'verifying', 'saved', 'failed', 'save_error', 'prepare_error'];
const fields = ['ok', 'wifi_configured', 'wifi_saved', 'wifi_state', 'wifi_ready', 'wifi_attempt', 'rtc_valid', 'rtc_time', 'epoch', 'time_set'];
export function controlResponse(data) {
  if (!(data instanceof Uint8Array) || !data.length || data.length > 512) throw new Error('clock-control 响应长度无效');
  const r = JSON.parse(text.decode(data));
  if (!r || Array.isArray(r) || typeof r !== 'object' || typeof r.ok !== 'boolean') throw new Error('clock-control JSON 无效');
  if (!r.ok) {
    if (Object.keys(r).sort().join(',') !== 'error,ok' || typeof r.error !== 'string') throw new Error('clock-control 错误响应无效');
    throw new Error(`设备拒绝操作：${r.error}`);
  }
  if (Object.keys(r).sort().join(',') !== [...fields].sort().join(',')) throw new Error('clock-control 字段不匹配（禁止密码字段）');
  for (const key of ['wifi_configured', 'wifi_saved', 'wifi_ready', 'rtc_valid', 'time_set']) {
    if (typeof r[key] !== 'boolean') throw new Error(`clock-control ${key} 类型无效`);
  }
  if (!states.includes(r.wifi_state) || !Number.isSafeInteger(r.wifi_attempt) || r.wifi_attempt < 0 ||
      r.wifi_saved !== (r.wifi_state === 'saved') || r.wifi_ready !== (r.wifi_state === 'ready') ||
      (r.wifi_saved && !r.wifi_configured) || (r.wifi_ready && !r.wifi_attempt)) throw new Error('clock-control Wi-Fi ACK 无效');
  if (r.rtc_valid ? (!Number.isSafeInteger(r.epoch) || typeof r.rtc_time !== 'string' || !/^20\d\d-\d\d-\d\d \d\d:\d\d:\d\d$/.test(r.rtc_time)) : (r.epoch !== null || r.rtc_time !== '')) throw new Error('clock-control RTC 状态无效');
  return r;
}
// Fixed UTC offset is explicit: do NOT let Date.parse silently use browser TZ or
// normalize invalid dates. Device additionally checks its own local RTC year.
export function localDateEpoch(value, offset) {
  const match = /^(20\d\d)-(\d\d)-(\d\d)T(\d\d):(\d\d)(?::(\d\d))?$/.exec(value);
  const zone = /^([+-])(\d\d):(\d\d)$/.exec(offset);
  if (!match || !zone || match[0] !== value || zone[0] !== offset || +zone[2] > 14 || +zone[3] > 59 || (+zone[2] === 14 && +zone[3] !== 0)) throw new Error('日期须为 2000–2099，时区须为 ±HH:MM（最多 ±14:00）');
  const [year, month, day, hour, minute, second] = match.slice(1).map(x => +(x || 0));
  const wall = Date.UTC(year, month - 1, day, hour, minute, second);
  const check = new Date(wall);
  if (check.getUTCFullYear() !== year || check.getUTCMonth() !== month - 1 || check.getUTCDate() !== day || check.getUTCHours() !== hour || check.getUTCMinutes() !== minute || check.getUTCSeconds() !== second) throw new Error('日期或时间不存在');
  const minutes = (+zone[2] * 60 + +zone[3]) * (zone[1] === '+' ? 1 : -1);
  return validEpoch(wall / 1000 - minutes * 60);
}
export function phoneTime(now = new Date()) {
  const pad = n => String(n).padStart(2, '0');
  const offset = -now.getTimezoneOffset();
  return {
    value: `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}T${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`,
    offset: `${offset < 0 ? '-' : '+'}${pad(Math.floor(Math.abs(offset) / 60))}:${pad(Math.abs(offset) % 60)}`,
  };
}
export function wifiOutcome(status, attempt) {
  if (status.wifi_attempt !== attempt) throw new Error('Wi-Fi 提交编号变化，不能确认本次保存');
  if (status.wifi_saved && status.wifi_state === 'saved') return 'saved';
  if (['failed', 'save_error', 'prepare_error'].includes(status.wifi_state)) return 'failed';
  return 'pending'; // Including SDK Connected: not an application save ACK.
}
export async function requestControl(secure, op, epoch) {
  return controlResponse(await secure.request('clock-control', controlRequest(op, epoch)));
}
