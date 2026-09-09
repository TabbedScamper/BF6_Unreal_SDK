// ============================================================================
// ONE TAPE MEASURE, USED ON BOTH PAGES.
//
// The whole point of this file is that the site and our editor are measured by
// the SAME code. Two measuring routines that were written separately produce
// two numbers that differ for reasons nobody can pin down, and the difference
// that matters gets lost among the differences that do not. So this is loaded
// into our editor by editor.html and prepended to the script injected into the
// Portal site, and both call BF6Measure.toolbox(Blockly, workspace).
//
// It asks the ELEMENTS what applies to them rather than asking the stylesheets
// what they are called. That is the part a name filter cannot do, and it is
// why the toolbox capture kept reporting "nothing missing" while the pull-out
// came out the size of a tooltip: the site dresses its toolbox in its own
// class names, so every rule that makes a pull-out a pull-out was thrown away
// by a filter looking for the word "blockly".
// ============================================================================
(function () {
  'use strict';
  if (window.BF6Measure) return;

  var LOOK_PROPS = [
    'position', 'display', 'width', 'minWidth', 'maxWidth', 'height', 'minHeight',
    'maxHeight', 'flex', 'flexDirection', 'overflow', 'overflowX', 'overflowY',
    'padding', 'margin', 'boxSizing', 'transform', 'translate', 'left', 'right',
    'top', 'bottom', 'zIndex', 'backgroundColor', 'backgroundImage', 'border',
    'borderRadius', 'boxShadow', 'opacity', 'visibility', 'gap', 'fontFamily',
    'fontSize', 'fontWeight', 'color', 'letterSpacing', 'textTransform',
    // Everything measured on this page is SVG, and an SVG shape is coloured
    // by fill and stroke, not by color and background. Leaving them out meant
    // every icon and every path reported its look with the one property that
    // decides whether it can be seen at all missing.
    'fill', 'stroke', 'strokeWidth'
  ];

  function looksOf(node) {
    if (!node) return null;
    var cs = null;
    try { cs = window.getComputedStyle(node); } catch (e) { return null; }
    if (!cs) return null;
    var out = {};
    LOOK_PROPS.forEach(function (p) {
      var v = '';
      try { v = cs[p]; } catch (e) {}
      if (v !== '' && v !== undefined && v !== null) out[p] = String(v);
    });
    return out;
  }

  // Every css rule in the document that this element actually matches,
  // whatever the rule happens to be named.
  function rulesMatching(node, seen) {
    var out = [];
    if (!node || !node.matches) return out;
    var sheets = document.styleSheets;
    for (var s = 0; s < sheets.length; s++) {
      var rules = null;
      try { rules = sheets[s].cssRules; } catch (e) { rules = null; }
      if (!rules) continue;
      for (var r = 0; r < rules.length; r++) {
        var sel = '', txt = '';
        try { sel = rules[r].selectorText || ''; } catch (e) {}
        try { txt = rules[r].cssText || ''; } catch (e) {}
        if (!sel || !txt) continue;
        var hit = false;
        // A grouped selector is tested one part at a time: ":hover" and the
        // like throw on matches(), and one bad part must not lose the rule.
        sel.split(',').forEach(function (one) {
          if (hit) return;
          var t = one.replace(/::?[a-z-]+(\([^)]*\))?/gi, '').trim();
          if (!t) return;
          try { if (node.matches(t)) hit = true; } catch (e) {}
        });
        if (hit && !seen[txt]) { seen[txt] = 1; out.push(txt); }
      }
    }
    return out;
  }

  function classOf(node) {
    if (!node) return '';
    var c = node.className;
    if (c && c.baseVal !== undefined) return String(c.baseVal);
    return String(c || '');
  }

  function describe(node, what, seen) {
    if (!node) return { what: what, present: false };
    var r = null;
    try { r = node.getBoundingClientRect(); } catch (e) {}
    return {
      what: what,
      present: true,
      tag: String(node.tagName || '').toLowerCase(),
      id: node.id || '',
      cls: classOf(node),
      rect: r ? { x: Math.round(r.x), y: Math.round(r.y), w: Math.round(r.width), h: Math.round(r.height) } : null,
      look: looksOf(node),
      rules: rulesMatching(node, seen)
    };
  }

  function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : null; }

  function round2(v) {
    return (typeof v === 'number' && isFinite(v)) ? Math.round(v * 100) / 100 : null;
  }

  // Every attribute written on an element. An icon's shape lives in its
  // attributes (d, x, y, width, href), and those are the numbers to copy.
  function attrsOf(node) {
    var out = {};
    try {
      for (var i = 0; i < node.attributes.length; i++) {
        var a = node.attributes[i];
        out[a.name] = String(a.value || '').slice(0, 300);
      }
    } catch (e) {}
    return out;
  }

  // The pull-out cannot be measured until it has been opened once, so one
  // category is opened and the selection is put back afterwards. The page is
  // left as it was found.
  function toolbox(B, ws) {
    if (!B || !ws) return null;
    var tb = null, fo = null;
    try { tb = ws.getToolbox ? ws.getToolbox() : null; } catch (e) {}
    try { fo = ws.getFlyout ? ws.getFlyout() : null; } catch (e) {}

    var was = null, opened = '', failed = '';
    try { was = tb && tb.getSelectedItem ? tb.getSelectedItem() : null; } catch (e) {}
    var firstCat = null;
    if (tb) {
      var items = [];
      try { items = tb.getToolboxItems ? tb.getToolboxItems() : []; } catch (e) {}
      for (var i = 0; i < items.length; i++) {
        var sel = false;
        try { sel = !!(items[i].isSelectable && items[i].isSelectable()); } catch (e) {}
        if (!sel) continue;
        firstCat = items[i];
        try { opened = items[i].getName ? items[i].getName() : ''; } catch (e) {}
        try { tb.setSelectedItem(items[i]); }
        catch (e) { failed = String((e && e.message) || e); }
        break;
      }
    }

    var seen = {};
    var foWs = null;
    try { foWs = fo && fo.getWorkspace ? fo.getWorkspace() : null; } catch (e) {}

    var tbDiv = null, foSvg = null, foBg = null, row = null, label = null;
    try { tbDiv = tb && (tb.HtmlDiv || (tb.getHtmlDiv && tb.getHtmlDiv())); } catch (e) {}
    try { foSvg = fo && (fo.svgGroup_ || (fo.getSvgRoot && fo.getSvgRoot())); } catch (e) {}
    try { foBg = fo && fo.svgBackground_; } catch (e) {}
    try { row = document.querySelector('.blocklyToolboxCategory, .blocklyTreeRow'); } catch (e) {}
    try { label = document.querySelector('.blocklyTreeLabel'); } catch (e) {}

    // How wide the blocks in the pull-out actually are. A flyout that is the
    // right width around blocks of the wrong width still looks wrong, and
    // without this the two cannot be told apart.
    var blockWidths = [];
    try {
      (foWs ? foWs.getTopBlocks(false) : []).forEach(function (b) {
        var hw = null;
        try { hw = b.getHeightWidth ? b.getHeightWidth() : null; } catch (e) {}
        blockWidths.push({
          type: b.type,
          w: hw ? Math.round(hw.width) : null,
          h: hw ? Math.round(hw.height) : null
        });
      });
    } catch (e) {}

    var out = {
      url: location.href,
      openedCategory: opened,
      openFailed: failed,
      classes: {
        toolbox: tb ? ((tb.constructor && tb.constructor.name) || '') : '',
        flyout: fo ? ((fo.constructor && fo.constructor.name) || '') : '',
        category: firstCat ? ((firstCat.constructor && firstCat.constructor.name) || '') : ''
      },
      counts: {
        items: (function () { try { return tb.getToolboxItems().length; } catch (e) { return -1; } })(),
        selectable: (function () {
          try {
            return tb.getToolboxItems().filter(function (i) {
              try { return !!(i.isSelectable && i.isSelectable()); } catch (e) { return false; }
            }).length;
          } catch (e) { return -1; }
        })()
      },
      flyout: fo ? {
        width: num((function () { try { return fo.getWidth(); } catch (e) { return null; } })()),
        height: num((function () { try { return fo.getHeight(); } catch (e) { return null; } })()),
        visible: (function () { try { return !!fo.isVisible(); } catch (e) { return null; } })(),
        autoClose: fo.autoClose,
        horizontalLayout: fo.horizontalLayout,
        MARGIN: num(fo.MARGIN),
        GAP_X: num(fo.GAP_X),
        GAP_Y: num(fo.GAP_Y),
        CORNER_RADIUS: num(fo.CORNER_RADIUS),
        SCROLLBAR_MARGIN: num(fo.SCROLLBAR_MARGIN),
        tabWidth: num(fo.tabWidth_),
        toolboxPosition: num(fo.toolboxPosition_),
        blocks: blockWidths.length,
        blockSizes: blockWidths,
        scale: num((function () { try { return foWs.getScale(); } catch (e) { return null; } })())
      } : null,
      nodes: [
        describe(tbDiv, 'toolbox div', seen),
        describe(row, 'a category row', seen),
        describe(label, 'a category label', seen),
        describe(foSvg, 'flyout group', seen),
        describe(foBg, 'flyout background', seen),
        describe(document.querySelector('.blocklyFlyoutScrollbar, .blocklyScrollbarVertical'), 'flyout scrollbar', seen),
        describe((function () { try { return ws.getParentSvg(); } catch (e) { return null; } })(), 'workspace svg', seen),
        describe(document.querySelector('.injectionDiv'), 'injection div', seen)
      ],
      options: (function () {
        var o = ws.options || {};
        return {
          toolboxPosition: o.toolboxPosition,
          horizontalLayout: o.horizontalLayout,
          renderer: o.renderer,
          rtl: o.RTL,
          moveOptions: o.moveOptions,
          zoomOptions: o.zoomOptions
        };
      })(),
      cssRules: Object.keys(seen).length
    };

    try { if (was) tb.setSelectedItem(was); else if (tb && tb.clearSelection) tb.clearSelection(); } catch (e) {}
    return out;
  }

  // WHAT AN EMPTY SOCKET IS ACTUALLY MADE OF.
  //
  // The readings taken off the site's blocks show every empty value input with
  // fields: [] - the site puts NOTHING in the socket. So the symbol a creator
  // sees inside an empty bubble is not a field at all, it is drawn by the
  // site's own renderer, and no amount of reading the definitions will ever
  // find it. Our editor puts a FieldImage in the row instead, which lands the
  // symbol BESIDE the socket rather than inside it, and that is the difference
  // the screenshots keep showing.
  //
  // This dumps the svg the site really emits for a block with an empty socket:
  // every element under the block's group, with the attributes that say what
  // it is. It is the only way to learn how the bubble and its symbol are made.
  function sockets(B, ws, limit) {
    if (!B || !ws) return null;
    var want = limit || 3;
    var seenCheck = {};
    var out = [];
    var blocks = [];
    try { blocks = ws.getAllBlocks(false); } catch (e) { return null; }

    for (var i = 0; i < blocks.length && out.length < want; i++) {
      var b = blocks[i];
      var empty = null;
      try {
        (b.inputList || []).forEach(function (inp) {
          if (empty) return;
          if (!inp.connection) return;
          if (inp.connection.type !== B.INPUT_VALUE) return;
          if (inp.connection.targetConnection) return;   // filled: not what we want
          empty = inp;
        });
      } catch (e) {}
      if (!empty) continue;
      // One row per DISTINCT socket type, not the first three blocks: the
      // question is what a Boolean socket is drawn like versus a Player one,
      // and three samples of the same type answer none of it.
      var ck = '';
      try { ck = (empty.connection.getCheck() || ['any']).join('+'); } catch (e) { ck = 'any'; }
      if (seenCheck[ck]) continue;
      seenCheck[ck] = 1;

      var g = null;
      try { g = b.getSvgRoot(); } catch (e) {}
      if (!g) continue;

      var parts = [];
      var kids = g.querySelectorAll('*');
      for (var k = 0; k < kids.length && parts.length < 400; k++) {
        var n = kids[k];
        var tag = String(n.tagName || '').toLowerCase();
        var rec = { tag: tag, cls: classOf(n) };
        ['x', 'y', 'width', 'height', 'rx', 'ry', 'r', 'cx', 'cy', 'fill',
         'fill-opacity', 'stroke', 'transform', 'href', 'filter', 'clip-path'].forEach(function (a) {
          var v = null;
          try { v = n.getAttribute(a); } catch (e) {}
          if (v !== null && v !== '') rec[a] = String(v).slice(0, 160);
        });
        try {
          var xh = n.getAttribute('xlink:href') ||
            (n.href && n.href.baseVal) || null;
          if (xh) rec.xlinkHref = String(xh).slice(0, 160);
        } catch (e) {}
        try {
          var d = n.getAttribute('d');
          if (d) rec.d = String(d).slice(0, 400);
        } catch (e) {}
        if (tag === 'text') { try { rec.text = String(n.textContent || '').slice(0, 60); } catch (e) {} }
        parts.push(rec);
      }

      var shape = null;
      try {
        var cp = ws.getRenderer().getConstants();
        var sh = cp.shapeFor(empty.connection);
        shape = {
          type: sh && sh.type,
          isDynamic: !!(sh && sh.isDynamic),
          width: (sh && typeof sh.width === 'function') ? sh.width(24) : (sh && sh.width),
          height: (sh && typeof sh.height === 'function') ? sh.height(24) : (sh && sh.height)
        };
      } catch (e) { shape = { error: String((e && e.message) || e) }; }

      var conn = null;
      try { var o = empty.connection.getOffsetInBlock(); conn = { x: o.x, y: o.y }; } catch (e) {}

      out.push({
        block: b.type,
        input: empty.name,
        connOffset: conn,
        check: (function () { try { return empty.connection.getCheck(); } catch (e) { return null; } })(),
        shape: shape,
        fieldsInInput: (empty.fieldRow || []).length,
        svg: parts
      });
    }
    return out;
  }

  // A CANVAS OF OUR OWN, FOR SHAPES THE PROJECT DOES NOT HAPPEN TO CONTAIN.
  //
  // The table can only learn from what is on the page, and three times now a
  // rule has been missed because the project had no example of it: no block
  // carried an icon, no socket took four types, and exactly ONE block is
  // collapsed - so the jagged edge a collapsed block draws was seen once,
  // and once is not a rule.
  //
  // Collapsing a block on the site's own canvas would be an edit to the
  // user's experience, which this file does not do. So a second workspace is
  // injected off screen with the same renderer and theme, blocks are built
  // and collapsed there, and it is thrown away. Nothing the user owns is
  // touched, and the readings come from the same renderer that draws theirs.
  function scratchBlocks(B, ws) {
    var host = null, sw = null, out = [];
    try {
      var rendererName = '';
      try { rendererName = (ws.options && ws.options.renderer) || ''; } catch (e) {}
      // The renderer is registered under a name and that name is what inject
      // takes. Without it the scratch canvas would draw with stock zelos and
      // teach the table numbers the site never produced, so nothing is
      // measured at all rather than measured wrongly.
      if (!rendererName) {
        try { rendererName = (ws.getRenderer() && ws.getRenderer().name) || ''; } catch (e) {}
      }
      if (!rendererName) return out;

      // Types that really exist here, and that take a statement so there is
      // something to collapse away.
      var want = [];
      var all = [];
      try { all = ws.getAllBlocks(false); } catch (e) {}
      var seenT = {};
      for (var i = 0; i < all.length && want.length < 3; i++) {
        var t = all[i].type;
        if (seenT[t]) continue;
        if (all[i].outputConnection) continue;
        seenT[t] = 1;
        want.push(t);
      }
      if (!want.length) return out;

      host = document.createElement('div');
      host.setAttribute('data-bf6-scratch', '1');
      host.style.cssText =
        'position:absolute;left:-20000px;top:0;width:1400px;height:900px;';
      document.body.appendChild(host);

      var opts = { renderer: rendererName };
      try { var th = ws.getTheme && ws.getTheme(); if (th) opts.theme = th; } catch (e) {}
      sw = B.inject(host, opts);
      if (!sw) return out;

      var made = [];
      want.forEach(function (t) {
        try {
          var nb = sw.newBlock(t);
          nb.initSvg();
          nb.setCollapsed(true);
          made.push(nb);
        } catch (e) {}
      });
      if (!made.length) return out;
      try { sw.render(); } catch (e) {}

      // Measured by the same routine as everything else, so the readings are
      // the same kind of reading. Every block here is collapsed, so each one
      // is an independent sighting of the jagged edge.
      var SL = layout(B, sw, made.map(function (b) { return b.type; }));
      out = (SL && SL.blocks) || [];
    } catch (e) {
      out = [];
    } finally {
      try { if (sw) sw.dispose(); } catch (e) {}
      try { if (host && host.parentNode) host.parentNode.removeChild(host); } catch (e) {}
    }
    return out;
  }

  // THE NUMBERS THAT DECIDE WHAT AN EMPTY SOCKET LOOKS LIKE.
  //
  // Two things are being asked here and neither can be guessed:
  //
  //   1. what SHAPE the renderer gives a socket, per type. Stock zelos gives a
  //      Boolean socket a hexagon; the site draws it as the same rounded pill
  //      as everything else, which is why ours has diamonds where the site has
  //      none. shapeFor is asked directly, with a real connection of each type.
  //   2. how WIDE an empty socket is drawn. The site's are wide pills with room
  //      for the symbol; stock zelos makes them a stub. The constants that set
  //      that are read off the provider rather than guessed.
  function socketRules(B, ws) {
    if (!B || !ws) return null;
    var cp = null;
    try { cp = ws.getRenderer().getConstants(); } catch (e) { return { error: 'no constants' }; }

    var CONSTS = ['EMPTY_INLINE_INPUT_PADDING', 'EMPTY_INLINE_INPUT_HEIGHT',
      'MIN_BLOCK_WIDTH', 'MIN_BLOCK_HEIGHT', 'MEDIUM_PADDING', 'LARGE_PADDING',
      'SMALL_PADDING', 'CORNER_RADIUS', 'MAX_DYNAMIC_CONNECTION_SHAPE_WIDTH',
      'FIELD_BORDER_RECT_X_PADDING', 'FIELD_BORDER_RECT_Y_PADDING',
      'MIN_BLOCK_WIDTH_WITH_STATEMENT', 'INLINE_PADDING_Y'];
    var constants = {};
    CONSTS.forEach(function (k) {
      if (cp[k] !== undefined && typeof cp[k] !== 'function') constants[k] = cp[k];
    });

    // A real block with a real connection of each type, because shapeFor reads
    // the connection and nothing else will do.
    var probe = new B.Workspace();
    var TYPES = ['Boolean', 'Number', 'String', 'Player', 'Vector', 'Array', null];
    var shapes = {};
    try {
      B.Blocks.bf6_shape_probe = {
        init: function () {
          var args = TYPES.map(function (t, i) {
            var a = { type: 'input_value', name: 'S' + i };
            if (t) a.check = [t];
            return a;
          });
          this.jsonInit({
            type: 'bf6_shape_probe',
            message0: args.map(function (_, i) { return '%' + (i + 1); }).join(' '),
            args0: args,
            inputsInline: true,
            output: null
          });
        }
      };
      var pb = probe.newBlock('bf6_shape_probe');
      TYPES.forEach(function (t, i) {
        var inp = pb.getInput('S' + i);
        var rec = { check: t || 'any' };
        try {
          var sh = cp.shapeFor(inp.connection);
          rec.type = sh && sh.type;
          rec.isDynamic = !!(sh && sh.isDynamic);
          rec.width = (sh && typeof sh.width === 'function') ? sh.width(24) : (sh && sh.width);
          rec.height = (sh && typeof sh.height === 'function') ? sh.height(24) : (sh && sh.height);
        } catch (e) { rec.error = String((e && e.message) || e); }
        shapes[t || 'any'] = rec;
      });
      pb.dispose(false);
    } catch (e) {
      shapes.error = String((e && e.message) || e);
    }
    try { delete B.Blocks.bf6_shape_probe; } catch (e) {}
    try { probe.dispose(); } catch (e) {}

    // What the SHAPES enum means on this provider, so a bare number can be read.
    var names = {};
    try {
      Object.keys(cp.SHAPES || {}).forEach(function (k) { names[cp.SHAPES[k]] = k; });
    } catch (e) {}

    // THE HOLE'S OWN COLOUR, read off a socket that is really on screen.
    // The site sets it as an attribute on the path, not in css, so no
    // stylesheet capture can ever find it. It is #0a0a0a on every socket of
    // every block whatever colour the block is, which is why ours came out a
    // dark shade of the block instead: that is Blockly's own default, the
    // block's border colour.
    var outline = null;
    try {
      var op = document.querySelector('.blocklyOutlinePath');
      if (op) outline = { fill: op.getAttribute('fill'), stroke: op.getAttribute('stroke') };
    } catch (e) {}

    // The spacing table, measured off real blocks on this very page - plus
    // any shape the page does not happen to contain, built on a canvas of
    // our own. See scratchBlocks.
    var spacing = null;
    try {
      var L = layout(B, ws, typesWorthMeasuring(B, ws, 60));
      if (L && L.blocks) {
        var extra = scratchBlocks(B, ws);
        if (extra && extra.length) L.blocks = L.blocks.concat(extra);
      }
      if (L && L.blocks && L.blocks.length) spacing = spacingTable(L);
    } catch (e) {}

    // HOW WIDE AN EMPTY SOCKET IS, PER NUMBER OF TYPES IT WILL ACCEPT.
    //
    // Measured on the site, an empty socket is not one width but three:
    // 72.62, 99.23 and 125.85. It draws ONE SYMBOL PER TYPE IT ACCEPTS, so a
    // socket that takes a Number or a String is wider than one that takes
    // only a Number, and one that takes three is wider still. Ours drew a
    // single symbol in every socket and kept room for one, so every
    // multi-type socket came out 26.61 or 53.23 narrow - which is most of
    // what was left after the leaves matched.
    //
    // The three widths are read off real sockets rather than worked out from
    // a symbol width and a gap. Two numbers guessed from three observations
    // fit by construction and prove nothing; the widths themselves are what
    // the renderer has to produce, so the widths are what is carried.
    var emptyByChecks = (function () {
      var seen = {}, all = [];
      try { all = ws.getAllBlocks(false); } catch (e) { return null; }
      // EVERY BLOCK THAT COULD HAVE ONE, NOT THE FIRST FEW HUNDRED.
      //
      // Capped at 400 blocks, this found sockets taking one, two and three
      // types and never met a four - so PlaySound stayed 79.84 narrow, which
      // is exactly the three extra symbols a four type socket draws. The cap
      // was there because measuring a block is not free, so instead the cheap
      // question is asked first: a block with nothing empty in it cannot
      // teach this table anything and is never measured at all.
      for (var i = 0; i < all.length; i++) {
        var couldTeach = false;
        try {
          var ins = all[i].inputList || [];
          for (var j = 0; j < ins.length; j++) {
            var cn = ins[j].connection;
            if (cn && cn.type === B.INPUT_VALUE && !cn.targetConnection) { couldTeach = true; break; }
          }
        } catch (e) { continue; }
        if (!couldTeach) continue;
        var info = null;
        try { info = ws.getRenderer().makeRenderInfo_(all[i]); info.measure(); }
        catch (e) { continue; }
        (info.rows || []).forEach(function (row) {
          (row.elements || []).forEach(function (el) {
            try {
              if (!B.blockRendering.Types.isInlineInput(el)) return;
              if (el.connectedBlock) return;
              var c = el.input && el.input.connection;
              if (!c || !c.getCheck) return;
              var ck = c.getCheck();
              var n = ck ? ck.length : 0;
              (seen[n] = seen[n] || []).push(Math.round(el.width * 100) / 100);
            } catch (e2) {}
          });
        });
      }
      var out = {};
      Object.keys(seen).forEach(function (k) {
        var counts = {}, best = null, bestN = 0;
        seen[k].forEach(function (v) {
          counts[v] = (counts[v] || 0) + 1;
          if (counts[v] > bestN) { bestN = counts[v]; best = v; }
        });
        out[k] = { px: Number(best), of: seen[k].length, agreed: bestN };
      });
      return out;
    })();

    // How much room a socket keeps for the symbol drawn inside it. The site
    // reserves exactly one symbol: its empty socket measures 72.62 where the
    // same socket with the same constants measures 48 without, and the
    // difference is 24.615, which is the symbol's own width to three places.
    var h = constants.EMPTY_INLINE_INPUT_HEIGHT;
    var iconRoom = (h > 0) ? (h * 10 / 13) : null;

    return { constants: constants, shapes: shapes, shapeNames: names,
             emptyByChecks: emptyByChecks,
             outline: outline, spacing: spacing, iconRoom: iconRoom };
  }

  // =========================================================================
  // THE BLOCK'S OWN LAYOUT, ROW BY ROW AND ELEMENT BY ELEMENT
  //
  // Totals do not diagnose. Knowing our ruleBlock is 332 wide where the site's
  // is 457 says only that something is different; it does not say whether a
  // field measured short, a padding is smaller, a row wrapped that should not
  // have, or a socket was drawn as a stub. Blockly measures a block into rows
  // of measurables before it draws a single path, and that measurement is the
  // thing to compare.
  //
  // The measurable TYPE is a bitfield with stable numbers (FIELD 1, ICON 4,
  // SPACER 8, INLINE_INPUT 256, STATEMENT_INPUT 512, and so on), so it survives
  // the site's minifier where a class name does not. That is what makes the two
  // sides comparable at all.
  //
  // Nothing is created on the workspace: an existing block of each type is
  // found and measured again. Building blocks on the site's own canvas would
  // change the user's experience, and this is a measurement, not an edit.
  var TYPE_NAMES = null;
  function typeNames(B) {
    if (TYPE_NAMES) return TYPE_NAMES;
    TYPE_NAMES = {};
    try {
      var T = B.blockRendering.Types;
      Object.keys(T).forEach(function (k) {
        if (typeof T[k] === 'number' && k !== 'nextTypeValue_') TYPE_NAMES[T[k]] = k;
      });
    } catch (e) {}
    return TYPE_NAMES;
  }

  function nameType(B, n) {
    var names = typeNames(B);
    if (names[n]) return names[n];
    // A composite: name every bit that is set, so a compound measurable still
    // reads as something rather than as a number.
    var bits = [];
    Object.keys(names).forEach(function (k) {
      var v = Number(k);
      if (v && (n & v) === v) bits.push(names[k]);
    });
    return bits.length ? bits.join('|') : String(n);
  }

  function elementOf(B, e) {
    var rec = {
      type: nameType(B, e.type),
      w: Math.round((e.width || 0) * 100) / 100,
      h: Math.round((e.height || 0) * 100) / 100,
      x: Math.round((e.xPos || 0) * 100) / 100,
      mid: Math.round((e.centerline || 0) * 100) / 100
    };
    if (e.bf6Room !== undefined) rec.room = e.bf6Room;
    if (e.bf6Filled !== undefined) rec.filled = e.bf6Filled;
    if (e.bf6HasConn !== undefined) rec.hasConn = e.bf6HasConn;
    if (e.bf6HasKid !== undefined) rec.hasKid = e.bf6HasKid;
    try {
      if (e.field) {
        rec.field = e.field.name || '(unnamed)';
        rec.fieldClass = (e.field.constructor && e.field.constructor.name) || '';
        var t = '';
        try { t = e.field.getText ? e.field.getText() : ''; } catch (e2) {}
        if (t) rec.text = String(t).slice(0, 24);
        try {
          var sz = e.field.getSize && e.field.getSize();
          if (sz) rec.fieldSize = Math.round(sz.width * 100) / 100 + 'x' + Math.round(sz.height * 100) / 100;
        } catch (e2) {}
      }
    } catch (e2) {}
    try { if (e.input) rec.input = e.input.name || '(unnamed)'; } catch (e2) {}
    try { if (e.shape) rec.shape = e.shape.type; } catch (e2) {}
    try { if (e.connectedBlock) rec.filledWith = e.connectedBlock.type; } catch (e2) {}
    return rec;
  }

  function findBlock(B, ws, type) {
    var all = [];
    try { all = ws.getAllBlocks(false); } catch (e) {}
    for (var i = 0; i < all.length; i++) if (all[i].type === type) return all[i];
    // Not on the canvas: the pull-out has one of nearly everything, and it is
    // drawn by the same renderer with the same constants.
    try {
      var fw = ws.getFlyout && ws.getFlyout() && ws.getFlyout().getWorkspace();
      if (fw) {
        var f = fw.getAllBlocks(false);
        for (var j = 0; j < f.length; j++) if (f[j].type === type) return f[j];
      }
    } catch (e) {}
    return null;
  }

  // THE BLOCKS BOTH SIDES MEASURE - ONE LIST, IN THE ONE FILE BOTH SIDES
  // LOAD.
  //
  // This list was written out twice, once in site_sync.js and once in
  // editor_ui.js, and they drifted: eight blocks were added to the site's
  // copy and the editor went on measuring the old ten, so the comparison
  // reported that our side had simply not measured them. Two copies of the
  // same list is the same bug as two measuring routines, which is what this
  // whole file exists to avoid.
  //
  // The first ten cover every SHAPE of block. The rest are types the sweep
  // says still measure differently. A '#id' entry names one exact block:
  // ids are stable across both sides, so it is the same block on each.
  var TYPES_TO_MEASURE = [
    'Number', 'Boolean', 'Text', 'variableReferenceBlock', 'UIAnchorItem',
    'subroutineArgumentBlock', 'subroutineInstanceBlock', 'ruleBlock',
    'AIBattlefieldBehavior', 'FindUIWidgetWithName',
    'Message', 'AddUIText', 'SetUITextLabel', 'AddUIContainer',
    'PlaySound', 'PlayMusic', 'If', 'ForVariable', 'SetVariable', 'GetMCOM'
    // A '#id' entry may be added here to measure one exact block. Ids belong
    // to a particular experience, so none are kept: they go in while a
    // specific block is being chased and come out again afterwards. The sweep
    // is what says which id to put here.
  ];

  function layout(B, ws, types) {
    if (!B || !ws) return null;
    var out = { blocks: [], missing: [], font: null, constants: {} };

    // Text is measured with a font, and a font that did not load measures
    // narrow. If every text element is short by the same ratio, that is the
    // answer and no amount of reading paddings will find it.
    try {
      var t = document.querySelector('.blocklyText');
      if (t) {
        var cs = window.getComputedStyle(t);
        out.font = {
          family: cs.fontFamily, size: cs.fontSize, weight: cs.fontWeight,
          letterSpacing: cs.letterSpacing
        };
      }
    } catch (e) {}

    var cp = null;
    try { cp = ws.getRenderer().getConstants(); } catch (e) {}
    if (cp) {
      ['FIELD_TEXT_FONTSIZE', 'FIELD_TEXT_FONTWEIGHT', 'FIELD_TEXT_FONTFAMILY',
       'FIELD_TEXT_HEIGHT', 'FIELD_TEXT_BASELINE', 'MIN_BLOCK_WIDTH',
       'MIN_BLOCK_HEIGHT', 'EMPTY_INLINE_INPUT_PADDING', 'EMPTY_INLINE_INPUT_HEIGHT',
       'MEDIUM_PADDING', 'LARGE_PADDING', 'SMALL_PADDING', 'NOTCH_WIDTH',
       'NOTCH_OFFSET_LEFT', 'TAB_WIDTH', 'STATEMENT_INPUT_PADDING_LEFT',
       'STATEMENT_BOTTOM_SPACER', 'BETWEEN_STATEMENT_PADDING_Y',
       'MAX_DYNAMIC_CONNECTION_SHAPE_WIDTH', 'FIELD_BORDER_RECT_X_PADDING',
       'FIELD_BORDER_RECT_Y_PADDING', 'FIELD_BORDER_RECT_HEIGHT',
       'MIN_BLOCK_WIDTH_WITH_STATEMENT', 'INSIDE_CORNERS', 'CORNER_RADIUS'
      ].forEach(function (k) {
        if (cp[k] !== undefined && typeof cp[k] !== 'function' &&
            (typeof cp[k] !== 'object' || cp[k] === null)) out.constants[k] = cp[k];
      });
    }

    function measureOne(type, b) {
      var info = null;
      try {
        info = ws.getRenderer().makeRenderInfo_(b);
        info.measure();
      } catch (e) {
        out.blocks.push({ type: type, error: String((e && e.message) || e) });
        return;
      }
      var hw = null;
      try { hw = b.getHeightWidth(); } catch (e) {}
      var rec = {
        type: type,
        // A VALUE BLOCK AND A STATEMENT BLOCK ARE NOT SPACED THE SAME.
        // Measured: the leading spacer before the first field is 0 on a value
        // block and 12 on a statement block. A table that does not know which
        // it is looking at averages the two and is wrong for both.
        kind: b.outputConnection ? 'V' : 'S',
        inline: !!b.inputsInline,
        measured: { w: Math.round(info.width * 100) / 100, h: Math.round(info.height * 100) / 100 },
        drawn: hw ? { w: Math.round(hw.width), h: Math.round(hw.height) } : null,
        startX: Math.round((info.startX || 0) * 100) / 100,
        rows: (info.rows || []).map(function (row) {
          return {
            type: nameType(B, row.type),
            w: Math.round((row.width || 0) * 100) / 100,
            h: Math.round((row.height || 0) * 100) / 100,
            y: Math.round((row.yPos || 0) * 100) / 100,
            align: row.align,
            elements: (row.elements || []).map(function (e) { return elementOf(B, e); })
          };
        })
      };
      out.blocks.push(rec);
    }

    // A BLOCK WITH SOMETHING PLUGGED INTO IT IS A DIFFERENT SHAPE FROM AN
    // EMPTY ONE, AND findBlock RETURNS WHICHEVER IT MEETS FIRST.
    //
    // Every LEAF matches the site exactly, and 454 containers still do not,
    // in deltas so quantised they can only be one rule each:
    // variableReferenceBlock is 8.55 narrow on all 218 of them, and
    // subroutineInstanceBlock is 24.62 WIDE on all 146 - which is iconRoom to
    // three places, room being kept for a symbol in a socket that is full.
    //
    // Neither shows up in a reading of an empty one, so both types were being
    // measured in the one state where they agree. A filled instance of each
    // type is measured as well, chosen by the same rule on both sides.
    // An instance with at least one value socket still open AND at least one
    // filled: the mixed state, which is where the two sides part.
    function firstCollapsed(type) {
      var all = [];
      try { all = ws.getAllBlocks(false); } catch (e) { return null; }
      for (var i = 0; i < all.length; i++) {
        if (all[i].type !== type) continue;
        try { if (all[i].isCollapsed && all[i].isCollapsed()) return all[i]; } catch (e) {}
      }
      return null;
    }

    function byId(type) {
      var all = [], out = [];
      try { all = ws.getAllBlocks(false); } catch (e) { return out; }
      for (var i = 0; i < all.length; i++) if (all[i].type === type) out.push(all[i]);
      out.sort(function (x, y) { return x.id < y.id ? -1 : (x.id > y.id ? 1 : 0); });
      return out;
    }

    function firstWithEmptySocket(type) {
      var all = [];
      try { all = ws.getAllBlocks(false); } catch (e) { return null; }
      for (var i = 0; i < all.length; i++) {
        if (all[i].type !== type) continue;
        var ins = all[i].inputList || [], open = 0, full = 0;
        for (var j = 0; j < ins.length; j++) {
          var c = ins[j].connection;
          if (!c) continue;
          if (c.targetConnection) full++; else open++;
        }
        if (open && full) return all[i];
      }
      return null;
    }

    function firstFilled(type) {
      var all = [];
      try { all = ws.getAllBlocks(false); } catch (e) { return null; }
      for (var i = 0; i < all.length; i++) {
        if (all[i].type !== type) continue;
        var ins = all[i].inputList || [];
        for (var j = 0; j < ins.length; j++) {
          var c = ins[j].connection;
          if (c && c.targetConnection) return all[i];
        }
      }
      return null;
    }

    (types || []).forEach(function (type) {
      // AN EXACT BLOCK, BY ID.
      //
      // Every other rule here picks an instance by a property and then hopes
      // it is one of the ones that disagree. It usually is not: three
      // samples of Message taken in id order all matched, while 37 other
      // Messages were 159.69 out. The sweep already knows exactly WHICH
      // blocks differ, so the probe has to be able to be pointed at them.
      // Ids are stable across both sides, so '#acu' is the same block on the
      // site as it is here and the two readings line up row for row.
      if (type.charAt(0) === '#') {
        var want = type.slice(1), found = null, every = [];
        try { every = ws.getAllBlocks(false); } catch (e) {}
        for (var q = 0; q < every.length; q++) {
          if (every[q].id === want) { found = every[q]; break; }
        }
        if (!found) { out.missing.push(type); return; }
        measureOne(found.type + ' #' + want, found);
        return;
      }
      var b = findBlock(B, ws, type);
      if (!b) { out.missing.push(type); return; }
      measureOne(type, b);
      var filled = firstFilled(type);
      if (filled && filled !== b) measureOne(type + ' filled', filled);
      // AND ONE WITH A HOLE STILL IN IT.
      //
      // 147 subroutineInstanceBlocks are 24.62 too WIDE on our side, which is
      // iconRoom to three places - the room kept for the symbol drawn inside
      // an empty socket. An empty instance of the type matches the site
      // exactly and a full one matches too, so the state that disagrees is
      // the mixed one: some sockets filled, at least one still open. That is
      // the only place the rule can be read.
      // A COLLAPSED ONE, WHICH IS A SHAPE NOTHING ELSE HAS.
      //
      // A collapsed block draws a JAGGED_EDGE, and no block in the type list
      // was collapsed, so the table learned nothing about the spacers either
      // side of one and fell back to Blockly's 12 where the site uses 24.
      var folded = firstCollapsed(type);
      if (folded && folded !== b) measureOne(type + ' collapsed', folded);
      var holed = firstWithEmptySocket(type);
      if (holed && holed !== b && holed !== filled) measureOne(type + ' with a hole', holed);
      // AND THREE NAMED ONES, SO THE TWO SIDES CAN BE COMPARED BLOCK FOR
      // BLOCK RATHER THAN EXAMPLE FOR EXAMPLE.
      //
      // Every rule above picks an instance by a PROPERTY, and the instance it
      // lands on may differ from the one whose numbers disagree - which is
      // how a reading of subroutineInstanceBlock came back 16.07 apart when
      // the difference being chased was 24.62. Sorting by id and taking the
      // first three picks the SAME blocks on both sides, because both sides
      // hold the same experience and ids are stable across it. The label
      // carries the id so the comparison cannot be mistaken.
      byId(type).slice(0, 3).forEach(function (one) {
        measureOne(type + ' @' + one.id, one);
      });
    });

    // A BLOCK CARRYING AN ICON IS A DIFFERENT SHAPE FROM THE SAME BLOCK
    // WITHOUT ONE, AND NOTHING IN THE TYPE LIST CARRIES ONE.
    //
    // Two Text blocks measured exactly 12px narrower than the site's, and
    // they are the only two in the project carrying a comment. The '?' in
    // their words looks like the cause and is not: that glyph measures 7.2px
    // on both sides. The cause is that no measured block had an icon, so the
    // row an icon sits in was never seen on either side, the spacing table
    // learned no rule for it, and our icon rows fell back to Blockly's own
    // spacing - 12 where the site's is 24, once, on one row.
    //
    // So every distinct combination of type and icons found in the workspace
    // is measured as well, under a label that names both. The table then
    // learns icon rows the same way it learns every other row, which is the
    // point: a number nobody measured is a number nobody can copy.
    try {
      var all = ws.getAllBlocks(false);
      // TWO OF EACH, NOT ONE.
      //
      // The spacing table only trusts a number the site gave twice, which is
      // the whole point of it: a context measured once is as likely to be
      // alignment padding as a rule. Measuring one block per kind of icon
      // therefore produced a reading that could never be used - V|ICON>FIELD
      // came back 24 with agreed 1, and was refused, and the value blocks
      // carrying a comment stayed twelve pixels narrow.
      // Two independent blocks agreeing is real agreement, so two are taken.
      var seenIcons = {}, taken = 0;
      for (var ai = 0; ai < all.length && taken < 12; ai++) {
        var ics = [];
        try { ics = all[ai].getIcons ? all[ai].getIcons() : []; } catch (e) { continue; }
        if (!ics.length) continue;
        var kinds = ics.map(function (x) {
          try { return String(x.getType ? x.getType() : (x.type || '?')); }
          catch (e) { return '?'; }
        }).sort().join('+');
        var label = all[ai].type + ' with ' + kinds;
        var nth = (seenIcons[label] || 0) + 1;
        if (nth > 2) continue;
        seenIcons[label] = nth;
        taken++;
        measureOne(label + ' #' + nth, all[ai]);
      }
    } catch (e) {}

    return out;
  }

  // THE SPACING RULE, READ OFF THE MEASUREMENT RATHER THAN OFF THE CODE.
  //
  // getInRowSpacing_ is a METHOD on the render info, so like shapeFor it never
  // crossed with the capture and ours kept Blockly's answer. Measured on the
  // site against ours, every interior spacer in an input row is 24 where ours
  // is 12, and the row-edge ones are unchanged:
  //
  //   FIELD->FIELD 24/12   FIELD->INPUT 24/12   INPUT->INPUT 24/12
  //   INPUT->EDGE  24/12   FIELD->EDGE  24/12   EDGE->INPUT  32/-
  //   EDGE->FIELD  12/12   EDGE->SPACER -13     SPACER->EDGE -21
  //
  // That is 12 pixels lost on both sides of every socket on every block, which
  // is most of why ours came out narrower and more cramped. It is captured as
  // a table keyed by what sits on each side, so it stays measured: if the site
  // changes a number, the next capture carries it.
  // THE THREE KINDS OF INPUT ARE NOT SPACED THE SAME, AND CALLING THEM ALL
  // 'INPUT' HID IT.
  //
  // S|EDGE>INPUT came back 32 with only 13 of 24 readings agreeing - the one
  // context in the whole table the site appeared to contradict itself on. It
  // does not: a row beginning with a statement input starts at 12 and one
  // beginning with an inline input starts at 32, and lumping the two together
  // let the commoner one win and made the other 20px wrong on every row.
  // Six rows of one block, six times 20.
  //
  // The consumer in editor.js must name these EXACTLY the same way, or every
  // key misses and the table silently does nothing.
  function spacerKind(e) {
    if (!e) return 'EDGE';
    if (/IN_ROW_SPACER/.test(e.type)) return 'SPACER';
    if (/STATEMENT_INPUT/.test(e.type)) return 'STATEMENT';
    if (/EXTERNAL_VALUE_INPUT/.test(e.type)) return 'EXTERNAL';
    if (/INLINE_INPUT/.test(e.type)) return 'INLINE';
    if (/FIELD/.test(e.type)) return 'FIELD';
    return e.type.split('|')[0];
  }

  function spacingTable(L) {
    var seen = {};
    (L && L.blocks || []).forEach(function (b) {
      (b.rows || []).forEach(function (r) {
        if (!/INPUT_ROW/.test(r.type)) return;
        var els = r.elements || [];
        for (var i = 0; i < els.length; i++) {
          if (!/IN_ROW_SPACER/.test(els[i].type)) continue;

          // NOT EVERY SPACER CAME FROM THE SPACING DECISION.
          //
          // zelos adds its own at the very ends of a value block's row, after
          // the fact, to pull the output connection in. They are always first
          // or last and always negative. Counting them as spacing readings is
          // what made a value block's leading spacer look like it sat next to
          // another spacer, when the decision that produced it saw the edge of
          // the row - so the table was keyed on something the renderer never
          // asks about, and none of it applied.
          var atEdge = (i === 0 || i === els.length - 1);
          if (atEdge && els[i].w < 0) continue;

          // And the key is worked out by looking OUTWARD past any spacer to
          // the nearest real element, which is exactly what the renderer had
          // in its hands when it decided.
          var L = i - 1, R = i + 1;
          while (L >= 0 && /IN_ROW_SPACER/.test(els[L].type)) L--;
          while (R < els.length && /IN_ROW_SPACER/.test(els[R].type)) R++;
          var key = (b.kind || 'S') + '|' +
            spacerKind(els[L]) + '>' + spacerKind(els[R]);
          (seen[key] = seen[key] || []).push(els[i].w);
        }
      });
    });
    // The commonest value wins. Alignment padding is added AFTER the spacing
    // decision and shows up here as one-off large numbers, so a mode is right
    // and an average would be wrong.
    var out = {};
    Object.keys(seen).forEach(function (k) {
      var counts = {}, best = null, bestN = 0;
      seen[k].forEach(function (v) {
        counts[v] = (counts[v] || 0) + 1;
        if (counts[v] > bestN) { bestN = counts[v]; best = v; }
      });
      out[k] = { px: Number(best), of: seen[k].length, agreed: bestN };
    });
    return out;
  }

  // Types worth measuring when nobody named any: real blocks off the page that
  // have a value socket, one per type, because a table built from one block is
  // a table built from one block.
  function typesWorthMeasuring(B, ws, want) {
    var out = [], seen = {};
    function scan(w) {
      var all = [];
      try { all = w.getAllBlocks(false); } catch (e) { return; }
      for (var i = 0; i < all.length && out.length < want; i++) {
        var b = all[i];
        if (seen[b.type]) continue;
        // EVERY SHAPE OF BLOCK, not only the ones with a socket. The first
        // table was built from six blocks that all happened to be statement
        // blocks with sockets, so it had no idea how a value block or a bare
        // dropdown is spaced and got both of them wrong.
        var has = false;
        (b.inputList || []).forEach(function (inp) {
          if ((inp.fieldRow || []).length) has = true;
          try { if (inp.connection && inp.connection.type === B.INPUT_VALUE) has = true; } catch (e) {}
        });
        if (!has) continue;
        seen[b.type] = 1;
        out.push(b.type);
      }
    }
    scan(ws);
    try {
      var fw = ws.getFlyout && ws.getFlyout() && ws.getFlyout().getWorkspace();
      if (fw && out.length < want) scan(fw);
    } catch (e) {}
    return out;
  }

  // =========================================================================
  // EVERY BLOCK ON THE PAGE, BY ID
  //
  // The site and our editor have the SAME experience open, block for block and
  // id for id, because ours was imported from it. So this does not sample and
  // it does not guess at like-for-like: it measures all of them and the two
  // readings line up by id, with identical content on both sides. Anything
  // left over after that is a real difference in how we draw it.
  //
  // Only sizes are recorded, because 5,000 full layouts is a payload nobody
  // can read. The types that come out worst are then worth a full layout.
  function sweep(B, ws) {
    if (!B || !ws) return null;
    var all = [];
    try { all = ws.getAllBlocks(false); } catch (e) { return null; }
    var rows = [], byType = {};
    for (var i = 0; i < all.length; i++) {
      var b = all[i];
      var hw = null;
      try { hw = b.getHeightWidth(); } catch (e) {}
      if (!hw) continue;
      // WITH THE STACK, AND WITHOUT IT.
      //
      // getHeightWidth measures the block AND everything under it, so one
      // primitive drawn a few pixels wrong shows up as a difference on every
      // ancestor it sits inside. That turns one cause into a hundred symptoms
      // and hides which is which. block.width and block.height are the block's
      // own, so a leaf tells the truth about itself.
      var w = Math.round(hw.width * 100) / 100;
      var h = Math.round(hw.height * 100) / 100;
      var ow = Math.round((b.width || 0) * 100) / 100;
      var oh = Math.round((b.height || 0) * 100) / 100;
      var kids = 0;
      try { kids = b.getChildren(false).length; } catch (e) {}
      rows.push([b.id, b.type, w, h, ow, oh, kids]);
      var t = byType[b.type] || (byType[b.type] = { n: 0, w: 0, h: 0 });
      t.n++; t.w += w; t.h += h;
    }
    Object.keys(byType).forEach(function (k) {
      byType[k].w = Math.round(byType[k].w / byType[k].n * 100) / 100;
      byType[k].h = Math.round(byType[k].h / byType[k].n * 100) / 100;
    });
    return { url: location.href, count: rows.length, rows: rows, byType: byType };
  }

  // =========================================================================
  // WHAT A FIELD IS ACTUALLY PAINTED, AND WHAT ELSE HANGS OFF A BLOCK
  //
  // Three things reported by eye and none of them findable in a definition:
  // a question mark the site draws and we do not, true/false on a coloured
  // ground where ours is white, and grey text where the site's is black.
  //
  // All three are RENDERED state, so all three are read off elements that are
  // really on screen: the computed fill of each field's rect and text, and the
  // icons Blockly hangs on a block (comment, warning, mutator, and whatever
  // the site adds of its own).
  function fieldLook(B, ws, want) {
    if (!B || !ws) return null;
    var out = { blocks: [], icons: {}, css: {} };
    var all = [];
    try { all = ws.getAllBlocks(false); } catch (e) { return null; }

    // The stylesheet answer for the two classes that decide it, whatever rule
    // won: this is what a screenshot shows.
    ['.blocklyText', '.blocklyEditableText>rect', '.blocklyFieldRect',
     '.blocklyDropdownText', '.blocklyEditableText>text'].forEach(function (sel) {
      try {
        var n = document.querySelector(sel.replace(/>.*/, ''));
        if (sel.indexOf('>') > 0) n = document.querySelector(sel);
        if (!n) return;
        var cs = window.getComputedStyle(n);
        out.css[sel] = { fill: cs.fill, stroke: cs.stroke, color: cs.color,
          fontFamily: cs.fontFamily, fontSize: cs.fontSize };
      } catch (e) {}
    });

    var seen = {};
    for (var i = 0; i < all.length && out.blocks.length < (want || 8); i++) {
      var b = all[i];
      if (seen[b.type]) continue;
      var root = null;
      try { root = b.getSvgRoot(); } catch (e) {}
      if (!root) continue;

      var fields = [];
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          var rec = { name: f.name || '(unnamed)', editable: !!f.EDITABLE };
          try { rec.text = String(f.getText ? f.getText() : '').slice(0, 24); } catch (e) {}
          // The field's own svg: the border rect and the text inside it.
          try {
            var g = f.getSvgRoot ? f.getSvgRoot() : null;
            if (g) {
              rec.cls = classOf(g);
              var rect = g.querySelector('rect');
              if (rect) {
                var rs = window.getComputedStyle(rect);
                rec.rectFill = rs.fill;
                rec.rectStroke = rs.stroke;
                rec.rx = rect.getAttribute('rx');
              }
              var txt = g.querySelector('text');
              if (txt) {
                var ts = window.getComputedStyle(txt);
                rec.textFill = ts.fill;
                rec.textWeight = ts.fontWeight;
              }
            }
          } catch (e) {}
          fields.push(rec);
        });
      });
      if (!fields.length) continue;
      seen[b.type] = 1;

      // Everything Blockly hangs on the block that is not a field: the comment
      // bubble, the warning, the mutator gear, and anything the site adds.
      var icons = [];
      try {
        (b.getIcons ? b.getIcons() : []).forEach(function (ic) {
          icons.push({
            klass: (ic.constructor && ic.constructor.name) || '',
            type: String(ic.getType ? ic.getType() : (ic.type || ''))
          });
        });
      } catch (e) {}
      var help = null;
      try { help = typeof b.helpUrl === 'function' ? b.helpUrl() : b.helpUrl; } catch (e) {}

      out.blocks.push({
        type: b.type, fields: fields, icons: icons,
        help: help ? String(help).slice(0, 80) : null,
        comment: (function () { try { return b.getCommentText ? b.getCommentText() : null; } catch (e) { return null; } })()
      });
    }

    // And a census of icons across the WHOLE page, so a question mark on one
    // block in ten thousand is not missed by a sample of eight.
    try {
      all.forEach(function (b) {
        var n = 0;
        try { n = (b.getIcons ? b.getIcons() : []).length; } catch (e) {}
        if (!n) return;
        (b.getIcons() || []).forEach(function (ic) {
          var k = String(ic.getType ? ic.getType() : (ic.type || 'unknown'));
          out.icons[k] = (out.icons[k] || 0) + 1;
        });
      });
    } catch (e) {}
    return out;
  }

  // =========================================================================
  // THREE THINGS THAT ARE NOT SHAPE AND NOT SIZE
  //
  //   1. the icon the site hangs on all 105 If blocks and we hang on none
  //   2. one label drawn orange where ours is white
  //   3. five warnings our editor raises that the site does not
  //
  // None of them can be answered from a definition, a constant or a stylesheet
  // name, so each is read off the running page: what the icon IS and what it
  // opens, which rule really paints that label, and what the warnings SAY.
  // WHAT IS ON THE PAGE AT ALL, BY CLASS.
  //
  // Every gap found so far was something nobody thought to look for: a class
  // we never draw, a rule we never captured, a property we never read. A
  // census does not need to know what it is looking for. Two of them, run on
  // both sides and subtracted, say what one page has that the other does not.
  function domCensus() {
    var out = {};
    try {
      var root = document.querySelector('.injectionDiv') || document.body;
      var all = root.querySelectorAll('*');
      var cap = Math.min(all.length, 60000);
      for (var i = 0; i < cap; i++) {
        var cs = classOf(all[i]).split(' ');
        for (var k = 0; k < cs.length; k++) {
          var c = cs[k].trim();
          if (!c) continue;
          out[c] = (out[c] || 0) + 1;
        }
      }
    } catch (e) {}
    return out;
  }

  // EVERY RULE THAT REALLY APPLIES TO SOMETHING ON THIS PAGE.
  //
  // The css capture keeps only rules whose TEXT mentions blockly, which is a
  // guess about relevance and has been wrong twice: it threw away the :root
  // rule defining the custom properties the blockly rules are written in, and
  // every @font-face. Both were invisible because the capture reported
  // success either way.
  //
  // This asks the other question - not 'does the rule look relevant' but
  // 'does the rule actually hit an element that exists here'. One example
  // element per distinct class is enough: a rule that matters matches at
  // least one of them.
  function applyingRules() {
    var seen = {}, out = [];
    try {
      var root = document.querySelector('.injectionDiv') || document.body;
      var byClass = {}, all = root.querySelectorAll('*');
      var cap = Math.min(all.length, 60000);
      for (var i = 0; i < cap; i++) {
        var cs = classOf(all[i]).split(' ');
        for (var k = 0; k < cs.length; k++) {
          var c = cs[k].trim();
          if (c && !byClass[c]) byClass[c] = all[i];
        }
      }
      var names = Object.keys(byClass);
      for (var n = 0; n < names.length && n < 400; n++) {
        var hits = rulesMatching(byClass[names[n]], seen);
        for (var h = 0; h < hits.length; h++) out.push(hits[h].slice(0, 600));
      }
    } catch (e) {}
    return out;
  }

  // EVERYTHING ON THE PAGE YOU CAN PRESS - INCLUDING OUTSIDE THE CANVAS.
  //
  // Every probe here is scoped to the blockly injection div, because that is
  // what is being copied. But the page around it is part of what the user
  // sees: the rules screen carries its own toolbar, and importing and
  // exporting a workspace lives there, not in blockly's context menu - which
  // is why a capture of the context menu found eight entries and none of them
  // was export.
  //
  // A whole-page sweep of anything pressable, with what it says and whether
  // it sits inside the canvas or outside it, is the cheapest way to see a
  // feature we have not noticed at all.
  function pageControls() {
    var out = [];
    try {
      var inj = document.querySelector('.injectionDiv');
      var sel = 'button,[role=button],[role=menuitem],a[href],input,select,summary,[aria-label]';
      var all = document.querySelectorAll(sel);
      var seen = {};
      for (var i = 0; i < all.length && out.length < 250; i++) {
        var el = all[i];
        var words = String(el.textContent || '').replace(new RegExp('[' + String.fromCharCode(9,10,13,32) + ']+', 'g'), ' ').trim().slice(0, 60);
        var label = '';
        try { label = String(el.getAttribute('aria-label') || '').slice(0, 60); } catch (e) {}
        var title = '';
        try { title = String(el.getAttribute('title') || '').slice(0, 60); } catch (e) {}
        var key = el.tagName + '|' + words + '|' + label + '|' + title;
        if (seen[key]) continue;
        seen[key] = 1;
        var vis = false;
        try { var r = el.getBoundingClientRect(); vis = !!(r.width && r.height); } catch (e) {}
        out.push({
          tag: el.tagName,
          words: words,
          label: label,
          title: title,
          cls: classOf(el).slice(0, 90),
          inCanvas: !!(inj && inj.contains(el)),
          visible: vis
        });
      }
    } catch (e) {}
    return out;
  }

  function diagnose(B, ws) {
    var out = { icons: [], labels: [], warnings: [], mutatorBlocks: {} };
    var all = [];
    try { all = ws.getAllBlocks(false); } catch (e) { return out; }

    // ---- 1. the icons, and what a mutator icon opens ---------------------
    var seenIcon = {};
    all.forEach(function (b) {
      var icons = [];
      try { icons = b.getIcons ? b.getIcons() : []; } catch (e) { return; }
      icons.forEach(function (ic) {
        var kind = '';
        try { kind = String(ic.getType ? ic.getType() : (ic.type || '')); } catch (e) {}
        var key = b.type + '/' + kind;
        if (seenIcon[key]) return;
        seenIcon[key] = 1;
        var rec = {
          block: b.type,
          kind: kind,
          klass: (ic.constructor && ic.constructor.name) || ''
        };
        // A mutator icon carries the list of blocks its bubble offers. That
        // list is the whole recipe: without it there is nothing to build.
        try {
          var names = ic.flyoutBlockTypes || ic.quarkNames_ || ic.blockTypes_ || null;
          if (names) rec.opens = [].slice.call(names);
        } catch (e) {}
        try { rec.bubbleOpen = !!(ic.bubbleIsVisible && ic.bubbleIsVisible()); } catch (e) {}
        // WHAT THE ICON IS, NOT JUST WHAT IT IS CALLED.
        //
        // Two Text blocks came out exactly 12px narrower than the site's,
        // and they are exactly the two carrying a comment. The '?' in their
        // words is the author pointing at the icon, not the cause: that
        // glyph measures the same on both sides, to the hundredth of a
        // pixel. The site simply does not use Blockly's stock comment icon.
        // It draws a question mark, and it reserves more room for it.
        //
        // A block asks the icon getSize() for the room to leave, and draws
        // whatever the icon put in its own svg. Both are read here, because
        // a minified class name says nothing about either.
        try {
          var sz = ic.getSize ? ic.getSize() : null;
          if (sz) rec.size = { w: round2(sz.width), h: round2(sz.height) };
        } catch (e) {}
        try {
          // THE SITE'S ICON WILL NOT HAND OVER ITS OWN SVG.
          //
          // getSvgRoot is not on the minified class, so the first reading came
          // back with no markup at all and the question mark the site draws on
          // a comment could not be copied. The drawing is on the page either
          // way: Blockly puts every icon in a .blocklyIconGroup inside the
          // block's own group, so it is found by looking rather than by asking.
          var iroot = null;
          try { iroot = ic.getSvgRoot ? ic.getSvgRoot() : null; } catch (e3) {}
          if (!iroot) {
            try {
              var host = b.getSvgRoot && b.getSvgRoot();
              var groups = host ? host.querySelectorAll('.blocklyIconGroup') : [];
              // The block's OWN icons, not those of blocks plugged into it.
              for (var gi = 0; gi < groups.length; gi++) {
                var owner = groups[gi].closest ? groups[gi].closest('.blocklyDraggable') : null;
                if (!owner || owner === host) { iroot = groups[gi]; break; }
              }
              if (!iroot && groups.length) iroot = groups[0];
            } catch (e4) {}
          }
          if (iroot) {
            rec.svg = String(iroot.outerHTML || '').slice(0, 4000);
            rec.look = looksOf(iroot);
            rec.rules = rulesMatching(iroot, {});
            var kids = [].slice.call(iroot.querySelectorAll('*')).slice(0, 8);
            rec.parts = kids.map(function (k) {
              return {
                tag: k.tagName,
                cls: classOf(k),
                text: String(k.textContent || '').slice(0, 24),
                attrs: attrsOf(k),
                look: looksOf(k)
              };
            });
          }
        } catch (e) {}
        // and what the block itself offers the bubble
        try {
          if (typeof b.decompose === 'function') rec.hasDecompose = true;
          if (typeof b.compose === 'function') rec.hasCompose = true;
          if (typeof b.saveConnections === 'function') rec.hasSaveConnections = true;
        } catch (e) {}
        out.icons.push(rec);
      });
    });

    // Every block type whose NAME says it belongs to a mutator bubble. The
    // site's own readings carry them, so the sub-blocks a mutator needs are
    // already on disk if it turns out to be Blockly's standard one.
    try {
      Object.keys(B.Blocks || {}).forEach(function (t) {
        if (/^controls_if/.test(t) || /mutator/i.test(t)) out.mutatorBlocks[t] = true;
      });
    } catch (e) {}

    try { out.labelWords = labelWords(B, ws); } catch (e) {}
    // What each ruleBlock thinks its scope field is doing. Reasoning about
    // this got it wrong twice; the block itself can just be asked.
    try {
      out.ruleScope = [];
      var rb = ws.getAllBlocks(false).filter(function (b) { return b.type === 'ruleBlock'; });
      for (var i = 0; i < rb.length && i < 8; i++) {
        var f = null;
        try { f = rb[i].getField('OBJECTTYPE'); } catch (e) {}
        var row = null;
        try { row = rb[i].getInput('OBJECTTYPE_DUMMY'); } catch (e) {}
        out.ruleScope.push({
          id: rb[i].id,
          event: (function () { try { return rb[i].getFieldValue('EVENTTYPE'); } catch (e) { return '?'; } })(),
          isOngoing: !!rb[i].isOngoing_,
          hasField: !!f,
          serializable: f ? !!f.SERIALIZABLE : null,
          isSer: (f && f.isSerializable) ? !!f.isSerializable() : null,
          hasRow: !!row,
          rowVisible: row ? !!row.isVisible() : null
        });
      }
    } catch (e) {}
    try { out.census = domCensus(); } catch (e) {}
    try { out.applying = applyingRules(); } catch (e) {}
    try { out.controls = pageControls(); } catch (e) {}

    // ---- 2. the label that is painted differently ------------------------
    // Every rule that really applies to it, whatever the rule is named, plus
    // whatever is written on the element itself.
    var seenLabel = {};
    all.forEach(function (b) {
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          var txt = '';
          try { txt = String(f.getText ? f.getText() : ''); } catch (e) {}
          if (!txt) return;
          var key = b.type + '/' + txt;
          if (seenLabel[key] || out.labels.length > 40) return;
          var g = null, t = null;
          try { g = f.getSvgRoot ? f.getSvgRoot() : null; } catch (e) {}
          try { t = g && g.querySelector('text'); } catch (e) {}
          if (!t) return;
          var cs = null;
          try { cs = window.getComputedStyle(t); } catch (e) {}
          if (!cs || cs.fill === 'rgb(255, 255, 255)') return;   // the ordinary case
          seenLabel[key] = 1;
          var seen = {};
          out.labels.push({
            block: b.type, field: f.name || '(unnamed)', text: txt.slice(0, 24),
            fill: cs.fill,
            gClass: classOf(g), tClass: classOf(t),
            inlineFill: (t.style && t.style.fill) || '',
            attrFill: t.getAttribute ? t.getAttribute('fill') : '',
            rules: rulesMatching(t, seen).slice(0, 6)
          });
        });
      });
    });

    // ---- 3. what the warnings actually say -------------------------------
    all.forEach(function (b) {
      var text = null;
      try { text = b.getWarningText ? b.getWarningText() : null; } catch (e) {}
      if (!text) return;
      if (out.warnings.length > 20) return;
      out.warnings.push({
        block: b.type, id: b.id, text: String(text).slice(0, 200),
        disabled: !!(b.isEnabled && !b.isEnabled()),
        parent: (b.getParent && b.getParent()) ? b.getParent().type : null
      });
    });

    out.font = fontProbe(ws);

    // What an argument block is actually showing, on each side, because its
    // dropdown is built from the subroutine it sits in and cannot be read off
    // a definition at all.
    out.argBlocks = [];
    try {
      all.forEach(function (b) {
        if (b.type !== 'subroutineArgumentBlock' || out.argBlocks.length >= 8) return;
        var shown = '', stored = '';
        try { stored = String(b.getFieldValue('ARGUMENT_INDEX')); } catch (e) {}
        try {
          var f = b.getField('ARGUMENT_INDEX');
          shown = String(f && f.getText ? f.getText() : '');
        } catch (e) {}
        var root = '';
        try { root = b.getRootBlock() ? b.getRootBlock().type : ''; } catch (e) {}
        // What the enclosing subroutine actually knows about its own
        // parameters, and what the field would offer if asked right now.
        var rootParams = null, offers = null, subName = '';
        try {
          var rb = b.getRootBlock();
          if (rb) {
            subName = String(rb.getFieldValue && rb.getFieldValue('SUBROUTINE_NAME') || '');
            rootParams = rb.params_ ? rb.params_.length : 'no params_ property';
          }
        } catch (e) {}
        try {
          var fld = b.getField('ARGUMENT_INDEX');
          offers = fld && fld.getOptions ? fld.getOptions(false).map(function (o) { return o[0]; }) : null;
        } catch (e) { offers = 'threw'; }
        out.argBlocks.push({ id: b.id, stored: stored, shown: shown, root: root,
          sub: subName, rootParams: rootParams, offers: offers });
      });
    } catch (e) {}

    return out;
  }

  // =========================================================================
  // WHERE THE TIME GOES
  //
  // "laggy" is not a measurement and neither is a frame rate on its own. What
  // decides whether a Blockly workspace of five thousand blocks feels smooth is
  // three numbers that can each be read directly:
  //
  //   how many svg nodes are on the page      (what the browser has to keep)
  //   how long a frame takes while panning    (what the eye actually sees)
  //   how much of that frame is layout        (the part we can usually remove)
  //
  // The pan is driven from here rather than by hand so the same movement is
  // measured every time, and the workspace is put back exactly where it was.
  // Nothing is guessed and nothing is averaged over a warm-up: the first
  // frames are reported separately, because the first frames are the ones a
  // person notices.
  function perf(B, ws, done, atScale) {
    var out = { at: new Date().toISOString() };
    if (!B || !ws) { done(out); return; }

    // MEASURE WHERE IT HURTS. At scale 1 only a handful of blocks are on
    // screen; the frame that actually stutters is the one with a hundred of
    // them on it, which is a zoomed-out view. The scale is put back after.
    var scaleWas = null;
    if (atScale > 0) {
      try { scaleWas = ws.getScale(); ws.setScale(atScale); } catch (e) {}
    }

    // ---- what is on the page --------------------------------------------
    try {
      var svg = ws.getParentSvg();
      out.nodes = {
        allInDocument: document.getElementsByTagName('*').length,
        inWorkspaceSvg: svg ? svg.getElementsByTagName('*').length : -1,
        paths: svg ? svg.getElementsByTagName('path').length : -1,
        texts: svg ? svg.getElementsByTagName('text').length : -1,
        images: svg ? svg.getElementsByTagName('image').length : -1,
        groups: svg ? svg.getElementsByTagName('g').length : -1
      };
    } catch (e) {}
    try {
      var blocks = ws.getAllBlocks(false);
      out.blocks = blocks.length;
      out.topBlocks = ws.getTopBlocks(false).length;
      var rendered = 0;
      blocks.forEach(function (b) { if (b.rendered) rendered++; });
      out.rendered = rendered;
      out.nodesPerBlock = out.blocks ? Math.round(out.nodes.inWorkspaceSvg / out.blocks * 10) / 10 : 0;
    } catch (e) {}

    // ---- what the workspace is set up to do every frame ------------------
    try {
      out.options = {
        scale: ws.getScale(),
        measuredAtScale: (atScale > 0) ? atScale : null,
        listeners: (ws.listeners_ && ws.listeners_.length) || 0,
        dragSurface: !!ws.getBlockDragSurface || !!ws.blockDragSurface_,
        gridEnabled: !!(ws.getGrid && ws.getGrid()),
        renderer: ws.getRenderer && ws.getRenderer().getClassName
          ? ws.getRenderer().getClassName() : ''
      };
    } catch (e) {}

    // ---- how long a frame takes while the canvas moves -------------------
    // Whether text is being drawn AT THE MOMENT OF THE PAN. Reading it after
    // the run reports the state the scale was put back to, which said 'text
    // drawn' on a run where it had been hidden throughout - a true statement
    // about the wrong instant.
    try {
      var probeText = (ws.getParentSvg() || document).querySelector('.blocklyBlockCanvas text');
      out.textDrawn = probeText
        ? window.getComputedStyle(probeText).display !== 'none' : null;
    } catch (e) {}

    // HOW MANY DISTINCT STRINGS, not how many text nodes. If a few hundred
    // labels are drawn eight thousand times, then drawing each ONCE into a
    // picture and reusing it turns the expensive thing into the cheap one.
    // Our own measurement says images cost nothing here and text costs 44%,
    // so the ratio below decides whether that trade is worth making.
    try {
      var svgT = ws.getParentSvg();
      var ts = svgT ? svgT.querySelectorAll('.blocklyBlockCanvas text') : [];
      var distinct = {}, total = 0, longest = 0;
      for (var ti = 0; ti < ts.length; ti++) {
        var body = ts[ti].textContent || '';
        total++;
        if (body.length > longest) longest = body.length;
        distinct[body] = (distinct[body] || 0) + 1;
      }
      var keys = Object.keys(distinct);
      keys.sort(function (a, b) { return distinct[b] - distinct[a]; });
      out.text = {
        nodes: total,
        distinct: keys.length,
        reuse: keys.length ? Math.round(total / keys.length * 10) / 10 : 0,
        longest: longest,
        commonest: keys.slice(0, 6).map(function (k) {
          return { text: k.slice(0, 22), drawn: distinct[k] };
        })
      };
    } catch (e) {}

    // WHAT ELSE IS BEING PAINTED. A filter is the most expensive thing an svg
    // can carry and the easiest to leave switched on by accident; a grid
    // repaints across the whole viewport; scrollbars redraw on every scroll.
    // None of them show up in a node count, so they are counted directly.
    try {
      var root = ws.getParentSvg();
      var filtered = 0, shadowed = 0;
      var all = root ? root.querySelectorAll('*') : [];
      for (var fi = 0; fi < all.length; fi++) {
        var f = all[fi].getAttribute && all[fi].getAttribute('filter');
        if (f && f !== 'none') filtered++;
      }
      out.painting = {
        filtered: filtered,
        defsFilters: root ? root.querySelectorAll('filter').length : -1,
        gridPattern: root ? root.querySelectorAll('pattern').length : -1,
        scrollbars: root ? root.querySelectorAll('.blocklyScrollbarHandle').length : -1,
        cursors: root ? root.querySelectorAll('.blocklyCursor, .blocklyMarker').length : -1,
        highlight: root ? root.querySelectorAll('.blocklyHighlightedConnectionPath').length : -1
      };
    } catch (e) {}

    var frames = [];
    var startScroll = null;
    try { startScroll = { x: ws.scrollX, y: ws.scrollY }; } catch (e) {}

    var STEPS = 45;
    var i = 0;
    var last = 0;
    function step(t) {
      if (last) frames.push(Math.round((t - last) * 100) / 100);
      last = t;
      if (i < STEPS) {
        // A real pan, the same one every time: Blockly's own scroll path, so
        // everything a drag would trigger is triggered.
        try { ws.scroll(startScroll.x - i * 12, startScroll.y - i * 6); } catch (e) {}
        i++;
        requestAnimationFrame(step);
        return;
      }
      try { ws.scroll(startScroll.x, startScroll.y); } catch (e) {}
      if (scaleWas !== null) { try { ws.setScale(scaleWas); } catch (e) {} }

      var sorted = frames.slice().sort(function (a, b) { return a - b; });
      var sum = 0;
      frames.forEach(function (f) { sum += f; });
      out.frames = {
        count: frames.length,
        first5: frames.slice(0, 5),
        mean: frames.length ? Math.round(sum / frames.length * 100) / 100 : 0,
        median: sorted.length ? sorted[Math.floor(sorted.length / 2)] : 0,
        p95: sorted.length ? sorted[Math.floor(sorted.length * 0.95)] : 0,
        worst: sorted.length ? sorted[sorted.length - 1] : 0,
        over16ms: frames.filter(function (f) { return f > 16.7; }).length,
        over33ms: frames.filter(function (f) { return f > 33.3; }).length
      };
      done(out);
    }
    requestAnimationFrame(step);
  }

  function perfLine(p) {
    if (!p) return 'nothing measured';
    var f = p.frames || {};
    return (p.blocks || 0) + ' blocks, ' + (p.rendered || 0) + ' rendered, ' +
      ((p.nodes && p.nodes.inWorkspaceSvg) || 0) + ' svg nodes (' +
      (p.nodesPerBlock || 0) + ' per block, ' +
      ((p.nodes && p.nodes.paths) || 0) + ' paths, ' +
      ((p.nodes && p.nodes.texts) || 0) + ' texts, ' +
      ((p.nodes && p.nodes.images) || 0) + ' images) | pan frames mean ' +
      f.mean + 'ms median ' + f.median + ' p95 ' + f.p95 + ' worst ' + f.worst +
      ', over 16.7ms ' + f.over16ms + '/' + f.count +
      ' | first ' + JSON.stringify(f.first5) +
      ' | text ' + (p.textDrawn === false ? 'HIDDEN, too small to read' : 'drawn') +
      (p.text ? (' ' + p.text.nodes + ' nodes from ' + p.text.distinct +
        ' distinct strings, each drawn ' + p.text.reuse + ' times') : '') +
      (p.painting ? (' | filters ' + p.painting.filtered + ' on elements, ' +
        p.painting.defsFilters + ' defined, patterns ' + p.painting.gridPattern +
        ', scrollbar handles ' + p.painting.scrollbars) : '') +
      ' | listeners ' + ((p.options && p.options.listeners) || 0) +
      ' scale ' + ((p.options && p.options.scale) || '?');
  }

  // THE EXTRA CLASSES THE SITE PUTS ON A FIELD'S TEXT.
  //
  // The site paints particular labels through a class:
  //   .blocklyText.subroutineBlockSubroutineText { fill: #ed7f33 }
  //   .blocklyText.fieldHeaderText { font-family: Purista-Semibold }
  // FieldLabel keeps that class in a property, but the site's minifier
  // renames it, so asking a headless probe block for it comes back null
  // every time. The RENDERED text element carries it in plain sight, which
  // no minifier can touch, so it is read from there instead.
  //
  // Keyed by block type and then by the field's name, or by its words when it
  // has no name, because an unnamed label IS its words.
  function fieldClasses(B, ws) {
    var out = {};
    var all = [];
    try { all = ws.getAllBlocks(false); } catch (e) { return out; }
    for (var i = 0; i < all.length; i++) {
      var b = all[i];
      if (out[b.type] && out[b.type].__done) continue;
      var rec = out[b.type] || (out[b.type] = {});
      var any = false;
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          var g = null, t = null;
          try { g = f.getSvgRoot ? f.getSvgRoot() : null; } catch (e) {}
          try { t = g && g.querySelector('text'); } catch (e) {}
          if (!t) return;
          any = true;
          var cls = classOf(t).split(/\s+/).filter(function (c) {
            return c && c !== 'blocklyText' && c !== 'blocklyDropdownText';
          });
          if (!cls.length) return;
          var words = '';
          try { words = String(f.getText ? f.getText() : ''); } catch (e) {}
          var key = f.name || ('#' + words);
          rec[key] = cls.join(' ');
        });
      });
      if (any) rec.__done = 1;
    }
    // Types that turned out to carry nothing extra are not worth sending.
    Object.keys(out).forEach(function (t) {
      delete out[t].__done;
      if (!Object.keys(out[t]).length) delete out[t];
    });
    return out;
  }

  // HOW WIDE THIS FONT DRAWS A HANDFUL OF CHARACTERS.
  //
  // Two Text blocks out of 272 came out 12px narrower than the site's, and
  // they are exactly the two whose words contain a question mark. That points
  // at one glyph, not at a layout rule, and a glyph is measurable: the same
  // strings are measured in the same font on both sides and the numbers are
  // compared. If our '?' is narrower, the mirrored font does not carry it and
  // the browser is substituting one from somewhere else.
  // THE WORDS A BLOCK IS WRITTEN WITH, PER TYPE, COUNTED.
  //
  // 218 variableReferenceBlocks measured 8.55px narrow, every spacer around
  // them identical, and the whole difference was one label: the site writes
  // 'For' where we had written 'of'. That is not a layout bug at all, it is
  // the wrong WORD, and no amount of reading paddings would ever have found
  // it - a width is a symptom, the text is the cause.
  //
  // variableReferenceBlock is one of the hand-written types, so it is not in
  // the 604 site readings and the live page is the only evidence there is.
  // One block saying 'For' is an anecdote, so every instance of every type is
  // read and the words are counted. Both sides run this, so the comparison is
  // of two censuses rather than of two examples.
  function labelWords(B, ws) {
    var out = {};
    var all = [];
    try { all = ws.getAllBlocks(false); } catch (e) { return out; }
    for (var i = 0; i < all.length; i++) {
      var b = all[i];
      var rec = out[b.type] || (out[b.type] = {});
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          // Only the fixed writing on the block. A dropdown or a text box
          // holds the user's data, which differs per block by design and
          // would bury the words that do not.
          var editable = true;
          try { editable = (f.EDITABLE !== undefined) ? !!f.EDITABLE : true; } catch (e) {}
          if (editable) return;
          var t = '';
          try { t = String(f.getText ? f.getText() : ''); } catch (e) {}
          if (!t) return;
          var key = (f.name || '#') + '=' + t;
          rec[key] = (rec[key] || 0) + 1;
        });
      });
    }
    Object.keys(out).forEach(function (t) {
      if (!Object.keys(out[t]).length) delete out[t];
    });
    return out;
  }

  function fontProbe(ws) {
    var out = { font: '', widths: {} };
    try {
      var t = document.querySelector('.blocklyBlockCanvas text');
      if (!t) return out;
      var cs = window.getComputedStyle(t);
      out.font = cs.fontStyle + ' ' + cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.fontFamily;
      var c = document.createElement('canvas').getContext('2d');
      c.font = out.font;
      ['?', 'x', '??', 'Click on ? for more info', 'Click on x for more info']
        .forEach(function (str) {
          out.widths[str] = Math.round(c.measureText(str).width * 100) / 100;
        });
      // THE HEADER FACE, WHICH IS A DIFFERENT FONT FROM THE BODY ONE.
      //
      // Mod, RULE, Conditions and Actions are drawn in Purista-Semibold at
      // weight 600, not in the body face, so the body reading above says
      // nothing about them. A face that failed to load falls back silently
      // and the words come out a different width and a different height with
      // no error anywhere.
      //
      // Both are measured: what the browser lays the real element out at,
      // and what a canvas makes of the same string in the same font. Those
      // two disagreeing is what a text picture drawn in the wrong face looks
      // like.
      out.header = (function () {
        var h = { found: false };
        try {
          var el = document.querySelector('.blocklyText.fieldHeaderText');
          if (!el) return h;
          h.found = true;
          var hs = window.getComputedStyle(el);
          h.font = hs.fontStyle + ' ' + hs.fontWeight + ' ' + hs.fontSize + ' ' + hs.fontFamily;
          h.family = hs.fontFamily;
          h.weight = hs.fontWeight;
          h.size = hs.fontSize;
          h.text = String(el.textContent || '').slice(0, 24);
          try {
            var bb = el.getBBox();
            h.drawn = { w: Math.round(bb.width * 100) / 100,
                        h: Math.round(bb.height * 100) / 100 };
          } catch (e) {}
          var hc = document.createElement('canvas').getContext('2d');
          hc.font = h.font;
          h.canvas = Math.round(hc.measureText(h.text).width * 100) / 100;
          // WHAT THE NAME ACTUALLY RESOLVES TO.
          //
          // The site declares 46 faces and not one of them is Purista, so
          // 'Purista-Semibold, sans-serif' there is a name with nothing
          // behind it falling through to the system sans. check() says true
          // for it anyway, because it answers about whether SOMETHING can
          // draw the text, not about whether that font is present.
          //
          // The only way to tell is to measure the same string under each
          // candidate and see which one the real element agrees with.
          h.candidates = {};
          [['asked', h.font],
           ['sans', hs.fontStyle + ' ' + hs.fontWeight + ' ' + hs.fontSize + ' sans-serif'],
           ['purista-only', hs.fontStyle + ' ' + hs.fontWeight + ' ' + hs.fontSize + ' "Purista-Semibold"'],
           ['bftitle', hs.fontStyle + ' ' + hs.fontWeight + ' ' + hs.fontSize + ' "BFTitle-Semi-Bold"'],
           // BFHead-Light is the other substituted face. The rule that wants
           // it is ._blocklyHelp_ h1..h6 { font-family: "BFHead-Light",
           // var(--font-fallback) }, and --font-fallback is sans-serif, so
           // the same question applies: does that name resolve to anything on
           // the site, or does it fall through? Measured at a plain weight so
           // the reading stands on its own rather than borrowing the header
           // row's 600.
           ['head-stack', 'normal 400 16px "BFHead-Light", sans-serif'],
           ['head-only', 'normal 400 16px "BFHead-Light"'],
           ['head-sans', 'normal 400 16px sans-serif'],
           ['head-bftext-light', 'normal 400 16px "BFText-Light"']
          ].forEach(function (pair) {
            try {
              var cc = document.createElement('canvas').getContext('2d');
              cc.font = pair[1];
              h.candidates[pair[0]] = {
                font: cc.font,
                Mod: Math.round(cc.measureText('Mod').width * 100) / 100,
                Heading: Math.round(cc.measureText('Getting started').width * 100) / 100,
                Conditions: Math.round(cc.measureText('Conditions').width * 100) / 100
              };
            } catch (e) {}
          });
          h.widths = {};
          ['Mod', 'RULE', 'Conditions', 'Actions', 'SUBROUTINE'].forEach(function (s) {
            h.widths[s] = Math.round(hc.measureText(s).width * 100) / 100;
          });
          // Ask by name whether each face the page asks for is really here.
          h.loaded = {};
          try {
            String(hs.fontFamily).split(',').forEach(function (fam) {
              var name = fam.trim().replace(/["']/g, '');
              if (!name) return;
              h.loaded[name] = document.fonts
                ? document.fonts.check(hs.fontWeight + ' ' + hs.fontSize + ' "' + name + '"')
                : null;
            });
          } catch (e) {}
          try { h.faces = document.fonts ? document.fonts.size : null; } catch (e) {}
        } catch (e) {}
        return h;
      })();

      // EVERY FACE THE PAGE DECLARES, AND WHERE IT COMES FROM.
      //
      // Purista-Semibold measures 40% narrower on our side than on the
      // site's while document.fonts.check says true for both - because we
      // declare a face under that NAME pointing at a different file
      // (BF_TITLE_SEMI-BOLD.ttf), so the name resolves and the glyphs are
      // somebody else's. check() answers about the name, never about the
      // shapes, so it cannot see this and neither could I until the widths
      // were compared string by string.
      //
      // The mirror has no Purista at all, so the first thing needed is where
      // the site fetches it from. A CSSFontFaceRule carries that in its src.
      out.faces = (function () {
        var list = [];
        try {
          var sheets = document.styleSheets;
          for (var i = 0; i < sheets.length; i++) {
            var rules = null;
            try { rules = sheets[i].cssRules; } catch (e) { continue; }
            if (!rules) continue;
            for (var r = 0; r < rules.length; r++) {
              var rule = rules[r];
              if (!rule || rule.type !== 5) continue;   // CSSFontFaceRule
              var st = rule.style || {};
              list.push({
                family: String(st.getPropertyValue ? st.getPropertyValue('font-family') : '').trim(),
                weight: String(st.getPropertyValue ? st.getPropertyValue('font-weight') : '').trim(),
                style: String(st.getPropertyValue ? st.getPropertyValue('font-style') : '').trim(),
                src: String(st.getPropertyValue ? st.getPropertyValue('src') : '').slice(0, 400)
              });
            }
          }
        } catch (e) {}
        return list;
      })();

      // And what the browser says it actually used.
      out.families = cs.fontFamily;
      try { out.loaded = document.fonts ? document.fonts.check(cs.fontSize + " '" + String(cs.fontFamily).split(',')[0].replace(/["']/g, '') + "'") : null; } catch (e) {}
    } catch (e) {}
    return out;
  }

  function layoutLine(L) {
    if (!L) return 'nothing measured';
    return (L.blocks || []).map(function (b) {
      if (b.error) return b.type + ' FAILED: ' + b.error;
      return b.type + ' ' + b.measured.w + 'x' + b.measured.h +
        ' in ' + (b.rows || []).length + ' row(s)';
    }).join('; ') + (L.missing.length ? ' | not found: ' + L.missing.join(', ') : '') +
      ' | text ' + (L.font ? (L.font.size + ' ' + L.font.weight + ' ' + L.font.family) : 'unknown');
  }

  function line(m) {
    if (!m) return 'nothing to measure';
    var f = m.flyout || {};
    return m.classes.toolbox + ' / ' + m.classes.flyout + ' / ' + m.classes.category +
      ', opened "' + m.openedCategory + '"' + (m.openFailed ? ' FAILED: ' + m.openFailed : '') +
      ' -> flyout ' + f.width + 'x' + f.height + ' holding ' + f.blocks +
      ' block(s) at scale ' + f.scale +
      ', margin ' + f.MARGIN + ' gap ' + f.GAP_X + '/' + f.GAP_Y +
      ', ' + m.nodes.filter(function (n) { return n.present; }).length + ' of ' +
      m.nodes.length + ' elements found, ' + m.cssRules + ' css rule(s) that apply';
  }

  window.BF6Measure = {
    LOOK_PROPS: LOOK_PROPS,
    looksOf: looksOf,
    rulesMatching: rulesMatching,
    describe: describe,
    toolbox: toolbox,
    sockets: sockets,
    socketRules: socketRules,
    layout: layout,
    TYPES_TO_MEASURE: TYPES_TO_MEASURE,
    sweep: sweep,
    fieldLook: fieldLook,
    diagnose: diagnose,
    fieldClasses: fieldClasses,
    labelWords: labelWords,
    domCensus: domCensus,
    applyingRules: applyingRules,
    pageControls: pageControls,
    perf: perf,
    perfLine: perfLine,
    layoutLine: layoutLine,
    line: line
  };
})();
