// ============================================================================
// BF6 BLOCKS - the page.
//
// Browser only: the workspace, the navigator, the slot search, the minimap and
// the wire back to the tool. Everything that has to survive a headless test
// lives in editor.js instead.
// ============================================================================
(function () {
  'use strict';

  var BF6 = window.BF6Blocks;
  var Blockly = window.Blockly;
  BF6.attach(Blockly);

  var UI = {
    ws: null,
    seq: 0,
    journal: {},             // seq -> unit the site has not acknowledged yet
    sessionLost: false,
    autosaveTimer: null,
    debounce: null,
    applying: 0,
    chunks: {},
    ruleMode: null,          // block id shown alone, or null
    region: null,            // the source file shown alone, or null
    regionStash: null,
    allRegions: null,
    backStack: [],           // where following a call came from
    ruleModeStash: null,
    searchHits: [],
    searchAt: -1,
    highlight: {},           // block id -> reason
    snippets: [],
    lastStatus: {}
  };
  window.BF6UI = UI;

  function $(id) { return document.getElementById(id); }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  // ---- the wire -----------------------------------------------------------
  function bridge() { return (window.ue && window.ue.bf6blocks) ? window.ue.bf6blocks : null; }
  var CHUNK = 256 * 1024;
  /* NEVER CALL INTO UNREAL FROM INSIDE AN EVENT HANDLER.
   *
   * This is the bug that froze the editor on every toolbox click, and it took
   * a long time to find because it looks like everything else.
   *
   * window.ue.bf6blocks.msg is a call from the page into the editor's own
   * process. Clicking a toolbox category runs inside Unreal's browser event
   * dispatch, on the game thread. Calling back into that thread while it is
   * still inside the dispatch deadlocks: the editor stops answering, the page
   * stops painting, the console times out, and even Chrome's own remote
   * debugger hangs with it, because they all share the thread.
   *
   * Proven with a stock Blockly page carrying nothing of ours: it opened
   * categories perfectly, and adding one bridge call per workspace event froze
   * it after TWO calls. Not volume, not Blockly, not the renderer, not the
   * stylesheet, not the number of blocks - all of which were tried first.
   *
   * The fix is to let the stack unwind. Messages are queued and flushed from a
   * timer, so the native call always happens with the event dispatch finished.
   * Order is preserved, which some messages depend on, and a failed send no
   * longer takes the rest of the queue with it.
   */
  var OUT = [];
  var outTimer = null;

  function flushOut() {
    outTimer = null;
    var b = bridge();
    if (!b || !b.msg) { OUT.length = 0; return; }
    /* Taken in one go: a handler reached from a message must not append to the
     * batch being drained, or a busy page never leaves this loop. */
    var batch = OUT;
    OUT = [];
    for (var i = 0; i < batch.length; i++) {
      try { b.msg(batch[i]); } catch (e) {}
    }
  }

  function queue(text) {
    OUT.push(text);
    if (outTimer === null) { outTimer = setTimeout(flushOut, 0); }
  }

  /* STRAIGHT ACROSS, NO QUEUE. For the handful of lines whose whole purpose is
   * to survive a hang.
   *
   * send() queues and flushes from a timer, which is right for ordinary
   * traffic. It is useless for tracing a freeze: the queue never flushes,
   * every breadcrumb is lost, and the log then says nothing happened when in
   * fact plenty did. That ambiguity has already cost two rounds here.
   *
   * A direct call is safe. The deadlock earlier today was a GLog->Flush() in
   * the C++ handler, not the call itself; the same page sends these deferred
   * and synchronously with no difference once that flush was removed. */
  function sendNow(obj) {
    obj.from = 'tool';
    var b = bridge();
    if (!b || !b.msg) { return; }
    try { b.msg(JSON.stringify(obj)); } catch (e) {}
  }

  function trace(text) {
    sendNow({ op: 'log', level: 'display', text: text });
  }

  function send(obj) {
    obj.from = 'tool';
    if (!bridge()) { return; }
    var text = JSON.stringify(obj);
    if (text.length <= CHUNK) { queue(text); return; }
    var id = 'c' + (++UI.seq) + '_' + Date.now();
    var n = Math.ceil(text.length / CHUNK);
    for (var i = 0; i < n; i++) {
      queue(JSON.stringify({ from: 'tool', op: 'chunk', cid: id, i: i, n: n,
                             part: text.substr(i * CHUNK, CHUNK) }));
    }
  }
  UI.send = send;

  // EVERY FAILURE IN THIS PAGE HAS BEEN SILENT.
  //
  // console.log from here reaches nothing, and a throw inside a Blockly
  // callback - building a flyout, rendering a block - is swallowed by Blockly
  // and shows up as a feature that simply stopped working. There was no way to
  // tell "the toolbox will not open" from "the toolbox threw on the first
  // block", which is a long way to walk for one line of text.
  //
  // So anything that gets away goes down the same wire everything else uses.
  //
  // THE GLOBAL HANDLER ALONE IS NOT ENOUGH HERE, and it is worth knowing why
  // before trusting it: this page is served from file:///, so the browser
  // treats every one of its scripts as another origin and hands window.onerror
  // the sanitised string "Script error." with no message, no file and no line.
  // It is proof that SOMETHING threw and nothing more. reportFault is the half
  // that carries the detail, from a catch inside our own code, and every place
  // that can swallow a throw calls it.
  var said = {};
  function sayFault(what) {
    if (!what || said[what]) return;         // one line per distinct fault
    said[what] = 1;
    send({ op: 'log', level: 'error', text: 'page error: ' + what });
  }
  UI.reportFault = function (doing, e) {
    var msg = String((e && e.message) || e);
    var stack = (e && e.stack) ? ' | ' + String(e.stack).split('\n').slice(0, 4)
      .map(function (l) { return l.trim(); }).join(' <- ') : '';
    sayFault(doing + ': ' + msg + stack);
  };
  (function reportEscapes() {
    function say(what) { sayFault(what); }
    window.addEventListener('error', function (e) {
      var where = e.filename ? (' at ' + String(e.filename).split('/').pop() + ':' + e.lineno) : '';
      say(String((e.error && e.error.message) || e.message || 'unknown') + where +
        ((e.error && e.error.stack) ? ' | ' + String(e.error.stack).split('\n').slice(0, 3).join(' <- ') : ''));
    });
    window.addEventListener('unhandledrejection', function (e) {
      var r = e.reason;
      say('unhandled rejection: ' + String((r && r.message) || r));
    });
  })();

  function status(text, bad) {
    var s = $('status');
    if (!s) return;
    s.textContent = text;
    s.className = bad ? 'status bad' : 'status';
  }

  // ---- definitions and look ----------------------------------------------
  function applyCapture(msg) {
    // TWO DIFFERENT THINGS ARRIVE UNDER 'definitions'.
    //
    // One is the site's mod catalogue: eight arrays called objects, events,
    // values, actions, selectionLists, controlActions, types and constraints,
    // holding every function the game exposes with its parameter names and
    // return types. It is not keyed by block type and it is not a block.
    //
    // The other, under 'synthesized', is one reading per block type.
    //
    // Merging them put 'objects' and 'events' in as block types and set the
    // count to 632 when 604 was the truth. The catalogue is kept whole and
    // apart, where it says what it is.
    var defs = {};
    if (msg.definitions) {
      for (var k in msg.definitions) {
        var v = msg.definitions[k];
        var isBlock = v && typeof v === 'object' && !Array.isArray(v)
          && (Array.isArray(v.inputs) || v.message0 !== undefined);
        if (isBlock) defs[k] = v;
        else { BF6.state.catalogue = BF6.state.catalogue || {}; BF6.state.catalogue[k] = v; }
      }
    }
    if (msg.synthesized) for (var k2 in msg.synthesized) if (!defs[k2]) defs[k2] = msg.synthesized[k2];
    BF6.state.tooltips = msg.tooltips || {};
    BF6.state.icons = msg.icons || {};
    BF6.state.helpUrls = msg.helpUrls || {};
    BF6.state.helpLinks = msg.helpLinks || [];
    BF6.state.siteOptions = msg.options || null;
    BF6.state.theme = msg.theme || null;
    BF6.state.toolbox = msg.toolbox || null;
    BF6.state.contextMenuIds = msg.contextMenuIds || [];
    if (Object.keys(defs).length) BF6.installDefinitions(defs, msg.source || 'live');

    /* THE CONVERTER NEEDS THESE TOO, AND NOBODY WAS GIVING THEM TO IT.
     *
     * convert.js has a pass, ensureSocketTypes, that empties any socket whose
     * type cannot accept what was put in it, so a workspace always loads with a
     * visible hole instead of being refused whole. That pass reads its types
     * from setBlockChecks, and the PAGE never called it: only the offline test
     * harness did.
     *
     * So every conversion done inside the editor ran with that safety net
     * switched off. Importing UGZ's src produced an Add block in
     * AddUIWeaponImage's String socket - a widget name built with
     * "prefix_" + playerId - and Blockly refused all 55,463 blocks over it. The
     * same conversion offline, with the checks supplied, emptied that one
     * socket and loaded.
     *
     * These are the same records the editor builds its own blocks from, so the
     * converter and the editor now agree about what fits where by construction
     * rather than by two files being kept in step by hand.
     */
    try {
      if (window.BF6Convert && window.BF6Convert.setBlockChecks) {
        window.BF6Convert.setBlockChecks(defs);
        trace('converter: socket type checks installed for ' +
              Object.keys(defs).length + ' block type(s)');
      }
    } catch (e) { UI.reportFault('giving the converter its socket types', e); }
    BF6.buildSignatures(BF6.state.tooltips);
    if (BF6.state.toolbox) BF6.buildCategoryIndex(BF6.state.toolbox);
    rebuildWorkspace();
    status('definitions: ' + BF6.state.definitionsSource + ', ' +
      Object.keys(defs).length + ' types');

    // AND SAY IT WHERE IT CAN BE READ AFTERWARDS. This only ever went to a
    // status strip in the page, so "the blocks do not match the site" and "the
    // site's definitions never installed" looked identical from outside: the
    // editor knew which it was and had nowhere to say so.
    var readings = 0, ready = 0;
    for (var d in defs) {
      if (!Object.prototype.hasOwnProperty.call(defs, d)) continue;
      if (BF6.isProbeRecord(defs[d])) readings++; else ready++;
    }
    send({
      op: 'log',
      text: 'definitions: ' + BF6.state.definitionsSource + ', ' +
        Object.keys(defs).length + ' types (' + readings +
        ' converted from the site reading, ' + ready + ' already Blockly JSON)' +
        (BF6.state.catalogue
          ? ', catalogue ' + Object.keys(BF6.state.catalogue).join('/')
          : ', no catalogue')
    });
  }

  function offlineDefinitions(wsJson) {
    var types = window.BF6_TYPES_FALLBACK || [];
    BF6.installFallback(types, BF6.observe(wsJson || { blocks: { blocks: [] } }));
    BF6.buildSignatures(null);
    BF6.state.toolbox = BF6.state.toolbox || window.BF6_TOOLBOX_FALLBACK || null;
    if (BF6.state.toolbox) BF6.buildCategoryIndex(BF6.state.toolbox);
  }

  // ---- toolbox ------------------------------------------------------------
  // The site's toolbox, converted to what Blockly wants here: its categories,
  // its labels, its ready made flyout blocks. A SEARCH category at the top
  // filters the same list by name, signature and category.
  function toolboxFor() {
    var src = BF6.state.toolbox;
    var contents = [];
    // The site's own search results bin: a category at the very top that is
    // never shown as a row and exists only to hold what the search bar found.
    // The bar itself is a real text input at the top of the toolbox, built in
    // installSearchbar below, which is where the site puts it.
    contents.push({
      kind: 'category', name: 'SEARCH RESULTS', colour: '#004444',
      toolboxitemid: SEARCH_BIN_ID, contents: []
    });
    function conv(node) {
      var kind = String(node.kind || '').toLowerCase();
      if (kind === 'category') {
        var o = {
          kind: 'category', name: node.name || '',
          contents: (node.contents || []).map(conv).filter(Boolean)
        };
        if (node.categorystyle) o.categorystyle = node.categorystyle;
        if (node.colour) o.colour = node.colour;
        if (node.cssconfig) o.cssconfig = node.cssconfig;
        // VARIABLES and SUBROUTINES carry no blocks of their own: the site
        // fills them at open time from what the workspace actually holds. The
        // attribute that says so used to be dropped here, which is why both
        // categories opened empty.
        if (node.custom) o.custom = node.custom;
        if (String(node.hidden) === 'true') return null;   // the site's own hidden search bin
        if (String(node.expanded) === 'true') o.expanded = true;
        return o;
      }
      if (kind === 'sep') return { kind: 'sep' };
      if (kind === 'label') return { kind: 'label', text: node.text || node.name || '' };
      if (kind === 'block') {
        if (node.blockstate && node.blockstate.type && Blockly.Blocks[node.blockstate.type]) {
          var b = JSON.parse(JSON.stringify(node.blockstate));
          b.kind = 'block';
          return b;
        }
        if (node.type && Blockly.Blocks[node.type]) return { kind: 'block', type: node.type };
        return null;
      }
      return null;
    }
    if (src && src.contents) {
      src.contents.forEach(function (c) { var o = conv(c); if (o) contents.push(o); });
    } else {
      Object.keys(Blockly.Blocks).sort().forEach(function (t) {
        contents.push({ kind: 'block', type: t });
      });
    }
    return { kind: 'categoryToolbox', contents: contents };
  }

  // ==========================================================================
  // The toolbox search bar
  //
  // The site puts a real text input at the top of the toolbox, above the
  // categories, and types into it to filter every block. We used to put a
  // SEARCH category there instead, whose flyout said "type in the box above
  // the canvas" and pointed at an input in a collapsed side panel. That is a
  // sign, not a search bar. This is the site's own arrangement: input at the
  // top of the toolbox, 100ms debounce, results into a hidden category.
  // ==========================================================================
  var SEARCH_BIN_ID = 'bf6-search-results';
  var SEARCHBAR_CLASS = 'searchbar';       // the site's own class names, so the
  var TOOLBAR_CLASS = 'toolbar';           // captured stylesheet dresses them

  // Every block the toolbox offers, in the order it offers them, so a search
  // result sits in the same order the creator saw it in the categories.
  function searchTargets() {
    var out = [];
    var seen = {};
    function walk(node) {
      if (!node) return;
      var kind = String(node.kind || '').toUpperCase();
      if (kind === 'BLOCK') {
        var t = node.type || (node.blockstate && node.blockstate.type);
        if (t && !seen[t] && Blockly.Blocks[t]) {
          seen[t] = 1;
          out.push({ type: t, displayName: node.displayName || node.displayname || '' });
        }
        return;
      }
      (node.contents || []).forEach(walk);
    }
    var src = BF6.state.toolbox;
    if (src && src.contents) src.contents.forEach(walk);
    // Nothing to walk offline: every type Blockly knows, alphabetically.
    if (!out.length) {
      Object.keys(Blockly.Blocks).sort().forEach(function (t) {
        out.push({ type: t, displayName: '' });
      });
    }
    return out;
  }

  // Rank a query against one block. A run of the query as typed beats letters
  // merely scattered through the name, and a hit at the start beats one in the
  // middle, which is what makes SetPlayer* come up first for "setplay".
  function scoreMatch(query, hay) {
    if (!hay) return -1;
    var h = hay.toLowerCase();
    var i = h.indexOf(query);
    if (i === 0) return 1000;
    if (i > 0) return 800 - i;
    // subsequence: every letter in order, anywhere
    var qi = 0, gaps = 0, last = -1;
    for (var k = 0; k < h.length && qi < query.length; k++) {
      if (h[k] === query[qi]) {
        if (last >= 0) gaps += k - last - 1;
        last = k;
        qi++;
      }
    }
    if (qi < query.length) return -1;
    return 400 - Math.min(gaps, 300);
  }

  function searchResults(raw) {
    var q = String(raw || '').trim().toLowerCase();
    if (!q) return [];
    var scored = [];
    searchTargets().forEach(function (t) {
      var best = Math.max(
        scoreMatch(q, t.displayName),
        scoreMatch(q, t.type));
      // The tooltip carries the signature, so "player" also finds every block
      // that takes one. Ranked below a name hit, never above it.
      if (best < 0) {
        var tip = BF6.stripTags(BF6.tooltipOf(t.type));
        if (tip && tip.toLowerCase().indexOf(q) >= 0) best = 100;
      }
      if (best < 0) {
        var cat = BF6.categoryOf(t.type);
        if (cat && cat.toLowerCase().indexOf(q) >= 0) best = 50;
      }
      if (best >= 0) scored.push({ type: t.type, score: best });
    });
    scored.sort(function (a, b) {
      if (b.score !== a.score) return b.score - a.score;
      return a.type < b.type ? -1 : a.type > b.type ? 1 : 0;
    });
    return scored.slice(0, 120).map(function (s) {
      return { kind: 'block', type: s.type };
    });
  }
  UI.searchResults = searchResults;

  function searchBin() {
    try { return UI.ws.getToolbox().getToolboxItemById(SEARCH_BIN_ID); }
    catch (e) { return null; }
  }

  function clearSearchResult() {
    try {
      var fo = UI.ws.getFlyout();
      if (fo) fo.hide();
    } catch (e) {}
    var bin = searchBin();
    if (bin && bin.hide) { try { bin.hide(); } catch (e) {} }
  }

  function runToolboxSearch(text) {
    UI.toolboxQuery = text;
    try {
      var panelBox = document.getElementById('toolsearch');
      if (panelBox && panelBox.value !== text) panelBox.value = text;
    } catch (e) {}
    var q = String(text || '').trim();
    if (!q) { clearSearchResult(); return 0; }
    var found = searchResults(q);
    var bin = searchBin();
    if (!bin) return 0;
    if (!found.length) {
      // Say so in the flyout rather than leaving the last result standing.
      found = [{ kind: 'label', text: 'No block matches "' + q + '"' }];
    }
    try {
      bin.updateFlyoutContents(found);
      if (bin.show) bin.show();
      UI.ws.getToolbox().setSelectedItem(bin);
    } catch (e) { return 0; }
    return found.length;
  }
  UI.runToolboxSearch = runToolboxSearch;

  // ==========================================================================
  // The flyout has to be as wide as the blocks in it
  //
  // Blockly sizes a category flyout from the widths it measured when it built
  // it, and a block's width comes from its rendered TEXT. Two things move
  // after that measurement: the site's fonts arrive and every label gets
  // wider, and the type symbols now in each socket add to the row. Blocks
  // measured before either lands overflow the panel they were laid out in and
  // run into each other, which is the stacking.
  //
  // The site does not cap its flyout, and neither do we: Blockly is asked to
  // measure again once things have settled. The floor is the site's own
  // minimum for a block flyout, so a category of short blocks still opens to a
  // panel worth reading rather than a sliver.
  // ==========================================================================
  var FLYOUT_MIN_WIDTH = 400;

  // THE SITE'S OWN SPACING, MEASURED OFF THE SITE.
  //
  // These were Blockly's defaults, and Blockly's defaults are not the site's:
  // read off the live page, the Portal flyout runs MARGIN 8, GAP_X 24,
  // GAP_Y 24, CORNER_RADIUS 8 against stock 20 / 8 / 24 / 8. Blockly measures
  // the pull-out from its contents PLUS these, so getting them wrong makes the
  // panel the wrong width around blocks that are the right size, which is
  // exactly the "tiny window instead of a pull-out" the site is not.
  var SITE_FLYOUT = { MARGIN: 8, GAP_X: 24, GAP_Y: 24, CORNER_RADIUS: 8 };

  function installFlyoutSizing() {
    var fo = null;
    try { fo = UI.ws.getFlyout(); } catch (e) { fo = null; }
    if (!fo || fo.bf6Sized) return false;
    fo.bf6Sized = true;

    Object.keys(SITE_FLYOUT).forEach(function (k) { fo[k] = SITE_FLYOUT[k]; });

    var origReflow = fo.reflow;
    fo.reflow = function () {
      origReflow.call(this);
      if (this.horizontalLayout) return;              // only the vertical one
      if (this.width_ < FLYOUT_MIN_WIDTH) {
        this.width_ = FLYOUT_MIN_WIDTH;
        try { this.position(); } catch (e) {}
      }
    };

    // Measure again after the browser has actually laid the text out, and once
    // more when the fonts report ready. Both are cheap and both are the moment
    // a width can change underneath us.
    var origShow = fo.show;
    fo.show = function (defs) {
      // A THROW IN HERE IS A CATEGORY THAT DOES NOT OPEN, and Blockly calls
      // this from a click handler where the message goes nowhere. Reported and
      // re-thrown: the fault is not swallowed, it is just no longer silent.
      try { origShow.call(this, defs); }
      catch (e) { UI.reportFault('opening a flyout', e); throw e; }
      var self = this;
      try { self.reflow(); } catch (e) { UI.reportFault('measuring a flyout', e); }
      requestAnimationFrame(function () {
        try { self.reflow(); } catch (e) { UI.reportFault('re-measuring a flyout', e); }
      });
    };

    if (document.fonts && document.fonts.ready && document.fonts.ready.then) {
      document.fonts.ready.then(function () {
        try { if (fo.isVisible()) fo.reflow(); } catch (e) {}
      });
    }
    return true;
  }

  // WHAT THE TOOLBOX IS ACTUALLY DOING, asked from outside.
  //
  // "the categories are not popping out" can be any of five different things
  // and they all look the same from the outside: no toolbox at all, a toolbox
  // with no categories, a category that refuses to select, a flyout that opens
  // at no width, or a flyout that opens behind something. This says which,
  // and it OPENS one to find out rather than reporting the resting state.
  // WHERE THE TIME GOES, asked from outside and answered when it is done.
  // The pan it drives takes about a second, so this cannot answer inline.
  // =========================================================================
  // ONE EXPERIMENT AT A TIME, EACH ONE MEASURED
  //
  // The measured truth is that the SITE pans this project at 82ms a frame and
  // we do it at 58, so we are not behind it: five thousand blocks is simply
  // more than Blockly draws smoothly. Getting past that means beating
  // Blockly, and every idea for doing that is a guess until it is measured.
  // So each one goes on alone, by name, and BF6.Blocks.Perf reads the result.
  // Anything that does not earn its place comes straight back off.
  // =========================================================================
  // Creators choose how far out labels remain visible. The default keeps
  // them to 15% zoom; zero means always visible. This hides text only, never
  // the site's block badges or socket icons. Viewport pruning independently
  // avoids painting off-screen blocks without changing the program.
  var TEXT_FLOOR = 0.15;
  var TEXT_HIDDEN_CLASS = 'bf6-text-too-small';

  function textFloor() {
    var value = BF6.state.prefs && BF6.state.prefs.textFloor;
    var floor = value === undefined ? TEXT_FLOOR : Number(value);
    return Number.isFinite(floor) ? Math.max(0, Math.min(.5, floor)) : TEXT_FLOOR;
  }
  UI.textFloor = textFloor;

  function updateTextFloor() {
    try {
      var div = UI.ws && UI.ws.getInjectionDiv && UI.ws.getInjectionDiv();
      if (!div) return;
      var small = UI.ws.getScale() < textFloor();
      if (small === UI.textHidden && div.classList.contains(TEXT_HIDDEN_CLASS) === small) return;
      UI.textHidden = small;
      div.classList.toggle(TEXT_HIDDEN_CLASS, small);
      // Coming back INTO readable range: the text is drawn again, so it wants
      // picturing again. Going out of range there is nothing to do, because
      // nothing is being drawn.
      if (!small) { try { textPictureSweep(); } catch (e) {} }
    } catch (e) { UI.reportFault('hiding unreadable text', e); }
  }
  UI.updateTextFloor = updateTextFloor;

  var PERF_STYLE_ID = 'bf6-perf-experiment';
  UI.perfOn = {};

  /* The freeze trail, off by default. See installFreezeTrail. */
  UI.trailOn = false;

  var PERF_EXPERIMENTS = {
    // Anti-aliasing every block outline is the browser's job on 5,419 paths.
    // crispEdges asks it to stop.
    fastpaths: '.blocklyPath, .blocklyOutlinePath { shape-rendering: optimizeSpeed; }',

    // The same for 8,259 text nodes: hinting and kerning per glyph is what
    // text-rendering optimizeSpeed turns off.
    fasttext: '.blocklyText, text { text-rendering: optimizeSpeed; }',

    // Tell the compositor the canvas moves, so a pan translates a layer
    // instead of repainting the tree.
    layer: '.blocklyBlockCanvas, .blocklyBubbleCanvas { will-change: transform; }',

    // 5,140 images, every one a data uri, every one composited. This is what
    // it costs to draw none of them.
    noimages: '.blocklyBlockCanvas image { display: none; }',

    // And what it costs to draw no text.
    notext: '.blocklyBlockCanvas text { display: none; }',

    // WHICH HALF OF THE IMAGES COSTS. Once the text is pictured there are two
    // populations: ~8,255 label pictures and ~5,140 type symbols. Hiding all
    // images together says only that images cost; these say which.
    notextpics: 'image.bf6-text-pic { display: none; }',
    noicons: '.blocklyBlockCanvas image:not(.bf6-text-pic) { display: none; }',

    // A picture is never clicked: the field underneath it takes the click.
    // Taking them out of hit testing is 8,000 fewer things to consider on
    // every move of the mouse.
    nohit: 'image.bf6-text-pic, .blocklyBlockCanvas text { pointer-events: none; }',

    // The pictures are drawn at the zoom they will be seen at, so smoothing
    // them again on the way to the screen is work for nothing.
    rawpics: 'image.bf6-text-pic { image-rendering: optimizeSpeed; }',

    // A filter is the most expensive thing an svg element can carry.
    nofilter: '.blocklyBlockCanvas * { filter: none !important; }',

    // Blockly draws a highlight path under the block the mouse is over, which
    // is a repaint per mouse move across a five thousand block canvas.
    nohover: '.blocklyDraggable:hover>.blocklyPath { filter: none; }',

    // The whole canvas is one paint unit: tell the browser nothing outside it
    // is affected by what happens inside.
    contain: '.injectionDiv { contain: layout paint; }',

    // The site's own blur behind the pull-out is a full-screen effect.
    noblur: '.blocklyFlyout { backdrop-filter: none !important; }'
  };

  // =========================================================================
  // TEXT DRAWN AS A PICTURE
  //
  // Measured, not assumed: an svg <text> node is expensive in this compositor
  // and an <image> is nearly free. Hiding all 8,259 texts took the frame from
  // 57ms to 31.9; hiding all 5,140 images changed nothing at all. And the
  // labels repeat - 8,259 text nodes come from only 765 distinct strings, each
  // drawn about eleven times.
  //
  // So each distinct label is drawn ONCE into a canvas and the picture is put
  // on every block that says the same word. Measured at 63ms -> 52ms with the
  // text still fully readable, which is 17 to 22 per cent for nothing given up.
  //
  // Three things this has to get right, and each one is why it was a flag
  // before it was a feature:
  //
  //   STALENESS  a block that re-renders draws its own text again, so the
  //              sweep runs after every batch of renders rather than once.
  //   SHARPNESS  a picture drawn at one size blurs when zoomed past it, so it
  //              is drawn at the workspace's own maximum scale and never
  //              magnified beyond what it was made for.
  //   MEMORY     the cache is capped and thrown away whole when the font
  //              changes, because a font change invalidates every picture.
  //
  // The original <text> stays in the dom, hidden. Cutting it out measured no
  // faster (52.2 against 52.6: the cost is the painting, not the node count),
  // and leaving it there is what keeps Blockly's measuring and editing exact.
  var PIC_CLASS = 'bf6-text-pic';
  var PIC_CAP = 4000;
  var textPics = {};
  var textPicCount = 0;
  var textPicCanvas = null;
  var textPicScale = 0;

  function forgetTextPictures() {
    textPics = {};
    textPicCount = 0;
    textPicScale = 0;
  }
  UI.forgetTextPictures = forgetTextPictures;

  // The scale a picture is drawn at: the furthest in the workspace can zoom,
  // so it is never magnified past what it was made for. Capped, because a
  // maxScale of 20 would be a canvas nobody can afford.
  function pictureScale() {
    var s = 3;
    try {
      var z = UI.ws && UI.ws.options && UI.ws.options.zoomOptions;
      if (z && z.maxScale > 0) s = z.maxScale;
    } catch (e) {}
    return Math.max(1, Math.min(4, s));
  }

  function textPicture(body, font, fill, ss) {
    var key = ss + '|' + font + '|' + fill + '|' + body;
    if (textPics[key]) return textPics[key];
    if (textPicCount >= PIC_CAP) return null;      // never grows without bound
    if (!textPicCanvas) textPicCanvas = document.createElement('canvas');
    var ctx = textPicCanvas.getContext('2d');
    ctx.font = font;
    var m = ctx.measureText(body);
    var w = Math.max(1, Math.ceil(m.width));
    var asc = Math.ceil(m.actualBoundingBoxAscent || 12);
    var desc = Math.ceil(m.actualBoundingBoxDescent || 4);
    var h = Math.max(1, asc + desc);
    textPicCanvas.width = Math.ceil(w * ss);
    textPicCanvas.height = Math.ceil(h * ss);
    ctx = textPicCanvas.getContext('2d');
    ctx.scale(ss, ss);
    ctx.font = font;
    ctx.fillStyle = fill;
    ctx.textBaseline = 'alphabetic';
    ctx.fillText(body, 0, asc);
    var rec = { url: textPicCanvas.toDataURL('image/png'), w: w, h: h, asc: asc };
    textPics[key] = rec;
    textPicCount++;
    return rec;
  }

  function textPicturesOff() {
    var svg = UI.ws && UI.ws.getParentSvg && UI.ws.getParentSvg();
    if (!svg) return 0;
    var olds = svg.querySelectorAll('image.' + PIC_CLASS);
    for (var k = 0; k < olds.length; k++) {
      if (olds[k].bf6For) {
        olds[k].bf6For.style.display = '';
        olds[k].bf6For.bf6Pictured = false;
      }
      if (olds[k].parentNode) olds[k].parentNode.removeChild(olds[k]);
    }
    return olds.length;
  }

  function wantTextPictures() {
    var p = BF6.state.prefs || {};
    // Ordinary text stays editable and follows font/field changes immediately.
    // Viewport pruning removes the off-screen paint cost that motivated PNGs.
    return p.textPictures === undefined ? false : !!p.textPictures;
  }

  // One sweep over whatever is not pictured yet. Cheap to call often: a block
  // already done is one property read.
  function textPictureSweep() {
    if (!wantTextPictures()) return 0;
    // Nothing to picture when the text is not being drawn at all.
    if (UI.textHidden) return 0;
    var svg = UI.ws && UI.ws.getParentSvg && UI.ws.getParentSvg();
    if (!svg) return 0;

    var ss = pictureScale();
    if (ss !== textPicScale) { textPicturesOff(); forgetTextPictures(); textPicScale = ss; }

    var texts = svg.querySelectorAll('.blocklyBlockCanvas text');
    var made = 0;
    for (var i = 0; i < texts.length; i++) {
      var t = texts[i];
      if (t.bf6Pictured) continue;
      var body = t.textContent || '';
      if (!body) continue;
      var cs = window.getComputedStyle(t);
      var font = cs.fontStyle + ' ' + cs.fontWeight + ' ' + cs.fontSize + ' ' + cs.fontFamily;
      var rec = textPicture(body, font, cs.fill, ss);
      if (!rec) break;                              // cache full: leave the rest as text
      // WHERE x AND y MEAN IS NOT WHERE THE PICTURE GOES.
      //
      // An svg <text> is placed by its ANCHOR, and Blockly centres field text:
      // with text-anchor middle the x attribute is the text's centre, not its
      // left edge, and with end it is its right edge. An image has no anchor -
      // its x IS its left edge - so putting the picture at the text's x hung
      // every centred label half its own width to the right of where the words
      // had been. Same for the baseline: alphabetic means y is the baseline,
      // central and middle mean y is the middle of the line.
      var x = parseFloat(t.getAttribute('x') || '0');
      var y = parseFloat(t.getAttribute('y') || '0');
      var anchor = cs.textAnchor || t.getAttribute('text-anchor') || 'start';
      if (anchor === 'middle') x -= rec.w / 2;
      else if (anchor === 'end') x -= rec.w;
      var base = cs.dominantBaseline || t.getAttribute('dominant-baseline') || '';
      if (base === 'central' || base === 'middle') y -= rec.h / 2;
      else y -= rec.asc;
      var img = document.createElementNS('http://www.w3.org/2000/svg', 'image');
      img.setAttribute('class', PIC_CLASS);
      img.setAttribute('x', x);
      img.setAttribute('y', y);
      img.setAttribute('width', rec.w);
      img.setAttribute('height', rec.h);
      img.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', rec.url);
      img.setAttribute('href', rec.url);
      img.bf6For = t;
      t.parentNode.insertBefore(img, t.nextSibling);
      t.style.display = 'none';
      t.bf6Pictured = true;
      made++;
    }
    return made;
  }
  UI.textPictureSweep = textPictureSweep;

  // DID THE PICTURE LAND WHERE THE WORDS WERE?
  //
  // 'it looks right' is not a measurement, and the first version of this put
  // every centred label half its own width off. The original text is still in
  // the dom, so a sample of it is shown for one measurement, both boxes are
  // read, and it is hidden again. Nothing is guessed and the page is left as
  // it was found.
  function textPictureCheck(sample) {
    var svg = UI.ws && UI.ws.getParentSvg && UI.ws.getParentSvg();
    if (!svg) return null;
    var pics = svg.querySelectorAll('image.' + PIC_CLASS);
    var want = Math.min(sample || 25, pics.length);
    if (!want) return { checked: 0 };
    // Step through the whole population, not the first N: with half the
    // stacks culled a short walk from the start can hit nothing measurable.
    var step = Math.max(1, Math.floor(pics.length / (want * 3)));
    var worstX = 0, worstY = 0, worstW = 0, checked = 0, worstOn = '', skipped = 0;
    for (var i = 0; i < pics.length && checked < want; i += step) {
      var img = pics[i];
      var t = img.bf6For;
      if (!t) continue;
      // A PICTURE ON A CULLED STACK HAS NO BOX TO MEASURE.
      //
      // getBBox on a display:none element returns zeros, so comparing it
      // against the text it replaced produced nonsense - 181px of width
      // error on a label that is drawn perfectly, purely because it happened
      // to be off screen. An instrument that cries wolf is worse than none,
      // especially right after a real fault was missed. They are counted and
      // skipped, and the count is reported so the sample size is honest.
      var farAway = false;
      try { farAway = !!(img.closest && img.closest('.' + FAR_CLASS)); } catch (e) {}
      if (farAway) { skipped++; continue; }
      var was = t.style.display;
      t.style.display = '';
      var tb = null, ib = null;
      try { tb = t.getBBox(); ib = img.getBBox(); } catch (e) {}
      t.style.display = was;
      if (!tb || !ib || !tb.width) continue;
      checked++;
      var dx = Math.abs(ib.x - tb.x);
      var dy = Math.abs((ib.y + ib.height / 2) - (tb.y + tb.height / 2));
      var dw = Math.abs(ib.width - tb.width);
      if (dx > worstX) { worstX = dx; worstOn = (t.textContent || '').slice(0, 18); }
      if (dy > worstY) worstY = dy;
      if (dw > worstW) worstW = dw;
    }
    return {
      checked: checked,
      skippedFarAway: skipped,
      worstLeftOff: Math.round(worstX * 100) / 100,
      worstMiddleOff: Math.round(worstY * 100) / 100,
      worstWidthOff: Math.round(worstW * 100) / 100,
      worstOn: worstOn
    };
  }
  UI.textPictureCheck = textPictureCheck;

  // AFTER THE PAINT, NOT BEFORE IT.
  //
  // Loading a workspace turns rendering off, builds everything and turns it
  // back on, so at the moment the load call returns the text nodes do not
  // exist yet: sweeping there found nothing and reported nothing, which read
  // exactly like a feature that was not wired up. It is scheduled instead -
  // once on the next frame, and once more a moment later for anything the
  // first frame had not finished.
  function scheduleTextPictures() {
    if (UI.picTimer) { clearTimeout(UI.picTimer); UI.picTimer = null; }
    requestAnimationFrame(function () {
      // Before the pictures: a label that is about to change its words must
      // change them BEFORE it is photographed, or the picture keeps the old
      // ones and nothing will ever correct it.
      try { refreshArgumentLabels(); } catch (e) {}
      var made = 0;
      try { made = textPictureSweep(); } catch (e) { UI.reportFault('picturing text', e); }
      UI.picTimer = setTimeout(function () {
        UI.picTimer = null;
        var more = 0;
        try { more = textPictureSweep(); } catch (e) {}
        try { cullFarImages(); } catch (e) {}
        if (made + more) {
          send({ op: 'log', text: 'text pictures: ' + (made + more) +
            ' label(s) drawn from ' + textPicCount + ' distinct picture(s)' });
        }
      }, 400);
    });
  }
  UI.scheduleTextPictures = scheduleTextPictures;
  UI.textPicturesOff = textPicturesOff;

  // Kept for the measuring harness, so a before and after is still one command.
  function textAsPictures(on) {
    if (!on) { var n = textPicturesOff(); send({ op: 'log', text: 'text pictures off: ' + n + ' put back' }); return n; }
    var made = textPictureSweep();
    send({ op: 'log', text: 'text as pictures: ' + made + ' label(s) swapped, ' +
      textPicCount + ' distinct picture(s) held' });
    return made;
  }
  UI.textAsPictures = textAsPictures;

  // =========================================================================
  // SYMBOLS ON BLOCKS THAT ARE NOWHERE NEAR THE SCREEN
  //
  // The browser paints the whole svg canvas as one layer, so a picture eight
  // screens away still costs: measured, hiding every image takes the frame
  // from 55.9ms to 24.8. The images are not decoration that can just go -
  // they are the socket symbols and the words - but the ones far off screen
  // are not being looked at by anyone.
  //
  // THIS IS NOT THE CULLING THAT WENT WRONG. That one hid BLOCKS, and a stale
  // bound meant a block vanished while you were looking at it. This touches
  // only the images ON a block: the block, its shape, its colour and its
  // connections are never affected, so the worst a stale bound can do is
  // leave a symbol off something you had to scroll a screen and a half to
  // reach - and the next settle puts it back.
  var FAR_CLASS = 'bf6-far';
  var FAR_MARGIN = 1.5;          // viewports of slack in every direction

  // AN ARGUMENT BLOCK ONLY KNOWS ITS NAME ONCE ITS SUBROUTINE HAS ONE.
  //
  // The dropdown's options are the enclosing subroutine's parameters, and
  // those arrive in the subroutine's own loadExtraState. A dynamic dropdown
  // caches the text of whatever option was selected when it last built its
  // menu, so the argument blocks generated their text while the parameter
  // list was still empty and kept showing a bare index: 0 where the site
  // says Enable, 1 where it says Name.
  //
  // Nothing is wrong with the value - the save holds the index either way -
  // so this only asks the field to say itself again, once the load has
  // settled and the subroutines know their own parameters.
  function refreshArgumentLabels() {
    if (!UI.ws) return 0;
    var n = 0;
    try {
      UI.ws.getAllBlocks(false).forEach(function (b) {
        if (b.type !== 'subroutineArgumentBlock') return;
        var f = b.getField('ARGUMENT_INDEX');
        if (!f) return;
        try {
          var v = f.getValue();
          if (f.getOptions) f.getOptions(false);   // rebuild the menu
          if (f.forceRerender) f.forceRerender();
          else if (f.setValue) { f.setValue(v); }
          n++;
        } catch (e) {}
      });
    } catch (e) { UI.reportFault('refreshing the argument labels', e); }
    return n;
  }
  UI.refreshArgumentLabels = refreshArgumentLabels;

  function wantFarCull() {
    var p = BF6.state.prefs || {};
    return p.cullFarSymbols === undefined ? true : !!p.cullFarSymbols;
  }

  function cullFarImages() {
    if (UI.ws && UI.ws.bf6Viewport) { UI.ws.bf6Viewport.update(); return 0; }
    if (!UI.ws || !wantFarCull()) return 0;

    // ASK BLOCKLY WHERE THE VIEW IS. DO NOT WORK IT OUT.
    //
    // This built the view rectangle by hand out of scrollLeft and scrollTop,
    // which are not the view's position in workspace coordinates, so the
    // rectangle landed somewhere the blocks are not and EVERY stack came out
    // far away. Text is drawn as pictures now, pictures are images, and the
    // cull hides images: so the whole workspace lost its words at once.
    //
    // Blockly already computes this and hands it over in workspace units,
    // which is the number wanted and no arithmetic of mine.
    var v = null;
    try { v = UI.ws.getMetricsManager().getViewMetrics(true); } catch (e) {}
    if (!v || !(v.width > 0) || !(v.height > 0)) return 0;

    var left = v.left - v.width * FAR_MARGIN;
    var top = v.top - v.height * FAR_MARGIN;
    var right = v.left + v.width * (1 + FAR_MARGIN);
    var bottom = v.top + v.height * (1 + FAR_MARGIN);

    var tops = UI.ws.getTopBlocks(false);
    var verdicts = [];
    var far = 0;
    for (var i = 0; i < tops.length; i++) {
      var b = tops[i];
      var xy = null, hw = null, root = null;
      try { xy = b.getRelativeToSurfaceXY(); hw = b.getHeightWidth(); root = b.getSvgRoot(); }
      catch (e) { continue; }
      if (!xy || !hw || !root) continue;
      var isFar = (xy.x > right) || (xy.x + hw.width < left) ||
                  (xy.y > bottom) || (xy.y + hw.height < top);
      if (isFar) far++;
      verdicts.push([root, isFar]);
    }
    if (!verdicts.length) return 0;

    // AND REFUSE TO BELIEVE AN ANSWER THAT HIDES EVERYTHING.
    //
    // Something is always on screen: that is what a view is. So a sweep saying
    // every single stack is far away has not found an empty screen, it has
    // found a broken measurement, and the right answer is to do nothing rather
    // than blank the canvas. This is the guard that turns the bug above from
    // "all the text vanished" into "the cull did not run", which is a bad
    // frame rate instead of an unusable editor.
    if (far === verdicts.length) {
      UI.reportFault('culling far symbols',
        new Error('all ' + far + ' stacks measured off screen, so nothing was hidden'));
      return 0;
    }

    for (var k = 0; k < verdicts.length; k++) {
      if (verdicts[k][1]) verdicts[k][0].classList.add(FAR_CLASS);
      else verdicts[k][0].classList.remove(FAR_CLASS);
    }
    return far;
  }
  UI.cullFarImages = cullFarImages;

  function perfExperiment(name, on) {
    if (!name) {
      send({ op: 'log', text: 'perf experiments: ' + Object.keys(PERF_EXPERIMENTS).join(', ') +
        ' | on now: ' + (Object.keys(UI.perfOn).join(',') || 'none') });
      return;
    }
    if (name === 'cullpics') {
      if (on === false) {
        var svg = UI.ws.getParentSvg();
        var was = svg.querySelectorAll('.' + FAR_CLASS);
        for (var i = 0; i < was.length; i++) was[i].classList.remove(FAR_CLASS);
        send({ op: 'log', text: 'far-image culling off' });
      } else {
        var n = cullFarImages();
        send({ op: 'log', text: 'far-image culling: ' + n + ' stack(s) out of view' });
      }
      return;
    }
    if (name === 'textpics') { textAsPictures(on !== false); return; }

    if (name === 'off') { UI.perfOn = {}; }
    else if (!PERF_EXPERIMENTS[name]) {
      send({ op: 'log', text: 'no perf experiment called ' + name });
      return;
    } else if (on === false) { delete UI.perfOn[name]; }
    else { UI.perfOn[name] = 1; }

    var css = Object.keys(UI.perfOn).map(function (k) { return PERF_EXPERIMENTS[k]; }).join(String.fromCharCode(10));
    var tag = document.getElementById(PERF_STYLE_ID);
    if (!tag) {
      tag = document.createElement('style');
      tag.id = PERF_STYLE_ID;
      document.head.appendChild(tag);
    }
    tag.textContent = css;
    send({ op: 'log', text: 'perf experiments on: ' + (Object.keys(UI.perfOn).join(',') || 'none') });
  }
  UI.perfExperiment = perfExperiment;

  function reportPerf(atScale) {
    try {
      window.BF6Measure.perf(Blockly, UI.ws, function (p) {
        p.op = 'perfmine';
        send(p);
        send({ op: 'log', text: 'perf: ' + window.BF6Measure.perfLine(p) });
      }, atScale);
    } catch (e) { UI.reportFault('measuring performance', e); }
  }

  function reportToolbox() {
    // MEASURED BY THE SAME CODE THAT MEASURES THE SITE. measure.js is loaded
    // by this page and prepended to the script injected into Portal, so the
    // two readings can be put side by side and the only differences left are
    // real ones. Two measuring routines written separately give two numbers
    // that differ for reasons nobody can pin down.
    var m = null;
    try { m = window.BF6Measure && window.BF6Measure.toolbox(Blockly, UI.ws); }
    catch (e) { UI.reportFault("measuring the toolbox", e); }
    if (!m) { send({ op: "log", text: "toolbox probe: nothing to measure" }); return; }
    m.op = "toolboxmine";
    send(m);
    send({ op: "log", text: "toolbox probe: " + window.BF6Measure.line(m) });

    // A CENSUS OF THE WHOLE CANVAS, not one sample.
    //
    // "the slots still have no icons" and "one socket has a symbol" are
    // different claims, and only the first one is the question. This counts
    // every empty value socket the user can actually see, how many of them
    // carry the symbol, and what shape each is drawn with, so a partial fix
    // cannot read as a working one.
    try {
      var all = UI.ws.getAllBlocks(false);
      var empty = 0, withIcon = 0, rendered = 0, noUrl = 0;
      var shapes = {};
      var cp = null;
      try { cp = UI.ws.getRenderer().getConstants(); } catch (e) {}
      all.forEach(function (b) {
        var root = null;
        try { root = b.getSvgRoot(); } catch (e) {}
        if (root) rendered++;
        (b.inputList || []).forEach(function (inp) {
          if (!inp.connection) return;
          if (inp.connection.type !== Blockly.INPUT_VALUE) return;
          if (inp.connection.targetConnection) return;
          empty++;
          if (!BF6.typeIconUrl(BF6.socketTypeName(b, inp))) noUrl++;
          try {
            var sh = cp && cp.shapeFor(inp.connection);
            var key = sh ? ('type' + sh.type + (sh.isDynamic ? '' : ' STATIC')) : 'none';
            shapes[key] = (shapes[key] || 0) + 1;
          } catch (e) { shapes.threw = (shapes.threw || 0) + 1; }
        });
        if (root) {
          try { withIcon += root.querySelectorAll(':scope > g.blocklyValueIcon').length; } catch (e) {}
        }
      });
      send({
        op: 'log',
        text: 'canvas census: ' + all.length + ' block(s), ' + rendered + ' rendered, ' +
          empty + ' empty value socket(s), ' + withIcon + ' carrying the symbol, ' +
          noUrl + ' with no symbol to draw; shapes ' + JSON.stringify(shapes) +
          '; drawer hook ' + (BF6.valueIconsInstalled() ? 'in' : 'NOT IN') +
          ', layout rules ' + (BF6.layoutRulesInstalled() ? 'in' : 'NOT IN')
      });
    } catch (e) { UI.reportFault('taking the canvas census', e); }

    try {
      var pc = textPictureCheck(25);
      if (pc) {
        send({ op: 'log', text: 'text pictures: ' + pc.checked + ' checked against the words ' +
          'they replaced (' + pc.skippedFarAway + ' skipped, culled off screen)' +
          ', worst left edge off by ' + pc.worstLeftOff + 'px, middle by ' +
          pc.worstMiddleOff + 'px, width by ' + pc.worstWidthOff + 'px' +
          (pc.worstOn ? (' (worst on "' + pc.worstOn + '")') : '') });
      }
    } catch (e) { UI.reportFault('checking the text pictures', e); }

    // And what an empty socket is actually made of on OUR canvas, in the same
    // words the site was described in. The symbol is drawn by the renderer, so
    // the only honest way to know it is there is to read the svg back.
    var rows = null, where = 'the canvas';
    try { rows = window.BF6Measure.sockets(Blockly, UI.ws, 3); }
    catch (e) { UI.reportFault('reading our own sockets', e); }
    // An editor opened without a project has an empty canvas and nothing to
    // read. The pull-out always has blocks in it, and they are rendered by the
    // same renderer, so it answers the same question.
    if (!rows || !rows.length) {
      try {
        var fw = UI.ws.getFlyout() && UI.ws.getFlyout().getWorkspace();
        if (fw) { rows = window.BF6Measure.sockets(Blockly, fw, 3); where = 'the pull-out'; }
      } catch (e) { UI.reportFault('reading the pull-out sockets', e); }
    }
    if (!rows || !rows.length) {
      send({ op: 'log', text: 'socket probe: no block with an empty socket anywhere to read' });
      return;
    }
    send({ op: 'log', text: 'socket probe: read from ' + where });

    // WHERE THE SYMBOL ACTUALLY LANDED, in the same numbers the site was
    // measured in. "a group exists" and "the symbol is inside the hole" are
    // different claims, and only the second one is the question. The site puts
    // its group at (holeX - r, holeCentreY); if ours is somewhere else the
    // picture is drawn and invisible, which looks exactly like no picture.
    try {
      var r0 = rows[0];
      var hole = null, grp = null, im = null;
      for (var j = 0; j < r0.svg.length; j++) {
        var nd = r0.svg[j];
        if (!hole && nd.cls === 'blocklyOutlinePath') hole = nd;
        if (!grp && nd.cls === 'blocklyValueIcon') { grp = nd; im = r0.svg[j + 1] || null; }
      }
      var holeAt = 'none drawn';
      if (hole) {
        var g0 = /^\s*M ([-\d.]+),([-\d.]+)\s+h ([-\d.]+) a (\d+)/.exec(hole.d || '');
        holeAt = g0 ? ('x=' + g0[1] + ' y=' + g0[2] + ' w=' + (parseFloat(g0[3]) + 2 * parseFloat(g0[4])) +
          ' h=' + (2 * parseFloat(g0[4])) + ' r=' + g0[4] + ' fill=' + hole.fill) : (hole.d || '').slice(0, 60);
      }
      send({
        op: 'log',
        text: 'socket geometry: hole ' + holeAt +
          ' | group ' + (grp ? grp.transform : 'none') +
          ' | image ' + (im ? (im.width + ' at ' + im.transform + ' src ' +
            String(im.xlinkHref || im.href || '').slice(0, 24)) : 'none')
      });
      // THE ATTRIBUTE IS NOT THE ANSWER. The dump records what is written on
      // the path, and the site's hole colour is applied by a css rule that
      // beats it, so reading the attribute back would report a failure that is
      // not one. The computed value is what the eye sees.
      var painted = '';
      try {
        var live = document.querySelector('.blocklyOutlinePath');
        if (live) painted = window.getComputedStyle(live).fill;
      } catch (e) {}
      // And OUR side of the numbers that decide how wide a socket is drawn,
      // next to the site's, since a socket the right colour and the wrong
      // width still does not match.
      var mine = null;
      try { mine = window.BF6Measure.socketRules(Blockly, UI.ws); } catch (e) {}
      send({
        op: 'log',
        text: 'socket paint: hole computed fill ' + (painted || 'unknown') +
          ' | our constants ' + JSON.stringify(mine && mine.constants) +
          ' | our shapes ' + (mine && mine.shapes
            ? Object.keys(mine.shapes).map(function (k) { return k + '=' + mine.shapes[k].type; }).join(',')
            : 'none')
      });
    } catch (e) { UI.reportFault('reading the socket geometry', e); }

    // And the same measurement of the same blocks on our side, written next
    // to the site's so the two can be diffed element by element.
    try {
      var L = window.BF6Measure.layout(Blockly, UI.ws, window.BF6Measure.TYPES_TO_MEASURE);
      if (L) {
        L.op = 'layoutmine';
        send(L);
        send({ op: 'log', text: 'block layout: ' + window.BF6Measure.layoutLine(L) });
      }
    } catch (e) { UI.reportFault('measuring the block layout', e); }

    // And every block on our canvas, to line up against the site's sweep.
    try {
      var S = window.BF6Measure.sweep(Blockly, UI.ws);
      if (S && S.count) {
        S.op = 'sweepmine';
        send(S);
        send({ op: 'log', text: 'swept ' + S.count + ' block(s) of ' +
          Object.keys(S.byType).length + ' type(s)' });
      }
    } catch (e) { UI.reportFault('sweeping the canvas', e); }

    // And how OUR fields are painted, next to the site's.
    try {
      var F = window.BF6Measure.fieldLook(Blockly, UI.ws, 10);
      if (F) {
        F.op = 'fieldmine';
        send(F);
        send({ op: 'log', text: 'field look: ' + F.blocks.length + ' block(s), icons ' +
          JSON.stringify(F.icons) + ', text ' + JSON.stringify(F.css['.blocklyText'] || null) });
      }
    } catch (e) { UI.reportFault('reading our fields', e); }

    try {
      var D = window.BF6Measure.diagnose(Blockly, UI.ws);
      if (D) {
        D.op = 'diagnosemine';
        send(D);
        if (D.font) send({ op: 'log', text: 'font: ' + D.font.font + '  widths ' + JSON.stringify(D.font.widths) });
        if (D.argBlocks && D.argBlocks.length) send({ op: 'log', text: 'argument blocks: ' + JSON.stringify(D.argBlocks) });
        send({ op: 'log', text: 'diagnose: ' + D.icons.length + ' icon kind(s), ' +
          D.labels.length + ' odd label(s), ' + D.warnings.length + ' warning(s), sub-blocks ' +
          (Object.keys(D.mutatorBlocks).join(',') || 'none') });
      }
    } catch (e) { UI.reportFault('diagnosing', e); }
    send({
      op: 'log',
      text: 'socket probe: ' + rows.map(function (r) {
        // The image is the node right after its group. Matching on the address
        // instead would report zero every time here and non-zero on the site,
        // for no reason but that ours is mirrored to a data uri.
        var icons = [], imgs = [];
        for (var i = 0; i < r.svg.length; i++) {
          if (r.svg[i].cls !== 'blocklyValueIcon') continue;
          icons.push(r.svg[i]);
          var next = r.svg[i + 1];
          if (next && next.tag === 'image' && (next.xlinkHref || next.href)) imgs.push(next);
        }
        return r.block + '.' + r.input + ' shape ' + (r.shape && r.shape.type) +
          ' ' + (r.shape && r.shape.width) + 'x' + (r.shape && r.shape.height) +
          ', ' + icons.length + ' symbol group(s), ' + imgs.length + ' type image(s), ' +
          r.fieldsInInput + ' field(s) in the socket';
      }).join('; ')
    });
  }
  UI.reportToolbox = reportToolbox;

  // The input itself, put where the site puts it: first child of the toolbox,
  // above the category list. Rebuilt with the workspace, because injecting a
  // workspace builds a new toolbox div.
  function installSearchbar() {
    var tb = null;
    try { tb = UI.ws.getToolbox(); } catch (e) { tb = null; }
    var host = tb && (tb.HtmlDiv || tb.htmlDiv_);
    if (!host) return false;

    var input = document.createElement('input');
    input.type = 'search';
    input.size = 2;
    input.className = SEARCHBAR_CLASS;
    input.id = 'toolboxsearch';
    input.placeholder = 'Search blocks';
    input.setAttribute('aria-label', 'Search blocks');

    var wrap = document.createElement('div');
    wrap.className = SEARCHBAR_CLASS + 'div';
    var span = document.createElement('span');
    span.className = SEARCHBAR_CLASS + 'span';
    span.appendChild(input);
    wrap.appendChild(span);

    var list = host.firstChild;
    if (list && list.classList) list.classList.add(TOOLBAR_CLASS);
    host.insertBefore(wrap, host.firstChild);

    var timer = null;
    function onType() {
      var v = input.value;
      clearTimeout(timer);
      timer = setTimeout(function () { runToolboxSearch(v); }, 100);
    }
    input.addEventListener('keyup', onType);
    input.addEventListener('search', onType);
    input.addEventListener('input', onType);
    // Blockly listens on the whole injection div; without this a keystroke in
    // the box can also reach the workspace as a shortcut.
    input.addEventListener('keydown', function (e) {
      e.stopPropagation();
      if (e.key === 'Escape') { input.value = ''; runToolboxSearch(''); input.blur(); }
    });

    UI.searchInput = input;
    // Built empty and never shown as a row.
    var bin = searchBin();
    if (bin && bin.hide) { try { bin.hide(); } catch (e) {} }
    return true;
  }

  // ==========================================================================
  // VARIABLES and SUBROUTINES
  //
  // Both are filled at open time from what the workspace actually holds, which
  // is why they carry no blocks in the toolbox file and why both opened empty
  // until now. Each is built the way the site builds it: an action button
  // first, then the blocks that make sense for what already exists.
  // ==========================================================================

  function xmlBlock(type, fieldName, fieldValue) {
    var b = Blockly.utils.xml.createElement('block');
    b.setAttribute('type', type);
    if (fieldName) {
      var f = Blockly.utils.xml.createElement('field');
      f.setAttribute('name', fieldName);
      f.appendChild(Blockly.utils.xml.createTextNode(String(fieldValue == null ? '' : fieldValue)));
      b.appendChild(f);
    }
    return b;
  }

  function flyoutButton(text, callbackKey) {
    var btn = document.createElement('button');
    btn.setAttribute('text', text);
    btn.setAttribute('callbackKey', callbackKey);
    return btn;
  }

  function variablesFlyout(workspace) {
    var out = [flyoutButton('Create variable', 'BF6_MANAGE_VARIABLES')];
    var any = 0;
    try { any = (workspace.getAllVariables() || []).length; } catch (e) { any = 0; }
    if (!any) {
      out.push({ kind: 'label', text: 'No variables yet. Create one to use it here.' });
      return out;
    }
    // The site offers the reference itself, then reading it, then writing it,
    // with the reference already dropped into each one's first socket.
    out.push(xmlBlock('variableReferenceBlock'));
    ['GetVariable', 'SetVariable'].forEach(function (type) {
      if (!Blockly.Blocks[type]) return;
      var host = Blockly.utils.xml.createElement('block');
      host.setAttribute('type', type);
      var val = Blockly.utils.xml.createElement('value');
      val.setAttribute('name', 'VALUE-0');
      val.appendChild(xmlBlock('variableReferenceBlock'));
      host.appendChild(val);
      out.push(host);
    });
    return out;
  }

  function subroutineNames(workspace) {
    try {
      return (workspace.getTopBlocks(true) || [])
        .filter(function (b) { return b.type === 'subroutineBlock'; })
        .map(function (b) { return b.getFieldValue('SUBROUTINE_NAME') || ''; })
        .filter(function (n) { return !!n; });
    } catch (e) { return []; }
  }

  function subroutinesFlyout(workspace) {
    var out = [flyoutButton('Create subroutine', 'BF6_CREATE_SUBROUTINE')];
    var names = subroutineNames(workspace);
    if (!names.length) {
      out.push({ kind: 'label', text: 'No subroutines yet. Create one to call it here.' });
      return out;
    }
    if (Blockly.Blocks['subroutineArgumentBlock']) out.push(xmlBlock('subroutineArgumentBlock'));
    if (Blockly.Blocks['subroutineInstanceBlock']) {
      names.forEach(function (n) {
        out.push(xmlBlock('subroutineInstanceBlock', 'SUBROUTINE_NAME', n));
      });
    }
    return out;
  }

  // The two buttons. Creating a subroutine drops a real subroutineBlock on the
  // canvas, because that is the only way one comes into being.
  function createSubroutine() {
    if (!Blockly.Blocks['subroutineBlock']) {
      status('subroutine blocks are not loaded yet', true);
      return;
    }
    var taken = {};
    subroutineNames(UI.ws).forEach(function (n) { taken[n] = 1; });
    var name = 'Subroutine';
    for (var i = 2; taken[name]; i++) name = 'Subroutine ' + i;
    var b = UI.ws.newBlock('subroutineBlock');
    try { b.setFieldValue(name, 'SUBROUTINE_NAME'); } catch (e) {}
    b.initSvg();
    b.render();
    var m = UI.ws.getMetricsManager().getViewMetrics(true);
    b.moveBy(m.left + 40, m.top + 40);
    UI.ws.centerOnBlock(b.id);
    try { UI.ws.getToolbox().refreshSelection(); } catch (e) {}
    status('subroutine "' + name + '" created');
  }

  function manageVariables() {
    try { Blockly.Variables.createVariableButtonHandler(UI.ws, function () {
      try { UI.ws.getToolbox().refreshSelection(); } catch (e) {}
    }); } catch (e) { status('could not open the variable dialog', true); }
  }

  // ==========================================================================
  // The site's look
  //
  // Four things carry it, and all four come from the capture: the renderer, its
  // constants, the theme and the site's own CSS. editor.js does the parts that
  // have to survive a headless test; what is left here is the parts that need a
  // document: putting the stylesheet on the page and drawing the icons.
  // ==========================================================================
  var themeSeq = 0;

  // Defining a theme registers it under its name, and a second definition under
  // the same name is refused, so every apply gets a fresh name.
  function themeFor() {
    var spec = BF6.themeSpec ? BF6.themeSpec() : null;
    if (!spec) {
      // Nothing captured this session: the older defs-borne theme, if any.
      var t = BF6.state.theme;
      if (!t) return null;
      spec = {
        blockStyles: t.blockStyles || {}, categoryStyles: t.categoryStyles || {},
        componentStyles: t.componentStyles || {}, fontStyle: t.fontStyle || {},
        startHats: !!t.startHats
      };
    }
    if (UI.redVariables) {
      spec.blockStyles = Object.assign({}, spec.blockStyles);
      spec.blockStyles['variable-block-style'] = Object.assign({}, spec.blockStyles['variable-block-style'], {
        colourPrimary: '#6e0000', colourSecondary: '#870000', colourTertiary: '#460000'
      });
    }
    try {
      return Blockly.Theme.defineTheme('bf6portal_' + (++themeSeq), {
        base: Blockly.Themes.Classic,
        blockStyles: spec.blockStyles,
        categoryStyles: spec.categoryStyles,
        componentStyles: spec.componentStyles,
        fontStyle: spec.fontStyle,
        startHats: spec.startHats
      });
    } catch (e) { return null; }
  }

  // The site's stylesheet, put on the page AFTER Blockly's own so it wins, plus
  // one rule per captured category icon: the toolbox names an icon by css class
  // and this is where that class gets its picture.
  function injectSiteCss() {
    var tag = $('bf6-site-css');
    if (!tag) {
      tag = document.createElement('style');
      tag.id = 'bf6-site-css';
      document.head.appendChild(tag);
    }
    // Always last in the head, so it is the last word on any selector.
    document.head.appendChild(tag);
    var st = BF6.state.style;
    var parts = [];
    // The faces first: an @font-face has to be on the page before the rule
    // that asks for the family, or the first paint is done in the fallback.
    // The files are read from the local mirror where they sit; nothing is
    // copied into the plugin.
    // THE SOCKET HOLE'S COLOUR, WHICH IS NOT IN ANY STYLESHEET.
    //
    // The site paints it as an attribute on the path, so no amount of reading
    // its css will find it, and Blockly's own default is the BLOCK's border
    // colour. That is why our sockets came out as a dark shade of whatever the
    // block was and the site's are all the same near black. Measured off a
    // socket that was really on screen. A presentation attribute loses to a
    // css rule, so this wins without touching the renderer.
    var sr = (st && st.socketRules) || null;
    if (sr && sr.outline && sr.outline.fill) {
      parts.push('.blocklyOutlinePath { fill: ' + sr.outline.fill + '; }');
    }
    if (st && st.fontCss) parts.push(st.fontCss);
    // THE VARIABLES FIRST, THEN THE RULES THAT USE THEM.
    //
    // The site's stylesheet is written in terms of custom properties it
    // defines elsewhere, on rules that never say 'blockly' and so were never
    // captured. A rule whose value is an undefined var() is not a rule with a
    // default, it is an invalid declaration, and the property falls back to
    // its initial value - black, for a fill. The question mark on a comment
    // was drawn correctly and painted black on a near black disc.
    //
    // They go on :root so anything in the page can resolve them, and BEFORE
    // the site's own css so a captured :root rule could still override.
    if (st && st.css && st.css.vars) {
      var names = Object.keys(st.css.vars);
      if (names.length) {
        parts.push(':root{' + names.map(function (n) {
          return n + ':' + st.css.vars[n] + ';';
        }).join('') + '}');
      }
    }
    if (st && st.css && st.css.text) { parts.push(st.css.text); }

    // Only the block canvas: the toolbox, the pull-out and every dialog keep
    // their words whatever the canvas is zoomed to.
    /* THE BLUR BEHIND THE PULL-OUT IS NOT SURVIVABLE IN THIS BROWSER.
     *
     * The site's stylesheet puts a backdrop-filter on the flyout. On the site
     * that is a cheap GPU effect. Here the page is rendered offscreen inside
     * the editor, and a backdrop-filter over a panel that size has to
     * re-composite everything behind it, on the CPU, on the thread the whole
     * editor shares.
     *
     * Measured, not guessed: selecting a toolbox category logged
     * "category setSelected on ..." and never returned, with the editor at
     * ~90% of one core indefinitely. Blocks also highlighted sluggishly on
     * hover, which is the same cost with nothing of ours running. Categories
     * with more blocks made it worse, which is why it first read as a block
     * count problem and cost a long detour.
     *
     * The blur is decoration. Everything else about the site's look is kept.
     */
    /* The site's own blur, shadows and hover filters are kept. They were all
     * suppressed for a while on 2026-09-07 while the editor was freezing on
     * every toolbox click, and none of them had anything to do with it: the
     * cause was a synchronous log flush in the C++ bridge handler. Suppressing
     * them changed the look and fixed nothing, so they go back.
     *
     * PERF_EXPERIMENTS above still holds nofilter, nohover and noblur for
     * measuring what these cost. That is where an experiment belongs. */

    parts.push('.' + TEXT_HIDDEN_CLASS + ' .blocklyBlockCanvas text, .' +
      TEXT_HIDDEN_CLASS + ' .blocklyBlockCanvas image.' + PIC_CLASS + ' { display: none; }');
    parts.push('.' + FAR_CLASS + ' image { display: none; }');

    /* A STACK THAT IS OFF SCREEN SHOULD NOT BE PAINTED AT ALL.
     *
     * cullFarImages already works out which top level stacks are outside the
     * view, and it only took their pictures off. The blocks themselves were
     * still painted and still hit tested every frame, which on a 10,000 block
     * rule is most of the work being done for something nobody can see.
     *
     * visibility, not display. An SVG subtree set to visibility:hidden is not
     * rendered and not hit tested, but it IS still laid out, so Blockly can
     * still measure its text. display:none would zero those measurements, and
     * a block that re-rendered while hidden would come back the wrong size and
     * stay that way.
     *
     * Only on a workspace big enough to need it. perf-heavy is set past 1,500
     * blocks, so a small project keeps the site's exact behaviour and this
     * cannot regress the common case.
     */
    parts.push('body.perf-heavy .' + FAR_CLASS +
               ' { visibility: hidden; pointer-events: none; }');

    // A LABEL IS NEVER THE THING YOU CLICK. The click lands on the field's
    // own group and its border rect, so eight thousand pictures and eight
    // thousand text nodes do not need considering on every mouse move.
    // Measured on its own: 48.9ms to 47.0. Small, and it costs nothing.
    // The socket symbols are NOT included - those are buttons.
    parts.push('image.' + PIC_CLASS + ', .blocklyBlockCanvas text { pointer-events: none; }');

    // THE FIELD COLOURS GO LAST, AND A DROPDOWN IS NOT A TEXT BOX.
    //
    // Two mistakes were in one rule here. It came BEFORE the site's own
    // captured css, which carries Blockly's defaults, so Blockly's grey
    // #575E75 won and every text box read grey where the site's reads black.
    // And it painted the ground of EVERY editable field, so a dropdown got
    // the text box's grey slab instead of the block's own colour showing
    // through: that is true and false on a white ground.
    //
    // Measured on the site, field by field, on elements really on screen:
    //   a text box   grey #a8a8a8 ground, rounded 4, BLACK text
    //   a dropdown   no ground at all, WHITE text
    //   a label      no ground, white text
    // Every picture was drawn in the old font and the old ink, so they all go.
    try { UI.textPicturesOff(); UI.forgetTextPictures(); } catch (e) {}

    var cs = (st && st.theme && st.theme.componentStyles) || {};
    if (cs.fieldBorderRectColor || cs.fieldTextColor) {
      var box = cs.fieldBorderRectColor || '#a8a8a8';
      var ink = cs.fieldTextColor || '#000000';
      var drop = cs.fieldDropdownTextColor || '#ffffff';
      parts.push(
        // A dropdown carries blocklyDropdownText on its text, and :has lets
        // the ground be chosen by what is inside it. Chromium has had :has
        // since 105 and this page is CEF, so it is available; the plain rule
        // below it stays correct on its own if it ever is not.
        '.blocklyEditableText>rect, .blocklyEditableText>.blocklyFieldRect' +
        '{fill:' + box + ';}' +
        // !important because Blockly writes this one INLINE on the element
        // when it colours an editable field, and an inline style beats any
        // rule that does not say so. Measured: without it the text stays
        // Blockly's own #575E75 grey where the site's is black.
        '.blocklyEditableText>text, .blocklyEditableText>.blocklyText' +
        '{fill:' + ink + ' !important;}' +
        '.blocklyEditableText:has(>.blocklyDropdownText)>rect,' +
        '.blocklyEditableText:has(>.blocklyDropdownText)>.blocklyFieldRect' +
        '{fill:transparent;}' +
        '.blocklyDropdownText{fill:' + drop + ' !important;}' +
        (cs.fieldEditableTextHoverColor
          ? '.blocklyEditableText:hover>rect{fill:' + cs.fieldEditableTextHoverColor + ';}'
          : ''));
    }
    if (st && st.categoryIcons) {
      Object.keys(st.categoryIcons).forEach(function (key) {
        if (key.indexOf('class:') !== 0) return;
        var cls = key.slice(6);
        var rec = st.categoryIcons[key] || {};
        var body = [];
        if (rec.backgroundImage) {
          body.push('background-image:' + rec.backgroundImage);
          body.push('background-repeat:no-repeat');
          body.push('background-position:center');
          // The site's own sheet sizes these boxes. Without it the span an icon
          // class lands on has no box at all and the picture never shows, so a
          // box is given here and the site's sheet, which comes later in the
          // same tag, still wins wherever it has an opinion.
          body.push('background-size:contain');
          body.push('display:inline-block');
          body.push('flex:0 0 auto');
        }
        if (rec.maskImage) {
          body.push('-webkit-mask-image:' + rec.maskImage);
          body.push('mask-image:' + rec.maskImage);
          body.push('-webkit-mask-repeat:no-repeat');
          body.push('mask-repeat:no-repeat');
          body.push('-webkit-mask-position:center');
          body.push('mask-position:center');
          if (rec.colour) body.push('background-color:' + rec.colour);
        }
        if (body.length) {
          body.push('width:' + (rec.width && rec.width !== 'auto' ? rec.width : '20px'));
          body.push('height:' + (rec.height && rec.height !== 'auto' ? rec.height : '20px'));
        }
        if (body.length) parts.push('.' + cls + '{' + body.join(';') + '}');
      });
    }
    tag.textContent = parts.join('\n');
    // Our stand-in chrome steps aside once the site's own sheet is here.
    try {
      document.body.classList[(st && st.css && st.css.text) ? 'remove' : 'add']('no-site-style');
    } catch (e) {}
    return tag.textContent.length;
  }

  // Blockly draws three little pictures out of its media folder: the quote
  // marks either side of a string field, and a 1x1 spacer. The site serves its
  // own copies of exactly those three, and the mirror has them, so the workspace
  // is pointed at the mirror's folder rather than at Blockly's default, which is
  // a URL on the open internet and resolves to nothing here.
  //
  // Pointing at it is not always enough: a file:// page is its own opaque
  // origin and a picture loaded across folders can be refused without a word.
  // The same three arrive inlined as well, and this rewrites any that did not
  // load. Both come from the mirror; neither ships.
  // sprites.png belongs on this list too. Blockly draws zoom in, zoom out and
  // show-everything out of that one sheet, so when it does not load those
  // three controls are blank squares, which is what they were above the
  // minimap. It is the site's own sheet, so they end up with the site's icons
  // rather than lookalikes drawn here.
  var MEDIA_NAMES = ['quote0.png', 'quote1.png', '1x1.png', 'sprites.png'];

  function patchMediaImages(root) {
    var st = BF6.state.style;
    var imgs = st && st.blockImages;
    if (!imgs) return 0;
    var host = root || document;
    var n = 0;
    try {
      var nodes = host.querySelectorAll('image, img');
      for (var i = 0; i < nodes.length; i++) {
        var el = nodes[i];
        var href = el.getAttribute('href') || el.getAttribute('xlink:href') ||
          el.getAttribute('src') || '';
        if (!href || href.indexOf('data:') === 0) continue;
        for (var m = 0; m < MEDIA_NAMES.length; m++) {
          var name = MEDIA_NAMES[m];
          if (href.length < name.length || href.slice(-name.length) !== name) continue;
          var url = imgs[name];
          if (!url) break;
          if (el.tagName && String(el.tagName).toLowerCase() === 'img') el.setAttribute('src', url);
          else {
            el.setAttribute('href', url);
            try { el.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', url); } catch (e) {}
          }
          n++;
          break;
        }
      }
    } catch (e) {}
    return n;
  }
  UI.patchMediaImages = patchMediaImages;

  // A payload from the site, from the cache, or from the offline default file.
  function applyStyle(msg) {
    var sum = BF6.setStyle(msg);
    // THE RULES ARRIVE WITH THE STYLE, SO THEY ARE INSTALLED WHEN IT ARRIVES.
    //
    // Installing them at workspace-build time ran before the style message had
    // been handled, found no captured rules, gave up and never tried again: the
    // hook reported itself as not installed and every block kept Blockly's own
    // spacing. It is a no-op once it has taken, so calling it on both paths is
    // safe and neither path can be the only one.
    try { BF6.installLayoutRules(); } catch (e) { UI.reportFault('installing the layout rules', e); }

    // AND CONVERT THE DEFINITIONS AGAIN IF THE PICTURES ONLY ARRIVED NOW.
    //
    // The definitions come over before the style does, and the style is what
    // carries the mirrored symbols, so on the first pass every socket badge
    // resolves to nothing and the block is built without one. The conversion
    // has already happened by then, so it never corrects itself: blocks sat on
    // the canvas with no head badge while every offline check passed, because
    // the checks had the mirror in hand and the running editor did not, yet.
    var dropped = BF6.state.iconsDropped || 0;
    if (dropped && BF6.state.rawReadings) {
      BF6.installDefinitions(BF6.state.rawReadings, BF6.state.definitionsSource);
      send({
        op: 'log',
        text: 'definitions converted again now the symbols are here: ' + dropped +
          ' had resolved to nothing, ' + (BF6.state.iconsDropped || 0) + ' still do'
      });
    }

    // Colours go onto the definitions first, so the blocks the rebuild creates
    // are born the right colour. Then the renderer, the constants and the theme
    // all land in the one rebuild.
    BF6.applyBlockColours();
    injectSiteCss();
    rebuildWorkspace();
    patchMediaImages();
    paintStylePill(sum);
    status(BF6.styleSummaryLine(sum));
    send({ op: 'log', text: BF6.styleSummaryLine(sum) });
    // What the toolbox asked for against what the tiers could answer. The
    // count, not the guess: a mirror full of icons still leaves a category bare
    // if the toolbox names a key the mirror has no file for.
    try {
      var cov = BF6.iconCoverage(BF6.state.toolbox || window.BF6_TOOLBOX_FALLBACK);
      if (cov && cov.categories) {
        send({ op: 'log', text: 'toolbox icons: ' + cov.resolved + ' of ' + cov.categories +
          ' categories drawn (' + cov.byTier.live + ' site, ' + cov.byTier.mirror + ' mirror, ' +
          cov.byTier.fallback + ' offline default)' +
          (cov.missing.length ? ', no icon for ' + cov.missing.join(', ') : '') });
      }
    } catch (e) {}
    return sum;
  }

  function paintStylePill(sum) {
    var pill = $('stylepill');
    if (!pill) return;
    var s = sum || (BF6.styleSummary ? BF6.styleSummary() : null);
    pill.className = 'pill';
    if (!s || s.source === 'none') {
      pill.textContent = 'Site style not captured yet';
      pill.className = 'pill warnpill';
      pill.title = 'Press CAPTURE STYLE with the Portal blocks page open in the panel. ' +
        'Until then the blocks are drawn with Blockly\'s geras renderer, which is the ' +
        'closest shape to Portal\'s, and the colours are whatever the definitions carry.';
      return;
    }
    var where = s.source === 'live' ? 'from the site' :
      s.source === 'cache' ? 'from the last capture' :
        s.source === 'default' ? 'from the offline default' : 'approximate';
    // The icons have their own precedence and can be a better tier than the
    // rest of the look, so the pill names the two separately rather than
    // letting one stand for the other.
    var icons = s.iconSource === 'live' ? 'site icons' :
      s.iconSource === 'mirror' ? 'mirror icons' :
        s.iconSource === 'fallback' ? 'offline icons' : 'no icons';
    pill.textContent = 'Style ' + where + ', ' + icons;
    if (s.source === 'live' || s.source === 'cache') pill.className = 'pill on';
    else pill.className = 'pill warnpill';
    pill.title = BF6.styleSummaryLine(s) +
      (s.source === 'default'
        ? '\n\nExtracted from the site\'s public bundle, not from a signed-in page. ' +
        'Press CAPTURE STYLE with the Portal blocks page open to make it exact.'
        : s.source === 'approx'
          ? '\n\nApproximate. Press CAPTURE STYLE with the Portal blocks page open.'
          : '');
  }

  // THERE WAS NO WAY TO PAN THIS CANVAS.
  //
  // Blockly pans by dragging the BACKGROUND, and it has no middle mouse pan of
  // its own. On a converted project the viewport is full of blocks, so a drag
  // almost always lands on one and moves that block instead. The wheel is given
  // to zoom deliberately (see workspaceOptions), so it cannot scroll either.
  // Between them a creator could zoom in and out of a 7469 by 7883 board and
  // never move across it.
  //
  // Middle drag pans, and so does holding Alt, which is what every other
  // canvas tool does. Both are added here rather than by changing the wheel
  // back, because zooming on the wheel is the right default for a board this
  // size and nothing has to be given up to get panning.
  /* A BREADCRUMB TRAIL, BECAUSE NOTHING CAN BE ASKED A QUESTION MID FREEZE.
   *
   * The editor's browser runs in the editor's own process, so a JavaScript
   * task that never returns takes the whole thing with it: the console stops
   * answering, the MCP tools time out, and even the CEF remote debugger stops
   * responding, which was tried and hung with it. Any instrument that has to be
   * ASKED is useless here by construction.
   *
   * What does survive is anything written BEFORE the freeze. send() reaches
   * C++ through the native bridge and is logged straight away, which is how
   * "pan: first mousedown seen" reached the log a moment before the editor
   * locked. So the suspect paths are wrapped to say what they are about to do,
   * and the last line in the log names whatever did not come back.
   *
   * Entry and exit both, with the elapsed time on exit: a step that logs its
   * start and never its end is the one that hung, and a slow step that does
   * finish is a different and much less interesting problem.
   */
  function installFreezeTrail() {
    /* SAY SO BEFORE DOING ANYTHING. The first version of this reported only at
     * the end, threw somewhere in the middle, and produced no line at all -
     * which reads exactly like a function that was never called. An instrument
     * that can fail silently is not an instrument. */
    send({ op: 'log', level: 'display', text: 'trail: installing' });
    try {
      installFreezeTrailInner();
    } catch (e) {
      send({ op: 'log', level: 'display',
             text: 'trail: could NOT install: ' + (e && e.message ? e.message : e) });
    }
  }

  function installFreezeTrailInner() {
    if (!window.Blockly) { send({ op: 'log', text: 'trail: no Blockly' }); return; }
    var installed = 0;

    function wrap(owner, name, label, describe) {
      if (!owner || typeof owner[name] !== 'function' || owner['__bf6t_' + name]) { return; }
      var orig = owner[name];
      owner['__bf6t_' + name] = true;
      owner[name] = function () {
        var what = label;
        try { if (describe) { what += ' ' + describe.apply(this, arguments); } } catch (e) {}
        send({ op: 'log', level: 'display', text: 'trail: ' + what + ' ...' });
        var t0 = Date.now();
        try {
          return orig.apply(this, arguments);
        } finally {
          send({ op: 'log', level: 'display',
                 text: 'trail: ' + what + ' done in ' + (Date.now() - t0) + 'ms' });
        }
      };
      installed++;
    }

    var B = window.Blockly;
    /* Opening a category is what the user was doing when it locked. */
    if (B.Toolbox && B.Toolbox.prototype) {
      wrap(B.Toolbox.prototype, 'setSelectedItem', 'toolbox select', function (item) {
        try { return '"' + (item && item.getName ? item.getName() : '?') + '"'; }
        catch (e) { return ''; }
      });
      wrap(B.Toolbox.prototype, 'refreshSelection', 'toolbox refresh');
    }
    /* The pull-out itself: how many blocks it was asked to build. */
    var FL = (B.VerticalFlyout && B.VerticalFlyout.prototype)
          || (B.Flyout && B.Flyout.prototype);
    wrap(FL, 'show', 'flyout show', function (defs) {
      try { return '(' + (defs && defs.length ? defs.length : (defs && defs.contents ?
             defs.contents.length : '?')) + ' item(s))'; } catch (e) { return ''; }
    });
    wrap(FL, 'layout', 'flyout layout');
    /* position is deliberately NOT traced. It fires constantly on its own and,
     * now that every trail line is flushed to disk, tracing it costs more than
     * it tells us. */

    /* WHERE IT ACTUALLY HANGS: between selecting a category and showing the
     * flyout, which is where the category's block list gets built. Measured:
     * "toolbox select OBJECTIVE" never returns and "flyout show" is never
     * reached, so the cost is in producing the contents, not drawing them. A
     * dynamic category produces its contents from a registered callback, and
     * ours are the obvious suspects. */
    if (B.ToolboxCategory && B.ToolboxCategory.prototype) {
      wrap(B.ToolboxCategory.prototype, 'getContents', 'category contents');
      /* OBJECTIVE and USER INTERFACE are static categories, so their contents
       * are stored data and cannot be what takes for ever. setSelected is the
       * other half of selecting one, and it restyles the row against the
       * 49KB of CSS captured from the site. That is the remaining candidate
       * inside the window we have measured. */
      wrap(B.ToolboxCategory.prototype, 'setSelected', 'category setSelected',
        function (sel) { return sel ? 'on' : 'off'; });
    }
    if (B.WorkspaceSvg && B.WorkspaceSvg.prototype) {
      wrap(B.WorkspaceSvg.prototype, 'getToolboxCategoryCallback',
        'category callback lookup', function (n) { return '"' + n + '"'; });
    }
    if (B.Toolbox && B.Toolbox.prototype) {
      wrap(B.Toolbox.prototype, 'updateFlyout_', 'toolbox update flyout');
    }

    /* THE PULL-OUT IS INNOCENT. Measured: a category opens in 18ms and reports
     * done. What locks the editor is the NEXT thing, the mousedown on a block
     * inside it, so the trail follows the gesture and the block it makes. */
    if (B.Gesture && B.Gesture.prototype) {
      /* handleBlockStart is the one a CLICK goes through; doStart only runs
       * once a drag begins. The first pass wrapped doStart and saw nothing,
       * which narrowed the hang to before it rather than proving the gesture
       * innocent. */
      wrap(B.Gesture.prototype, 'handleBlockStart', 'gesture block start');
      wrap(B.Gesture.prototype, 'handleWsStart', 'gesture workspace start');
      wrap(B.Gesture.prototype, 'doStart', 'gesture start');
      wrap(B.Gesture.prototype, 'handleFlyoutStart', 'gesture from flyout');
      wrap(B.Gesture.prototype, 'startDraggingBlock', 'gesture drag block');
    }
    if (B.WorkspaceSvg && B.WorkspaceSvg.prototype) {
      wrap(B.WorkspaceSvg.prototype, 'getGesture', 'workspace getGesture');
    }

    /* WHICH BLOCK. A category of 8 builds in 18ms and one of 23 in 26ms, and
     * one of 68 never returns, so the cost is not the count: one block in that
     * category does not terminate. newBlock is the common door every flyout
     * block comes through, so the last type named here is the culprit. */
    if (B.WorkspaceSvg && B.WorkspaceSvg.prototype && !B.WorkspaceSvg.prototype.__bf6t_newBlock) {
      var origNew = B.WorkspaceSvg.prototype.newBlock;
      B.WorkspaceSvg.prototype.__bf6t_newBlock = true;
      B.WorkspaceSvg.prototype.newBlock = function (type) {
        send({ op: 'log', level: 'display', text: 'trail: newBlock ' + type + ' ...' });
        var t0 = Date.now();
        try { return origNew.apply(this, arguments); }
        finally {
          send({ op: 'log', level: 'display',
                 text: 'trail: newBlock ' + type + ' done in ' + (Date.now() - t0) + 'ms' });
        }
      };
      installed++;
    }

    /* A dropdown that builds its own options is the classic way one block
     * takes for ever: the generator runs on creation and can loop. */
    if (B.FieldDropdown && B.FieldDropdown.prototype) {
      wrap(B.FieldDropdown.prototype, 'getOptions', 'dropdown options');
    }
    wrap(FL, 'createBlock', 'flyout create block', function (b) {
      try { return '(' + (b && b.type ? b.type : '?') + ')'; } catch (e) { return ''; }
    });
    if (B.Flyout && B.Flyout.prototype) {
      wrap(B.Flyout.prototype, 'placeNewBlock', 'flyout place new block');
    }

    /* Rendering one block is normal and constant, so logging every one would
     * bury the answer. Only a render that took real time is worth a line. */
    if (B.BlockSvg && B.BlockSvg.prototype && !B.BlockSvg.prototype.__bf6t_render) {
      var origRender = B.BlockSvg.prototype.render;
      B.BlockSvg.prototype.__bf6t_render = true;
      B.BlockSvg.prototype.render = function () {
        var t0 = Date.now();
        try { return origRender.apply(this, arguments); }
        finally {
          var ms = Date.now() - t0;
          if (ms > 250) {
            send({ op: 'log', level: 'display', text: 'trail: SLOW render of ' +
              (this.type || '?') + ' took ' + ms + 'ms' });
          }
        }
      };
      installed++;
    }

    send({ op: 'log', level: 'display',
           text: 'trail: freeze trail installed on ' + installed + ' step(s)' });
  }

  function enableDragPan() {
    var host = document.getElementById('canvas');
    if (!host) {
      send({ op: 'log', level: 'error', text: 'pan: no #canvas element, panning not installed' });
      return;
    }
    /* The workspace is re-injected whenever the style or the definitions
     * change, and this is called each time. The listeners live on the host div
     * and on window, neither of which is replaced, so installing again would
     * just stack another copy of every handler on top of the last. */
    if (host.__bf6PanInstalled) { return; }
    host.__bf6PanInstalled = true;
    send({ op: 'log', level: 'display', text: 'pan: middle drag and Alt drag installed on #canvas' });
    var sawDown = false;
    var panning = false, spaceDown = false;
    var fromX = 0, fromY = 0, atX = 0, atY = 0;

    function metrics() {
      try { return UI.ws.getMetrics(); } catch (e) { return null; }
    }
    /* WHICH NUMBER scroll() ACTUALLY WANTS.
     *
     * ws.scroll(x, y) takes a canvas TRANSLATION, which is what ws.scrollX and
     * ws.scrollY hold, and it is negative once the view has moved off the
     * origin. It is not metrics.scrollLeft, which is the positive distance from
     * the content's left edge.
     *
     * Starting a drag from metrics.scrollLeft handed scroll() a positive number
     * on a workspace whose translation was around -2000. The first thing
     * scroll() does is clamp: Math.min(x, -metrics.scrollLeft). So every drag
     * collapsed to the same clamped boundary and the view either jumped once
     * and then refused to move, or did not move at all - the "asked for
     * +200,+120, moved 0,0" report on the large workspace.
     *
     * Blockly's own scrollbar round-trips through ws.scrollX/scrollY, so that
     * is the convention this follows. */
    function translation() {
      var x = UI.ws && typeof UI.ws.scrollX === 'number' ? UI.ws.scrollX : null;
      var y = UI.ws && typeof UI.ws.scrollY === 'number' ? UI.ws.scrollY : null;
      return (x === null || y === null) ? null : { x: x, y: y };
    }
    function start(e) {
      var t = translation();
      if (!t) { return false; }
      panning = true;
      fromX = e.clientX; fromY = e.clientY;
      atX = t.x; atY = t.y;
      host.style.cursor = 'grabbing';
      return true;
    }
    function stop() {
      if (!panning) { return; }
      panning = false;
      host.style.cursor = spaceDown ? 'grab' : '';
    }

    // Blockly consumes pointerdown and can suppress the compatibility mouse
    // event. Claim the pan modifier before its gesture handler runs.
    host.addEventListener('pointerdown', function (e) {
      /* Once only: says which buttons actually reach the page, which is the
       * thing in doubt when a gesture does nothing. */
      /* Once only: says which buttons actually reach the page, which is the
       * thing in doubt when a gesture does nothing. With the trail on it
       * reports every click and what it landed on, plus whether the click ever
       * returned, which separates a hang inside the handler chain from one in
       * work scheduled afterwards. */
      if (!sawDown || UI.trailOn) {
        var tgt = '';
        try {
          var el = e.target;
          tgt = (el && el.tagName ? el.tagName.toLowerCase() : '?') +
                (el && el.getAttribute && el.getAttribute('class')
                  ? '.' + String(el.getAttribute('class')).split(' ')[0] : '');
        } catch (err) { tgt = '?'; }
        sawDown = true;
        send({ op: 'log', level: 'display', text: 'pan: mousedown button=' +
          e.button + ', space=' + spaceDown + ', on ' + tgt });
        if (UI.trailOn) {
          setTimeout(function () {
            send({ op: 'log', level: 'display', text: 'trail: click returned, page still alive' });
          }, 0);
        }
      }
      // 1 is the middle button. Space plus the left button does the same.
      if (e.button !== 1 && !(spaceDown && e.button === 0)) { return; }
      send({ op: 'log', level: 'display', text: 'pan: starting a pan (button ' + e.button + ')' });
      if (start(e)) { e.preventDefault(); e.stopPropagation(); }
    }, true);

    window.addEventListener('pointermove', function (e) {
      if (!panning) { return; }
      try { UI.ws.scroll(atX + (e.clientX - fromX), atY + (e.clientY - fromY)); }
      catch (err) { stop(); return; }
      e.preventDefault();
    }, true);

    window.addEventListener('pointerup', function () { stop(); }, true);
    window.addEventListener('pointercancel', function () { stop(); }, true);
    window.addEventListener('blur', function () { spaceDown = false; stop(); });

    // Space is a modifier here, not a character: it must not scroll the page or
    // press whatever control happens to have focus.
    /* ALT, NOT SPACE.
     *
     * Space is bound in the editor underneath this page: pressing it opens the
     * radial tools menu behind the browser, which is both wrong and audible.
     * Alt is not, and it is held rather than typed, so it does not fight a
     * text field either. */
    window.addEventListener('keydown', function (e) {
      if (e.key !== 'Alt' || spaceDown) { return; }
      var t = e.target;
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.isContentEditable)) { return; }
      spaceDown = true;
      if (!panning) { host.style.cursor = 'grab'; }
      e.preventDefault();
    });
    window.addEventListener('keyup', function (e) {
      if (e.key !== 'Alt') { return; }
      spaceDown = false;
      if (!panning) { host.style.cursor = ''; }
    });

    // Middle click otherwise starts the browser's own autoscroll.
    host.addEventListener('auxclick', function (e) {
      if (e.button === 1) { e.preventDefault(); }
    }, true);
  }

  function workspaceOptions() {
    var o = BF6.state.siteOptions || {};
    // The captured renderer if our Blockly knows it; otherwise the closest base
    // renderer with every captured measurement forced onto its constants, which
    // is what makes the notches, tabs, corners and hats line up. With nothing
    // captured at all, geras: Portal's blocks are bevelled and carry the classic
    // side notch and puzzle tab, and the status pill says the look is a guess.
    var renderer = null;
    try { renderer = BF6.installStyledRenderer('bf6portal'); } catch (e) { renderer = null; }
    if (!renderer) {
      var known = false;
      try {
        known = !!o.renderer && Blockly.registry.hasItem(
          Blockly.registry.Type.RENDERER, o.renderer);
      } catch (e) { known = false; }
      renderer = known ? o.renderer : 'geras';
    }
    var opts = {
      toolbox: toolboxFor(),
      renderer: renderer,
      rtl: !!o.rtl,
      trashcan: o.trashcan !== false,
      comments: o.comments !== false,
      disable: o.disable !== false,
      collapse: o.collapse !== false,
      sounds: false,
      grid: o.grid || { spacing: 24, length: 3, colour: '#1b1f22', snap: false },
      // THE WHEEL ZOOMS. Blockly gives the wheel to whichever of these two
      // claims it: with move.wheel on it scrolls the canvas and zooming is
      // left to Ctrl and wheel, which is not what anyone reaches for on a
      // workspace this size. Turning move.wheel off hands the plain wheel to
      // zoom.wheel below. Dragging still pans and the scrollbars still work,
      // so nothing is taken away.
      move: {
        scrollbars: (o.move && o.move.scrollbars !== undefined) ? o.move.scrollbars : true,
        drag: (o.move && o.move.drag !== undefined) ? o.move.drag : true,
        wheel: false
      },
      zoom: {
        controls: true, wheel: true, startScale: (o.zoom && o.zoom.startScale) || 1,
        maxScale: (o.zoom && o.zoom.maxScale) || 3,
        // HOW FAR OUT YOU CAN GO: AS FAR AS THE PROJECT NEEDS.
        //
        // The site's captured options say minScale 0.25, and for a while this
        // deferred to them because deferring to the capture is right almost
        // everywhere else. It is wrong here. A quarter scale does not fit a
        // 45,000 pixel tall mod on screen, so honouring the site's number
        // stopped a creator seeing their own work - and the site's number is a
        // decision the site made for the site, not a fact about Blockly.
        //
        // Everything else in the zoom options is still the site's. Only the
        // floor is ours, and it is set low enough that no project can reach it:
        // refreshZoomFloor lowers it further still if one ever does.
        minScale: 0.001,
        scaleSpeed: (o.zoom && o.zoom.scaleSpeed) || 1.2, pinch: true
      }
    };
    // Blockly's media folder: the mirror's own copy of the three pictures the
    // site serves, when there is a mirror. Left unset otherwise, because a
    // wrong path is worse than the default.
    var st = BF6.state.style;
    if (st && st.mediaPath) opts.media = st.mediaPath;
    var theme = themeFor();
    if (theme) opts.theme = theme;
    return opts;
  }

  // The classes the site's own stylesheet is scoped to, taken from what was
  // captured off the site rather than written in here.
  function wearSiteScopeClasses() {
    try {
      var st = BF6.state.style || {};
      var r = st.renderer && (st.renderer.name || st.renderer.registryKey);
      var t = st.theme && st.theme.name;
      var div = UI.ws && UI.ws.getInjectionDiv && UI.ws.getInjectionDiv();
      if (!div) return;
      if (r) div.classList.add(String(r) + '-renderer');
      if (t) div.classList.add(String(t).replace(/_/g, '-') + '-theme');
    } catch (e) { UI.reportFault('putting the site scope classes on', e); }
  }

  function rebuildWorkspace(preserve) {
    var wsJson = UI.ws && preserve !== false ? BF6.saveWorkspace(UI.ws) : null;
    if (UI.glyphTimer) clearInterval(UI.glyphTimer);
    if (UI.sideObserver) UI.sideObserver.disconnect();
    UI.staleById = {};
    if (UI.ws) {
      if (UI.ws.bf6Viewport) UI.ws.bf6Viewport.dispose();
      UI.ws.dispose(); UI.ws = null;
    }
    UI.ws = Blockly.inject('canvas', workspaceOptions());
    if (window.BF6Viewport) window.BF6Viewport.install(Blockly, UI.ws);
    enableDragPan();
    /* OFF, and kept. The trail wraps eighteen Blockly entry points and narrates
     * each one to the log, which is how the toolbox freeze was finally cornered
     * after the editor, the console, the MCP tools and Chrome's own debugger
     * had all proved useless (they share the thread that hangs). It costs two
     * log lines per wrapped call, so it does not belong on by default. Flip
     * this to true when something hangs and the log stops. */
    if (UI.trailOn) { installFreezeTrail(); }

    // THE SITE'S OWN CSS IS SCOPED, AND WE WERE OUTSIDE THE SCOPE.
    //
    // Blockly names the injection div after its renderer and its theme:
    // '<renderer>-renderer <theme>-theme'. The site's comes out as
    // 'portal-renderer portal-dark-theme' and ours as
    // 'bf6portal-renderer bf6portal_5-theme', because we register both under
    // names of our own. Nearly every rule in the 49 KB of css we capture off
    // the site begins '.portal-renderer.portal-dark-theme', so on our page
    // not one of them matched anything. That is the same fault as the missing
    // 'blockly' class, one level deeper, and it is why a label the site
    // paints orange came out white here.
    //
    // The captured names are used rather than a pair written in: if the site
    // renames its theme, the next capture carries it.
    wearSiteScopeClasses();

    // HOOKED WHERE THE SCALE CHANGES, and on the workspace that exists.
    //
    // Two mistakes in one line before this: setScale does not raise the
    // viewport event the listener was on, so zooming left the floor asleep;
    // and the hook was installed in applyStyle, which then REBUILDS the
    // workspace, so it was put on an object that was thrown away moments
    // later. The measurement said 'text drawn' at a third of full size both
    // times, which is what made it findable.
    UI.textHidden = undefined;
    try {
      var origSetScale = UI.ws.setScale;
      UI.ws.setScale = function (s) {
        var r = origSetScale.call(this, s);
        updateTextFloor();
        return r;
      };
    } catch (e) { UI.reportFault('watching the zoom', e); }
    updateTextFloor();

    // The two categories the site fills at open time, and the buttons that
    // stand at the top of each. Registered before anything can open them.
    UI.ws.registerToolboxCategoryCallback('CUSTOM_VARIABLE_FLYOUT', variablesFlyout);
    UI.ws.registerToolboxCategoryCallback('CUSTOM_SUBROUTINE_FLYOUT', subroutinesFlyout);
    UI.ws.registerButtonCallback('BF6_MANAGE_VARIABLES', manageVariables);
    UI.ws.registerButtonCallback('BF6_CREATE_SUBROUTINE', createSubroutine);
    installSearchbar();
    // The symbol inside an empty socket is drawn by the renderer, the way the
    // site draws it. Installed on the drawer once the workspace exists, so
    // every block rendered from here on carries it.
    try { BF6.installValueIcons(); } catch (e) { UI.reportFault('installing the socket symbols', e); }
    // And the measured layout rules: the site's row spacing, and the room a
    // socket keeps for the symbol we now draw inside it.
    try { BF6.installLayoutRules(); } catch (e) { UI.reportFault('installing the layout rules', e); }
    // The toolbox builds its flyout lazily, so this can be too early. Retried
    // briefly rather than left undone, because a silent no-op here shows up
    // much later as blocks running into each other.
    if (!installFlyoutSizing()) {
      var tries = 0;
      var t = setInterval(function () {
        if (installFlyoutSizing() || ++tries > 40) clearInterval(t);
      }, 100);
    }
    // The socket glyphs follow the connections: shown on an empty socket,
    // hidden the moment one is filled. Cheap because it only touches the block
    // the event names.
    UI.ws.addChangeListener(function (ev) {
      if (!ev) return;
      var T = Blockly.Events;
      if (ev.type !== T.BLOCK_MOVE && ev.type !== T.BLOCK_CREATE &&
          ev.type !== T.BLOCK_DELETE && ev.type !== T.BLOCK_CHANGE) return;
      UI.glyphSeen = (UI.glyphSeen || 0) + 1;

      /* A DRAG IS ONE MOVE EVENT PER MOUSE MOVE, AND THIS USED TO RENDER ON
       * EVERY ONE OF THEM.
       *
       * syncSocketGlyphs ends in block.render() whenever it changed a glyph,
       * and it was being run on the dragged block AND its parent for every
       * move event the drag produced. On a subroutine with hundreds of
       * children that is a full subtree re-render per mouse movement, which is
       * why clicking around in a loaded workspace locked the editor up.
       *
       * The glyphs only ever depend on whether a socket is FILLED. Sliding a
       * block around cannot change that; only re-parenting can. So a move that
       * kept the same parent is skipped, and a plain field edit is skipped too
       * because typing in a field does not fill or empty a socket. A mutation
       * can add or remove inputs, so that one still goes through.
       */
      if (ev.type === T.BLOCK_MOVE && ev.oldParentId === ev.newParentId) { return; }
      if (ev.type === T.BLOCK_CHANGE && ev.element !== 'mutation') { return; }

      UI.glyphRan = (UI.glyphRan || 0) + 1;
      try {
        var b = ev.blockId ? UI.ws.getBlockById(ev.blockId) : null;
        if (b) {
          UI.glyphRenders = (UI.glyphRenders || 0) + BF6.syncSocketGlyphs(b);
          var p = b.getParent && b.getParent();
          if (p) { UI.glyphRenders = (UI.glyphRenders || 0) + BF6.syncSocketGlyphs(p); }
        }
      } catch (e) {}
    });
    /* WHAT THE EDITOR IS DOING WHILE IT LOOKS FROZEN.
     *
     * A locked up page writes nothing, so a freeze and a page that has simply
     * finished look identical from outside. This counts the work the glyph
     * listener causes and says so once a second while there is any, which
     * turns "it locked up when I clicked" into a number. Silent when idle, so
     * it costs nothing and does not fill the log. */
    UI.glyphTimer = setInterval(function () {
      if (!UI.glyphSeen && !UI.glyphRan) { return; }
      send({ op: 'log', level: 'display', text:
        'glyph listener: ' + UI.glyphSeen + ' event(s) in the last second, ' +
        UI.glyphRan + ' ran, ' + (UI.glyphSeen - UI.glyphRan) + ' skipped, ' +
        UI.glyphRenders + ' render(s)' });
      UI.glyphSeen = 0; UI.glyphRan = 0; UI.glyphRenders = 0;
    }, 1000);

    watchCollapse(UI.ws);
    installBlockTools();
    installTabTools();
    /* A block created while a socket is pending came from the offered flyout,
     * so it goes into that socket. */
    UI.ws.addChangeListener(function (ev) {
      if (!ev || ev.type !== Blockly.Events.BLOCK_CREATE || !UI.pendingSocket) { return; }
      try { connectPendingSocket(UI.ws.getBlockById(ev.blockId)); } catch (e) {}
    });
    /* The bookmarks step aside when the helper panel opens. It is toggled from
     * several places, so the class is watched rather than every caller found. */
    try {
      var sideEl = $('side');
      if (sideEl && window.MutationObserver) {
        UI.sideObserver = new MutationObserver(positionRegionTabs);
        UI.sideObserver.observe(sideEl, { attributes: true, attributeFilter: ['class'] });
      }
    } catch (e) {}
    /* CULL WHILE THE VIEW MOVES, NOT ONLY WHEN IT SETTLES.
     *
     * The off screen sweep ran on a 250ms settle timer, so a pan repainted
     * every stack for the whole gesture and only stopped once the mouse did.
     * Blockly reports a viewport change on every scroll and zoom step, which is
     * exactly when the answer changes. Throttled to one sweep a frame or so:
     * the test is tens of comparisons on top level stacks, not thousands. */
    UI.ws.addChangeListener(function (ev) {
      if (!ev || ev.type !== Blockly.Events.VIEWPORT_CHANGE) { return; }
      if (!document.body.classList.contains('perf-heavy')) { return; }
      if (UI.cullPending) { return; }
      UI.cullPending = true;
      requestAnimationFrame(function () {
        UI.cullPending = false;
        try { cullFarImages(); } catch (e) {}
      });
    });
    /* The pull-out changes width when a category opens, so the strip has to be
     * re-measured. A frame later: Blockly lays the flyout out after the event. */
    UI.ws.addChangeListener(function (ev) {
      if (!ev || ev.type !== 'toolbox_item_select') { return; }
      setTimeout(positionRegionTabs, 0);
      setTimeout(positionRegionTabs, 120);
    });
    /* FOLLOW THE CALL. Double clicking a CALL block goes to the subroutine it
     * names; double clicking the subroutine cycles through its callers. A
     * double click is free here: Blockly uses single click to select and drag,
     * and nothing else claims the second one. */
    UI.ws.addChangeListener(function (ev) {
      if (!ev || ev.type !== Blockly.Events.CLICK) { return; }
      var now = Date.now();
      var same = (UI.lastClickId === ev.blockId) && (now - (UI.lastClickAt || 0) < 400);
      UI.lastClickId = ev.blockId; UI.lastClickAt = now;
      if (!same || !ev.blockId) { return; }
      var b = UI.ws.getBlockById(ev.blockId);
      if (b && (b.type === 'subroutineInstanceBlock' || b.type === 'subroutineBlock')) {
        followCall(b);
      }
    });

    UI.ws.addChangeListener(onChange);
    UI.ws.addChangeListener(onSelect);
    if (wsJson) BF6.loadWorkspace(UI.ws, wsJson);
    installCustomTooltip();
    installSlotAffordances();
    refreshNavigator();
    drawMinimap();
    patchMediaImages();
  }

  // ---- tooltips, the way the site writes them ----------------------------
  function installCustomTooltip() {
    try {
      Blockly.Tooltip.setCustomTooltip(function (div, element) {
        var type = element && element.type;
        if (!type && element && element.getSourceBlock) type = element.getSourceBlock().type;
        var html = type ? BF6.tooltipOf(type) : '';
        div.innerHTML = '';
        var wrap = el('div', 'tip');
        if (html) wrap.innerHTML = html;
        else wrap.textContent = (element && element.tooltip) ? String(element.tooltip) : String(type || '');
        var cat = type ? BF6.categoryOf(type) : '';
        if (cat) wrap.appendChild(el('div', 'tip-cat', cat));
        div.appendChild(wrap);
      });
    } catch (e) { /* older Blockly: the default tooltip stays */ }
  }

  // ---- the help panel -----------------------------------------------------
  function showHelp(block) {
    var box = $('help');
    if (!box) return;
    box.innerHTML = '';
    if (!block) { box.appendChild(el('div', 'dim', 'Select a block to see what it takes and what it gives back.')); return; }
    var type = block.type;
    box.appendChild(el('div', 'help-type', type));
    var cat = BF6.categoryOf(type);
    if (cat) box.appendChild(el('div', 'help-cat', cat));
    var sig = (BF6.state.signatures || {})[type] || [];
    sig.forEach(function (s) {
      var line = el('div', 'help-sig');
      line.textContent = s.name + '(' + s.args.join(', ') + ')' + (s.ret ? ' : ' + s.ret : '');
      box.appendChild(line);
    });
    var raw = BF6.tooltipOf(type);
    if (raw) { var d = el('div', 'help-raw'); d.innerHTML = raw; box.appendChild(d); }
    var inputs = el('div', 'help-inputs');
    (block.inputList || []).forEach(function (inp) {
      if (!inp.name) return;
      var want = BF6.slotWants(type, inp.name);
      var row = el('div', 'help-input');
      row.textContent = inp.name + '  ' + (inp.type === Blockly.inputTypes.STATEMENT ? 'statement' :
        inp.type === Blockly.inputTypes.VALUE ? 'value' : 'label') +
        (want ? '  takes ' + want.join(' | ') : '');
      inputs.appendChild(row);
    });
    box.appendChild(inputs);
    var url = (BF6.state.helpUrls || {})[type];
    if (url) {
      var a = el('button', 'ghost', 'OPEN DOCS');
      a.onclick = function () { send({ op: 'openUrl', url: url }); };
      box.appendChild(a);
    }
    var theme = themeForBlock(type);
    if (theme && (BF6.state.faq || UI.answers)) {
      var c = el('button', 'ghost', 'ANSWERS ABOUT ' + theme.toUpperCase());
      c.onclick = function () {
        if ($('answerbox')) { $('answerbox').value = theme; refreshAnswers(theme); }
        $('faqbox').value = '';
        refreshFaq('', theme);
        showPane('ask');
      };
      box.appendChild(c);
    }
  }

  // ==========================================================================
  // Answers
  //
  // Two things, kept apart on purpose and never merged into one list:
  //
  //   the curated answers  Resources/faq/answers/*.json. Written out, with a
  //                        one line summary, the gotchas, a block example that
  //                        has been loaded headlessly and checked, and the same
  //                        thing again in TypeScript. INSERT drops the example
  //                        on the canvas through the same path a snippet takes.
  //
  //   the threads          Resources/script/faq.json, the wider mined corpus:
  //                        what creators asked each other, in their own words,
  //                        with nothing checked. It sits below the answers,
  //                        under a line that says exactly that, so a curated
  //                        answer can never be mistaken for a raw thread.
  //
  // Both arrive from the tool, because a file page cannot read a local file.
  // ==========================================================================
  function loadAnswers(sets, note) {
    var all = [];
    (sets || []).forEach(function (s) {
      (s.entries || []).forEach(function (e) {
        // Only what this editor can actually show. An entry with no blocks
        // example still belongs here: the words are the answer, the example is
        // the demonstration.
        var eds = [].concat(e.editors || []).map(function (x) { return String(x).toLowerCase(); });
        if (eds.length && eds.indexOf('blocks') < 0) return;
        var copy = JSON.parse(JSON.stringify(e));
        copy.set = s.theme || '';
        all.push(copy);
      });
    });
    UI.answers = all;
    UI.answersNote = note || '';
    refreshAnswers($('answerbox') ? $('answerbox').value : '');
  }

  // Search across the question, the answer and the gotchas, which is where the
  // thing a person is actually trying to do is written down. Every word in the
  // query has to land somewhere.
  function searchAnswers(query, limit) {
    var q = String(query || '').toLowerCase().trim();
    var words = q ? q.split(/\s+/) : [];
    var out = [];
    (UI.answers || []).forEach(function (e) {
      var hay = ((e.question || '') + ' ' + (e.short || '') + ' ' + (e.answer || '') + ' ' +
        [].concat(e.gotchas || []).join(' ') + ' ' +
        [].concat(e.themes || []).join(' ')).toLowerCase();
      for (var i = 0; i < words.length; i++) if (hay.indexOf(words[i]) < 0) return;
      var score = 0;
      if (q && (e.question || '').toLowerCase().indexOf(q) >= 0) score += 100;
      if (q && (e.short || '').toLowerCase().indexOf(q) >= 0) score += 50;
      out.push({ e: e, score: score });
    });
    out.sort(function (a, b) { return b.score - a.score; });
    return out.slice(0, limit || 20).map(function (r) { return r.e; });
  }
  UI.searchAnswers = searchAnswers;

  function refreshAnswers(query) {
    var box = $('answerlist');
    if (!box) return;
    box.innerHTML = '';
    if (!UI.answers) {
      box.appendChild(el('div', 'dim', 'The answers have not arrived from the tool yet.'));
      return;
    }
    if (!UI.answers.length) {
      box.appendChild(el('div', 'dim',
        'No answer sets are installed. They live in Resources/faq/answers.'));
      return;
    }
    var rows = searchAnswers(query, 20);
    if (!rows.length) {
      box.appendChild(el('div', 'dim', 'No answer matches that. Try fewer words, or read the threads below.'));
      return;
    }
    rows.forEach(function (e) { box.appendChild(answerRow(e)); });
  }

  function answerRow(e) {
    var wrap = el('div', 'ans');
    var q = el('div', 'ans-q');
    q.appendChild(document.createTextNode(e.question || '(question)'));
    if (e.short) q.appendChild(el('span', 'ans-short', e.short));
    var body = el('div', 'ans-body');
    q.onclick = function () { body.classList.toggle('on'); };
    wrap.appendChild(q);
    wrap.appendChild(body);

    if (e.answer) body.appendChild(el('div', 'ans-text', e.answer));

    var gotchas = [].concat(e.gotchas || []);
    if (gotchas.length) {
      body.appendChild(el('div', 'h', 'Watch out for'));
      gotchas.forEach(function (g) { body.appendChild(el('div', 'ans-gotcha', '- ' + g)); });
    }

    // The block example, one click onto the canvas. This is the point of the
    // panel: a beginner sees the shape rather than reads about it.
    var ws = e.blocks && e.blocks.workspace;
    if (ws) {
      body.appendChild(el('div', 'h', 'The rule'));
      if (e.blocks.note) body.appendChild(el('div', 'sub', e.blocks.note));
      var ins = el('button', 'row');
      ins.appendChild(el('span', 't', 'Insert this rule'));
      ins.appendChild(document.createTextNode(
        'Drops it on the canvas where you can read it and change it.'));
      ins.onclick = function () {
        pasteSnippet(ws, e.blocks.ruleName || e.question || 'answer');
        closeShelf();
      };
      body.appendChild(ins);
      var v = e.verified && e.verified.blocks;
      body.appendChild(el('div', v ? 'ans-verified' : 'ans-unverified',
        v ? 'This example has been loaded and checked' : 'This example has not been checked'));
    }

    // The same thing in TypeScript, for anyone moving between the two editors.
    if (e.ts && e.ts.code) {
      var show = el('button', 'ghost', 'SHOW THE SAME THING IN CODE');
      var code = el('div', 'ans-code', e.ts.code);
      show.onclick = function () {
        code.classList.toggle('on');
        show.textContent = code.classList.contains('on')
          ? 'HIDE THE CODE' : 'SHOW THE SAME THING IN CODE';
      };
      body.appendChild(show);
      if (e.ts.note) body.appendChild(el('div', 'sub', e.ts.note));
      body.appendChild(code);
    }

    var srcs = [].concat(e.sources || []);
    if (srcs.length) {
      body.appendChild(el('div', 'ans-src', 'Written from: ' + srcs.join('; ')));
    }
    return wrap;
  }

  // ---- community threads --------------------------------------------------
  // Entries come from Resources/script/faq.json, which the tool reads and
  // pushes in (a file:// page cannot fetch a local file). Blocks-editor and
  // both-editor entries only; a missing file just means the panel says so.
  var FAQ_THEMES = ['spawn', 'ui', 'sound', 'vehicle', 'capture point', 'team',
    'timer', 'variable', 'event', 'area trigger', 'bots'];

  function loadFaq(entries) {
    /* THE WHOLE DOCUMENT ARRIVES, NOT THE LIST INSIDE IT.
     *
     * faq.json is an object with version, source, note, themes, counts and
     * entries, and the tool sends that object under the name "entries". So
     * this was handed a document where it expected an array and threw
     * "(entries || []).filter is not a function" on every launch, which took
     * the FAQ panel out entirely and all 2,736 answers with it.
     *
     * Nobody saw it because the throw happened inside the chunked-message
     * handler, whose catch reported it as a corrupt message and printed six
     * words to a status strip at the bottom of the page.
     *
     * Both shapes are accepted, so this keeps working whichever end changes.
     */
    if (entries && !Array.isArray(entries) && Array.isArray(entries.entries)) {
      entries = entries.entries;
    }
    BF6.state.faq = (entries || []).filter(function (e) {
      var ed = String(e.editor || 'both').toLowerCase();
      return ed === 'blocks' || ed === 'both';
    });
    refreshFaq();
  }

  function searchFaq(query, theme, limit) {
    var q = String(query || '').toLowerCase().trim();
    var th = String(theme || '').toLowerCase().trim();
    var out = [];
    (BF6.state.faq || []).forEach(function (e) {
      if (out.length >= (limit || 25)) return;
      var themes = [].concat(e.theme || e.themes || []).join(' ').toLowerCase();
      if (th && themes.indexOf(th) < 0) return;
      if (q) {
        var hay = ((e.question || '') + ' ' + (e.answer || '') + ' ' + themes).toLowerCase();
        if (hay.indexOf(q) < 0) return;
      }
      out.push(e);
    });
    return out;
  }
  UI.searchFaq = searchFaq;

  function themeForBlock(type) {
    var hay = (type + ' ' + BF6.categoryOf(type)).toLowerCase();
    for (var i = 0; i < FAQ_THEMES.length; i++) {
      var t = FAQ_THEMES[i];
      if (hay.indexOf(t.replace(' ', '')) >= 0 || hay.indexOf(t) >= 0) return t;
    }
    return '';
  }

  function refreshFaq(query, theme) {
    var box = $('faqlist');
    if (!box) return;
    box.innerHTML = '';
    if (!BF6.state.faq) {
      box.appendChild(el('div', 'dim',
        'No community threads installed. Drop faq.json into Resources/script and reopen.'));
      return;
    }
    var rows = searchFaq(query, theme, 25);
    if (!rows.length) { box.appendChild(el('div', 'dim', 'No thread matches that.')); return; }
    rows.forEach(function (e) {
      var row = el('div', 'faq-row');
      var q = el('div', 'faq-q', e.question || '(question)');
      var a = el('div', 'faq-a', e.answer || '');
      a.style.display = 'none';
      q.onclick = function () { a.style.display = a.style.display === 'none' ? 'block' : 'none'; };
      row.appendChild(q); row.appendChild(a);
      var themes = [].concat(e.theme || e.themes || []).join(', ');
      if (themes) row.appendChild(el('div', 'faq-theme', themes));
      box.appendChild(row);
    });
  }


  // ---- change plumbing ----------------------------------------------------
  function onChange(e) {
    if (UI.pendingProject || UI.applying > 0 || BF6.state.loading > 0) return;
    if (!e || e.isUiEvent) return;
    if (UI.debounce) clearTimeout(UI.debounce);
    UI.debounce = setTimeout(flush, 150);
    if (e.type === Blockly.Events.BLOCK_CREATE || e.type === Blockly.Events.BLOCK_DELETE ||
      e.type === Blockly.Events.BLOCK_MOVE) {
      // ONE REBUILD, NOT ONE PER EVENT.
      //
      // This used to schedule a fresh navigator and minimap rebuild for every
      // single event with nothing to cancel the last, so dragging one block
      // across the canvas queued dozens of them and every one walks the whole
      // workspace measuring stacks. That is the lag: not the drawing, the
      // scanning, over and over, after the drag has already finished.
      //
      // Now the timer is replaced rather than added to, and nothing runs while
      // a drag is still in progress.
      // The zoom check runs NOW, not on the timer: it is one comparison, and
      // waiting a quarter second to stop drawing text is a quarter second of
      // the stutter it exists to remove.
      updateTextFloor();
      if (UI.viewTimer) clearTimeout(UI.viewTimer);
      UI.viewTimer = setTimeout(function () {
        UI.viewTimer = null;
        try { if (UI.ws.isDragging && UI.ws.isDragging()) return; } catch (err) {}
        // A block that re-rendered drew its own text again, so anything not
        // pictured is picked up here. Already-pictured blocks cost one property
        // read each, which is why this can run on every settle.
        textPictureSweep();
        // And the symbols on stacks that scrolled out of reach come off, while
        // the ones that scrolled in go back on. Only top level stacks are
        // tested, so this is tens of comparisons, not thousands.
        cullFarImages();
        refreshNavigator();
        drawMinimap();
        // The fast view is a snapshot of geometry, so it is re-taken when the
        // blocks change underneath it. Only while it is actually on screen.
      }, 250);
    }
  }

  function flush() {
    UI.debounce = null;
    if (!UI.ws || UI.pendingProject) return;
    /* A LOAD IS NOT A SET OF EDITS TO PUBLISH.
     *
     * Clearing the journal after a load was not enough: flush had already SENT
     * every unit to the site, and the log filled with
     *   [site] apply replaceTop failed for c0551 ...
     * as the site tried to apply a workspace nobody asked it to take. Opening a
     * file on this machine must not write to a live experience, so nothing goes
     * out until the load has settled. */
    if (UI.loadingDoc || UI.applying || BF6.state.loading) { return; }
    var msgs = BF6.diffUnits(UI.ws);
    var vars = JSON.stringify(BF6.variableList(UI.ws));
    if (vars !== UI.lastVars) {
      UI.lastVars = vars;
      msgs.push({ op: 'variables', list: JSON.parse(vars) });
    }
    msgs.forEach(function (m) {
      if (UI.ruleMode && m.id === UI.ruleMode.id && m.op === 'replaceTop') m.anchor = UI.ruleMode.anchor;
      m.seq = ++UI.seq;
      // Every unit stays in the journal until the site says it applied it. A
      // sign-out mid-edit then costs nothing: the journal is what gets replayed.
      if (m.op === 'replaceTop') UI.journal[m.seq] = { id: m.id, op: m.op };
      else if (m.op === 'deleteTop') UI.journal[m.seq] = { ids: m.ids, op: m.op };
      send(m);
    });
    if (msgs.length) {
      status(UI.sessionLost
        ? msgs.length + ' change' + (msgs.length > 1 ? 's' : '') + ' held locally (' + pendingCount() + ' waiting)'
        : msgs.length + ' change' + (msgs.length > 1 ? 's' : '') + ' sent, seq ' + UI.seq);
    }
    scheduleAutosave();
    refreshStale();
  }

  // ---- the journal --------------------------------------------------------
  function pendingCount() { return Object.keys(UI.journal).length; }

  function journalIds() {
    var ids = {};
    Object.keys(UI.journal).forEach(function (seq) {
      var e = UI.journal[seq];
      if (e.id) ids[e.id] = 1;
      (e.ids || []).forEach(function (i) { ids[i] = 1; });
    });
    return Object.keys(ids);
  }

  function noteApplied(seq) {
    if (UI.journal[seq]) delete UI.journal[seq];
    paintPending();
  }

  function paintPending() {
    var p = $('pending');
    if (!p) return;
    var n = pendingCount();
    p.textContent = n ? n + ' NOT ON THE SITE YET' : '';
    p.className = n ? 'pill warnpill' : 'pill hidden';
  }

  // ---- autosave -----------------------------------------------------------
  // Two seconds after the last change, the whole workspace goes to disk beside
  // the experience. The site can sign anyone out at any moment; nothing typed
  // here should depend on the site being reachable.
  function scheduleAutosave() {
    if (UI.autosaveTimer) clearTimeout(UI.autosaveTimer);
    UI.autosaveTimer = setTimeout(function () {
      UI.autosaveTimer = null;
      if (!UI.ws) return;
      if (UI.loadFailed) return;
      if (UI.loadingDoc || UI.applying || BF6.state.loading) { scheduleAutosave(); return; }
      var json = projectDocument();
      var counts = BF6.countWorkspace(json);
      send({
        op: 'autosave', json: json, project: UI.hostProject || '', revision: UI.hostRevision || '',
        meta: {
          savedAt: new Date().toISOString(),
          blocks: counts.blocks, rules: counts.rules,
          subroutines: counts.subroutines, variables: counts.variables,
          pending: pendingCount(), pendingIds: journalIds(),
          sessionLost: !!UI.sessionLost
        }
      });
    }, 2000);
  }

  // ---- signed out, and back again ----------------------------------------
  function setSession(stateName, text) {
    UI.sessionLost = (stateName === 'lost');
    var b = $('banner');
    if (!b) return;
    if (UI.sessionLost) {
      b.textContent = text ||
        'Portal signed you out. Your edits are safe here and will be re-applied when you are back in.';
      b.className = 'banner on';
    } else {
      b.textContent = '';
      b.className = 'banner';
    }
    paintPending();
  }

  // The site handed back its workspace after a sign-out. Whatever we changed
  // while we were cut off wins; anything we never touched comes back from the
  // site, because another tab or another person may have moved it.
  function reconcileWithSite(siteJson) {
    var local = projectSnapshot();
    if (UI.region || UI.ruleMode) {
      UI.region = null; UI.ruleMode = null; UI.ruleModeStash = null;
      $('rulemode').classList.remove('on');
      UI.applying++;
      try { BF6.loadWorkspace(UI.ws,local); UI.doc = local; } finally { UI.applying--; }
    }
    var r = BF6.reconcile(local, siteJson, journalIds());
    UI.applying++;
    try {
      r.toLocal.forEach(function (u) {
        BF6.applyReplace(UI.ws, { id: u.id, json: u.json, anchor: u.anchor });
      });
    } finally { UI.applying--; }
    BF6.snapshotUnits(UI.ws);
    UI.journal = {};
    r.toSite.forEach(function (u) {
      var seq = ++UI.seq;
      UI.journal[seq] = { id: u.id, op: 'replaceTop' };
      send({ op: 'replaceTop', seq: seq, id: u.id, json: u.json, anchor: u.anchor });
    });
    send({
      op: 'log', level: 'log',
      text: 'reconciled after sign-in: ' + r.stats.pushed + ' unit(s) re-applied to the site, ' +
        r.stats.pulled + ' taken from the site, ' + r.stats.unchanged + ' unchanged, ' +
        r.stats.deleted + ' gone from the site'
    });
    status('back in: ' + r.stats.pushed + ' of your edits re-applied, ' +
      r.stats.pulled + ' taken from the site');
    after();
    return r;
  }
  UI.reconcileWithSite = reconcileWithSite;

  // Two versions of the same experience: the one on disk and the one the site
  // just returned. The counts of each are on the buttons, because that is what
  // tells a person which one is theirs.
  function offerRestore(siteJson, local) {
    var bar = $('restore');
    if (!bar) return;
    var lc = BF6.countWorkspace(local.json);
    var sc = BF6.countWorkspace(siteJson);
    bar.innerHTML = '';
    bar.className = 'restore on';
    bar.appendChild(el('span', 'restore-text',
      'This experience has unsaved local work from ' + (local.meta && local.meta.savedAt || 'an earlier session') +
      (local.meta && local.meta.pending ? ' with ' + local.meta.pending + ' edit(s) never confirmed by the site.' : '.')));
    var a = el('button', 'primary', 'RESTORE LOCAL  ' + lc.blocks + ' blocks, ' + lc.rules + ' rules');
    var b = el('button', null, 'USE SITE  ' + sc.blocks + ' blocks, ' + sc.rules + ' rules');
    a.onclick = function () {
      bar.className = 'restore';
      UI.region = null; UI.ruleMode = null; UI.ruleModeStash = null;
      $('rulemode').classList.remove('on');
      readExtendedSidecar(local.json);
      BF6.loadWorkspace(UI.ws, local.json);
      UI.doc = BF6.unwrap(local.json);
      UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));
      // Everything that differs from the site goes back out.
      var r = BF6.reconcile(BF6.saveWorkspace(UI.ws), siteJson,
        (local.meta && local.meta.pendingIds) || Object.keys(BF6.unitsOfJson(local.json)));
      r.toSite.forEach(function (u) {
        var seq = ++UI.seq;
        UI.journal[seq] = { id: u.id, op: 'replaceTop' };
        send({ op: 'replaceTop', seq: seq, id: u.id, json: u.json, anchor: u.anchor });
      });
      after();
      status('restored the local copy: ' + r.stats.pushed + ' unit(s) sent back to the site');
    };
    b.onclick = function () {
      bar.className = 'restore';
      UI.region = null; UI.ruleMode = null; UI.ruleModeStash = null;
      $('rulemode').classList.remove('on');
      readExtendedSidecar(siteJson);
      BF6.loadWorkspace(UI.ws, siteJson);
      UI.doc = BF6.unwrap(siteJson);
      UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));
      UI.journal = {};
      after();
      status('using the site copy');
    };
    bar.appendChild(a);
    bar.appendChild(b);
  }

  function onSelect(e) {
    // Opening the rule you clicked, and folding the one you left.
    if (!e || e.type !== Blockly.Events.SELECTED) return;
    var block = e.newElementId ? UI.ws.getBlockById(e.newElementId) : null;
    showHelp(block);
    if (!block) return;
    // Selecting a block that addresses placed objects lights them up in the level.
    var ids = [], kinds = [];
    var scan = [block].concat(block.getDescendants ? block.getDescendants(false) : []);
    scan.forEach(function (b) {
      var kind = BF6.OBJID_BLOCKS[b.type];
      if (!kind) return;
      var inp = b.getInput('VALUE-0');
      var child = inp && inp.connection && inp.connection.targetBlock();
      if (child && child.type === 'Number') {
        var v = Number(child.getFieldValue('NUM'));
        if (!isNaN(v)) { ids.push(v); kinds.push(kind); }
      }
    });
    send({ op: 'blockSelected', blockId: block.id, type: block.type, ids: ids, kinds: kinds });
  }

  // ---- messages in ---------------------------------------------------------
  // ---- hearing a sound from a block -------------------------------------
  //
  // A sound in a workspace is a dropdown value like SFX_Alarm - the same names
  // the script editor searches - so the same two paths serve it: the game
  // itself through the High Poly add-on, or a recorded clip played here. The
  // tool decides which; this only has to know whether there is one at all.
  var sfxSource = '';
  var sfxAudioEl = null;

  function sfxLooksLikeSound(v) {
    return typeof v === 'string' && /^(SFX_|FX_|VFX_)[A-Za-z0-9_]+$/.test(v);
  }

  function sfxPlayName(name) {
    if (sfxAudioEl) { try { sfxAudioEl.pause(); } catch (e) {} sfxAudioEl = null; }
    if (!sfxSource) { status('no sound library is available to play from', true); return; }
    send({ op: 'sfxplay', name: name });
  }

  function handle(msg) {
    if (!msg || !msg.op) return;
    switch (msg.op) {
      case 'sfxsource': { sfxSource = msg.source || ''; return; }
      case 'sfxplayed': {
        if (!msg.ok) { status(msg.why || ('could not play ' + msg.name), true); return; }
        // Empty audio means the editor played it itself, which is the good case.
        if (msg.audio) {
          try {
            sfxAudioEl = new Audio('data:audio/ogg;base64,' + msg.audio);
            sfxAudioEl.play();
          } catch (e) { status('could not play ' + msg.name, true); }
        }
        return;
      }
      case 'chunk': {
        var slot = UI.chunks[msg.cid] || (UI.chunks[msg.cid] = []);
        slot[msg.i] = msg.part;
        var done = true;
        for (var i = 0; i < msg.n; i++) if (slot[i] === undefined) done = false;
        if (done) {
          var text = slot.join('');
          delete UI.chunks[msg.cid];
          /* PARSING AND HANDLING ARE TWO DIFFERENT FAILURES.
           *
           * One try around both meant a handler that threw was reported as a
           * corrupt message, and the first real instance of this cost hours:
           * the text had parsed perfectly and the fault was a shape mismatch
           * three calls deeper. Parse here, hand off outside the catch. */
          var parsed = null, parseFailed = false;
          try { parsed = JSON.parse(text); }
          catch (e) {
            /* THIS USED TO STOP AT A STATUS STRIP AT THE BOTTOM OF THE PAGE.
             *
             * A big message from the tool arrives in pieces and is glued back
             * together here. When the glued text will not parse, the whole
             * message is lost: a definition set, a style capture, a workspace.
             * The editor then carries on in a half built state and the only
             * sign was six words the user happened to read off the screen.
             *
             * Everything known about the failure goes to the log now, because
             * which message was lost and where it broke is the whole question.
             */
            var where = -1;
            var m = /position (\d+)/.exec(String(e && e.message));
            if (m) { where = Number(m[1]); }
            send({ op: 'log', level: 'display', text:
              'trail: BAD CHUNKED MESSAGE cid=' + msg.cid + ' pieces=' + msg.n +
              ' joined=' + text.length + ' chars, error: ' +
              (e && e.message ? e.message : e) +
              (where >= 0 ? ' | around: ' +
                JSON.stringify(text.substr(Math.max(0, where - 60), 120)) : '') +
              ' | starts: ' + JSON.stringify(text.substr(0, 80)) });
            status('bad chunked message', true);
            parseFailed = true;
          }
          if (!parseFailed) {
            /* The pieces are all here and they parsed. Said out loud because a
             * load that dies after this point and one that never got the
             * message look identical from the log, and that ambiguity has cost
             * several rounds already. */
            trace('load: chunked message complete, op "' + (parsed && parsed.op) +
              '", ' + msg.n + ' piece(s), ' + text.length + ' chars');
            /* Outside the catch on purpose. A throw from here is a bug in the
             * handler and must report itself as one, naming the op. */
            try { handle(parsed); }
            catch (e2) {
              send({ op: 'log', level: 'display', text:
                'trail: HANDLER THREW on op "' + (parsed && parsed.op) + '" (' +
                text.length + ' chars, ' + msg.n + ' piece(s)): ' +
                (e2 && e2.message ? e2.message : e2) });
              UI.reportFault('handling a chunked ' + (parsed && parsed.op), e2);
            }
          }
        }
        break;
      }
      case 'defs': applyCapture(msg); break;
      case 'style': applyStyle(msg); break;
      case 'projectWorkspace': queueProjectWorkspace(msg); break;
      case 'workspace': {
        if (UI.loadingDoc || UI.applying || BF6.state.loading) {
          status('Wait for the current workspace to finish opening.', true); break;
        }
        // WHY THIS RUNS AGAIN FOR A FALLBACK.
        //
        // Offline definitions are built by OBSERVING a workspace: a field the
        // workspace actually carries a value for becomes a real editable
        // field, and one it never carries becomes a label showing the field's
        // own name, because inventing a serializable field would write itself
        // into the save. At boot there is no workspace yet, so everything is
        // observed as absent and every Text block comes out reading "Text
        // TEXT" instead of its own words.
        //
        // So when the real workspace arrives and the definitions are still the
        // offline ones, they are rebuilt against it first. Definitions that
        // came from the site are left alone: they are already right.
        /* Three steps run before a single block is built, and any of them can
         * be the one that never returns. Each says so, because a silence after
         * "Handed" told us only that the page had it. */
        trace('load: workspace message in');
        if (!Object.keys(BF6.state.types).length) {
          trace('load: building offline definitions (no types)');
          offlineDefinitions(msg.json);
        } else if (BF6.state.definitionsSource === 'fallback') {
          trace('load: rebuilding fallback definitions');
          offlineDefinitions(msg.json);
        }
        trace('load: definitions ready');
        if (!UI.ws) { trace('load: booting the canvas'); boot(); }
        if (msg.reconcile) { reconcileWithSite(msg.json); break; }
        trace('load: handing to the batched loader');
        /* IN BATCHES, SO THE EDITOR STAYS ALIVE.
         *
         * This is the path BF6.Blocks.LoadFile and the site both arrive on,
         * and it used to build the whole workspace in one call. The page runs
         * on Unreal's thread, so that froze the entire editor: a 2,000 block
         * slice held it for over a hundred seconds and 52,427 blocks never
         * returned. The rest of this case now runs when the batches finish. */
        UI.loadingDoc = true; UI.loadFailed = false;
        if (UI.debounce) { clearTimeout(UI.debounce); UI.debounce = null; }
        UI.applying++;
        loadWorkspaceIncremental(msg.json,
          function (n, t) { showLoadProgress(n, t, 'the workspace'); },
          function () { UI.applying--; hideLoadProgress(); afterWorkspaceMessage(msg); },
          function (stepName, e) {
            UI.applying--; UI.loadingDoc = false; UI.loadFailed = true; hideLoadProgress();
            UI.reportFault(stepName, e);
            status('could not load that workspace while ' + stepName + ': ' +
              ((e && e.message) || e), true);
          });
        break;
      }
      case 'applied': noteApplied(msg.seq); break;
      default: return handleRest(msg);
    }
  }

  /* What used to run straight after the workspace message. It now runs when the
   * batched load reports itself finished, which is later than the message that
   * started it. */
  function afterWorkspaceMessage(msg) {
        BF6.syncAllSocketGlyphs(UI.ws);
        // Use the same load boundary as file imports: deferred rendering and
        // annotation events must not be published as creator edits.
        finishWorkspaceDoc(msg.json, 'workspace loaded', function () {
          if (msg.local && msg.local.json) offerRestore(msg.json, msg.local);
        });
  }

  /* Every other message. Split out only so the workspace case above could
   * become asynchronous without reindenting the whole switch. */
  function handleRest(msg) {
    switch (msg.op) {
      case 'prepareUpdate':
        try {
          if (UI.pendingProject) throw Error('An experience is still opening.');
          send({op:'updateWorkspace', token:msg.token, json:projectDocument(),
            project:UI.hostProject || '', revision:UI.hostRevision || ''});
        } catch (e) { send({op:'updateWorkspace', token:msg.token, error:String(e.message || e)}); }
        break;
      case 'session': setSession(msg.state, msg.text); break;
      case 'portalResult': showPortalResult(msg); break;
      case 'replaceTop': UI.applying++; try { BF6.applyReplace(UI.ws, msg); } finally { UI.applying--; } after(); break;
      case 'deleteTop': UI.applying++; try { BF6.applyDelete(UI.ws, msg); } finally { UI.applying--; } after(); break;
      case 'variables': UI.applying++; try { BF6.applyVariables(UI.ws, msg.list); } finally { UI.applying--; } after(); break;
      case 'objIds': BF6.state.objIds = msg.rows || []; refreshStale(); refreshNavigator(); break;
      case 'sceneSelected': highlightScene(msg.objIds || []); break;
      case 'snippetList': UI.snippets = msg.names || []; refreshSnippets(); break;
      case 'snippet': pasteSnippet(msg.json, msg.name); break;
      case 'lootBinding':
        try {
          var lootRoot = window.BF6LootBinding.apply(UI.ws, Blockly, msg);
          flush(); after(); centreOn(lootRoot.id);
          send({op:'lootBindingResult', message:'Loot spawner ' + msg.objId + ' connected. Edit its event and conditions in the highlighted rule.'});
        } catch (lootError) {
          send({op:'lootBindingResult', message:'Loot connection: ' + lootError.message});
        }
        break;
      case 'gameLogLines': onGameLog(msg); break;
      case 'selfTest': selfTest(); break;
      /* SHOW THIS SOURCE AS BLOCKS.
       *
       * The Script editor has a "Show as blocks" lens over every function and
       * it has always answered "not wired yet", even though both ends existed:
       * the C++ side already opens this panel and loads a snippet, and this
       * panel already pastes one without disturbing the canvas. The only thing
       * missing was the conversion, and the converter only lives here.
       *
       * The WHOLE file is converted rather than the one function, because a
       * function on its own has no imports, no module variables and no event
       * registration, and would convert into far less than it means. The
       * requested name is then centred, so the user still lands on what they
       * clicked. */
      case 'sourceToBlocks': showSourceAsBlocks(msg.source || '', msg.name || '',
                                                msg.file || 'source.ts'); break;
      case 'useSelected': fillSelectedObjId(msg); break;
      case 'saveResult': status('SAVE: ' + (msg.text || ''), !msg.ok); break;
      case 'importFile':
        if (msg.typescriptUrl) UI.typescriptUrl = msg.typescriptUrl;
        importText(msg.text || '', msg.name || msg.path || '');
        break;
      case 'convertData':
        UI.convertCatalog = msg.catalog || null;
        UI.convertEvents = msg.events || null;
        /* The event table is what the extended event blocks build their
           dropdowns from, so they are registered again now it exists. */
        installExtended('the event table arrived');
        if (msg.typescriptUrl) UI.typescriptUrl = msg.typescriptUrl;
        break;
      case 'scriptFolder': importScriptFolder(msg.files || {}, msg.dir || ''); break;
      case 'exportScriptTo': exportAsScript(msg.dir || ''); break;
      case 'experience': UI.experience = msg; break;
      case 'mapChanged': onMapChanged(msg.map || ''); break;
      case 'exportResult':
        status(msg.ok ? 'wrote ' + msg.path + ' (' + msg.bytes + ' characters)'
          : 'could not write ' + msg.path, !msg.ok);
        break;
      case 'sendToSiteResult': status(msg.text || '', !msg.ok); break;
      case 'importControl':
        status(msg.selector
          ? 'the site\'s import control is ' + msg.selector + '. Paste the path the tool copied.'
          : 'no import control on that page: paste the copied path into the site\'s own import.',
          !msg.selector);
        break;
      case 'attachmentsDone': {
        var lines = (msg.results || []).map(function (r) { return r.filename + ': ' + r.what; });
        status(lines.length ? lines.join('   ') : 'nothing to bring in');
        break;
      }
      case 'probe': reportToolbox(); break;
      case 'perf': reportPerf(msg.scale); break;
      case 'perfTry': perfExperiment(msg.name, msg.on); break;
      case 'status': UI.lastStatus = msg; break;
      case 'connected':
        BF6.state.connected = !!msg.connected;
        /* Kept even when it is absent, so an older host simply leaves the pill
         * saying the page is not open rather than inventing a sign-in state. */
        if (typeof msg.signIn === 'string') { BF6.state.signIn = msg.signIn; }
        paintConnection();
        break;
      case 'faq': loadFaq(msg.entries); break;
      case 'answers': loadAnswers(msg.sets, msg.note); break;
      case 'prefs': applyPrefs(msg.values); break;
      case 'saveToFile':
        // One export path for the console command and the buttons alike.
        doExport(msg.format || 'workspace', msg.path || '');
        break;
      default: break;
    }
  }
  window.BF6Blocks.recv = function (text) {
    try { handle(typeof text === 'string' ? JSON.parse(text) : text); }
    catch (e) {
      status('bad message: ' + e, true);
      // AND SAY IT OUTSIDE THE PAGE. This used to stop at a status strip, so a
      // message that threw halfway through - leaving the editor half built -
      // looked exactly like a feature that was never wired up.
      var op = '';
      try { op = (typeof text === 'string' ? JSON.parse(text) : text).op || ''; } catch (e2) {}
      UI.reportFault('handling ' + (op || 'a message'), e);
    }
  };

  function after() {
    BF6.snapshotUnits(UI.ws);
    refreshNavigator(); drawMinimap(); refreshStale();
  }

  function paintConnection() {
    var d = $('conn');
    if (!d) return;
    /* WHAT THIS PILL IS ACTUALLY ABOUT.
     *
     * "connected" means the Portal blocks page has said hello inside the tool's
     * own web panel. It is not a statement about the internet, and it is not a
     * statement about being signed in.
     *
     * It used to read SITE OFFLINE for both, so somebody signed in to Portal
     * with the panel closed, or open on a different page, was told they were
     * offline and reasonably went looking for a connection problem that did not
     * exist. The two states have different fixes, so they get different words:
     * one needs signing in, the other needs the blocks page opened. */
    var signIn = BF6.state.signIn || '';
    if (BF6.state.connected) {
      d.textContent = 'SITE CONNECTED';
      d.className = 'pill on';
      d.title = 'The Portal blocks page is open in the panel, so edits can go across.';
    } else if (/^Linked/i.test(signIn)) {
      d.textContent = 'PAGE NOT OPEN';
      d.className = 'pill';
      d.title = 'You are signed in to Portal. The blocks page is not open in the panel, '
              + 'so nothing can be sent to it yet. Open PORTAL and go to the blocks page. '
              + 'Your work is safe here in the meantime.';
    } else if (signIn) {
      d.textContent = 'NOT SIGNED IN';
      d.className = 'pill';
      d.title = 'Portal says: ' + signIn + '. Open PORTAL and sign in. '
              + 'Your work is safe here in the meantime.';
    } else {
      d.textContent = 'PAGE NOT OPEN';
      d.className = 'pill';
      d.title = 'The Portal blocks page is not open in the panel, so nothing can be sent '
              + 'to it. Your work is safe here in the meantime.';
    }
  }

  // ---- what Portal said ---------------------------------------------------
  // A rejected save is shown on the blocks it is about. Our own two lint
  // findings sit beside it, because a duplicate or unresolved ObjId is the
  // usual reason a save comes back INVALID_ARGUMENT.
  function showPortalResult(msg) {
    var panel = $('portal');
    if (!panel) return;
    // The panel it lives in is closed most of the time, so the handle says
    // there is something behind it rather than the answer going unseen.
    markNews('wrong');
    panel.innerHTML = '';
    var ok = (msg.status === 0 || msg.status === undefined) && !(msg.blocks || []).length && !msg.message;
    if (ok) {
      BF6.clearPortalWarnings(UI.ws);
      panel.className = 'portal on good';
      panel.appendChild(el('div', 'portal-head', 'SAVED ON PORTAL AT ' +
        new Date().toLocaleTimeString()));
      after();
      return;
    }
    panel.className = 'portal on';
    var head = el('div', 'portal-head', msg.status !== undefined && msg.status !== 0
      ? 'PORTAL REFUSED THE SAVE  (grpc-status ' + msg.status + ')'
      : 'PORTAL REPORTED A PROBLEM');
    panel.appendChild(head);
    if (msg.message) panel.appendChild(el('div', 'portal-msg', msg.message));
    if (msg.dialog) panel.appendChild(el('div', 'portal-msg', msg.dialog));

    var relay = BF6.portalRelay(UI.ws, msg);
    (msg.blocks || []).forEach(function (row) {
      var r = el('div', 'portal-row');
      r.appendChild(el('div', 'portal-block', row.id + (row.type ? '  ' + row.type : '')));
      r.appendChild(el('div', 'portal-text', row.text || ''));
      if (relay.applied.indexOf(row.id) >= 0) r.onclick = function () { centreOn(row.id); };
      else r.appendChild(el('div', 'dim', 'not in this workspace'));
      panel.appendChild(r);
    });

    // The message often names a rule rather than a block id.
    if (msg.message) {
      var hit = null;
      UI.ws.getAllBlocks(false).forEach(function (b) {
        if (hit) return;
        if (msg.message.indexOf(b.id) >= 0) { hit = b.id; return; }
        var nm = b.getFieldValue && (b.getFieldValue('NAME') || b.getFieldValue('SUBROUTINE_NAME'));
        if (nm && nm.length > 2 && msg.message.indexOf(nm) >= 0) hit = b.id;
      });
      if (hit) {
        var jump = el('button', 'ghost', 'JUMP TO THE BLOCK IT NAMES');
        jump.onclick = function () { centreOn(hit); };
        panel.appendChild(jump);
      }
    }

    var findings = BF6.lintFindings(UI.ws);
    if (findings.length) {
      panel.appendChild(el('div', 'portal-head2', 'USUAL CAUSES FOUND HERE'));
      findings.slice(0, 20).forEach(function (f) {
        var r = el('div', 'portal-row');
        r.appendChild(el('div', 'portal-text', f.text));
        if (f.blockId) r.onclick = function () { centreOn(f.blockId); };
        panel.appendChild(r);
      });
    }
    after();
  }

  // ---- navigator ----------------------------------------------------------
  // ==========================================================================
  // REGIONS: the source file a block came from
  //
  // A converted project is one workspace of tens of thousands of blocks with no
  // seams in it. UGZ is 52,427 blocks in 428 top level items across 29 files,
  // and a flat list of 427 subroutines is not something a person can read.
  //
  // The converter lays one column per source file and now writes the file name
  // onto each top level block as `data`. That single fact is what lets the
  // editor group, tab, colour and scope by something a reader recognises,
  // instead of by a column position that means nothing on screen.
  //
  // Blockly 10.3.0 has no virtualised rendering: there is no way to hold 30,000
  // blocks and draw only the visible ones. The only lever is not putting them
  // on the canvas, which is what the region tabs below do.
  // ==========================================================================

  /* The file a block belongs to. Top level blocks carry it; a nested block
   * inherits it from whichever top level block it hangs under, so a search hit
   * deep inside a rule still knows where it lives. */
  function fileOfBlock(b) {
    try {
      var top = b;
      while (top && top.getParent && top.getParent()) { top = top.getParent(); }
      var raw = (top && top.data) || (b && b.data) || '';
      if (!raw) { return ''; }
      var o = JSON.parse(raw);
      return (o && o.file) || '';
    } catch (e) { return ''; }
  }

  /* A file path is too long for a tab. The last two segments keep it
   * recognisable without the noise: "src/systems/weapons.ts" -> "systems/weapons". */
  function shortFile(f) {
    if (!f) { return '(no file)'; }
    var p = String(f).replace(/\.[cm]?[jt]sx?$/, '').split('/');
    return p.length > 1 ? p.slice(-2).join('/') : p[0];
  }

  /* Every region on the canvas, in the order the columns are laid out, with the
   * blocks that belong to each. Rules live chained inside the mod block rather
   * than in a column, so they are their own region and always first. */
  function buildRegions() {
    var byFile = {}, order = [];
    function bucket(f) {
      if (!has(byFile, f)) { byFile[f] = { file: f, tops: [], blocks: 0 }; order.push(f); }
      return byFile[f];
    }
    if (!UI.ws) { return []; }
    UI.ws.getTopBlocks(true).forEach(function (top) {
      var f = (top.type === 'modBlock') ? ' rules' : (fileOfBlock(top) || '(no file)');
      var r = bucket(f);
      r.tops.push(top.id);
      try { r.blocks += top.getDescendants(false).length; } catch (e) {}
    });
    return order.map(function (f) {
      var r = byFile[f];
      r.label = (f === ' rules') ? 'RULES' : shortFile(f);
      return r;
    });
  }

  function has(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }

  /* A stable colour per region, used by the tabs and the minimap so the two
   * agree. Hue spread around the wheel; the site's own palette is for blocks,
   * not for this. */
  function regionColour(i, n) {
    return 'hsl(' + Math.round((i * 360) / Math.max(1, n)) + ',55%,55%)';
  }

  // ---- region focus: only one file's blocks on the canvas ------------------
  //
  // Built on the same stash-and-write-back the single rule focus mode uses, so
  // edits made inside a region go back into the whole workspace on the way out.

  /* SWITCHING FILES MUST NOT TOUCH THE WHOLE WORKSPACE.
   *
   * The first version called BF6.saveWorkspace to stash the canvas before
   * clearing it. On Undead Ground Zero that serialises 55,463 blocks to JSON
   * synchronously, every single time a bookmark is clicked, before any of the
   * batching below gets a chance to run. Batching the rebuild did nothing,
   * because the freeze was in the step before it.
   *
   * The document the project was loaded from is already in memory and does not
   * change unless the user edits something. So a region is built from THAT,
   * and the only thing ever serialised is the handful of top level items the
   * reader actually had open, written back when they leave.
   */
  /* PUT THE VARIABLE NAMES BACK. ONE PLACE, BECAUSE THREE PATHS NEED IT.
   *
   * clear() empties the variable map along with the canvas, and a block loaded
   * afterwards names its variable by id. Blockly does not refuse an id it has
   * never seen: it invents a variable with a generated single letter name. So
   * a workspace that loses its variables looks like it is full of blocks
   * called "Global Variable a".
   *
   * Blockly.serialization.variables.load does not exist in this build, though
   * the object around it does, so the obvious guard passes and the call throws.
   * createVariable is what the serializer calls anyway and it is always there.
   */
  function restoreVariables(list) {
    if (!list || !list.length || !UI.ws) { return 0; }
    var failed = 0, firstErr = '';
    for (var i = 0; i < list.length; i++) {
      var rec = list[i] || {};
      try {
        if (!UI.ws.getVariableById(rec.id)) {
          UI.ws.createVariable(rec.name, rec.type || '', rec.id);
        }
      } catch (e) {
        failed++;
        if (!firstErr) { firstErr = (e && e.message) ? e.message : String(e); }
      }
    }
    if (failed) {
      trace('variables: ' + failed + ' of ' + list.length +
            ' could not be restored, first: ' + firstErr);
    }
    return list.length - failed;
  }

  function docTops() {
    try {
      var st = UI.doc;
      return (st && st.blocks && st.blocks.blocks) || [];
    } catch (e) { return []; }
  }

  function fileOfDocTop(t) {
    if (!t) { return '(no file)'; }
    if (t.type === 'modBlock') { return ' rules'; }
    try {
      var o = t.data ? JSON.parse(t.data) : null;
      return (o && o.file) || '(no file)';
    } catch (e) { return '(no file)'; }
  }

  /* Whatever is on the canvas now goes back into the document, so an edit made
   * inside one file is not lost by opening another. Only the open items are
   * serialised, which is hundreds of blocks rather than tens of thousands. */
  function writeRegionBack() {
    if (!UI.region || !UI.doc) { return; }
    UI.doc = projectSnapshot();
    var live = canvasSnapshot();
    UI.region.topIds = ((live.blocks && live.blocks.blocks) || []).map(function(b){return b.id;});
  }

  function canvasSnapshot() {
    var live = BF6.saveWorkspace(UI.ws);
    return UI.ruleMode ? window.BF6ProjectState.mergeFocus(UI.ruleModeStash,live,UI.ruleMode.id) : live;
  }

  function projectSnapshot() {
    if (UI.loadFailed) throw new Error('This workspace did not finish loading. Reopen a complete workspace before saving or exporting.');
    if (UI.loadingDoc || UI.applying || BF6.state.loading) throw new Error('Wait for the workspace to finish opening before saving or exporting.');
    var live = canvasSnapshot();
    if (!UI.region || !UI.doc) return live;
    var ids = UI.region.topIds || docTops().filter(function(b){return fileOfDocTop(b)===UI.region.file;}).map(function(b){return b.id;});
    return window.BF6ProjectState.mergeRegion(UI.doc,live,ids,UI.region.file);
  }

  function projectDocument() {
    var doc = BF6.writeWorkspaceDoc(projectSnapshot());
    attachExtendedSidecar(doc);
    return doc;
  }

  function showRegion(file, label) {
    var wanted = docTops().filter(function (t) { return fileOfDocTop(t) === file; });
    if (!wanted.length) { status('nothing in that file', true); return; }
    if (UI.region) UI.region.topIds = wanted.map(function(b){return b.id;});

    UI.applying++;
    BF6.state.loading++;
    Blockly.Events.disable();
    try { UI.ws.clear(); } catch (e) {}
    /* THE VARIABLES GO BACK IN BEFORE ANY BLOCK DOES.
     *
     * clear() empties the variable map as well as the canvas, and a block
     * loaded afterwards names its variable by id. Blockly does not refuse an
     * id it has never seen: it invents a variable and gives it a generated
     * single letter name. So every bookmark click quietly replaced 254 real
     * names with a, b, c and the reader had no idea what anything was.
     *
     * The document still holds the real list, so it is put back first, every
     * time, before a single block is appended. */
    try {
      if (UI.doc && UI.doc.variables && UI.doc.variables.length) {
        restoreVariables(UI.doc.variables);
      }
    } catch (e) { UI.reportFault('restoring the variable names', e); }
    showLoadProgress(0, wanted.length, label);

    var ri = 0;
    function regionBatch() {
      var stop = Math.min(ri + 5, wanted.length);
      try {
        for (; ri < stop; ri++) {
          Blockly.serialization.blocks.append(wanted[ri], UI.ws);
        }
      } catch (e) {
        Blockly.Events.enable(); BF6.state.loading--; UI.applying--;
        hideLoadProgress();
        UI.reportFault('opening ' + label, e);
        return;
      }
      showLoadProgress(ri, wanted.length, label);
      if (ri < wanted.length) { setTimeout(regionBatch, 0); return; }

      try { refreshCollapsedText(); } catch (e) {}
      Blockly.Events.enable();
      BF6.state.loading--;
      UI.applying--;
      hideLoadProgress();

      BF6.snapshotUnits(UI.ws);
      UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));

      var n = 0;
      try { n = UI.ws.getAllBlocks(false).length; } catch (e) {}
      status(label + ': ' + wanted.length + ' item(s), ' + n + ' blocks.');
      refreshRegionTabs(); refreshNavigator(); drawMinimap();
      try { cullFarImages(); } catch (e) {}
      focusWorkspace(); checkGuards('a tab switch');
    }
    regionBatch();
  }

  function enterRegion(file) {
    if (UI.loadingDoc || UI.applying || BF6.state.loading) { status('Wait for the current tab to finish opening.', true); return; }
    if (!UI.ws || !UI.doc) { return; }
    if (UI.debounce) { clearTimeout(UI.debounce); flush(); }
    if (UI.ruleMode) { exitRuleMode(); }
    if (!UI.region) UI.doc = canvasSnapshot();
    writeRegionBack();
    UI.region = { file: file };
    showRegion(file, shortFile(file === ' rules' ? 'RULES' : file));
  }

  /* ALL FILES. Honest about the cost: this is the whole project on one canvas,
   * which is what the per file view exists to avoid. */
  function exitRegion() {
    if (UI.loadingDoc || UI.applying || BF6.state.loading) { status('Wait for the current tab to finish opening.', true); return; }
    if (!UI.ws || !UI.doc) { return; }
    if (UI.ruleMode) exitRuleMode();
    if (UI.debounce) { clearTimeout(UI.debounce); flush(); }
    writeRegionBack();
    UI.region = null;

    var tops = docTops();
    UI.applying++;
    BF6.state.loading++;
    Blockly.Events.disable();
    try { UI.ws.clear(); } catch (e) {}
    /* THE VARIABLES GO BACK IN BEFORE ANY BLOCK DOES.
     *
     * clear() empties the variable map as well as the canvas, and a block
     * loaded afterwards names its variable by id. Blockly does not refuse an
     * id it has never seen: it invents a variable and gives it a generated
     * single letter name. So every bookmark click quietly replaced 254 real
     * names with a, b, c and the reader had no idea what anything was.
     *
     * The document still holds the real list, so it is put back first, every
     * time, before a single block is appended. */
    try {
      if (UI.doc && UI.doc.variables && UI.doc.variables.length) {
        restoreVariables(UI.doc.variables);
      }
    } catch (e) { UI.reportFault('restoring the variable names', e); }
    showLoadProgress(0, tops.length, 'all files');

    var ai = 0;
    function allBatch() {
      var stop = Math.min(ai + 5, tops.length);
      try {
        for (; ai < stop; ai++) { Blockly.serialization.blocks.append(tops[ai], UI.ws); }
      } catch (e) {
        Blockly.Events.enable(); BF6.state.loading--; UI.applying--;
        hideLoadProgress(); UI.reportFault('opening all files', e); return;
      }
      showLoadProgress(ai, tops.length, 'all files');
      if (ai < tops.length) { setTimeout(allBatch, 0); return; }
      try { refreshCollapsedText(); } catch (e) {}
      Blockly.Events.enable(); BF6.state.loading--; UI.applying--;
      hideLoadProgress();
      status('all files');
      BF6.snapshotUnits(UI.ws);
      UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));
      refreshRegionTabs(); refreshNavigator(); drawMinimap();
      try { cullFarImages(); } catch (e) {}
    }
    allBatch();
  }

  /* The bookmarks live on the right edge against the helper rail, so the only
   * thing they have to keep out of the way of is the slide-out panel. When it
   * opens they step aside by its width; when it closes they come back. */
  function positionRegionTabs() {
    var host = $('regiontabs');
    if (!host) { return; }
    try {
      var side = $('side');
      var open = !!(side && side.classList.contains('open'));
      host.classList.toggle('pushed', open);
    } catch (e) {}
  }

  /* The tab strip. Rebuilt from the workspace rather than remembered, so it is
   * always what is actually there. */
  /* TABS ARE NOT ONLY FOR IMPORTED PROJECTS.
   *
   * A converted project gets its tabs from the source files it came from,
   * because that is an organisation its author already chose. Someone building
   * by hand has no such thing and ends up with one flat canvas that grows until
   * it is unreadable, which is the same problem for the same reason.
   *
   * The tab is only ever a name written on a block, so a person can write one
   * as easily as the converter can. It goes in `data`, which the serializer
   * round trips, so a workspace saved with tabs still has them when it comes
   * back, and Portal ignores the field entirely.
   */
  function assignTab(block, name) {
    if (!block) { return; }
    var top = block;
    while (top.getParent && top.getParent()) { top = top.getParent(); }
    var o = {};
    try { if (top.data) { o = JSON.parse(top.data) || {}; } } catch (e) { o = {}; }
    if (name) { o.file = name; } else { delete o.file; }
    try {
      top.data = Object.keys(o).length ? JSON.stringify(o) : null;
    } catch (e) { return; }
    status(name ? ('"' + (top.getFieldValue('SUBROUTINE_NAME') ||
                          top.getFieldValue('NAME') || top.type) +
                   '" is now in the ' + name + ' tab')
                : 'taken out of its tab');
    /* The document backs the tabs, so it has to learn about this too or the
     * next tab switch will put the block back where it was. */
    try {
      if (UI.doc && UI.doc.blocks && UI.doc.blocks.blocks) {
        var tops = UI.doc.blocks.blocks;
        for (var i = 0; i < tops.length; i++) {
          if (tops[i].id === top.id) { tops[i].data = top.data; break; }
        }
      }
    } catch (e) {}
    refreshRegionTabs(); refreshNavigator(); drawMinimap();
  }

  /* Every tab that exists right now, so a block can be filed into one that is
   * already there instead of the name being retyped and misspelt. */
  function existingTabNames() {
    var out = [], seen = {};
    buildRegions().forEach(function (r) {
      if (r.file === ' rules' || r.file === '(no file)') { return; }
      if (!seen[r.file]) { seen[r.file] = true; out.push(r.file); }
    });
    return out;
  }

  // PLAY THE SOUND THIS BLOCK IS SET TO.
  //
  // Offered only on a block that actually holds one, found by looking at its
  // field values rather than by knowing which block types take a sound - the
  // catalogue has hundreds and a hardcoded list would go stale the first time
  // the site added one.
  function blockSoundName(block) {
    if (!block || !block.inputList) { return ''; }
    for (var i = 0; i < block.inputList.length; i++) {
      var fields = block.inputList[i].fieldRow || [];
      for (var j = 0; j < fields.length; j++) {
        var v = '';
        try { v = fields[j].getValue(); } catch (e) { v = ''; }
        if (sfxLooksLikeSound(v)) { return v; }
      }
    }
    return '';
  }

  function installSoundPreview() {
    var reg = Blockly.ContextMenuRegistry && Blockly.ContextMenuRegistry.registry;
    if (!reg || UI.soundPreviewRegistered) { return; }
    UI.soundPreviewRegistered = true;
    var SCOPE = Blockly.ContextMenuRegistry.ScopeType.BLOCK;
    try {
      if (reg.getItem && reg.getItem('bf6_playsound')) { return; }
      reg.register({
        id: 'bf6_playsound', scopeType: SCOPE, weight: 149,
        displayText: function (scope) {
          var n = blockSoundName(scope.block);
          return n ? ('Play ' + n) : 'Play this sound';
        },
        preconditionFn: function (scope) {
          // Hidden rather than greyed out on a block with no sound in it: a
          // disabled item on every block in the workspace is just noise.
          return blockSoundName(scope.block) ? 'enabled' : 'hidden';
        },
        callback: function (scope) {
          var n = blockSoundName(scope.block);
          if (n) { sfxPlayName(n); }
        }
      });
      trace('tools: play-this-sound registered');
    } catch (e) {}
  }

  function installTabTools() {
    var reg = Blockly.ContextMenuRegistry && Blockly.ContextMenuRegistry.registry;
    if (!reg || UI.tabToolsRegistered) { return; }
    UI.tabToolsRegistered = true;
    var SCOPE = Blockly.ContextMenuRegistry.ScopeType.BLOCK;
    try {
      if (reg.getItem && reg.getItem('bf6_tab')) { return; }
      reg.register({
        id: 'bf6_tab', scopeType: SCOPE, weight: 150,
        displayText: 'Put in a tab...',
        preconditionFn: function () { return 'enabled'; },
        callback: function (scope) {
          var have = existingTabNames();
          var hint = have.length ? ('\n\nTabs you already have:\n  ' +
                                    have.slice(0, 12).join('\n  ')) : '';
          var name = window.prompt('Which tab should this go in?' + hint +
                                   '\n\nLeave blank to take it out of its tab.');
          if (name === null) { return; }
          assignTab(scope.block, name.trim());
        }
      });
      trace('tools: put-in-a-tab registered');
      installSoundPreview();
      send({ op: 'sfxsource' });
    } catch (e) {}
  }

  /* BLOCKLY'S OWN SHORTCUTS NEED THE WORKSPACE TO BE THE FOCUSED THING.
   *
   * Undo, redo, copy, paste and delete are registered by Blockly against the
   * focused workspace. Nothing in this editor ever focused it. The tab used to
   * be given focus from the C++ side, and that call was removed today because
   * it walked the Slate tree looking for a widget with two parents and hung the
   * whole editor.
   *
   * This is the page's own focus, not Slate's: markFocused tells Blockly which
   * workspace is live, and focusing the injection div makes the keystrokes
   * arrive. Neither touches the widget tree, so neither can bring that hang
   * back.
   */
  function focusWorkspace() {
    try {
      if (!UI.ws) { return; }
      if (UI.ws.markFocused) { UI.ws.markFocused(); }
      var div = UI.ws.getInjectionDiv && UI.ws.getInjectionDiv();
      if (div && div.focus) {
        if (!div.hasAttribute('tabindex')) { div.setAttribute('tabindex', '-1'); }
        div.focus({ preventScroll: true });
      }
    } catch (e) {}
  }

  /* AND SAY WHETHER THE LOAD LEFT ANYTHING SWITCHED OFF.
   *
   * Every bulk build here turns Blockly events off and the edit guards up, and
   * puts them back when it finishes. If a path ever returns without doing so,
   * events stay disabled for the rest of the session: undo stops recording,
   * edits stop being pushed to the site, and nothing announces it. That is the
   * kind of failure this session has spent hours on, so it reports itself. */
  function checkGuards(where) {
    try {
      var on = Blockly.Events.isEnabled ? Blockly.Events.isEnabled() : true;
      var applying = UI.applying || 0;
      var loading = (BF6.state && BF6.state.loading) || 0;
      if (!on || applying !== 0 || loading !== 0) {
        trace('guards after ' + where + ': events ' + (on ? 'on' : 'OFF') +
              ', applying ' + applying + ', loading ' + loading +
              '  <-- shortcuts and pushes are suppressed while this is not 0/0/on');
      }
    } catch (e) {}
  }

  function refreshRegionTabs() {
    var host = $('regiontabs');
    if (!host) { return; }
    var regions = UI.region ? (UI.allRegions || []) : buildRegions();
    if (!UI.region) { UI.allRegions = regions; }
    host.innerHTML = '';
    if (regions.length < 2) { host.style.display = 'none'; return; }
    host.style.display = '';

    var all = el('div', 'rtab' + (UI.region ? '' : ' on'), 'ALL FILES');
    all.style.borderLeftColor = '#8fa3b0';
    all.title = 'Everything at once. Slow on a large project.';
    all.onclick = function () { if (UI.region) { exitRegion(); } };
    host.appendChild(all);

    regions.forEach(function (r, i) {
      var t = el('div', 'rtab' + (UI.region && UI.region.file === r.file ? ' on' : ''),
                 r.label);
      t.title = (r.file === ' rules' ? 'The mod rules' : r.file) +
                '  -  ' + r.tops.length + ' item(s), ' + r.blocks + ' blocks';
      /* The spine colour, the same hue this file gets on the minimap, so a band
       * on the map and a bookmark on the edge read as the same thing. */
      t.style.borderLeftColor = regionColour(i, regions.length);
      t.onclick = function () { enterRegion(r.file); };
      host.appendChild(t);
    });
    positionRegionTabs();
  }

  // ---- follow the call ----------------------------------------------------
  //
  // A CALL block names the subroutine it calls. Jumping between the two is the
  // difference between reading a project and staring at it.

  function subroutineNamed(name) {
    var hit = null;
    UI.ws.getTopBlocks(false).forEach(function (t) {
      if (!hit && t.type === 'subroutineBlock' &&
          t.getFieldValue('SUBROUTINE_NAME') === name) { hit = t; }
    });
    return hit;
  }

  function callersOf(name) {
    var out = [];
    UI.ws.getAllBlocks(false).forEach(function (b) {
      if (b.type === 'subroutineInstanceBlock' &&
          b.getFieldValue('SUBROUTINE_NAME') === name) { out.push(b); }
    });
    return out;
  }

  /* Where the reader came from, so following a call is not a one way trip. */
  function pushBack(id) {
    UI.backStack = UI.backStack || [];
    UI.backStack.push(id);
    if (UI.backStack.length > 50) { UI.backStack.shift(); }
    refreshBreadcrumb();
  }

  function goBack() {
    if (!UI.backStack || !UI.backStack.length) { return; }
    var id = UI.backStack.pop();
    centreOn(id);
    refreshBreadcrumb();
  }

  function followCall(b) {
    if (!b) { return; }
    var name = b.getFieldValue('SUBROUTINE_NAME');
    if (!name) { return; }
    if (b.type === 'subroutineInstanceBlock') {
      var def = subroutineNamed(name);
      if (!def) {
        status('"' + name + '" is not on the canvas. Open ALL FILES, or its own file.', true);
        return;
      }
      pushBack(b.id);
      centreOn(def.id);
      status('went to ' + name);
    } else {
      var uses = callersOf(name);
      if (!uses.length) { status(name + ' is never called'); return; }
      UI.useAt = ((UI.useAt || 0) + 1) % uses.length;
      pushBack(b.id);
      centreOn(uses[UI.useAt].id);
      status('used by: ' + (UI.useAt + 1) + ' of ' + uses.length);
    }
  }

  function refreshBreadcrumb() {
    var el2 = $('breadcrumb');
    if (!el2) { return; }
    var n = (UI.backStack || []).length;
    el2.textContent = n ? ('BACK (' + n + ')') : '';
    el2.style.display = n ? '' : 'none';
  }

  // ==========================================================================
  // QUICK ANSWERS ABOUT THE BLOCK UNDER THE CURSOR
  //
  // A converted project is somebody else's code in a form they never wrote it
  // in. The three questions that come up constantly are "where did this come
  // from", "where is this set", and "what does this do", and the editor already
  // holds the answer to all three and never offered any of them:
  //
  //   provenance   every top level block carries its source file and line
  //   references   every SetVariable naming a variable is one pass away
  //   the FAQ      2,736 community answers, loaded but never surfaced
  // ==========================================================================

  /* Where a variable is written and where it is read. One pass, both answers,
   * because anything that walks 55,000 blocks should be asked once. */
  /* THE WHOLE PROJECT, NOT THE OPEN TAB.
   *
   * Walking the canvas answers only for the blocks currently on it, and with
   * file tabs that is one file out of thirty-one. "Set in 4, read in 37" would
   * silently mean "of the ones you happen to be looking at", which is worse
   * than refusing to answer, because it reads as a complete count.
   *
   * The document holds every block whether it is drawn or not, so the search
   * runs there and each hit remembers which file it is in. Jumping to one in
   * another file opens that tab first.
   */
  function variableUses(varId) {
    var writes = [], reads = [];
    if (!varId) { return { writes: writes, reads: reads }; }

    function scan(node, parent, socket, file) {
      if (!node || typeof node !== 'object') { return; }
      if (Array.isArray(node)) {
        node.forEach(function (n) { scan(n, parent, socket, file); });
        return;
      }
      if (node.data) {
        try { var o = JSON.parse(node.data); if (o && o.file) { file = o.file; } }
        catch (e) {}
      }
      if (node.type === 'variableReferenceBlock' && node.fields && node.fields.VAR) {
        var id = (node.fields.VAR && node.fields.VAR.id) || node.fields.VAR;
        if (id === varId) {
          /* Sitting in a Set block's first socket is the thing being written;
           * anywhere else it is being read. */
          var isWrite = !!(parent && /^Set/.test(parent.type || '') && socket === 'VALUE-0');
          (isWrite ? writes : reads).push({ id: node.id, file: file });
        }
      }
      if (node.inputs) {
        for (var k in node.inputs) {
          if (node.inputs[k] && node.inputs[k].block) {
            scan(node.inputs[k].block, node, k, file);
          }
        }
      }
      if (node.next && node.next.block) { scan(node.next.block, parent, socket, file); }
    }

    var tops = docTops();
    if (tops.length) { scan(tops, null, null, ''); }
    else if (UI.ws) {
      /* No imported document: a hand built workspace is entirely on the canvas,
       * so the canvas is the whole truth. */
      UI.ws.getAllBlocks(false).forEach(function (blk) {
        var f = blk.getField && blk.getField('VAR');
        var v = f && f.getVariable && f.getVariable();
        if (!v || v.getId() !== varId) { return; }
        var p = blk.getParent && blk.getParent();
        var isWrite = false;
        if (p && /^Set/.test(p.type || '')) {
          var first = p.getInput && p.getInput('VALUE-0');
          if (first && first.connection && first.connection.targetBlock() === blk) {
            isWrite = true;
          }
        }
        (isWrite ? writes : reads).push({ id: blk.id, file: fileOfBlock(blk) });
      });
    }
    return { writes: writes, reads: reads };
  }

  /* Go to a hit that may not be on the canvas, opening its file first. */
  function goToHit(hit) {
    if (!hit) { return; }
    if (UI.ws && UI.ws.getBlockById(hit.id)) { pushBack(hit.id); centreOn(hit.id); return; }
    if (hit.file) {
      status('that one is in ' + shortFile(hit.file) + ', opening it');
      enterRegion(hit.file);
      /* The tab loads in batches, so the jump waits for it to arrive. */
      var tries = 0;
      (function wait() {
        if (UI.ws && UI.ws.getBlockById(hit.id)) { pushBack(hit.id); centreOn(hit.id); return; }
        if (tries++ > 60) { status('could not reach that block', true); return; }
        setTimeout(wait, 100);
      }());
      return;
    }
    status('that block is not loaded', true);
  }
  /* Step through a list of block ids, remembering where we came from. */
  function walkHits(ids, label) {
    if (!ids || !ids.length) { status('no ' + label, true); return; }
    UI.walk = UI.walk || {};
    var key = label;
    UI.walk[key] = ((UI.walk[key] === undefined) ? -1 : UI.walk[key]) + 1;
    if (UI.walk[key] >= ids.length) { UI.walk[key] = 0; }
    goToHit(ids[UI.walk[key]]);
    status(label + ': ' + (UI.walk[key] + 1) + ' of ' + ids.length);
  }

  /* The file and line a block was converted from. */
  function sourceOf(b) {
    try {
      var top = b;
      while (top && top.getParent && top.getParent()) { top = top.getParent(); }
      var o = top && top.data ? JSON.parse(top.data) : null;
      return (o && o.file) ? o : null;
    } catch (e) { return null; }
  }

  /* Community answers that mention this block. The FAQ had never once loaded
   * before today, so these have been sitting unused for the life of the tool. */
  function faqFor(type) {
    var out = [], q = String(type || '').toLowerCase();
    if (!q) { return out; }
    (BF6.state.faq || []).forEach(function (e) {
      if (out.length >= 5) { return; }
      var hay = ((e.question || '') + ' ' + (e.answer || '')).toLowerCase();
      if (hay.indexOf(q) >= 0) { out.push(e); }
    });
    return out;
  }

  /* WHAT PORTAL WILL ALLOW, WHILE THERE IS STILL TIME TO CHANGE IT.
   *
   * Portal refuses a workspace past about 128 variables per scope, and nothing
   * said so until an upload failed. Undead Ground Zero needs 247 globals, which
   * is worth knowing before the deploy rather than after it. */
  /* CAN THIS WORKSPACE GO TO PORTAL AS BLOCKS.
   *
   * Variables are the only hard limit the Portal editor actually enforces:
   * 128 Global and 128 shared between Player and Team, read out of its own
   * bundle. maxDepth is enforced nowhere and there is no cap on blocks, rules
   * or subroutines, so a big workspace is fine and a variable-heavy one is not.
   *
   * Packing on the way out buys a lot of room, so the question is not "how
   * many variables are on the canvas" but "how many are left after packing",
   * and only packForPortal knows that. It reparses the workspace, so this is
   * called when somebody acts, not on every repaint.
   *
   * Returns null when the converter is not loaded, which is a reason to let
   * the send proceed rather than block it on our own missing script.
   */
  /* EXTENDED BLOCKS MUST NEVER LEAVE BY A NATIVE ROUTE.
   *
   * The extended language exists because Portal's native blocks have no locals,
   * so its blocks have no native equivalent by construction. Every native
   * outbound path - packing, Send to site, pushing to the page, any automatic
   * sync - would either drop them or reinterpret them into something that is
   * not the author's program. Guarding only the button the user pressed leaves
   * the other doors open, so every route calls this.
   *
   * Returns true when it is safe to continue.
   */
  /* THE EXTENDED DEFINITIONS MUST EXIST BEFORE ANYTHING IS DESERIALISED.
   *
   * Blockly builds a block by looking its type up in Blockly.Blocks at load
   * time. A saved project holding bf6x_ blocks that arrives before they are
   * registered does not fail loudly: the loader substitutes a placeholder and
   * DROPS the whole input subtree under it, which reads as a corrupted save.
   *
   * So this runs at boot, and again whenever the event table arrives, because
   * the event-typed blocks build their dropdowns from it. Re-installing is
   * cheap and idempotent; installing too late is not recoverable.
   *
   * Note the deliberate absence of {standalone: true}: that override supplies
   * the demo's offline Number/Text/Boolean blocks, and the native definitions
   * here are the real ones.
   */
  function installExtended(why) {
    var X = window.BF6ExtendedBlocks;
    if (!X || !X.install || typeof Blockly === 'undefined') { return false; }
    try {
      X.install(Blockly, { events: UI.convertEvents || null });
      UI.extendedInstalled = true;
      trace('extended block definitions installed (' + why + ')');
      return true;
    } catch (e) {
      UI.reportFault('installing the extended block definitions', e);
      return false;
    }
  }

  /* ======================================================================
   * SELF TEST
   *
   * Everything else in this file reports what the tool is doing. This asks
   * whether it is doing it correctly, from inside the page, where the DOM is.
   *
   * It exists because the expensive bugs of this project have not been logic
   * errors. They have been a panel drawn over the control it was meant to sit
   * beside, a button wired to nothing, a module that loaded but was never
   * installed. None of those throw. All of them are obvious the moment
   * somebody looks, and nobody looks at every control on every change.
   *
   * Each check logs one line: "selftest: PASS name" or "selftest: FAIL name -
   * why". The host reads them back out of the log, so this needs no return
   * channel and works with the panel open and nothing else running.
   * ====================================================================== */
  function selfTest() {
    var pass = 0, fail = 0, skipped = 0;
    /* A CHECK THAT DID NOT RUN IS NOT A CHECK THAT PASSED.
     *
     * Every skip went through ok(), so the completion line counted them as
     * passes: a run that could only look at three things out of twelve still
     * reported "12 passed, 0 failed", and a release note recorded that as full
     * coverage. Skips are counted and reported on their own now, so the number
     * that gets quoted is the number of checks that actually ran. */
    function ok(name) { pass++; trace('selftest: PASS ' + name); }
    function skip(name, why) { skipped++; trace('selftest: SKIP ' + name + ' - ' + why); }
    function bad(name, why) { fail++; trace('selftest: FAIL ' + name + ' - ' + why); }

    /* ---- is anything drawn on top of something you are meant to click ----
     *
     * The bug this is really for: the file tabs were drawn over the block
     * helper and then over the search box, twice, and both times it was found
     * by the user rather than by the tool. Two visible interactive things
     * occupying the same pixels is almost always wrong, and it is cheap to
     * check exhaustively in a way no human does.
     *
     * Deliberate covers are exempt: a popup, a flyout or a modal is SUPPOSED
     * to sit over the page, so anything inside one is skipped, as is anything
     * marked data-overlap-ok. */
    function visibleRects() {
      /* CONTROLS AND PANELS, NOT JUST CONTROLS.
       *
       * The first version selected buttons and inputs only, so opening the
       * toolbox flyout changed nothing it could see: the flyout is SVG, not
       * HTML, and it registered as zero new elements. The overlaps that have
       * actually shipped were a PANEL over a control - the tab strip over the
       * search box, the flyout over the tab strip - so the panels have to be
       * in the set or the interesting case is invisible to the check.
       */
      var sel = 'button, select, input, textarea, .pill, .tab, [role="button"], ' +
                '.blocklyFlyout, .blocklyToolboxDiv, #regiontabs, #gamelog, ' +
                '#slotpop, #navigator, #helper';
      var out = [], all = document.querySelectorAll(sel), i;
      for (i = 0; i < all.length; i++) {
        var el = all[i];
        if (el.closest('[data-overlap-ok], .popup, .flyout, .modal, #slotpop, #blockpreview')) { continue; }
        if (el.hidden || el.disabled) { continue; }
        var cs = window.getComputedStyle(el);
        if (cs.display === 'none' || cs.visibility === 'hidden' || parseFloat(cs.opacity) < 0.05) { continue; }
        var r = el.getBoundingClientRect();
        if (r.width < 4 || r.height < 4) { continue; }
        out.push({ el: el, r: r });
      }
      return out;
    }

    try {
      var items = visibleRects(), i, j, clashes = [];
      for (i = 0; i < items.length; i++) {
        for (j = i + 1; j < items.length; j++) {
          var a = items[i], b = items[j];
          if (a.el.contains(b.el) || b.el.contains(a.el)) { continue; }
          var ox = Math.min(a.r.right, b.r.right) - Math.max(a.r.left, b.r.left);
          var oy = Math.min(a.r.bottom, b.r.bottom) - Math.max(a.r.top, b.r.top);
          if (ox <= 2 || oy <= 2) { continue; }
          /* Overlapping by a hair is a rounding artefact; a quarter of the
           * smaller control is somebody's mistake. */
          var smaller = Math.min(a.r.width * a.r.height, b.r.width * b.r.height);
          if ((ox * oy) < smaller * 0.25) { continue; }
          clashes.push(name(a.el) + ' over ' + name(b.el));
        }
      }
      if (clashes.length) {
        bad('no overlapping controls', clashes.length + ': ' + clashes.slice(0, 6).join('; '));
      } else {
        ok('no overlapping controls (' + items.length + ' checked)');
      }
    } catch (e) { bad('no overlapping controls', 'the check itself threw: ' + (e.message || e)); }

    function name(el) {
      if (!el) { return '?'; }
      return (el.id ? '#' + el.id : '') +
             (el.className && typeof el.className === 'string'
               ? '.' + el.className.split(/\s+/)[0] : '') ||
             el.tagName.toLowerCase();
    }

    /* ---- every control does something ----
     *
     * A button with no handler looks identical to one that works until it is
     * pressed. "Show as blocks is not wired yet" sat in the script editor for
     * months exactly like this. */
    try {
      var dead = [], btns = document.querySelectorAll('button'), b2;
      for (b2 = 0; b2 < btns.length; b2++) {
        var bt = btns[b2];
        if (bt.hidden || bt.disabled) { continue; }
        if (bt.onclick || bt.getAttribute('onclick') || bt.form) { continue; }
        if (bt.dataset && bt.dataset.noHandler === 'ok') { continue; }
        dead.push(name(bt));
      }
      if (dead.length) { bad('every button is wired', dead.length + ' with no handler: ' + dead.slice(0, 8).join(', ')); }
      else { ok('every button is wired (' + btns.length + ' checked)'); }
    } catch (e) { bad('every button is wired', 'the check itself threw: ' + (e.message || e)); }

    /* ---- the pieces this page depends on ---- */
    try {
      var need = [
        ['Blockly', typeof Blockly !== 'undefined'],
        ['the converter', !!window.BF6Convert],
        ['the extended compiler', !!window.BF6Extended],
        ['the extended blocks', !!window.BF6ExtendedBlocks],
        ['the extended integration', !!window.BF6ExtendedIntegration],
        ['a workspace', !!UI.ws]
      ], k, missing = [];
      for (k = 0; k < need.length; k++) { if (!need[k][1]) { missing.push(need[k][0]); } }
      if (missing.length) { bad('every module loaded', 'missing ' + missing.join(', ')); }
      else { ok('every module loaded'); }
    } catch (e) { bad('every module loaded', 'the check itself threw: ' + (e.message || e)); }

    /* ---- the extended definitions are REGISTERED, not merely loaded ----
     * Loading the module and installing its blocks are two different things,
     * and the gap between them cost a session earlier today. */
    try {
      var n = 0, t;
      for (t in Blockly.Blocks) { if (t.indexOf('bf6x_') === 0) { n++; } }
      if (n > 0) { ok('extended blocks registered (' + n + ')'); }
      else { bad('extended blocks registered', 'no bf6x_ types in Blockly.Blocks'); }
    } catch (e) { bad('extended blocks registered', 'the check itself threw: ' + (e.message || e)); }

    /* ---- nothing left switched off by a bulk load ---- */
    try {
      var on = Blockly.Events.isEnabled ? Blockly.Events.isEnabled() : true;
      var applying = UI.applying || 0;
      var loading = (BF6.state && BF6.state.loading) || 0;
      if (on && !applying && !loading) { ok('edit guards balanced'); }
      else { bad('edit guards balanced', 'events ' + (on ? 'on' : 'OFF') +
                 ', applying ' + applying + ', loading ' + loading); }
    } catch (e) { bad('edit guards balanced', 'the check itself threw: ' + (e.message || e)); }

    /* ---- the canvas is actually on screen ----
     * A workspace injected into a zero sized div renders nothing and reports
     * no error, which reads exactly like a failed load. */
    try {
      var c = document.getElementById('canvas');
      var cr = c ? c.getBoundingClientRect() : null;
      if (cr && cr.width > 200 && cr.height > 200) { ok('canvas has size (' + Math.round(cr.width) + 'x' + Math.round(cr.height) + ')'); }
      else { bad('canvas has size', cr ? (Math.round(cr.width) + 'x' + Math.round(cr.height)) : 'no #canvas'); }
    } catch (e) { bad('canvas has size', 'the check itself threw: ' + (e.message || e)); }

    /* ---- THE PANELS THAT ACTUALLY COLLIDE ----
     *
     * The overlap pass above only sees what is on screen, and on a quiet page
     * that is a toolbar. Every overlap bug this project has shipped involved
     * something that is normally CLOSED: the toolbox flyout over the tab
     * strip, the tab strip over the search box, the slot popup over the
     * flyout. So they are opened, re-checked, and put back.
     *
     * Wrapped tightly: a check that leaves a panel open would hand the user a
     * changed editor, and a check that throws must not stop the rest. */
    /* A CHECK THAT DID NOT OPEN THE PANEL IS NOT A PASS.
     *
     * The first version of this reported "no overlap with the toolbox flyout
     * open (25 checked)" three times, with the same 25 as the closed baseline,
     * because none of the open functions actually opened anything. It was
     * green and it was testing nothing, which is worse than red: it says the
     * risky configuration is safe when it was never looked at.
     *
     * So the count is compared against the quiet baseline, and a panel that
     * did not change what is on screen is reported as UNTESTED rather than
     * passed. */
    var baseline = -1;
    try { baseline = visibleRects().length; } catch (e) {}

    function reCheckWith(label, open, close, hasContent) {
      /* EMPTY IS NOT THE SAME AS BROKEN.
       *
       * A tab strip with no files in it and a log panel with no log drawn
       * cannot overlap anything, because there is nothing there. Reporting
       * those as failures teaches people to ignore the red lines, and the red
       * lines are the entire point. They are reported as skipped, with the
       * reason, and stay distinct from a panel that was asked to open and
       * did not. */
      if (typeof hasContent === 'function') {
        var why = '';
        try { why = hasContent(); } catch (e0) { why = 'the content check threw: ' + (e0.message || e0); }
        if (why) { skip('no overlap with ' + label, why); return; }
      }
      /* ALREADY OPEN IS ALREADY TESTED.
       *
       * With a project loaded the tab strip is on screen before this runs, so
       * opening it changes nothing and the count does not move. That is not an
       * untested configuration: the baseline pass covered it. Distinguishing
       * the two matters, because "untested" is a real finding and crying it on
       * a panel that was already checked is how a suite gets ignored. */
      var wasVisible = false;
      try {
        var probe = visibleRects();
        wasVisible = probe.some(function (p) {
          return p.el.id && ('#' + p.el.id) === label.replace(/[^#\w-].*$/, '');
        });
      } catch (e1) {}

      try {
        open();
        var items = visibleRects(), i, j, clashes = [];
        if (baseline >= 0 && items.length <= baseline) {
          /* Did the thing we tried to open end up on screen anyway? If it is
           * there, the baseline already covered it. */
          var present = false;
          try {
            present = items.some(function (p) {
              return p.el && p.el.getBoundingClientRect().width > 4 &&
                     p.el.id && label.indexOf(p.el.id) >= 0;
            });
          } catch (e3) {}
          if (present || wasVisible) {
            ok('no overlap with ' + label + ' (already on screen, covered by the main pass)');
          } else {
            bad('no overlap with ' + label,
                'UNTESTED: opening it changed nothing on screen (' + items.length +
                ' controls, same as closed), so this configuration was never checked');
          }
          try { close(); } catch (e2) {}
          return;
        }
        for (i = 0; i < items.length; i++) {
          for (j = i + 1; j < items.length; j++) {
            var a = items[i], b = items[j];
            if (a.el.contains(b.el) || b.el.contains(a.el)) { continue; }
            var ox = Math.min(a.r.right, b.r.right) - Math.max(a.r.left, b.r.left);
            var oy = Math.min(a.r.bottom, b.r.bottom) - Math.max(a.r.top, b.r.top);
            if (ox <= 2 || oy <= 2) { continue; }
            var smaller = Math.min(a.r.width * a.r.height, b.r.width * b.r.height);
            if ((ox * oy) < smaller * 0.25) { continue; }
            clashes.push(name(a.el) + ' over ' + name(b.el));
          }
        }
        if (clashes.length) { bad('no overlap with ' + label, clashes.length + ': ' + clashes.slice(0, 5).join('; ')); }
        else { ok('no overlap with ' + label + ' (' + items.length + ' checked)'); }
      } catch (e) {
        bad('no overlap with ' + label, 'the check itself threw: ' + (e.message || e));
      }
      try { close(); } catch (e) {}
    }

    /* The flyout is the one that has actually collided with things twice, so
     * it is opened the way a click opens it: select a real category and let
     * Blockly build the flyout, rather than poking at internals that may or
     * may not draw anything. */
    reCheckWith('the toolbox flyout open',
      function () {
        var tb = UI.ws && UI.ws.getToolbox && UI.ws.getToolbox();
        if (!tb) { return; }
        var items = (tb.getToolboxItems && tb.getToolboxItems()) || [];
        var k;
        for (k = 0; k < items.length; k++) {
          /* A category with contents. The search bin draws nothing until
           * something is typed, so selecting it would prove nothing. */
          if (items[k] && items[k].isSelectable && items[k].isSelectable() &&
              items[k].getId && items[k].getId() !== SEARCH_BIN_ID) {
            tb.setSelectedItem(items[k]);
            break;
          }
        }
      },
      function () {
        try { var fo = UI.ws.getFlyout(); if (fo) { fo.hide(); } } catch (e) {}
        var tb = UI.ws && UI.ws.getToolbox && UI.ws.getToolbox();
        if (tb && tb.clearSelection) { tb.clearSelection(); }
      });

    reCheckWith('the file tabs shown #regiontabs',
      function () { var t = $('regiontabs'); if (t) { t.hidden = false; } },
      function () { /* left as found: refreshRegionTabs owns its own visibility */ },
      function () {
        var t = $('regiontabs');
        if (!t) { return 'there is no tab strip on this page'; }
        return t.children.length ? '' : 'no files are open, so the strip is empty';
      });

    reCheckWith('the game log panel open #gamelog',
      function () { var g = $('gamelog'); if (g) { g.hidden = false; } },
      function () { var g = $('gamelog'); if (g && !UI.logWatching) { g.hidden = true; } },
      function () {
        var g = $('gamelog');
        if (!g) { return 'there is no log panel on this page'; }
        return g.children.length ? '' : 'no log has been pulled, so the panel is empty';
      });

    /* ---- the converter still round trips ----
     *
     * The single most valuable thing this page can check about itself: take a
     * small program, convert it to blocks and back, and see that the rules and
     * subroutines survive. It is the whole tool in one assertion, and it runs
     * in a millisecond. */
    try {
      var C = window.BF6Convert;
      if (!C || !C.tsToBlocks || !C.blocksToTs) {
        bad('the converter round trips', 'the converter is not loaded');
      } else if (!C.setTypeScript || !UI.typescriptUrl || !window.ts) {
        /* Say WHICH of the three was missing. "Skipped" with no reason is how
         * a check quietly stops running and nobody notices for a month. */
        var missing = [];
        if (!C.setTypeScript) { missing.push('setTypeScript'); }
        if (!UI.typescriptUrl) { missing.push('typescriptUrl'); }
        if (!window.ts) { missing.push('window.ts'); }
        trace('selftest: note - the round trip needs ' + missing.join(', '));
        /* NO COMPILER IS A NORMAL STATE.
         *
         * TypeScript is loaded on demand, so on a page nobody has imported a
         * script into there is nothing to round trip WITH. That is not a
         * failure of the converter, and reporting it as one trains people to
         * ignore a red line. Saying it was skipped, and why, keeps the
         * distinction between "checked and fine" and "not checked". */
        skip('the converter round trips', 'no TypeScript compiler loaded yet');
      } else {
        var src = 'export function OnPlayerDeployed(eventPlayer: mod.Player) {\n' +
                  '    mod.SetGameModeTargetScore(1);\n}\n';
        var r = C.tsToBlocks({ 'index.ts': src }, {});
        var ws = r && (r.workspace || r.json);
        var back = ws && C.blocksToTs(ws, {});
        var text = back && Object.keys(back.files).map(function (f) { return back.files[f]; }).join('\n');
        if (text && text.indexOf('SetGameModeTargetScore') >= 0) { ok('the converter round trips'); }
        else { bad('the converter round trips', 'the call did not survive the trip'); }
      }
    } catch (e) { bad('the converter round trips', 'threw: ' + (e.message || e)); }

    /* ---- saving and reloading a workspace keeps it ----
     * A save that loses blocks is the worst failure this tool has, and it is
     * silent: the file writes, the count is wrong, nobody looks. */
    try {
      /* THE NUMBERS GO IN THE LINE, ALWAYS.
       *
       * This reported "empty canvas, nothing to lose" on a workspace holding
       * 39,943 blocks. The check was not wrong about what it measured; it was
       * measuring the wrong thing and had no way to show that. A check that
       * prints what it saw cannot quietly pass on a number nobody expected. */
      var onCanvas = UI.ws.getAllBlocks(false).length;
      var topLevel = UI.ws.getTopBlocks(false).length;
      var doc = BF6.writeWorkspaceDoc(BF6.saveWorkspace(UI.ws));
      var seen = (JSON.stringify(doc).match(/"type":/g) || []).length;
      var where = onCanvas + ' on canvas, ' + topLevel + ' top level, ' + seen + ' in the document';
      if (onCanvas === 0 && seen === 0) { skip('workspace save', 'nothing is loaded, so there is nothing to round trip: ' + where); }
      else if (seen >= onCanvas && seen > 0) { ok('workspace save keeps every block (' + where + ')'); }
      else { bad('workspace save keeps every block', where); }
    } catch (e) { bad('workspace save keeps every block', 'threw: ' + (e.message || e)); }

    /* ---- the look actually arrived ----
     * The difference between "the editor opened" and "the editor looks like
     * Portal", which is exactly what a first run without a capture used to
     * get wrong. */
    try {
      var st = (BF6.state && BF6.state.style) || null;
      var nConst = st && st.constants ? Object.keys(st.constants).length : 0;
      var nTypes = 0, tkey;
      try { for (tkey in Blockly.Blocks) { nTypes++; } } catch (e2) {}
      if (nConst >= 50 && nTypes > 300) { ok('site look and definitions present (' + nConst + ' constants, ' + nTypes + ' types)'); }
      else { bad('site look and definitions present', nConst + ' style constants, ' + nTypes + ' block types'); }
    } catch (e) { bad('site look and definitions present', 'threw: ' + (e.message || e)); }

    trace('selftest: DONE ' + pass + ' passed, ' + fail + ' failed, ' + skipped + ' skipped');
  }

  function nativeRouteAllowed(what) {
    var I = window.BF6ExtendedIntegration;
    if (!I || !I.requireNative || !I.snapshot) { return true; }
    try {
      I.requireNative(projectDocument());
      return true;
    } catch (e) {
      status((e && e.message) || 'This project cannot go out as native blocks.', true);
      var b = $('btn-exportscript');
      if (b) {
        b.classList.add('urgent');
        setTimeout(function () { b.classList.remove('urgent'); }, 6000);
      }
      trace('blocked a native route (' + what + '): ' + ((e && e.message) || e));
      return false;
    }
  }

  function portalReadiness() {
    var Convert = window.BF6Convert;
    if (!Convert || !Convert.packForPortal) { return null; }
    var doc;
    try { doc = projectDocument(); }
    catch (e) { return null; }
    try { return Convert.packForPortal(doc, {}); }
    catch (e) { return null; }
  }

  /* THE CHOICE, MADE HONESTLY.
   *
   * Under the limit, blocks are the better answer: the workspace goes up as
   * blocks and stays editable on the site. Over it, Portal will refuse the
   * upload no matter how it is dressed up, and the only route that still ships
   * the mode is TypeScript. Saying that plainly beats sending something that
   * comes back rejected.
   */
  function offerTypeScriptInstead(state) {
    var over = state.after.Global - state.ceiling;
    var objOver = state.after.Object - state.ceiling;
    var parts = [];
    if (over > 0) { parts.push('Global ' + state.after.Global + ' of ' + state.ceiling); }
    if (objOver > 0) {
      parts.push('Player and Team ' + state.after.Object + ' of ' + state.ceiling);
    }
    status('Too big for blocks on Portal: ' + parts.join(', ') +
           ' after packing. Portal would refuse this upload. Use ' +
           '"Export for Portal" to build one uploadable TypeScript script instead.', true);

    /* Point at the button that does it rather than describing it. */
    var b = $('btn-exportscript');
    if (b) {
      b.classList.add('urgent');
      try { b.scrollIntoView({ block: 'nearest' }); } catch (e) {}
      setTimeout(function () { b.classList.remove('urgent'); }, 6000);
    }
    if (state.blockers && state.blockers.length) {
      trace('cannot be folded (' + state.blockers.length + '): ' +
            state.blockers.slice(0, 20).join(', '));
    }
  }

  /* The verdict, worked out once and remembered until the workspace changes
   * shape, so clicking the pill is cheap and the automatic check on a large
   * project does not run on every repaint. */
  function showBudgetVerdict(pill) {
    var stamp = 0;
    try { stamp = UI.ws.getAllBlocks(false).length + UI.ws.getAllVariables().length; }
    catch (e) {}
    if (UI.verdictStamp === stamp && UI.verdict !== undefined) {
      paintVerdict(pill, UI.verdict);
      return;
    }
    var state = portalReadiness();
    UI.verdictStamp = stamp;
    UI.verdict = state;
    paintVerdict(pill, state);
  }

  function paintVerdict(pill, state) {
    if (!state) { return; }
    var head = state.fits
      ? 'Fits Portal as blocks after packing: Global ' + state.after.Global +
        ', Player and Team ' + state.after.Object + ', both within ' + state.ceiling + '.'
      : 'Will NOT upload as blocks: Global ' + state.after.Global + ' and Player/Team ' +
        state.after.Object + ' against a limit of ' + state.ceiling +
        ', even after packing. This one has to go out as TypeScript.';
    pill.title = head + '  The canvas keeps every readable name; Send to site packs a copy.' +
      (state.fits ? '' : '  Cannot be folded: ' +
        (state.blockers || []).slice(0, 12).join(', ') + '.');
    pill.classList.toggle('warnpill', !state.fits);
    pill.classList.toggle('okpill', !!state.fits);
  }

  function refreshBudget() {
    var pill = $('budget');
    if (!pill || !UI.ws) { return; }
    var byScope = {}, worst = 0, worstName = '';
    try {
      UI.ws.getAllVariables().forEach(function (v) {
        var sc = v.type || 'Global';
        byScope[sc] = (byScope[sc] || 0) + 1;
        if (byScope[sc] > worst) { worst = byScope[sc]; worstName = sc; }
      });
    } catch (e) { return; }
    var blocks = 0;
    try { blocks = UI.ws.getAllBlocks(false).length; } catch (e) {}
    /* PLAYER AND TEAM ARE ONE BUDGET, NOT TWO.
     *
     * Read out of the site bundle: getObjectVariableCount filters
     * type !== "Global" as a single aggregate, so capacity is Global 128 plus
     * Object 128, and counting the three scopes separately understated the
     * pressure on a project that used both Player and Team. */
    var globalN = byScope.Global || 0;
    var objectN = (byScope.Player || 0) + (byScope.Team || 0);
    worst = Math.max(globalN, objectN);
    worstName = globalN >= objectN ? 'Global' : 'Player and Team';

    pill.textContent = worstName + ' ' + worst + '/128   ' + blocks + ' blocks';
    pill.classList.toggle('warnpill', worst > 128);

    /* ASK PROPERLY WHEN IT LOOKS CLOSE.
     *
     * The count on the canvas is not the count Portal sees, because the send
     * packs first. A pill reading 648/128 on a project that packs to 120 would
     * be alarming and wrong, and one reading 130/128 on a project that packs
     * to 400 would be reassuring and wrong. So once the raw count is over, the
     * real answer is worked out and the pill reports THAT.
     *
     * Only when it is over: packForPortal reparses the workspace, which is far
     * too expensive for a repaint that also runs on every drag. */
    pill.onclick = function () { showBudgetVerdict(pill); };
    pill.style.cursor = 'pointer';
    if (worst > 128) { showBudgetVerdict(pill); }

    /* AND THIS COUNT IS NOT WHAT PORTAL WILL SEE.
     *
     * The canvas deliberately keeps every real name so the project can be
     * read; the copy sent to the site is packed on the way out. A pill
     * reading 648/128 looks like a project that cannot ship when most of that
     * folds away on send, so the tooltip says which number is which. */
    pill.title = Object.keys(byScope).map(function (k) {
      return k + ' ' + byScope[k];
    }).join(', ') +
      '  -  Portal allows 128 Global and 128 shared between Player and Team.' +
      '  These are the readable names on the canvas; Send to site packs them' +
      ' down first and reports what it could not fold.';
  }

  /* THE ACTIONS LIVE ON THE RIGHT CLICK, WHERE BLOCKLY ALREADY PUTS ACTIONS.
   *
   * Adding buttons to the top bar for these would mean the user has to select
   * a block and then travel to a toolbar; the context menu is already about
   * the thing under the cursor, which is exactly what every one of these
   * answers. Registered once, guarded so a second registration is harmless
   * when the page reloads its scripts. */
  function installBlockTools() {
    var reg = Blockly.ContextMenuRegistry && Blockly.ContextMenuRegistry.registry;
    if (!reg || UI.toolsRegistered) { return; }
    UI.toolsRegistered = true;
    var SCOPE = Blockly.ContextMenuRegistry.ScopeType.BLOCK;

    function add(id, text, precondition, callback, weight) {
      try {
        if (reg.getItem && reg.getItem(id)) { return; }
        reg.register({
          id: id, scopeType: SCOPE, weight: weight || 200,
          displayText: text,
          preconditionFn: function (scope) {
            try { return precondition(scope.block) ? 'enabled' : 'hidden'; }
            catch (e) { return 'hidden'; }
          },
          callback: function (scope) {
            try { callback(scope.block); } catch (e) { UI.reportFault(text, e); }
          }
        });
      } catch (e) {}
    }

    /* WHERE DID THIS COME FROM. The provenance the converter now writes onto
     * every top level block, finally asked for. */
    add('bf6_source', function (scope) {
      var o = sourceOf(scope.block);
      return o ? ('Came from ' + o.file + ':' + (o.line || '?')) : '';
    }, function (b) { return !!sourceOf(b); }, function (b) {
      var o = sourceOf(b);
      status('This block was converted from ' + o.file + ' line ' + (o.line || '?'));
      send({ op: 'openSource', path: o.file, line: o.line || 0 });
    }, 100);

    /* WHERE IS THIS SET. The question a reader asks of every variable. */
    /* A VARIABLE'S NAME IS A PROPERTY HERE, NOT A METHOD.
     *
     * Both of the cross reference items threw "v.getName is not a function" on
     * every click. Blockly's VariableModel in this build carries getId() as a
     * method but the name as a plain .name field; getName() arrived in a later
     * version. Reading it one way worked in whatever I checked against and
     * failed in the editor, so it is read both ways here and nowhere else. */
    function varName(v) {
      if (!v) { return '(unnamed)'; }
      if (typeof v.getName === 'function') { return v.getName(); }
      return v.name || '(unnamed)';
    }

    add('bf6_assigned', 'Where is this set?', function (b) {
      return b.type === 'variableReferenceBlock';
    }, function (b) {
      var f = b.getField('VAR');
      var v = f && f.getVariable && f.getVariable();
      if (!v) { return; }
      var u = variableUses(v.getId());
      /* Where they live matters as much as how many: on a converted project
       * most of them are in files that are not currently open. */
      var files = {};
      u.writes.concat(u.reads).forEach(function (h) {
        var f = h.file || '(this tab)';
        files[f] = (files[f] || 0) + 1;
      });
      var where = Object.keys(files).sort(function (x, y) { return files[y] - files[x]; })
        .slice(0, 3).map(function (f) { return files[f] + ' in ' + shortFile(f); }).join(', ');
      status(varName(v) + ': set in ' + u.writes.length + ', read in ' + u.reads.length +
             (where ? '  (' + where + ')' : ''));
      walkHits(u.writes, 'set ' + varName(v));
    }, 110);

    add('bf6_read', 'Where is this used?', function (b) {
      return b.type === 'variableReferenceBlock';
    }, function (b) {
      var f = b.getField('VAR');
      var v = f && f.getVariable && f.getVariable();
      if (!v) { return; }
      walkHits(variableUses(v.getId()).reads, 'read ' + varName(v));
    }, 111);

    /* WHAT DOES THIS DO. The community answers, filtered to this block. */
    add('bf6_faq', 'What do people ask about this?', function (b) {
      return faqFor(b.type).length > 0;
    }, function (b) {
      var hits = faqFor(b.type);
      showFaqFor(b.type, hits);
    }, 120);

    /* EXTRACT TO SUBROUTINE. A 10,198 block rule is not readable; the only way
     * out is to name pieces of it. The blocks move into a new subroutine and a
     * call takes their place. */
    add('bf6_extract', 'Move these into a new subroutine', function (b) {
      return !!(b.previousConnection || b.nextConnection) &&
             b.type !== 'subroutineBlock' && b.type !== 'modBlock';
    }, function (b) { extractToSubroutine(b); }, 130);

    /* SAVE AS SNIPPET. There is a snippet loader and nothing that writes one. */
    add('bf6_snippet', 'Save these blocks as a snippet', function () { return true; },
      function (b) {
        var name = window.prompt('Name this snippet');
        if (!name) { return; }
        try {
          send({ op: 'saveSnippet', name: name, json: BF6.saveUnit(b) });
          status('snippet "' + name + '" sent to the tool');
        } catch (e) { UI.reportFault('saving a snippet', e); }
      }, 140);

    trace('tools: block actions registered');
  }

  /* Everything from this block down becomes a subroutine, and a call to it is
   * left behind in its place. */
  function extractToSubroutine(b) {
    var name = window.prompt('Name the new subroutine');
    if (!name) { return; }
    UI.applying++;
    try {
      BF6.state.loading++;
      var moved = BF6.saveUnit(b);
      var sub = Blockly.serialization.blocks.append({
        type: 'subroutineBlock',
        fields: { SUBROUTINE_NAME: name },
        extraState: { subroutineName: name, parameters: [] }
      }, UI.ws);
      var body = Blockly.serialization.blocks.append(moved, UI.ws);
      var slot = sub.getInput('ACTIONS');
      if (slot && slot.connection && body.previousConnection) {
        slot.connection.connect(body.previousConnection);
      }
      var callBlk = Blockly.serialization.blocks.append({
        type: 'subroutineInstanceBlock',
        fields: { SUBROUTINE_NAME: name },
        extraState: { subroutineName: name, parameters: [] }
      }, UI.ws);
      var prev = b.previousConnection && b.previousConnection.targetConnection;
      b.dispose(false);
      if (prev && callBlk.previousConnection) { prev.connect(callBlk.previousConnection); }
      var xy = UI.ws.getMetricsManager().getViewMetrics(true);
      sub.moveBy(xy.left + 60, xy.top + 60);
      BF6.state.loading--;
      status('made subroutine "' + name + '" and left a call in its place');
    } catch (e) {
      BF6.state.loading--;
      UI.reportFault('moving blocks into a subroutine', e);
    } finally { UI.applying--; }
    after();
  }

  /* The answers, in the side panel that already exists for reading things. */
  function showFaqFor(type, hits) {
    var list = $('navlist');
    if (!list) { status(hits.length + ' answer(s) mention ' + type); return; }
    UI.navTab = 'faq';
    list.innerHTML = '';
    list.appendChild(el('div', 'nav-group', hits.length + ' answer(s) about ' + type));
    hits.forEach(function (e) {
      var row = el('div', 'nav-row');
      row.appendChild(el('div', 'nav-name', e.question || '(question)'));
      row.appendChild(el('div', 'nav-sub', BF6.stripTags(e.answer || '').slice(0, 240)));
      list.appendChild(row);
    });
    status(hits.length + ' answer(s) about ' + type + ' in the panel');
  }

  function refreshNavigator() {
    if (!UI.ws) return;
    refreshRegionTabs(); refreshBreadcrumb(); refreshBudget();
    var panel = $('pane-rules');
    if (panel && !panel.classList.contains('on')) return;
    var rules = [], subs = [], orphans = [];
    var counts = {};
    UI.ws.getTopBlocks(false).forEach(function (top) {
      if (top.type === 'modBlock') {
        var inp = top.getInput('RULES');
        var b = inp && inp.connection && inp.connection.targetBlock();
        while (b) {
          if (b.type === 'ruleBlock') {
            var nm = b.getFieldValue('NAME') || '(unnamed)';
            counts[nm] = (counts[nm] || 0) + 1;
            rules.push({ id: b.id, name: nm, event: b.getFieldValue('EVENTTYPE') || '',
              object: b.getFieldValue('OBJECTTYPE') || '' });
          }
          b = b.getNextBlock();
        }
      } else if (top.type === 'subroutineBlock') {
        subs.push({ id: top.id, name: top.getFieldValue('SUBROUTINE_NAME') || '(unnamed)',
                    file: fileOfBlock(top) || '(no file)',
                    size: (function () { try { return top.getDescendants(false).length - 1; } catch (e) { return 0; } }()) });
      } else {
        orphans.push({ id: top.id, name: top.type });
      }
    });
    var used = {};
    UI.ws.getAllBlocks(false).forEach(function (b) {
      if (b.type === 'subroutineInstanceBlock') {
        used[b.getFieldValue('SUBROUTINE_NAME')] = (used[b.getFieldValue('SUBROUTINE_NAME')] || 0) + 1;
      }
    });
    var varUse = {};
    UI.ws.getAllBlocks(false).forEach(function (b) {
      if (b.type !== 'variableReferenceBlock') return;
      var f = b.getField('VAR');
      var v = f && f.getVariable && f.getVariable();
      if (v) varUse[v.getId()] = (varUse[v.getId()] || 0) + 1;
    });

    var list = $('navlist');
    if (!list) return;
    list.innerHTML = '';
    var tab = UI.navTab || 'rules';
    if (tab === 'rules') {
      rules.forEach(function (r) {
        var row = el('div', 'nav-row');
        var line = el('div', 'nav-name', r.name);
        if (counts[r.name] > 1) { line.appendChild(el('span', 'warn', ' DUPLICATE NAME')); }
        if (UI.highlight[r.id]) row.classList.add('lit');
        row.appendChild(line);
        row.appendChild(el('div', 'nav-sub', r.event + (r.object ? '  ' + r.object : '')));
        row.onclick = function () { centreOn(r.id); };
        list.appendChild(row);
      });
    } else if (tab === 'subs') {
      /* GROUPED BY THE FILE THEY CAME FROM.
       *
       * A flat list of 427 subroutines is a wall. The same 427 under 29 file
       * headings is a table of contents, and the heading is a name the reader
       * already knows from their own source. Sorted biggest file first, because
       * that is usually where the work is.
       */
      var groups = {}, gorder = [];
      subs.forEach(function (s) {
        var f = s.file || '(no file)';
        if (!has(groups, f)) { groups[f] = []; gorder.push(f); }
        groups[f].push(s);
      });
      gorder.sort(function (a, b) { return groups[b].length - groups[a].length; });

      gorder.forEach(function (f) {
        var items = groups[f];
        var head = el('div', 'nav-group');
        head.textContent = shortFile(f === '(no file)' ? '' : f) + '   ' + items.length;
        head.title = f + ' - click to show only this file on the canvas';
        head.style.cursor = 'pointer';
        head.onclick = function () { enterRegion(f); };
        list.appendChild(head);

        items.forEach(function (s) {
          var row = el('div', 'nav-row');
          var line = el('div', 'nav-name', s.name);
          if (!used[s.name]) line.appendChild(el('span', 'warn', ' UNUSED'));
          row.appendChild(line);
          row.appendChild(el('div', 'nav-sub',
            (used[s.name] || 0) + ' call' + (used[s.name] === 1 ? '' : 's') +
            (s.size ? '  -  ' + s.size + ' blocks' : '')));
          row.onclick = function () { centreOn(s.id); };
          list.appendChild(row);
        });
      });
    } else if (tab === 'vars') {
      UI.ws.getAllVariables().forEach(function (v) {
        var row = el('div', 'nav-row');
        row.appendChild(el('div', 'nav-name', v.name));
        row.appendChild(el('div', 'nav-sub', (v.type || 'Global') + '   ' + (varUse[v.getId()] || 0) + ' use' +
          ((varUse[v.getId()] || 0) === 1 ? '' : 's')));
        row.onclick = function () {
          var hit = UI.ws.getAllBlocks(false).filter(function (b) {
            if (b.type !== 'variableReferenceBlock') return false;
            var f = b.getField('VAR');
            var vv = f && f.getVariable && f.getVariable();
            return vv && vv.getId() === v.getId();
          });
          if (hit.length) centreOn(hit[0].id);
        };
        list.appendChild(row);
      });
    } else if (tab === 'orphans') {
      orphans.forEach(function (o) {
        var row = el('div', 'nav-row');
        row.appendChild(el('div', 'nav-name', o.name));
        row.appendChild(el('div', 'nav-sub', 'free floating'));
        row.onclick = function () { centreOn(o.id); };
        list.appendChild(row);
      });
    }
    var head = $('navcount');
    if (head) head.textContent = rules.length + ' RULES   ' + subs.length + ' SUBROUTINES   ' +
      UI.ws.getAllVariables().length + ' VARIABLES   ' + orphans.length + ' LOOSE';
    UI.rules = rules; UI.subs = subs; UI.orphans = orphans;
  }

  function centreOn(id) {
    var b = UI.ws.getBlockById(id);
    if (!b) return;
    UI.ws.centerOnBlock(id);
    UI.ws.getAllBlocks(false).forEach(function (x) { try { x.setHighlighted && x.setHighlighted(false); } catch (e) {} });
    try { b.select(); b.setHighlighted && b.setHighlighted(true); } catch (e) {}
    drawMinimap();
  }

  // ---- what things mean ---------------------------------------------------
  //
  // READ FROM THE STYLE, NEVER TYPED OUT. Every swatch is the colour the
  // renderer will actually paint, every symbol is the file it will actually
  // draw, and the example is a real block built by the real renderer. A legend
  // written by hand is one that goes quietly wrong the first time the site
  // moves, and this editor exists to match the site.
  var LEGEND_PARTS = [
    ['Head badge', 'The word in the block own colour at the top left - RULE, SUBROUTINE, MOD. It says what kind of thing this is.'],
    ['Name', 'A box you can type in. Only the white boxes are editable; a plain word is a label.'],
    ['Dropdown', 'A word with no box that opens a list when clicked. It only ever offers values that are allowed there.'],
    ['Socket', 'The dark rounded hole. Something goes in it, and the symbol inside says what kind of thing fits.'],
    ['Socket symbol', 'One small picture per type the socket accepts. Two symbols means it takes either.'],
    ['Statement row', 'A C shaped mouth, like CONDITIONS and ACTIONS. Blocks stack inside it, top to bottom, and run in that order.'],
    ['Notch', 'The bump and dent top and bottom. Blocks that stack have them; a block that returns a value does not.'],
    ['Comment', 'A dark circle with a question mark. Click it to read the note somebody left on the block.'],
    ['Warning', 'A badge just off the right edge. This one belongs to the tool, not to Portal, and marks a block pointing at an object that is not in this level.'],
    ['Gear', 'Opens a small builder for blocks that change shape, like adding an Else If to an If.'],
    ['Jagged edge', 'A torn looking right edge means the block is folded up. Double click it to open it again.']
  ];

  function legendRow(cls, swatchEl, name, what) {
    var row = el('div', cls);
    if (swatchEl) row.appendChild(swatchEl);
    row.appendChild(el('div', 'lgName', name));
    row.appendChild(el('div', 'lgWhat', what));
    return row;
  }

  // ONE REAL BLOCK, DRAWN BY THE REAL RENDERER.
  //
  // A legend made of words describes blocks; a legend made of BLOCKS is the
  // thing itself. Each row injects a tiny read only workspace with the site
  // renderer and the site theme, builds one actual block in it and shrinks to
  // fit. So every colour, every corner, every socket and every symbol in here
  // is what the canvas will draw, and none of it can drift.
  //
  // Read only, no toolbox, no scrollbars, no zoom: it is a picture you cannot
  // knock out of shape by clicking it.
  function legendBlock(host, type, build) {
    var ws = null;
    try {
      ws = Blockly.inject(host, {
        readOnly: true, trashcan: false, scrollbars: false, sounds: false,
        renderer: (UI.ws && UI.ws.options && UI.ws.options.renderer) || undefined,
        theme: (UI.ws && UI.ws.getTheme && UI.ws.getTheme()) || undefined,
        zoom: { startScale: 0.85 }
      });
    } catch (e) { return null; }
    if (!ws) return null;
    var b = null;
    try {
      b = ws.newBlock(type);
      if (build) { build(b, ws); }
      b.initSvg();
      b.render();
      b.moveBy(6, 6);
    } catch (e) {
      try { ws.dispose(); } catch (e2) {}
      host.textContent = 'cannot draw ' + type;
      return null;
    }
    /* Shrink the frame to the block, so rows are not mostly empty canvas. */
    try {
      var hw = b.getHeightWidth();
      host.style.height = Math.max(34, Math.round(hw.height * 0.85) + 14) + 'px';
      host.style.width = Math.min(430, Math.round(hw.width * 0.85) + 18) + 'px';
      Blockly.svgResize(ws);
    } catch (e) {}
    UI.legendWs = UI.legendWs || [];
    UI.legendWs.push(ws);
    return ws;
  }

  function legendShow(type, what, build) {
    var wrap = el('div', 'lgShow');
    var host = el('div', 'lgCanvas');
    wrap.appendChild(host);
    wrap.appendChild(el('div', 'lgWhat', what));
    return { wrap: wrap, host: host, type: type, build: build };
  }

  /* The families, each shown with a block that really belongs to it. Types
   * that are missing are skipped rather than drawn wrong. */
  var LEGEND_FAMILIES = [
    ['ruleBlock', 'A RULE. Something that happens in the game, and what to do when it does. Everything starts here.'],
    ['subroutineBlock', 'A SUBROUTINE. A named set of actions you can run from any rule, so you write it once.'],
    ['If', 'CONTROL. Chooses what runs. If, While and the loops are all this colour.'],
    ['SetVariable', 'An ACTION. Does something. Actions stack top to bottom and run in that order.'],
    ['GetVariable', 'A VALUE. Produces something to plug into a socket. Notice it has no notches.'],
    ['Number', 'A NUMBER you type in.'],
    ['Text', 'A piece of TEXT you type in.'],
    ['Boolean', 'TRUE or FALSE. Only fits a socket that asks for a yes or no.']
  ];

  function buildLegend() {
    var box = $('legendBody');
    if (!box) return 0;
    /* Panels are rebuilt on every open, and a Blockly workspace left behind
     * keeps its listeners and its svg. Dispose the old ones first. */
    (UI.legendWs || []).forEach(function (w) { try { w.dispose(); } catch (e) {} });
    UI.legendWs = [];
    box.innerHTML = '';
    var pending = [], n = 0;

    box.appendChild(el('div', 'lgHead', 'The kinds of block'));
    box.appendChild(el('div', 'lgWhat',
      'Colour tells you what a block is for. These are real blocks, drawn the same way the canvas draws them.'));
    LEGEND_FAMILIES.forEach(function (f) {
      if (!Blockly.Blocks[f[0]]) return;
      var row = legendShow(f[0], f[1], null);
      box.appendChild(row.wrap);
      pending.push(row);
      n++;
    });

    box.appendChild(el('div', 'lgHead', 'What fits a socket'));
    box.appendChild(el('div', 'lgWhat',
      'An empty socket draws one small symbol for every kind of thing it accepts. Match the symbol and it will fit.'));
    var st = (BF6.style && BF6.style()) || null;
    var icons = (st && st.icons) || {};
    Object.keys(icons).filter(function (k) { return k.indexOf('type-') === 0; }).sort()
      .forEach(function (k) {
        var name = k.slice(5);
        var url = BF6.typeIconUrl ? BF6.typeIconUrl(name) : null;
        if (!url) return;
        var row = el('div', 'lgRow');
        var img = el('img', 'lgSym');
        img.src = url; img.alt = '';
        row.appendChild(img);
        row.appendChild(el('div', 'lgName', prettyTypeName(name)));
        row.appendChild(el('div', 'lgWhat', 'Goes in a socket that asks for ' + prettyTypeName(name) + '.'));
        box.appendChild(row);
        n++;
      });

    box.appendChild(el('div', 'lgHead', 'The parts of a block'));
    box.appendChild(el('div', 'lgWhat',
      'This is a real rule block. Everything named below is on it.'));
    if (Blockly.Blocks.ruleBlock) {
      var anat = legendShow('ruleBlock', '', null);
      box.appendChild(anat.wrap);
      pending.push(anat);
    }
    LEGEND_PARTS.forEach(function (p) {
      var row = el('div', 'lgPart');
      var b = el('b', ''); b.textContent = p[0];
      row.appendChild(b);
      row.appendChild(el('div', 'lgWhat', p[1]));
      box.appendChild(row);
      n++;
    });

    /* Injected only once the rows are in the document: Blockly measures the
     * host, and a host with no place on the page measures zero. */
    pending.forEach(function (r) { legendBlock(r.host, r.type, r.build); });
    return n;
  }
  UI.buildLegend = buildLegend;

  function prettyStyleName(k) {
    return String(k).replace(/-/g, ' ').replace(/ ?block ?style$/i, '').trim();
  }
  function prettyTypeName(t) {
    return String(t).replace(/-/g, ' ');
  }
  // The site names its styles after what they hold, so the name is most of the
  // explanation. Only the ones whose name is not self explanatory get a gloss.
  var STYLE_GLOSS = {
    'rule-block-style': 'A rule: something that happens, and what to do when it does.',
    'control-block-style': 'Decides what runs: If, While, loops.',
    'variable-block-style': 'Reads or writes a value you have stored.',
    'value-block-style': 'Produces a value to plug into a socket.',
    'subroutine-block-style': 'A named set of actions you can call from anywhere.'
  };
  function styleMeaning(k) {
    return STYLE_GLOSS[k] || 'Blocks in the ' + prettyStyleName(k) + ' family.';
  }

  // ---- stale ObjIds -------------------------------------------------------
  function refreshStale() {
    if (!UI.ws) return;
    var stale = BF6.staleRefs(UI.ws);
    var seen = {};
    var before = UI.staleById || {}, current = {};
    stale.forEach(function (r) { current[r.blockId] = r.kind + ':' + r.objId; });
    Object.keys(before).forEach(function (id) {
      if (current[id] !== undefined) return;
      var b = UI.ws.getBlockById(id);
      try { if (b) b.setWarningText(null, 'bf6stale'); } catch (e) {}
    });
    stale.forEach(function (r) {
      var b = UI.ws.getBlockById(r.blockId);
      if (!b) return;
      seen[r.objId] = 1;
      if (before[r.blockId] === current[r.blockId]) return;
      try {
        b.setWarningText('ObjId ' + r.objId + ' is not on any placed ' + r.kind + ' in this level.',
          'bf6stale');
        // OURS, SO IT DOES NOT GET TO CHANGE THE BLOCK'S SIZE.
        //
        // This badge is the tool's, not the site's, and while it sat in the
        // row it made every block carrying one 50px narrower than the same
        // block on Portal - a difference with no cause on the site at all.
        // Marking it takes it out of the measurement and the drawer puts it
        // just off the block's right edge. The warning bubble, its text and
        // its click are untouched.
        (b.getIcons ? b.getIcons() : []).forEach(function (ic) {
          try {
            if (ic && ic.getType && String(ic.getType()) === 'warning') BF6.markAnnotationIcon(ic);
          } catch (e2) {}
        });
        try { b.queueRender ? b.queueRender() : b.render(); } catch (e2) {}
      } catch (e) {}
    });
    var lbl = $('stale');
    if (lbl) {
      var n = Object.keys(seen).length;
      lbl.textContent = n ? n + ' STALE OBJID' + (n > 1 ? 'S' : '') : '';
      lbl.className = n ? 'pill warnpill' : 'pill hidden';
    }
    UI.staleList = stale;
    UI.staleById = current;
  }

  function highlightScene(objIds) {
    UI.highlight = {};
    var wanted = {};
    (objIds || []).forEach(function (i) { wanted[String(i)] = 1; });
    UI.sceneHits = [];
    BF6.objIdRefs(UI.ws).forEach(function (r) {
      if (!wanted[String(r.objId)]) return;
      UI.highlight[r.blockId] = 'scene';
      UI.sceneHits.push(r.blockId);
      var b = UI.ws.getBlockById(r.blockId);
      try { b && b.setHighlighted && b.setHighlighted(true); } catch (e) {}
    });
    UI.sceneAt = -1;
    var lbl = $('scenehits');
    if (lbl) lbl.textContent = UI.sceneHits.length ? UI.sceneHits.length + ' BLOCKS REFERENCE THE SELECTION' : '';
    refreshNavigator();
    drawMinimap();
  }

  function nextSceneHit() {
    if (!UI.sceneHits || !UI.sceneHits.length) return;
    UI.sceneAt = (UI.sceneAt + 1) % UI.sceneHits.length;
    centreOn(UI.sceneHits[UI.sceneAt]);
  }

  // ---- canvas search ------------------------------------------------------
  function runSearch(text) {
    UI.searchHits = [];
    UI.searchAt = -1;
    var q = (text || '').toLowerCase();
    if (!q) { status('search cleared'); return; }
    UI.ws.getAllBlocks(false).forEach(function (b) {
      var hay = b.type.toLowerCase();
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          try { hay += ' ' + String(f.getText ? f.getText() : ''); } catch (e) {}
        });
      });
      if (hay.toLowerCase().indexOf(q) >= 0) UI.searchHits.push(b.id);
    });
    /* WHERE THE HITS ARE, NOT JUST HOW MANY.
     *
     * "312 matches" on a 52,000 block project tells the reader nothing they can
     * act on. Grouped by source file it becomes a shortlist: the file with 40 of
     * them is where the thing being looked for actually lives. */
    var byFile = {}, files = [];
    UI.searchHits.forEach(function (id) {
      var b = UI.ws.getBlockById(id);
      var f = b ? (fileOfBlock(b) || '(no file)') : '(no file)';
      if (!has(byFile, f)) { byFile[f] = 0; files.push(f); }
      byFile[f]++;
    });
    files.sort(function (a, b) { return byFile[b] - byFile[a]; });
    var where = files.slice(0, 3).map(function (f) {
      return byFile[f] + ' in ' + shortFile(f === '(no file)' ? '' : f);
    }).join(', ');
    status(UI.searchHits.length + ' match' + (UI.searchHits.length === 1 ? '' : 'es') +
      (files.length > 1 ? '  (' + where + (files.length > 3 ? ', ...' : '') + ')' : ''));
    if (UI.searchHits.length) stepSearch(1);
  }
  function stepSearch(dir) {
    if (!UI.searchHits.length) return;
    UI.searchAt = (UI.searchAt + dir + UI.searchHits.length) % UI.searchHits.length;
    centreOn(UI.searchHits[UI.searchAt]);
    status((UI.searchAt + 1) + ' of ' + UI.searchHits.length);
  }

  // ---- rule at a time -----------------------------------------------------
  function enterRuleMode(id) {
    if (UI.applying || BF6.state.loading) return;
    if (UI.debounce) { clearTimeout(UI.debounce); flush(); }
    if (UI.ruleMode) exitRuleMode();
    var b = UI.ws.getBlockById(id);
    if (!b) return;
    UI.ruleModeStash = BF6.saveWorkspace(UI.ws);
    var unit = BF6.saveUnit(b);
    var anchor = BF6.captureAnchor(b);
    UI.ruleMode = { id: id, anchor: anchor };
    UI.applying++;
    try {
      BF6.state.loading++;
      Blockly.Events.disable();
      try {
        UI.ws.clear();
        restoreVariables(UI.ruleModeStash.variables || []);
        var nb = Blockly.serialization.blocks.append(unit, UI.ws);
        nb.moveBy(40, 40);
      } finally { Blockly.Events.enable(); BF6.state.loading--; }
    } finally { UI.applying--; }
    $('rulemode').classList.add('on');
    BF6.snapshotUnits(UI.ws);
    UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));
    status('rule at a time: editing one block. EXIT writes it back.');
    drawMinimap();
  }

  function exitRuleMode() {
    if (!UI.ruleMode) return;
    if (UI.debounce) { clearTimeout(UI.debounce); flush(); }
    var stash = canvasSnapshot();
    UI.ruleMode = null; UI.ruleModeStash = null;
    UI.applying++;
    try {
      BF6.loadWorkspace(UI.ws, stash);
    } finally { UI.applying--; }
    $('rulemode').classList.remove('on');
    // Push the edited rule out for real, now that it is back in place.
    UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));
    after();
    status('rule written back');
  }

  // ---- slot search --------------------------------------------------------
  // Clicking an empty socket answers "what fits here", which the site never
  // does. The list is typed: the block's own signature says what the slot
  // takes, and only blocks that give that back are offered first.
  function installSlotAffordances() {
    var svg = UI.ws.getParentSvg();
    if (!svg || svg.bf6SlotHooked) return;
    svg.bf6SlotHooked = true;
    // A plain click on an empty socket opens the search. Clicking a hole is
    // the obvious way to ask what goes in it, and in Blockly it otherwise does
    // nothing at all, so nothing is taken away by answering. Alt-click and the
    // SLOT button still work and reach further, for a socket too small to hit
    // at low zoom.
    //
    // Two guards keep it out of the way: a click that was really the end of a
    // drag is ignored, and a plain click has to land close to the socket,
    // where a deliberate one does. The wide radius stays for the two explicit
    // routes, which have already said what they want.
    svg.addEventListener('mousedown', function (ev) {
      UI.slotDownAt = { x: ev.clientX, y: ev.clientY };
    }, true);
    // ONE CLICK, ONE MECHANISM.
    //
    // The socket's own glyph answers when it is clicked, which is exact and
    // needs no maths. This canvas listener used to try the same thing by
    // comparing a click against every connection's position, so a plain click
    // near a socket did nothing and the glyph then wanted a second one: two
    // clicks for a thing that should take one.
    //
    // It is kept only for the two DELIBERATE routes, which reach further than
    // a small glyph at low zoom: holding Alt, and the SLOT button. A plain
    // click is left entirely to the glyph.
    /* THE GLYPH CANNOT ANSWER ANY MORE, SO THE CANVAS HAS TO.
     *
     * The comment above says a plain click is left entirely to the socket's own
     * glyph, and that was right when the glyph was a FieldImage carrying a
     * click handler. It is not one now: with the drawer hook in, the renderer
     * paints the symbol inside the hole itself and syncSocketGlyphs REMOVES the
     * field. A painted shape has no handler, so plain clicks reached nothing at
     * all and the feature looked absent.
     *
     * The canvas listener takes plain clicks back, with a tight radius so it
     * only answers a click that really landed on a hole. Alt and the SLOT
     * button keep the wide radius, because they have already said what they
     * want and may be aiming at a small target at low zoom.
     */
    /* HOVER SAYS IT IS A TARGET, BEFORE ANY CLICK.
     *
     * Nothing on screen said a hole could be clicked, so nobody clicked one.
     * The rectangle lights up under the pointer, which also makes it obvious
     * where a dragged block will land. Throttled to a frame, and it only ever
     * looks at the one block under the pointer. */
    svg.addEventListener('mousemove', function (ev) {
      if (UI.hoverPending) { return; }
      UI.hoverPending = true;
      requestAnimationFrame(function () {
        UI.hoverPending = false;
        try {
          if (UI.ws && UI.ws.isDragging && UI.ws.isDragging()) {
            showSocketHighlight(null); return;
          }
          showSocketHighlight(socketUnderPointer(ev));
        } catch (e) {}
      });
    }, true);
    svg.addEventListener('mousedown', function () { focusWorkspace(); }, true);
    svg.addEventListener('mouseleave', function () {
      try { showSocketHighlight(null); } catch (e) {}
    }, true);

    svg.addEventListener('click', function (ev) {
      var deliberate = ev.altKey || UI.slotClickArmed;
      /* The end of a drag arrives here as a click. Ignore it: the user was
       * moving a block, not asking what fits in a hole. */
      if (!deliberate && UI.ws && UI.ws.isDragging && UI.ws.isDragging()) { return; }
      /* The same rectangle the highlight drew. If the pointer was inside a
       * hole it is a hit, with no radius to tune. The old distance test is
       * kept only as a fallback for the deliberate routes, which may be aimed
       * at a socket too small to land on at low zoom. */
      var hit = socketUnderPointer(ev);
      if (!hit && deliberate) { hit = pickEmptySlot(ev, 60); }
      if (hit) {
        trace('slot: canvas click found ' + hit.block.type + '.' +
              (hit.next ? 'NEXT' : (hit.input && hit.input.name)));
        openSlotSearch(hit);
        ev.stopPropagation();
        ev.preventDefault();
      } else if (deliberate) {
        trace('slot: no empty socket within reach of that click');
      }
    }, true);
  }

  // ==========================================================================
  // EMPTY SOCKETS ARE TARGETS, SO THEY BEHAVE LIKE TARGETS
  //
  // The old test measured the distance from the click to a connection POINT.
  // A hole is not a point: it is roughly 72 by 32, and a jagged edge runs the
  // width of the block. So a click that plainly landed inside a hole missed,
  // and there was nothing on screen to say a hole could be clicked at all.
  // The same maths is what makes dropping a block into a socket feel fussy.
  //
  // This works from the block under the pointer instead. elementFromPoint gives
  // the block, the block gives its own empty connections, and each connection
  // gives a rectangle the pointer is either inside or not. Only one block is
  // ever examined, so it costs the same on a 10,000 block canvas as on ten.
  // ==========================================================================

  /* Measured from the site's own renderer capture: an empty inline hole is 32
   * high and about 72 wide, and a statement notch runs the block width. */
  var HOLE_H = 32, HOLE_W = 72, EDGE_H = 26;

  function blockUnderPointer(ev) {
    try {
      var el2 = document.elementFromPoint(ev.clientX, ev.clientY);
      var g = el2 && el2.closest ? el2.closest('.blocklyDraggable') : null;
      while (g) {
        var id = g.getAttribute('data-id');
        if (id) {
          var b = UI.ws.getBlockById(id);
          if (b) { return b; }
        }
        g = g.parentElement && g.parentElement.closest ?
            g.parentElement.closest('.blocklyDraggable') : null;
      }
    } catch (e) {}
    return null;
  }

  /* Every empty connection on ONE block, as a rectangle in workspace units. */
  function socketRectsOf(b) {
    var out = [];
    if (!b) { return out; }
    var bxy;
    try { bxy = b.getRelativeToSurfaceXY(); } catch (e) { return out; }
    (b.inputList || []).forEach(function (inp) {
      if (!inp.connection || inp.connection.targetBlock()) { return; }
      var off = inp.connection.getOffsetInBlock ? inp.connection.getOffsetInBlock() : null;
      if (!off) { return; }
      var statement = inp.type === Blockly.inputTypes.STATEMENT;
      var w = statement ? Math.max(HOLE_W, (b.width || HOLE_W) - off.x - 8) : HOLE_W;
      var h = statement ? EDGE_H : HOLE_H;
      out.push({ block: b, input: inp, statement: statement,
                 x: bxy.x + off.x, y: bxy.y + off.y - h / 2, w: w, h: h });
    });
    /* The jagged edge underneath a rule or a function: where the next block
     * clips on. It is a target too, and nothing ever offered it. */
    if (b.nextConnection && !b.nextConnection.targetBlock()) {
      var noff = b.nextConnection.getOffsetInBlock ?
                 b.nextConnection.getOffsetInBlock() : { x: 0, y: b.height || 0 };
      out.push({ block: b, input: null, next: true, statement: true,
                 x: bxy.x + noff.x, y: bxy.y + noff.y - EDGE_H / 2,
                 w: Math.max(HOLE_W, b.width || HOLE_W), h: EDGE_H });
    }
    return out;
  }

  function socketUnderPointer(ev) {
    var b = blockUnderPointer(ev);
    if (!b) { return null; }
    var pt = mouseToWs(ev);
    var rects = socketRectsOf(b), i, r;
    for (i = 0; i < rects.length; i++) {
      r = rects[i];
      if (pt.x >= r.x && pt.x <= r.x + r.w && pt.y >= r.y && pt.y <= r.y + r.h) {
        return r;
      }
    }
    return null;
  }

  /* THE HIGHLIGHT. One rectangle, drawn on the block canvas so it moves and
   * zooms with the blocks, reused rather than rebuilt so hovering costs
   * nothing. */
  function socketHighlight() {
    if (UI.slotHi && UI.slotHi.parentNode) { return UI.slotHi; }
    try {
      var ns = 'http://www.w3.org/2000/svg';
      var r = document.createElementNS(ns, 'rect');
      r.setAttribute('class', 'bf6-slot-hi');
      r.setAttribute('rx', '6');
      r.setAttribute('fill', 'rgba(96,180,255,0.16)');
      r.setAttribute('stroke', '#6cf');
      r.setAttribute('stroke-width', '2');
      r.setAttribute('pointer-events', 'none');
      /* getCanvas is not on the workspace in this Blockly build, and asking
       * for it threw straight into a catch that returned null, so the
       * highlight silently never existed. The block canvas is a DOM node with
       * a stable class, which is what the rest of this file already uses. */
      var host = UI.ws.getParentSvg && UI.ws.getParentSvg();
      var layer = host && host.querySelector('.blocklyBlockCanvas');
      if (!layer) { trace('slot: no block canvas to draw the highlight on'); return null; }
      layer.appendChild(r);
      UI.slotHi = r;
      return r;
    } catch (e) { trace('slot: highlight could not be made: ' + (e && e.message)); return null; }
  }

  function showSocketHighlight(rect) {
    var r = socketHighlight();
    if (!r) { return; }
    if (!rect) { r.setAttribute('visibility', 'hidden'); return; }
    r.setAttribute('x', rect.x);
    r.setAttribute('y', rect.y);
    r.setAttribute('width', Math.max(8, rect.w));
    r.setAttribute('height', Math.max(8, rect.h));
    r.setAttribute('visibility', 'visible');
  }

  function pickEmptySlot(ev, radius) {
    var pt = mouseToWs(ev);
    var best = null, bestD = radius || 60;
    UI.ws.getAllBlocks(false).forEach(function (b) {
      (b.inputList || []).forEach(function (inp) {
        if (!inp.connection || inp.connection.targetBlock()) return;
        var xy = inp.connection.getOffsetInBlock ? inp.connection.getOffsetInBlock() : null;
        var bxy = b.getRelativeToSurfaceXY();
        var cx = bxy.x + (xy ? xy.x : 0), cy = bxy.y + (xy ? xy.y : 0);
        var d = Math.abs(cx - pt.x) + Math.abs(cy - pt.y);
        if (d < bestD) { bestD = d; best = { block: b, input: inp }; }
      });
      if (b.nextConnection && !b.nextConnection.targetBlock()) {
        var bxy2 = b.getRelativeToSurfaceXY();
        var d2 = Math.abs(bxy2.x - pt.x) + Math.abs(bxy2.y + b.height - pt.y);
        if (d2 < bestD) { bestD = d2; best = { block: b, input: null, next: true }; }
      }
    });
    return best;
  }

  function mouseToWs(ev) {
    var svg = UI.ws.getParentSvg();
    var rect = svg.getBoundingClientRect();
    var scale = UI.ws.scale;
    var origin = UI.ws.getOriginOffsetInPixels ? UI.ws.getOriginOffsetInPixels() : { x: 0, y: 0 };
    return {
      x: (ev.clientX - rect.left - origin.x) / scale,
      y: (ev.clientY - rect.top - origin.y) / scale
    };
  }

  // The socket glyphs call this when one is clicked. Precise by construction:
  // the picture that was clicked belongs to the socket being asked about, so
  // there is nothing to hit-test and nothing to get wrong.
  BF6.onSocketClick = function (block, input) {
    /* Traced because this has looked broken from outside twice now, and the
     * two ways it can fail are indistinguishable on screen: the click never
     * arriving, and the panel opening where it cannot be seen. */
    trace('slot: socket clicked on ' + (block && block.type) + '.' +
          (input && input.name));
    if (!block || !input) return;
    openSlotSearch({ block: block, input: input });
  };

  /* THE SUGGESTIONS ARE REAL BLOCKS, BECAUSE THEY ARE THE REAL PULL-OUT.
   *
   * A list of names in a panel was never going to look like the toolbox: the
   * toolbox draws actual blocks, in their category colours, at the size they
   * will be on the canvas. Rebuilding that by hand means rendering Blockly
   * blocks into a panel, which is Blockly's job and it already does it.
   *
   * So the candidates go into the workspace's own flyout. They come out
   * coloured and shaped correctly for free, they drag exactly like toolbox
   * blocks, and a click drops one straight into the socket that asked.
   */
  function openSlotFlyout(hit) {
    var fly = null;
    try { fly = UI.ws.getFlyout && UI.ws.getFlyout(); } catch (e) { fly = null; }
    if (!fly) { return false; }

    var kind = hit.next ? 'statement' :
      (hit.input && hit.input.type === Blockly.inputTypes.STATEMENT ? 'statement' : 'value');
    var inputName = hit.next ? 'NEXT' : (hit.input && hit.input.name);

    var r;
    try { r = BF6.slotCandidates(hit.block.type, inputName, kind, ''); }
    catch (e) { return false; }

    var contents = [];

    /* The level's own objects first, as real Number blocks already holding
     * their ObjId, so picking one is the same gesture as picking a block. */
    try {
      var wants = r.wanted || [];
      var takesNumber = !wants.length || wants.indexOf('Number') >= 0;
      if (takesNumber && kind === 'value') {
        (BF6.state.objIds || []).slice(0, 25).forEach(function (o) {
          contents.push({ kind: 'block', type: 'Number',
                          fields: { NUM: Number(o.id) } });
        });
      }
    } catch (e) {}

    var seen = {}, n = 0;
    [r.exact, r.loose, r.unknown].forEach(function (group) {
      (group || []).forEach(function (row) {
        if (n >= 120 || seen[row.type]) { return; }
        seen[row.type] = true; n++;
        contents.push({ kind: 'block', type: row.type });
      });
    });
    if (!contents.length) { return false; }

    /* What the socket takes, said once, since a flyout has no heading. */
    var wantedText = r.wanted ? r.wanted.join(' or ') :
                     (kind === 'statement' ? 'any action' : 'any value');
    status(hit.block.type + '.' + inputName + ' takes ' + wantedText +
           '  -  ' + n + ' block(s) offered on the left. Drag one in, or press Escape.');

    UI.pendingSocket = hit;
    try {
      fly.show({ kind: 'flyoutToolbox', contents: contents });
    } catch (e) { UI.pendingSocket = null; return false; }
    trace('slot: offered ' + contents.length + ' block(s) for ' +
          hit.block.type + '.' + inputName);
    return true;
  }

  /* A block dragged out of that flyout belongs in the socket that opened it. */
  function connectPendingSocket(made) {
    var hit = UI.pendingSocket;
    if (!hit || !made) { return; }
    UI.pendingSocket = null;
    try {
      var target = hit.next ? hit.block.nextConnection :
                   (hit.input && hit.input.connection);
      if (!target || target.targetBlock()) { return; }
      var from = hit.next || hit.statement ? made.previousConnection : made.outputConnection;
      if (from && target.isConnectionAllowed(from)) {
        target.connect(from);
        status('put ' + made.type + ' in ' + hit.block.type);
      }
    } catch (e) { UI.reportFault('putting the block in the socket', e); }
  }

  /* A PICTURE OF THE ACTUAL BLOCK, IN THE COMPACT PANEL.
   *
   * A name in a list does not tell you what you are about to place. The real
   * pull-out shows the block itself, in its category colour, in its own shape,
   * which is most of why it is readable. Borrowing the whole pull-out was too
   * big for a socket question, so the block is rendered once in a hidden
   * workspace and its drawing is copied into the row.
   *
   * Cached by type: a project offers the same forty or so blocks over and
   * over, and rendering each one once is the difference between instant and
   * unusable.
   */
  function previewWorkspace() {
    if (UI.previewWs) { return UI.previewWs; }
    var host = $('blockpreview');
    if (!host) { return null; }
    try {
      UI.previewWs = Blockly.inject(host, {
        readOnly: true, scrollbars: false, sounds: false,
        renderer: (UI.ws && UI.ws.options && UI.ws.options.renderer) || undefined,
        theme: (UI.ws && UI.ws.getTheme && UI.ws.getTheme()) || undefined
      });
    } catch (e) {
      trace('slot: preview workspace could not be made: ' + (e && e.message));
      UI.previewWs = null;
    }
    return UI.previewWs;
  }

  function blockPreview(type, fields) {
    UI.previewCache = UI.previewCache || {};
    var key = type + (fields ? JSON.stringify(fields) : '');
    if (UI.previewCache[key]) { return UI.previewCache[key].cloneNode(true); }
    var ws = previewWorkspace();
    if (!ws) { return null; }
    var out = null;
    try {
      ws.clear();
      var spec = { type: type };
      if (fields) { spec.fields = fields; }
      var b = Blockly.serialization.blocks.append(spec, ws);
      if (b.initSvg) { b.initSvg(); }
      if (b.render) { b.render(); }
      var root = b.getSvgRoot();
      var bb = root.getBBox();
      var ns = 'http://www.w3.org/2000/svg';
      var svg = document.createElementNS(ns, 'svg');
      var scale = Math.min(1, 250 / Math.max(1, bb.width));
      svg.setAttribute('width', Math.ceil(bb.width * scale));
      svg.setAttribute('height', Math.ceil(bb.height * scale));
      svg.setAttribute('viewBox', bb.x + ' ' + bb.y + ' ' + bb.width + ' ' + bb.height);
      svg.setAttribute('class', 'blocklyBlockCanvas slot-preview-svg');
      svg.appendChild(root.cloneNode(true));
      ws.clear();
      UI.previewCache[key] = svg;
      out = svg.cloneNode(true);
    } catch (e) { out = null; }
    return out;
  }

  function openSlotSearch(hit) {
    var pop = $('slotpop');
    if (!pop) { trace('slot: there is no slotpop element on this page'); return; }
    pop.innerHTML = '';
    pop.classList.add('on');
    var kind = hit.next ? 'statement' :
      (hit.input.type === Blockly.inputTypes.STATEMENT ? 'statement' : 'value');
    var inputName = hit.next ? 'NEXT' : hit.input.name;
    var head = el('div', 'slot-head', 'WHAT FITS IN ' + hit.block.type + '.' + inputName);
    pop.appendChild(head);
    var box = el('input', 'slot-filter');
    box.setAttribute('placeholder', 'filter');
    pop.appendChild(box);
    var list = el('div', 'slot-list');
    pop.appendChild(list);
    function paint() {
      var r = BF6.slotCandidates(hit.block.type, inputName, kind, box.value);
      list.innerHTML = '';
      var wanted = r.wanted ? r.wanted.join(' | ') : (kind === 'statement' ? 'any action' : 'any value');
      head.textContent = 'WHAT FITS IN ' + hit.block.type + '.' + inputName + '   takes ' + wanted;
      /* WHAT IS ACTUALLY IN THE LEVEL, FIRST.
       *
       * The editor already knows every placed object and its ObjId, and a
       * socket that wants a Number is nearly always asking for one of them.
       * Offering the real objects ahead of the 604 block types turns a
       * question about block vocabulary into a question about this level,
       * which is the one the author can actually answer.
       */
      var sceneRows = [];
      try {
        var wants = r.wanted || [];
        var takesNumber = !wants.length || wants.indexOf('Number') >= 0;
        (BF6.state.objIds || []).forEach(function (o) {
          var ty = String(o.type || '');
          var fits = takesNumber || wants.some(function (w) {
            return ty && w.toLowerCase().indexOf(ty.toLowerCase()) >= 0;
          });
          if (!fits) { return; }
          var hay = (o.name + ' ' + ty + ' ' + o.id).toLowerCase();
          if (box.value && hay.indexOf(box.value.toLowerCase()) < 0) { return; }
          sceneRows.push(o);
        });
      } catch (e) {}
      if (sceneRows.length) {
        list.appendChild(el('div', 'slot-group', 'IN YOUR LEVEL'));
        sceneRows.slice(0, 60).forEach(function (o) {
          var item = el('div', 'slot-item');
          var opv = blockPreview('Number', { NUM: Number(o.id) });
          if (opv) { var oshot = el('div', 'slot-shot'); oshot.appendChild(opv); item.appendChild(oshot); }
          item.appendChild(el('div', 'slot-type', o.name || ('ObjId ' + o.id)));
          item.appendChild(el('div', 'slot-sig',
            (o.type || 'object') + '   ObjId ' + o.id));
          item.onclick = function () { insertObjIdInSlot(hit, o.id); closeSlot(); };
          list.appendChild(item);
        });
      }
      var groups = [['FITS', r.exact], ['OTHER VALUES', r.loose], ['TYPE UNKNOWN', r.unknown]];
      var first = null;
      groups.forEach(function (g) {
        if (!g[1].length) return;
        list.appendChild(el('div', 'slot-group', g[0]));
        var byCat = {};
        g[1].forEach(function (row) { (byCat[row.category || 'OTHER'] = byCat[row.category || 'OTHER'] || []).push(row); });
        Object.keys(byCat).sort().forEach(function (cat) {
          list.appendChild(el('div', 'slot-cat', cat));
          byCat[cat].slice(0, 200).forEach(function (row) {
            var item = el('div', 'slot-item');
            /* The block as it will actually look, then the words. A picture
             * of the thing answers 'which one is it' faster than its name. */
            var pv = blockPreview(row.type, null);
            if (pv) {
              var shot = el('div', 'slot-shot');
              shot.appendChild(pv);
              item.appendChild(shot);
            }
            /* The same colour the toolbox gives this category, so a block
             * found here and the same block found in the pull-out look alike. */
            try {
              var st2 = BF6.state.style;
              var cs = st2 && st2.categoryStyles && st2.categoryStyles[row.category];
              var col = (cs && (cs.colour || cs.color)) ||
                        (BF6.state.blockColours || {})[row.type];
              if (col) { item.style.borderLeftColor = col; }
            } catch (e) {}
            item.appendChild(el('div', 'slot-type', row.type));
            var sig = (BF6.state.signatures || {})[row.type];
            /* What it does, in the site's own words, with the signature under
             * it. The description is the thing a person new to blocks needs;
             * the signature is what tells an author whether it fits. */
            var desc = BF6.stripTags(row.tooltip || '');
            if (desc) { item.appendChild(el('div', 'slot-desc', desc.slice(0, 150))); }
            if (sig) {
              item.appendChild(el('div', 'slot-sig', sig.map(function (s) {
                return s.name + '(' + s.args.join(', ') + ')' + (s.ret ? ' : ' + s.ret : '');
              }).join('   ')));
            }
            item.onclick = function () { insertInSlot(hit, row.type); closeSlot(); };
            if (!first) first = row.type;
            list.appendChild(item);
          });
        });
      });
      UI.slotFirst = first;
    }
    box.oninput = paint;
    box.onkeydown = function (e) {
      if (e.key === 'Enter' && UI.slotFirst) { insertInSlot(hit, UI.slotFirst); closeSlot(); }
      if (e.key === 'Escape') closeSlot();
    };
    paint();
    /* BESIDE THE HOLE IT IS ABOUT, NOT IN THE MIDDLE OF THE SCREEN.
     * Centre screen meant the socket being filled was usually hidden behind
     * the panel choosing what to put in it. */
    try {
      var host = $('centre') || document.body;
      var hb = host.getBoundingClientRect();
      var at = hit.screen || null;
      if (!at && hit.block) {
        var bb = hit.block.getSvgRoot().getBoundingClientRect();
        at = { x: bb.right, y: bb.top };
      }
      if (at) {
        var w = 320, pad = 12;
        var left = at.x - hb.left + pad;
        var top = at.y - hb.top;
        /* The canvas deliberately keeps its full width when the side panel
         * opens, so #centre's right edge is not the edge this can use. The
         * panel sits over the right of it, and clamping to hb.width alone put
         * the suggestions underneath the panel where they could not be read.
         * The minimap and Blockly's own furniture step aside for the same
         * reason; this is the same move, measured off the panel itself so it
         * stays right if the panel's width changes. */
        var edge = hb.width;
        try {
          var shelf = document.body.classList.contains('shelf-open') ? $('side') : null;
          if (shelf) {
            var sb = shelf.getBoundingClientRect();
            if (sb.width > 0) { edge = Math.min(edge, sb.left - hb.left); }
          }
        } catch (e) {}
        if (left + w > edge - pad) { left = Math.max(pad, edge - w - pad); }
        if (top < pad) { top = pad; }
        if (top > hb.height - 160) { top = Math.max(pad, hb.height - 160); }
        pop.style.left = Math.round(left) + 'px';
        pop.style.top = Math.round(top) + 'px';
        pop.style.transform = 'none';
      }
    } catch (e) {}
    box.focus();
    /* Did it open, and can it be seen? A panel that renders behind the site
     * stylesheet looks exactly like one that never opened. */
    try {
      var rr = pop.getBoundingClientRect();
      var cs = window.getComputedStyle(pop);
      trace('slot: panel ' + Math.round(rr.width) + 'x' + Math.round(rr.height) +
            ' at ' + Math.round(rr.left) + ',' + Math.round(rr.top) +
            '  display ' + cs.display + '  z ' + cs.zIndex +
            '  rows ' + pop.querySelectorAll('.slot-item').length);
    } catch (e) {}
  }
  function closeSlot() { var p = $('slotpop'); p.classList.remove('on'); p.innerHTML = ''; UI.slotClickArmed = false; }

  /* A placed object is an ObjId, and an ObjId is a Number block. Same route
   * the SELECTED button already uses, so the two agree. */
  function insertObjIdInSlot(hit, objId) {
    UI.applying++;
    try {
      BF6.state.loading++;
      var made = null;
      try {
        made = Blockly.serialization.blocks.append(
          { type: "Number", fields: { NUM: Number(objId) } }, UI.ws);
      } finally { BF6.state.loading--; }
      if (made && hit.input && hit.input.connection && made.outputConnection) {
        hit.input.connection.connect(made.outputConnection);
        status("put ObjId " + objId + " in " + hit.block.type + "." + hit.input.name);
      }
    } catch (e) { UI.reportFault("placing an object id", e); }
    finally { UI.applying--; }
    after();
  }

  function insertInSlot(hit, type) {
    UI.applying++;
    var made = null;
    try {
      BF6.state.loading++;
      try {
        made = Blockly.serialization.blocks.append({ type: type }, UI.ws);
        var target = hit.next ? hit.block.nextConnection : hit.input.connection;
        var mine = made.previousConnection || made.outputConnection;
        if (target && mine) target.connect(mine);
      } finally { BF6.state.loading--; }
    } catch (e) { status('could not connect a ' + type + ' there: ' + e, true); }
    finally { UI.applying--; }
    flush();
    after();
    if (made) { try { made.select(); } catch (e) {} }
  }

  // ---- minimap ------------------------------------------------------------
  function drawMinimap() {
    var c = $('minimap');
    if (!c || !UI.ws) return;
    if (!c.clientWidth || !c.clientHeight || UI.loadingDoc || UI.applying ||
        (UI.ws.isDragging && UI.ws.isDragging())) return;
    var ctx = c.getContext('2d');
    var w = c.width = c.clientWidth, h = c.height = c.clientHeight;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = '#0D0F10';
    ctx.fillRect(0, 0, w, h);
    var tops = UI.ws.getTopBlocks(false);
    if (!tops.length) return;
    var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    var boxes = [];
    tops.forEach(function (b) {
      var xy = b.getRelativeToSurfaceXY();
      var hw = b.getHeightWidth ? b.getHeightWidth() : { width: 200, height: 60 };
      var box = { x: xy.x, y: xy.y, w: hw.width, h: hw.height, type: b.type, id: b.id,
                 file: (b.type === 'modBlock') ? ' rules' : (fileOfBlock(b) || '(no file)') };
      boxes.push(box);
      minX = Math.min(minX, box.x); minY = Math.min(minY, box.y);
      maxX = Math.max(maxX, box.x + box.w); maxY = Math.max(maxY, box.y + box.h);
    });
    /* One hue per file, in the same order the tab strip uses so the two agree. */
    var mapFileIndex = {}, mapFileCount = 0;
    boxes.forEach(function (b) {
      if (b.file && !has(mapFileIndex, b.file)) { mapFileIndex[b.file] = mapFileCount++; }
    });
    mapFileCount = Math.max(1, mapFileCount);
    var pad = 20;
    var sx = (w - pad) / Math.max(1, maxX - minX), sy = (h - pad) / Math.max(1, maxY - minY);
    var s = Math.min(sx, sy);
    UI.mapXform = { minX: minX, minY: minY, s: s, pad: pad / 2 };
    boxes.forEach(function (b) {
      /* COLOURED BY FILE, NOT BY BLOCK TYPE.
       *
       * Three colours for mod, subroutine and everything else made the minimap
       * a shape. One colour per source file makes it a map: the reader can see
       * that weapons is the big band on the left and recognise it again in the
       * tab strip, which uses the same hues. */
      ctx.fillStyle = b.file ? regionColour(mapFileIndex[b.file] || 0, mapFileCount)
                             : '#5A5F63';
      if (UI.highlight[b.id]) ctx.fillStyle = '#FF581A';
      ctx.fillRect(pad / 2 + (b.x - minX) * s, pad / 2 + (b.y - minY) * s,
        Math.max(2, b.w * s), Math.max(2, b.h * s));
    });
    // the rules inside the mod, so a big project is not one grey slab
    UI.ws.getAllBlocks(false).forEach(function (b) {
      if (b.type !== 'ruleBlock') return;
      var xy = b.getRelativeToSurfaceXY();
      var hw = b.getHeightWidth ? b.getHeightWidth() : { width: 200, height: 40 };
      ctx.fillStyle = UI.highlight[b.id] ? '#FF581A' : '#A6572B';
      ctx.fillRect(pad / 2 + (xy.x - minX) * s, pad / 2 + (xy.y - minY) * s,
        Math.max(2, hw.width * s), Math.max(2, hw.height * s));
    });
    // the viewport
    var m = UI.ws.getMetrics();
    if (m) {
      ctx.strokeStyle = '#FF581A';
      ctx.lineWidth = 1;
      var vx = (m.viewLeft - minX) * s + pad / 2;
      var vy = (m.viewTop - minY) * s + pad / 2;
      ctx.strokeRect(vx, vy, m.viewWidth * s, m.viewHeight * s);
    }
  }

  function minimapJump(ev) {
    if (!UI.mapXform || !UI.ws) return;
    var c = $('minimap');
    var rect = c.getBoundingClientRect();
    var x = (ev.clientX - rect.left - UI.mapXform.pad) / UI.mapXform.s + UI.mapXform.minX;
    var y = (ev.clientY - rect.top - UI.mapXform.pad) / UI.mapXform.s + UI.mapXform.minY;
    /* viewWidth/viewHeight are already in the same pixel space as the canvas
     * translation, so only the workspace coordinate needs scaling. Scaling the
     * viewport half-extent as well put the jump off-centre by a factor of the
     * zoom, which was invisible at 100% and grew with every step away from it.
     * Blockly's own zoomCenter uses viewWidth/2 unscaled for the same reason. */
    var vm = UI.ws.getMetrics();
    UI.ws.scroll(-x * UI.ws.scale + vm.viewWidth / 2,
      -y * UI.ws.scale + vm.viewHeight / 2);
    drawMinimap();
  }

  // Never let the floor sit above what it takes to see the whole board.
  function refreshZoomFloor() {
    if (!UI.ws) return;
    var bb = UI.ws.getBlocksBoundingBox();
    var m = UI.ws.getMetrics();
    if (!bb || !m) return;
    var w = (bb.right - bb.left) || bb.width || 1;
    var h = (bb.bottom - bb.top) || bb.height || 1;
    var need = BF6.fitScale({ width: w, height: h }, m.viewWidth, m.viewHeight, 40);
    var opt = UI.ws.options.zoomOptions;
    // A tenth below what it takes to fit, so the fit is never ON the limit,
    // and no clamp of our own above that.
    if (opt && need < opt.minScale) opt.minScale = Math.max(0.0001, need * 0.9);
  }

  function fitAll() {
    refreshZoomFloor();
    try { UI.ws.zoomToFit(); } catch (e) {}
    drawMinimap();
    status('fitted the whole board');
  }

  // ---- snippets -----------------------------------------------------------
  function refreshSnippets() {
    var sel = $('snippets');
    if (!sel) return;
    sel.innerHTML = '';
    var first = el('option', null, 'Pick an example');
    first.value = '';
    sel.appendChild(first);
    UI.snippets.forEach(function (n) {
      var o = el('option', null, n);
      o.value = n;
      sel.appendChild(o);
    });
  }

  function pasteSnippet(json, name) {
    if (!json) return;
    var ctx = {
      objId: function (kind) {
        var sel = (BF6.state.sceneSelected || []);
        if (sel.length) return sel[0];
        var row = (BF6.state.objIds || []).filter(function (r) {
          return !kind || (r.type || '').toLowerCase().indexOf(kind.toLowerCase()) >= 0;
        })[0];
        if (row) return row.id;
        var typed = window.prompt('ObjId for the ' + kind + ' this snippet needs:');
        return typed === null ? null : Number(typed);
      },
      variable: function (nm, type) {
        var v = UI.ws.getVariable(nm, type || '');
        if (!v) v = UI.ws.createVariable(nm, type || '');
        return v.getId();
      },
      text: function (label) {
        var typed = window.prompt(label, label);
        return typed === null ? label : typed;
      }
    };
    var filled = BF6.fillPlaceholders(json, ctx);
    var blocks = filled.json.blocks && filled.json.blocks.blocks ? filled.json.blocks.blocks :
      (Array.isArray(filled.json) ? filled.json : [filled.json]);
    var m = UI.ws.getMetrics();
    var cx = m ? (m.viewLeft + m.viewWidth / 2) : 0;
    var cy = m ? (m.viewTop + m.viewHeight / 3) : 0;
    UI.applying++;
    var placed = [];
    try {
      BF6.state.loading++;
      try {
        blocks.forEach(function (b, i) {
          var copy = BF6.stripIds(JSON.parse(JSON.stringify(b)));
          var nb = Blockly.serialization.blocks.append(copy, UI.ws);
          var xy = nb.getRelativeToSurfaceXY();
          nb.moveBy(cx - xy.x + i * 40, cy - xy.y + i * 40);
          placed.push(nb);
          // A rule belongs in the mod, not loose on the canvas.
          if (nb.type === 'ruleBlock') {
            var mod = UI.ws.getTopBlocks(false).filter(function (t) { return t.type === 'modBlock'; })[0];
            if (mod) {
              var inp = mod.getInput('RULES');
              var last = inp && inp.connection && inp.connection.targetBlock();
              while (last && last.getNextBlock()) last = last.getNextBlock();
              try {
                if (last) last.nextConnection.connect(nb.previousConnection);
                else inp.connection.connect(nb.previousConnection);
              } catch (e) {}
            }
          }
        });
      } finally { BF6.state.loading--; }
    } finally { UI.applying--; }
    flush(); after();
    status('pasted ' + (name || 'snippet') + ': ' + placed.length + ' top level block' +
      (placed.length === 1 ? '' : 's') +
      (filled.missing.length ? '  (' + filled.missing.length + ' placeholder left at 0)' : ''));
    if (placed.length) centreOn(placed[0].id);
  }

  function pasteClipboard(text) {
    var json;
    try { json = JSON.parse(text); } catch (e) { return false; }
    if (!json || typeof json !== 'object') return false;
    pasteSnippet(json.mod ? json.mod : json, 'clipboard');
    return true;
  }

  // ---- object assignment --------------------------------------------------
  function focusedNumberBlock() {
    var b = Blockly.getSelected();
    if (!b) return null;
    if (b.type === 'Number') {
      var p = b.getParent();
      if (p && BF6.OBJID_BLOCKS[p.type]) return { number: b, holder: p, kind: BF6.OBJID_BLOCKS[p.type] };
      return { number: b, holder: null, kind: null };
    }
    if (BF6.OBJID_BLOCKS[b.type]) {
      var inp = b.getInput('VALUE-0');
      var child = inp && inp.connection && inp.connection.targetBlock();
      if (child && child.type === 'Number') return { number: child, holder: b, kind: BF6.OBJID_BLOCKS[b.type] };
      return { number: null, holder: b, kind: BF6.OBJID_BLOCKS[b.type] };
    }
    return null;
  }

  function useSelectedObject() {
    var f = focusedNumberBlock();
    if (!f) { status('select the Number in a Get block first', true); return; }
    send({ op: 'useSelected', kind: f.kind || '', wantAssign: true });
    UI.pendingFill = f;
  }

  function fillSelectedObjId(msg) {
    if (!msg.ok) { status(msg.text || 'no object selected in the level', true); return; }
    var f = UI.pendingFill || focusedNumberBlock();
    if (!f) { status('nothing focused to fill', true); return; }
    UI.applying++;
    try {
      var num = f.number;
      if (!num && f.holder) {
        BF6.state.loading++;
        try {
          num = Blockly.serialization.blocks.append({ type: 'Number' }, UI.ws);
          f.holder.getInput('VALUE-0').connection.connect(num.outputConnection);
        } finally { BF6.state.loading--; }
      }
      if (num) num.setFieldValue(msg.objId, 'NUM');
    } finally { UI.applying--; }
    UI.pendingFill = null;
    flush(); after();
    status('filled ObjId ' + msg.objId + ' from ' + (msg.name || 'the selected object'));
  }

  function assignToSelected() {
    var f = focusedNumberBlock();
    if (!f || !f.number) { status('select the Number that holds the ObjId', true); return; }
    var v = Number(f.number.getFieldValue('NUM'));
    if (isNaN(v)) { status('that is not a number', true); return; }
    send({ op: 'assignToSelected', objId: v, kind: f.kind || '' });
  }

  // ==========================================================================
  // Import and export
  //
  // Files move both ways in the site's own formats. editor.js owns the two
  // schemas and the detection; what is here is the page: the drop target, the
  // buttons, the panel that lists what came out of a full experience, and the
  // one place a file is turned into a workspace.
  // ==========================================================================
  UI.shell = null;             // a full experience's non-workspace fields
  UI.shellName = '';           // where that shell came from, for the summary
  UI.pendingAttachments = [];  // what an imported experience carried

  function growDefinitions(wsJson) {
    // A file can carry a type this session has never seen, from a live capture
    // or from the converter. The definitions grow to cover whatever it uses.
    //
    // GROWING THE NATIVE DEFINITIONS USED TO DESTROY THE EXTENDED ONES.
    //
    // installFallback registers a generic shape for every type it does not
    // recognise, and it does not recognise the extended language's blocks
    // because they are not Portal blocks. Reopening a project therefore
    // replaced each custom block with a plain fallback and lost its dynamic
    // inputs. That was found by a real browser test in the handoff package,
    // not reasoned about, and it is exactly the kind of failure that looks
    // like a corrupt save.
    //
    // preserveDefinitions snapshots the extension's constructors, runs this
    // synchronously, and puts them back.
    try {
      var types = window.BF6_TYPES_FALLBACK || [];
      var refresh = function () {
        BF6.installFallback(types, BF6.observe(wsJson));
        BF6.buildSignatures(BF6.state.tooltips || null);
      };
      if (window.BF6ExtendedIntegration && window.BF6ExtendedIntegration.preserveDefinitions) {
        window.BF6ExtendedIntegration.preserveDefinitions(Blockly, refresh);
      } else {
        refresh();
      }
    } catch (e) {}
  }

  // A THROW IN HERE ARRIVED AS THE WORDS "Script error." AND NOTHING ELSE.
  //
  // This page is served from file://, and Chromium treats every other file://
  // script as an opaque origin, so an exception raised inside the vendored
  // Blockly reaches window.onerror with its message and its stack stripped off.
  // A workspace that threw therefore looked exactly like a workspace that drew
  // nothing: a blank canvas and one useless line in the log.
  //
  // Catching it HERE, inside our own script, is what makes it legible. The
  // exception object is still intact at this point, so reportFault can carry
  // the real message and stack out to the tool. The steps are named separately
  // because knowing WHICH one failed is most of the answer.
  /* A LOAD MUST NOT STOP THE EDITOR.
   *
   * The page shares Unreal's thread, so one long JavaScript task freezes the
   * whole editor: no viewport, no menus, no console, nothing. Loading a
   * workspace in a single call did exactly that. Measured on 2026-09-07: a
   * 2,000 block slice held the editor for over a hundred seconds, and 52,427
   * blocks never came back at all.
   *
   * So the load is cut into batches of top level items with a yield between
   * them. Each batch is a short task, the editor breathes in the gaps, and the
   * progress bar can actually paint. Slower in total than one blocking call,
   * and the difference between a tool you can use and one that appears to have
   * crashed.
   *
   * Events and rendering stay off for the whole run rather than per batch:
   * turning them on between batches would render the half built workspace and
   * cost more than the load.
   */
  var BATCH = 5;

  /* The bar. Built on first use so it costs nothing until something loads, and
   * styled inline so it does not depend on the captured site stylesheet: a
   * project that fails to load must still be able to say so. */
  function loadProgressEl() {
    var el = document.getElementById('bf6-load-progress');
    if (el) { return el; }
    el = document.createElement('div');
    el.id = 'bf6-load-progress';
    el.style.cssText = 'position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);' +
      'z-index:99999;min-width:340px;padding:18px 22px;border-radius:8px;' +
      'background:rgba(16,18,20,0.96);border:1px solid #3a4046;' +
      'box-shadow:0 8px 32px rgba(0,0,0,0.55);' +
      'font:14px BFText-Regular,system-ui,sans-serif;color:#e7ecef;';
    el.innerHTML =
      '<div id="bf6-load-what" style="margin-bottom:10px"></div>' +
      '<div style="height:6px;background:#2a2f34;border-radius:3px;overflow:hidden">' +
      '<div id="bf6-load-fill" style="height:100%;width:0%;background:#4ea1ff;' +
      'transition:width .12s linear"></div></div>' +
      '<div id="bf6-load-count" style="margin-top:8px;color:#9aa6ae;' +
      'font-variant-numeric:tabular-nums"></div>';
    document.body.appendChild(el);
    return el;
  }

  function showLoadProgress(n, total, what) {
    var canvas = $('canvas');
    if (canvas) { canvas.inert = true; canvas.style.pointerEvents = 'none'; }
    var el = loadProgressEl();
    el.style.display = '';
    var pct = total ? Math.round((n / total) * 100) : 0;
    document.getElementById('bf6-load-what').textContent =
      what ? ('Opening ' + what) : 'Opening the workspace';
    document.getElementById('bf6-load-fill').style.width = pct + '%';
    document.getElementById('bf6-load-count').textContent = total
      ? (n + ' of ' + total + ' top level item(s), ' + pct + '%')
      : 'preparing';
  }

  function hideLoadProgress() {
    var canvas = $('canvas');
    if (canvas) { canvas.inert = false; canvas.style.pointerEvents = ''; }
    var el = document.getElementById('bf6-load-progress');
    if (el) { el.style.display = 'none'; }
  }

  /* A COLLAPSED BLOCK IS NAMED BEFORE IT KNOWS ITS NAME.
   *
   * Blockly builds the one line summary shown on a collapsed block at the
   * moment it collapses, out of the block's current field values. Loading a
   * saved block does this, in Blockly's own order:
   *
   *   loadCoords -> loadAttributes -> loadExtraState -> ... -> loadFields
   *
   * loadAttributes is what applies collapsed:true, and loadFields runs after
   * it. So the summary is made from the field DEFAULT and the real name lands
   * a step too late. Every converted subroutine came in reading "Subroutine",
   * and expanding one regenerated the text and showed the right name, which is
   * exactly how the user described it.
   *
   * Collapsing again after the load rebuilds the text with the fields in
   * place. Only collapsed blocks pay for it, and only once per load.
   */
  /* What a collapsed row should say. The name first and in full, then a size so
   * the reader can tell a two line helper from a six hundred block routine
   * without opening either. Returns null for blocks with nothing better to say
   * than Blockly's own summary. */
  function collapsedLabelFor(b) {
    try {
      var kids = b.getDescendants ? (b.getDescendants(false).length - 1) : 0;
      var size = kids > 0 ? '   (' + kids + ' blocks)' : '';

      var name = b.getFieldValue && b.getFieldValue('SUBROUTINE_NAME');
      if (name) {
        var lead = (b.type === 'subroutineInstanceBlock') ? 'CALL  ' : 'SUBROUTINE  ';
        return lead + name + size;
      }
      var rule = b.getFieldValue && b.getFieldValue('NAME');
      if (rule) { return 'RULE  ' + rule + size; }

      /* A COLLAPSED BLOCK MUST NOT COME OUT BLANK.
       *
       * Blockly summarises a collapsed block with toString(), which walks the
       * fields and asks each for its TEXT. This editor draws most labels as
       * pictures instead of text for speed, so the text it finds is empty and
       * the collapsed block reads as nothing at all. Collapsing anything by
       * hand produced a blank bar.
       *
       * The field VALUE is untouched by that: only the drawing was replaced.
       * So the summary is built from values, which is what the field would have
       * said if it were still text.
       */
      var parts = [];
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          var v = '';
          try { v = f.getValue ? String(f.getValue() == null ? '' : f.getValue()) : ''; }
          catch (e) { v = ''; }
          if (!v) { try { v = f.getText ? String(f.getText() || '') : ''; } catch (e2) { v = ''; } }
          v = v.replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
          if (v) { parts.push(v); }
        });
      });
      var text = parts.join(' ');
      if (!text) { return b.type + size; }
      if (text.length > 60) { text = text.slice(0, 57) + '...'; }
      return text + size;
    } catch (e) { return null; }
  }

  /* COLLAPSING BY HAND GOES THROUGH THE SAME REPAIR AS LOADING.
   *
   * The fix above only ran after a workspace load, so the COLLAPSE ALL button
   * and the right click menu still produced blank bars. Blockly reports a
   * collapse as a BLOCK_CHANGE with element "collapsed", so that is where this
   * belongs: one block, one label, no sweep. */
  function watchCollapse(ws) {
    ws.addChangeListener(function (ev) {
      if (!ev || ev.type !== Blockly.Events.BLOCK_CHANGE) { return; }
      if (ev.element !== 'collapsed') { return; }
      try {
        var b = UI.ws.getBlockById(ev.blockId);
        if (!b) { return; }
        if (ev.newValue && b.isCollapsed()) {
          var label = collapsedLabelFor(b);
          var cf = b.getField('_TEMP_COLLAPSED_FIELD');
          if (label && cf) { cf.setValue(label); }
        }
        /* Opening one makes it hundreds of times taller than the row it was
         * given, so everything under it has to move. See reflowColumn. */
        reflowColumn(b);
      } catch (e) {}
    });
  }

  /* THE COLUMN CLOSES UP AFTER AN EXPAND, INSTEAD OF BLOCKS LANDING ON TOP OF
   * EACH OTHER.
   *
   * The converter lays one file per column with every subroutine on a 44 pixel
   * row, which is exactly right while they are all collapsed. Open one and it
   * becomes hundreds of pixels tall while its neighbours stay where they were,
   * so it draws straight over the ones below it.
   *
   * Blockly has no layout engine to ask, so the column is re-flowed by hand:
   * take the blocks sharing this column, in the order they already sit, and
   * stack them again using their real heights. Only the one column moves, and
   * nothing changes order, so the reader's map of the file survives.
   */
  function reflowColumn(block) {
    if (!UI.ws || !block) { return; }
    var GAP = 12, COL_TOLERANCE = 60;
    var here;
    try { here = block.getRelativeToSurfaceXY(); } catch (e) { return; }

    var column = [];
    UI.ws.getTopBlocks(false).forEach(function (t) {
      if (t.type === 'modBlock') { return; }
      var xy;
      try { xy = t.getRelativeToSurfaceXY(); } catch (e) { return; }
      if (Math.abs(xy.x - here.x) <= COL_TOLERANCE) { column.push({ b: t, y: xy.y }); }
    });
    if (column.length < 2) { return; }
    column.sort(function (a, b2) { return a.y - b2.y; });

    var top = column[0].y;
    var y = top;
    /* Events off: this is layout, not an edit the user made, and it must not
     * fill the undo stack or be pushed to the site as a change. */
    var wasLoading = BF6.state.loading;
    BF6.state.loading++;
    Blockly.Events.disable();
    try {
      for (var i = 0; i < column.length; i++) {
        var t = column[i].b;
        var xy = t.getRelativeToSurfaceXY();
        if (Math.round(xy.y) !== Math.round(y)) { t.moveBy(0, y - xy.y); }
        var hw = { height: 44 };
        try { hw = t.getHeightWidth(); } catch (e) {}
        y += Math.max(36, hw.height) + GAP;
      }
    } finally {
      Blockly.Events.enable();
      BF6.state.loading = wasLoading;
    }
    drawMinimap();
  }

  function refreshCollapsedText() {
    var n = 0;
    try {
      var all = UI.ws.getAllBlocks(false);
      for (var i = 0; i < all.length; i++) {
        var b = all[i];
        if (b && b.isCollapsed && b.isCollapsed()) {
          b.setCollapsed(false);
          b.setCollapsed(true);
          /* AND WRITE THE LABEL OURSELVES, BECAUSE BLOCKLY'S IS TRUNCATED.
           *
           * Blockly summarises a collapsed block with toString(COLLAPSE_CHARS),
           * 30 characters, built by walking every field in order. For a
           * subroutine that reads "SUBROUTINE getAmpedConfi..." - the name is
           * the one thing worth showing and it is the thing that gets cut.
           *
           * The summary is an ordinary field, so it can simply be set. Naming
           * it after the subroutine and nothing else means 427 collapsed rows
           * read as a table of contents rather than a wall of "SUBROUTINE".
           */
          var label = collapsedLabelFor(b);
          if (label) {
            var cf = b.getField('_TEMP_COLLAPSED_FIELD');
            if (cf) { try { cf.setValue(label); } catch (e) {} }
          }
          n++;
        }
      }
    } catch (e) { UI.reportFault('naming the collapsed blocks', e); }
    if (n) { trace('load: renamed ' + n + ' collapsed block(s)'); }
    return n;
  }

  function loadWorkspaceIncremental(doc, onProgress, onDone, onFail) {
    var st, tops;
    try {
      st = BF6.unwrap(doc);
      tops = (st && st.blocks && st.blocks.blocks) || [];
    } catch (e) { onFail('reading the workspace', e); return; }

    var total = tops.length, done = 0;
    var B = window.Blockly;

    /* KEPT, SO SWITCHING FILES NEVER HAS TO RE-READ THE CANVAS.
     *
     * This is the document the project came from. Holding on to it means a
     * bookmark click is "take these items out of a list I already have" rather
     * than "serialise 55,463 blocks and pick through the result", which is what
     * made every tab click freeze the editor. */
    UI.doc = st;

    /* SAY SOMETHING BEFORE THE FIRST BATCH, NOT AFTER IT.
     *
     * The bar used to appear only once a batch had completed, so a load that
     * died inside batch one looked identical to one that never started. Both
     * showed nothing at all, which is the same mistake the freeze trail made
     * earlier today. The bar goes up first, and the log says how much work was
     * accepted, so "no bar" now means the message never arrived. */
    onProgress(0, total);
    trace('load: starting, ' + total + ' top level item(s) in batches of ' + BATCH);

    try { UI.ws.setResizesEnabled(false); } catch (e) {}
    try { B.Events.disable(); } catch (e) {}

    // A workspace message replaces the previous document. Appending left old
    // blocks and variables behind, especially on a second import.
    try { UI.ws.clear(); }
    catch (e) { finish(e, 'clearing the previous workspace'); return; }

    /* THE VARIABLES FIRST, BY THE ONE CALL THAT EXISTS IN THIS BUILD.
     *
     * This used to call Blockly.serialization.variables.load. That object is
     * present in this Blockly, so the guard testing for it passed, but it has
     * no load method: the call threw every time and an empty catch swallowed
     * it, on the reasoning that a workspace with no variables is normal. So
     * variables have NEVER been restored on this path.
     *
     * A block whose variable id is unknown still loads, because Blockly invents
     * one and gives it a generated single letter name. That is the whole
     * "Global Variable a" mystery: 254 real names replaced by letters, on every
     * load, silently.
     *
     * createVariable(name, type, id) is what the serializer itself calls and it
     * is on the workspace in every version. Failures are counted and reported
     * rather than hidden.
     */
    (function loadTheVariables() {
      var want = (st.variables && st.variables.length) || 0;
      if (!want) { return; }
      var failed = 0, firstErr = '';
      for (var vi = 0; vi < want; vi++) {
        var rec = st.variables[vi] || {};
        try {
          if (!UI.ws.getVariableById(rec.id)) {
            UI.ws.createVariable(rec.name, rec.type || '', rec.id);
          }
        } catch (e) {
          failed++;
          if (!firstErr) { firstErr = (e && e.message) ? e.message : String(e); }
        }
      }
      var got = 0;
      try { got = UI.ws.getAllVariables().length; } catch (e) {}
      var note = failed ? ('  ' + failed + ' refused, first: ' + firstErr) : '';
      trace('load: variables ' + got + ' of ' + want + ' in place' + note);
    }());
    function finish(err, stepName) {
      if (!err) { refreshCollapsedText(); }
      try { B.Events.enable(); } catch (e) {}
      try { UI.ws.setResizesEnabled(true); } catch (e) {}
      if (err) { onFail(stepName || 'loading the blocks onto the canvas', err); }
      else { onDone(total); }
    }

    function batch() {
      if (done >= total) { finish(null); return; }
      var end = Math.min(done + BATCH, total);
      try {
        for (; done < end; done++) {
          B.serialization.blocks.append(tops[done], UI.ws, { recordUndo: false });
        }
      } catch (e) {
        /* NAME THE PARENT, NOT JUST THE CHILD.
         *
         * Blockly's refusal says which block would not connect and what the two
         * connections wanted, but not what it was connecting TO. "Add expected
         * Number,Vector, found String" leaves the actual offender - the socket
         * holding it - unnamed, and finding it meant hunting the id through a
         * five megabyte file by hand.
         *
         * The id is in the message, and the block is on the canvas by now, so
         * the parent and the socket can simply be looked up and reported with
         * it. One line instead of a round trip. */
        /* LOOK IN THE JSON, NOT ON THE CANVAS.
         *
         * A block Blockly refused never got added, so getBlockById returns
         * nothing and the parent cannot be asked for. The first version of this
         * did exactly that and printed nothing, which read as the block having
         * no parent rather than as the lookup being impossible.
         *
         * The document being appended is right here and always has it. */
        var extra = '';
        try {
          var m = /id="([^"]+)"/.exec(String(e && e.message));
          if (m) {
            var want = m[1], hit = null;
            (function look(n, parent, socket) {
              if (hit || !n || typeof n !== 'object') { return; }
              if (n.id === want) { hit = { b: n, p: parent, s: socket }; return; }
              if (n.inputs) {
                for (var k in n.inputs) {
                  if (n.inputs[k] && n.inputs[k].block) { look(n.inputs[k].block, n, k); }
                }
              }
              if (n.next && n.next.block) { look(n.next.block, parent, socket); }
            }(tops[done], null, null));
            if (hit) {
              var where = hit.p ? (hit.p.type + '.' + hit.s) : '(top level)';
              extra = '  [' + hit.b.type + ' sits in ' + where + ']';
              trace('load: REFUSED ' + hit.b.type + ' (' + want + ') in ' + where +
                    ' - ' + (e && e.message));
            } else {
              trace('load: REFUSED id ' + want + ', not found in the item being added');
            }
          }
        } catch (e2) {}
        finish(e, 'loading block ' + (done + 1) + ' of ' + total + extra);
        return;
      }
      onProgress(done, total);
      /* One line per batch. Cheap at this granularity, and it means a load
       * that stops can be read off the log by where it stopped rather than
       * guessed at from a bar nobody was watching. */
      if (done === total || (done % (BATCH * 10)) === 0) {
        trace('load: ' + done + ' of ' + total + ' item(s)');
      }
      /* setTimeout rather than a microtask: a promise would run before the
       * browser gets a chance to paint, which is the whole point of yielding. */
      setTimeout(batch, 0);
    }
    batch();
  }

  /* THE SIDECAR: WHAT THE WORKSPACE ALONE DOES NOT SAY.
   *
   * An extended project is its Blockly workspace plus a strings table, and the
   * strings are not in the workspace: blocks reference them by key. Saving only
   * the blocks means reopening the project shows every message as a missing
   * key, and the handoff is explicit that the sidecar must not be discarded and
   * later reconstructed from generated TypeScript - by then the mapping is
   * gone.
   *
   * The field is only written when the project actually needs it, so a native
   * workspace saves byte for byte as it always did.
   */
  function attachExtendedSidecar(doc) {
    var X = window.BF6Extended, I = window.BF6ExtendedIntegration;
    if (!doc || !X || !I || !X.canExportNative) { return doc; }
    try {
      if (X.canExportNative(doc)) { return doc; }
      doc.bf6x = { version: 1, strings: (UI.project && UI.project.strings) || {} };
    } catch (e) { /* never lose a save over the sidecar */ }
    return doc;
  }

  function readExtendedSidecar(doc) {
    var side = doc && doc.bf6x;
    if (!UI.project) { UI.project = {}; }
    UI.project.strings = {};
    UI.project.extended = false;
    if (!side) { return; }
    UI.project.strings = side.strings || {};
    UI.project.extended = true;
    trace('extended project: restored ' +
          Object.keys(UI.project.strings).length + ' string(s) from the sidecar');
  }

  function loadWorkspaceDoc(doc, what) {
    if (UI.loadingDoc || UI.applying || BF6.state.loading) { status('Wait for the current workspace to finish opening before importing another.', true); return false; }
    UI.loadFailed = false;
    UI.openRegionAfterLoad = null;
    if (UI.settleTimer) { clearTimeout(UI.settleTimer); UI.settleTimer = null; }
    UI.region = null; UI.ruleMode = null; UI.ruleModeStash = null;
    if ($('rulemode')) $('rulemode').classList.remove('on');
    UI.loadingDoc = true;
    var step = 'reading the file';
    try {
      /* Before the definitions grow and before the canvas is rebuilt: the
       * strings belong to the project, not to the blocks being drawn. */
      readExtendedSidecar(doc);
      step = 'growing the block definitions for this workspace';
      growDefinitions(BF6.unwrap(doc));
      step = 'rebuilding the canvas';
      rebuildWorkspace(false);
    } catch (e) {
      UI.reportFault(step, e);
      status('could not load that workspace while ' + step + ': ' +
        ((e && e.message) || e), true);
      UI.loadingDoc = false;
      UI.loadFailed = true;
      return;
    }

    showLoadProgress(0, 0, what);
    loadWorkspaceIncremental(doc,
      function (n, total) { showLoadProgress(n, total, what); },
      function (total) {
        hideLoadProgress();
        send({ op: 'log', level: 'display',
               text: 'workspace loaded in batches: ' + total + ' top level item(s)' });
        finishWorkspaceDoc(doc, what);
      },
      function (stepName, e) {
        hideLoadProgress();
        UI.reportFault(stepName, e);
        status('could not load that workspace while ' + stepName + ': ' +
          ((e && e.message) || e), true);
        UI.loadingDoc = false;
        UI.loadFailed = true;
      });
  }

  /* Everything that used to follow the blocking load, now that it finishes
   * later than the call that started it. */
  function finishWorkspaceDoc(doc, what, onSettled) {
    showLoadProgress(1, 1, 'finishing block layout');
    UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));

    /* A BIG PROJECT DOES NOT OPEN WHOLE. IT OPENS ON ITS FIRST FILE.
     *
     * Blockly draws and hit tests every block whether or not it is on screen,
     * and it cannot virtualise, so 55,463 blocks on one canvas is slow to the
     * point of unusable no matter what else is tuned. Undead Ground Zero is
     * 439 top level items across 31 files; one file is about 1,800 blocks,
     * which is the size the site itself runs at comfortably.
     *
     * So a large import lands on the biggest file with the tab strip up, and
     * ALL FILES is one click away for anyone who wants the whole thing. The
     * work is all still there; it is just not all being drawn.
     */
    try {
      var count = UI.ws.getAllBlocks(false).length;
      if (!UI.region && count > 6000) {
        var files = new Set(docTops().map(fileOfDocTop).filter(function (f) {
          return f && f !== ' rules' && f !== '(no file)';
        }));
        var regions = buildRegions().filter(function (r) { return files.has(r.file); });
        regions.sort(function (a, b) { return b.blocks - a.blocks; });
        if (regions.length > 1) {
          trace('load: ' + count + ' blocks is too many to draw at once, opening ' +
                regions[0].label + ' (' + regions[0].blocks + ' blocks). ALL FILES shows everything.');
          UI.openRegionAfterLoad = regions[0].file;
        }
      }
    } catch (e) { UI.reportFault('choosing a file to open on', e); }
    // The floor is worked out the moment a project arrives, not only when FIT
    // is pressed: a creator who scrolls out by hand should not meet a wall that
    // the fit button would have moved for them.
    refreshZoomFloor();
    scheduleTextPictures();
    refreshNavigator(); drawMinimap(); refreshStale();

    // SHOW WHAT WAS JUST LOADED.
    //
    // The view kept whatever scroll and zoom it already had, so a workspace
    // whose blocks sit anywhere else arrived off screen. Loading a converted
    // project therefore looked exactly like loading nothing at all: 5,187
    // blocks present, a canvas 2448 by 1287, and an empty rectangle to look at.
    // Three real faults were found chasing that before anyone printed the
    // count.
    /* OPEN ON SOMETHING READABLE, NOT ON EVERYTHING AT ONCE.
     *
     * zoomToFit put a 7469 by 7883 board inside a 2216 wide viewport, which is
     * a scale of 0.16. Every block was then two or three pixels of dark grey on
     * a dark background: present, hit testable, and indistinguishable from an
     * empty canvas. Panning it looked like nothing moving, because a uniform
     * smear has nothing to move against.
     *
     * A person opening a project wants to start reading it, so the view goes to
     * the first thing in it at a size that can be read. Fit is still one click
     * away for anyone who wants the whole shape. */
    try {
      var tops0 = UI.ws.getTopBlocks(true);
      var firstTop = null, ti;
      for (ti = 0; ti < tops0.length; ti++) {
        if (tops0[ti].type === 'modBlock') { firstTop = tops0[ti]; break; }
      }
      if (!firstTop && tops0.length) { firstTop = tops0[0]; }
      if (firstTop) {
        UI.ws.setScale(0.8);
        UI.ws.centerOnBlock(firstTop.id);
      }
    } catch (e) { UI.reportFault('showing the start of the workspace', e); }

    /* CAN THIS WORKSPACE BE SCROLLED AT ALL?
     *
     * Asking the user to try a gesture only says whether the gesture worked. It
     * cannot separate an event that never arrives from a scroll call that does
     * nothing. So the page moves itself and reports whether the view followed. */
    try {
      var m0 = UI.ws.getMetrics();
      var movable = UI.ws.isMovable ? UI.ws.isMovable() : null;
      var bars = !!UI.ws.scrollbar;
      var hbar = !!(UI.ws.scrollbar && UI.ws.scrollbar.hScroll);
      /* The range Blockly will allow. scroll() clamps to this, so if it is
       * empty every scroll is a no-op no matter what asked for it. */
      var mm = UI.ws.getMetricsManager();
      var sm = mm.getScrollMetrics ? mm.getScrollMetrics(true) : null;
      var cm = mm.getContentMetrics ? mm.getContentMetrics(true) : null;
      send({ op: 'log', level: 'display', text: 'scroll range: ' +
        (sm ? Math.round(sm.left) + ',' + Math.round(sm.top) + ' ' +
             Math.round(sm.width) + 'x' + Math.round(sm.height) : 'none') +
        '; content metrics: ' +
        (cm ? Math.round(cm.left) + ',' + Math.round(cm.top) + ' ' +
             Math.round(cm.width) + 'x' + Math.round(cm.height) : 'none') +
        '; movableH=' + (UI.ws.isMovableHorizontally ? UI.ws.isMovableHorizontally() : '?') +
        ' movableV=' + (UI.ws.isMovableVertically ? UI.ws.isMovableVertically() : '?') });
      /* Measured in the same convention the pan gesture uses: scroll() takes a
       * canvas translation, so the test drives ws.scrollX/scrollY and checks
       * those, not metrics.scrollLeft. Reading the wrong pair here is what let
       * a broken pan report a plausible-looking number. */
      var t0x = UI.ws.scrollX, t0y = UI.ws.scrollY;
      UI.ws.scroll(t0x + 200, t0y + 120);
      var movedX = Math.round(UI.ws.scrollX - t0x);
      var movedY = Math.round(UI.ws.scrollY - t0y);
      /* Being already hard against an edge is not a failure, and saying so
       * stops a legitimately clamped workspace reading as a bug. */
      var clamped = (movedX !== 200 || movedY !== 120);
      send({ op: 'log', level: 'display', text: 'pan self test: movable=' + movable +
        ', scrollbar object=' + bars + ', horizontal bar=' + hbar +
        '; asked for +200,+120 and the canvas moved ' + movedX + ',' + movedY +
        (clamped ? ' (clamped at the scroll limit)' : '') +
        '; translation was ' + Math.round(t0x) + ',' + Math.round(t0y) +
        '; content ' + Math.round(m0.contentWidth) + 'x' + Math.round(m0.contentHeight) +
        ' in viewport ' + Math.round(m0.viewWidth) + 'x' + Math.round(m0.viewHeight) });
      UI.ws.scroll(t0x, t0y);
    } catch (e) { UI.reportFault('testing whether the view can scroll', e); }


    // WHAT THE VIEW ACTUALLY ENDED UP AS.
    //
    // A workspace can hold every block, on a canvas of the right size, and
    // still show nothing: parked off to one side, or fitted to a scale so small
    // the blocks are under a pixel across. Those two look identical to an empty
    // canvas and to each other, so the numbers go to the log rather than being
    // guessed at from outside.
    try {
      var vm = UI.ws.getMetrics ? UI.ws.getMetrics() : null;
      var bb = UI.ws.getBlocksBoundingBox ? UI.ws.getBlocksBoundingBox() : null;
      var idiv = UI.ws.getInjectionDiv ? UI.ws.getInjectionDiv() : null;
      var cs = (idiv && window.getComputedStyle) ? window.getComputedStyle(idiv) : null;
      send({ op: 'log', level: 'display', text: 'view: scale ' +
        (UI.ws.scale || 0).toFixed(4) +
        ', scroll ' + (vm ? Math.round(vm.scrollLeft) + ',' + Math.round(vm.scrollTop) : '?') +
        ', viewport ' + (vm ? Math.round(vm.viewWidth) + 'x' + Math.round(vm.viewHeight) : '?') +
        ', content ' + (bb ? Math.round(bb.left) + ',' + Math.round(bb.top) + ' ' +
          Math.round(bb.right - bb.left) + 'x' + Math.round(bb.bottom - bb.top) : '?') +
        ', div ' + (cs ? cs.display + '/' + cs.visibility + '/pe:' + cs.pointerEvents : '?') });
      /* WHAT IS ACTUALLY UNDER THE MOUSE IN THE MIDDLE OF THE CANVAS.
       *
       * Blocks present, canvas sized, view fitted, and still nothing to see or
       * click means something is sitting on top of it. Asking the document
       * which element is at that point names the culprit outright, instead of
       * guessing at panes one at a time. */
      var host = document.getElementById('canvas');
      if (host && document.elementFromPoint) {
        var r = host.getBoundingClientRect();
        var el = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
        var path = [];
        for (var p = el; p && path.length < 6; p = p.parentElement) {
          path.push(p.tagName.toLowerCase() +
            (p.id ? '#' + p.id : '') +
            (p.className && typeof p.className === 'string' && p.className
              ? '.' + p.className.split(/s+/).slice(0, 2).join('.') : ''));
        }
        send({ op: 'log', level: 'display', text: 'canvas rect ' +
          Math.round(r.width) + 'x' + Math.round(r.height) + ' at ' +
          Math.round(r.left) + ',' + Math.round(r.top) +
          '; topmost element at its centre: ' + path.join(' < ') });
      }
    } catch (e) { UI.reportFault('reporting the view', e); }

    // OPENING A FILE IS NOT EDITING IT.
    //
    // flush() sends the site every unit that differs from the last snapshot.
    // loadWorkspace takes that snapshot as it finishes, and then the cosmetics
    // run: text pictures alone redraw thousands of labels. The diff then sees
    // blocks that changed and queues each one as a change the site has not been
    // told about, so opening a workspace on a blank project showed dozens of
    // pending pushes that nobody made.
    //
    // The wrong number is the smaller half. Signed in, those would have been
    // written to a real experience. So the snapshot is retaken once everything
    // that touches a block has run, and the journal is cleared, because
    // anything waiting in it belonged to the workspace this one just replaced.
    /* AFTER THE COSMETICS, NOT BEFORE THEM.
     *
     * A zero delay was too early: text pictures are scheduled separately and
     * redraw hundreds of labels a second later, so the snapshot was taken
     * before the blocks stopped changing and the diff pushed them all to the
     * site anyway. The settle waits long enough for every deferred pass to
     * finish, and nothing is sent until it has. */
    if (UI.settleTimer) { clearTimeout(UI.settleTimer); }
    UI.settleTimer = setTimeout(function () {
      UI.settleTimer = null;
      try {
        BF6.snapshotUnits(UI.ws);
        UI.lastVars = JSON.stringify(BF6.variableList(UI.ws));
        UI.journal = {};
        paintPending();
      } catch (e) { UI.reportFault('settling after a load', e); }
      finally { UI.loadingDoc = false; hideLoadProgress(); }
      if (onSettled) onSettled();
      if (UI.openRegionAfterLoad) {
        var file = UI.openRegionAfterLoad; UI.openRegionAfterLoad = null;
        enterRegion(file);
      }
    }, 2500);

    var loaded = UI.ws.getAllBlocks(false).length;
    var topCount = 0;
    try { topCount = UI.ws.getTopBlocks(false).length; } catch (e) {}
    status(what + ': ' + loaded + ' blocks, ' +
      BF6.variableList(UI.ws).length + ' variables');
    /* SAY IT OUTSIDE THE PAGE TOO.
     *
     * The status strip is the only place this was ever reported, so a load that
     * built the blocks and drew none of them looked exactly like a load that
     * built nothing. The tool cannot see the strip; it can see the log. */
    send({ op: 'log', level: 'display', text: 'workspace loaded: ' + loaded +
      ' block(s), ' + topCount + ' top level, ' + BF6.variableList(UI.ws).length +
      ' variable(s); canvas ' + (UI.ws && UI.ws.getParentSvg && UI.ws.getParentSvg() ?
        (UI.ws.getParentSvg().clientWidth + 'x' + UI.ws.getParentSvg().clientHeight) :
        'size unknown') });
  }

  // The TypeScript compiler API, which the converter cannot parse a script
  // export without. It is not shipped with the tool: it is 9 MB and every
  // Portal script project already has one, so the tool finds it and hands the
  // page its path.
  function ensureTypeScript(cb) {
    if (window.ts && window.ts.createSourceFile) { cb(window.ts); return; }
    var url = UI.typescriptUrl;
    if (!url) { cb(null); return; }
    if (UI.tsLoading) { UI.tsWaiting.push(cb); return; }
    UI.tsLoading = true;
    UI.tsWaiting = [cb];
    var tag = document.createElement('script');
    tag.src = url;
    tag.onload = function () {
      UI.tsLoading = false;
      var ts = window.ts && window.ts.createSourceFile ? window.ts : null;
      UI.tsWaiting.forEach(function (f) { f(ts); });
      UI.tsWaiting = [];
    };
    tag.onerror = function () {
      UI.tsLoading = false;
      UI.tsWaiting.forEach(function (f) { f(null); });
      UI.tsWaiting = [];
    };
    document.head.appendChild(tag);
    status('loading the TypeScript compiler from ' + url);
  }

  function importScriptExport(text, name) {
    var Convert = window.BF6Convert;
    if (!Convert) {
      status('the converter is not on this page: convert.js did not load', true);
      return;
    }
    ensureTypeScript(function (ts) {
      if (!ts) {
        status('a script export needs the TypeScript compiler. Set [BF6UnrealSDK] TypeScriptLib ' +
          'to a typescript.js, or install the Portal script template.', true);
        return;
      }
      Convert.setTypeScript(ts);
      if (UI.convertCatalog) Convert.setCatalog(UI.convertCatalog);
      if (UI.convertEvents) Convert.setEvents(UI.convertEvents);
      var sources = {};
      sources[name || 'export.ts'] = text;
      var res = null;
      try { res = Convert.tsToBlocks(sources, {}); }
      catch (e) { status('the converter refused that script: ' + (e.message || e), true); return; }
      if (!acceptNativeImport(res, name)) return;
      loadWorkspaceDoc(res.workspace, 'imported ' + (name || 'a script export'));
      showConvertReport(res.report, name);
    });
  }

  // A SCRIPT PROJECT IS ELEVEN FILES, NOT ONE.
  //
  // importScriptExport takes a single file because that is what the site's
  // own export button hands you. Ours writes a project - rules split by
  // family, subroutines, variables, a runtime and an index - and a converter
  // shown one file of that set would resolve none of the cross references.
  // tsToBlocks already takes a map of name to text, so the whole folder goes
  // in at once.
  function importScriptFolder(files, dir) {
    var Convert = window.BF6Convert;
    var names = Object.keys(files || {});
    if (!Convert) { status('the converter is not on this page: convert.js did not load', true); return; }
    if (!names.length) { status('that folder held no TypeScript', true); return; }
    ensureTypeScript(function (ts) {
      if (!ts) {
        status('reading a script needs the TypeScript compiler. Install the Portal script ' +
          'template, or set [BF6UnrealSDK] TypeScriptLib to a typescript.js.', true);
        return;
      }
      Convert.setTypeScript(ts);
      if (UI.convertCatalog) Convert.setCatalog(UI.convertCatalog);
      if (UI.convertEvents) Convert.setEvents(UI.convertEvents);
      var res = null;
      try { res = Convert.tsToBlocks(files, {}); }
      catch (e) { status('the converter refused that project: ' + (e.message || e), true); return; }
      if (!acceptNativeImport(res, dir)) return;
      /* WHAT THE CONVERTER ACTUALLY PRODUCED, EVERY TIME.
       *
       * Packing folds many variables into pack0/pack1 and makes a project
       * unreadable. It is meant to be off for reading now, and saying so on
       * every import is the only way to tell a converter that did not pack from
       * a page still running an older copy of it. */
      try {
        var vlist = (res.workspace && res.workspace.mod && res.workspace.mod.variables) || [];
        var gen = vlist.filter(function (v) { return /^(pack|list|frame)d/.test(v.name || ''); });
        trace('import: converter ' + ((window.BF6Convert && window.BF6Convert.VERSION) || '?') +
              ', ' + vlist.length + ' variable(s), ' + gen.length + ' still generic' +
              (gen.length ? '  e.g. ' + gen.slice(0, 5).map(function (v) { return v.name; }).join(', ') : ''));
      } catch (e) {}
      loadWorkspaceDoc(res.workspace, 'imported ' + names.length + ' file(s) from ' + (dir || 'a folder'));
      /* THE CONVERTER ALREADY PLACED THESE, ONE COLUMN PER SOURCE FILE.
       *
       * arrangeByFamily re-lays the top blocks out by rule family, which is the
       * right thing for a workspace that arrived without positions. A converted
       * project is not one: irToBlocks gives every subroutine an x and y that
       * put it in its own file column, and running the family pass over that
       * threw all 31 columns into one lane and undid the grouping the author
       * wrote the project with. */
      if (!hasAuthoredLayout(res.workspace)) { arrangeByFamily(); }
      showConvertReport(res.report, names.length + ' file(s)');
    });
  }

  // LAID OUT THE WAY IT WAS WRITTEN OUT.
  //
  // The converter groups rules into families on the way out - players, AI,
  // objectives, vehicles, round flow, UI - and a round trip that came back as
  // one long column would throw that away. The same grouping is used to place
  // the blocks: one column per family, in the converter's own order, rules
  // down the column in the order they were read.
  //
  // Blockly's own cleanUp puts everything in a single column and knows nothing
  // about any of this, so it is deliberately not used here.
  /* Did whoever produced this workspace mean the positions in it? A converted
   * project carries deliberate ones; a hand assembled snippet usually does not. */
  function hasAuthoredLayout(doc) {
    try {
      var tops = doc.mod.blocks.blocks, placed = 0, i;
      for (i = 0; i < tops.length; i++) {
        if (tops[i].x || tops[i].y) { placed++; }
      }
      return placed > 1;
    } catch (e) { return false; }
  }

  // WINDOW.CONFIRM DOES NOT WORK HERE, AND FAILS AS A NO.
  //
  // This page runs in the editor's embedded browser, which has no dialog
  // handler, so window.confirm returns a falsy value without ever showing
  // anything. Every guard written as `if (!confirm(...)) return;` therefore took
  // the cancel path every time: Reset workspace looked dead because it was
  // asking a question nobody could answer.
  //
  // The button asks for itself instead. First click arms it and says what will
  // happen, second click within a few seconds does it, and it disarms on its own
  // so a forgotten armed button cannot fire later by accident.
  var ARMED = {};
  function armButton(btn, prompt, run) {
    var id = btn.id || String(Math.random());
    if (ARMED[id]) {
      clearTimeout(ARMED[id].timer);
      btn.textContent = ARMED[id].label;
      delete ARMED[id];
      run();
      return;
    }
    var label = btn.textContent;
    btn.textContent = 'Click again to confirm';
    status(prompt);
    ARMED[id] = {
      label: label,
      timer: setTimeout(function () {
        btn.textContent = label;
        delete ARMED[id];
        status('cancelled, nothing was changed');
      }, 5000)
    };
  }

  // Host-driven opens must wait for an in-flight import, never drop the new
  // workspace or clear the old one through user-edit events. Maps in the same
  // experience share one identity and retain their current edits and viewport.
  function queueProjectWorkspace(msg) {
    UI.pendingProject = msg;
    if (UI.projectOpenTimer) clearTimeout(UI.projectOpenTimer);
    if (UI.debounce) { clearTimeout(UI.debounce); UI.debounce = null; }
    function openWhenReady() {
      UI.projectOpenTimer = null;
      if (UI.loadingDoc || UI.applying || BF6.state.loading) {
        UI.projectOpenTimer = setTimeout(openWhenReady, 50); return;
      }
      var next = UI.pendingProject;
      if (!next) return;
      if (UI.hostProject === next.project && UI.hostRevision === next.revision) {
        UI.pendingProject = null; scheduleAutosave(); return;
      }
      UI.projectWorkspaces = UI.projectWorkspaces || new Map();
      if (UI.hostProject && !UI.loadFailed) {
        var previous = { revision: UI.hostRevision, json: projectDocument() };
        UI.projectWorkspaces.set(UI.hostProject, previous);
        send({op:'projectRecovery', project:UI.hostProject, revision:UI.hostRevision, json:previous.json});
        // Recovery survives reloading this browser tab; failure to store never
        // prevents the in-memory copy from keeping the creator's edits.
        try { sessionStorage.setItem('bf6-project:' + UI.hostProject, JSON.stringify(previous)); } catch (e) {}
      }
      var cached = UI.projectWorkspaces.get(next.project);
      if (!cached) {
        try { cached = JSON.parse(sessionStorage.getItem('bf6-project:' + next.project) || 'null'); } catch (e) {}
      }
      UI.pendingProject = null;
      UI.hostProject = next.project; UI.hostRevision = next.revision;
      UI.journal = {};
      if (UI.autosaveTimer) { clearTimeout(UI.autosaveTimer); UI.autosaveTimer = null; }
      if ((!cached || cached.revision !== next.revision) && next.recovery) cached = next.recovery;
      loadWorkspaceDoc(cached && cached.revision === next.revision ? cached.json : next.json,
        'opened ' + (next.name || 'this project'));
    }
    openWhenReady();
  }

  // The editor opened a different map, so the rules on the canvas belong to
  // the project that was open before. They are cleared through the workspace's
  // own clear, which is undoable, and the status line says so rather than the
  // blocks just vanishing.
  function onMapChanged(mapName) {
    var n = 0;
    try { n = UI.ws.getAllBlocks(false).length; } catch (e) {}
    UI.doc = null; UI.region = null; UI.ruleMode = null; UI.ruleModeStash = null;
    if (UI.project) { UI.project.strings = {}; UI.project.extended = false; }
    if ($('rulemode')) $('rulemode').classList.remove('on');
    if (!n) { return; }
    try { UI.ws.clear(); } catch (e) { return; }
    try { drawMinimap(); } catch (e) {}
    status('a different map is open' + (mapName ? ' (' + mapName + ')' : '') +
      ', so the ' + n + ' block(s) from the last project were cleared. Undo brings them back.');
  }

  function arrangeByFamily() {
    if (!UI.ws || !window.BF6Convert) return 0;
    var Convert = window.BF6Convert;
    var tops = [];
    try { tops = UI.ws.getTopBlocks(false); } catch (e) { return 0; }
    if (!tops.length) return 0;

    var order = (Convert.families || []).map(function (f) { return f[0]; });
    var lanes = {};
    tops.forEach(function (b) {
      var fam = 'other';
      try {
        if (b.type === 'ruleBlock') {
          fam = Convert.familyOf(b.getFieldValue('EVENTTYPE') || '') || 'other';
        } else if (b.type === 'subroutineBlock') {
          fam = 'subroutines';
        } else {
          fam = 'loose';
        }
      } catch (e) {}
      (lanes[fam] = lanes[fam] || []).push(b);
    });

    // Families in the converter's order, then the two lanes it has no family
    // for, so a column never appears in a different place between imports.
    var columns = order.filter(function (f) { return lanes[f]; });
    ['subroutines', 'loose'].forEach(function (f) { if (lanes[f]) columns.push(f); });

    var GAP_X = 80, GAP_Y = 48, x = 0, moved = 0;
    UI.ws.setResizesEnabled(false);
    try {
      columns.forEach(function (fam) {
        var y = 0, widest = 0;
        lanes[fam].forEach(function (b) {
          var at = null, hw = null;
          try { at = b.getRelativeToSurfaceXY(); hw = b.getHeightWidth(); } catch (e) { return; }
          if (!at || !hw) return;
          try { b.moveBy(x - at.x, y - at.y); moved++; } catch (e) { return; }
          y += hw.height + GAP_Y;
          if (hw.width > widest) widest = hw.width;
        });
        x += widest + GAP_X;
      });
    } finally {
      UI.ws.setResizesEnabled(true);
    }
    try { drawMinimap(); } catch (e) {}
    status('laid out ' + moved + ' stack(s) in ' + columns.length + ' family column(s)');
    return moved;
  }
  UI.arrangeByFamily = arrangeByFamily;

  // Nothing is dropped in silence: whatever the converter could not represent
  // is listed, with what it suggests instead.
  function acceptNativeImport(result, name) {
    var problems = window.BF6Convert.nativeImportProblems(result && result.report);
    if (!problems.length) return true;
    showConvertReport(result.report || {}, name, true);
    status('Conversion stopped. ' + problems.join(' ') + ' Keep this project in the TypeScript editor. Your current blocks were preserved.', true);
    return false;
  }

  function showConvertReport(report, name, stopped) {
    var box = $('portal');
    if (!box) return;
    box.innerHTML = '';
    var bad = (report.unconvertible || []).length;
    var problems = window.BF6Convert.nativeImportProblems(report);
    box.className = bad || problems.length ? 'portal on' : 'portal on good';
    box.appendChild(el('div', 'portal-head', stopped ? 'CONVERSION STOPPED: KEEP THIS PROJECT IN TYPESCRIPT' : bad
      ? 'IMPORTED, WITH ' + bad + ' THING' + (bad === 1 ? '' : 'S') + ' THE BLOCKS CANNOT SAY'
      : 'IMPORTED: EVERYTHING IN THAT SCRIPT BECAME BLOCKS'));
    problems.forEach(function (problem) { box.appendChild(el('div', 'portal-text', problem)); });
    if (name) box.appendChild(el('div', 'portal-msg', name));
    (report.unconvertible || []).forEach(function (u) {
      var row = el('div', 'portal-row');
      row.appendChild(el('div', 'portal-block',
        (u.file || '') + (u.line ? ':' + u.line : '') + '  ' + (u.construct || '')));
      if (u.suggestion) row.appendChild(el('div', 'portal-text', u.suggestion));
      box.appendChild(row);
    });
    if ((report.warnings || []).length) {
      box.appendChild(el('div', 'portal-head2', 'WARNINGS'));
      report.warnings.forEach(function (w) { box.appendChild(el('div', 'portal-text', w)); });
    }
  }

  // A full experience: the blocks come in, and everything else it carried is
  // listed with one button that brings it into the tool.
  function importExperience(doc, name) {
    var read = BF6.readExperience(doc);
    UI.shell = read.shell;
    UI.shellName = name || 'an imported experience';
    UI.pendingAttachments = read.attachments.map(function (a, i) {
      var slot = 0;
      var m = /mapIdx=(\d+)/.exec(String(a.metadata || ''));
      if (m) slot = parseInt(m[1], 10);
      return { filename: a.filename, attachmentType: a.attachmentType, kind: a.kind,
        text: a.text, bytes: a.bytes, mapIdx: slot };
    });
    loadWorkspaceDoc(read.workspace, 'imported the workspace from ' + (name || 'an experience'));

    var box = $('portal');
    if (!box) return;
    box.innerHTML = '';
    box.className = 'portal on good';
    box.appendChild(el('div', 'portal-head', 'IMPORTED ' + (read.shell.name || name || 'AN EXPERIENCE')));
    if (read.shell.description) box.appendChild(el('div', 'portal-msg', read.shell.description));
    box.appendChild(el('div', 'portal-msg', 'Game mode ' + read.shell.gameMode +
      ', ' + Object.keys(read.shell.mutators || {}).length + ' mutator(s), ' +
      Object.keys(read.shell.assetRestrictions || {}).length + ' asset restriction(s). ' +
      'A full export from here will carry all of it.'));
    box.appendChild(el('div', 'portal-head2', 'MAP ROTATION'));
    read.mapRotation.forEach(function (r) {
      var row = el('div', 'portal-row');
      row.appendChild(el('div', 'portal-block', r.id));
      row.appendChild(el('div', 'portal-text',
        r.spatialFilename ? r.spatialFilename : 'no map data attached to this slot'));
      box.appendChild(row);
    });
    box.appendChild(el('div', 'portal-head2', 'ATTACHMENTS'));
    read.attachments.forEach(function (a) {
      var row = el('div', 'portal-row');
      row.appendChild(el('div', 'portal-block', a.filename + '  (' + a.kind + ', type ' + a.attachmentType + ')'));
      row.appendChild(el('div', 'portal-text', a.text.length + ' characters'));
      box.appendChild(row);
    });
    var take = el('button', 'primary', 'Bring these into the tool');
    take.onclick = function () {
      send({ op: 'importAttachments', attachments: UI.pendingAttachments });
      status('handing ' + UI.pendingAttachments.length + ' attachment(s) to the tool');
    };
    box.appendChild(take);
  }

  // The one door every file comes through.
  function importText(text, name) {
    var d = BF6.detectFormat(text);
    switch (d.format) {
      case 'workspace':
        loadWorkspaceDoc(d.doc, 'imported ' + (name || 'a workspace'));
        break;
      case 'experience':
        importExperience(d.doc, name);
        break;
      case 'typescript':
        importScriptExport(text, name);
        break;
      case 'spatial':
        UI.pendingAttachments = [{ filename: name || 'imported.spatial.json',
          attachmentType: 1, kind: 'spatial', text: text, mapIdx: 0 }];
        send({ op: 'importAttachments', attachments: UI.pendingAttachments });
        status('that is map data, not blocks: handing it to the tool as a save');
        break;
      case 'strings':
        UI.pendingAttachments = [{ filename: name || 'strings.json',
          attachmentType: 4, kind: 'strings', text: text, mapIdx: 0 }];
        send({ op: 'importAttachments', attachments: UI.pendingAttachments });
        status('that is a strings bundle, not blocks: handing it to the script editor');
        break;
      default:
        status('that file is not one the editor can read. It looks like ' + d.looksLike, true);
        break;
    }
  }
  UI.importText = importText;

  // ---- export --------------------------------------------------------------
  /* THE CANVAS IS FOR READING. THE COPY THAT LEAVES IS FOR PORTAL.
   *
   * These are two different jobs and they used to be one switch. Portal counts
   * variables and refuses an upload past 128 Global and 128 shared between
   * Player and Team, so a large project has to fold most of its names into
   * shared arrays: zombieHealth stops being a name and becomes
   * ValueInArray(pack1, 7). Doing that on import made every converted project
   * unreadable; not doing it at all made every large one unuploadable.
   *
   * So it happens here, on the way out, and only here. The workspace you are
   * looking at keeps every real name. The copy handed to the site is folded
   * down, quietest variables first, and only as far as the budget forces, so
   * the names that survive are the ones actually read. Nothing below touches
   * the open workspace.
   */
  function packForSite(doc) {
    var Convert = window.BF6Convert;
    if (!Convert || !Convert.packForPortal) { return { doc: doc, note: '' }; }
    var out = null;
    try { out = Convert.packForPortal(doc, {}); }
    catch (e) {
      /* Never block an export on this. An unpacked upload that Portal argues
       * with beats no upload at all, and the message says which happened. */
      trace('packing for the site failed, sending the workspace as it stands: ' + (e.message || e));
      return { doc: doc, note: 'sent unpacked: ' + (e.message || e) };
    }
    if (!out || !out.json) { return { doc: doc, note: '' }; }

    var note = 'packed for Portal: Global ' + out.before.Global + ' to ' + out.after.Global +
               ', Player and Team ' + out.before.Object + ' to ' + out.after.Object;
    if (out.fits) {
      note += '. Within the Portal budget.';
    } else {
      /* SAY WHAT IS IN THE WAY, BY NAME.
       *
       * A workspace the site will refuse is worth more as the list of
       * variables responsible than as a number, because the fix is in the
       * source and the user is the only one who can make it. */
      note += '. Still ' + (out.after.Global - out.ceiling) + ' over the Global limit of ' +
              out.ceiling + '. ' + out.blockers.length + ' variables cannot be folded: ' +
              'arrays, because appending an array to an array concatenates it rather ' +
              'than nesting, and loop counters, because the For block binds a variable ' +
              'and not an array slot. First few: ' + out.blockers.slice(0, 8).join(', ') + '.';
    }
    return { doc: out.json, note: note, fits: out.fits, blockers: out.blockers };
  }

  function buildExport(format, forSite) {
    var doc = projectDocument();
    var packNote = '';
    if (forSite && !nativeRouteAllowed('packing for Portal')) {
      throw new Error('extended blocks cannot be packed as native blocks');
    }
    if (forSite) {
      var packed = packForSite(doc);
      doc = packed.doc;
      packNote = packed.note;
      if (packNote) { status(packNote, packed.fits === false); }
    }
    if (format !== 'experience') {
      return { json: doc, summary: null, suggested: 'workspace.json', packNote: packNote };
    }
    var exp = UI.experience || {};
    var maps = null;
    if (exp.maps && exp.maps.length) {
      maps = exp.maps.filter(function (m) { return m.spatialText; }).map(function (m) {
        return { map: m.map, index: m.index, spatialText: m.spatialText,
          spatialFilename: m.spatialFilename };
      });
      if (!maps.length) maps = null;
    }
    var built = BF6.writeExperience({
      workspace: doc,
      shell: UI.shell,
      name: exp.name || undefined,
      description: exp.description || undefined,
      maps: maps
    });
    var base = (built.doc.name || 'experience').replace(/[^A-Za-z0-9_.-]+/g, '_');
    return { json: built.doc, summary: built.summary, suggested: base + '.json',
             packNote: packNote };
  }

  // BLOCKS OUT AS A SCRIPT PROJECT.
  //
  // The converter does the work and it is the same converter the tests drive,
  // so what lands on disk is what blocks2ts on the command line produces: one
  // file per family of rules, plus subroutines, variables, a runtime and an
  // index. The tool only chooses the folder and writes what it is handed.
  /* AN EXTENDED WORKSPACE HAS ITS OWN COMPILER, AND MUST USE IT.
   *
   * The native lowering turns blocks into Portal's own vocabulary, where there
   * are no locals, so a workspace built out of extended blocks cannot go
   * through it: at best the blocks are unknown, at worst they are reinterpreted
   * into something that is not the author's program. BF6Extended.compile emits
   * ordinary TypeScript instead, where a local is a real local, which is the
   * whole reason the extended language exists.
   *
   * One button, because from the author's side it is one intention: turn this
   * into a script project. Which compiler runs is ours to work out.
   */
  function exportExtendedAsScript(dir, sourceOnly) {
    var X = window.BF6Extended, I = window.BF6ExtendedIntegration;
    var doc = projectDocument();
    var res;
    try {
      res = X.compile(doc, {
        catalog: UI.convertCatalog,
        events: UI.convertEvents,
        legacy: window.BF6Convert
      });
    } catch (e) {
      status('the extended compiler failed: ' + (e.message || e), true);
      return;
    }

    /* A STRUCTURAL PREVIEW IS NOT A BUILD.
     *
     * Diagnostics stop the export outright rather than producing files that
     * look finished. Each one can name the block responsible, so the first is
     * selected and centred: reading "argument 2 has the wrong type" is far less
     * use than being shown where. */
    if (!res || !res.ok) {
      var ds = (res && res.diagnostics) || [];
      status(ds.length
        ? ('cannot build: ' + ds.length + ' problem(s). ' + (ds[0].message || ds[0]))
        : 'cannot build: the compiler reported no output', true);
      ds.slice(0, 40).forEach(function (d) {
        trace('diagnostic: ' + (d.message || d) + (d.blockId ? '  [' + d.blockId + ']' : ''));
      });
      var first = ds.find(function (d) { return d && d.blockId; });
      if (first) {
        try { pushBack(first.blockId); centreOn(first.blockId); UI.ws.getBlockById(first.blockId).select(); }
        catch (e) {}
      }
      return;
    }

    var files = res.files || {};
    var names = Object.keys(files);
    if (!names.length) { status('the extended compiler produced no files', true); return; }
    status('writing ' + names.length + ' file(s): ' + names.join(', '));
    /* Nested paths now survive the bridge, so src/index.ts lands in src/. */
    send({ op: sourceOnly ? 'exportScript' : 'exportPortalScript', files: files, dir: dir || '' });
  }

  /* Convert a file from the Script editor and paste it onto this canvas,
   * centring whatever the user actually clicked. */
  function showSourceAsBlocks(source, wanted, file) {
    var Convert = window.BF6Convert;
    if (!Convert) { status('the converter did not load, so blocks cannot be shown', true); return; }
    if (!source) { status('the Script editor sent no source to show', true); return; }
    if (UI.convertCatalog) Convert.setCatalog(UI.convertCatalog);
    if (UI.convertEvents) Convert.setEvents(UI.convertEvents);

    var res;
    var files = {};
    files[file] = source;
    try { res = Convert.tsToBlocks(files, {}); }
    catch (e) { status('that file could not be shown as blocks: ' + (e.message || e), true); return; }
    if (!acceptNativeImport(res, file)) return;

    var ws = res && (res.workspace || res.json);
    if (!ws) { status('that file produced no blocks', true); return; }

    pasteSnippet(ws, wanted ? ('Show as blocks: ' + wanted) : ('Show as blocks: ' + file));

    /* Land on the thing they clicked. The converter names a rule after its
     * handler and a subroutine after its function, so the name is the link. */
    if (wanted) {
      var hit = null;
      try {
        hit = UI.ws.getAllBlocks(false).filter(function (b) {
          var n = (b.getFieldValue && (b.getFieldValue('NAME') ||
                   b.getFieldValue('SUBROUTINE_NAME'))) || '';
          return String(n).replace(/\s+/g, '').toLowerCase() ===
                 String(wanted).replace(/[_\s]+/g, '').toLowerCase();
        })[0] || null;
      } catch (e) {}
      if (hit) { pushBack(hit.id); centreOn(hit.id); status('showing ' + wanted + ' as blocks'); }
      else { status('showed the file as blocks; could not find ' + wanted + ' by name'); }
    }
    if (res.report) { showConvertReport(res.report, 'show as blocks'); }
  }

  /* WHAT THE MOD ACTUALLY PRINTED WHILE IT RAN.
   *
   * The only view of a running mod that is not the mod's own on-screen UI.
   * After a Host Locally session this is the first thing worth reading, and
   * until now it lived four folders deep in a temp directory under a name
   * nobody would guess.
   *
   * Kept to the last 500 rows: a long match writes thousands and the panel is
   * for reading the recent ones, not for archiving.
   */
  function onGameLog(msg) {
    var box = $('gamelog');
    if (!box) { return; }
    box.hidden = false;

    if (msg.replace) {
      box.innerHTML = '';
      if (!msg.found) {
        var none = document.createElement('div');
        none.className = 'system';
        none.textContent = msg.why || 'no log yet';
        box.appendChild(none);
        status(msg.why || 'no game log yet', true);
        return;
      }
      status('read ' + (msg.entries || []).length + ' line(s) from ' + shortFile(msg.path || ''));
    }

    if (msg.newSession) {
      var mark = document.createElement('div');
      mark.className = 'newsession';
      mark.textContent = 'the mod restarted, this is a new session';
      box.appendChild(mark);
    }

    (msg.entries || []).forEach(function (e) {
      var row = document.createElement('div');
      row.className = e.kind === 'error' ? 'error' : (e.kind === 'system' ? 'system' : '');
      if (e.at) {
        var t = document.createElement('span');
        t.className = 'at';
        /* The file stamps UTC; only the time is useful while reading along. */
        t.textContent = String(e.at).slice(11) || String(e.at);
        row.appendChild(t);
      }
      row.appendChild(document.createTextNode(e.text || ''));
      box.appendChild(row);
    });

    while (box.childNodes.length > 500) { box.removeChild(box.firstChild); }
    /* Only follow the tail when the reader is already at the bottom, so
     * scrolling back to read something does not get yanked away. */
    if (box.scrollTop + box.clientHeight >= box.scrollHeight - 40) {
      box.scrollTop = box.scrollHeight;
    }
  }

  function exportAsScript(dir, sourceOnly) {
    if (UI.loadingDoc || UI.applying || BF6.state.loading) { status('Wait for the workspace to finish opening before exporting.', true); return; }
    if (UI.loadFailed) { status('The workspace did not finish loading. Reopen it before exporting.', true); return; }
    if (typeof dir !== 'string') dir = '';
    /* Extended first: a workspace holding extended blocks has exactly one
     * correct route and it is not the native one. */
    var X = window.BF6Extended, I = window.BF6ExtendedIntegration;
    if (X && I && X.canExportNative && I.snapshot) {
      var isExtended = false;
      try {
        isExtended = !X.canExportNative(
          projectDocument());
      } catch (e) { status('Cannot inspect this workspace: ' + (e.message || e), true); return; }
      if (isExtended) { return exportExtendedAsScript(dir, sourceOnly); }
    }

    var Convert = window.BF6Convert;
    if (!Convert) { status('the converter is not on this page: convert.js did not load', true); return; }
    if (UI.convertCatalog) Convert.setCatalog(UI.convertCatalog);
    if (UI.convertEvents) Convert.setEvents(UI.convertEvents);
    // The converter reads the SAVED document, the same shape the workspace
    // export writes and the same thing the command line feeds it - not a live
    // Blockly workspace, which it would find empty.
    var doc = projectDocument();
    var res = null;
    try { res = Convert.blocksToTs(doc, {}); }
    catch (e) { status('the converter could not write that script: ' + (e.message || e), true); return; }
    if (res && res.report && res.report.unconvertible && res.report.unconvertible.length) {
      showConvertReport(res.report, 'script export');
      status('Export stopped: ' + res.report.unconvertible.length + ' block construct(s) could not be converted. Fix the reported blocks before exporting.', true);
      return;
    }
    var files = (res && res.files) || null;
    if (!files || !Object.keys(files).length) {
      status('the script export produced no files', true);
      return;
    }
    status('writing ' + Object.keys(files).length + ' file(s)');
    send({ op: sourceOnly ? 'exportScript' : 'exportPortalScript', files: files, dir: dir || '' });
    if (res.report) showConvertReport(res.report, 'script export');
  }
  UI.exportAsScript = exportAsScript;

  function doExport(format, path) {
    var out;
    try { out = buildExport(format); }
    catch (e) { status('Export stopped: ' + (e.message || e), true); return; }
    send({ op: 'exportFile', format: format || 'workspace', path: path || '',
      suggested: out.suggested, json: out.json });
    if (out.summary) showExportSummary(out.summary);
  }

  // Every field the export could not fill honestly is named, with where the
  // ones it did fill came from.
  function showExportSummary(sum) {
    var box = $('portal');
    if (!box) return;
    box.innerHTML = '';
    box.className = sum.defaulted.length ? 'portal on' : 'portal on good';
    box.appendChild(el('div', 'portal-head', sum.defaulted.length
      ? 'EXPORTED, WITH ' + sum.defaulted.length + ' FIELD' +
        (sum.defaulted.length === 1 ? '' : 'S') + ' AT THE SITE\'S DEFAULT'
      : 'EXPORTED: EVERY FIELD FILLED'));
    box.appendChild(el('div', 'portal-head2', 'WHERE EACH FIELD CAME FROM'));
    Object.keys(sum.sources).forEach(function (k) {
      var row = el('div', 'portal-row');
      row.appendChild(el('div', 'portal-block', k));
      row.appendChild(el('div', 'portal-text', sum.sources[k]));
      box.appendChild(row);
    });
    if (sum.attachments.length) {
      box.appendChild(el('div', 'portal-head2', 'ATTACHMENTS WRITTEN'));
      sum.attachments.forEach(function (a) {
        box.appendChild(el('div', 'portal-text',
          a.filename + '  (' + a.kind + ', type ' + a.attachmentType + ', ' + a.bytes + ' base64 bytes)'));
      });
    }
    if (sum.maps.length) {
      box.appendChild(el('div', 'portal-head2', 'MAP ROTATION'));
      sum.maps.forEach(function (m) { box.appendChild(el('div', 'portal-text', m)); });
    }
  }

  // ---- a file dropped on the board ----------------------------------------
  function wireDropTarget() {
    var zone = $('centre');
    if (!zone) return;
    ['dragenter', 'dragover'].forEach(function (ev) {
      zone.addEventListener(ev, function (e) {
        e.preventDefault(); e.stopPropagation();
        try { e.dataTransfer.dropEffect = 'copy'; } catch (x) {}
        zone.classList.add('dropping');
      });
    });
    ['dragleave', 'drop'].forEach(function (ev) {
      zone.addEventListener(ev, function (e) {
        e.preventDefault(); e.stopPropagation();
        zone.classList.remove('dropping');
      });
    });
    zone.addEventListener('drop', function (e) {
      var files = (e.dataTransfer && e.dataTransfer.files) || [];
      if (!files.length) {
        var text = e.dataTransfer && e.dataTransfer.getData('text');
        if (text) importText(text, 'dropped text');
        return;
      }
      var file = files[0];
      status('reading ' + file.name);
      var reader = new FileReader();
      reader.onload = function () { importText(String(reader.result || ''), file.name); };
      reader.onerror = function () { status('could not read ' + file.name, true); };
      reader.readAsText(file);
    });
  }

  // ---- chrome -------------------------------------------------------------
  // ==========================================================================
  // ONE CANVAS, AND A STRIP OF HANDLES
  //
  // The blocks are the work and they fill the window. Everything that helps you
  // make them sits behind one small labelled handle on the left strip and
  // slides out over the canvas, the same way the script editor and the UI
  // builder do it: closed by default, one panel at a time, Escape closes it, a
  // click on the canvas closes it unless it is pinned, and the tool comes back
  // the way you left it.
  //
  // KEEP OPEN is for the panels you work beside rather than read once: YOUR
  // RULES while you navigate, EXPLAIN THIS while you build.
  // ==========================================================================
  var prefs = {};                 // what the tool remembered from last time
  var shelfOpen = false;
  var shelfPane = 'start';        // which handle, open or not
  var shelfPinned = false;

  function setPref(name, value) {
    prefs[name] = value;
    send({ op: 'pref', name: name, value: String(value) });
  }

  // The one sentence at the top of each panel lives in the markup; the handle
  // name lives here, so the strip and the panel header can never disagree.
  var PANE_NAME = {
    start: 'Start here', rules: 'Your rules', find: 'Find a block',
    fits: 'What fits here', examples: 'Examples', explain: 'Explain this',
    level: 'The level', wrong: 'What went wrong', ask: 'Ask a question',
    legend: 'What things mean'
  };

  function showPane(name) {
    if (!PANE_NAME[name]) return;
    shelfPane = name;
    shelfOpen = true;
    var panes = document.querySelectorAll('.pane');
    for (var i = 0; i < panes.length; i++) panes[i].classList.remove('on');
    var p = $('pane-' + name);
    if (p) p.classList.add('on');
    // Built on open, not at start up: it needs the captured style, which
    // arrives after the page does, and rebuilding is cheap.
    if (name === 'legend') {
      try { buildLegend(); } catch (err) { UI.reportFault('building the legend', err); }
    }
    var btns = document.querySelectorAll('.railbtn');
    for (i = 0; i < btns.length; i++) {
      btns[i].classList.toggle('on', btns[i].dataset.pane === name);
      if (btns[i].dataset.pane === name) btns[i].classList.remove('hasnews');
    }
    $('side').classList.add('open');
    // The panel slides in over the right of the canvas, so the three things
    // that already live in that corner step aside: our minimap, and Blockly's
    // own trashcan and zoom controls. Nothing is resized, only moved.
    document.body.classList.add('shelf-open');
    /* The what-fits-here popup answers the same question this panel does, and
     * it lives in the same corner. The stylesheet stops it being drawn under an
     * open panel; this puts its state away to match, so it is not left marked
     * open and holding stale suggestions for a socket the user has moved on
     * from. */
    try {
      var sp = $('slotpop');
      if (sp && sp.classList.contains('on')) { closeSlot(); }
    } catch (e) {}
    $('sideTitle').textContent = PANE_NAME[name];
    if (name === 'rules') refreshNavigator();
    if (name === 'find') setTimeout(function () { try { $('search').focus(); } catch (e) {} }, 0);
    if (name === 'ask') setTimeout(function () { try { $('answerbox').focus(); } catch (e) {} }, 0);
    setPref('shelf', name);
    setPref('shelfOpen', true);
  }
  UI.showPane = showPane;

  // Closing leaves nothing marked ON, so anything elsewhere that asks "is this
  // panel showing?" stays true to what is on screen.
  function closeShelf(remember) {
    shelfOpen = false;
    var panes = document.querySelectorAll('.pane');
    for (var i = 0; i < panes.length; i++) panes[i].classList.remove('on');
    var btns = document.querySelectorAll('.railbtn');
    for (i = 0; i < btns.length; i++) btns[i].classList.remove('on');
    $('side').classList.remove('open');
    document.body.classList.remove('shelf-open');
    if (remember !== false) setPref('shelfOpen', false);
  }

  function toggleShelf(name) {
    if (shelfOpen && shelfPane === name) { closeShelf(); return; }
    showPane(name);
  }

  function setPinned(on) {
    shelfPinned = !!on;
    $('sidePin').classList.toggle('on', shelfPinned);
    $('sidePin').textContent = shelfPinned ? 'Stays open' : 'Keep open';
    setPref('shelfPinned', shelfPinned);
  }

  // A handle that has something new behind it says so, quietly, rather than
  // opening itself over the work.
  function markNews(name) {
    if (shelfOpen && shelfPane === name) return;
    var btns = document.querySelectorAll('.railbtn');
    for (var i = 0; i < btns.length; i++) {
      if (btns[i].dataset.pane === name) btns[i].classList.add('hasnews');
    }
  }
  UI.markNews = markNews;

  function closeMore() { $('more').hidden = true; }

  // What the tool remembered, applied once. Later messages are about the work,
  // not about how the window was left, so they never move a panel out from
  // under somebody.
  var prefsApplied = false;
  function applyFieldSpacing(compact) {
    BF6.state.prefs = BF6.state.prefs || {};
    var changed = !!BF6.state.prefs.compactFields !== !!compact;
    BF6.state.prefs.compactFields = !!compact;
    var choice = $('field-spacing');
    if (choice) choice.value = compact ? 'compact' : 'portal';
    if (changed && UI.ws) BF6.applyFieldSpacing(UI.ws, !!compact);
  }
  function applyReadability(p) {
    BF6.state.prefs = BF6.state.prefs || {};
    if (p && p.textFloor !== undefined) BF6.state.prefs.textFloor = p.textFloor;
    if (p && p.simplifiedOverview !== undefined)
      BF6.state.prefs.simplifiedOverview = String(p.simplifiedOverview) === 'true';
    var floor = textFloor(), slider = $('text-distance'), label = $('text-distance-value');
    if (slider) slider.value = Math.round((.5 - floor) * 100);
    if (label) label.textContent = floor === 0 ? 'Always visible' : 'Visible at ' + Math.round(floor * 100) + '% zoom and closer';
    if (slider && label) slider.setAttribute('aria-valuetext', label.textContent);
    var overview = $('simplified-overview');
    if (overview) overview.checked = BF6.state.prefs.simplifiedOverview === true;
    updateTextFloor();
    if (UI.ws && UI.ws.bf6Viewport) UI.ws.bf6Viewport.update();
  }
  function applyVariableColor(on) {
    UI.redVariables = !!on;
    var button = $('btn-redvars');
    if (button) {
      button.textContent = 'Variable color: ' + (on ? 'Dark red' : 'Portal');
      button.setAttribute('aria-pressed', String(!!on));
    }
    if (UI.ws) { var theme = themeFor(); if (theme) UI.ws.setTheme(theme); }
  }
  function applyPrefs(p) {
    if (p && p.compactFields !== undefined) applyFieldSpacing(String(p.compactFields) === 'true');
    applyReadability(p);
    if (p && p.redVariables !== undefined) applyVariableColor(String(p.redVariables) === 'true');
    if (prefsApplied) return;
    prefsApplied = true;
    var v = p || {};
    if (String(v.shelfPinned) === 'true') setPinned(true);
    var name = PANE_NAME[v.shelf] ? v.shelf : 'start';
    // Nothing remembered at all is a first run, and a first run opens on
    // START HERE rather than on an empty canvas with no way in.
    var open = (v.shelfOpen === undefined) ? true : String(v.shelfOpen) === 'true';
    if (open) showPane(name);
    else { shelfPane = name; closeShelf(false); }
  }

  function wireShelf() {
    var rail = document.querySelectorAll('.railbtn');
    for (var ri = 0; ri < rail.length; ri++) {
      rail[ri].onclick = (function (b) {
        return function () { toggleShelf(b.dataset.pane); };
      })(rail[ri]);
    }
    $('sideClose').onclick = function () { closeShelf(); };
    $('sidePin').onclick = function () { setPinned(!shelfPinned); };

    // Click away closes it, unless it is pinned. The strip, the panel and the
    // overflow are not "away", and neither is anything Blockly puts on the page
    // over the canvas: a field dropdown is part of the block you are editing.
    /* THE PULL-OUT CLOSES WHEN YOU CLICK AWAY FROM IT.
     * Blockly leaves a flyout open until another category is chosen or a
     * block is dragged out, so it sat over the canvas covering the work. */
    document.addEventListener('mousedown', function (ev) {
      try {
        var f = UI.ws && UI.ws.getFlyout && UI.ws.getFlyout();
        if (f && f.isVisible && f.isVisible()) {
          var inFlyout = ev.target && ev.target.closest &&
                         (ev.target.closest('.blocklyFlyout') ||
                          ev.target.closest('.blocklyToolboxDiv'));
          if (!inFlyout) { f.hide(); }
        }
      } catch (e) {}
    }, true);

    document.addEventListener('mousedown', function (ev) {
      if (!shelfOpen || shelfPinned) return;
      if ($('side').contains(ev.target) || $('rail').contains(ev.target)) return;
      if ($('more').contains(ev.target)) return;
      var t = ev.target;
      while (t && t.classList) {
        if (t.classList.contains('blocklyWidgetDiv') ||
          t.classList.contains('blocklyDropDownDiv') ||
          t.classList.contains('blocklyTooltipDiv')) return;
        t = t.parentNode;
      }
      closeShelf();
    }, true);

    // ---- the overflow ------------------------------------------------------
    $('btnMore').onclick = function (ev) {
      ev.stopPropagation();
      var m = $('more');
      if (!m.hidden) { closeMore(); return; }
      var r = $('btnMore').getBoundingClientRect();
      m.style.left = Math.round(r.left) + 'px';
      m.style.top = Math.round(r.bottom + 2) + 'px';
      m.style.maxHeight = Math.max(80, window.innerHeight - r.bottom - 12) + 'px';
      m.hidden = false;
    };
    $('more').addEventListener('click', function (ev) {
      if (ev.target.tagName === 'BUTTON') closeMore();
    });
    $('more').addEventListener('change', function (ev) { if (ev.target.tagName === 'SELECT') closeMore(); });
    document.addEventListener('mousedown', function (ev) {
      if ($('more').hidden) return;
      if ($('more').contains(ev.target) || ev.target === $('btnMore')) return;
      closeMore();
    }, true);

    // ---- START HERE, which is a set of doors into the other panels ---------
    $('start-examples').onclick = function () { showPane('examples'); };
    $('start-find').onclick = function () { showPane('find'); };
    $('start-fits').onclick = function () { showPane('fits'); };
    $('start-explain').onclick = function () { showPane('explain'); };
    $('start-save').onclick = function () { closeShelf(); $('btn-save').click(); };
    $('start-ask').onclick = function () { showPane('ask'); };

    $('answerbox').oninput = function () { refreshAnswers(this.value); };

    // ---- Escape ------------------------------------------------------------
    // Not on capture, and not when something already answered it: Blockly uses
    // Escape to drop a drag and to dismiss its own field editors, and taking
    // that away would be a worse trade than one more press to close a panel.
    document.addEventListener('keydown', function (ev) {
      if (ev.key !== 'Escape' || ev.defaultPrevented) return;
      if (!$('more').hidden) { closeMore(); ev.preventDefault(); return; }
      if ($('slotpop').classList.contains('on')) return;   // closeSlot has it
      if (shelfOpen) { closeShelf(); ev.preventDefault(); }
    });

    // If the tool never answers, the page still lands somewhere sensible and
    // the panels say so rather than sitting on "loading" for ever.
    setTimeout(function () {
      applyPrefs(null);
      if (!UI.answers) refreshAnswers('');
    }, 2500);
  }

  function wire() {
    wireShelf();
    $('btn-fit').onclick = fitAll;
    $('btn-collapse').onclick = function () { UI.ws.getTopBlocks(false).forEach(function (b) { b.setCollapsed(true); }); };
    $('btn-expand').onclick = function () { UI.ws.getAllBlocks(false).forEach(function (b) { b.setCollapsed(false); }); };
    $('btn-clean').onclick = function () { UI.ws.cleanUp(); drawMinimap(); };
    $('btn-undo').onclick = function () { UI.ws.undo(false); };
    $('btn-redo').onclick = function () { UI.ws.undo(true); };
    $('btn-save').onclick = function () { status('asking the site to save'); send({ op: 'savePortal' }); };
    $('btn-pull').onclick = function () { status('pulling the workspace from the site'); send({ op: 'pullWorkspace' }); };
    $('btn-push').onclick = function () {
      /* Same reason as the reset button: confirm never answers here. */
      if (!window.__bf6SiteArmed) {
        window.__bf6SiteArmed = true;
        setTimeout(function () { window.__bf6SiteArmed = false; }, 5000);
        status('this replaces the workspace on the site. Click again within five ' +
          'seconds to go ahead.');
        return;
      }
      window.__bf6SiteArmed = false;
      if (!nativeRouteAllowed('push to the page')) { return; }
      send({ op: 'pushWorkspace', json: projectSnapshot() });
    };
    // ---- the five in the top right, as the site has them ------------------
    // Each one is the same action that already existed somewhere behind the
    // More menu, except reset, which is new. Nothing was moved out of More:
    // every console command and every old onclick still finds its button.
    $('topsearch').oninput = function () {
      if (UI.searchInput) UI.searchInput.value = this.value;
      var bar = $('toolsearch');
      if (bar) bar.value = this.value;
      // The toolbox filter is the part that must never throw: it is what the
      // typing is for. The find panel is a bonus and is only fed when it is
      // actually up, so a search from the bar cannot fail on a closed panel.
      try { runToolboxSearch(this.value); } catch (e) {}
      try { if (UI.searchInput) runSearch(this.value); } catch (e) {}
    };
    $('btn-exportws').onclick = function () {
      status('exporting the workspace');
      doExport('workspace', '');
    };
    $('btn-importws').onclick = function () {
      status('choose a workspace, a full experience or a script file');
      send({ op: 'openImport' });
    };
    $('btn-importscript').onclick = function () {
      status('choose the folder holding the script project');
      send({ op: 'openImportScript' });
    };
    $('btn-exportscript').onclick = function () { exportAsScript('', false); };
    $('btn-exportsource').onclick = function () { exportAsScript('', true); };
    $('btn-resetws').onclick = function () {
      var n = 0;
      try { n = UI.ws.getAllBlocks(false).length; } catch (e) {}
      if (!n) { status('the canvas is already empty'); return; }
      var btn = this;
      // It says how much, and it goes through the workspace's own clear so one
      // Undo brings the whole thing back.
      armButton(btn, 'this will clear all ' + n + ' block(s). Undo brings them back.', function () {
        try { UI.ws.clear(); }
        catch (e) { status('could not clear the canvas: ' + (e.message || e), true); return; }
        try { drawMinimap(); } catch (e) {}
        status('cleared ' + n + ' block(s). Undo brings them back.');
      });
    };

    $('btn-import').onclick = function () {
      status('choose a workspace, a full experience or a script export');
      send({ op: 'openImport' });
    };
    $('export').onchange = function () {
      var kind = this.value;
      this.value = '';
      if (!kind) return;
      status('exporting the ' + (kind === 'experience' ? 'full experience' : 'workspace'));
      doExport(kind, '');
    };
    $('btn-tosite').onclick = function () {
      /* CHECK BEFORE SENDING, NOT AFTER BEING REJECTED.
       *
       * This used to hand over whatever came back from packing, including a
       * workspace already known to be over Portal's variable budget. Portal
       * refuses those, so the only thing the send accomplished was a round
       * trip to an error. If it cannot fit, say so and point at the route
       * that does work. */
      if (!nativeRouteAllowed('send to site')) { return; }
      var state = portalReadiness();
      if (state && !state.fits) { offerTypeScriptInstead(state); return; }

      /* The only path that packs: this is the one going to Portal. Saving a
       * workspace to disk keeps the readable names, because that file is for
       * coming back to. */
      var out = buildExport(UI.shell ? 'experience' : 'workspace', true);
      send({ op: 'sendToSite', format: UI.shell ? 'experience' : 'workspace',
        suggested: out.suggested, json: out.json });
      if (out.summary) showExportSummary(out.summary);
    };
    $('btn-logpull').onclick = function () {
      status('reading the game log');
      send({ op: 'gameLogPull', lines: 400 });
    };
    /* Start it BEFORE launching: the watcher re-looks for the folder every
       second until it appears, so arming it first is the normal order. */
    UI.logWatching = false;
    $('btn-logwatch').onclick = function () {
      UI.logWatching = !UI.logWatching;
      this.classList.toggle('on', UI.logWatching);
      this.textContent = UI.logWatching ? "Stop watching the game's log" : "Watch the game's log";
      var box = $('gamelog'); if (box) { box.hidden = false; }
      send({ op: 'gameLogWatch', on: UI.logWatching });
    };
    $('btn-capstyle').onclick = function () {
      status('asking the site for its renderer, constants, theme and stylesheet');
      send({ op: 'captureStyle' });
    };
    $('btn-redvars').onclick = function () {
      applyVariableColor(!UI.redVariables);
      setPref('redVariables', UI.redVariables);
    };
    $('text-distance').oninput = function () {
      applyReadability({ textFloor: (.5 - Number(this.value) / 100).toFixed(2) });
    };
    $('field-spacing').onchange = function () {
      var compact = this.value === 'compact';
      applyFieldSpacing(compact);
      setPref('compactFields', compact);
    };
    $('text-distance').onchange = function () { setPref('textFloor', textFloor()); };
    $('simplified-overview').onchange = function () {
      applyReadability({ simplifiedOverview: this.checked });
      setPref('simplifiedOverview', this.checked);
    };
    $('btn-slot').onclick = function () { UI.slotClickArmed = true; status('click an empty socket'); };
    $('btn-usesel').onclick = useSelectedObject;
    $('btn-assign').onclick = assignToSelected;
    $('btn-nextref').onclick = nextSceneHit;
    $('btn-rule').onclick = function () {
      if (UI.ruleMode) { exitRuleMode(); return; }
      var b = Blockly.getSelected();
      if (!b) { status('select a rule or a subroutine first', true); return; }
      var root = b;
      while (root && root.type !== 'ruleBlock' && root.type !== 'subroutineBlock') root = root.getParent();
      if (!root) { status('that block is not in a rule', true); return; }
      enterRuleMode(root.id);
    };
    $('search').oninput = function () { runSearch(this.value); };
    $('search').onkeydown = function (e) {
      if (e.key === 'Enter') stepSearch(e.shiftKey ? -1 : 1);
    };
    $('btn-prev').onclick = function () { stepSearch(-1); };
    $('btn-next').onclick = function () { stepSearch(1); };
    // The panel's own box and the bar at the top of the toolbox are the same
    // search: typing in either one drives the toolbox and shows the other the
    // same text, so they can never disagree.
    $('toolsearch').oninput = function () {
      if (UI.searchInput) UI.searchInput.value = this.value;
      runToolboxSearch(this.value);
    };
    $('snippets').onchange = function () {
      if (this.value) send({ op: 'snippetLoad', name: this.value });
      this.value = '';
    };
    ['rules', 'subs', 'vars', 'orphans'].forEach(function (tab) {
      $('tab-' + tab).onclick = function () {
        UI.navTab = tab;
        document.querySelectorAll('.tab').forEach(function (t) { t.classList.remove('on'); });
        this.classList.add('on');
        refreshNavigator();
      };
    });
    $('faqbox').oninput = function () { refreshFaq(this.value, $('faqtheme').value); };
    $('faqtheme').onchange = function () { refreshFaq($('faqbox').value, this.value); };
    FAQ_THEMES.forEach(function (t) {
      var o = el('option', null, t.toUpperCase());
      o.value = t;
      $('faqtheme').appendChild(o);
    });
    $('minimap').onclick = minimapJump;
    $('minimap').onmousemove = function (e) { if (e.buttons === 1) minimapJump(e); };
    document.addEventListener('keydown', function (e) {
      if ((e.ctrlKey || e.metaKey) && e.key === '0') { e.preventDefault(); fitAll(); }
      if ((e.ctrlKey || e.metaKey) && e.key === 'f') { e.preventDefault(); showPane('find'); }
      if (e.key === 'Escape') closeSlot();
    });
    document.addEventListener('paste', function (e) {
      var text = (e.clipboardData || window.clipboardData).getData('text');
      if (text && text.trim().charAt(0) === '{') { if (pasteClipboard(text)) e.preventDefault(); }
    });
    window.addEventListener('resize', function () { drawMinimap(); refreshZoomFloor(); positionRegionTabs(); });
    /* A heartbeat, but only when the trail is on. A tick asked for every
     * second that arrives late says the main thread was blocked and by how
     * much; silence says it never came back. It settled the hover question in
     * one run: the thread was never blocked while hovering, so that cost was
     * painting rather than anything of ours. */
    if (UI.trailOn) {
      var beatAt = Date.now(), beatN = 0;
      setInterval(function () {
        var now = Date.now(), late = now - beatAt - 1000;
        beatAt = now; beatN++;
        if (late > 150 || beatN % 10 === 0) {
          send({ op: 'log', level: 'display', text: 'trail: alive ' + beatN +
            (late > 150 ? ', thread was blocked for ' + late + 'ms' : '') });
        }
      }, 1000);
    }

    /* BACK, after following a call. */
    var bc = $('breadcrumb');
    if (bc) { bc.onclick = function () { goBack(); }; }

    /* DISMISS. The suggestions had no way to be closed except by choosing
     * something, so a click elsewhere left them sitting there. Escape and a
     * click outside both put them away, which is what every other panel in
     * the editor does. */
    document.addEventListener('mousedown', function (ev) {
      var pop = $('slotpop');
      if (pop && pop.classList.contains('on')) {
        if (!pop.contains(ev.target)) { closeSlot(); }
      }
      if (UI.pendingSocket) {
        /* Clicking the canvas rather than the offered flyout means the
         * question has been abandoned. */
        var onFlyout = ev.target && ev.target.closest &&
                       ev.target.closest('.blocklyFlyout');
        if (!onFlyout) { UI.pendingSocket = null; }
      }
    }, true);
    document.addEventListener('keydown', function (ev) {
      if (ev.key !== 'Escape') { return; }
      var pop = $('slotpop');
      if (pop && pop.classList.contains('on')) { closeSlot(); }
      if (UI.pendingSocket) {
        UI.pendingSocket = null;
        try { var f = UI.ws.getFlyout && UI.ws.getFlyout(); if (f) { f.hide(); } } catch (e) {}
        status('');
      }
    });

    setInterval(function () { drawMinimap(); }, 1500);
  }

  // ---- boot ---------------------------------------------------------------
  function boot() {
    offlineDefinitions(null);
    /* Before the workspace exists, so nothing can be deserialised into a
     * canvas whose extended types are not registered yet. */
    installExtended('boot');
    rebuildWorkspace();
    paintConnection();
    paintStylePill(null);
    status('waiting for the site');
  }

  window.addEventListener('DOMContentLoaded', function () {
    wire();
    wireDropTarget();
    boot();
    /* WHICH CONVERTER IS ACTUALLY RUNNING.
     *
     * A cached file:// script is indistinguishable from a fresh one from the
     * outside, and two rounds were spent testing a fix that was on disk but
     * not loaded. The converter carries a build stamp; printing it means the
     * log can answer whether a change is live, instead of it being inferred
     * from whether the bug went away. */
    try {
      var cv = (window.BF6Convert && window.BF6Convert.VERSION) || '(none)';
      trace('converter version: ' + cv);
    } catch (e) {}
    send({ op: 'ready', version: BF6.state.version });
    send({ op: 'needExperience' });
  });
})();
