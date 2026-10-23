/* HTTP non-secure-context browser regression; mock LAN HTTP server + Pages.
 * No hardware, firmware activation, or external credentials.
 * NODE_PATH="$(npm root -g)" node tools/test_admin_update.cjs
 */
const {chromium} = require('@playwright/test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const crypto = require('node:crypto');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../main/admin.html'));
const adminSource = fs.readFileSync(path.join(__dirname, '../main/admin.c'), 'utf8');
const rootPolicies = [...adminSource.matchAll(/httpd_resp_set_hdr\(r,\s*"Content-Security-Policy",\s*"(default-src[^"\r\n]+)"\)/g)];
assert.equal(rootPolicies.length, 1, 'expected exactly one production root CSP literal');
const productionCSP = rootPolicies[0][1];
const legacyCSP = productionCSP.replace(/connect-src[^;]+;/, "connect-src 'self';");
assert.notEqual(legacyCSP, productionCSP, 'production CSP must allow fixed Pages fetch');
const app = Buffer.alloc(336);
app[0] = 0xe9; app.writeUInt16LE(9, 12); app.writeUInt32LE(0xabcd5432, 32);
app.write('bbbbbbb', 48); app.write('epaper-clock', 80);
const commit = 'b'.repeat(40), sha256 = crypto.createHash('sha256').update(app).digest('hex');
const originalManifest = {schema:1,version:'bbbbbbb',commit,size:app.length,sha256,project:'epaper-clock',chip:9,
  url:`app-${commit}-${sha256}.bin`};
const auth = `Basic ${Buffer.from('admin:test-only-password').toString('base64')}`;
let scenario;
function reset() {
  scenario = {manifest:{...originalManifest}, fetchFail:false, binFail:false, otaFail:false, posted:[], activity:0, downloads:0,
    data:{appversion:'aaaaaaa',project:'epaper-clock',chip:9,ota_sha256:true,boot_id:1,
      ota:{state:'idle',busy:false,next_slot_bytes:7864320,current_slot_bytes:7864320},
      render_mode:'mono',power:{sleep_mode:'pmu',pwr_on_ms:1000}}};
}
const server = http.createServer(async (req, res) => {
  if (req.headers.authorization !== auth) {res.writeHead(401, {'WWW-Authenticate':'Basic realm="test"'}); res.end(); return;}
  let body = Buffer.alloc(0);
  for await (const chunk of req) body = Buffer.concat([body, chunk]);
  if (req.url === '/') {
    res.setHeader('Content-Type','text/html');
    res.setHeader('Content-Security-Policy',scenario.csp);
    res.end(html);return;
  }
  res.setHeader('Content-Type','application/json');
  if (req.url === '/api/status') {
    if (scenario.statusFail) {res.writeHead(503);res.end('{"error":"session unavailable"}');return;}
    res.end(JSON.stringify(scenario.data));return;
  }
  if (req.url === '/api/activity') {scenario.activity++;res.end('{"ok":true}');return;}
  if (req.url === '/api/ota') {
    scenario.posted.push({headers:req.headers, body});
    if (scenario.otaFail) {res.writeHead(400);res.end('{"error":"SHA256 mismatch; boot unchanged"}');return;}
    scenario.data.boot_id++;
    scenario.data.appversion = 'bbbbbbb';
    scenario.data.ota = {...scenario.data.ota, state:'success',expected_version:'bbbbbbb'};
    res.end('{"ok":true}');return;
  }
  res.end('{"ok":true}');
});
(async () => {
  await new Promise(resolve=>server.listen(0, '127.0.0.1', resolve));
  const origin = `http://lan-clock.test:${server.address().port}`;
  const browser = await chromium.launch({headless:true, chromiumSandbox:true, args:['--host-resolver-rules=MAP lan-clock.test 127.0.0.1','--no-proxy-server']});
  let total = 0;
  async function setup(csp = productionCSP) {
    reset();
    scenario.csp = csp;
    scenario.publicRequests = 0;
    const context = await browser.newContext({httpCredentials:{username:'admin',password:'test-only-password',origin}});
    const page = await context.newPage();
    page.on('pageerror', error => { throw error; });
    page.on('dialog', dialog => dialog.accept());
    await page.addInitScript(() => {
      window.policyViolations = [];
      document.addEventListener('securitypolicyviolation', event => {
        policyViolations.push({directive:event.effectiveDirective, blockedURI:event.blockedURI});
      });
    });
    await page.route('https://billxc.github.io/**', async route => {
      scenario.publicRequests++;
      const request = route.request(), headers = await request.allHeaders();
      assert.equal(headers.authorization, undefined, 'LAN Basic credential leaked to Pages');
      assert.equal(headers.cookie, undefined);
      assert.equal(headers.referer, undefined);
      assert.equal(request.method(), 'GET');
      const manifestRequest = request.url().endsWith('/manifest.json');
      if (!manifestRequest) {scenario.downloads++; assert.equal(request.url(), new URL(originalManifest.url, 'https://billxc.github.io/epaper-clock/firmware/').href);}
      if ((manifestRequest && scenario.fetchFail) || (!manifestRequest && scenario.binFail)) return route.abort('failed');
      if (scenario.redirect) return route.fulfill({status:302,headers:{location:'https://evil.test/app.bin','Access-Control-Allow-Origin':'*'}});
      return route.fulfill({status:200,headers:{'Access-Control-Allow-Origin':'*'},
        contentType:manifestRequest?'application/json':'application/octet-stream',
        body:manifestRequest?JSON.stringify(scenario.manifest):app});
    });
    const response = await page.goto(origin);
    assert.equal(response.headers()['content-security-policy'], csp);
    await page.waitForFunction(()=>document.querySelector('#version').textContent === 'aaaaaaa');
    assert.deepEqual(await page.evaluate(()=>[isSecureContext, typeof crypto.subtle]), [false, 'undefined']);
    assert(await page.locator('#install-update').isDisabled());
    return {page, context};
  }
  async function check(page) {
    await page.locator('#check-update').click();
    await page.waitForFunction(()=>!document.querySelector('#check-update').disabled);
  }
  try {
    {
      // Only connect-src differs: browser blocks the manifest BEFORE any Pages
      // request, despite the fixture's valid CORS response. This is not CORS.
      const {page, context} = await setup(legacyCSP);
      await check(page);
      assert.match(await page.textContent('#update-message'), /检查失败.*Failed to fetch/);
      await page.waitForFunction(() => policyViolations.some(v =>
        v.directive === 'connect-src' && v.blockedURI ===
        'https://billxc.github.io/epaper-clock/firmware/manifest.json'));
      assert.equal(scenario.publicRequests,0);
      assert.equal(scenario.downloads,0); assert.equal(scenario.posted.length,0);
      assert(await page.locator('#install-update').isDisabled());
      await context.close(); total++;
    }
    {
      const {page, context} = await setup();
      await page.selectOption('#render-mode','linear'); await page.selectOption('#sleep-mode','deep_sleep');
      await page.selectOption('#pwr-on-ms','512');
      await check(page);
      assert.equal(await page.inputValue('#render-mode'), 'linear');
      assert.equal(await page.inputValue('#sleep-mode'), 'deep_sleep');
      assert.equal(await page.inputValue('#pwr-on-ms'), '512');
      assert.equal(scenario.downloads,0); assert.equal(scenario.posted.length,0);
      assert(!(await page.locator('#install-update').isDisabled()));
      const activity = scenario.activity;
      await page.waitForTimeout(1600);
      await page.evaluate(()=>poll());
      assert.equal(scenario.activity, activity, 'background work renewed idle timer');
      await page.locator('#install-update').click();
      await page.waitForFunction(()=>document.querySelector('#ota-message').textContent.includes('升级成功'));
      assert.equal(scenario.posted.length,1); assert.equal(scenario.downloads,1);
      assert.deepEqual(scenario.posted[0].body,app);
      assert.equal(scenario.posted[0].headers['x-firmware-sha256'],sha256);
      assert.equal(scenario.posted[0].headers.authorization,auth);
      await context.close(); total++;
    }
    for (const [mutate, expected] of [
      [()=>scenario.data.appversion='bbbbbbb', /同版本/],
      [()=>scenario.data.ota.next_slot_bytes=100, /容量不足/],
      [()=>scenario.manifest.project='wrong', /不匹配/],
      [()=>scenario.manifest.chip=0, /不匹配/],
      [()=>scenario.data.ota_sha256=false, /不支持/],
      [()=>scenario.manifest.url='https://evil.test/app.bin', /不合法/],
      [()=>scenario.manifest.url='../app.bin', /不合法/],
      [()=>scenario.manifest.sha256='invalid', /不合法/],
      [()=>scenario.fetchFail=true, /检查失败/],
      [()=>scenario.redirect=true, /检查失败/],
      [()=>scenario.statusFail=true, /检查失败/],
    ]) {
      const {page, context} = await setup(); mutate(); await check(page);
      assert.match(await page.textContent('#update-message'),expected);
      assert(await page.locator('#install-update').isDisabled());
      assert.equal(scenario.posted.length,0); assert.equal(scenario.downloads,0);
      await context.close(); total++;
    }
    for (const [mutate, expected] of [
      [()=>scenario.binFail=true,/在线安装未开始/],
      [()=>scenario.manifest.size=app.length+1,/大小与 manifest 不一致/],
      [()=>scenario.manifest.version='ccccccc',/与 manifest.*不匹配/],
      [()=>scenario.otaFail=true,/SHA256 mismatch/],
    ]) {
      const {page, context} = await setup(); mutate(); await check(page);
      await page.locator('#install-update').click();
      await page.waitForFunction(()=>!document.querySelector('#check-update').disabled);
      const text = await page.textContent('#update-message') + await page.textContent('#ota-message');
      assert.match(text,expected);
      assert.equal(scenario.posted.length, scenario.otaFail?1:0);
      assert(!(await page.locator('#upgrade').isDisabled()));
      await context.close(); total++;
    }
    {
      const {page, context} = await setup(); await check(page);
      scenario.statusFail = true;
      await page.locator('#install-update').click();
      await page.waitForFunction(()=>!document.querySelector('#check-update').disabled);
      assert.match(await page.textContent('#update-message'), /session unavailable/);
      assert.equal(scenario.downloads,0); assert.equal(scenario.posted.length,0);
      await context.close(); total++;
    }
    {
      const {page, context} = await setup();
      await page.setInputFiles('#firmware',{name:'manual.bin',mimeType:'application/octet-stream',buffer:app});
      await page.locator('#upgrade').click();
      await page.waitForFunction(()=>document.querySelector('#ota-message').textContent.includes('升级成功'));
      assert.equal(scenario.posted.length,1); assert.equal(scenario.downloads,0);
      assert.equal(scenario.posted[0].headers['x-firmware-sha256'], undefined);
      await context.close(); total++;
    }
    {
      const {page, context} = await setup(); await check(page);
      page.removeAllListeners('dialog'); page.on('dialog', dialog=>dialog.dismiss());
      await page.locator('#install-update').click();
      assert.equal(scenario.downloads,0); assert.equal(scenario.posted.length,0);
      await page.evaluate(()=>document.querySelector('#install-update').click());
      assert.equal(scenario.downloads,0); assert.equal(scenario.posted.length,0);
      await context.close(); total++;
    }
    console.log(`admin HTTP non-secure update browser: ${total} scenarios passed (production root CSP; includes legacy connect-src self rejection; mock only)`);
  } finally {await browser.close(); await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error); process.exitCode=1; server.close();});
