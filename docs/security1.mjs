import { bytes, blob, sessionRequest, sessionResponse, utf8 } from './protocol.mjs';
import { bleLog, bleFailure } from './ble-log.mjs?v=ble-feedback-1';

export function equal(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}
export async function sharedKey(subtle, privateKey, peerPublic, pop) {
  if (peerPublic.length !== 32 || !pop) throw new Error('需要 32 字节公钥及非空 PoP');
  const peer = await subtle.importKey('raw', peerPublic, { name: 'X25519' }, false, []);
  const shared = new Uint8Array(await subtle.deriveBits({ name: 'X25519', public: peer }, privateKey, 256));
  if (shared.every(v => v === 0)) throw new Error('无效 X25519 共享密钥');
  const digest = new Uint8Array(await subtle.digest('SHA-256', utf8.encode(pop)));
  for (let i = 0; i < 32; i++) shared[i] ^= digest[i];
  digest.fill(0);
  return shared;
}

// IDF/Python use ONE CTR stream for both directions, including handshake proofs.
// WebCrypto is stateless: retain partial-block offset and regenerate its prefix.
export class CtrStream {
  static async create(subtle, rawKey, iv) {
    if (rawKey.length !== 32 || iv.length !== 16) throw new Error('AES-256-CTR 参数长度错误');
    const key = await subtle.importKey('raw', rawKey, 'AES-CTR', false, ['encrypt']);
    return new CtrStream(subtle, key, iv);
  }
  constructor(subtle, key, iv) {
    this.subtle = subtle; this.key = key; this.counter = iv.slice(); this.offset = 0; this.busy = false;
  }
  async update(data) {
    if (!this.key || this.busy) throw new Error('CTR 流已关闭或有并发请求');
    this.busy = true;
    try {
      const input = new Uint8Array(this.offset + data.length);
      input.set(data, this.offset);
      const output = new Uint8Array(await this.subtle.encrypt({ name: 'AES-CTR', counter: this.counter, length: 128 }, this.key, input));
      const result = output.slice(this.offset);
      let carry = Math.floor(input.length / 16);
      for (let i = 15; i >= 0 && carry; i--) {
        carry += this.counter[i]; this.counter[i] = carry & 255; carry = Math.floor(carry / 256);
      }
      if (carry) throw new Error('CTR 计数器耗尽');
      this.offset = input.length % 16;
      return result;
    } finally { this.busy = false; }
  }
  destroy() { this.key = null; this.counter.fill(0); this.offset = 0; }
}

export class Security1 {
  static async establish(exchange, pop, subtle = globalThis.crypto.subtle, onProgress = () => {}) {
    if (!pop || utf8.encode(pop).length > 256) throw new Error('请输入设备显示的 PoP（最多 256 字节）');
    const started = performance.now();
    try {
      onProgress('蓝牙端点已就绪，生成安全握手密钥');
      bleLog('security1.key_exchange.start');
      const pair = await subtle.generateKey({ name: 'X25519' }, false, ['deriveBits']);
      const clientPublic = new Uint8Array(await subtle.exportKey('raw', pair.publicKey));
      onProgress('验证六位码：安全握手 1/2');
      const first = sessionResponse(await exchange('prov-session', sessionRequest(0, bytes(1, clientPublic))), 1);
      bleLog('security1.hello.ok');
      const devicePublic = blob(first, 2, true), random = blob(first, 3, true);
      if (devicePublic.length !== 32 || random.length !== 16) throw new Error('设备 Security1 参数长度错误');
      const rawKey = await sharedKey(subtle, pair.privateKey, devicePublic, pop);
      let stream;
      try { stream = await CtrStream.create(subtle, rawKey, random); }
      finally { rawKey.fill(0); }
      try {
        onProgress('验证六位码：安全握手 2/2');
        const verify = await stream.update(devicePublic);
        const response = sessionResponse(await exchange('prov-session', sessionRequest(2, bytes(2, verify))), 3);
        const proof = blob(response, 3, true);
        if (proof.length !== 32 || !equal(await stream.update(proof), clientPublic)) throw new Error('设备身份验证失败，请核对 PoP');
        bleLog('security1.verified', { elapsed_ms: Math.round(performance.now() - started) });
        onProgress('六位码验证通过，安全会话已建立');
        return new Security1(exchange, stream);
      } catch (error) { stream.destroy(); throw error; }
    } catch (error) {
      bleFailure('security1.failed', error, { elapsed_ms: Math.round(performance.now() - started) });
      throw error;
    }
  }
  constructor(exchange, stream) { this.exchange = exchange; this.stream = stream; this.busy = false; }
  async request(endpoint, plaintext) {
    if (!this.stream || this.busy) throw new Error('安全会话不可用或正在执行请求');
    this.busy = true;
    try {
      bleLog('secure.request', { endpoint, bytes: plaintext.length });
      const encrypted = await this.stream.update(plaintext);
      const response = await this.exchange(endpoint, encrypted);
      const result = await this.stream.update(response);
      bleLog('secure.response', { endpoint, bytes: result.length });
      return result;
    } catch (error) {
      // Never retry a packet: bytes may already have been consumed on the peer.
      this.destroy(); throw error;
    } finally { this.busy = false; }
  }
  destroy() { this.stream?.destroy(); this.stream = null; }
}

export async function cryptoCheck(subtle = globalThis.crypto?.subtle) {
  if (!subtle) throw new Error('此环境没有 WebCrypto');
  const a = await subtle.generateKey({ name: 'X25519' }, false, ['deriveBits']);
  const b = await subtle.generateKey({ name: 'X25519' }, false, ['deriveBits']);
  const pub = new Uint8Array(await subtle.exportKey('raw', b.publicKey));
  const raw = await sharedKey(subtle, a.privateKey, pub, 'capability-check');
  const stream = await CtrStream.create(subtle, raw, new Uint8Array(16)); raw.fill(0);
  try { if ((await stream.update(new Uint8Array(17))).length !== 17) throw new Error('AES-CTR 自检失败'); }
  finally { stream.destroy(); }
}
