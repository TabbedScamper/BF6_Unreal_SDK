// Real browser checks for personal colors, help parity and unchanged programs.
const fs = require('node:fs'), path = require('node:path'), assert = require('node:assert/strict');
const { pathToFileURL } = require('node:url');
const { chromium } = require(process.env.BF6_PLAYWRIGHT || 'playwright');
const root = path.resolve(__dirname, '..');
(async () => {
  const browser = await chromium.launch({headless: true, executablePath: process.env.BF6_CHROMIUM || undefined,
    args: ['--allow-file-access-from-files']});
  try {
    const page = await browser.newPage({viewport: {width: 1600, height: 1000}}), errors = [];
    page.on('pageerror', e => errors.push(e.message));
    await page.goto(pathToFileURL(path.join(root, 'editor.html')).href);
    await page.waitForFunction(() => window.BF6UI && BF6UI.ws);
    const read = name => JSON.parse(fs.readFileSync(path.join(root, 'offline', name + '.json'), 'utf8'));
    await page.evaluate(defs => BF6Blocks.recv(defs), {op:'defs', definitions:read('definitions'), synthesized:read('definitions_synth'),
      toolbox:read('toolbox'), options:read('options'), tooltips:read('tooltips'), icons:read('icons'), helpUrls:read('help_urls')});
    const style = read('style'), pack = read('mirror_icons');
    Object.assign(style, {mirrorIcons:pack.categoryIcons, mirrorValueTypeIcons:pack.valueTypeIcons,
      blockImages:pack.blockImages, fontCss:pack.fontCss, fontAliases:pack.fontAliases, fonts:pack.fontFamilies});
    await page.evaluate(s => BF6Blocks.recv(s), style);
    await page.evaluate(() => document.fonts.ready);
    const baseline = await page.evaluate(async () => {
      BF6Blocks.recv({op:'prefs', values:{shelfOpen:'false', categoryColors:'{}'}});
      window.messages = []; window.ue = {bf6blocks: {msg: s => messages.push(JSON.parse(s))}};
      Blockly.Events.disable();
      try {
        ['variableReferenceBlock', 'Number', 'If'].forEach((type, i) => {
          const b = BF6UI.ws.newBlock(type, 'color-' + i); b.initSvg(); b.render(); b.moveBy(100, 100 + i * 140);
        });
      } finally { Blockly.Events.enable(); }
      await Blockly.renderManagement.finishQueuedRenders();
      BF6UI.ws.setScale(1); BF6UI.ws.scroll(0,0); BF6UI.ws.bf6Viewport.invalidate();
      window.colorProgram = JSON.stringify(BF6Blocks.saveWorkspace(BF6UI.ws));
      return {color: BF6UI.ws.getBlockById('color-0').getColour(), valueColor:BF6UI.ws.getBlockById('color-1').getColour()};
    });
    // Enter through an actual block's right-click menu.
    const rect = await page.evaluate(() => {
      const r = BF6UI.ws.getBlockById('color-0').getSvgRoot().getBoundingClientRect();
      return {x:r.x + 70, y:r.y + 15};
    });
    await page.mouse.click(rect.x, rect.y, {button:'right'});
    await page.getByText('Change Variables color...', {exact:true}).click();
    assert.equal(await page.locator('#category-color-group').inputValue(), 'variable-block-style');
    const wheel = page.getByRole('slider', {name:'Category hue'});
    await wheel.focus(); await page.keyboard.press('Home');
    assert.equal(await wheel.getAttribute('aria-valuenow'), '0');
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), baseline.color, 'Preview recolored the large workspace');
    await page.getByRole('button', {name:'Apply', exact:true}).click();
    await page.evaluate(() => Blockly.renderManagement.finishQueuedRenders());
    const red = await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour());
    assert.notEqual(red, baseline.color);
    assert.equal(await page.evaluate(color => {
      const probe=document.createElement('span'); probe.style.color=color;
      return getComputedStyle(BF6UI.ws.getBlockById('color-0').getSvgRoot().querySelector('.blocklyPath')).fill === probe.style.color;
    }, red), true, 'Rendered block fill did not follow the preference');
    assert.equal(await page.evaluate(() => BF6UI.ws.getTheme().categoryStyles['bf6-variables-category'].colour), '#8c1d1d', 'Variable toolbox border did not follow its category');
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-1').getColour()), baseline.valueColor, 'Unrelated category recolored');
    assert.equal(await page.evaluate(() => JSON.stringify(BF6Blocks.saveWorkspace(BF6UI.ws)) === colorProgram), true);
    const preference = await page.evaluate(() => messages.find(m => m.op === 'pref' && m.name === 'categoryColors'));
    assert.equal(JSON.parse(preference.value)['variable-block-style'], 0);
    assert.deepEqual(await page.evaluate(() => messages.filter(m => ['replaceTop','deleteTop','variables','chunk'].includes(m.op))), []);
    await page.evaluate(() => BF6UI.showPane('legend'));
    assert.equal(await page.locator('[data-color-family="variable-block-style"] .bf6-family-swatch').evaluate(e => e.style.backgroundColor),
      await page.evaluate(red => {const e=document.createElement('span'); e.style.backgroundColor=red; return e.style.backgroundColor;}, red));
    assert.equal(await page.evaluate(() => BF6UI.legendWs.every(w => w.getTheme().blockStyles['variable-block-style'].colourPrimary === BF6UI.ws.getTheme().blockStyles['variable-block-style'].colourPrimary)), true);
    // A fresh block and a new captured Portal style must honor personal colors.
    await page.evaluate(s => BF6Blocks.recv(s), style);
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), red);
    assert.equal(await page.evaluate(() => JSON.stringify(BF6Blocks.saveWorkspace(BF6UI.ws)) === colorProgram), true);
    await page.evaluate(() => {
      const known = new Set(BF6UI.ws.getAllVariables().map(v => v.getId()));
      Blockly.Events.disable();
      try {const b=BF6UI.ws.newBlock('variableReferenceBlock'); b.initSvg(); b.render(); window.newColor=b.getColour(); b.dispose();}
      finally {
        BF6UI.ws.getAllVariables().filter(v => !known.has(v.getId())).forEach(v => BF6UI.ws.getVariableMap().deleteVariableById(v.getId()));
        Blockly.Events.enable();
      }
    });
    assert.equal(await page.evaluate(() => newColor), red);
    await page.locator('#btnMore').click(); await page.getByRole('button', {name:'Block colors...', exact:true}).click();
    await page.locator('#category-color-hue').fill('220');
    await page.keyboard.press('Escape');
    assert.equal(await page.locator('#category-color-dialog').count(), 0);
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), red, 'Cancel saved a preview');
    await page.evaluate(value => BF6Blocks.recv({op:'prefs',values:{categoryColors:value}}), preference.value);
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), red);
    await page.locator('#btnMore').click(); await page.getByRole('button', {name:'Block colors...', exact:true}).click();
    await page.getByRole('button', {name:'Reset all', exact:true}).click();
    await page.getByRole('button', {name:'Apply', exact:true}).click();
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), baseline.color);
    assert.equal(await page.evaluate(() => JSON.stringify(BF6Blocks.saveWorkspace(BF6UI.ws)) === colorProgram), true);
    // Invalid settings and legacy migration are deterministic.
    assert.deepEqual(await page.evaluate(() => BF6BlockColors.validate('{"variable-block-style":999,"rule-block-style":"red","__proto__":2}')), {});
    await page.evaluate(() => BF6Blocks.recv({op:'prefs',values:{redVariables:'true'}}));
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), red);
    await page.evaluate(() => BF6Blocks.recv({op:'prefs',values:{redVariables:'true',categoryColors:'{}'}}));
    assert.equal(await page.evaluate(() => BF6UI.ws.getBlockById('color-0').getColour()), baseline.color);
    assert.deepEqual(errors, []);
    if (process.argv[2]) {await page.locator('#btnMore').click(); await page.getByRole('button', {name:'Block colors...', exact:true}).click(); await page.screenshot({path:process.argv[2]});}
    console.log('PASS: right-click hue wheel, preview/cancel, native preference messages, help colors, style recapture, new blocks, reset, legacy settings, unchanged export.');
  } finally {await browser.close();}
})().catch(e => {console.error(e);process.exit(1);});
