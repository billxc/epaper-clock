// Independent of BLE. No device fetch, storage, credentials or discovery here.
export const PROTOCOL = 'epaper-admin-v1';
const HEX32 = /^[0-9a-f]{32}$/;
const MAX_APP = 16 * 1024 * 1024;
const HELP = '请在新窗口直接访问设备核对证书 / BasicAuth 登录；也可能是 CSP、LNA 权限或网络差异。一次证书例外不保证 iframe 可用。';

export function parseOrigin(value) {
  const text = value.trim();
  // Check the original syntax too: URL() otherwise normalizes paths such as /a/.. .
  if (!/^https:\/\/[^/?#@\\\s]+\/?$/i.test(text)) throw new Error('仅允许 HTTPS 根 origin，不得包含路径、查询或 fragment。');
  const url = new URL(text);
  if (url.protocol !== 'https:' || url.username || url.password || !url.hostname || url.pathname !== '/' || url.search || url.hash)
    throw new Error('地址不可含用户名密码，且必须是 HTTPS 根 origin。');
  return url.origin;
}

export function validateManifest(m) {
  if (!m || m.schema !== 1 || m.project !== 'epaper-clock' || m.chip !== 9 ||
      typeof m.commit !== 'string' || typeof m.sha256 !== 'string' ||
      !/^[0-9a-f]{40}$/.test(m.commit) || m.version !== m.commit.slice(0, 7) ||
      !Number.isSafeInteger(m.size) || m.size < 288 || m.size > MAX_APP ||
      !/^[0-9a-f]{64}$/.test(m.sha256) || m.url !== `app-${m.commit}-${m.sha256}.bin`)
    throw new Error('Pages manifest 不符合 schema 1 / epaper-clock / ESP32-S3 或不可变文件规则。');
  // Forward only the public production schema, never arbitrary extra fields.
  return Object.fromEntries(['schema','version','commit','size','sha256','project','chip','url'].map(key => [key, m[key]]));
}

function nonce() {
  return Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');
}
function validReady(m) {
  return m.project === 'epaper-clock' && m.chip === 9 && typeof m.version === 'string' &&
    /^[!-~]{1,31}$/.test(m.version) && Number.isSafeInteger(m.next_slot_bytes) && m.next_slot_bytes > 0 &&
    Array.isArray(m.capabilities) && ['sha256','transfer-install'].every(c => m.capabilities.includes(c));
}

export function start(doc = document) {
  const el = Object.fromEntries(['origin','open','connect','disconnect','connection','check','install','update','frame-host'].map(id => [id, doc.getElementById(id)]));
  const firmwareBase = new URL('../firmware/', doc.baseURI);
  if (firmwareBase.origin !== location.origin) throw new Error('固件必须同源。');
  let session = null;
  function controls() {
    el.check.disabled = !session?.ready || session.busy || !!session.pending;
    el.install.disabled = el.check.disabled || !session.manifest;
    for (const id of ['origin','open','connect','disconnect']) el[id].disabled = !!session?.pending;
  }
  function clearSession(remove = true) {
    if (session) {
      clearTimeout(session.deadline); clearInterval(session.helloTimer); clearTimeout(session.installTimer);
      session.abort.abort(); session = null;
    }
    if (remove) el['frame-host'].replaceChildren();
    controls();
  }
  function current(s) { return session === s && !s.abort.signal.aborted; }
  function send(s, type, extra = {}, transfer = []) {
    if (current(s)) s.frame.contentWindow.postMessage({protocol:PROTOCOL,type,nonce:s.nonce,...extra}, s.origin, transfer);
  }
  function target() {
    try { return parseOrigin(el.origin.value); }
    catch (error) { el.connection.textContent = error.message; return null; }
  }
  function failConnection(s, message) {
    if (!current(s)) return;
    clearSession(false);
    el.connection.textContent = `${message} ${HELP}`;
  }
  el.origin.addEventListener('input', () => {
    if (session?.pending) return;
    clearSession();
    el.connection.textContent = '地址已修改，旧会话已失效；请先在新窗口核对设备，再连接 iframe。';
    el.update.textContent = '尚未检查更新。';
  });
  el.open.addEventListener('click', event => {
    if (!event.isTrusted || session?.pending) return;
    const origin = target();
    if (!origin) return;
    window.open(`${origin}/`, '_blank', 'noopener,noreferrer');
    if (!session?.ready) el.connection.textContent = '已请求在新窗口打开设备；请在那里核对证书并登录，再回来连接 iframe。这不代表已连接。';
  });
  el.disconnect.addEventListener('click', event => {
    if (!event.isTrusted || session?.pending) return;
    clearSession(); el.origin.value = '';
    el.connection.textContent = '已断开并清空地址；已交给设备的安装不会被撤回。';
    el.update.textContent = '尚未检查更新。';
  });
  function connectTarget(origin) {
    clearSession();
    if (!window.isSecureContext || !crypto.subtle) {
      el.connection.textContent = '需要安全上下文及 WebCrypto SHA-256；不降级到未校验安装。'; return;
    }
    const frame = doc.createElement('iframe');
    frame.title = '用户指定的设备 HTTPS 管理页'; frame.referrerPolicy = 'no-referrer';
    const s = {origin, frame, nonce:nonce(), abort:new AbortController(), ready:null, manifest:null, busy:false, pending:null};
    session = s;
    el.connection.textContent = '阶段：加载 iframe。尚未连接；加载后仍需等待设备 ready。';
    el.update.textContent = '连接后请手动检查更新；不会自动安装。';
    function waitForReady() {
      clearTimeout(s.deadline);
      s.deadline = setTimeout(() => failConnection(s, '等待设备 ready 超时，未连接。'), 20000);
    }
    frame.addEventListener('load', () => {
      if (!current(s)) return;
      // Device binds its first hello. Never rebind a live iframe to a new nonce:
      // stop on subsequent document loads. Only a manual reconnect creates a new iframe.
      if (s.loaded) {
        el.update.textContent = s.pending ? 'iframe 已重新加载，安装结果未知；请直接访问设备核对，不会自动重试。' : '旧会话已失效；不会自动重连或安装。';
        failConnection(s, 'iframe 文档已重新加载；请手动点击连接iframe，新建独立会话。');
        return;
      }
      s.loaded = true;
      el.connection.textContent = '阶段：等待设备 ready。iframe 已触发加载事件，但尚未连接。';
      controls(); waitForReady();
      send(s, 'hello');
      s.helloTimer = setInterval(() => send(s, 'hello'), 1500);
    });
    waitForReady(); frame.src = `${origin}/`; el['frame-host'].replaceChildren(frame); controls();
  }
  el.connect.addEventListener('click', event => {
    if (!event.isTrusted || session?.pending) return;
    const origin = target();
    if (origin) connectTarget(origin);
  });
  window.addEventListener('message', event => {
    const s = session, m = event.data;
    if (!s || event.source !== s.frame.contentWindow || event.origin !== s.origin ||
        !m || typeof m !== 'object' || m.protocol !== PROTOCOL || m.nonce !== s.nonce) return;
    if (m.type === 'ready') {
      if (s.ready || !validReady(m)) return;
      s.ready = {version:m.version, next_slot_bytes:m.next_slot_bytes};
      clearInterval(s.helloTimer); clearTimeout(s.deadline);
      el.connection.textContent = `已连接：设备已返回 ready，版本 ${m.version}。`;
      controls();
    } else if (m.type === 'result') {
      if (!s.pending || m.requestId !== s.pending || !HEX32.test(m.requestId)) return;
      const messages = {
        rebooting:'阶段：等待重启确认。设备已接受上传，正在重启；这不是启动成功证明，等待设备确认。',
        success:'设备报告新固件启动成功。', rollback:'设备报告已回滚；未确认新固件成功。',
        unconfirmed:'设备无法确认启动结果；请在新窗口直接访问核对。'
      };
      if (typeof m.state !== 'string' || !Object.hasOwn(messages, m.state)) return;
      el.update.textContent = messages[m.state];
      if (m.state !== 'rebooting') {
        clearSession(false);
        el.connection.textContent = '本次安装已结束，请手动重新连接，读取设备当前版本后再检查更新。';
      }
    } else if (m.type === 'error') {
      if (m.requestId !== undefined && m.requestId !== s.pending) return;
      if (s.pending && m.requestId !== s.pending) return;
      // Render a fixed explanation, not raw device text or status blobs.
      const codes = {status:'设备状态不可用',busy:'设备正忙',replay:'设备拒绝重复请求',manifest:'设备拒绝固件元数据',image:'设备拒绝镜像',upload:'设备上传失败',unconfirmed:'设备无法确认安装结果'};
      if (typeof m.code !== 'string' || !Object.hasOwn(codes, m.code)) return;
      el.update.textContent = `${codes[m.code]}；请直接访问设备核对。不会自动重试。`;
      clearTimeout(s.installTimer); s.pending = null; s.manifest = null; controls();
    }
    // No status/RPC extension is defined in v1; unknown types are intentionally ignored.
  });
  async function fetchPublic(url, signal) {
    if (url.origin !== location.origin || !url.href.startsWith(firmwareBase.href)) throw new Error('拒绝非同源固件地址。');
    const response = await fetch(url, {signal,credentials:'omit',referrerPolicy:'no-referrer',redirect:'error',cache:'no-store'});
    if (!response.ok) throw new Error(`Pages 下载失败 (${response.status})。`);
    return response;
  }
  async function readLimited(response, maxBytes) {
    // Bound memory even if the server ignores Content-Length or returns a larger file.
    const reader = response.body.getReader(), chunks = [];
    let size = 0;
    try {
      while (true) {
        const {done, value} = await reader.read();
        if (done) break;
        size += value.byteLength;
        if (size > maxBytes) { await reader.cancel(); throw new Error('Pages 响应超过声明大小或安全上限。'); }
        chunks.push(value);
      }
    } finally { reader.releaseLock(); }
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    return bytes;
  }
  function compatible(s, m) {
    if (m.version === s.ready.version) throw new Error('设备已经是此版本，无需重复安装。');
    if (m.size > s.ready.next_slot_bytes) throw new Error('固件超过设备下一 OTA 槽容量。');
  }
  el.check.addEventListener('click', async event => {
    if (!event.isTrusted) return;
    const s = session;
    if (!s?.ready || s.busy || s.pending) return;
    s.busy = true; s.manifest = null; controls(); el.update.textContent = '阶段：下载本站 Pages manifest（不下载 app、不写设备）…';
    const signal = s.abort.signal;
    try {
      const response = await fetchPublic(new URL('manifest.json', firmwareBase), signal);
      const text = new TextDecoder('utf-8', {fatal:true}).decode(await readLimited(response, 16384));
      const m = validateManifest(JSON.parse(text));
      if (!current(s) || signal.aborted) return;
      if (m.version === s.ready.version) {
        el.update.textContent = `设备已经是此版本 ${m.version}，无需重复安装。`;
        return;
      }
      compatible(s, m); s.manifest = m;
      el.update.textContent = `可安装 ${m.version}，${m.size} 字节，source commit ${m.commit}。尚未下载或安装。`;
    } catch (error) {
      if (current(s) && !signal.aborted) el.update.textContent = `检查失败：${error.message}`;
    } finally { if (current(s) && !signal.aborted) { s.busy = false; controls(); } }
  });
  el.install.addEventListener('click', async event => {
    if (!event.isTrusted) return;
    const s = session;
    if (!s?.ready || !s.manifest || s.busy || s.pending) return;
    const m = s.manifest;
    if (!window.confirm(`安装 ${m.version} 到当前所选设备？\n请使用可靠供电，安装及重启期间勿关机、拔电。\n上传接受不代表启动成功，结果未知时请先直接访问设备核对。`)) return;
    if (!current(s)) return;
    s.busy = true; controls(); el.update.textContent = '阶段：下载本站不可变 app bin；尚未传递安装…';
    const signal = s.abort.signal;
    try {
      compatible(s, m);
      const response = await fetchPublic(new URL(m.url, firmwareBase), signal);
      const buffer = (await readLimited(response, m.size)).buffer;
      if (buffer.byteLength !== m.size) throw new Error('app 大小不匹配。');
      if (!current(s) || signal.aborted) return;
      el.update.textContent = '阶段：校验完整 bin 大小 / SHA-256；尚未传递安装…';
      const digest = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', buffer)), b => b.toString(16).padStart(2, '0')).join('');
      if (digest !== m.sha256) throw new Error('app SHA-256 不匹配。');
      if (!current(s) || signal.aborted) return;
      s.pending = nonce(); controls();
      send(s, 'install', {requestId:s.pending, confirmed:true, manifest:m, buffer}, [buffer]);
      el.update.textContent = '阶段：传递安装。已将校验后的 app 转交设备 iframe；等待设备安装结果，不自动重试。';
      s.installTimer = setTimeout(() => {
        if (current(s) && s.pending) {
          el.update.textContent = '等待安装结果超时，结果未知。请直接访问设备核对；重新连接前不能再次安装。';
          // Keep pending consumed/locked, but reject any late result after this timeout.
          failConnection(s, '安装结果未知，会话已失效。');
        }
      }, 180000);
    } catch (error) {
      if (current(s) && !signal.aborted) { s.pending = null; s.manifest = null; el.update.textContent = `安装未完成：${error.message} 不会自动重试。`; }
    } finally { if (current(s) && !signal.aborted) { s.busy = false; controls(); } }
  });
  window.addEventListener('beforeunload', event => {
    if (session?.pending) { event.preventDefault(); event.returnValue = '安装及重启中，请保持可靠供电，不要关闭页面。'; }
  });
  window.addEventListener('pagehide', () => { clearSession(); el.origin.value = ''; });
  controls();
}

if (typeof document !== 'undefined') start();
