import test from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto } from 'node:crypto';
import { blob, bytes, concat, configRequest, configResponse, credentials, decode, number, scanEntries, scanRequest, sessionRequest, sessionResponse, uint, utf8, varint, wifiStatus, wifiDisposition } from './protocol.mjs';
import { CtrStream, Security1, cryptoCheck, sharedKey } from './security1.mjs';

const hex = s => Uint8Array.from(Buffer.from(s, 'hex'));
const h = data => Buffer.from(data).toString('hex');
const subtle = webcrypto.subtle;
const privateScalar = '77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a';
const clientPublic = hex('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
const devicePublic = hex('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f');
const iv = Uint8Array.from({ length: 16 }, (_, i) => i);
const key = hex('65046bcceb1aee9271c9284304291b6722475dad7b75db8a6878676a2b5cab63');
// Independent fixtures: Python cryptography 46.0.5, one AES-CTR encryptor.update
// across BOTH directions, RFC 7748 Alice/Bob scalars, SHA256('test-pop') XOR DH.
const clientProof = hex('2558d1629930574db40a4fa6ce7084cb56339ef662de7997304cdaaa5b224cc9');
const deviceProof = hex('4d0cf142d01a1f42b295f81cd3c85d89812f6c8c455807556307f0d0ae993fe1');
const sequence = [
  [devicePublic, clientProof],
  [clientPublic, deviceProof],
  [hex('5200'), hex('46f1')],
  [hex('08015a021001'), hex('c6d94262779a')],
  [hex('080262100a045465737412083132333435363738'), hex('72c0607529f53b2b3e4c08fe2ee1dc0d745026db')],
  [hex('08036a00'), hex('fc8cfe1d')],
  [hex('08047200'), hex('a1b435eb')],
];
async function fixedCrypto() {
  const privateKey = await subtle.importKey('pkcs8', hex('302e020100300506032b656e04220420' + privateScalar), { name: 'X25519' }, false, ['deriveBits']);
  const publicKey = await subtle.importKey('raw', clientPublic, { name: 'X25519' }, true, []);
  return { privateKey, api: {
    generateKey: async () => ({ privateKey, publicKey }),
    exportKey: subtle.exportKey.bind(subtle), importKey: subtle.importKey.bind(subtle),
    deriveBits: subtle.deriveBits.bind(subtle), digest: subtle.digest.bind(subtle), encrypt: subtle.encrypt.bind(subtle),
  } };
}
const response0 = () => sessionRequest(1, concat(bytes(2, devicePublic), bytes(3, iv)));
const response1 = () => sessionRequest(3, bytes(3, deviceProof));

test('protobuf: exact official command tags and UTF-8 byte lengths', () => {
  assert.equal(h(configRequest(0)), '5200');
  assert.equal(h(configRequest(4)), '08047200');
  assert.equal(h(credentials('Test', '12345678')), '080262100a045465737412083132333435363738');
  const m = decode(blob(decode(credentials('家', '12345678')), 12));
  assert.equal(h(blob(m, 1)), 'e5aeb6');
  assert.throws(() => credentials('家'.repeat(11), '12345678'));
  assert.throws(() => credentials('Test', 'short'));
  assert.throws(() => credentials('Test', 'a'.repeat(64)), /64 位原始 PSK/);
  assert.throws(() => credentials('Test\0', '12345678'));
  assert.equal(h(scanRequest(0, concat(uint(3, 5), uint(4, 120)))), '520418052078');
  assert.equal(h(scanRequest(4, concat(uint(1, 2), uint(2, 1)))), '0804720408021001');
  const cmd0 = sessionRequest(0, bytes(1, clientPublic));
  assert.equal(h(cmd0).slice(0, 16), '10015a25a201220a'); // field 20 has multi-byte tag
});

test('protobuf: defaults, repeated scan entries, signed RSSI and LAN IPv4', () => {
  assert.equal(number(decode(new Uint8Array()), 1), 0);
  const connected = concat(bytes(1, utf8.encode('192.168.1.23')), bytes(3, utf8.encode('Test')));
  const status = concat(uint(1, 1), bytes(11, bytes(11, connected)));
  assert.deepEqual(wifiStatus(status), { state: 0, ip: '192.168.1.23', reason: '' });
  assert.deepEqual(wifiStatus(hex('08015a021001')), { state: 1, ip: '', reason: '' });
  assert.match(wifiStatus(concat(uint(1, 1), bytes(11, uint(2, 3)))).reason, /认证失败/);
  const entry = concat(bytes(1, utf8.encode('Test')), uint(2, 6), uint(3, -42), uint(5, 3));
  const result = concat(uint(1, 5), bytes(15, concat(bytes(1, entry), bytes(1, entry))));
  assert.deepEqual(scanEntries(result), Array(2).fill({ ssid: 'Test', channel: 6, rssi: -42, auth: 3 }));
  assert.equal(h(varint(-42)), 'd6ffffffffffffffff01');
});

test('Wi-Fi Disconnected/IDLE and failure terminate polling and allow user retry', () => {
  assert.equal(wifiDisposition(wifiStatus(hex('08015a021001'))), 'connecting');
  assert.equal(wifiDisposition(wifiStatus(hex('08015a021002'))), 'retry');
  assert.equal(wifiDisposition(wifiStatus(hex('08015a021003'))), 'retry');
  assert.equal(wifiDisposition({ state: 0 }), 'connected');
  assert.throws(() => wifiDisposition({ state: 4 }));
});

test('protobuf: reject malformed, conflicting, duplicate, wrong response and errors', () => {
  for (const raw of ['80', '0a05ff', '00', '0b', '0880808080808080808080', 'ffffffffffffffffff02']) assert.throws(() => decode(hex(raw)));
  assert.throws(() => number(decode(hex('08000801')), 1));
  assert.throws(() => configResponse(hex('08036a020801'), 3));
  assert.throws(() => configResponse(hex('08015a00'), 3));
  assert.throws(() => configResponse(hex('08036a007a00'), 3));
  assert.throws(() => configResponse(hex('0803'), 3));
  assert.throws(() => wifiStatus(hex('08015a06100350005a00')), /oneof/);
  assert.throws(() => sessionResponse(hex('10005200'), 1), /降级/);
  assert.throws(() => sessionResponse(sessionRequest(1, uint(1, 2)), 1));
});

test('WebCrypto capability check executes X25519, SHA-256 and AES-CTR', async () => { await cryptoCheck(subtle); });

test('Security1: fixed RFC 7748 key exchange + PoP XOR SHA256', async () => {
  const { privateKey } = await fixedCrypto();
  assert.equal(h(await sharedKey(subtle, privateKey, devicePublic, 'test-pop')), h(key));
  await assert.rejects(sharedKey(subtle, privateKey, new Uint8Array(32), 'test-pop'));
  await assert.rejects(sharedKey(subtle, privateKey, devicePublic, ''));
});

test('Security1: one CTR stream, Python fixtures across 2/6/20/4-byte packet boundaries', async () => {
  const stream = await CtrStream.create(subtle, key, iv);
  for (const [plain, cipher] of sequence) assert.equal(h(await stream.update(plain)), h(cipher));
  stream.destroy(); await assert.rejects(stream.update(hex('00')));
  const decrypting = await CtrStream.create(subtle, key, iv);
  for (const [plain, cipher] of sequence) assert.equal(h(await decrypting.update(cipher)), h(plain));
  decrypting.destroy();
});

test('Security1: arbitrary chunking including 1-byte boundaries equals one update', async () => {
  const input = Uint8Array.from({ length: 513 }, (_, i) => i & 255);
  const once = await CtrStream.create(subtle, key, iv), split = await CtrStream.create(subtle, key, iv);
  const expected = await once.update(input), pieces = [];
  for (let offset = 0, chunk = 0; offset < input.length; chunk++) {
    const length = [1, 2, 15, 16, 17, 31][chunk % 6];
    pieces.push(await split.update(input.slice(offset, offset + length))); offset += length;
  }
  assert.equal(h(concat(...pieces)), h(expected));
  once.destroy(); split.destroy();
});

test('Security1: full fixed handshake and alternating encrypted config requests', async () => {
  const { api } = await fixedCrypto(); let step = 0;
  const secure = await Security1.establish(async (endpoint, data) => {
    step++;
    if (step === 1) { assert.equal(endpoint, 'prov-session'); assert.equal(h(data), h(sessionRequest(0, bytes(1, clientPublic)))); return response0(); }
    if (step === 2) { assert.equal(h(data), h(sessionRequest(2, bytes(2, clientProof)))); return response1(); }
    assert.equal(endpoint, 'prov-config');
    if (step === 3) { assert.equal(h(data), '46f1'); return hex('c6d94262779a'); }
    if (step === 4) { assert.equal(h(data), h(sequence[4][1])); return hex('fc8cfe1d'); }
    throw new Error('unexpected exchange');
  }, 'test-pop', api);
  assert.equal(wifiStatus(await secure.request('prov-config', configRequest(0))).state, 1);
  configResponse(await secure.request('prov-config', credentials('Test', '12345678')), 3);
  assert.equal(step, 4); secure.destroy();
});

test('Security1: bad PoP and Security0 fail closed; transport error destroys stream', async () => {
  const { api } = await fixedCrypto(); let step = 0;
  await assert.rejects(Security1.establish(async () => ++step === 1 ? response0() : response1(), 'wrong-pop', api), /验证失败/);
  await assert.rejects(Security1.establish(async () => hex('10005200'), 'test-pop', api), /降级/);
  const stream = await CtrStream.create(subtle, key, iv);
  const secure = new Security1(async () => { throw new Error('lost packet'); }, stream);
  await assert.rejects(secure.request('prov-config', configRequest(0)), /lost packet/);
  await assert.rejects(secure.request('prov-config', configRequest(0)), /不可用/);
});
