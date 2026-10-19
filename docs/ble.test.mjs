import test from 'node:test';
import assert from 'node:assert/strict';
import { BleTransport, DEFAULT_SERVICE } from './ble.mjs';
const view = bytes => new DataView(Uint8Array.from(bytes).buffer);
function mockDevice(characteristics) {
  let connected = false;
  const gatt = {
    get connected() { return connected; },
    async connect() { connected = true; return { async getPrimaryService(uuid) {
      assert.equal(uuid, DEFAULT_SERVICE); return { async getCharacteristics() { return characteristics; } };
    } }; },
    disconnect() { connected = false; },
  };
  return { gatt, addEventListener() {}, removeEventListener() {} };
}
function characteristic(name, uuid, log = []) {
  return {
    uuid, properties: { read: true, write: true },
    async getDescriptors() { return name ? [{ uuid: '00002901-0000-1000-8000-00805f9b34fb', async readValue() { return view(new TextEncoder().encode(name)); } }] : []; },
    async writeValueWithResponse(data) { log.push(['write', ...data]); },
    async readValue() { log.push(['read']); return view([7, 8]); },
  };
}
test('BLE maps arbitrary characteristic UUIDs by 0x2901 names and writes then reads', async () => {
  const log = [], session = characteristic('prov-session', 'unrelated-uuid-1'), config = characteristic('prov-config', 'unrelated-uuid-2', log);
  const transport = new BleTransport(mockDevice([config, session]), () => {});
  await transport.open(DEFAULT_SERVICE);
  assert.equal(transport.endpoints.get('prov-config'), config);
  assert.deepEqual([...await transport.exchange('prov-config', Uint8Array.of(1, 2))], [7, 8]);
  assert.deepEqual(log, [['write', 1, 2], ['read']]); transport.close();
});
test('BLE rejects missing descriptors even when UUID looks like IDF endpoint', async () => {
  const c = characteristic(null, '1775ff51-6b43-439b-877c-060f2d9bed07');
  const transport = new BleTransport(mockDevice([c]), () => {});
  await assert.rejects(transport.open(DEFAULT_SERVICE), /0x2901/);
  assert.equal(transport.closed, true);
});
test('BLE write failure closes session, does not retry or read', async () => {
  let writes = 0, reads = 0;
  const c = characteristic('prov-config', 'random');
  c.writeValueWithResponse = async () => { writes++; throw new Error('write failed'); };
  c.readValue = async () => { reads++; return view([1]); };
  const transport = new BleTransport(mockDevice([c, characteristic('prov-session', 'other')]), () => {});
  await transport.open(DEFAULT_SERVICE);
  await assert.rejects(transport.exchange('prov-config', Uint8Array.of(1)), /write failed/);
  assert.equal(writes, 1); assert.equal(reads, 0); assert.equal(transport.closed, true);
  await assert.rejects(transport.exchange('prov-config', Uint8Array.of(1)), /BLE/);
});
