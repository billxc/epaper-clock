import test from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto } from 'node:crypto';
import { BleTransport, DEFAULT_SERVICE } from './ble.mjs';
import { CtrStream, Security1 } from './security1.mjs';
import { controlRequest, controlResponse, localDateEpoch, MIN_EPOCH, MAX_EPOCH, phoneTime, requestControl, validatePop, wifiOutcome } from './clock-control.mjs';

const utf8 = new TextEncoder(), text = new TextDecoder();
const packet = object => utf8.encode(JSON.stringify(object));
const status = (patch = {}) => ({
  ok: true, wifi_configured: false, wifi_saved: false, wifi_state: 'idle',
  wifi_ready: false, wifi_attempt: 0, rtc_valid: false, rtc_time: '', epoch: null,
  time_set: false, ...patch,
});

test('PoP is exactly six ASCII digits, including leading zeros (never numeric)', () => {
  assert.equal(validatePop('000123'), '000123');
  assert.equal(validatePop('000000'), '000000');
  for (const value of [123, '123', '1234567', ' 000123', '１２３４５６', '12345x', '123456\n', '123\0xx']) assert.throws(() => validatePop(value));
});

test('clock-control JSON request shapes and integer UTC epoch envelope', () => {
  assert.equal(text.decode(controlRequest('status')), '{"op":"status"}');
  assert.equal(text.decode(controlRequest('prepare_wifi')), '{"op":"prepare_wifi"}');
  for (const epoch of [MIN_EPOCH, 2147483648, MAX_EPOCH]) assert.deepEqual(JSON.parse(text.decode(controlRequest('set_time', epoch))), { op: 'set_time', epoch });
  for (const epoch of [MIN_EPOCH - 1, MAX_EPOCH + 1, 1.5, NaN, Infinity, '1700000000', null, undefined]) assert.throws(() => controlRequest('set_time', epoch));
  assert.throws(() => controlRequest('status', MIN_EPOCH));
  assert.throws(() => controlRequest('reset_wifi'));
});

test('explicit UTC offset conversion is browser-TZ independent and leap-day strict', () => {
  const utc = localDateEpoch('2026-07-01T12:34:56', '+08:00');
  assert.equal(utc, Date.UTC(2026, 6, 1, 4, 34, 56) / 1000);
  assert.equal(localDateEpoch('2024-02-29T00:00', '-05:30'), Date.UTC(2024, 1, 29, 5, 30) / 1000);
  assert.equal(localDateEpoch('2000-01-01T00:00:00', '+00:00'), MIN_EPOCH);
  assert.equal(localDateEpoch('2099-12-31T23:59:59', '-00:00'), MAX_EPOCH);
  assert.equal(localDateEpoch('2026-01-01T14:00:00', '+14:00'), Date.UTC(2026, 0, 1) / 1000);
  for (const date of ['2023-02-29T00:00', '2024-02-30T00:00', '2026-04-31T00:00', '2026-00-01T00:00', '2026-13-01T00:00', '2026-01-00T00:00', '2026-01-01T24:00', '2026-01-01T00:60', '2026-01-01T00:00:60', '1999-12-31T23:59', '2100-01-01T00:00', '2026-01-01T00:00Z']) assert.throws(() => localDateEpoch(date, '+00:00'));
  for (const zone of ['+14:01', '-15:00', '+08:60', '8', 'Asia/Shanghai', 'Z', '+8:00']) assert.throws(() => localDateEpoch('2026-01-01T00:00', zone));
  assert.throws(() => localDateEpoch('2000-01-01T00:00', '+08:00')); // UTC before envelope
  assert.throws(() => localDateEpoch('2099-12-31T23:59:59', '-08:00'));
});

test('phone-time fills wall time and explicit offset without sending or losing seconds', () => {
  const now = new Date('2026-07-01T04:05:06.789Z');
  const filled = phoneTime(now);
  assert.equal(localDateEpoch(filled.value, filled.offset), Math.floor(now.getTime() / 1000));
});

test('clock-control response types, no saved password, invalid RTC and error handling', () => {
  assert.deepEqual(controlResponse(packet(status())), status());
  const valid = status({ rtc_valid: true, rtc_time: '2026-07-01 12:34:56', epoch: 1782880496, time_set: true });
  assert.deepEqual(controlResponse(packet(valid)), valid);
  assert.throws(() => controlResponse(packet({ ok: false, error: 'epoch_range' })), /epoch_range/);
  for (const raw of ['', '{', 'null', '[]', '{"ok":true}', '{"ok":0}', '{"ok":false}']) assert.throws(() => controlResponse(utf8.encode(raw)));
  for (const patch of [{ password: 'not-allowed' }, { wifi_saved: true }, { wifi_ready: true }, { wifi_attempt: -1 }, { wifi_attempt: 1.5 }, { wifi_state: 'Connected' }, { rtc_valid: true }, { epoch: 123 }, { time_set: 1 }, { rtc_time: 'invalid' }]) assert.throws(() => controlResponse(packet(status(patch))));
  assert.throws(() => controlResponse(new Uint8Array(513)));
  assert.throws(() => controlResponse(Uint8Array.of(255)));
});

test('save ACK belongs to current prepared attempt; Connected and old configuration are not saved', () => {
  const ready = status({ wifi_state: 'ready', wifi_ready: true, wifi_attempt: 2, wifi_configured: true });
  assert.equal(wifiOutcome(controlResponse(packet(ready)), 2), 'pending');
  const connected = status({ wifi_state: 'verifying', wifi_attempt: 2, wifi_configured: true });
  assert.equal(wifiOutcome(controlResponse(packet(connected)), 2), 'pending');
  assert.equal(wifiOutcome({ ...connected, wifi_state: 'Connected' }, 2), 'pending');
  const saved = status({ wifi_state: 'saved', wifi_saved: true, wifi_attempt: 2, wifi_configured: true });
  assert.equal(wifiOutcome(controlResponse(packet(saved)), 2), 'saved');
  assert.throws(() => wifiOutcome(saved, 3), /编号/);
  for (const wifi_state of ['failed', 'save_error', 'prepare_error']) assert.equal(wifiOutcome(controlResponse(packet(status({ wifi_state, wifi_attempt: 2, wifi_configured: true }))), 2), 'failed');
});

test('custom endpoint JSON is encrypted in the SAME Security1 CTR stream as prov-config', async () => {
  const key = Uint8Array.from({ length: 32 }, (_, i) => i + 1), iv = new Uint8Array(16);
  const client = await CtrStream.create(webcrypto.subtle, key, iv);
  const device = await CtrStream.create(webcrypto.subtle, key, iv);
  let step = 0;
  const secure = new Security1(async (endpoint, encrypted) => {
    const plain = await device.update(encrypted);
    assert.notDeepEqual(encrypted, plain);
    step++;
    if (step === 1) {
      assert.equal(endpoint, 'prov-config');
      assert.deepEqual([...plain], [0x52, 0]);
      return device.update(Uint8Array.of(8, 1));
    }
    assert.equal(endpoint, 'clock-control');
    const command = JSON.parse(text.decode(plain));
    assert.deepEqual(command, step === 2 ? { op: 'status' } : { op: 'set_time', epoch: MIN_EPOCH });
    return device.update(packet(status(step === 3 ? { rtc_valid: true, rtc_time: '2000-01-01 00:00:00', epoch: MIN_EPOCH, time_set: true } : {})));
  }, client);
  try {
    assert.deepEqual([...await secure.request('prov-config', Uint8Array.of(0x52, 0))], [8, 1]);
    assert.equal((await requestControl(secure, 'status')).wifi_saved, false);
    assert.equal((await requestControl(secure, 'set_time', MIN_EPOCH)).time_set, true);
    assert.equal(step, 3);
  } finally { secure.destroy(); device.destroy(); }
});

test('0x2901 discovers arbitrary clock-control UUID; missing custom endpoint fails closed', async () => {
  const characteristic = name => ({
    uuid: `arbitrary-${name}`, properties: { read: true, write: true },
    async getDescriptors() { return [{ uuid: '00002901-0000-1000-8000-00805f9b34fb', async readValue() { return new DataView(utf8.encode(name).buffer); } }]; },
    async writeValueWithResponse() {},
  });
  const make = names => {
    const device = { addEventListener() {}, removeEventListener() {}, gatt: {
      connected: true, disconnect() { this.connected = false; },
      async connect() { return { async getPrimaryService() { return { async getCharacteristics() { return names.map(characteristic); } }; } }; },
    } };
    return new BleTransport(device, () => {});
  };
  const good = make(['prov-session', 'clock-control', 'prov-config']);
  await good.open(DEFAULT_SERVICE);
  assert.equal(good.requireEndpoint('clock-control').uuid, 'arbitrary-clock-control'); good.close();
  const old = make(['prov-session', 'prov-config']);
  await old.open(DEFAULT_SERVICE);
  assert.throws(() => old.requireEndpoint('clock-control'), /不支持旧固件或明文降级/); old.close();
});
