// Real Chromium integration test. Pass a Portal workspace and an artifact folder.
// BF6_PLAYWRIGHT may point to an installed Playwright package; BF6_CHROMIUM may
// select an existing browser. No project or website is written by this test.
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { pathToFileURL } = require('node:url');
const { chromium } = require(process.env.BF6_PLAYWRIGHT || 'playwright');
const blocks = path.resolve(__dirname, '..');
const fixture = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const out = path.resolve(process.argv[3] || 'blocks-browser-results');
fs.mkdirSync(out, { recursive: true });
const original = fixture.mod || fixture;
function duplicate(doc) {
  const copy = JSON.parse(JSON.stringify(doc));
  function walk(n) {
    if (!n || typeof n !== 'object') return;
    if (n.type && n.id) n.id += '-stress-copy';
    Object.values(n).forEach(walk);
  }
  walk(copy.blocks.blocks);
  copy.blocks.blocks.forEach(b => { b.x = (b.x || 0) + 25000; });
  return { ...doc, blocks: { ...doc.blocks, blocks: [...doc.blocks.blocks, ...copy.blocks.blocks] } };
}
(async () => {
  const browser = await chromium.launch({ headless: true,
    executablePath: process.env.BF6_CHROMIUM || undefined, args: ['--allow-file-access-from-files'] });
  try {
    const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
    page.on('console', m => { if (m.type() === 'log') console.log(m.text()); });
    const errors = []; page.on('pageerror', e => { errors.push(e.message); console.error('PAGE ERROR', e.message); });
    await page.goto(pathToFileURL(path.join(blocks, 'editor.html')).href);
    await page.waitForFunction(() => window.BF6UI && BF6UI.ws);
    const read = name => JSON.parse(fs.readFileSync(path.join(blocks, 'offline', name + '.json'), 'utf8'));
    await page.evaluate(defs => BF6Blocks.recv(defs), { op: 'defs', definitions: read('definitions'),
      synthesized: read('definitions_synth'), toolbox: read('toolbox'), options: read('options'),
      tooltips: read('tooltips'), icons: read('icons'), helpUrls: read('help_urls') });
    const pack = read('mirror_icons'), style = read('style');
    assert.equal(JSON.stringify(pack).includes('file:///'), false, 'Bundled style depends on a developer machine');
    Object.assign(style, { mirrorIcons: pack.categoryIcons, mirrorValueTypeIcons: pack.valueTypeIcons,
      blockImages: pack.blockImages, fontCss: pack.fontCss, fontAliases: pack.fontAliases,
      fonts: pack.fontFamilies });
    await page.evaluate(style => BF6Blocks.recv(style), style);
    await page.evaluate(() => document.fonts.ready);
    await page.evaluate(() => {
      window.testMessages = [];
      const chunks = {};
      window.ue = { bf6blocks: { msg: text => {
        const m = JSON.parse(text);
        if (m.op !== 'chunk') { testMessages.push(m); return; }
        const c = chunks[m.cid] || (chunks[m.cid] = []); c[m.i] = m.part;
        if (c.filter(x => x !== undefined).length === m.n) { testMessages.push(JSON.parse(c.join(''))); delete chunks[m.cid]; }
      } } };
    });
    const cdp = await page.context().newCDPSession(page);
    if (Number(process.env.BF6_CPU_RATE) > 1) await cdp.send('Emulation.setCPUThrottlingRate', { rate: Number(process.env.BF6_CPU_RATE) });
    async function load(doc) {
      await page.waitForTimeout(350);
      await page.evaluate(() => { window.testMessages = []; });
      const start = Date.now();
      console.log('import starting');
      await page.evaluate(text => BF6UI.importText(text, 'stress-workspace.json'), JSON.stringify({ mod: doc }));
      await page.waitForFunction(() => !BF6UI.loadingDoc && !BF6UI.applying && !BF6Blocks.state.loading, {}, { timeout: 240000 });
      console.log('import finished');
      await page.evaluate(() => { console.log('before renderer flush'); return Promise.race([Blockly.renderManagement.finishQueuedRenders().then(() => console.log('renderer settled')), new Promise((_, reject) => setTimeout(() => reject(Error('Renderer did not settle')), 5000))]); });
      await page.waitForTimeout(100);
      assert.equal(await page.evaluate(() => !!BF6UI.loadFailed), false);
      await page.waitForTimeout(350);
      assert.deepEqual(await page.evaluate(() => testMessages.filter(m => ['replaceTop', 'deleteTop', 'variables', 'chunk'].includes(m.op)).map(m => m.op)), [], 'Loading published edits');
      return Date.now() - start;
    }
    const results = [];
    for (const doc of [original, duplicate(original)]) {
      const loadMs = await load(doc);
      const checks = await page.evaluate(async originalText => {
        const original = JSON.parse(originalText);
        const ws = BF6UI.ws, assert = (ok, msg) => { if (!ok) throw Error(msg); };
        console.log('checking serialized graph');
        function index(doc) {
          const byId = {};
          function walk(n) { if (!n || typeof n !== 'object') return;
            if (n.type && n.id) byId[n.id] = n;
            Object.values(n).forEach(walk); }
          walk(doc.blocks.blocks); return byId;
        }
        const expected = index(original), saved = BF6Blocks.saveWorkspace(ws), actual = index(saved);
        assert(Object.keys(expected).length === ws.getAllBlocks(false).length, 'Import lost or duplicated blocks');
        assert(Object.keys(actual).length === Object.keys(expected).length, 'Export lost blocks');
        for (const [id, source] of Object.entries(expected)) {
          const target = actual[id]; assert(target && source.type === target.type, 'Missing/type-changed block ' + id);
          for (const [key, value] of Object.entries(source.fields || {}))
            assert(JSON.stringify(value) === JSON.stringify((target.fields || {})[key]), 'Changed field ' + source.type + '.' + key);
          for (const [key, input] of Object.entries(source.inputs || {})) {
            for (const kind of ['block', 'shadow']) if (input[kind])
              assert(target.inputs && target.inputs[key] && target.inputs[key][kind] && target.inputs[key][kind].id === input[kind].id, 'Changed connection ' + id + '.' + key);
          }
          if (source.next && source.next.block) assert(target.next && target.next.block.id === source.next.block.id, 'Changed next connection ' + id);
        }
        assert(JSON.stringify(saved.variables) === JSON.stringify(original.variables), 'Variable identities/names changed');
        const text = ws.getTopBlocks(false).find(b => b.type === 'Text');
        assert(text, 'Fixture needs a free-standing Text block');
        const xy = text.getRelativeToSurfaceXY(); ws.setScale(.8); ws.scroll(-xy.x * .8 + 500, -xy.y * .8 + 200);
        await new Promise(requestAnimationFrame);
        const label = text.getField('TEXT').getSvgRoot().querySelector('text');
        assert(label && label.textContent && getComputedStyle(label).display !== 'none' && label.getBoundingClientRect().width > 0, 'Initial label is missing');
        assert(!text.getSvgRoot().closest('.bf6-viewport-pruned'), 'Visible label was pruned');
        const programBeforeZoom = JSON.stringify(BF6Blocks.saveWorkspace(ws));
        const positions = ws.getAllBlocks(false).map(b => [b, b.getRelativeToSurfaceXY()]);
        const zoomFrames = []; let lastZoomFrame = 0;
        for (let i = 0; i < 80; ++i) {
          const now = await new Promise(requestAnimationFrame);
          if (lastZoomFrame) zoomFrames.push(now - lastZoomFrame); lastZoomFrame = now;
          const oldMatrix = ws.getCanvas().getCTM(), anchor = new DOMPoint(800, 400).matrixTransform(oldMatrix.inverse());
          ws.zoom(800, 400, i < 40 ? .12 : -.12);
          const afterAnchor = anchor.matrixTransform(ws.getCanvas().getCTM());
          assert(Math.abs(afterAnchor.x - 800) < .01 && Math.abs(afterAnchor.y - 400) < .01, 'Zoom moved the cursor anchor');
        }
        for (const [b, p] of positions) {
          const q = b.getRelativeToSurfaceXY();
          assert(ws.getCanvas().contains(b.getSvgRoot()), 'Pruning detached a block transform');
          assert(p.x === q.x && p.y === q.y, 'Zoom changed a block coordinate');
        }
        assert(JSON.stringify(BF6Blocks.saveWorkspace(ws)) === programBeforeZoom, 'Zoom changed the exported program');
        // Revealing all artwork must restore real field nodes and the exact
        // order, including nested connected blocks, before rendering or editing.
        ws.bf6Viewport.invalidate();
        for (const b of ws.getAllBlocks(false)) {
          assert(!b.pathObject || b.getSvgRoot().contains(b.pathObject.svgPath), 'Path was not restored');
          for (const input of b.inputList) for (const field of input.fieldRow)
            assert(!field.getSvgRoot() || b.getSvgRoot().contains(field.getSvgRoot()), 'Field was not restored');
        }
        ws.bf6Viewport.update();
        // Render a changed field while it is far off screen, then return to it.
        ws.scroll(100000, 100000); await new Promise(requestAnimationFrame);
        const old = text.getFieldValue('TEXT');
        text.setFieldValue('A much longer label changed while off screen, without adding any blocks', 'TEXT');
        await Blockly.renderManagement.finishQueuedRenders();
        await new Promise(requestAnimationFrame);
        const hiddenWidth = text.width;
        ws.scroll(-xy.x * .8 + 500, -xy.y * .8 + 200); await new Promise(requestAnimationFrame);
        assert(!text.getSvgRoot().closest('.bf6-viewport-pruned'), 'Edited block did not reappear');
        assert(text.getField('TEXT').getSvgRoot().textContent.replace(/\u00a0/g, ' ').includes('much longer'), 'Edited label stayed stale: ' + text.getField('TEXT').getSvgRoot().textContent);
        text.getField('TEXT').forceRerender(); await Blockly.renderManagement.finishQueuedRenders();
        assert(Math.abs(text.width - hiddenWidth) < .01, 'Hidden field measured at a different width');
        text.setFieldValue(old, 'TEXT'); await Blockly.renderManagement.finishQueuedRenders();
        // Appearance changes must leave the serialized program exactly intact.
        const before = JSON.stringify(BF6Blocks.saveWorkspace(ws));
        BF6Blocks.recv({ op: 'prefs', values: { categoryColors: '{"variable-block-style":0}' } });
        await Blockly.renderManagement.finishQueuedRenders();
        const variableBlock = ws.getAllBlocks(false).find(b => b.type === 'variableReferenceBlock');
        assert(variableBlock.getColour() === '#601111', 'Category color did not reach actual blocks');
        assert(JSON.stringify(BF6Blocks.saveWorkspace(ws)) === before, 'Theme changed the program');
        BF6Blocks.recv({ op: 'prefs', values: { categoryColors: '{}' } }); await Blockly.renderManagement.finishQueuedRenders();
        // Exercise undo/redo after culling, using actual Blockly move events.
        const spacing = document.getElementById('field-spacing');
        const originalHeight = text.getField('TEXT').getSize().height, originalFont = getComputedStyle(label).font;
        let spacingStart = performance.now();
        spacing.value = 'compact'; spacing.dispatchEvent(new Event('change'));
        await Blockly.renderManagement.finishQueuedRenders();
        const compactLayoutMs = performance.now() - spacingStart;
        console.log('Compact field layout ms:', Math.round(compactLayoutMs));
        assert(text.getField('TEXT').getSize().height < originalHeight, 'Compact spacing did not shorten text fields: ' + JSON.stringify({originalHeight,now:text.getField('TEXT').getSize(),constant:ws.getRenderer().getConstants().FIELD_BORDER_RECT_HEIGHT,fieldConstant:text.getField('TEXT').getConstants().FIELD_BORDER_RECT_HEIGHT,editable:text.getField('TEXT').EDITABLE}));
        const variable = ws.getAllBlocks(false).find(b => b.type === 'variableReferenceBlock');
        assert(variable.getField('VAR').getSize().height < originalHeight, 'Compact spacing did not shorten variable fields');
        assert(getComputedStyle(label).font === originalFont, 'Compact spacing shrank the font');
        assert(JSON.stringify(BF6Blocks.saveWorkspace(ws)) === before, 'Field spacing changed the program');
        spacingStart = performance.now();
        spacing.value = 'portal'; spacing.dispatchEvent(new Event('change'));
        await Blockly.renderManagement.finishQueuedRenders();
        const portalLayoutMs = performance.now() - spacingStart;
        console.log('Portal field layout ms:', Math.round(portalLayoutMs));
        assert(text.getField('TEXT').getSize().height === originalHeight, 'Portal field spacing did not restore');
        const moved = new Promise(resolve => {
          const listener = ev => { if (ev.type === Blockly.Events.BLOCK_MOVE && ev.blockId === text.id) { ws.removeChangeListener(listener); resolve(); } };
          ws.addChangeListener(listener);
        });
        ws.clearUndo(); text.moveBy(160, 80); await moved;
        ws.undo(false); await Blockly.renderManagement.finishQueuedRenders();
        assert(Math.abs(text.getRelativeToSurfaceXY().x - xy.x) < .01, 'Move undo failed');
        ws.undo(true); await Blockly.renderManagement.finishQueuedRenders();
        assert(Math.abs(text.getRelativeToSurfaceXY().x - xy.x - 160) < .01, 'Move redo failed');
        ws.undo(false); await Blockly.renderManagement.finishQueuedRenders();
        // Zoom/pan from an entirely empty viewport must reveal incoming blocks.
        ws.scroll(-1000000, -1000000); await new Promise(requestAnimationFrame);
        ws.scroll(-xy.x * .8 + 500, -xy.y * .8 + 200); await new Promise(requestAnimationFrame);
        assert(!text.getSvgRoot().closest('.bf6-viewport-pruned'), 'Return from empty canvas stayed blank');
        return { blocks: ws.getAllBlocks(false).length, variables: ws.getAllVariables().length,
          compactLayoutMs, portalLayoutMs,
          zoomFrameMeanMs: zoomFrames.reduce((a,b) => a+b, 0) / zoomFrames.length,
          version: Blockly.VERSION, viewport: { ...ws.bf6Viewport.stats } };
      }, JSON.stringify(doc));
      await page.screenshot({ path: path.join(out, 'readable-' + checks.blocks + '.png') });
      results.push({ loadMs, checks }); console.log(results[results.length - 1]);
      // Real mouse input: pan the canvas without switching off pruning.
      const start = await page.evaluate(() => ({ x: BF6UI.ws.scrollX, y: BF6UI.ws.scrollY }));
      await page.mouse.move(1000, 400); await page.keyboard.down('Alt');
      await page.mouse.down(); await page.mouse.move(1120, 460, { steps: 16 }); await page.mouse.up(); await page.keyboard.up('Alt');
      const end = await page.evaluate(() => ({ x: BF6UI.ws.scrollX, y: BF6UI.ws.scrollY, pruned: BF6UI.ws.bf6Viewport.stats.pruned }));
      assert.ok(Math.abs(end.x - start.x) > 50, 'Mouse pan did not move the workspace');
      assert.ok(end.pruned > 0, 'Mouse pan disabled pruning');
      // The overview holds the exact same serializable program and returns to
      // a real editable block on a mouse click, without a separate reload.
      const target = await page.evaluate(async () => {
        const ws = BF6UI.ws, before = JSON.stringify(BF6Blocks.saveWorkspace(ws));
        const b = ws.getTopBlocks(false).find(b => b.type === 'Text'), xy = b.getRelativeToSurfaceXY();
        ws.setScale(.1); ws.scroll(-xy.x * .1 + 500, -xy.y * .1 + 200);
        await new Promise(requestAnimationFrame);
        if (ws.bf6Viewport.overview.stats.active) throw Error('Overview changed the default appearance');
        // The real slider updates immediately, saves on release, and keeps
        // icons visible even when the creator chooses to hide tiny labels.
        const slider = document.getElementById('text-distance');
        const overview = document.getElementById('simplified-overview');
        const label = b.getField('TEXT').getSvgRoot().querySelector('text');
        slider.value = '50'; slider.dispatchEvent(new Event('input')); slider.dispatchEvent(new Event('change'));
        if (getComputedStyle(label).display === 'none') throw Error('Always-visible labels stayed hidden');
        overview.checked = true; overview.dispatchEvent(new Event('change'));
        await new Promise(requestAnimationFrame);
        if (ws.bf6Viewport.overview.stats.active) throw Error('Overview hid always-visible labels');
        slider.value = '35'; slider.dispatchEvent(new Event('input'));
        overview.checked = false; overview.dispatchEvent(new Event('change'));
        if (getComputedStyle(label).display !== 'none') throw Error('Text cutoff did not apply');
        const icon = b.getSvgRoot().querySelector('image:not(.bf6-text-pic)');
        if (!icon || getComputedStyle(icon).display === 'none') throw Error('Text cutoff hid the block icon');
        overview.checked = true; overview.dispatchEvent(new Event('change'));
        await new Promise(requestAnimationFrame);
        if (!ws.bf6Viewport.overview.stats.active || ws.getCanvas().parentNode) throw Error('Overview did not detach drawing');
        if (JSON.stringify(BF6Blocks.saveWorkspace(ws)) !== before) throw Error('Overview changed exported program');
        const svg = ws.getParentSvg().getBoundingClientRect(), m = ws.getMetricsManager().getAbsoluteMetrics();
        return { id: b.id, x: svg.left + m.left + ws.scrollX + (xy.x + b.width / 2) * ws.scale,
          y: svg.top + m.top + ws.scrollY + (xy.y + b.height / 2) * ws.scale };
      });
      await page.mouse.click(target.x, target.y);
      await page.waitForFunction(id => Blockly.getSelected() && Blockly.getSelected().id === id && BF6UI.ws.scale >= .6, target.id);
      assert.equal(await page.evaluate(() => !!BF6UI.ws.getCanvas().parentNode), true, 'Overview failed to restore drawing');
      await page.evaluate(() => { const c = document.getElementById('simplified-overview'); c.checked = false; c.dispatchEvent(new Event('change')); });
      console.log('Mouse pan and overview selection passed');
    }
    await load({ blocks: { languageVersion: 0, blocks: [{ type: 'Number', id: 'replacement-only', fields: { NUM: 17 }, x: 20, y: 20 }] }, variables: [] });
    assert.equal(await page.evaluate(() => BF6UI.ws.getAllBlocks(false).length), 1, 'Second import retained previous blocks');
    assert.equal(await page.evaluate(() => BF6UI.ws.getAllVariables().length), 0, 'Second import retained previous variables');
    await page.evaluate(text => {
      testMessages = [];
      BF6Blocks.recv({ op: 'workspace', json: JSON.parse(text) });
    }, JSON.stringify(original));
    await page.waitForFunction(() => !BF6UI.loadingDoc && !BF6UI.applying, {}, { timeout: 240000 });
    await page.waitForTimeout(500);
    assert.deepEqual(await page.evaluate(() => testMessages.filter(m => ['replaceTop', 'deleteTop', 'variables', 'chunk'].includes(m.op)).map(m => m.op)), [], 'Website workspace load published edits');
    assert.deepEqual(errors, [], 'Browser page errors');
    fs.writeFileSync(path.join(out, 'browser-checks.json'), JSON.stringify(results, null, 2));
    console.log('Browser import, text, culling, variable theme, undo, export and replacement checks passed.');
  } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exit(1); });
