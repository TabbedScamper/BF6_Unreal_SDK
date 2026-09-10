// Keep the entire Blockly model live, but prune SVG branches outside the view.
// Bounds come from rendered block geometry, never from hidden DOM measurements.
(function (g) {
  'use strict';
  var HIDDEN = 'bf6-viewport-pruned';
  function install(B, ws) {
    if (ws.bf6Viewport) return ws.bf6Viewport;
    if (!document.getElementById('bf6-viewport-style')) {
      var style = document.createElement('style');
      style.id = 'bf6-viewport-style';
      style.textContent = '.' + HIDDEN + ' { display: none !important; }';
      document.head.appendChild(style);
    }
    // Rendering measures fields. Restore pruned branches BEFORE that starts,
    // including render calls made with Blockly events disabled during imports.
    if (!B.BlockSvg.prototype.bf6ViewportHooks) {
      ['render', 'renderEfficiently', 'queueRender', 'moveBy', 'setDragging', 'setCollapsed', 'setParent', 'dispose'].forEach(function (name) {
        var original = B.BlockSvg.prototype[name];
        if (!original) return;
        B.BlockSvg.prototype[name] = function () {
          var view = this.workspace && this.workspace.bf6Viewport;
          if (view) {
            if (name === 'setDragging') view.setDragging(this, arguments[0]);
            else view.invalidate();
          }
          return original.apply(this, arguments);
        };
      });
      B.BlockSvg.prototype.bf6ViewportHooks = true;
    }
    var records = [], hidden = new Set(), dirty = true, pending = 0, disposed = false;
    var dragging = new Set(), overview = g.BF6Overview ? g.BF6Overview.install(ws) : null;
    var stats = { blocks: 0, pruned: 0, rebuilds: 0, sweeps: 0, sweepMs: 0 };
    function restoreDrawing(r) {
      if (!r.parked) return;
      r.parked.forEach(function (item) {
        // Keep the exact order relative to connected blocks and other artwork.
        if (item.marker.parentNode === r.root) r.root.replaceChild(item.node, item.marker);
      });
      r.parked = null;
    }
    function parkDrawing(r) {
      // Chromium relayouts text under display:none SVG groups on scale changes.
      // Park only their drawing nodes. Block roots and their transform hierarchy
      // stay attached, so Blockly coordinates, connections and export still work.
      var childRoots = new Set(r.children.map(function (b) { return b.getSvgRoot(); }));
      r.parked = [];
      Array.from(r.root.childNodes).forEach(function (node) {
        if (childRoots.has(node)) return;
        var marker = document.createComment('offscreen artwork');
        r.root.replaceChild(marker, node);
        r.parked.push({ node: node, marker: marker });
      });
    }
    function reveal() {
      hidden.forEach(function (r) { restoreDrawing(r); r.root.classList.remove(HIDDEN); });
      hidden.clear(); stats.pruned = 0;
    }
    function schedule() {
      if (pending || disposed) return;
      pending = requestAnimationFrame(function () { pending = 0; update(); });
    }
    function invalidate() {
      if (disposed) return;
      if (overview) overview.restore();
      if (!dirty) reveal();
      dirty = true;
      schedule();
    }
    function rebuild() {
      reveal(); records = [];
      var ordered = [], stack = ws.getTopBlocks(false).slice(), byBlock = new Map();
      // Iterative traversal also handles very long statement chains.
      while (stack.length) {
        var b = stack.pop(), root = b.getSvgRoot();
        if (!root) continue;
        var xy = b.getRelativeToSurfaceXY();
        var width = Number(b.width), height = Number(b.height);
        if (!isFinite(width) || !isFinite(height)) { width = height = Infinity; }
        var rec = { block: b, root: root, x: xy.x, y: xy.y, width: width, height: height,
          left: ws.RTL ? xy.x - width : xy.x,
          right: ws.RTL ? xy.x : xy.x + width, top: xy.y, bottom: xy.y + height,
          children: b.getChildren(false) };
        byBlock.set(b, rec); ordered.push(rec);
        for (var k = 0; k < rec.children.length; k++) stack.push(rec.children[k]);
      }
      // A parent may be above the screen while its connected tail is visible.
      // Include EVERY descendant, including next connections, in its bounds.
      for (var i = ordered.length - 1; i >= 0; i--) {
        var r = ordered[i];
        for (var j = 0; j < r.children.length; j++) {
          var c = byBlock.get(r.children[j]);
          if (!c) continue;
          r.left = Math.min(r.left, c.left); r.right = Math.max(r.right, c.right);
          r.top = Math.min(r.top, c.top); r.bottom = Math.max(r.bottom, c.bottom);
        }
      }
      records = ordered; dirty = false; stats.blocks = records.length; stats.rebuilds++;
      document.body.classList.toggle('perf-heavy', records.length > 1500);
    }
    function update() {
      if (disposed) return;
      var ui = g.BF6UI, state = g.BF6Blocks && g.BF6Blocks.state;
      if ((ui && ui.ws === ws && (ui.applying || ui.loadingDoc)) || (state && state.loading)) {
        reveal(); dirty = true; schedule(); return;
      }
      if (dirty) rebuild();
      if (records.length < 1500) { reveal(); return; }
      var v = ws.getMetricsManager().getViewMetrics(true);
      if (!(v.width > 0 && v.height > 0)) { reveal(); return; }
      if (overview && !dragging.size) {
        // The optional overview captures computed path styles from live SVG.
        if (state && state.prefs && state.prefs.simplifiedOverview === true &&
            ws.scale < Math.min(.2, ui && ui.textFloor ? ui.textFloor() : .15)) reveal();
        try { if (overview.update(records, stats.rebuilds)) return; }
        catch (error) {
          overview.dispose(); overview = null;
          stats.overviewError = String(error && error.message || error);
        }
      }
      var start = performance.now(), margin = 128 / ws.scale;
      var left = v.left - margin, right = v.left + v.width + margin;
      var top = v.top - margin, bottom = v.top + v.height + margin;
      for (var i = 0; i < records.length; i++) {
        var r = records[i];
        var far = !dragging.has(r.block) && (r.left > right || r.right < left || r.top > bottom || r.bottom < top);
        if (far && !hidden.has(r)) { parkDrawing(r); r.root.classList.add(HIDDEN); hidden.add(r); }
        else if (!far && hidden.has(r)) { restoreDrawing(r); r.root.classList.remove(HIDDEN); hidden.delete(r); }
      }
      stats.pruned = hidden.size; stats.sweeps++; stats.sweepMs = performance.now() - start;
    }
    var originalTranslate = ws.translate;
    ws.translate = function () {
      var result = originalTranslate.apply(this, arguments);
      // Apply in the same frame as scroll/zoom, including scrollbar and host
      // navigation paths that do not emit a viewport event immediately.
      if (!dirty) update(); else schedule();
      return result;
    };
    var listener = function (ev) {
      if (!ev) return;
      if (!ev.isUiEvent || ev.type === B.Events.BLOCK_DRAG) invalidate();
    };
    ws.addChangeListener(listener);
    var api = { invalidate: invalidate, update: update, stats: stats, overview: overview,
      setDragging: function (block, on) {
        if (on) dragging.add(block); else dragging.delete(block);
        invalidate();
      },
      dispose: function () {
        disposed = true; if (pending) cancelAnimationFrame(pending);
        if (overview) overview.dispose();
        reveal(); ws.removeChangeListener(listener); ws.translate = originalTranslate;
        delete ws.bf6Viewport; records = [];
      } };
    ws.bf6Viewport = api; schedule(); return api;
  }
  g.BF6Viewport = { install: install };
})(window);
