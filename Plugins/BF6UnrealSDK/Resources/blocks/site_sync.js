// ============================================================================
// BF6 BLOCKS - the half that lives on portal.battlefield.com.
//
// Injected into every page the Portal panel loads. On the rules/blocks page it
// does three jobs:
//   1. captures what the site knows - block definitions, the toolbox with its
//      pre-filled flyout blocks, the theme, the renderer, tooltips, icons,
//      help URLs and the workspace options - and hands it to the tool once per
//      session, so the tool's own editor looks and helps exactly like the site;
//   2. applies edits made in the tool to the site's workspace, block by block,
//      keeping ids and firing the site's own change events so its Save button
//      lights up;
//   3. reports edits made here back to the tool.
//
// It never signs in, never posts, and never presses Save on its own. Saving
// stays a button the user chooses to press.
// ============================================================================
(function () {
  'use strict';
  if (window.BF6SiteSync) return;              // injected on every load: once is enough

  var ORIGIN = 'site';
  var S = {
    ready: false,
    seqOut: 0,
    applied: {},          // seq we applied, so our own echo is ignored
    units: {},
    captured: false,
    debounce: null,
    dirty: {},
    chunks: {}
  };

  function log(text, level) {
    try { console.log('[BF6 Blocks] ' + text); } catch (e) {}
    send({ op: 'log', level: level || 'log', text: String(text) });
  }

  // ---- the bridge ---------------------------------------------------------
  function bridge() {
    return (window.ue && window.ue.bf6blocks) ? window.ue.bf6blocks : null;
  }
  function rawSend(obj) {
    var b = bridge();
    if (!b || !b.msg) return false;
    try { b.msg(JSON.stringify(obj)); return true; }
    catch (e) { try { console.warn('[BF6 Blocks] send failed', e); } catch (e2) {} return false; }
  }
  // Big captures go across in pieces: one giant string argument is where a
  // CEF binding gives up.
  var CHUNK = 256 * 1024;
  function send(obj) {
    obj.from = ORIGIN;
    var text = JSON.stringify(obj);
    if (text.length <= CHUNK) return rawSend(obj);
    var id = 'c' + (++S.seqOut) + '_' + Date.now();
    var total = Math.ceil(text.length / CHUNK);
    for (var i = 0; i < total; i++) {
      rawSend({ from: ORIGIN, op: 'chunk', cid: id, i: i, n: total,
        part: text.substr(i * CHUNK, CHUNK) });
    }
    return true;
  }

  // ---- Blockly handles ----------------------------------------------------
  function BL() { return window._Blockly || window.Blockly || null; }
  function WS() {
    var B = BL();
    try { return B && B.getMainWorkspace ? B.getMainWorkspace() : null; } catch (e) { return null; }
  }

  // ---- definitions, captured from the site's own console.debug ------------
  // The bundle announces its block definitions once, at load. Hooking the
  // console is the only place the object is handed out whole.
  var capturedDefs = null;
  var RELOAD_KEY = 'bf6.defs.reloaded';
  function reloadedForDefs() {
    try { return sessionStorage.getItem(RELOAD_KEY) === '1'; } catch (e) { return true; }
  }
  function markReloadedForDefs() {
    try { sessionStorage.setItem(RELOAD_KEY, '1'); } catch (e) {}
  }
  (function hookConsole() {
    var orig = console.debug;
    console.debug = function () {
      try {
        if (arguments.length >= 2 && String(arguments[0]).indexOf('Frostbite Block Definitions') >= 0) {
          capturedDefs = arguments[1];
        }
      } catch (e) {}
      return orig.apply(console, arguments);
    };
  })();

  // ---- toolbox ------------------------------------------------------------
  // Toolbox BLOCK entries carry a DOM node (blockxml) holding the ready made
  // block with its default children. The site's bundle has no Blockly.Xml, so
  // the node is converted to serialization JSON here.
  var MUTATION_KEYS = {
    isongoingevent: ['isOngoingEvent', 'bool'],
    subroutinename: ['subroutineName', 'text'],
    isobjectvar: ['isObjectVar', 'bool'],
    elseif: ['elseif', 'int'],
    'else': ['else', 'int']
  };
  var unknownMutations = {};

  function xmlToState(node) {
    if (!node || node.nodeType !== 1) return null;
    var tag = (node.tagName || '').toLowerCase();
    if (tag !== 'block' && tag !== 'shadow') return null;
    var st = { type: node.getAttribute('type') };
    var kids = node.childNodes;
    for (var i = 0; i < kids.length; i++) {
      var c = kids[i];
      if (c.nodeType !== 1) continue;
      var ct = (c.tagName || '').toLowerCase();
      if (ct === 'field') {
        st.fields = st.fields || {};
        var fname = c.getAttribute('name');
        var id = c.getAttribute('id');
        st.fields[fname] = id ? { id: id } : c.textContent;
      } else if (ct === 'value' || ct === 'statement') {
        var child = null, shadow = null;
        for (var j = 0; j < c.childNodes.length; j++) {
          var g = c.childNodes[j];
          if (g.nodeType !== 1) continue;
          if ((g.tagName || '').toLowerCase() === 'shadow') shadow = xmlToState(g);
          else if ((g.tagName || '').toLowerCase() === 'block') child = xmlToState(g);
        }
        st.inputs = st.inputs || {};
        st.inputs[c.getAttribute('name')] = {};
        if (child) st.inputs[c.getAttribute('name')].block = child;
        if (shadow) st.inputs[c.getAttribute('name')].shadow = shadow;
      } else if (ct === 'next') {
        for (var k = 0; k < c.childNodes.length; k++) {
          var nb = c.childNodes[k];
          if (nb.nodeType === 1 && (nb.tagName || '').toLowerCase() === 'block') {
            st.next = { block: xmlToState(nb) };
          }
        }
      } else if (ct === 'mutation') {
        var extra = {};
        var attrs = c.attributes || [];
        for (var a = 0; a < attrs.length; a++) {
          var key = attrs[a].name.toLowerCase();
          var map = MUTATION_KEYS[key];
          if (!map) { unknownMutations[st.type + '.' + key] = attrs[a].value; continue; }
          if (map[1] === 'bool') extra[map[0]] = (attrs[a].value === 'true');
          else if (map[1] === 'int') extra[map[0]] = parseInt(attrs[a].value, 10) || 0;
          else extra[map[0]] = attrs[a].value;
        }
        if (Object.keys(extra).length) st.extraState = extra;
      }
    }
    return st;
  }

  function convertToolbox(node) {
    if (!node || typeof node !== 'object') return node;
    var out = {};
    for (var k in node) {
      if (!Object.prototype.hasOwnProperty.call(node, k)) continue;
      if (k === 'contents') {
        out.contents = (node.contents || []).map(convertToolbox);
      } else if (k === 'blockxml') {
        var v = node[k];
        var st = null;
        try {
          if (v && v.nodeType) st = xmlToState(v);
          else if (typeof v === 'string' && v.indexOf('<') === 0) {
            st = xmlToState(new DOMParser().parseFromString(v, 'text/xml').documentElement);
          }
        } catch (e) { st = null; }
        if (st) out.blockstate = st;
      } else {
        out[k] = node[k];
      }
    }
    return out;
  }

  // ---- what a block looks like, one headless instance at a time -----------
  function probeTypes(B, types) {
    var probe = new B.Workspace();
    var tooltips = {}, icons = {}, help = {}, failures = [];
    types.forEach(function (type) {
      var b = null;
      try { b = probe.newBlock(type); } catch (e) { failures.push(type); return; }
      try {
        var tip = b.tooltip;
        if (typeof tip === 'function') tip = tip.call(b);
        if (tip) tooltips[type] = String(tip);
      } catch (e) {}
      try { if (b.helpUrl) help[type] = String(typeof b.helpUrl === 'function' ? b.helpUrl.call(b) : b.helpUrl); } catch (e) {}
      try {
        (b.inputList || []).forEach(function (inp) {
          (inp.fieldRow || []).forEach(function (f) {
            if (f && typeof f.getSrc === 'function') {
              var src = f.getSrc();
              if (src) { icons[type] = icons[type] || {}; icons[type][f.name || 'ICON'] = src; }
            }
          });
        });
      } catch (e) {}
      try { b.dispose(false); } catch (e) {}
    });
    try { probe.dispose(); } catch (e) {}
    return { tooltips: tooltips, icons: icons, help: help, failures: failures };
  }

  // The toolbox categories are styled with CSS classes (cssconfig.icon). The
  // picture behind each one is read off the live DOM so the tool can show the
  // same icons without shipping a copy of the site's stylesheet.
  function categoryIcons() {
    var out = {};
    try {
      var nodes = document.querySelectorAll('[class*="toolbox-"], .blocklyTreeIcon, [class*="categoryIcon"]');
      for (var i = 0; i < nodes.length; i++) {
        var el = nodes[i];
        var cs = window.getComputedStyle(el);
        var bg = cs && cs.backgroundImage;
        if (!bg || bg === 'none') continue;
        var cls = (el.className && el.className.baseVal !== undefined) ? el.className.baseVal : String(el.className || '');
        cls.split(/\s+/).forEach(function (c) {
          if (c && c.indexOf('toolbox-') === 0 && !out[c]) out[c] = bg;
        });
      }
    } catch (e) {}
    return out;
  }

  function contextMenuIds(B) {
    var ids = [];
    try {
      var reg = B.ContextMenuRegistry && B.ContextMenuRegistry.registry;
      if (reg) {
        var store = reg.registry_ || reg.registry || reg.items_ || null;
        if (store && typeof store.forEach === 'function' && store.size !== undefined) {
          store.forEach(function (v, k) { ids.push(k); });
        } else if (store) {
          for (var k in store) if (Object.prototype.hasOwnProperty.call(store, k)) ids.push(k);
        }
      }
    } catch (e) {}
    return ids;
  }

  function helpButton() {
    var out = [];
    try {
      var links = document.querySelectorAll('a[href]');
      for (var i = 0; i < links.length && out.length < 12; i++) {
        var t = (links[i].textContent || '').trim().toLowerCase();
        var h = links[i].getAttribute('href') || '';
        if (t.indexOf('help') >= 0 || t.indexOf('doc') >= 0 || t.indexOf('guide') >= 0 ||
          h.indexOf('help') >= 0 || h.indexOf('docs') >= 0) {
          out.push({ text: (links[i].textContent || '').trim().slice(0, 40), href: links[i].href });
        }
      }
    } catch (e) {}
    return out;
  }

  function scanExamples(tb) {
    var hits = [];
    (function walk(n, path) {
      if (!n || typeof n !== 'object') return;
      var nm = n.name || n.text || n.displayName || '';
      if (nm && /example|template|sample/i.test(nm)) hits.push({ path: path, name: nm, node: n });
      (n.contents || []).forEach(function (c, i) { walk(c, path + '/' + (nm || i)); });
    })(tb, '');
    return hits;
  }

  function workspaceOptions(ws) {
    var o = ws.options || {};
    function pick(v) { try { return JSON.parse(JSON.stringify(v)); } catch (e) { return null; } };
    var renderer = '';
    try { renderer = ws.getRenderer ? ws.getRenderer().getClassName ? ws.getRenderer().name || ws.getRenderer().getClassName() : (ws.getRenderer().name || '') : ''; } catch (e) {}
    if (!renderer) { try { renderer = o.renderer || ''; } catch (e) {} }
    return {
      renderer: renderer,
      rtl: !!o.RTL,
      grid: pick(o.gridOptions),
      zoom: pick(o.zoomOptions),
      move: pick(o.moveOptions),
      trashcan: !!o.hasTrashcan,
      comments: !!o.comments,
      disable: !!o.disable,
      collapse: !!o.collapse,
      maxBlocks: o.maxBlocks === undefined ? Infinity === o.maxBlocks ? 0 : o.maxBlocks : o.maxBlocks,
      sounds: !!o.hasSounds,
      css: o.hasCss === undefined ? true : !!o.hasCss
    };
  }

  function themeOf(ws) {
    try {
      var t = ws.getTheme();
      return {
        name: t.name,
        blockStyles: JSON.parse(JSON.stringify(t.blockStyles || {})),
        categoryStyles: JSON.parse(JSON.stringify(t.categoryStyles || {})),
        componentStyles: JSON.parse(JSON.stringify(t.componentStyles || {})),
        fontStyle: JSON.parse(JSON.stringify(t.fontStyle || {})),
        startHats: !!t.startHats
      };
    } catch (e) { return null; }
  }

  // ==========================================================================
  // The site's look, all four parts of it
  //
  // A Blockly workspace looks the way it looks because of four things, and
  // none of them can be guessed from the definitions:
  //
  //   1. the RENDERER          the silhouette: notches, tabs, corners, hats
  //   2. its CONSTANT PROVIDER every measurement that silhouette is built from
  //   3. the THEME             block styles, category styles, component colours
  //   4. the injected CSS      toolbox, flyout, scrollbars, fonts, tooltips
  //
  // All four are read generically. Nothing here knows a class name of the
  // site's in advance, because a class name on this site is a build artefact.
  // ==========================================================================

  // Which renderer is drawing, and what it is registered as, so a renderer the
  // site wrote itself is visible rather than silently looking like the default.
  function rendererInfo(B, ws) {
    var out = { name: '', registryKey: '', className: '', registered: [] };
    var inst = null;
    try { inst = ws.getRenderer ? ws.getRenderer() : null; } catch (e) {}
    if (!inst) return out;
    try { out.name = inst.name || ''; } catch (e) {}
    try { out.className = (inst.constructor && inst.constructor.name) || ''; } catch (e) {}
    try {
      var Type = (B.registry && B.registry.Type && B.registry.Type.RENDERER)
        ? B.registry.Type.RENDERER : 'renderer';
      var all = B.registry.getAllItems(Type, true) || {};
      Object.keys(all).forEach(function (k) {
        out.registered.push(k);
        try { if (all[k] === inst.constructor) out.registryKey = k; } catch (e) {}
      });
    } catch (e) {}
    if (!out.registryKey) out.registryKey = out.name;
    if (!out.name) out.name = out.registryKey;
    return out;
  }

  // Every measurement on the constant provider: own and inherited, numbers,
  // strings, booleans, and the plain objects that hold the derived path data
  // (NOTCH, PUZZLE_TAB, START_HAT, INSIDE_CORNERS, OUTSIDE_CORNERS). Those path
  // strings ARE the connection shapes, so they are worth more than the numbers
  // they came from. Functions, DOM nodes and cycles are skipped.
  function captureConstants(ws) {
    var cp = null;
    try { cp = ws.getRenderer().getConstants(); } catch (e) { return null; }
    if (!cp) return null;
    var seen = [];
    var MAX_DEPTH = 4, MAX_KEYS = 400, MAX_ARRAY = 256;

    function plain(v, depth) {
      if (v === null) return null;
      var t = typeof v;
      if (t === 'number') return isFinite(v) ? v : undefined;
      if (t === 'string' || t === 'boolean') return v;
      if (t !== 'object') return undefined;                 // functions, symbols
      if (depth >= MAX_DEPTH) return undefined;
      if (v.nodeType !== undefined || v.ownerDocument !== undefined) return undefined;   // DOM
      if (seen.indexOf(v) >= 0) return undefined;           // cycle
      seen.push(v);
      var r;
      if (Array.isArray(v)) {
        r = [];
        for (var i = 0; i < v.length && i < MAX_ARRAY; i++) {
          var e = plain(v[i], depth + 1);
          if (e !== undefined) r.push(e);
        }
      } else {
        r = {};
        var n = 0;
        for (var k in v) {
          if (n >= MAX_KEYS) break;
          var val;
          try { val = v[k]; } catch (e2) { continue; }
          var c = plain(val, depth + 1);
          if (c !== undefined) { r[k] = c; n++; }
        }
        if (!n) r = undefined;
      }
      seen.pop();
      return r;
    }

    var out = {}, count = 0;
    for (var key in cp) {                                    // own and inherited
      if (count >= MAX_KEYS) break;
      var v;
      try { v = cp[key]; } catch (e) { continue; }
      if (typeof v === 'function') continue;
      var c = plain(v, 0);
      if (c === undefined) continue;
      out[key] = c;
      count++;
    }
    return count ? out : null;
  }

  // What colour each type actually came out. A type that sets its colour in
  // code rather than through a style is invisible to the theme, so both the
  // style name and the resolved colour are recorded and the tool prefers the
  // style when its theme knows it.
  function probeStyles(B, types) {
    var probe = new B.Workspace();
    var out = {}, n = 0;
    types.forEach(function (type) {
      var b = null;
      try { b = probe.newBlock(type); } catch (e) { return; }
      var rec = {};
      try { if (typeof b.getStyleName === 'function') rec.style = b.getStyleName(); } catch (e) {}
      try { if (typeof b.getColour === 'function') rec.colour = b.getColour(); } catch (e) {}
      try { if (typeof b.getColourSecondary === 'function') rec.colourSecondary = b.getColourSecondary(); } catch (e) {}
      try { if (typeof b.getColourTertiary === 'function') rec.colourTertiary = b.getColourTertiary(); } catch (e) {}
      try { if (b.hat) rec.hat = b.hat; } catch (e) {}
      try {
        var shape = (b.outputShape_ !== undefined) ? b.outputShape_ : b.outputShape;
        if (shape !== undefined && shape !== null) rec.outputShape = shape;
      } catch (e) {}
      try { b.dispose(false); } catch (e) {}
      if (Object.keys(rec).length) { out[type] = rec; n++; }
    });
    try { probe.dispose(); } catch (e) {}
    return out;
  }

  function computedLook(sel) {
    try {
      var node = document.querySelector(sel);
      if (!node) return null;
      var cs = window.getComputedStyle(node);
      if (!cs) return null;
      return {
        fontFamily: cs.fontFamily, fontSize: cs.fontSize, fontWeight: cs.fontWeight,
        fontStyle: cs.fontStyle, letterSpacing: cs.letterSpacing,
        textTransform: cs.textTransform, color: cs.color,
        background: cs.backgroundColor, border: cs.border
      };
    } catch (e) { return null; }
  }

  // =========================================================================
  // The toolbox and its pull-out, mined off the live site
  //
  // WHY THIS EXISTS AS ITS OWN PASS. Everything else here is captured by NAME:
  // rules whose selector says "blockly", constants the provider happens to
  // hold. The site's toolbox is not built out of Blockly's parts - it
  // registers its own PortalToolboxCategory and dresses the whole thing in its
  // own class names - so a capture that filters on the word "blockly" throws
  // away the very rules that make the pull-out a pull-out and keeps only the
  // ones that were already ours. The result reads as "nothing missing" while
  // the flyout comes out the size of a tooltip.
  //
  // So this asks the ELEMENTS what applies to them, rather than asking the
  // stylesheets what they are called. It opens a real category first, because
  // a flyout that has never been shown has no size to report.
  // =========================================================================

  // The tape measure lives in measure.js so the site and our own editor are
  // measured by exactly the same code. It is prepended to this script when it
  // is injected, so it is already here.
  // What the site really emits for an EMPTY socket. The readings show the
  // site puts no field in one, so whatever a creator sees inside the bubble
  // is drawn by the site's renderer and no definition will ever carry it.
  // How the site MEASURES a block, row by row and element by element. The
  // totals only say that something differs; this says which element.
  // Every block on the page, measured. Same experience on both sides, so the
  // two sweeps line up by id with identical content.
  // How the site PAINTS its fields, and what else it hangs on a block.
  function captureDiagnose() {
    var D = null;
    try { D = window.BF6Measure.diagnose(BL(), WS()); } catch (e) {}
    if (!D) { log('nothing to diagnose on this page'); return null; }
    D.op = 'diagnoselook';
    send(D);
    if (D.font) log('font: ' + D.font.font + '  widths ' + JSON.stringify(D.font.widths));
    if (D.argBlocks && D.argBlocks.length) log('argument blocks: ' + JSON.stringify(D.argBlocks));
    log('diagnose: ' + D.icons.length + ' icon kind(s), ' + D.labels.length +
      ' odd label(s), ' + D.warnings.length + ' warning(s), mutator sub-blocks ' +
      Object.keys(D.mutatorBlocks).join(',') || 'none');
  }

  function captureFields() {
    var F = null;
    try { F = window.BF6Measure.fieldLook(BL(), WS(), 10); } catch (e) {}
    if (!F) { log('the fields could not be read on this page'); return null; }
    F.op = 'fieldlook';
    send(F);
    log('field look: ' + F.blocks.length + ' block(s), icons across the page ' +
      JSON.stringify(F.icons) + ', text ' + JSON.stringify(F.css['.blocklyText'] || null));
    return F;
  }

  function captureSweep() {
    var S = null;
    try { S = window.BF6Measure.sweep(BL(), WS()); } catch (e) {}
    if (!S) { log('the workspace could not be swept on this page'); return null; }
    S.op = 'sweeplook';
    send(S);
    log('swept ' + S.count + ' block(s) of ' + Object.keys(S.byType).length + ' type(s)');
    return S;
  }

  function captureLayout(types) {
    var L = null;
    try { L = window.BF6Measure.layout(BL(), WS(), types || window.BF6Measure.TYPES_TO_MEASURE); } catch (e) {}
    if (!L) { log('the block layout could not be measured on this page'); return null; }
    L.op = 'layoutlook';
    L.url = location.href;
    send(L);
    log('block layout: ' + window.BF6Measure.layoutLine(L));
    return L;
  }

  function captureSocketLook() {
    var rows = null;
    try { rows = window.BF6Measure && window.BF6Measure.sockets(BL(), WS(), 4); } catch (e) {}
    if (!rows || !rows.length) { log('no block with an empty socket was found to look at'); return null; }
    var rules = null;
    try { rules = window.BF6Measure.socketRules(BL(), WS()); } catch (e) {}
    send({ op: 'socketlook', url: location.href, rows: rows, rules: rules });
    if (rules && rules.shapes) {
      log('socket shapes on the site: ' + Object.keys(rules.shapes).map(function (k) {
        var r = rules.shapes[k];
        return k + '=' + r.type + (rules.shapeNames[r.type] ? '(' + rules.shapeNames[r.type] + ')' : '') +
          ' ' + r.width + 'x' + r.height;
      }).join(', '));
      log('empty socket constants: ' + JSON.stringify(rules.constants));
    }
    log('empty sockets on the site: ' + rows.map(function (r) {
      return r.block + '.' + r.input + ' shape ' + (r.shape && r.shape.type) +
        ' with ' + r.fieldsInInput + ' field(s) in the socket and ' + r.svg.length + ' svg node(s)';
    }).join('; '));
    return rows;
  }

  function captureToolboxLook() {
    var m = null;
    try { m = window.BF6Measure && window.BF6Measure.toolbox(BL(), WS()); } catch (e) {}
    if (!m) { log("the toolbox could not be measured on this page"); return null; }
    m.op = "toolboxlook";
    send(m);
    log("toolbox look: " + window.BF6Measure.line(m));
    return m;
  }

  // The stylesheet the site puts over Blockly's own. Only the rules that name
  // blockly are taken: the rest of a 190 KB site bundle is not ours to carry.
  // Cross-origin sheets throw on cssRules, and each one that does is named in
  // the sources list so a gap is visible rather than silent.
  function captureCss() {
    var parts = [], sources = [], total = 0;
    var CAP = 512 * 1024;
    function push(text, from) {
      if (!text) return;
      if (total >= CAP) return;
      if (total + text.length > CAP) text = text.slice(0, CAP - total);
      parts.push(text); sources.push(from); total += text.length;
    }
    var B = BL();
    try {
      var c = B && B.Css && (B.Css.content || B.Css.CONTENT);
      if (typeof c === 'string') push(c, 'Blockly.Css');
    } catch (e) {}
    try {
      var tags = document.querySelectorAll('style');
      for (var i = 0; i < tags.length; i++) {
        var t = tags[i].textContent || '';
        if (t.toLowerCase().indexOf('blockly') < 0) continue;
        push(t, 'style tag ' + i + (tags[i].id ? ' #' + tags[i].id : ''));
      }
    } catch (e) {}
    try {
      var sheets = document.styleSheets;
      for (var s = 0; s < sheets.length; s++) {
        var rules = null;
        var href = '';
        try { href = sheets[s].href || ('inline sheet ' + s); } catch (e) { href = 'sheet ' + s; }
        try { rules = sheets[s].cssRules; } catch (e) { rules = null; }
        if (!rules) { sources.push('NOT READABLE (cross origin): ' + href); continue; }
        var keep = [];
        for (var r = 0; r < rules.length; r++) {
          var rule = rules[r];
          var sel = '', txt = '';
          try { sel = rule.selectorText || ''; } catch (e) {}
          try { txt = rule.cssText || ''; } catch (e) {}
          var hay = (sel || txt).toLowerCase();
          if (hay.indexOf('blockly') < 0) continue;
          if (txt) keep.push(txt);
        }
        if (keep.length) push(keep.join('\n'), href + ' (' + keep.length + ' rules)');
      }
    } catch (e) {}
    // THE CUSTOM PROPERTIES THE RULES ARE WRITTEN IN TERMS OF.
    //
    // Only rules whose text says 'blockly' are kept, which is right for the
    // rules and wrong for what they depend on: the site writes
    // .blocklyIconSymbol { fill: var(--tmln-colors-primary) } and defines
    // that variable on :root, in a rule that never mentions blockly and was
    // therefore always thrown away. Twelve properties were used and not one
    // of them defined.
    //
    // An undefined custom property does not fall back to anything sensible -
    // the declaration is simply invalid, so fill reverts to black. That is
    // why the question mark the site draws on a comment could not be seen on
    // our side: a black ? on the near black disc it sits in. Our markup was
    // byte for byte the site's, in the right place, in the one colour that
    // cannot be seen.
    //
    // The VALUES are resolved rather than the defining rules hunted down: a
    // variable can be defined anywhere up the tree, redefined per theme, and
    // built out of other variables. What the site's own element resolves it
    // to is the answer, whatever produced it.
    var vars = {};
    try {
      var seenVar = {};
      var joined = parts.join(String.fromCharCode(10));
      var NAMECHARS = 'abcdefghijklmnopqrstuvwxyz' +
                      'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_';
      var at = 0;
      while ((at = joined.indexOf('var(--', at)) >= 0) {
        var from = at + 4, to = from;
        while (to < joined.length && NAMECHARS.indexOf(joined.charAt(to)) >= 0) to++;
        if (to > from) seenVar[joined.slice(from, to)] = 1;
        at = to;
      }
      var host = document.querySelector('.blocklyWorkspace') ||
                 document.querySelector('.injectionDiv') ||
                 document.documentElement;
      var vcs = window.getComputedStyle(host);
      Object.keys(seenVar).forEach(function (n) {
        var v = '';
        try { v = String(vcs.getPropertyValue(n) || '').trim(); } catch (e) {}
        if (v) vars[n] = v;
      });
    } catch (e) {}

    return {
      text: parts.join('\n'),
      vars: vars,
      sources: sources,
      truncated: total >= CAP,
      fonts: {
        blocklyText: computedLook('.blocklyText'),
        toolbox: computedLook('.blocklyToolboxDiv'),
        treeRow: computedLook('.blocklyTreeRow'),
        treeLabel: computedLook('.blocklyTreeLabel'),
        flyout: computedLook('.blocklyFlyout'),
        tooltip: computedLook('.blocklyTooltipDiv'),
        body: computedLook('body')
      }
    };
  }

  function classListOf(node) {
    if (!node) return [];
    var cls = (node.className && node.className.baseVal !== undefined)
      ? node.className.baseVal : String(node.className || '');
    return cls.split(/\s+/).filter(Boolean);
  }

  // Each toolbox category's icon, as a picture the tool can draw: the computed
  // background image (a data URL where the site inlined it), the mask image
  // where the icon is a tinted mask, or the SVG markup itself. Keyed both by
  // category name and by every css class on the icon, because cssconfig.icon in
  // the captured toolbox names the class, not the category.
  function captureCategoryIcons() {
    var out = {};
    function record(key, node) {
      if (!key || !node || out[key]) return;
      var rec = {};
      try {
        var cs = window.getComputedStyle(node);
        if (cs) {
          if (cs.backgroundImage && cs.backgroundImage !== 'none') rec.backgroundImage = cs.backgroundImage;
          var mask = cs.maskImage || cs.webkitMaskImage;
          if (mask && mask !== 'none') rec.maskImage = mask;
          rec.colour = cs.color;
          rec.background = cs.backgroundColor;
          rec.width = cs.width;
          rec.height = cs.height;
        }
      } catch (e) {}
      try {
        var svg = (node.tagName && String(node.tagName).toLowerCase() === 'svg')
          ? node : (node.querySelector ? node.querySelector('svg') : null);
        if (svg && svg.outerHTML && svg.outerHTML.length < 40000) rec.svg = svg.outerHTML;
      } catch (e) {}
      if (Object.keys(rec).length) out[key] = rec;
    }
    try {
      var rows = document.querySelectorAll('.blocklyTreeRow, [class*="categoryRow"], [class*="toolboxCategory"]');
      for (var i = 0; i < rows.length; i++) {
        var row = rows[i];
        var label = row.querySelector('.blocklyTreeLabel, [class*="Label"]');
        var name = label ? (label.textContent || '').trim() : '';
        var icon = row.querySelector('.blocklyTreeIcon, [class*="icon" i], svg, i') || row;
        if (name) record('name:' + name, icon);
        classListOf(icon).forEach(function (c) {
          if (c && c !== 'blocklyTreeIcon') record('class:' + c, icon);
        });
        try {
          var cs = window.getComputedStyle(row);
          var selected = classListOf(row).join(' ').toLowerCase().indexOf('selected') >= 0;
          var rk = 'row:' + (name || String(i));
          out[rk] = {
            background: cs.backgroundColor, colour: cs.color,
            borderLeft: cs.borderLeftColor, borderLeftWidth: cs.borderLeftWidth,
            padding: cs.padding, selected: selected
          };
        } catch (e) {}
      }
    } catch (e) {}
    // The older sweep, kept: some icons live on elements the tree row does not
    // contain, and their class is what the toolbox cssconfig names.
    try {
      var nodes = document.querySelectorAll('[class*="toolbox-"]');
      for (var n = 0; n < nodes.length; n++) {
        classListOf(nodes[n]).forEach(function (c) {
          if (c.indexOf('toolbox-') === 0) record('class:' + c, nodes[n]);
        });
      }
    } catch (e) {}
    return out;
  }

  // One message, every part of the look. Sent on its own so a restyle can be
  // asked for without pulling the definitions across again.
  function captureStyle(reason) {
    var B = BL(), ws = WS();
    if (!B || !ws) {
      log('style capture asked for, but this page has no Blockly workspace', 'warn');
      send({ op: 'style', v: 1, source: 'live', url: location.href, failed: true,
        note: 'no Blockly workspace on this page' });
      return false;
    }
    var types = Object.keys(B.Blocks || {});
    var payload = {
      op: 'style',
      v: 1,
      source: 'live',
      url: location.href,
      capturedAt: new Date().toISOString(),
      blocklyVersion: (B.VERSION || ''),
      reason: reason || 'auto',
      renderer: rendererInfo(B, ws),
      constants: captureConstants(ws),
      theme: themeOf(ws),
      blockColours: probeStyles(B, types),
      css: captureCss(),
      categoryIcons: captureCategoryIcons(),
      // WHAT SHAPE A SOCKET IS, PER TYPE. It is a METHOD on the provider,
      // not a number, so it never crossed with the constants and our own
      // Blockly kept using its default: a hexagon for anything Boolean. The
      // site draws every socket as the same rounded pill. Measured rather
      // than assumed, and carried here so it stays measured.
      // The extra css classes the site puts on particular field texts, read
      // off rendered blocks because the minifier hides the property.
      fieldClasses: (function () {
        try { return window.BF6Measure.fieldClasses(B, ws); } catch (e) { return null; }
      })(),
      socketRules: (function () {
        try { return window.BF6Measure.socketRules(B, ws); } catch (e) { return null; }
      })()
    };
    send(payload);

    var th = payload.theme || {};
    var missing = [];
    if (!payload.renderer || !payload.renderer.name) missing.push('renderer');
    if (!payload.constants) missing.push('constants');
    if (!payload.theme) missing.push('theme');
    if (!Object.keys(payload.blockColours || {}).length) missing.push('blockColours');
    if (!payload.css || !payload.css.text) missing.push('css');
    if (!Object.keys(payload.categoryIcons || {}).length) missing.push('categoryIcons');
    log('style captured: renderer ' + ((payload.renderer && payload.renderer.name) || 'unknown') +
      ' (registered as ' + ((payload.renderer && payload.renderer.registryKey) || 'unknown') + ')' +
      ', ' + Object.keys(payload.constants || {}).length + ' constants' +
      ', theme ' + (th.name || 'unnamed') +
      ' with ' + Object.keys(th.blockStyles || {}).length + ' block styles' +
      ', ' + Object.keys(th.categoryStyles || {}).length + ' category styles' +
      ', ' + Object.keys(th.componentStyles || {}).length + ' component colours' +
      ', ' + Object.keys(payload.blockColours || {}).length + ' typed colours' +
      ', ' + ((payload.css && payload.css.text) || '').length + ' bytes of CSS from ' +
      ((payload.css && payload.css.sources) || []).length + ' source(s)' +
      ', ' + Object.keys(payload.categoryIcons || {}).length + ' icon records' +
      (missing.length ? '. NOT CAPTURED: ' + missing.join(', ') : '. Nothing missing'));
    S.styled = true;
    return true;
  }

  // A definition for a type the console never announced, read off a headless
  // instance: enough to draw the block and hold its values.
  function synthDefinition(B, type) {
    var probe = new B.Workspace();
    var out = null;
    try {
      var b = probe.newBlock(type);
      out = { type: type, inputs: [], fields: [] };
      out.colour = b.getColour ? b.getColour() : null;
      out.style = b.getStyleName ? b.getStyleName() : null;
      out.output = b.outputConnection ? (b.outputConnection.getCheck() || true) : null;
      out.previousStatement = b.previousConnection ? (b.previousConnection.getCheck() || true) : null;
      out.nextStatement = b.nextConnection ? (b.nextConnection.getCheck() || true) : null;
      try { var tip = b.tooltip; out.tooltip = typeof tip === 'function' ? String(tip.call(b)) : (tip ? String(tip) : ''); } catch (e) {}
      (b.inputList || []).forEach(function (inp) {
        var rec = { name: inp.name || null, type: inp.type, fields: [] };
        // WHAT THE SOCKET ACCEPTS, from the socket itself. This was being
        // worked out from the tooltip's signature text, which is a rendering of
        // the truth rather than the truth: it says "Any" where the connection
        // lists thirty types, and it says nothing at all for a block whose
        // tooltip the site leaves blank.
        try { if (inp.connection) rec.check = inp.connection.getCheck(); } catch (e) {}
        (inp.fieldRow || []).forEach(function (f) {
          var fr = { name: f.name || null, klass: (f.constructor && f.constructor.name) || '', value: null };
          try { fr.value = f.getValue ? f.getValue() : null; } catch (e) {}
          try {
            if (typeof f.getOptions === 'function') fr.options = f.getOptions(false);
          } catch (e) {}
          try { if (typeof f.getSrc === 'function') fr.src = f.getSrc(); } catch (e) {}
          // WHAT KIND OF FIELD IT IS, WITHOUT ASKING ITS CLASS NAME.
          //
          // EDITABLE and SERIALIZABLE are public on every Blockly field and
          // survive the site's minifier, where the class name does not. They
          // are the difference between a name a creator can type into and a
          // name the block merely remembers: the site's SUBROUTINE_NAME is
          // the second kind, and building it as the first gave it a text box
          // and a border the site does not draw, 13 pixels taller and 16
          // wider than the real one.
          // THE CSS CLASS THE SITE PUTS ON A FIELD.
          //
          // The site's stylesheet paints particular labels by class:
          //   .blocklyText.subroutineBlockSubroutineText { fill: #ed7f33 }
          //   .blocklyText.fieldHeaderText { font-family: Purista-Semibold }
          // A field built without its class matches none of it, so the label
          // the site draws orange in its header font came out plain white.
          // The class is a constructor argument Blockly keeps on the field,
          // so it can be read here without the block ever being drawn.
          try { fr.cssClass = f.class_ || f.cssClass_ || f.cssClass || null; } catch (e) {}
          try { fr.editable = !!f.EDITABLE; } catch (e) {}
          try { fr.serializable = !!f.SERIALIZABLE; } catch (e) {}
          // HOW BIG THE SITE DRAWS IT. An image field carries its own size and
          // nothing else does: the file's intrinsic size is not it (the quote
          // marks are a 44x36 png drawn at a fraction of that), and guessing it
          // is how a block ends up the right shape with the wrong proportions.
          // Read three ways because only the first is public API and a headless
          // probe block can refuse it.
          try {
            var sz = (typeof f.getSize === 'function') ? f.getSize() : null;
            if (sz && sz.width) { fr.w = sz.width; fr.h = sz.height; }
          } catch (e) {}
          try {
            if (fr.w == null && f.size_ && f.size_.width) { fr.w = f.size_.width; fr.h = f.size_.height; }
          } catch (e) {}
          try {
            if (fr.w == null && typeof f.imageHeight_ === 'number') fr.h = f.imageHeight_;
          } catch (e) {}
          rec.fields.push(fr);
        });
        out.inputs.push(rec);
      });
      b.dispose(false);
    } catch (e) { out = null; }
    try { probe.dispose(); } catch (e) {}
    return out;
  }

  // ---- the capture --------------------------------------------------------
  function capture() {
    var B = BL(), ws = WS();
    if (!B || !ws) return false;

    // THE CATALOGUE IS ANNOUNCED ONCE, TO A CONSOLE, AS THE BUNDLE LOADS.
    //
    // Hooking console.debug is the only place it is handed out whole, and the
    // hook has to be in the page before the bundle runs. It is - the script is
    // injected as the document starts - but reaching the blocks page WITHOUT a
    // document load, by the site's own router from a page that was already
    // open, means no injection, no fresh hook, and the announcement was made
    // long ago on a page that has since gone. Seen exactly once on 2026-09-06:
    // 604 block readings came back and the catalogue came back empty.
    //
    // A real reload puts the hook in front of the bundle again. Once, and only
    // when there is nothing to lose by it.
    //
    // The mark has to survive the reload, and the script's own closure does
    // not: a flag on this object would be a fresh false on the new page and
    // the page would reload for ever. sessionStorage lives as long as the tab.
    if (!capturedDefs && !reloadedForDefs()) {
      // "nothing to lose by it" used to be a claim in this comment rather than
      // a test. Reloading discards whatever is unsaved on the page, and this
      // runs on the user's real Portal editor, so it now has to prove the claim
      // before it acts. Two signals, either of which is enough to refuse:
      //
      //   S.dirty   what we have watched the workspace change since the last
      //             push, which is the edits this script knows about.
      //   the site's own beforeunload handler, which Portal registers when IT
      //             thinks there is something worth warning about. That covers
      //             edits made in parts of the page we do not watch.
      //
      // Refusing costs a catalogue capture, which the next visit gets anyway.
      // Reloading wrongly costs the user work they cannot get back.
      var pendingEdits = 0;
      try { pendingEdits = Object.keys(S.dirty || {}).length; } catch (e) {}
      var siteWouldWarn = false;
      try { siteWouldWarn = typeof window.onbeforeunload === 'function'; } catch (e) {}

      if (pendingEdits || siteWouldWarn) {
        log('the mod catalogue was not announced on this page, and this page has ' +
            'unsaved changes, so it has been left alone. Save, or open the blocks ' +
            'page fresh, and the catalogue will be read then.', 'warn');
        return false;
      }
      markReloadedForDefs();
      log('the mod catalogue was not announced on this page, so it is being loaded once more to hear it');
      try { location.reload(); } catch (e) {}
      return false;
    }
    // Heard it: the mark comes off, so a capture asked for later in the same
    // tab still gets its one reload if it needs one.
    if (capturedDefs) { try { sessionStorage.removeItem(RELOAD_KEY); } catch (e) {} }

    var defs = capturedDefs || {};
    var known = Object.keys(defs);
    var all = Object.keys(B.Blocks || {});
    var missing = all.filter(function (t) { return known.indexOf(t) < 0; });
    var synth = {};
    missing.forEach(function (t) {
      var d = synthDefinition(B, t);
      if (d) synth[t] = d;
    });
    var probe = probeTypes(B, all);
    var tb = null;
    try { tb = convertToolbox(ws.options.languageTree); } catch (e) { tb = null; }
    var examples = tb ? scanExamples(tb) : [];

    send({
      op: 'defs',
      url: location.href,
      definitions: defs,
      definitionCount: known.length,
      synthesized: synth,
      toolbox: tb,
      theme: themeOf(ws),
      options: workspaceOptions(ws),
      tooltips: probe.tooltips,
      icons: probe.icons,
      categoryIcons: categoryIcons(),
      helpUrls: probe.help,
      helpLinks: helpButton(),
      contextMenuIds: contextMenuIds(B),
      examples: examples.map(function (h) { return { path: h.path, name: h.name, node: h.node }; }),
      unknownMutations: unknownMutations,
      probeFailures: probe.failures,
      blocklyVersion: (B.VERSION || '')
    });
    S.captured = true;
    log('captured ' + known.length + ' definitions, ' + Object.keys(synth).length +
      ' synthesised, ' + all.length + ' types, toolbox ' + (tb ? 'yes' : 'no') +
      ', examples ' + examples.length);
    return true;
  }

  // ---- sync units (same rule as the tool: a rule is its own unit) ---------
  function unitsOf(ws) {
    var out = [];
    ws.getTopBlocks(false).forEach(function (top) {
      if (top.type === 'modBlock') {
        out.push(top);
        var inp = top.getInput('RULES');
        var b = inp && inp.connection && inp.connection.targetBlock();
        while (b) { out.push(b); b = b.getNextBlock(); }
      } else out.push(top);
    });
    return out;
  }

  function saveUnit(B, block) {
    return B.serialization.blocks.save(block, {
      addCoordinates: !block.getParent(), addNextBlocks: false, doFullSerialization: false
    });
  }

  function captureAnchor(block) {
    var conn = (block.previousConnection && block.previousConnection.targetConnection) ||
      (block.outputConnection && block.outputConnection.targetConnection);
    if (!conn) {
      var xy = block.getRelativeToSurfaceXY();
      return { x: Math.round(xy.x), y: Math.round(xy.y) };
    }
    var parent = conn.getSourceBlock();
    var input = null;
    try { input = conn.getParentInput(); } catch (e) { input = null; }
    return { parentId: parent.id, input: input ? input.name : null };
  }

  function reattach(ws, block, anchor) {
    if (!anchor || !block) return;
    if (anchor.parentId) {
      var parent = ws.getBlockById(anchor.parentId);
      if (!parent) return;
      var target = anchor.input ?
        (parent.getInput(anchor.input) && parent.getInput(anchor.input).connection) :
        parent.nextConnection;
      var mine = block.previousConnection || block.outputConnection;
      if (target && mine) { try { target.connect(mine); } catch (e) {} }
    } else if (anchor.x !== undefined) {
      var cur = block.getRelativeToSurfaceXY();
      block.moveBy(anchor.x - cur.x, anchor.y - cur.y);
    }
  }

  function snapshot(ws) {
    var B = BL();
    var map = {};
    unitsOf(ws).forEach(function (b) { map[b.id] = JSON.stringify(saveUnit(B, b)); });
    S.units = map;
  }

  // ---- applying the tool's edits here -------------------------------------
  // The site's own listeners must see something happen or its Save button
  // stays grey, so the delete and the append run with events off and a single
  // create event is fired afterwards, in one group.
  var applying = 0;
  function applyReplace(msg) {
    var B = BL(), ws = WS();
    if (!B || !ws) return;
    applying++;
    var group = B.Events.getGroup();
    B.Events.setGroup(msg.group || ('bf6' + Date.now()));
    try {
      var old = ws.getBlockById(msg.id);
      var anchor = msg.anchor || null;
      var tail = null;
      B.Events.disable();
      try {
        if (old) {
          if (!anchor) anchor = captureAnchor(old);
          if (old.nextConnection && old.nextConnection.targetBlock()) {
            tail = old.nextConnection.targetBlock();
            tail.unplug(false);
          }
          old.dispose(false);
        }
        var nb = B.serialization.blocks.append(msg.json, ws);
        reattach(ws, nb, anchor);
        if (tail && nb.nextConnection && tail.previousConnection) {
          try { nb.nextConnection.connect(tail.previousConnection); } catch (e) {}
        }
      } finally { B.Events.enable(); }
      var made = ws.getBlockById(msg.id);
      if (made) {
        try { B.Events.fire(new (B.Events.get(B.Events.BLOCK_CREATE))(made)); } catch (e) {}
        try { made.queueRender ? made.queueRender() : (made.render && made.render()); } catch (e) {}
      }
      S.units[msg.id] = JSON.stringify(msg.json);
      if (msg.seq !== undefined) send({ op: 'applied', seq: msg.seq, id: msg.id });
    } catch (e) {
      log('apply replaceTop failed for ' + msg.id + ': ' + e, 'error');
    } finally {
      B.Events.setGroup(group || false);
      applying--;
    }
  }

  function applyDelete(msg) {
    var B = BL(), ws = WS();
    if (!B || !ws) return;
    applying++;
    try {
      var ids = msg.ids || [msg.id];
      ids.forEach(function (id) {
        var b = ws.getBlockById(id);
        if (!b) return;
        try { b.dispose(false); } catch (e) {}
        delete S.units[id];
      });
      if (msg.seq !== undefined) send({ op: 'applied', seq: msg.seq });
    } finally { applying--; }
  }

  function applyVariables(list) {
    var ws = WS();
    if (!ws) return;
    applying++;
    try {
      (list || []).forEach(function (v) {
        if (!ws.getVariableById(v.id)) ws.createVariable(v.name, v.type || '', v.id);
      });
    } finally { applying--; }
  }

  function fullWorkspace() {
    var B = BL(), ws = WS();
    if (!B || !ws) return null;
    return B.serialization.workspaces.save(ws);
  }

  // ---- the Save control ---------------------------------------------------
  // Found by its text, because a class name on this site is a build artefact
  // and would break on the next deploy. The selector actually used is logged
  // so it can be re-anchored without guessing.
  function findSaveControl() {
    var wanted = ['save', 'save changes', 'save rules', 'publish'];
    var nodes = document.querySelectorAll('button, [role="button"], a');
    for (var i = 0; i < nodes.length; i++) {
      var el = nodes[i];
      var t = (el.textContent || '').trim().toLowerCase();
      if (!t || t.length > 24) continue;
      for (var w = 0; w < wanted.length; w++) {
        if (t === wanted[w]) {
          var sel = el.tagName.toLowerCase();
          if (el.id) sel += '#' + el.id;
          else if (el.className && typeof el.className === 'string') {
            sel += '.' + el.className.trim().split(/\s+/).slice(0, 2).join('.');
          }
          return { el: el, selector: sel, text: (el.textContent || '').trim() };
        }
      }
    }
    return null;
  }

  // ---- what the site says about a save ------------------------------------
  // Three sources, because no one of them is reliable on its own: the
  // gRPC-web trailer on the save call, the warning icons the site puts on
  // blocks, and whatever it shows the user.
  var lastGrpc = null;

  (function hookFetch() {
    if (!window.fetch) return;
    var orig = window.fetch;
    window.fetch = function (input, init) {
      var url = (typeof input === 'string') ? input : (input && input.url) || '';
      var p = orig.apply(this, arguments);
      if (!/updatePlayElement|PlayElement|WebPlay/i.test(url)) return p;
      return p.then(function (res) {
        try {
          res.clone().arrayBuffer().then(function (buf) {
            var t = readGrpcTrailer(new Uint8Array(buf));
            if (t) { lastGrpc = t; lastGrpc.url = url; }
          }).catch(function () {});
        } catch (e) {}
        return res;
      });
    };
  })();

  // gRPC-web frames: one flags byte, four length bytes, then the payload. The
  // frame with 0x80 set is the trailer, and it is plain text.
  function readGrpcTrailer(bytes) {
    var i = 0;
    while (i + 5 <= bytes.length) {
      var flags = bytes[i];
      var len = (bytes[i + 1] << 24) | (bytes[i + 2] << 16) | (bytes[i + 3] << 8) | bytes[i + 4];
      var start = i + 5, end = start + len;
      if (end > bytes.length) break;
      if (flags & 0x80) {
        var text = '';
        for (var k = start; k < end; k++) text += String.fromCharCode(bytes[k]);
        var st = /grpc-status:\s*(\d+)/i.exec(text);
        var ms = /grpc-message:\s*([^\r\n]*)/i.exec(text);
        return {
          status: st ? parseInt(st[1], 10) : 0,
          message: ms ? decodeURIComponent(ms[1].replace(/\+/g, ' ')) : '',
          raw: text.slice(0, 400)
        };
      }
      i = end;
    }
    return null;
  }

  function blockWarnings() {
    var ws = WS();
    var out = [];
    if (!ws) return out;
    ws.getAllBlocks(false).forEach(function (b) {
      var text = null;
      try { text = b.getWarningText ? b.getWarningText() : null; } catch (e) {}
      if (!text) {
        try {
          var icon = b.getIcon && (b.getIcon('warning') || b.getIcon('error'));
          if (icon && icon.getText) text = icon.getText();
        } catch (e) {}
      }
      if (!text && b.warning && b.warning.getText) {
        try { text = b.warning.getText(); } catch (e) {}
      }
      if (text) out.push({ id: b.id, type: b.type, text: String(text) });
    });
    return out;
  }

  // Anything the site put in front of the user, and the control that closes it.
  function readAndClearDialog() {
    var nodes = document.querySelectorAll('[role="alert"], [role="dialog"], [role="alertdialog"]');
    for (var i = 0; i < nodes.length; i++) {
      var el = nodes[i];
      var text = (el.textContent || '').trim();
      if (!text || text.length > 600) continue;
      var closer = el.querySelector('button[aria-label*="lose" i], button[aria-label*="ismiss" i], [role="button"][aria-label*="lose" i]');
      if (!closer) {
        var btns = el.querySelectorAll('button, [role="button"]');
        for (var b = 0; b < btns.length; b++) {
          var t = (btns[b].textContent || '').trim().toLowerCase();
          if (t === 'ok' || t === 'close' || t === 'dismiss' || t === 'got it') { closer = btns[b]; break; }
        }
      }
      var sel = el.tagName.toLowerCase() + (el.id ? '#' + el.id : '') +
        '[role=' + (el.getAttribute('role') || '') + ']';
      if (closer) {
        try { closer.click(); } catch (e) {}
        log('dismissed the site dialog: ' + sel + ' close control "' +
          (closer.getAttribute('aria-label') || closer.textContent || '').trim().slice(0, 30) + '"');
      } else {
        log('site dialog left open (no close control found): ' + sel);
      }
      return { text: text, selector: sel };
    }
    return null;
  }

  function reportSaveVerdict() {
    var dlg = readAndClearDialog();
    var warns = blockWarnings();
    var g = lastGrpc;
    lastGrpc = null;
    var msg = {
      op: 'portalResult',
      status: g ? g.status : (warns.length || (dlg && dlg.text) ? undefined : 0),
      message: g ? g.message : '',
      raw: g ? g.raw : '',
      dialog: dlg ? dlg.text : '',
      dialogSelector: dlg ? dlg.selector : '',
      blocks: warns,
      at: new Date().toISOString()
    };
    send(msg);
    log('save verdict: grpc-status ' + (msg.status === undefined ? 'unknown' : msg.status) +
      ', ' + warns.length + ' block warning(s)' + (dlg ? ', a dialog was shown' : ''));
  }

  function clickSave() {
    var hit = findSaveControl();
    if (!hit) {
      send({ op: 'saveResult', ok: false, text: 'No save control found on the page', selector: '' });
      return;
    }
    var disabled = hit.el.disabled || hit.el.getAttribute('aria-disabled') === 'true';
    log('save control: ' + hit.selector + ' text "' + hit.text + '"' + (disabled ? ' (disabled)' : ''));
    if (disabled) {
      send({ op: 'saveResult', ok: false, text: 'The site\'s save control is disabled: nothing to save', selector: hit.selector });
      return;
    }
    try { hit.el.click(); } catch (e) {
      send({ op: 'saveResult', ok: false, text: 'Click refused: ' + e, selector: hit.selector });
      return;
    }
    setTimeout(function () {
      var again = findSaveControl();
      var still = again && !(again.el.disabled || again.el.getAttribute('aria-disabled') === 'true');
      send({
        op: 'saveResult', ok: true, selector: hit.selector,
        text: still ? 'Save pressed. The control is still active, so check the page.'
          : 'Save pressed and the control went inactive, which is what a saved page looks like.'
      });
    }, 1500);
    // The verdict lands later than the click: the call has to come back and
    // the site has to draw whatever it thinks of the result.
    setTimeout(reportSaveVerdict, 3000);
    setTimeout(function () {
      var d = readAndClearDialog();
      if (d) send({ op: 'portalResult', dialog: d.text, dialogSelector: d.selector, blocks: blockWarnings() });
    }, 6000);
  }

  // ---- the site's own import control --------------------------------------
  // Found by its text and by the file inputs on the page, never by a class
  // name, because a class name here is a build artefact. Whatever is found is
  // reported with the selector that found it, so it can be re-anchored on the
  // site's next build without guessing.
  //
  // A file input cannot be filled from script: the browser will not let a page
  // put a path into one. So the most this can do is open the picker for the
  // user, with the path already on their clipboard.
  function findImportControl() {
    var wanted = ['import', 'import rules', 'import experience', 'upload', 'load file'];
    try {
      var inputs = document.querySelectorAll('input[type="file"]');
      if (inputs.length) {
        var el = inputs[0];
        var sel = 'input[type=file]' + (el.id ? '#' + el.id : '') +
          (el.accept ? '[accept="' + el.accept + '"]' : '');
        return { el: el, selector: sel, text: el.accept || '', how: 'a file input on the page' };
      }
    } catch (e) {}
    try {
      var nodes = document.querySelectorAll('button, [role="button"], a, label');
      for (var i = 0; i < nodes.length; i++) {
        var n = nodes[i];
        var t = (n.textContent || '').trim().toLowerCase();
        if (!t || t.length > 24) continue;
        for (var w = 0; w < wanted.length; w++) {
          if (t !== wanted[w]) continue;
          var s = n.tagName.toLowerCase();
          if (n.id) s += '#' + n.id;
          else if (n.className && typeof n.className === 'string') {
            s += '.' + n.className.trim().split(/\s+/).slice(0, 2).join('.');
          }
          return { el: n, selector: s, text: (n.textContent || '').trim(), how: 'a control named by its text' };
        }
      }
    } catch (e) {}
    return null;
  }

  function driveImport(msg) {
    var hit = findImportControl();
    if (!hit) {
      send({ op: 'importControl', selector: '', text: '', how: 'nothing found', drove: false,
        path: msg && msg.path });
      log('no import control found on this page: the tool wrote the file and put its path on the clipboard');
      return;
    }
    var drove = false;
    try { hit.el.click(); drove = true; } catch (e) { drove = false; }
    send({ op: 'importControl', selector: hit.selector, text: hit.text, how: hit.how,
      drove: drove, path: msg && msg.path });
    log('import control: ' + hit.selector + ' (' + hit.how + ')' +
      (drove ? ' - opened. The browser will not let a page fill a file picker, so paste the ' +
        'path the tool put on your clipboard.' : ' - it would not take a click.'));
  }

  // ---- edits made here go back to the tool --------------------------------
  function onChange(e) {
    if (applying > 0 || !S.ready) return;
    if (!e || e.isUiEvent) return;
    if (e.type === 'var_create' || e.type === 'var_delete' || e.type === 'var_rename') {
      S.dirty['@vars'] = 1;
    } else if (e.blockId) {
      S.dirty[e.blockId] = 1;
    } else {
      S.dirty['@all'] = 1;
    }
    if (S.debounce) clearTimeout(S.debounce);
    S.debounce = setTimeout(flush, 150);
  }

  function flush() {
    S.debounce = null;
    var B = BL(), ws = WS();
    if (!B || !ws) return;
    var msgs = [];
    var seen = {};
    unitsOf(ws).forEach(function (b) {
      var json = saveUnit(B, b);
      var text = JSON.stringify(json);
      seen[b.id] = 1;
      if (S.units[b.id] !== text) {
        S.units[b.id] = text;
        msgs.push({ op: 'replaceTop', id: b.id, json: json, anchor: captureAnchor(b) });
      }
    });
    var gone = [];
    Object.keys(S.units).forEach(function (id) { if (!seen[id]) { gone.push(id); delete S.units[id]; } });
    if (gone.length) msgs.push({ op: 'deleteTop', ids: gone });
    if (S.dirty['@vars']) {
      msgs.push({
        op: 'variables', list: ws.getAllVariables().map(function (v) {
          return { name: v.name, id: v.getId(), type: v.type };
        })
      });
    }
    S.dirty = {};
    msgs.forEach(function (m) { m.seq = ++S.seqOut; send(m); });
  }

  // ---- messages from the tool --------------------------------------------
  function handle(msg) {
    if (!msg || !msg.op) return;
    switch (msg.op) {
      case 'chunk': {
        var slot = S.chunks[msg.cid] || (S.chunks[msg.cid] = []);
        slot[msg.i] = msg.part;
        var done = slot.length === msg.n;
        for (var i = 0; done && i < msg.n; i++) if (slot[i] === undefined) done = false;
        if (done) {
          var text = slot.join('');
          delete S.chunks[msg.cid];
          try { handle(JSON.parse(text)); } catch (e) { log('bad chunked message: ' + e, 'error'); }
        }
        break;
      }
      case 'replaceTop': applyReplace(msg); break;
      case 'deleteTop': applyDelete(msg); break;
      case 'variables': applyVariables(msg.list); break;
      case 'getWorkspace': {
        var w = fullWorkspace();
        send({ op: 'workspace', json: w, url: location.href });
        snapshot(WS());
        break;
      }
      case 'setWorkspace': {
        var B = BL(), ws = WS();
        if (!B || !ws) break;
        applying++;
        try { B.serialization.workspaces.load(msg.json && msg.json.mod ? msg.json.mod : msg.json, ws); }
        catch (e) { log('setWorkspace failed: ' + e, 'error'); }
        finally { applying--; }
        snapshot(ws);
        break;
      }
      case 'clickSave': clickSave(); break;
      case 'recapture': capture(); break;
      case 'captureStyle': captureStyle(msg.reason || 'asked'); break;
      case 'captureToolbox': captureToolboxLook(); break;
      case 'captureSockets': captureSocketLook(); break;
      case 'captureLayout': captureLayout(msg.types); break;
      case 'captureSweep': captureSweep(); break;
      case 'captureFields': captureFields(); break;
      case 'captureDiagnose': captureDiagnose(); break;
      case 'capturePerf':
        try {
          window.BF6Measure.perf(BL(), WS(), function (p) {
            p.op = 'perflook';
            send(p);
            log('perf: ' + window.BF6Measure.perfLine(p));
          });
        } catch (e) { log('performance could not be measured here'); }
        break;
      case 'findImport': driveImport(msg); break;
      case 'ping': send({ op: 'pong', url: location.href, ready: S.ready }); break;
      default: break;
    }
  }

  window.BF6SiteSync = {
    recv: function (text) {
      try { handle(typeof text === 'string' ? JSON.parse(text) : text); }
      catch (e) { log('bad message: ' + e, 'error'); }
    },
    capture: capture,
    captureStyle: captureStyle,
    status: function () {
      return { ready: S.ready, captured: S.captured, styled: !!S.styled,
        units: Object.keys(S.units).length };
    }
  };

  // ---- come alive when the blocks page is up ------------------------------
  var tries = 0;
  function poll() {
    tries++;
    var ws = WS();
    var onBlocks = /\/rules\/blocks/.test(location.pathname) || !!ws;
    if (ws && onBlocks) {
      if (!S.ready) {
        S.ready = true;
        snapshot(ws);
        try { ws.addChangeListener(onChange); } catch (e) {}
        send({ op: 'ready', url: location.href, blocks: ws.getAllBlocks(false).length });
        setTimeout(function () { if (!S.captured) capture(); }, 500);
        // The look goes across on its own, a beat later, so a slow style
        // capture never holds up the definitions.
        setTimeout(function () { if (!S.styled) captureStyle('ready'); }, 900);
        // The pull-out cannot be measured until it has been opened once, and
        // that has to happen after the toolbox is built, so it comes last.
        setTimeout(function () { try { captureToolboxLook(); } catch (e) {} }, 1600);
      }
      return;
    }
    if (tries < 240) setTimeout(poll, 500);        // two minutes of patience
    else send({ op: 'ready', url: location.href, blocks: -1, note: 'no Blockly workspace on this page' });
  }
  poll();
})();
