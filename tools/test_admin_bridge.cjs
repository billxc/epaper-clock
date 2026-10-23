/* Production admin.html bridge/UI mock regression, NOT hardware HTTPS/OTA or
 * certificate-exception acceptance. All origins/API responses are intercepted.
 * NODE_PATH="$(npm root -g)" node tools/test_admin_bridge.cjs
 */
const {chromium} = require('@playwright/test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const crypto = require('node:crypto');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../main/admin.html'), 'utf8');
const parentOrigin = 'https://billxc.github.io', deviceOrigin = 'https://device.board.test';
const nonce = '1'.repeat(32), requestId = '2'.repeat(32), protocol = 'epaper-admin-v1';
const app = Buffer.alloc(336);
app[0] = 0xe9; app.writeUInt16LE(9,12); app.writeUInt32LE(0xabcd5432,32);
app.write('bbbbbbb',48); app.write('epaper-clock',80);
const sha256 = crypto.createHash('sha256').update(app).digest('hex'), commit = 'b'.repeat(40);
const manifest = {schema:1,version:'bbbbbbb',project:'epaper-clock',chip:9,size:app.length,sha256,commit,
  url:`app-${commit}-${sha256}.bin`};
const parentHTML = `<!doctype html><iframe src="${deviceOrigin}/"></iframe><iframe src="/sibling"></iframe>
<script>window.messages=[];window.addEventListener('message',e=>{if(e.source===frames[0]&&e.origin==='${deviceOrigin}')messages.push(e.data)});</script>`;
const adminSource = fs.readFileSync(path.join(__dirname, '../main/admin.c'), 'utf8');
const rootPolicies = [...adminSource.matchAll(/httpd_resp_set_hdr\(r,\s*"Content-Security-Policy",\s*"(default-src[^"\r\n]+)"\)/g)];
assert.equal(rootPolicies.length, 1, 'expected exactly one production root CSP literal');
const csp = rootPolicies[0][1];
(async () => {
  const browser = await chromium.launch({headless:true, chromiumSandbox:true});
  let total = 0;
  async function setup() {
    const context = await browser.newContext();
    const state = {activity:0,posts:[],statusFail:false,otaFail:false,statusReads:0,
      data:{appversion:'aaaaaaa',project:'epaper-clock',chip:9,ota_sha256:true,boot_id:1,
        ssid:'PRIVATE-SSID-SENTINEL',sta_ip:'192.0.2.10',
        ota:{state:'idle',busy:false,current_slot_bytes:7864320,next_slot_bytes:7864320}}};
    await context.route('**/*', async route => {
      const req = route.request(), url = new URL(req.url());
      if (url.origin === parentOrigin || url.origin === 'https://evil.test') {
        assert.equal(req.headers().authorization, undefined);
        return route.fulfill({contentType:'text/html',body:url.pathname==='/epaper-clock/'?parentHTML:'<!doctype html><p>sender</p>'});
      }
      assert.equal(url.origin, deviceOrigin, 'unexpected external fetch');
      if (url.pathname === '/') return route.fulfill({contentType:'text/html',headers:{'Content-Security-Policy':csp},body:html});
      if (url.pathname === '/api/status') {
        state.statusReads++;
        if (state.statusFail) return route.fulfill({status:401,contentType:'application/json',body:'{"error":"PRIVATE-ERROR-SENTINEL"}'});
        return route.fulfill({contentType:'application/json',body:JSON.stringify(state.data)});
      }
      if (url.pathname === '/api/activity') state.activity++;
      else if (url.pathname === '/api/ota') {
        state.posts.push({body:req.postDataBuffer(),headers:req.headers()});
        if (state.otaFail) return route.fulfill({status:400,contentType:'application/json',body:'{"error":"PRIVATE-ERROR-SENTINEL"}'});
        state.data.boot_id++; state.data.appversion='bbbbbbb';
        state.data.ota={...state.data.ota,state:'success',expected_version:'bbbbbbb'};
      } else throw new Error('Unexpected API '+url.pathname);
      await route.fulfill({contentType:'application/json',body:'{"ok":true}'});
    });
    const page = await context.newPage();
    page.on('pageerror', e => { throw e; });
    await page.goto(parentOrigin+'/epaper-clock/');
    const child = page.frames().find(f => f.url().startsWith(deviceOrigin));
    await child.waitForFunction(()=>document.querySelector('#version').textContent==='aaaaaaa');
    assert.equal(await child.locator('#direct-update-actions').isVisible(), false);
    assert.equal(await child.locator('#upgrade').isVisible(), true);
    return {context,page,child,state};
  }
  async function send(page, fields) {
    await page.evaluate(m=>frames[0].postMessage(m,'https://device.board.test'), {protocol,nonce,...fields});
  }
  async function hello(page) {
    await send(page,{type:'hello'});
    await page.waitForFunction(()=>messages.some(m=>m.type==='ready'));
  }
  async function install(page, changes={}) {
    const options = {type:'install',requestId,confirmed:true,manifest,...changes};
    return page.evaluate(({options,bytes,protocol,nonce})=>{
      const buffer = new Uint8Array(bytes).buffer;
      frames[0].postMessage({protocol,nonce,...options,buffer},'https://device.board.test',[buffer]);
      return buffer.byteLength;
    }, {options,bytes:[...app],protocol,nonce});
  }
  try {
    {
      const {context,page,child,state} = await setup();
      assert.equal(await page.evaluate(()=>messages.length),0,'onload must not produce ready');
      await send(page,{type:'hello',protocol:'wrong'});
      await send(page,{type:'hello',nonce:'short'});
      // Genuine sibling source with correct publisher origin must be rejected.
      const sibling = page.frames().find(f=>f.url().endsWith('/sibling'));
      await sibling.evaluate(m=>top.frames[0].postMessage(m,'https://device.board.test'),{protocol,nonce,type:'hello'});
      // Genuine wrong-origin sender (not synthetic MessageEvent fields).
      await sibling.goto('https://evil.test/sender');
      await sibling.evaluate(m=>top.frames[0].postMessage(m,'https://device.board.test'),{protocol,nonce,type:'hello'});
      await page.waitForTimeout(100);
      assert.equal(await page.evaluate(()=>messages.length),0);
      await hello(page);
      const ready = await page.evaluate(()=>messages.find(m=>m.type==='ready'));
      assert.deepEqual(Object.keys(ready).sort(),['protocol','type','nonce','version','project','chip','next_slot_bytes','capabilities'].sort());
      assert.equal(state.activity,0,'handshake must not renew idle timer');
      await send(page,{type:'hello',nonce:'3'.repeat(32)});
      await child.evaluate(()=>poll());
      assert.equal(state.activity,0);
      assert.equal(await install(page,{nonce:'3'.repeat(32)}),0);
      await page.waitForTimeout(100); assert.equal(state.posts.length,0);
      assert.equal(await install(page),0,'transfer must detach parent buffer');
      await page.waitForFunction(()=>messages.some(m=>m.type==='result'&&m.state==='success'));
      assert.equal(state.posts.length,1); assert.deepEqual(state.posts[0].body,app);
      assert.equal(state.posts[0].headers['x-firmware-sha256'],sha256);
      assert.equal(state.activity,1,'one explicit confirmed parent action activity');
      await install(page);
      await page.waitForFunction(()=>messages.some(m=>m.code==='replay'));
      assert.equal(state.posts.length,1); assert.equal(state.activity,1);
      const messages = JSON.stringify(await page.evaluate(()=>messages));
      assert(!messages.includes('PRIVATE-') && !messages.includes('192.0.2.10'));
      assert(messages.includes('rebooting') && messages.includes('success'));
      await context.close(); total++;
    }
    const cases = [
      [{confirmed:false},'confirm'],
      [{manifest:{...manifest,url:'https://evil.test/app.bin'}},'manifest'],
      [{manifest:{...manifest,size:app.length+1}},'manifest'],
      [{manifest:{...manifest,version:'ccccccc'}},'image'],
      [{manifest:{...manifest,sha256:'invalid'}},'manifest'],
    ];
    for (const [changes,code] of cases) {
      const {context,page,state} = await setup(); await hello(page); await install(page,changes);
      await page.waitForFunction(code=>messages.some(m=>m.type==='error'&&m.code===code),code);
      assert.equal(state.posts.length,0); assert.equal(state.activity,0);
      await context.close(); total++;
    }
    for (const mode of ['status','capacity','sameversion','upload']) {
      const {context,page,state} = await setup(); await hello(page);
      if(mode==='status') state.statusFail=true;
      if(mode==='capacity') state.data.ota.next_slot_bytes=100;
      if(mode==='sameversion') state.data.appversion='bbbbbbb';
      if(mode==='upload') state.otaFail=true;
      await install(page);
      await page.waitForFunction(()=>messages.some(m=>m.type==='error'));
      assert.equal(state.posts.length,mode==='upload'?1:0);
      assert(!JSON.stringify(await page.evaluate(()=>messages)).includes('PRIVATE-'));
      await context.close(); total++;
    }
    {
      const {context,page,state} = await setup(); state.statusFail=true;
      await send(page,{type:'hello'});
      await page.waitForFunction(()=>messages.some(m=>m.code==='status'));
      assert.equal(await page.evaluate(()=>messages.some(m=>m.type==='ready')),false);
      assert.equal(state.activity,0);
      state.statusFail=false; await hello(page); await context.close(); total++;
    }
    console.log(`admin bridge production UI: ${total} scenarios passed (production root CSP/frame-ancestors; intercepted mock only)`);
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
