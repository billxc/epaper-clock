/* Route-only browser fixtures; no real device, credentials, TLS exceptions or OTA.
 * NODE_PATH="$(npm root -g)" node tools/test_lan_pages.cjs
 * HTTPS .test / localhost URLs below are intercepted, never contacted.
 */
const {chromium} = require('@playwright/test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {pathToFileURL} = require('node:url');
const docs = path.join(__dirname, '../docs');
const parent = 'https://pages.test';
const device = 'https://localhost:9443';
const other = 'https://other.test';
const app = Buffer.alloc(1024, 0x5a);
const sha = crypto.createHash('sha256').update(app).digest('hex');
const commit = '1234567890abcdef1234567890abcdef12345678';
const manifest = {schema:1,project:'epaper-clock',chip:9,version:commit.slice(0,7),commit,size:app.length,sha256:sha,url:`app-${commit}-${sha}.bin`};
const deviceFixture = `<!doctype html><title>Mock only</title><script>
window.messages=[]; window.received=[]; let boundNonce=null;
window.send = m => parent.postMessage(m, '${parent}');
window.ready = extra => send({...messages.at(-1),type:'ready',version:'old1234',project:'epaper-clock',chip:9,next_slot_bytes:8*1024*1024,capabilities:['sha256','transfer-install'],...extra});
addEventListener('message', e => {
 if(e.origin !== '${parent}' || e.source !== parent) return;
 const m=e.data;
 if(m.type==='hello' && m.protocol==='epaper-admin-v1' && /^[0-9a-f]{32}$/.test(m.nonce)) {
   if(boundNonce && boundNonce!==m.nonce) return;
   boundNonce=m.nonce;messages.push(m);
 }
 if(m.type==='install') received.push({...m,bytes:Array.from(new Uint8Array(m.buffer)),isBuffer:m.buffer instanceof ArrayBuffer});
});
</script>`;
async function main() {
  const {parseOrigin,validateManifest} = await import(pathToFileURL(path.join(docs,'lan/lan.mjs')));
  for (const v of ['https://localhost','https://localhost:9443/','https://192.168.1.2','https://epaper.local:443','https://clock.example.org:9443','https://[::1]:9443']) assert.equal(parseOrigin(v),new URL(v).origin);
  for (const v of ['http://localhost','ftp://localhost','https://user:pass@localhost','https://@localhost','https://localhost/path','https://localhost/a/..','https://localhost/?','https://localhost/#','https://localhost//','https://localhost\\x','https://localhost:99999']) assert.throws(()=>parseOrigin(v),v);
  assert.deepEqual(validateManifest({...manifest,extra:'not forwarded'}),manifest);
  for (const patch of [{schema:2},{chip:0},{project:'other'},{version:'other'},{size:0},{size:17*1024*1024},{sha256:'0'.repeat(64)},{url:'https://other.test/app.bin'},{url:'../app.bin'},{commit:'bad'}]) assert.throws(()=>validateManifest({...manifest,...patch}));
  console.log('PASS origin / manifest validation');
  const browser = await chromium.launch({headless:true,chromiumSandbox:true});
  try {
    const context = await browser.newContext();
    const page = await context.newPage();
    const errors=[], requests=[];
    page.on('pageerror',e=>errors.push(e.message));
    let mode = 'normal', manifestOverride = null, releaseDownload;
    await context.route('**/*',async route=>{
      const req=route.request(), u=new URL(req.url()); requests.push({url:u.href,type:req.resourceType(),headers:req.headers()});
      if (u.origin===parent && u.pathname.startsWith('/epaper-clock/lan/')) {
        const name=u.pathname.split('/').pop() || 'index.html';
        assert(['index.html','lan.mjs','lan.css','README.md'].includes(name));
        return route.fulfill({contentType:name.endsWith('.mjs')?'text/javascript':name.endsWith('.css')?'text/css':'text/html',body:fs.readFileSync(path.join(docs,'lan',name))});
      }
      if(u.origin===parent && u.pathname==='/epaper-clock/firmware/manifest.json') return route.fulfill({contentType:'application/json',body:JSON.stringify(manifestOverride || manifest)});
      if(u.origin===parent && u.pathname===`/epaper-clock/firmware/${manifest.url}`) {
        if(mode==='delay') await new Promise(resolve=>{releaseDownload=resolve});
        return route.fulfill({contentType:'application/octet-stream',body:mode==='hash'?Buffer.alloc(app.length,1):mode==='size'?app.subarray(1):mode==='oversize'?Buffer.alloc(app.length+1):app});
      }
      if([device,other].includes(u.origin) && u.pathname==='/' && req.resourceType()==='document') return route.fulfill({contentType:'text/html',body:deviceFixture});
      throw new Error(`Unexpected network request ${u.href}`);
    });
    await page.addInitScript(()=>{
      const digest=crypto.subtle.digest.bind(crypto.subtle);
      crypto.subtle.digest=(algorithm,data)=>{window.digestBuffer=data;return digest(algorithm,data)};
      const native=window.setTimeout.bind(window);
      window.setTimeout=(fn,ms,...args)=>native(fn,window.fastTimeout && ms===20000?60:window.fastInstall && ms===180000?100:ms,...args);
    });
    await page.goto(`${parent}/epaper-clock/lan/`);
    const connection=page.locator('#connection'), update=page.locator('#update');
    const read = locator=>locator.textContent();
    const waitText = async (locator,text)=>{await locator.getByText(text,{exact:false}).waitFor()};
    async function connect(readyNow=true) {
      await page.locator('#origin').fill(device);
      await page.locator('#connect').click();
      await page.waitForFunction(()=>document.querySelector('iframe')?.contentWindow!==null);
      let child;
      for(let i=0;i<100;i++) {child=page.frames().find(f=>f.url()===`${device}/`);if(child)break;await page.waitForTimeout(10);}
      assert(child); await child.waitForFunction(()=>window.messages?.length>0);
      if(readyNow) {await child.evaluate(()=>ready());await waitText(connection,'已连接：');}
      return child;
    }
    let child=await connect(false);
    assert.match(await read(connection),/加载事件.*尚未连接/);
    assert(await page.locator('#check').isDisabled());
    assert.equal(requests.filter(r=>r.url.includes('/firmware/')).length,0);
    const hello=await child.evaluate(()=>messages.at(-1));
    assert.match(hello.nonce,/^[0-9a-f]{32}$/);
    await child.waitForFunction(()=>window.messages.length>1, null, {polling:100,timeout:5000});
    assert.equal(await child.evaluate(()=>messages.at(-1).nonce),hello.nonce,'hello retry keeps first binding');
    // Right source/origin but wrong protocol/nonce or invalid ready summary.
    await child.evaluate(()=>{ready({nonce:'0'.repeat(32)});ready({protocol:'wrong'});ready({project:'wrong'});send({...messages.at(-1),type:'status',project:'epaper-clock'});});
    assert(await page.locator('#check').isDisabled());
    // Genuine wrong-source sender at the same device origin; genuine wrong-origin sender.
    for(const origin of [device,other]) {
      const navigated=page.waitForEvent('framenavigated',{predicate:f=>f.url()===origin+'/'});
      await page.evaluate(origin=>{const f=document.createElement('iframe');f.id='forger';f.src=origin+'/';document.body.append(f)},origin);
      const sibling=await navigated;
      await sibling.waitForFunction(()=>typeof send==='function');
      await sibling.evaluate(m=>send({...m,type:'ready',version:'old1234',project:'epaper-clock',chip:9,next_slot_bytes:8388608,capabilities:['sha256','transfer-install']}),hello);
      await page.locator('#forger').evaluate(f=>f.remove());
    }
    assert(await page.locator('#check').isDisabled());
    await child.evaluate(()=>ready()); await waitText(connection,'已连接：');
    console.log('PASS load != ready; protocol, nonce, summary, source and origin rejection');
    // Top-level new window uses only the user origin and does not fetch APIs in parent.
    const popupPromise=context.waitForEvent('page');
    await page.locator('#open').click();const popup=await popupPromise;await popup.waitForLoadState();
    assert.equal(popup.url(),`${device}/`);assert.equal(await popup.evaluate(()=>opener),null);await popup.close();
    const beforeSyntheticCheck=requests.length;
    await page.locator('#check').evaluate(button=>{button.click();button.dispatchEvent(new MouseEvent('click',{bubbles:true}));});
    assert.equal(requests.length,beforeSyntheticCheck,'synthetic check makes no request');
    await page.locator('#check').click();await waitText(update,'可安装');
    assert.equal(requests.filter(r=>r.url.endsWith('.bin')).length,0);
    const beforeSyntheticInstall=requests.length;
    let syntheticDialogs=0;
    const rejectDialog=dialog=>{syntheticDialogs++;return dialog.dismiss()};page.on('dialog',rejectDialog);
    await page.evaluate(()=>{for(const id of ['install','open','connect','disconnect']) {const b=document.getElementById(id);b.click();b.dispatchEvent(new MouseEvent('click',{bubbles:true}));}});
    await page.waitForTimeout(50);page.off('dialog',rejectDialog);
    assert.equal(syntheticDialogs,0);assert.equal(requests.length,beforeSyntheticInstall);
    assert.equal(context.pages().length,1);assert(!child.isDetached());
    assert.equal(await child.evaluate(()=>received.length),0,'synthetic install never transfers');
    console.log('PASS synthetic check/install/open/connect/disconnect rejected without requests or dialogs');
    // Cancel is a true no-op: no bin and no device install.
    page.once('dialog',d=>d.dismiss());await page.locator('#install').click();
    assert.equal(requests.filter(r=>r.url.endsWith('.bin')).length,0);
    page.once('dialog',d=>d.accept());await page.locator('#install').click();
    await child.waitForFunction(()=>received.length===1);
    const received=await child.evaluate(()=>{const m=received[0];return {...m,buffer:undefined}});
    assert(received.isBuffer);assert(received.confirmed);assert.match(received.requestId,/^[0-9a-f]{32}$/);
    assert.deepEqual(Buffer.from(received.bytes),app);assert.deepEqual(received.manifest,manifest);
    assert.equal(await page.evaluate(()=>digestBuffer.byteLength),0,'sender buffer detached');
    const lockedIds=['origin','open','connect','disconnect'];
    for(const id of lockedIds) assert(await page.locator('#'+id).isDisabled(),id+' locked while pending');
    const unloadBlocked=()=>page.evaluate(()=>{const e=new Event('beforeunload',{cancelable:true});window.dispatchEvent(e);return e.defaultPrevented});
    assert(await unloadBlocked(),'parent beforeunload prevents departure while pending');
    const leaving=page.waitForEvent('dialog');const reload=page.reload().catch(()=>null);
    const warning=await leaving;assert.equal(warning.type(),'beforeunload');await warning.dismiss();await reload;
    assert(!child.isDetached(),'cancelled parent reload preserves the iframe');
    await page.locator('#origin').evaluate(input=>input.dispatchEvent(new Event('input',{bubbles:true})));
    assert(!child.isDetached(),'pending input event cannot clear session');
    await child.evaluate(()=>{const m=received[0];send({...m,type:'result',requestId:'0'.repeat(32),state:'success'});});
    assert.match(await read(update),/等待设备安装结果/);
    await child.evaluate(()=>{const m=received[0];send({protocol:m.protocol,nonce:m.nonce,type:'result',requestId:m.requestId,state:'rebooting'});});
    await waitText(update,'这不是启动成功证明'); assert(await page.locator('#install').isDisabled());
    await child.evaluate(()=>{const m=received[0];send({protocol:m.protocol,nonce:m.nonce,type:'result',requestId:m.requestId,state:'success'});});
    await waitText(update,'设备报告新固件启动成功');
    for(const id of lockedIds) assert(await page.locator('#'+id).isEnabled(),id+' restored after result');
    assert(!(await unloadBlocked()),'completed result releases beforeunload protection');
    assert(await page.locator('#check').isDisabled(),'old ready version cannot be reused after installation');
    console.log('PASS pending control lock and actual parent beforeunload dialog; result releases protection');
    console.log('PASS explicit check, confirmation, download, SHA, byte-exact transferable/detached buffer, request-bound result');
    // Every new session rejects old messages from the actual new child window as well.
    child=await connect(false);
    const newHello=await child.evaluate(()=>messages.at(-1));assert.notEqual(newHello.nonce,hello.nonce);
    await child.evaluate(old=>ready({nonce:old}),hello.nonce);assert(await page.locator('#check').isDisabled());
    await child.evaluate(()=>ready());await waitText(connection,'已连接：');
    const previousFrame=child;
    await child.goto(`${device}/`);await waitText(connection,'请手动点击连接iframe');
    assert(await page.locator('#check').isDisabled());
    assert.equal(await child.evaluate(()=>messages.length),0,'no automatic rebind after document reload');
    child=await connect(false);
    assert(previousFrame.isDetached(),'manual reconnect replaces the entire iframe');
    const reloadHello=await child.evaluate(()=>messages.at(-1));assert.notEqual(newHello.nonce,reloadHello.nonce);
    await child.evaluate(old=>ready({nonce:old}),newHello.nonce);assert(await page.locator('#check').isDisabled());
    await child.evaluate(()=>ready());await waitText(connection,'已连接：');
    for(const failure of ['hash','size','oversize']) {
      mode=failure;await page.locator('#check').click();await waitText(update,'可安装');
      page.once('dialog',d=>d.accept());await page.locator('#install').click();await waitText(update,failure==='hash'?'SHA-256 不匹配':failure==='size'?'大小不匹配':'安全上限');
      assert.equal(await child.evaluate(()=>received.length),0);
      for(const id of lockedIds) assert(await page.locator('#'+id).isEnabled(),id+' remains usable after download failure');
    }
    await child.evaluate(()=>send({...messages.at(-1),type:'error',code:'busy',message:'secret-must-not-render'}));
    await waitText(update,'设备正忙');assert(!(await read(update)).includes('secret-must-not-render'));
    mode='normal';manifestOverride={...manifest,url:'https://other.test/app.bin'};
    await page.locator('#check').click();await waitText(update,'检查失败');assert(await page.locator('#install').isDisabled());manifestOverride=null;
    console.log('PASS reconnect/reload replay rejection, corrupt size/hash and unsafe manifest blocked');
    // In-flight fetch is cancelled on session/address change, never forwarded later.
    mode='delay';await page.locator('#check').click();await waitText(update,'可安装');
    page.once('dialog',d=>d.accept());await page.locator('#install').click();
    while(!releaseDownload) await page.waitForTimeout(10);
    await page.locator('#origin').fill(other);releaseDownload();await page.waitForTimeout(100);
    assert.equal(page.frames().length,1);assert(await page.locator('#check').isDisabled());
    assert.match(await read(connection),/旧会话已失效/);
    mode='normal';
    child=await connect(true);await page.evaluate(()=>window.fastInstall=true);
    await page.locator('#check').click();await waitText(update,'可安装');
    page.once('dialog',d=>d.accept());await page.locator('#install').click();
    await waitText(update,'等待安装结果超时');assert(await page.locator('#check').isDisabled());
    await child.evaluate(()=>{const m=received.at(-1);send({protocol:m.protocol,nonce:m.nonce,type:'result',requestId:m.requestId,state:'success'});});
    assert.match(await read(update),/结果未知/);
    for(const id of lockedIds) assert(await page.locator('#'+id).isEnabled(),id+' restored after timeout');
    assert(!(await unloadBlocked()),'timeout clears parent unload protection');
    // Timeout still refuses ready, even if the document already fired load.
    await page.evaluate(()=>window.fastTimeout=true);child=await connect(false);
    await waitText(connection,'ready 超时');await child.evaluate(()=>ready());
    assert(await page.locator('#check').isDisabled());
    await page.locator('#disconnect').click();assert.equal(await page.locator('#origin').inputValue(),'');
    assert.equal(await page.evaluate(()=>localStorage.length),0);
    assert.equal(requests.filter(r=>new URL(r.url).origin!==parent && r.type!=='document').length,0);
    for(const r of requests.filter(r=>r.url.includes('/firmware/'))) {assert(!r.headers.authorization);assert(!r.headers.cookie);assert(!r.headers.referer);}
    assert.deepEqual(errors,[]);
    console.log('PASS address-change cancellation, ready/install timeouts fail-closed, no storage/device API/credential requests');
    console.log('All LAN Pages mock checks passed (not TLS/BasicAuth/LNA or device acceptance).');
    await context.close();
  } finally {await browser.close();}
}
main().catch(e=>{console.error(e);process.exitCode=1});
