// At unreadable zoom, cache the block silhouettes in a viewport-sized image.
// The Blockly model remains authoritative; click a silhouette to return to
// editable blocks. No cache is serialized or retained across workspace edits.
(function (g) {
  'use strict';
  function install(ws) {
    var svg = ws.getParentSvg(), host = svg.parentNode, blockCanvas = ws.getCanvas();
    var parent = blockCanvas.parentNode, next = blockCanvas.nextSibling;
    var view = document.createElement('div'), canvas = document.createElement('canvas');
    Object.assign(view.style, { position: 'absolute', pointerEvents: 'none', overflow: 'hidden', display: 'none' });
    Object.assign(canvas.style, { position: 'absolute', left: '0', top: '0', willChange: 'transform' });
    view.appendChild(canvas); host.appendChild(view);
    var label = document.createElement('span');
    label.textContent = 'Overview · Click a block to edit';
    Object.assign(label.style, { position: 'absolute', left: '12px', top: '12px', padding: '5px 8px',
      background: '#15191dee', color: '#dce2e5', font: '12px sans-serif' });
    view.appendChild(label);
    var ctx = canvas.getContext('2d'), paths = [], previous = null, revision = -1, active = false;
    var stats = { active: false, paints: 0, bytes: 0 };
    function restore() {
      if (!blockCanvas.parentNode) parent.insertBefore(blockCanvas, next && next.parentNode === parent ? next : null);
      view.style.display = 'none'; active = false; stats.active = false;
    }
    function snapshot(records, currentRevision) {
      restore(); paths = [];
      records.forEach(function (r) {
        var b = r.block, p = b.pathObject && b.pathObject.svgPath;
        if (!p || !p.getAttribute('d') || b.isInsertionMarker()) return;
        // Collapsed inputs keep their blocks in the model, but are not drawn.
        var node = r.root;
        while (node && node !== blockCanvas) {
          if (node.style && node.style.display === 'none') return;
          node = node.parentNode;
        }
        var style = getComputedStyle(p), matrix = p.transform.baseVal.consolidate();
        matrix = matrix ? matrix.matrix : { a: 1, b: 0, c: 0, d: 1, e: 0, f: 0 };
        paths.push({ block: b, root: r.root, x: r.x, y: r.y, w: r.width, h: r.height,
          path: new Path2D(p.getAttribute('d')), matrix: matrix,
          fill: style.fill.indexOf('url(') === 0 ? '#454545' : style.fill,
          stroke: style.stroke.indexOf('url(') === 0 ? 'none' : style.stroke,
          strokeWidth: parseFloat(style.strokeWidth) || 0 });
      });
      paths.sort(function (a, b) {
        return a.root.compareDocumentPosition(b.root) & Node.DOCUMENT_POSITION_FOLLOWING ? -1 : 1;
      });
      revision = currentRevision; previous = null;
    }
    function update(records, currentRevision) {
      var scale = ws.scale;
      var prefs = g.BF6Blocks && g.BF6Blocks.state.prefs || {};
      var floor = g.BF6UI && g.BF6UI.textFloor ? g.BF6UI.textFloor() : .15;
      // Preserve the exact SVG appearance unless the creator opts in. Even
      // then, never simplify a zoom where their labels should remain visible.
      if (prefs.simplifiedOverview !== true || scale >= Math.min(.2, floor) || records.length < 1500 || !ctx || !g.Path2D) { restore(); return false; }
      if (revision !== currentRevision) snapshot(records, currentRevision);
      var manager = ws.getMetricsManager(), a = manager.getAbsoluteMetrics(), v = manager.getViewMetrics();
      var w = Math.ceil(v.width), h = Math.ceil(v.height), x = ws.scrollX, y = ws.scrollY, margin = 512;
      if (!(w > 0 && h > 0) || (w + margin * 2) * (h + margin * 2) > 16 * 1024 * 1024) {
        restore(); return false;
      }
      Object.assign(view.style, { left: a.left + 'px', top: a.top + 'px', width: w + 'px', height: h + 'px', display: 'block' });
      // A hidden SVG still incurs layout/prepaint work when its transform
      // changes. Detach the drawing only; retain the live model and SVG nodes.
      if (blockCanvas.parentNode) blockCanvas.remove();
      active = true; stats.active = true;
      if (!previous || previous.scale !== scale || previous.w !== w || previous.h !== h ||
          Math.abs(x - previous.x) > margin - 32 || Math.abs(y - previous.y) > margin - 32) {
        canvas.width = w + margin * 2; canvas.height = h + margin * 2;
        paths.forEach(function (r) {
          var left = ws.RTL ? r.x - r.w : r.x, right = ws.RTL ? r.x : r.x + r.w;
          if (left * scale + x > w + margin || right * scale + x < -margin ||
              r.y * scale + y > h + margin || (r.y + r.h) * scale + y < -margin) return;
          var m = r.matrix;
          ctx.setTransform(scale * m.a, scale * m.b, scale * m.c, scale * m.d,
            (r.x + m.e) * scale + x + margin, (r.y + m.f) * scale + y + margin);
          if (r.fill !== 'none') { ctx.fillStyle = r.fill; ctx.fill(r.path); }
          // Subpixel outlines add tessellation cost without a readable edge.
          if (r.stroke !== 'none' && r.strokeWidth * scale >= .5) { ctx.strokeStyle = r.stroke; ctx.lineWidth = r.strokeWidth; ctx.stroke(r.path); }
        });
        previous = { scale: scale, x: x, y: y, w: w, h: h }; stats.paints++;
        stats.bytes = canvas.width * canvas.height * 4;
      }
      canvas.style.transform = 'translate(' + (x - previous.x - margin) + 'px,' + (y - previous.y - margin) + 'px)';
      return true;
    }
    function pick(event) {
      if (!active || event.button !== 0 || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
      // Leave toolbox, scrollbar, comments, and the helper shelf interactive.
      if (event.target !== ws.getParentSvg() && !event.target.classList.contains('blocklyMainBackground')) return;
      var rect = svg.getBoundingClientRect(), manager = ws.getMetricsManager();
      var a = manager.getAbsoluteMetrics(), v = manager.getViewMetrics();
      var sx = event.clientX - rect.left - a.left, sy = event.clientY - rect.top - a.top;
      if (sx < 0 || sy < 0 || sx > v.width || sy > v.height) return;
      var x = (sx - ws.scrollX) / ws.scale, y = (sy - ws.scrollY) / ws.scale;
      var hit = null;
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      for (var i = paths.length - 1; i >= 0; i--) {
        var r = paths[i], m = r.matrix, dx = x - r.x - m.e, dy = y - r.y - m.f;
        var det = m.a * m.d - m.b * m.c;
        if (!det) continue;
        if (ctx.isPointInPath(r.path, (m.d * dx - m.c * dy) / det, (-m.b * dx + m.a * dy) / det)) { hit = r; break; }
      }
      if (!hit) return; // Empty canvas still pans normally.
      event.preventDefault(); event.stopImmediatePropagation();
      restore(); ws.setScale(Math.min(.8, ws.options.zoomOptions.maxScale));
      ws.scroll(-hit.x * ws.scale + v.width / 3, -hit.y * ws.scale + v.height / 3);
      hit.block.select();
    }
    host.addEventListener('pointerdown', pick, true);
    return { update: update, restore: restore, stats: stats,
      dispose: function () { restore(); host.removeEventListener('pointerdown', pick, true); view.remove(); paths = []; canvas.width = canvas.height = 0; } };
  }
  g.BF6Overview = { install: install };
})(window);
