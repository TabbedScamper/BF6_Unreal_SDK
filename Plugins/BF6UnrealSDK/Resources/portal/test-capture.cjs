'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const { chromium } = require(process.env.BF6_PLAYWRIGHT || 'playwright');
(async () => {
  const browser = await chromium.launch({ headless: true, ...(process.env.BF6_CHROMIUM ? { executablePath: process.env.BF6_CHROMIUM } : {}) });
  try {
    const page = await browser.newPage();
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.route('http://portal.test/**', route => route.fulfill({ contentType: 'text/html', body: '<main>MY EXPERIENCES</main>' }));
    await page.goto('http://portal.test/bf6/experiences');
    await page.evaluate(() => {
      window.messages = []; window.requests = [];
      window.ue = { bf6portal: { pagestate: s => messages.push(JSON.parse(s)), capture: s => messages.push(JSON.parse(s)) } };
      window.fetch = async (url, init) => {
        requests.push({ url, method: init.method, body: Array.from(new Uint8Array(init.body.buffer || init.body, init.body.byteOffset || 0, init.body.byteLength)) });
        return new Response(new Uint8Array([0,0,0,0,0]));
      };
    });
    await page.addScriptTag({ path: path.join(__dirname, 'capture.js') });
    // No observed request: remount the list, without signing out or a reload.
    await page.evaluate(() => { BF6PortalCapture.getOwnedList(); BF6PortalCapture.getOwnedList(); });
    assert.equal(new URL(page.url()).pathname, '/bf6');
    await page.waitForURL('**/bf6/experiences');
    assert.deepEqual(await page.evaluate(() => requests), []);
    // A creator navigating during the deferred remount keeps their destination.
    await page.evaluate(() => { BF6PortalCapture.getOwnedList(); history.pushState({}, '', '/bf6/experience/settings/mode?id=11111111-1111-1111-1111-111111111111'); });
    await page.waitForTimeout(500);
    assert.match(page.url(), /\/experience\/settings\/mode/);
    await page.evaluate(() => BF6PortalCapture.getOwnedList());
    assert.equal(await page.evaluate(() => messages.at(-1).list), 'no-request-observed');
    // Replaying a typed-array slice must not leak adjacent bytes into the RPC.
    await page.evaluate(async () => {
      const storage = new Uint8Array([99,0,0,0,0,0,88]);
      await fetch('http://portal.test/santiago.web.play.WebPlay/getOwnedPlayElementsV2', { method:'POST', body: storage.subarray(1,6) });
      BF6PortalCapture.getOwnedList();
    });
    await page.waitForFunction(() => requests.length === 2 && messages.some(m => m.list === 'replayed:200'));
    assert.deepEqual(await page.evaluate(() => requests.map(r => r.body)), [[0,0,0,0,0],[0,0,0,0,0]]);
    // The requested card is opened by Modify, never Publish or its menu.
    await page.evaluate(() => {
      history.pushState({}, '', '/bf6/experiences');
      document.body.innerHTML = '<section class="experience-tile-module_tile__test"><div>Custom Rush Template 4.0 copy</div><button>Publish</button><button>Modify</button><button>Menu</button></section>';
      window.clicked = [];
      document.querySelectorAll('button').forEach(b => b.onclick = () => clicked.push(b.textContent));
      BF6PortalCapture.openExperience('d3173590-ad18-11f1-87f2-3ade824ce4ea', 'Custom Rush Template 4.0 copy');
    });
    assert.deepEqual(await page.evaluate(() => clicked), ['Modify']);
    assert.deepEqual(errors, []);
    console.log('PASS: list remount, navigation preservation, fresh replay, exact request bytes, correct card control');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
