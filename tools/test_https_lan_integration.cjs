/* One joint production Pages + admin.html positive integration fixture.
 * NODE_PATH="$(npm root -g)" node tools/test_https_lan_integration.cjs
 * ALL requests are route-fulfilled/aborted: no real network/device, TLS handshake,
 * certificate trust, BasicAuth, LNA or OTA/SDK image acceptance is tested.
 * Assume trust/login already completed; no httpCredentials or TLS exceptions.
 * Synthetic 3 MiB + 17 B payload passes UI prefix checks ONLY, NOT a flashable image.
 */
'use strict';
const {chromium} = require('@playwright/test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const root = path.join(__dirname, '..');
const publisher = 'https://billxc.github.io';
const parentURL = `${publisher}/epaper-clock/lan/`;
const device = 'https://device.test';
const protocol = 'epaper-admin-v1';
const hex32 = /^[0-9a-f]{32}$/;
const commit = 'b'.repeat(40), oldVersion = 'aaaaaaa';
const app = Buffer.alloc(3 * 1024 * 1024 + 17);
for (let i = 288; i < app.length; i++) app[i] = (i * 31 + (i >>> 8)) & 255;
app[0] = 0xe9;
app.writeUInt16LE(9, 12);
app.writeUInt32LE(0xabcd5432, 32);
app.write(commit.slice(0, 7), 48);
app.write('epaper-clock', 80);
const sha256 = crypto.createHash('sha256').update(app).digest('hex');
const manifest = {schema:1, project:'epaper-clock', chip:9, version:commit.slice(0, 7),
  commit, size:app.length, sha256, url:`app-${commit}-${sha256}.bin`};
const manifestURL = `${publisher}/epaper-clock/firmware/manifest.json`;
const binURL = `${publisher}/epaper-clock/firmware/${manifest.url}`;
const adminSource = fs.readFileSync(path.join(root, 'main/admin.c'), 'utf8');
const policies = [...adminSource.matchAll(/httpd_resp_set_hdr\(r,\s*"Content-Security-Policy",\s*"(default-src[^"\r\n]+)"\)/g)];
assert.equal(policies.length, 1, 'expected one unique production root CSP literal');
const csp = policies[0][1];
const adminHTML = fs.readFileSync(path.join(root, 'main/admin.html'));
const assets = new Map([
  [parentURL, ['text/html', fs.readFileSync(path.join(root, 'docs/lan/index.html'))]],
  [`${parentURL}lan.mjs`, ['text/javascript', fs.readFileSync(path.join(root, 'docs/lan/lan.mjs'))]],
  [`${parentURL}lan.css`, ['text/css', fs.readFileSync(path.join(root, 'docs/lan/lan.css'))]],
]);

async function main() {
  const browser = await chromium.launch({headless:true, chromiumSandbox:true});
  try {
    // No ignoreHTTPSErrors, browser flags, credentials or certificate changes.
    const context = await browser.newContext({serviceWorkers:'block'});
    // A real ambient Pages cookie must still be omitted by public firmware fetches.
    await context.addCookies([{name:'integration_ambient', value:'must-not-reach-firmware', url:publisher}]);
    const requests = [], errors = [], routeErrors = [], uploads = [];
    const status = {appversion:oldVersion, project:'epaper-clock', chip:9, ota_sha256:true,
      boot_id:41, ssid:'PRIVATE-SSID', sta_ip:'192.0.2.10',
      ota:{state:'idle', busy:false, current_slot_bytes:7864320, next_slot_bytes:7864320}};
    let activity = 0;
    await context.addInitScript(({publisher, protocol}) => {
      // Observe genuine messages only; never synthesize hello/ready/install/result.
      window.integration = {messages:[], violations:[]};
      addEventListener('securitypolicyviolation', e => integration.violations.push(`${e.violatedDirective}: ${e.blockedURI}`));
      addEventListener('message', e => {
        const m = e.data;
        if (!m || m.protocol !== protocol) return;
        const expectedSource = location.origin === publisher
          ? document.querySelector('iframe')?.contentWindow : parent;
        integration.messages.push({origin:e.origin, correctSource:e.source === expectedSource,
          protocol:m.protocol, type:m.type, nonce:m.nonce, requestId:m.requestId,
          version:m.version, state:m.state, confirmed:m.confirmed,
          manifest:m.manifest, bufferIsArrayBuffer:m.buffer instanceof ArrayBuffer,
          bufferBytes:m.buffer instanceof ArrayBuffer ? m.buffer.byteLength : null});
      });
      if (location.origin === publisher) {
        const digest = crypto.subtle.digest.bind(crypto.subtle);
        crypto.subtle.digest = (algorithm, bytes) => {
          // Retain the actual hashed buffer to prove transfer detaches it later.
          integration.hashedBuffer = bytes;
          integration.hashedBytes = bytes.byteLength;
          return digest(algorithm, bytes);
        };
      }
    }, {publisher, protocol});
    // Catch-all: no request may fall through to DNS/TLS/the real network.
    await context.route('**/*', async route => {
      try {
        const req = route.request(), url = new URL(req.url());
        const r = {url:url.href, method:req.method(), type:req.resourceType(),
          frame:req.frame().url(), headers:await req.allHeaders()};
        requests.push(r);
        if (assets.has(url.href)) {
          assert.equal(req.method(), 'GET');
          const [contentType, body] = assets.get(url.href);
          return await route.fulfill({contentType, body});
        }
        if (url.href === manifestURL || url.href === binURL) {
          assert.equal(req.method(), 'GET');
          assert.equal(r.frame, parentURL, 'only parent downloads public firmware');
          for (const h of ['authorization', 'cookie', 'referer']) assert.equal(r.headers[h], undefined, `public ${h}`);
          return await route.fulfill({contentType:url.href === manifestURL ? 'application/json' : 'application/octet-stream',
            body:url.href === manifestURL ? JSON.stringify(manifest) : app});
        }
        assert.equal(url.origin, device, 'unexpected origin (network is forbidden)');
        if (url.href === `${device}/`) {
          assert.equal(req.method(), 'GET'); assert.equal(r.type, 'document');
          return await route.fulfill({contentType:'text/html', headers:{'Content-Security-Policy':csp}, body:adminHTML});
        }
        assert.equal(r.frame, `${device}/`, 'Pages must never fetch device API');
        assert.equal(r.headers.origin || device, device, 'device API must be same-origin');
        // Chromium interception may precede generation of Fetch Metadata headers;
        // frame URL + request URL above establish the same-origin boundary.
        if (r.headers['sec-fetch-site'] !== undefined) assert.equal(r.headers['sec-fetch-site'], 'same-origin');
        assert.equal(url.search, '');
        if (url.pathname === '/api/status') {
          assert.equal(req.method(), 'GET'); assert.equal(r.type, 'fetch');
          return await route.fulfill({contentType:'application/json', body:JSON.stringify(status)});
        }
        if (url.pathname === '/api/activity') {
          assert.equal(req.method(), 'POST'); assert.equal(r.type, 'fetch'); activity++;
        } else if (url.pathname === '/api/ota') {
          assert.equal(req.method(), 'POST'); assert.equal(r.type, 'xhr');
          const body = req.postDataBuffer();
          assert.deepEqual(body, app, 'raw XHR upload is byte-exact');
          assert.equal(r.headers['content-type'], 'application/octet-stream');
          assert.equal(r.headers['x-firmware-sha256'], sha256);
          assert.equal(crypto.createHash('sha256').update(body).digest('hex'), sha256);
          uploads.push({bytes:body.length, sha256:r.headers['x-firmware-sha256']});
          status.ota = {...status.ota, state:'rebooting', busy:true};
        } else throw new Error(`Unexpected device path ${url.href}`);
        return await route.fulfill({contentType:'application/json', body:'{"ok":true}'});
      } catch (error) {
        routeErrors.push(error.stack); await route.abort('blockedbyclient');
      }
    });
    const page = await context.newPage();
    page.on('pageerror', e => errors.push(e.message));
    const waitText = async (id, text) => {
      try { await page.locator(`#${id}`).getByText(text, {exact:false}).waitFor({timeout:15000}); }
      catch (error) {
        console.error('Integration failure diagnostics:', {routeErrors, errors,
          connection:await page.locator('#connection').textContent(),
          update:await page.locator('#update').textContent(),
          frames:await Promise.all(page.frames().map(async f => ({url:f.url(),
            observed:await f.evaluate(() => window.integration).catch(() => null)})))});
        throw error;
      }
    };
    await page.goto(parentURL);
    await page.locator('#origin').fill(device);
    await page.locator('#connect').click();
    await waitText('connection', `版本 ${oldVersion}`);
    const child = page.frames().find(f => f.url() === `${device}/`);
    assert(child, 'real admin iframe loaded under production CSP');
    assert.equal(activity, 0); assert.equal(uploads.length, 0);
    assert.equal(requests.filter(r => r.url === manifestURL || r.url === binURL).length, 0);
    const hello = await child.evaluate(() => integration.messages.find(m => m.type === 'hello'));
    assert.equal(hello.origin, publisher); assert(hello.correctSource); assert.match(hello.nonce, hex32);
    console.log('PASS real parent click -> real hello/ready, production root CSP; no automatic download/activity');

    await page.locator('#check').click();
    await waitText('update', '可安装');
    assert.equal(requests.filter(r => r.url === manifestURL).length, 1);
    assert.equal(requests.filter(r => r.url === binURL).length, 0);
    assert.equal(activity, 0); assert.equal(uploads.length, 0);
    console.log('PASS check downloads manifest only; no app or device write');
    let confirmations = 0;
    page.once('dialog', async dialog => {
      confirmations++;
      assert.equal(dialog.type(), 'confirm'); assert.match(dialog.message(), /可靠供电/);
      await dialog.accept();
    });
    await page.locator('#install').click();
    await waitText('update', '这不是启动成功证明');
    assert.equal(confirmations, 1); assert.equal(uploads.length, 1); assert.equal(activity, 1);
    const install = await child.evaluate(() => integration.messages.find(m => m.type === 'install'));
    assert.equal(install.origin, publisher); assert(install.correctSource);
    assert.equal(install.nonce, hello.nonce); assert.match(install.requestId, hex32);
    assert.equal(install.confirmed, true); assert(install.bufferIsArrayBuffer);
    assert.equal(install.bufferBytes, app.length); assert.deepEqual(install.manifest, manifest);
    assert.deepEqual(await page.evaluate(() => ({hashed:integration.hashedBytes, remaining:integration.hashedBuffer.byteLength})),
      {hashed:app.length, remaining:0});
    console.log(`PASS real ArrayBuffer transfer/detachment -> child same-origin raw XHR: ${app.length} bytes equal, SHA256 ${sha256}`);

    // Only after observing the genuine rebooting result, emulate successful boot.
    // No timers/reconnect functions/bridge results are patched or called by the test.
    status.boot_id++;
    status.appversion = manifest.version;
    status.ota = {...status.ota, state:'success', busy:false, expected_version:manifest.version};
    await waitText('update', '设备报告新固件启动成功');
    await waitText('connection', '请手动重新连接');
    assert(await page.locator('#check').isDisabled());
    assert(await page.locator('#install').isDisabled());
    assert.equal(await child.locator('#version').textContent(), manifest.version);
    assert.deepEqual(await child.evaluate(() => integration.violations), []);
    const messages = await page.evaluate(() => integration.messages);
    const results = messages.filter(m => m.type === 'result');
    assert.deepEqual(results.map(m => m.state), ['rebooting', 'success']);
    for (const m of messages) {
      assert.equal(m.origin, device); assert(m.correctSource);
      assert.equal(m.protocol, protocol); assert.equal(m.nonce, hello.nonce);
    }
    for (const m of results) assert.equal(m.requestId, install.requestId);
    const statusReads = requests.filter(r => r.url === `${device}/api/status`);
    assert(statusReads.length >= 4, 'hello/poll, install validation and real reconnect status reads');
    console.log('PASS genuine child reconnect observes changed boot_id/version/success; request-bound result reaches parent');

    // Terminal result consumes old ready.version. Real manual reconnect must read
    // the new version; checking the same release must now reject a reinstall.
    const firmwareCount = requests.filter(r => r.url === manifestURL || r.url === binURL).length;
    await page.locator('#check').evaluate(button => button.click());
    assert.equal(requests.filter(r => r.url === manifestURL || r.url === binURL).length, firmwareCount);
    await page.locator('#connect').click();
    await waitText('connection', `版本 ${manifest.version}`);
    assert(child.isDetached(), 'manual reconnect creates a new iframe');
    const newChild = page.frames().find(f => f.url() === `${device}/`);
    const newHello = await newChild.evaluate(() => integration.messages.find(m => m.type === 'hello'));
    assert.match(newHello.nonce, hex32); assert.notEqual(newHello.nonce, hello.nonce);
    await page.locator('#check').click();
    await waitText('update', '设备已经是此版本');
    assert(await page.locator('#install').isDisabled());
    assert.equal(uploads.length, 1); assert.equal(activity, 1);
    assert.equal(requests.filter(r => r.url === binURL).length, 1);
    assert.equal(requests.filter(r => r.url === manifestURL).length, 2);
    assert.deepEqual(await page.evaluate(() => integration.violations), []);
    assert.deepEqual(await newChild.evaluate(() => integration.violations), []);
    assert.deepEqual(routeErrors, []); assert.deepEqual(errors, []);
    const api = requests.filter(r => new URL(r.url).pathname.startsWith('/api/'));
    assert(api.every(r => new URL(r.url).origin === device && r.frame === `${device}/`));
    const counts = Object.fromEntries(['/api/status', '/api/activity', '/api/ota'].map(p =>
      [p, api.filter(r => new URL(r.url).pathname === p).length]));
    console.log('PASS terminal success invalidates old ready; manual reconnect reads new version and blocks same-version install');
    console.log(`PASS boundaries: public manifest/bin omit Authorization/Cookie/Referer; only child same-origin APIs ${JSON.stringify(counts)}; no route fallthrough/CSP/page errors`);
    console.log('Joint production-JS integration passed. ROUTE MOCK ONLY: NOT real TLS/trust/BasicAuth/LNA/OTA; synthetic app is NOT flashable.');
    await context.close();
  } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
