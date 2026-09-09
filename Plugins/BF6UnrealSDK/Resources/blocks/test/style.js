// Proof for the site-style apply path.
//
// The look of a Blockly workspace is four things: the renderer, its constant
// provider, the theme and the injected CSS. site_sync.js reads all four off the
// live page; editor.js turns that payload into a theme, a renderer and a colour
// pass. There is no live page in a test, so a synthetic payload is fed through
// the same apply path and every value is chased to where it has to land.
//
// Run by roundtrip.js. Also runs alone:  node style.js
//
// Cases, in the order they run:
//   1. a full payload: every field lands on the theme, the constants and the
//      path data, and the constants really do override the base renderer's
//   2. partial payloads: a missing section falls back and nothing throws
//   3. tolerance: the same payload written three other legal ways
//   4. per type colours reach an actual block after the definitions load

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');

// ---- Blockly, headless -----------------------------------------------------
// blockly_compressed is a UMD bundle. Node has no DOM, so the few globals it
// touches while loading are stubbed; nothing below renders.
function loadBlockly() {
  const src = fs.readFileSync(path.join(BLOCKS, 'vendor', 'blockly_compressed.js'), 'utf8');
  const elementStub = () => ({
    style: {}, classList: { add() {}, remove() {}, contains() { return false; } },
    setAttribute() {}, getAttribute() { return null; }, appendChild(c) { return c; },
    removeChild(c) { return c; }, addEventListener() {}, removeEventListener() {},
    getElementsByTagName() { return []; }, querySelector() { return null; },
    querySelectorAll() { return []; }, insertBefore(c) { return c; }, remove() {},
    childNodes: [], children: [], firstChild: null, parentNode: null, ownerDocument: null
  });
  const documentStub = {
    createElement: elementStub, createElementNS: elementStub,
    createTextNode: () => ({}), createDocumentFragment: elementStub,
    getElementById: () => null, getElementsByTagName: () => [],
    querySelector: () => null, querySelectorAll: () => [],
    addEventListener() {}, removeEventListener() {},
    head: elementStub(), body: elementStub(), documentElement: elementStub()
  };
  const ctx = {
    console, setTimeout, clearTimeout, setInterval, clearInterval,
    document: documentStub,
    navigator: { userAgent: 'node', platform: 'node' },
    location: { href: 'file:///' },
    performance: { now: () => Date.now() },
    module: { exports: {} }, exports: {}
  };
  ctx.window = ctx;
  ctx.self = ctx;
  ctx.globalThis = ctx;
  vm.createContext(ctx);
  vm.runInContext(src, ctx, { filename: 'blockly_compressed.js' });
  const Blockly = (ctx.module.exports && ctx.module.exports.Blocks) ? ctx.module.exports
    : (ctx.Blockly || ctx.exports);
  if (!Blockly || !Blockly.serialization) throw new Error('Blockly did not load headless');
  return Blockly;
}

// ---- the synthetic capture -------------------------------------------------
// Deliberately unlike every base renderer: if a value survives to the provider
// it can only have come from here. The path strings are the connection shapes,
// so they are the ones that matter most.
function fullPayload() {
  return {
    op: 'style',
    v: 1,
    source: 'live',
    capturedAt: '2026-09-06T00:00:00.000Z',
    url: 'https://portal.battlefield.com/experience/rules/blocks?id=test',
    blocklyVersion: '10.3.0',
    renderer: {
      name: 'portalRenderer',
      registryKey: 'portalRenderer',
      className: 'PortalRenderer',
      registered: ['geras', 'thrasos', 'zelos', 'minimalist', 'portalRenderer']
    },
    constants: {
      NOTCH_WIDTH: 22,
      NOTCH_HEIGHT: 6,
      NOTCH_OFFSET_LEFT: 19,
      CORNER_RADIUS: 2,
      TAB_WIDTH: 10,
      TAB_HEIGHT: 18,
      TAB_OFFSET_FROM_TOP: 7,
      TAB_VERTICAL_OVERLAP: 3,
      MIN_BLOCK_WIDTH: 18,
      MIN_BLOCK_HEIGHT: 26,
      FIELD_BORDER_RECT_RADIUS: 1,
      FIELD_BORDER_RECT_HEIGHT: 18,
      FIELD_TEXT_FONTSIZE: 12,
      FIELD_TEXT_FONTWEIGHT: '600',
      FIELD_TEXT_FONTFAMILY: 'BF6 Sans, Segoe UI, sans-serif',
      ADD_START_HATS: true,
      START_HAT_HEIGHT: 18,
      START_HAT_WIDTH: 96,
      EMPTY_INLINE_INPUT_PADDING: 6,
      MEDIUM_PADDING: 7,
      LARGE_PADDING: 11,
      INSERTION_MARKER_COLOUR: '#ff581a',
      INSERTION_MARKER_OPACITY: 0.4,
      CURSOR_COLOUR: '#70ebff',
      MARKER_COLOUR: '#adfd86',
      NOTCH: {
        type: 2, width: 22, height: 6,
        pathLeft: ' l 8,6  6,0  8,-6 ', pathRight: ' l -8,6  -6,0  -8,-6 '
      },
      PUZZLE_TAB: {
        type: 1, width: 10, height: 18,
        pathDown: ' c 0,12  -10,-9  -10,9  s 10,-3  10,9 ',
        pathUp: ' c 0,-12  -10,9  -10,-9  s 10,3  10,-9 '
      },
      START_HAT: { height: 18, width: 96, path: ' c 28,-18  68,-18  96,0 ' },
      INSIDE_CORNERS: {
        width: 2, height: 2, pathTop: 'a 2 2 0 0,0 -2,2 ', pathBottom: 'a 2 2 0 0,0 2,2 '
      },
      OUTSIDE_CORNERS: {
        topLeft: ' m 0,2 a 2 2 0 0,1 2,-2 ', topRight: 'a 2 2 0 0,1 2,2 ',
        bottomRight: 'a 2 2 0 0,1 -2,2 ', bottomLeft: 'a 2 2 0 0,1 -2,-2 ', rightHeight: 2
      }
    },
    theme: {
      name: 'portal_dark',
      blockStyles: {
        rules_blocks: { colourPrimary: '#8a4b9b', colourSecondary: '#6d3b7b', colourTertiary: '#4f2b59', hat: 'cap' },
        actions_blocks: { colourPrimary: '#2c7bb6', colourSecondary: '#236293', colourTertiary: '#194a70' },
        values_blocks: { colourPrimary: '#3d8b53', colourSecondary: '#316f43', colourTertiary: '#255333' }
      },
      categoryStyles: {
        'rules-category': { colour: '#8a4b9b' },
        'actions-category': { colour: '#2c7bb6' },
        'values-category': { colour: '#3d8b53' }
      },
      componentStyles: {
        workspaceBackgroundColour: '#0d0f10',
        toolboxBackgroundColour: '#111314',
        toolboxForegroundColour: '#aec0cc',
        flyoutBackgroundColour: '#15181a',
        flyoutForegroundColour: '#bfcad1',
        flyoutOpacity: 0.96,
        scrollbarColour: '#545e63',
        scrollbarOpacity: 0.6,
        insertionMarkerColour: '#ff581a',
        insertionMarkerOpacity: 0.4,
        markerColour: '#adfd86',
        cursorColour: '#70ebff',
        selectedGlowColour: '#ff581a',
        selectedGlowOpacity: 0.8,
        replacementGlowColour: '#ffffff',
        replacementGlowOpacity: 0.8
      },
      fontStyle: { family: 'BF6 Sans, Segoe UI, sans-serif', weight: '600', size: 12 },
      startHats: true
    },
    blockColours: {
      // a type that carries a style the theme knows
      ruleBlock: { style: 'rules_blocks', colour: '#8a4b9b', hat: 'cap' },
      // a type that sets its colour in code, which is what the theme cannot see
      EventPlayer: { colour: '#2c7bb6' },
      GetTeam: { colour: '#3d8b53' },
      // a type that is not defined anywhere: it must be skipped, not thrown on
      NoSuchBlockType: { colour: '#ffffff' }
    },
    css: {
      text: '.blocklyText { fill: #ededed; font-family: "BF6 Sans"; }\n' +
        '.blocklyToolboxDiv { background: #111314; }',
      sources: ['style tag 0', 'https://portal.battlefield.com/assets/index.css (44 rules)'],
      truncated: false,
      fonts: { blocklyText: { fontFamily: 'BF6 Sans', fontSize: '12px' } }
    },
    categoryIcons: {
      'class:toolbox-rules': { backgroundImage: 'url("data:image/svg+xml;base64,AAAA")', width: '16px', height: '16px' },
      'name:RULES': { svg: '<svg width="16" height="16"></svg>' },
      'row:RULES': { background: 'rgb(17, 19, 20)', colour: 'rgb(174, 192, 204)', selected: false }
    }
  };
}

// ---- the run ---------------------------------------------------------------
function run(Blockly, BF6) {
  const fails = [];
  const notes = [];
  let checks = 0;
  const check = (ok, what) => { checks++; if (!ok) fails.push(what); return ok; };
  const eq = (a, b, what) => check(
    JSON.stringify(a) === JSON.stringify(b),
    `${what}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`);

  // ---- 1. the full payload -------------------------------------------------
  const payload = fullPayload();
  const sum = BF6.setStyle(payload);

  check(sum.source === 'live', 'summary source is not live');
  check(sum.renderer === 'portalRenderer', 'summary lost the renderer name');
  check(sum.theme === 'portal_dark', 'summary lost the theme name');
  eq(sum.counts.constants, Object.keys(payload.constants).length, 'constant count');
  eq(sum.counts.blockStyles, 3, 'block style count');
  eq(sum.counts.categoryStyles, 3, 'category style count');
  eq(sum.counts.componentStyles, 16, 'component colour count');
  eq(sum.counts.blockColours, 4, 'typed colour count');
  eq(sum.counts.categoryIcons, 3, 'icon count');
  eq(sum.counts.cssBytes, payload.css.text.length, 'css byte count');
  eq(sum.missing, [], 'a full payload reported a missing section');

  // the theme spec: every field, unchanged
  const spec = BF6.themeSpec();
  check(!!spec, 'themeSpec returned nothing for a full payload');
  if (spec) {
    eq(spec.blockStyles, payload.theme.blockStyles, 'theme blockStyles');
    eq(spec.categoryStyles, payload.theme.categoryStyles, 'theme categoryStyles');
    eq(spec.componentStyles, payload.theme.componentStyles, 'theme componentStyles');
    eq(spec.fontStyle, payload.theme.fontStyle, 'theme fontStyle');
    eq(spec.startHats, true, 'theme startHats');
    // a real Blockly theme must accept it
    let theme = null;
    try {
      theme = Blockly.Theme.defineTheme('bf6test_' + Date.now(), {
        base: Blockly.Themes.Classic,
        blockStyles: spec.blockStyles, categoryStyles: spec.categoryStyles,
        componentStyles: spec.componentStyles, fontStyle: spec.fontStyle,
        startHats: spec.startHats
      });
    } catch (e) { check(false, 'Blockly refused the captured theme: ' + e.message); }
    if (theme) {
      eq(theme.blockStyles.rules_blocks.colourPrimary, '#8a4b9b', 'theme block style on the Theme object');
      eq(theme.getComponentStyle('workspaceBackgroundColour'), '#0d0f10', 'component colour on the Theme object');
      eq(theme.startHats, true, 'startHats on the Theme object');
    }
  }

  // ---- 1b. the renderer and its constants ---------------------------------
  // The site's renderer is not one our Blockly knows, so the closest base draws
  // and every captured measurement is forced onto its constant provider.
  const base = BF6.baseRendererFor('portalRenderer');
  check(base === 'geras', 'unknown renderer did not fall back to geras, got ' + base);

  const regName = BF6.installStyledRenderer('bf6test_renderer');
  check(regName === 'bf6test_renderer', 'installStyledRenderer did not register, got ' + regName);

  const RClass = BF6.rendererClass('bf6test_renderer');
  check(!!RClass, 'the registered renderer cannot be read back out of the registry');

  let cp = null, baseCp = null;
  if (RClass) {
    const inst = new RClass('bf6test_renderer');
    cp = inst.makeConstants_();
    cp.init();
    const BaseClass = BF6.rendererClass(base);
    const baseInst = new BaseClass(base);
    baseCp = baseInst.makeConstants_();
    baseCp.init();
  }

  if (cp) {
    // it is a real subclass of the base provider, not a bare object
    check(cp instanceof baseCp.constructor,
      'the forced provider is not a subclass of the base provider');

    // every captured scalar landed
    let scalarsOk = 0, scalarsBad = [];
    Object.keys(payload.constants).forEach(k => {
      const want = payload.constants[k];
      if (want === null || typeof want === 'object') return;
      if (cp[k] === want) scalarsOk++;
      else scalarsBad.push(`${k}: ${JSON.stringify(cp[k])} != ${JSON.stringify(want)}`);
    });
    check(scalarsBad.length === 0, 'constants that did not land: ' + scalarsBad.join('; '));
    notes.push(`  ${scalarsOk} captured scalars landed on the provider`);

    // every captured path object landed, and init did not rebuild over them
    ['NOTCH', 'PUZZLE_TAB', 'START_HAT', 'INSIDE_CORNERS', 'OUTSIDE_CORNERS'].forEach(k => {
      eq(cp[k], payload.constants[k], 'connection shape ' + k);
    });

    // and they really are overrides: the base renderer says something else
    const differs = ['NOTCH_WIDTH', 'CORNER_RADIUS', 'TAB_WIDTH', 'TAB_HEIGHT', 'START_HAT_WIDTH']
      .filter(k => baseCp[k] !== cp[k]);
    check(differs.length >= 4,
      'the forced constants match the base renderer, so nothing was proved to be overridden');
    notes.push(`  overrode the base renderer on ${differs.length} of 5 sampled measurements ` +
      `(base NOTCH_WIDTH ${baseCp.NOTCH_WIDTH} -> ${cp.NOTCH_WIDTH}, ` +
      `CORNER_RADIUS ${baseCp.CORNER_RADIUS} -> ${cp.CORNER_RADIUS})`);
    check(JSON.stringify(baseCp.NOTCH) !== JSON.stringify(cp.NOTCH),
      'the notch path was not overridden: connections would not line up');
  }

  // ---- 1c. the captured shapes must still WORK ----------------------------
  //
  // The capture crosses as JSON and JSON carries no functions. NOTCH and
  // PUZZLE_TAB survive because they are numbers and path strings. The zelos
  // dynamic shapes do not: HEXAGONAL, ROUNDED and SQUARED are almost entirely
  // methods, and the real site sends {"type":1,"isDynamic":true} with all of
  // them gone. Writing that over the provider leaves the renderer holding an
  // object it calls height() on, the throw lands inside Blockly's own flyout
  // code, and the only symptom a person sees is a toolbox category that will
  // not open. That shipped on 2026-09-06.
  {
    const husks = {
      NOTCH_WIDTH: 22,
      HEXAGONAL: { type: 1, isDynamic: true },
      ROUNDED: { type: 2, isDynamic: true },
      SQUARED: { type: 3, isDynamic: true },
      // and the same trap by a different name: all behaviour, nothing in JSON
      styleManager: {}
    };
    BF6.setStyle({ source: 'live', renderer: 'zelos', constants: husks });
    BF6.installStyledRenderer('bf6test_shapes');
    const SClass = BF6.rendererClass('bf6test_shapes');
    check(!!SClass, 'the shape-guard renderer did not register');
    if (SClass) {
      const scp = new SClass('bf6test_shapes').makeConstants_();
      scp.init();

      // the scalar still landed: the guard is about objects, not about giving up
      check(scp.NOTCH_WIDTH === 22, 'the guard threw away a scalar it should have taken');

      ['HEXAGONAL', 'ROUNDED', 'SQUARED'].forEach(name => {
        const s = scp[name];
        check(!!s, `${name} is missing from the provider`);
        check(s && typeof s.height === 'function',
          `${name}.height is ${s ? typeof s.height : 'absent'}, so rendering an inline socket throws`);
        check(s && typeof s.width === 'function',
          `${name}.width is ${s ? typeof s.width : 'absent'}`);
      });

      // and the path the renderer actually walks: a value connection asks the
      // provider for its shape and immediately measures it.
      const shapeWs = new Blockly.Workspace();
      Blockly.Blocks.bf6test_shape_probe = {
        init: function () {
          this.jsonInit({
            type: 'bf6test_shape_probe',
            message0: '%1 %2 %3',
            args0: [
              { type: 'input_value', name: 'B', check: ['Boolean'] },
              { type: 'input_value', name: 'N', check: ['Number'] },
              { type: 'input_value', name: 'A' }
            ],
            inputsInline: true,
            output: null
          });
        }
      };
      const pb = shapeWs.newBlock('bf6test_shape_probe');
      ['B', 'N', 'A'].forEach(nm => {
        const conn = pb.getInput(nm).connection;
        // A dynamic shape is measured against the block it sits in, so both
        // calls take the row height. That is exactly what the renderer does.
        let h = null, w = null, threw = '';
        try {
          const shape = scp.shapeFor(conn);
          h = shape.height(20);
          w = shape.width(20);
        } catch (e) { threw = String(e.message || e); }
        check(!threw, `measuring the ${nm} socket threw: ${threw}`);
        check(typeof h === 'number' && typeof w === 'number',
          `the ${nm} socket's shape measured ${w}x${h}`);
      });
      try { pb.dispose(false); shapeWs.dispose(); } catch (e) {}
      delete Blockly.Blocks.bf6test_shape_probe;

      // styleManager was all methods too, so it must not have been flattened
      check(!scp.styleManager || typeof scp.styleManager === 'object',
        'styleManager was replaced by something unusable');
    }
    notes.push('  dynamic shapes survive the capture: HEXAGONAL, ROUNDED and SQUARED still measure');
  }

  // ---- 1d. no hexagons, because the site has none -------------------------
  //
  // shapeFor is a METHOD, so it never crosses with the capture, and Blockly's
  // own answer for anything accepting a Boolean is a hexagon. Asked directly
  // on the live site with a real connection of each of seven types, it
  // returned ROUND every single time. That is the diamond sockets in our
  // editor against the rounded pills on the site. The measured answer is
  // carried in the capture and applied, so this checks the wiring rather than
  // a hardcoded shape.
  {
    const ROUND_EVERYWHERE = {
      shapes: {
        Boolean: { type: 2, isDynamic: true },
        Number: { type: 2, isDynamic: true },
        String: { type: 2, isDynamic: true },
        Player: { type: 2, isDynamic: true },
        any: { type: 2, isDynamic: true }
      },
      shapeNames: { 2: 'ROUND' }
    };
    BF6.setStyle({
      source: 'live', renderer: 'zelos',
      constants: { EMPTY_INLINE_INPUT_HEIGHT: 32 },
      socketRules: ROUND_EVERYWHERE
    });
    BF6.installStyledRenderer('bf6test_round');
    const RClass2 = BF6.rendererClass('bf6test_round');
    check(!!RClass2, 'the socket-shape renderer did not register');
    if (RClass2) {
      const rcp = new RClass2('bf6test_round').makeConstants_();
      rcp.init();
      const w = new Blockly.Workspace();
      Blockly.Blocks.bf6test_round_probe = {
        init: function () {
          this.jsonInit({
            type: 'bf6test_round_probe',
            message0: '%1 %2 %3',
            args0: [
              { type: 'input_value', name: 'B', check: ['Boolean'] },
              { type: 'input_value', name: 'P', check: ['Player'] },
              { type: 'input_value', name: 'A' }
            ],
            inputsInline: true, output: null
          });
        }
      };
      const rb = w.newBlock('bf6test_round_probe');
      const shapeOf = nm => {
        try { return rcp.shapeFor(rb.getInput(nm).connection).type; } catch (e) { return 'threw ' + e.message; }
      };
      const b = shapeOf('B'), p = shapeOf('P'), a = shapeOf('A');
      check(b === p && p === a, `sockets differ by type: Boolean=${b} Player=${p} any=${a}`);
      check(b === 2, `a Boolean socket is shape ${b}, the site draws it 2 (ROUND)`);
      // A non-value connection is untouched and still goes to the base: only
      // sockets are ruled here, and a notch is not a socket.
      Blockly.Blocks.bf6test_stack_probe = {
        init: function () {
          this.jsonInit({
            type: 'bf6test_stack_probe', message0: 'stack',
            previousStatement: null, nextStatement: null
          });
        }
      };
      const sb = w.newBlock('bf6test_stack_probe');
      let notch = null, notchThrew = '';
      try { notch = rcp.shapeFor(sb.previousConnection); }
      catch (e) { notchThrew = String(e.message || e); }
      check(!notchThrew, `the override broke a statement notch: ${notchThrew}`);
      check(notch && notch.type !== 2, 'a statement notch was turned into a socket shape');
      try { sb.dispose(false); rb.dispose(false); w.dispose(); } catch (e) {}
      delete Blockly.Blocks.bf6test_stack_probe;
      delete Blockly.Blocks.bf6test_round_probe;
      notes.push('  every value socket takes the one shape the site measured, hexagons included');
    }
  }

  // ---- 1e. the row spacing and the room a socket keeps --------------------
  //
  // Both live in methods on the render info, so neither crossed with the
  // capture and ours kept Blockly's answer: 12 where the site uses 24, on both
  // sides of every socket on every block, and no room at all for the symbol
  // drawn inside a socket. Measured element by element on both sides; applied
  // from the capture, never from a number that looked right.
  {
    const MEASURED = {
      spacing: {
        // INLINE, not INPUT: the three kinds of input are named apart now,
        // because the site spaces them differently and one name for all
        // three let the commoner reading win and made the other 20px wrong.
        'S|FIELD>INLINE': { px: 24, of: 6, agreed: 6 },
        'S|INLINE>INLINE': { px: 24, of: 4, agreed: 4 },
        'S|EDGE>STATEMENT': { px: 12, of: 6, agreed: 6 },
        'S|EDGE>INLINE': { px: 32, of: 5, agreed: 5 },
        'S|FIELD>FIELD': { px: 24, of: 3, agreed: 3 },
        'S|EDGE>FIELD': { px: 12, of: 5, agreed: 5 },
        'V|EDGE>FIELD': { px: 0, of: 9, agreed: 9 },
        // measured once only: alignment padding leaking in, must be ignored
        'S|FIELD>EDGE': { px: 117.45, of: 1, agreed: 1 }
      },
      iconRoom: 32 * 10 / 13
    };
    BF6.setStyle({
      source: 'live', renderer: 'zelos',
      constants: { EMPTY_INLINE_INPUT_HEIGHT: 32 },
      socketRules: MEASURED
    });
    BF6.installStyledRenderer('bf6test_layout');
    const ok = BF6.installLayoutRules();
    check(ok || BF6.layoutRulesInstalled(), 'the layout rules did not install');

    const RI = Blockly.zelos.RenderInfo;
    check(!!(RI && RI.prototype.getInRowSpacing_), 'there is no row spacing method to rule');
    if (RI && RI.prototype.getInRowSpacing_) {
      const T = Blockly.blockRendering.Types;
      const fakeField = { type: T.FIELD };
      const fakeInput = { type: T.INLINE_INPUT | T.INPUT };
      const self = { block_: { outputConnection: null },
        constants_: { MEDIUM_PADDING: 12, LARGE_PADDING: 24 } };
      const ask = (a, b) => {
        try { return RI.prototype.getInRowSpacing_.call(self, a, b); } catch (e) { return 'threw ' + e.message; }
      };
      check(ask(fakeField, fakeInput) === 24,
        `a field next to a socket got ${ask(fakeField, fakeInput)}, the site measured 24`);
      check(ask(fakeInput, fakeInput) === 24,
        `two sockets side by side got ${ask(fakeInput, fakeInput)}, the site measured 24`);
      // AND THE THREE KINDS OF INPUT MUST NOT SHARE A RULE.
      //
      // A row starting with a statement input begins at 12 and one starting
      // with an inline input at 32. While both were called INPUT the table
      // held one number for both, 13 readings against 11, and the losing
      // side was 20px out on every row it appeared in.
      const fakeStatement = { type: T.STATEMENT_INPUT | T.INPUT };
      const leadStatement = ask(null, fakeStatement);
      const leadInline = ask(null, fakeInput);
      check(leadStatement === 12,
        `a row starting with a statement input got ${leadStatement}, the site measured 12`);
      check(leadInline === 32,
        `a row starting with an inline input got ${leadInline}, the site measured 32`);
      check(leadStatement !== leadInline,
        'the two kinds of input are sharing one spacing rule again');
      check(ask(fakeField, fakeField) === 24,
        `two fields got ${ask(fakeField, fakeField)}, the site measured 24`);
      check(ask(null, fakeField) === 12,
        `the start of a row got ${ask(null, fakeField)}, the site measured 12`);
      // the once-only reading must NOT have become a rule
      const edge = ask(fakeField, null);
      check(edge !== 117.45,
        'a spacing the site only produced once was taken for a rule');
      // THE BLOCK'S KIND IS PART OF THE ANSWER. Measured on the site, the
      // leading spacer is 12 on a statement block and 0 on a value block.
      const asValue = { block_: { outputConnection: {} },
        constants_: { MEDIUM_PADDING: 12, LARGE_PADDING: 24 } };
      const leadV = RI.prototype.getInRowSpacing_.call(asValue, null, fakeField);
      check(leadV === 0,
        `a value block's leading spacer got ${leadV}, the site measured 0`);
      check(ask(null, fakeField) === 12,
        'a statement block lost its leading spacer to the value block rule');
      notes.push('  row spacing follows the measured table, per block kind, and a one-off reading is not a rule');
    }
  }

  // ---- 1f. the row an ICON sits in ----------------------------------------
  //
  // Two Text blocks measured exactly 12px narrower than the site's, and they
  // are the only two in the project carrying a comment. Their words contain a
  // '?', which looks like the cause and is not: that glyph measures 7.2px on
  // both sides, to the hundredth of a pixel.
  //
  // The cause was that nothing measured had an icon at all. The type list the
  // layout probe walks picks one block per type and none of them carried one,
  // so the row an icon sits in was never seen on either side, the table
  // learned no rule for it, and our icon rows kept Blockly's own spacing - 12
  // where the site's is 24, once, on one row.
  //
  // Two things have to hold for that to stay fixed, and both are checked:
  // the table must be able to carry an ICON rule at all, and a rule the site
  // gave only once must still be refused, because that guard is what made the
  // shortfall visible instead of silently applying a guess.
  {
    const T = Blockly.blockRendering.Types;
    const WITH_ICONS = {
      spacing: {
        'V|ICON>FIELD': { px: 24, of: 2, agreed: 2 },
        'S|ICON>FIELD': { px: 24, of: 4, agreed: 4 },
        'S|EDGE>ICON': { px: 12, of: 4, agreed: 4 },
        // seen once only: must not become a rule, however plausible it looks
        'V|EDGE>ICON': { px: 99, of: 1, agreed: 1 }
      },
      iconRoom: 32 * 10 / 13
    };
    BF6.setStyle({
      source: 'live', renderer: 'zelos',
      constants: { EMPTY_INLINE_INPUT_HEIGHT: 32 },
      socketRules: WITH_ICONS
    });
    BF6.installStyledRenderer('bf6test_iconrow');
    BF6.installLayoutRules();

    const RI = Blockly.zelos.RenderInfo;
    check(typeof T.ICON === 'number', 'Blockly has no ICON measurable to key a rule on');
    if (RI && RI.prototype.getInRowSpacing_ && typeof T.ICON === 'number') {
      const icon = { type: T.ICON };
      const field = { type: T.FIELD };
      const consts = { MEDIUM_PADDING: 12, LARGE_PADDING: 24 };
      const asValue = { block_: { outputConnection: {} }, constants_: consts };
      const asStatement = { block_: { outputConnection: null }, constants_: consts };
      const ask = (self, a, b) => {
        try { return RI.prototype.getInRowSpacing_.call(self, a, b); }
        catch (e) { return 'threw ' + e.message; }
      };

      const v = ask(asValue, icon, field);
      check(v === 24,
        `a value block's icon got ${v} before its first field, the site measured 24`);
      const st = ask(asStatement, icon, field);
      check(st === 24,
        `a statement block's icon got ${st} before its first field, the site measured 24`);
      const lead = ask(asStatement, null, icon);
      check(lead === 12,
        `the spacer before a statement block's icon got ${lead}, the site measured 12`);

      // and the once-only reading must still be refused
      const once = ask(asValue, null, icon);
      check(once !== 99,
        'an icon spacing the site produced only once was taken for a rule');
      notes.push('  an icon row is spaced from the measured table, and a once-only icon reading is still refused');
    }
  }

  // a renderer our Blockly already knows is used as it is
  BF6.setStyle({ source: 'live', renderer: 'zelos' });
  check(BF6.installStyledRenderer('bf6test_known') === 'zelos',
    'a renderer we already have should be used by name, not rebuilt');

  // ---- 2. partial payloads -------------------------------------------------
  // The offline default is extracted from the site's public bundle and may not
  // recover every section. Each of these must apply what it has and say what it
  // is missing, without throwing.
  const partials = [
    ['theme only', { source: 'default', theme: fullPayload().theme }],
    ['constants only', { source: 'default', constants: fullPayload().constants }],
    ['css only', { source: 'default', css: fullPayload().css }],
    ['colours only', { source: 'default', blockColours: fullPayload().blockColours }],
    ['renderer only', { source: 'default', renderer: 'portalRenderer' }],
    ['empty object', { source: 'default' }],
    ['nothing at all', null]
  ];
  partials.forEach(([what, p]) => {
    let s = null, threw = '';
    try {
      s = BF6.setStyle(p);
      BF6.themeSpec();
      BF6.installStyledRenderer('bf6test_partial');
      BF6.applyBlockColours();
      BF6.styleSummaryLine(s);
    } catch (e) { threw = String(e.message || e); }
    check(!threw, `partial payload "${what}" threw: ${threw}`);
    if (s && p && p.theme) check(s.missing.indexOf('theme') < 0, `"${what}" lost its theme`);
    if (s && p && !p.theme) check(s.missing.indexOf('theme') >= 0, `"${what}" did not report a missing theme`);
  });
  // nothing captured: the caller must be told to keep its own renderer
  BF6.setStyle(null);
  check(BF6.installStyledRenderer('bf6test_none') === null,
    'with nothing captured, installStyledRenderer must return null so the caller keeps geras');
  check(BF6.styleSummary().source === 'none', 'a null payload should read as source none');
  check(BF6.themeSpec() === null, 'a null payload should give no theme spec');

  // ---- 3. the same capture, written the other legal ways -------------------
  // Three writers produce this file: the live page, the cache and the offline
  // extractor. Anything that means the same thing must normalise to one shape.
  const alt = {
    source: 'default',
    renderer: 'portalRenderer',                       // a bare string
    constants: {                                      // split into scalars and paths
      scalars: { NOTCH_WIDTH: 22, CORNER_RADIUS: 2, ADD_START_HATS: true },
      paths: { NOTCH: fullPayload().constants.NOTCH }
    },
    blockStyles: fullPayload().theme.blockStyles,     // theme spread over the top level
    categoryStyles: fullPayload().theme.categoryStyles,
    componentStyles: fullPayload().theme.componentStyles,
    themeName: 'portal_dark',
    startHats: true,
    blockColours: { EventPlayer: '#2c7bb6' },         // a bare colour string
    css: '.blocklyText { fill: #ededed; }',           // css as one string
    categoryIcons: { 'class:toolbox-rules': 'url("data:image/svg+xml;base64,AAAA")' }
  };
  const altSum = BF6.setStyle(alt);
  const norm = BF6.state.style;
  eq(altSum.renderer, 'portalRenderer', 'renderer written as a bare string');
  eq(norm.constants.NOTCH_WIDTH, 22, 'constants written under scalars');
  eq(norm.constants.NOTCH, fullPayload().constants.NOTCH, 'constants written under paths');
  eq(norm.theme.name, 'portal_dark', 'theme spread over the top level');
  eq(norm.theme.startHats, true, 'startHats spread over the top level');
  eq(norm.blockColours.EventPlayer, { colour: '#2c7bb6' }, 'a colour written as a bare string');
  eq(norm.css.text, '.blocklyText { fill: #ededed; }', 'css written as one string');
  eq(norm.categoryIcons['class:toolbox-rules'],
    { backgroundImage: 'url("data:image/svg+xml;base64,AAAA")' }, 'an icon written as a bare url');

  // ---- 4. per type colours reach a real block ------------------------------
  // Run last, because it wraps the definitions' init functions.
  BF6.setStyle(fullPayload());
  const applied = BF6.applyBlockColours();
  check(applied === 3, `expected 3 of 4 typed colours applied (one type is not defined), got ${applied}`);

  const ws = new Blockly.Workspace();
  const wantColours = { EventPlayer: '#2c7bb6', GetTeam: '#3d8b53' };
  Object.keys(wantColours).forEach(type => {
    if (!Blockly.Blocks[type]) { check(false, `${type} is not defined, so the colour cannot be proved`); return; }
    let b = null;
    try { b = ws.newBlock(type); } catch (e) { check(false, `${type} would not instantiate: ${e.message}`); return; }
    const got = String(b.getColour() || '').toLowerCase();
    check(got === wantColours[type], `${type} came out ${got}, wanted ${wantColours[type]}`);
    try { b.dispose(false); } catch (e) {}
  });
  // a type carrying a style the theme knows must not throw on a headless
  // workspace, and must still end up coloured
  if (Blockly.Blocks.ruleBlock) {
    let b = null, threw = '';
    try { b = ws.newBlock('ruleBlock'); } catch (e) { threw = String(e.message || e); }
    check(!threw, 'a styled type threw on a headless workspace: ' + threw);
    if (b) {
      check(!!b.getColour(), 'a styled type came out with no colour at all');
      try { b.dispose(false); } catch (e) {}
    }
  }
  // applying twice must not nest the wrappers
  const again = BF6.applyBlockColours();
  check(again === 3, `applying the colours twice changed the count: ${again}`);
  if (Blockly.Blocks.EventPlayer) {
    const b = ws.newBlock('EventPlayer');
    check(String(b.getColour() || '').toLowerCase() === '#2c7bb6',
      'the colour did not survive a second apply');
    try { b.dispose(false); } catch (e) {}
  }
  try { ws.dispose(); } catch (e) {}

  console.log('style checks      ', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log(n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run, loadBlockly, fullPayload };

if (require.main === module) {
  const Blockly = loadBlockly();
  const BF6 = require(path.join(BLOCKS, 'editor.js'));
  const TYPES = require(path.join(BLOCKS, 'vendor', 'types_fallback.js'));
  BF6.attach(Blockly);
  BF6.installFallback(TYPES, BF6.observe({ blocks: { blocks: [] } }));
  const r = run(Blockly, BF6);
  console.log(r.ok ? 'STYLE OK' : 'STYLE FAILED');
  process.exit(r.ok ? 0 : 1);
}
