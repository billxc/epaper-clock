/* Browser UI regression only: simulated GATT and Security1, not hardware BLE.
 * Requires @playwright/test; no browser/device credentials or real BLE access.
 * NODE_PATH="$(npm root -g)" node tools/test_ble_feedback.cjs
 */
const {chromium} = require('@playwright/test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const docs = path.join(__dirname, '../docs');
const securityStub = `export {cryptoCheck} from './security1-real.mjs';
export class Security1 {
  static async establish(exchange,pop,subtle,onProgress) {
    onProgress('验证六位码：安全握手 1/2');
    onProgress('验证六位码：安全握手 2/2');
    return new Security1();
  }
  destroy() {}
  async request() {
    await new Promise(resolve=>{window.finishStatus=resolve});
    return new TextEncoder().encode(JSON.stringify({ok:true,wifi_configured:false,wifi_saved:false,wifi_state:'idle',wifi_ready:false,wifi_attempt:0,rtc_valid:false,rtc_time:'',epoch:null,time_set:false}));
  }
}`;
async function setup(browser, mode) {
  const page = await browser.newPage();
  const errors = [], logTasks = [];
  page.on('pageerror', error => errors.push(error.message));
  page.on('console', message => logTasks.push(Promise.all(message.args().map(arg => arg.jsonValue()))));
  await page.addInitScript(({mode}) => {
    if (mode === 'timeout') {
      const timeout = window.setTimeout.bind(window);
      window.setTimeout = (fn, ms, ...args) => timeout(fn, ms === 20000 ? 100 : ms, ...args);
    }
    Object.defineProperty(navigator, 'bluetooth', {value: {
      getAvailability: async () => true,
      requestDevice: async () => {
        const listeners = new Map();
        const characteristic = name => ({
          uuid: name, properties: {read:true, write:true},
          getDescriptors: async () => [{uuid:'00002901-0000-1000-8000-00805f9b34fb',
            readValue: async () => new DataView(new TextEncoder().encode(name).buffer)}],
          writeValueWithResponse: async () => {}, readValue: async () => new DataView(new ArrayBuffer(1)),
        });
        const service = {getCharacteristics: async () => {
          await new Promise(resolve=>{window.finishDiscovery=resolve});
          return ['prov-session','prov-config','clock-control'].map(characteristic);
        }};
        const gatt = {connected:false,
          connect:async()=>{gatt.connected=true;return {getPrimaryService:async()=>service}},
          disconnect:()=>{gatt.connected=false;listeners.get('gattserverdisconnected')?.()},
        };
        return {gatt,addEventListener:(name,fn)=>listeners.set(name,fn),removeEventListener:name=>listeners.delete(name)};
      }
    }});
  }, {mode});
  await page.route('https://setup.test/**', route => {
    let name = new URL(route.request().url()).pathname.slice(1) || 'index.html';
    if (name === 'security1.mjs') return route.fulfill({contentType:'text/javascript',body:securityStub});
    if (name === 'security1-real.mjs') name='security1.mjs';
    assert.match(name,/^[a-z0-9.-]+$/);
    return route.fulfill({contentType:name.endsWith('.mjs')?'text/javascript':name.endsWith('.css')?'text/css':'text/html',body:fs.readFileSync(path.join(docs,name))});
  });
  await page.goto('https://setup.test/');
  await page.waitForFunction(()=>!document.getElementById('connect').disabled);
  return {page,errors,logTasks};
}
(async () => {
  const browser = await chromium.launch({headless:true,chromiumSandbox:true});
  try {
    const {page,errors,logTasks} = await setup(browser,'success');
    await page.locator('#connect').click();
    await page.locator('#connection-feedback').getByText('请输入屏幕上的 6 位数字校验码', {exact:false}).waitFor();
    await page.locator('#pop').fill('042731'); await page.locator('#connect').click();
    await page.waitForFunction(()=>!!window.finishDiscovery);
    assert.match(await page.locator('#connection-feedback').textContent(),/蓝牙已连接|发现特征/);
    assert(await page.locator('#wifi button[type=submit]').isDisabled());
    await page.evaluate(()=>window.finishDiscovery());
    await page.waitForFunction(()=>!!window.finishStatus);
    assert.match(await page.locator('#connection-feedback').textContent(),/身份验证已通过/);
    assert(await page.locator('#wifi button[type=submit]').isDisabled());
    await page.evaluate(()=>window.finishStatus());
    await page.waitForFunction(()=>!document.getElementById('wifi-fields').disabled);
    assert.equal(await page.locator('#connection-feedback').getAttribute('data-state'),'ready');
    assert.equal(await page.locator('#connect').textContent(),'设备已验证连接');
    assert.equal(await page.locator('#pop').inputValue(),'');
    const logs=JSON.stringify(await Promise.all(logTasks));
    assert(logs.includes('[epaper-ble]') && logs.includes('gatt.connect.ok'));
    assert(!logs.includes('042731'));
    await page.locator('#disconnect').click();
    assert(await page.locator('#wifi button[type=submit]').isDisabled());
    assert.equal(errors.length,0,errors.join('\n')); await page.close();
    const timeout = await setup(browser,'timeout');
    await timeout.page.locator('#pop').fill('042731'); await timeout.page.locator('#connect').click();
    await timeout.page.waitForFunction(()=>document.getElementById('connection-feedback').dataset.state==='error');
    assert.match(await timeout.page.locator('#connection-feedback').textContent(),/发现特征.*超时/);
    assert(await timeout.page.locator('#wifi button[type=submit]').isDisabled());
    assert.equal(await timeout.page.locator('#connect').isDisabled(),false);
    assert.equal(timeout.errors.length,0,timeout.errors.join('\n')); await timeout.page.close();
    console.log('PASS: visible BLE connection phases, validation feedback, verified controls, safe logs and timeout recovery (UI mocks, not hardware).');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1});
