/*
 * BF6 UI BUILDER - the editor page.
 *
 * The model, the layout maths and every exporter live in lib/bf6ui.js, which
 * the node tests also load, so what this page draws and what the tests assert
 * come out of one place. This file is the hands: the canvas, the tree, the
 * property panel, the timeline and the seam back to the tool.
 */
'use strict';

(function () {

var UI = window.BF6Ui;

/* ===================================================================== *
 * State
 * ===================================================================== */

var S = {
    design: UI.template('frosted-panel'),
    sel: [],                 // widget ids
    aspect: 16 / 9,
    zoom: 0.5,
    snap: true,
    grid: 8,
    undo: [],
    redo: [],
    clipboard: null,
    clip: null,              // the animation clip being edited
    playhead: 0,
    timelineOpen: false,   // the canvas only previews a clip while this is on
    keyProp: 'bgAlpha',
    keyEase: 'linear',
    selKey: null,            // { track: n, index: n }
    issues: [],
    sheetFormat: 'typescript',
    exports: null,
    hoverState: 'base',
    showHud: true          // the game HUD mock, on by default: layering is the point
};

var el = {};
['designName','btnNew','tplPick','btnUndo','btnRedo','aspect','zoom','snapGrid','btnCheck',
 'pillIssues','btnImport','btnExport','tree','live','liveHint','stage','stagewrap','safe','timeline',
 'clipPick','btnClipNew','btnPlay','btnKey','keyProp','keyEase','clipLoop','clipDur','btnKeyDel','tlrows',
 'props','issues','status','sheet','fmtcol','sheettext','warns','btnCopy','btnToScript','btnToBlocks',
 'btnSaveAs','btnSheetClose','btnDup','btnGroup','btnDel','btnUp','btnDown','btnHud',
 'rail','side','sideHead','sideTitle','sidePin','sideClose'
].forEach(function (id) { el[id] = document.getElementById(id); });

function say(text) { el.status.textContent = text; }

/* ===================================================================== *
 * The seam back to the tool
 * ===================================================================== */

var bridge = (window.ue && window.ue.bf6uibuilder) ? window.ue.bf6uibuilder : null;

function send(msg) {
    if (!bridge) { say('Not running inside the tool: ' + msg.op + ' has nowhere to go.'); return; }
    try { bridge.call(JSON.stringify(msg)); }
    catch (e) { say('The tool did not take that message: ' + e); }
}

window.BF6UiBuilder = {
    recv: function (json) {
        var m;
        try { m = JSON.parse(json); } catch (e) { return; }
        if (m.op === 'design') {
            try {
                var d = UI.importAny(typeof m.json === 'string' ? m.json : JSON.stringify(m.json),
                                     m.strings, m.name);
                pushUndo();
                S.design = d;
                S.sel = [];
                el.designName.value = d.name;
                redrawAll();
                say('Loaded ' + (m.name || d.name) + '.');
            } catch (e) { say('That file did not load: ' + e.message); }
        } else if (m.op === 'template') {
            loadTemplate(m.name);
        } else if (m.op === 'export') {
            openSheet(m.format || 'typescript');
            if (m.path) send({ op: 'exportFile', format: S.sheetFormat, path: m.path,
                               text: el.sheettext.value });
        } else if (m.op === 'palette') {
            // The tool read Resources/uibuilder/palette.json for us; a page on
            // file:// cannot fetch it itself.
            if (UI.setPalette(m.json)) { drawProps(); say('Palette loaded.'); }
        } else if (m.op === 'prefs') {
            // How the creator left the window last time: which handle was open,
            // and whether they had pinned it.
            applyShelfPrefs(m.prefs || {});
        } else if (m.op === 'status') {
            say(m.text || '');
        }
    }
};

/* ===================================================================== *
 * Undo
 * ===================================================================== */

function snapshot() {
    return JSON.stringify({ d: S.design, sel: S.sel });
}
function pushUndo() {
    S.undo.push(snapshot());
    if (S.undo.length > 100) S.undo.shift();
    S.redo.length = 0;
}
function restore(text) {
    var o = JSON.parse(text);
    S.design = o.d;
    S.sel = o.sel || [];
}
function undo() {
    if (!S.undo.length) return;
    S.redo.push(snapshot());
    restore(S.undo.pop());
    redrawAll();
}
function redo() {
    if (!S.redo.length) return;
    S.undo.push(snapshot());
    restore(S.redo.pop());
    redrawAll();
}

/* ===================================================================== *
 * Selection helpers
 * ===================================================================== */

function selNodes() {
    return S.sel.map(function (id) { return UI.findById(S.design.widgets, id); })
                .filter(function (n) { return !!n; });
}
function first() { return selNodes()[0] || null; }

function select(id, additive) {
    if (!additive) S.sel = id ? [id] : [];
    else if (S.sel.indexOf(id) >= 0) S.sel = S.sel.filter(function (x) { return x !== id; });
    else S.sel.push(id);
    redrawAll();
}

function siblingsOf(node) {
    var p = UI.parentOf(S.design.widgets, node.id);
    return p ? p.children : S.design.widgets;
}

function boxFor(node) {
    var p = UI.parentOf(S.design.widgets, node.id);
    if (!p) return UI.screenRect(S.aspect);
    var r = layoutCache[p.id];
    return UI.contentBox(p, r);
}

/* ===================================================================== *
 * The canvas
 * ===================================================================== */

var layoutCache = {};

function stageGeometry() {
    var screen = UI.screenRect(S.aspect);
    return { screen: screen, ox: -screen.left };
}

function drawStage() {
    var g = stageGeometry();
    layoutCache = UI.layout(S.design.widgets, S.aspect);

    el.stage.style.width = (g.screen.width * S.zoom) + 'px';
    el.stage.style.height = (UI.CANVAS_HEIGHT * S.zoom) + 'px';
    el.stage.style.backgroundSize = (S.grid * S.zoom) + 'px ' + (S.grid * S.zoom) + 'px';

    // The 1920 x 1080 safe area, drawn wherever it falls in this screen shape.
    el.safe.style.left = (g.ox * S.zoom) + 'px';
    el.safe.style.top = '0px';
    el.safe.style.width = (UI.CANVAS_WIDTH * S.zoom) + 'px';
    el.safe.style.height = (UI.CANVAS_HEIGHT * S.zoom) + 'px';

    // Rebuild the widget layer.
    var old = el.stage.querySelectorAll('.w, .clipmark, .guide, .hudr');
    for (var i = 0; i < old.length; i++) old[i].parentNode.removeChild(old[i]);

    /*
     * THE LAYER ORDER ON THE CANVAS, so what you see is what draws.
     *
     *   z 1   widgets set to BelowGameUI
     *   z 2   the game HUD mock
     *   z 3   widgets set to AboveGameUI
     *
     * The mock is approximate and says so; it is here to answer the one
     * question the LAYER control asks, which is what Under hides and what
     * Over covers.
     */
    var depths = UI.resolveDepths(S.design);

    var frag = document.createDocumentFragment();
    // Scrubbing a clip repaints the canvas at that tick. With the timeline
    // shut, the canvas shows the design as authored, so a template that fades
    // in is not drawn at its own tick zero and mistaken for a blank panel.
    var preview = (S.timelineOpen && S.clip)
        ? UI.applySample(S.design, UI.sampleClip(S.clip, S.playhead))
        : S.design;
    var previewLayout = UI.layout(preview.widgets, S.aspect);

    UI.walk(preview.widgets, function (n) {
        var r = previewLayout[n.id];
        if (!r) return;
        var e = makeWidgetEl(n, r, g.ox);
        e.style.zIndex = (depths[n.id] === 'BelowGameUI') ? 1 : 3;
        frag.appendChild(e);
    });
    el.stage.appendChild(frag);

    if (S.showHud) {
        UI.hudLayout(S.aspect).forEach(function (h) {
            var m = document.createElement('div');
            m.className = 'hudr';
            m.style.zIndex = 2;
            m.style.left = ((h.rect.left + g.ox) * S.zoom) + 'px';
            m.style.top = (h.rect.top * S.zoom) + 'px';
            m.style.width = (h.rect.width * S.zoom) + 'px';
            m.style.height = (h.rect.height * S.zoom) + 'px';
            m.title = h.label + ' (approximate: the real HUD has not been measured)';
            var b = document.createElement('b');
            b.textContent = h.label;
            m.appendChild(b);
            el.stage.appendChild(m);
        });
    }

    // Anything the check flagged as leaving the screen gets a red box.
    S.issues.filter(function (f) { return f.kind === 'clip' && f.aspect === aspectId(); })
        .forEach(function (f) {
            var r = layoutCache[f.id];
            if (!r) return;
            var m = document.createElement('div');
            m.className = 'clipmark';
            m.style.left = ((r.left + g.ox) * S.zoom) + 'px';
            m.style.top = (r.top * S.zoom) + 'px';
            m.style.width = (r.width * S.zoom) + 'px';
            m.style.height = (r.height * S.zoom) + 'px';
            el.stage.appendChild(m);
        });
}

function aspectId() {
    for (var i = 0; i < UI.ASPECTS.length; i++) {
        if (Math.abs(UI.ASPECTS[i].ratio - S.aspect) < 0.001) return UI.ASPECTS[i].id;
    }
    return '16:9';
}

function rgba(c, a) {
    c = c || [0, 0, 0];
    return 'rgba(' + Math.round((c[0] || 0) * 255) + ',' + Math.round((c[1] || 0) * 255) + ',' +
           Math.round((c[2] || 0) * 255) + ',' + (a === undefined ? 1 : a) + ')';
}

// As close to what the game draws as CSS gets: blur is a real backdrop blur,
// the two outline fills are borders of the engine's two weights, and each
// gradient runs from the edge it names.
function fillStyle(node, fillEl) {
    var c = node.bgColor, a = Number(node.bgAlpha);
    var st = fillEl.style;
    st.background = 'none'; st.border = 'none'; st.backdropFilter = ''; st.webkitBackdropFilter = '';
    switch (node.bgFill) {
        case 'Solid': st.background = rgba(c, a); break;
        case 'Blur':
            st.background = rgba(c, a * 0.55);
            st.backdropFilter = 'blur(10px)';
            st.webkitBackdropFilter = 'blur(10px)';
            break;
        case 'None': break;
        case 'OutlineThin': st.border = '1px solid ' + rgba(c, a); break;
        case 'OutlineThick': st.border = '4px solid ' + rgba(c, a); break;
        case 'GradientTop':
            st.background = 'linear-gradient(to bottom, ' + rgba(c, a) + ', ' + rgba(c, 0) + ')'; break;
        case 'GradientBottom':
            st.background = 'linear-gradient(to top, ' + rgba(c, a) + ', ' + rgba(c, 0) + ')'; break;
        case 'GradientLeft':
            st.background = 'linear-gradient(to right, ' + rgba(c, a) + ', ' + rgba(c, 0) + ')'; break;
        case 'GradientRight':
            st.background = 'linear-gradient(to left, ' + rgba(c, a) + ', ' + rgba(c, 0) + ')'; break;
    }
}

var ICON_SVG = {
    None: '<rect x="2" y="2" width="20" height="20" fill="none" stroke="currentColor" stroke-dasharray="3 3"/>',
    CrownOutline: '<path d="M3 18h18l-2-9-4 4-3-6-3 6-4-4z" fill="none" stroke="currentColor" stroke-width="1.6"/>',
    CrownSolid: '<path d="M3 18h18l-2-9-4 4-3-6-3 6-4-4z" fill="currentColor"/>',
    QuestionMark: '<path d="M9 8a3 3 0 1 1 4 3c-1 .6-1 1.4-1 2.4" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="18" r="1.4" fill="currentColor"/>',
    RifleAmmo: '<rect x="9" y="8" width="6" height="12" fill="currentColor"/><path d="M12 2l3 6H9z" fill="currentColor"/>',
    SelfHeal: '<path d="M10 3h4v7h7v4h-7v7h-4v-7H3v-4h7z" fill="currentColor"/>',
    SpawnBeacon: '<path d="M12 3l7 16H5z" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="15" r="2" fill="currentColor"/>',
    TEMP_PortalIcon: '<circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" stroke-width="2"/><circle cx="12" cy="12" r="3" fill="currentColor"/>'
};

function iconSvg(type, color) {
    return '<svg viewBox="0 0 24 24" style="width:100%;height:100%;color:' + color + '">' +
           (ICON_SVG[type] || ICON_SVG.None) + '</svg>';
}

var TEXT_JUSTIFY = { Left: 'flex-start', Center: 'center', Right: 'flex-end' };

function makeWidgetEl(node, r, ox) {
    var d = document.createElement('div');
    d.className = 'w' + (S.sel.indexOf(node.id) >= 0 ? ' sel' : '');
    d.dataset.id = node.id;
    d.style.left = ((r.left + ox) * S.zoom) + 'px';
    d.style.top = (r.top * S.zoom) + 'px';
    d.style.width = (r.width * S.zoom) + 'px';
    d.style.height = (r.height * S.zoom) + 'px';
    d.style.opacity = node.visible === false ? 0.28 : 1;

    var fill = document.createElement('div');
    fill.className = 'fill';
    if (node.type === 'Button') {
        var stateColor = node['buttonColor' +
            (S.hoverState === 'hover' ? 'Hover' : S.hoverState === 'pressed' ? 'Pressed' : 'Base')];
        var stateAlpha = node['buttonAlpha' +
            (S.hoverState === 'hover' ? 'Hover' : S.hoverState === 'pressed' ? 'Pressed' : 'Base')];
        fillStyle({ bgFill: node.bgFill, bgColor: stateColor, bgAlpha: stateAlpha }, fill);
    } else {
        fillStyle(node, fill);
    }
    d.appendChild(fill);

    if (node.type === 'Text') {
        var t = document.createElement('div');
        t.className = 'lbl';
        var f = UI.anchorFrac(node.textAnchor || 'Center');
        t.style.justifyContent = f.ax === 0 ? 'flex-start' : (f.ax === 1 ? 'flex-end' : 'center');
        t.style.alignItems = f.ay === 0 ? 'flex-start' : (f.ay === 1 ? 'flex-end' : 'center');
        t.style.color = rgba(node.textColor, node.textAlpha);
        t.style.fontSize = (Number(node.textSize) * S.zoom) + 'px';
        t.style.lineHeight = '1.05';
        t.style.fontFamily = 'Consolas, "Courier New", monospace';
        if (node.textBgBlur) {
            // SetUITextBgBlur: a blur behind the glyphs. The style guide asks
            // for one behind every line of text and every number.
            t.style.backdropFilter = 'blur(6px)';
            t.style.webkitBackdropFilter = 'blur(6px)';
        }
        t.textContent = renderedText(node);
        d.appendChild(t);
    }

    if (node.type === 'WeaponImage') {
        var weapon = document.createElement('div');
        weapon.className = 'lbl';
        weapon.style.fontSize = '14px';
        weapon.style.whiteSpace = 'pre-line';
        weapon.textContent = (node.weapon || 'Carbine_M4A1').replace(/_/g, ' ') + '\n' +
            (node.attachments || []).length + ' custom attachments\nWeapon image renders in Portal';
        d.appendChild(weapon);
    }
    if (node.type === 'Image') {
        var im = document.createElement('div');
        im.className = 'lbl';
        im.style.opacity = node.imageAlpha === undefined ? 1 : node.imageAlpha;
        im.innerHTML = iconSvg(node.imageType || 'None', rgba(node.imageColor, 1));
        d.appendChild(im);
    }

    if (S.sel.indexOf(node.id) >= 0) {
        ['nw', 'n', 'ne', 'w', 'e', 'sw', 's', 'se'].forEach(function (h) {
            var hd = document.createElement('div');
            hd.className = 'handle h-' + h;
            hd.dataset.handle = h;
            d.appendChild(hd);
        });
    }
    return d;
}

// The string template with its sample values dropped into the {} slots. This
// is what the LIVE VALUES panel is for: the engine never shows the template.
function renderedText(node) {
    var tmpl = S.design.strings[node.name];
    if (tmpl === undefined) tmpl = node.textLabel || '';
    var args = node.textArgs || [];
    var i = 0;
    return String(tmpl).replace(/\{\}/g, function () {
        var v = args[i++];
        return v === undefined || v === '' ? '{}' : String(v);
    });
}

/* ===================================================================== *
 * Dragging and resizing
 * ===================================================================== */

// A rect back into an anchored position. The exact inverse of placeIn, so a
// drag never quietly changes what the anchor means.
function rectToPosition(r, node, box) {
    var f = UI.anchorFrac(node.anchor);
    var apx = r.left + f.ax * r.width;
    var apy = r.top + f.ay * r.height;
    var px = f.ax === 0 ? apx - box.left
           : (f.ax === 1 ? box.right - apx : apx - (box.left + box.width / 2));
    var py = f.ay === 0 ? apy - box.top
           : (f.ay === 1 ? box.bottom - apy : apy - (box.top + box.height / 2));
    return [Math.round(px), Math.round(py)];
}

function snapValue(v) { return S.snap ? Math.round(v / S.grid) * S.grid : Math.round(v); }

// Edges worth snapping to: the parent's content box and every sibling.
function snapTargets(node) {
    var box = boxFor(node);
    var xs = [box.left, box.left + box.width / 2, box.right];
    var ys = [box.top, box.top + box.height / 2, box.bottom];
    siblingsOf(node).forEach(function (s) {
        if (s.id === node.id) return;
        var r = layoutCache[s.id];
        if (!r) return;
        xs.push(r.left, r.centerX, r.right);
        ys.push(r.top, r.centerY, r.bottom);
    });
    return { xs: xs, ys: ys };
}

function snapRect(r, node) {
    if (!S.snap) return r;
    var t = snapTargets(node);
    var tol = 6;
    var best;

    best = null;
    [['left', r.left], ['centerX', r.left + r.width / 2], ['right', r.left + r.width]].forEach(function (pair) {
        t.xs.forEach(function (x) {
            var d = Math.abs(pair[1] - x);
            if (d <= tol && (!best || d < best.d)) best = { d: d, shift: x - pair[1] };
        });
    });
    if (best) r.left += best.shift;

    best = null;
    [['top', r.top], ['centerY', r.top + r.height / 2], ['bottom', r.top + r.height]].forEach(function (pair) {
        t.ys.forEach(function (y) {
            var d = Math.abs(pair[1] - y);
            if (d <= tol && (!best || d < best.d)) best = { d: d, shift: y - pair[1] };
        });
    });
    if (best) r.top += best.shift;

    return r;
}

var drag = null;

el.stage.addEventListener('mousedown', function (ev) {
    var handle = ev.target.dataset.handle;
    var wEl = ev.target.closest ? ev.target.closest('.w') : null;
    if (!wEl) { if (!ev.ctrlKey) select(null); return; }
    var id = wEl.dataset.id;
    if (S.sel.indexOf(id) < 0) select(id, ev.ctrlKey);

    var nodes = selNodes();
    if (!nodes.length) return;
    pushUndo();
    drag = {
        handle: handle || null,
        startX: ev.clientX, startY: ev.clientY,
        items: nodes.map(function (n) {
            return { node: n, rect: UI.clone(layoutCache[n.id]), box: boxFor(n) };
        })
    };
    ev.preventDefault();
});

window.addEventListener('mousemove', function (ev) {
    if (!drag) return;
    var dx = (ev.clientX - drag.startX) / S.zoom;
    var dy = (ev.clientY - drag.startY) / S.zoom;

    drag.items.forEach(function (it) {
        var r = UI.clone(it.rect);
        if (!drag.handle) {
            r.left += dx; r.top += dy;
        } else {
            var h = drag.handle;
            if (h.indexOf('w') >= 0) { r.left += dx; r.width -= dx; }
            if (h.indexOf('e') >= 0) { r.width += dx; }
            if (h.indexOf('n') >= 0) { r.top += dy; r.height -= dy; }
            if (h.indexOf('s') >= 0) { r.height += dy; }
            r.width = Math.max(4, r.width);
            r.height = Math.max(4, r.height);
        }
        r.right = r.left + r.width; r.bottom = r.top + r.height;
        r.centerX = r.left + r.width / 2; r.centerY = r.top + r.height / 2;
        snapRect(r, it.node);
        r.left = snapValue(r.left); r.top = snapValue(r.top);
        if (drag.handle) { r.width = snapValue(r.width); r.height = snapValue(r.height); }
        it.node.size = [Math.round(r.width), Math.round(r.height)];
        it.node.position = rectToPosition(r, it.node, it.box);
    });

    drawStage();
    drawProps();
});

window.addEventListener('mouseup', function () {
    if (!drag) return;
    drag = null;
    runCheck(true);
    redrawAll();
});

/* ===================================================================== *
 * The tree
 * ===================================================================== */

function drawTree() {
    el.tree.innerHTML = '';
    (function emit(list, depth) {
        list.forEach(function (n) {
            var row = document.createElement('div');
            row.className = 'node' + (S.sel.indexOf(n.id) >= 0 ? ' sel' : '') +
                            (n.visible === false ? ' hidden' : '');
            row.style.paddingLeft = (6 + depth * 12) + 'px';
            row.innerHTML = '<span class="kind">' + n.type.slice(0, 3) + '</span>' +
                            '<span class="nm"></span><span class="grow"></span>' +
                            '<span class="eye">' + (n.visible === false ? 'off' : 'on') + '</span>';
            row.querySelector('.nm').textContent = n.name;
            row.onclick = function (ev) { select(n.id, ev.ctrlKey); };
            row.ondblclick = function () {
                var name = prompt('Widget name (letters, digits and underscores; this is the name a script looks up)', n.name);
                if (!name) return;
                pushUndo();
                renameWidget(n, UI.uniqueName(S.design.widgets, name, n.id));
                redrawAll();
            };
            row.querySelector('.eye').onclick = function (ev) {
                ev.stopPropagation();
                pushUndo();
                n.visible = n.visible === false;
                redrawAll();
            };
            el.tree.appendChild(row);
            if ((n.children || []).length) emit(n.children, depth + 1);
        });
    })(S.design.widgets, 0);
}

// A rename has to carry the strings entry and every animation track with it,
// because the name IS the handle everything else holds.
function renameWidget(node, next) {
    var was = node.name;
    if (was === next) return;
    if (Object.prototype.hasOwnProperty.call(S.design.strings, was)) {
        S.design.strings[next] = S.design.strings[was];
        delete S.design.strings[was];
    }
    (S.design.animations || []).forEach(function (c) {
        (c.tracks || []).forEach(function (t) { if (t.widget === was) t.widget = next; });
    });
    node.name = next;
}

function addWidget(type) {
    pushUndo();
    var parent = first();
    var node = UI.makeNode(type, {
        position: [0, 0],
        size: type === 'Text' ? [200, 32] : (type === 'Image' ? [48, 48] : [240, 120]),
        anchor: 'TopLeft'
    });
    node.name = UI.uniqueName(S.design.widgets, type.toLowerCase(), node.id);
    if (type === 'Text') S.design.strings[node.name] = 'TEXT';
    if (parent && parent.type === 'Container') parent.children.push(node);
    else if (parent) {
        var p = UI.parentOf(S.design.widgets, parent.id);
        (p ? p.children : S.design.widgets).push(node);
    } else S.design.widgets.push(node);
    S.sel = [node.id];
    redrawAll();
}

function deleteSelection() {
    var nodes = selNodes();
    if (!nodes.length) return;
    pushUndo();
    nodes.forEach(function (n) {
        var list = siblingsOf(n);
        var i = list.indexOf(n);
        if (i >= 0) list.splice(i, 1);
        UI.walk([n], function (x) { delete S.design.strings[x.name]; });
    });
    S.sel = [];
    redrawAll();
}

function duplicateSelection() {
    var nodes = selNodes();
    if (!nodes.length) return;
    pushUndo();
    var made = [];
    nodes.forEach(function (n) {
        var copy = UI.duplicateNode(n, S.design.widgets);
        // Carry the strings across under the new names.
        (function pair(a, b) {
            if (Object.prototype.hasOwnProperty.call(S.design.strings, a.name)) {
                S.design.strings[b.name] = S.design.strings[a.name];
            }
            (a.children || []).forEach(function (c, i) { pair(c, b.children[i]); });
        })(n, copy);
        copy.position = [copy.position[0] + 16, copy.position[1] + 16];
        siblingsOf(n).push(copy);
        made.push(copy.id);
    });
    S.sel = made;
    redrawAll();
}

function groupSelection() {
    var nodes = selNodes();
    if (!nodes.length) return;
    pushUndo();
    var box = boxFor(nodes[0]);
    var rects = nodes.map(function (n) { return layoutCache[n.id]; });
    var left = Math.min.apply(null, rects.map(function (r) { return r.left; }));
    var top = Math.min.apply(null, rects.map(function (r) { return r.top; }));
    var right = Math.max.apply(null, rects.map(function (r) { return r.right; }));
    var bottom = Math.max.apply(null, rects.map(function (r) { return r.bottom; }));

    var g = UI.makeNode('Container', { anchor: 'TopLeft', size: [right - left, bottom - top],
        bgFill: 'None', bgAlpha: 0 });
    g.name = UI.uniqueName(S.design.widgets, 'group', g.id);
    g.position = rectToPosition(UI.rect(left, top, right - left, bottom - top), g, box);

    var list = siblingsOf(nodes[0]);
    nodes.forEach(function (n) {
        var i = siblingsOf(n).indexOf(n);
        if (i >= 0) siblingsOf(n).splice(i, 1);
        // Keep each child where it was, now measured from the group's corner.
        var r = layoutCache[n.id];
        n.anchor = 'TopLeft';
        n.position = [Math.round(r.left - left), Math.round(r.top - top)];
        g.children.push(n);
    });
    list.push(g);
    S.sel = [g.id];
    redrawAll();
}

function reorder(delta) {
    var n = first();
    if (!n) return;
    pushUndo();
    var list = siblingsOf(n);
    var i = list.indexOf(n);
    var j = i + delta;
    if (j < 0 || j >= list.length) return;
    list.splice(i, 1);
    list.splice(j, 0, n);
    redrawAll();
}

/* ===================================================================== *
 * The property panel
 * ===================================================================== */

function rowEl(label, inner) {
    var r = document.createElement('div');
    r.className = 'row';
    var l = document.createElement('label');
    l.textContent = label;
    r.appendChild(l);
    if (inner) r.appendChild(inner);
    return r;
}

function numberInput(value, onChange, step) {
    var i = document.createElement('input');
    i.type = 'number';
    i.value = value;
    if (step) i.step = step;
    i.onchange = function () { pushUndo(); onChange(Number(i.value)); redrawAll(); };
    return i;
}

function applyAll(fn) {
    pushUndo();
    selNodes().forEach(fn);
    redrawAll();
}

function drawProps() {
    var n = first();
    el.props.innerHTML = '';
    if (!n) {
        var h = document.createElement('div');
        h.className = 'hint';
        h.textContent = 'Select a widget. The canvas is the 1920 by 1080 safe area, and a position is an inward inset from the edge its anchor names.';
        el.props.appendChild(h);
        return;
    }

    // ---- identity ----
    var nameIn = document.createElement('input');
    nameIn.type = 'text'; nameIn.value = n.name;
    nameIn.onchange = function () {
        pushUndo();
        renameWidget(n, UI.uniqueName(S.design.widgets, nameIn.value, n.id));
        redrawAll();
    };
    el.props.appendChild(rowEl('Name', nameIn));

    var kind = document.createElement('span');
    kind.className = 'pill on';
    kind.textContent = n.type;
    el.props.appendChild(rowEl('Type', kind));

    // ---- box ----
    var pos = document.createElement('div');
    pos.style.display = 'flex'; pos.style.gap = '4px'; pos.style.flex = '1';
    pos.appendChild(numberInput(n.position[0], function (v) { n.position[0] = v; }));
    pos.appendChild(numberInput(n.position[1], function (v) { n.position[1] = v; }));
    el.props.appendChild(rowEl('Inset x y', pos));

    var size = document.createElement('div');
    size.style.display = 'flex'; size.style.gap = '4px'; size.style.flex = '1';
    size.appendChild(numberInput(n.size[0], function (v) { n.size[0] = v; }));
    size.appendChild(numberInput(n.size[1], function (v) { n.size[1] = v; }));
    el.props.appendChild(rowEl('Size w h', size));

    el.props.appendChild(rowEl('Padding', numberInput(n.padding, function (v) {
        selNodes().forEach(function (x) { x.padding = v; });
    })));

    // ---- anchor, as the 3 by 3 it is ----
    var ap = document.createElement('div');
    ap.className = 'anchorpick';
    UI.ANCHORS.forEach(function (a, i) {
        var c = document.createElement('div');
        c.className = 'ap-' + i + (n.anchor === a ? ' on' : '');
        c.title = a + '  (the widget aligns its own ' + a + ' point, and the inset runs inward from there)';
        c.onclick = function () { applyAll(function (x) { x.anchor = a; }); };
        ap.appendChild(c);
    });
    el.props.appendChild(rowEl('Anchor', ap));

    // ---- alignment inside the parent ----
    var al = document.createElement('div');
    al.style.display = 'flex'; al.style.gap = '3px'; al.style.flex = '1';
    [['L', 0, null], ['C', 0.5, null], ['R', 1, null], ['T', null, 0], ['M', null, 0.5], ['B', null, 1]]
        .forEach(function (a) {
            var b = document.createElement('button');
            b.className = 'tiny ghost';
            b.textContent = a[0];
            b.title = 'Align inside the parent';
            b.onclick = function () {
                applyAll(function (x) {
                    var box = boxFor(x);
                    var r = UI.clone(layoutCache[x.id]);
                    if (a[1] !== null) r.left = box.left + (box.width - r.width) * a[1];
                    if (a[2] !== null) r.top = box.top + (box.height - r.height) * a[2];
                    x.position = rectToPosition(r, x, box);
                });
            };
            al.appendChild(b);
        });
    el.props.appendChild(rowEl('Align', al));

    // ---- fill, as swatches ----
    var fills = document.createElement('div');
    fills.className = 'fills';
    UI.BG_FILLS.forEach(function (f) {
        var sw = document.createElement('div');
        sw.className = 'fill-sw' + (n.bgFill === f ? ' on' : '');
        sw.title = f;
        var inner = document.createElement('div');
        inner.style.position = 'absolute'; inner.style.inset = '0';
        fillStyle({ bgFill: f, bgColor: n.bgColor, bgAlpha: 1 }, inner);
        sw.appendChild(inner);
        var lab = document.createElement('span');
        lab.textContent = f.replace('Gradient', 'GR ').replace('Outline', 'OUT ');
        sw.appendChild(lab);
        sw.onclick = function () { applyAll(function (x) { x.bgFill = f; }); };
        fills.appendChild(sw);
    });
    el.props.appendChild(rowEl('Fill', fills));

    colorRows('Bg colour', n, 'bgColor', 'bgAlpha');

    drawLayerControl(n);

    var vis = document.createElement('input');
    vis.type = 'checkbox'; vis.checked = n.visible !== false;
    vis.onchange = function () { applyAll(function (x) { x.visible = vis.checked; }); };
    el.props.appendChild(rowEl('Visible', vis));

    // ---- text ----
    if (n.type === 'Text') {
        var head = document.createElement('div');
        head.className = 'sect'; head.textContent = 'Text';
        el.props.appendChild(head);

        var tmpl = document.createElement('input');
        tmpl.type = 'text';
        tmpl.value = S.design.strings[n.name] === undefined ? (n.textLabel || '') : S.design.strings[n.name];
        tmpl.title = 'The strings.json template. Up to three {} slots; the engine has no free text.';
        tmpl.onchange = function () {
            pushUndo();
            S.design.strings[n.name] = tmpl.value;
            n.textLabel = tmpl.value;
            redrawAll();
        };
        el.props.appendChild(rowEl('Template', tmpl));

        var slots = UI.countSlots(tmpl.value);
        var note = document.createElement('span');
        note.className = 'pill' + (slots > 3 ? ' bad' : (slots ? ' on' : ''));
        note.textContent = slots + ' of 3 args';
        el.props.appendChild(rowEl('Slots', note));

        if (/[^\x20-\x7E]/.test(tmpl.value)) {
            var warn = document.createElement('span');
            warn.className = 'pill bad';
            warn.textContent = 'Not ASCII: renders as boxes';
            el.props.appendChild(rowEl('Font', warn));
        }

        el.props.appendChild(rowEl('Text size', numberInput(n.textSize, function (v) {
            selNodes().forEach(function (x) { x.textSize = v; });
        })));

        var ta = document.createElement('div');
        ta.className = 'anchorpick';
        UI.ANCHORS.forEach(function (a, i) {
            var c = document.createElement('div');
            c.className = 'ap-' + i + ((n.textAnchor || 'Center') === a ? ' on' : '');
            c.title = a;
            c.onclick = function () { applyAll(function (x) { x.textAnchor = a; }); };
            ta.appendChild(c);
        });
        el.props.appendChild(rowEl('Text anchor', ta));

        colorRows('Text colour', n, 'textColor', 'textAlpha');

        var blur = document.createElement('input');
        blur.type = 'checkbox'; blur.checked = !!n.textBgBlur;
        blur.title = 'SetUITextBgBlur: a blur behind the glyphs so text stays readable over a busy scene.';
        blur.onchange = function () { applyAll(function (x) { x.textBgBlur = blur.checked; }); };
        el.props.appendChild(rowEl('Text blur', blur));
    }

    // ---- image ----
    if (n.type === 'WeaponImage') {
        var weaponInfo = document.createElement('div');
        weaponInfo.className = 'sect';
        weaponInfo.textContent = 'Weapon: ' + n.weapon + '. Attachments: ' + ((n.attachments || []).join(', ') || 'Factory') + '. Change the preset in the spawner Loadout menu, then generate its card again. Layout edits are preserved.';
        el.props.appendChild(weaponInfo);
    }
    if (n.type === 'Image') {
        var ih = document.createElement('div');
        ih.className = 'sect'; ih.textContent = 'Image';
        el.props.appendChild(ih);

        var grid = document.createElement('div');
        grid.className = 'icons';
        UI.IMAGE_TYPES.forEach(function (t) {
            var b = document.createElement('div');
            b.className = 'icon-sw' + ((n.imageType || 'None') === t ? ' on' : '');
            b.title = t;
            b.innerHTML = iconSvg(t, 'currentColor') + '<b>' + t.replace('TEMP_Portal', 'PORTAL').slice(0, 9) + '</b>';
            b.onclick = function () { applyAll(function (x) { x.imageType = t; }); };
            grid.appendChild(b);
        });
        el.props.appendChild(rowEl('Icon', grid));
        colorRows('Tint', n, 'imageColor', 'imageAlpha');
        var hint = document.createElement('div');
        hint.className = 'hint';
        hint.textContent = 'Built-in icons only. Gadget and weapon art comes from AddUIGadgetImage and AddUIWeaponImage, which take an item rather than a picture.';
        el.props.appendChild(hint);
    }

    // ---- button ----
    if (n.type === 'Button') {
        var bh = document.createElement('div');
        bh.className = 'sect'; bh.textContent = 'Button states';
        el.props.appendChild(bh);

        var preview = document.createElement('div');
        preview.style.display = 'flex'; preview.style.gap = '3px'; preview.style.flex = '1';
        ['base', 'hover', 'pressed'].forEach(function (st) {
            var b = document.createElement('button');
            b.className = 'tiny' + (S.hoverState === st ? ' primary' : ' ghost');
            b.textContent = st;
            b.onclick = function () { S.hoverState = st; redrawAll(); };
            preview.appendChild(b);
        });
        el.props.appendChild(rowEl('Preview', preview));

        ['Base', 'Hover', 'Pressed', 'Focused', 'Disabled'].forEach(function (st) {
            colorRows(st, n, 'buttonColor' + st, 'buttonAlpha' + st);
        });

        var en = document.createElement('input');
        en.type = 'checkbox'; en.checked = n.buttonEnabled !== false;
        en.onchange = function () { applyAll(function (x) { x.buttonEnabled = en.checked; }); };
        el.props.appendChild(rowEl('Enabled', en));

        var bhint = document.createElement('div');
        bhint.className = 'hint';
        bhint.textContent = 'The export switches every UIButtonEvent on for this widget and routes ButtonUp by name in onUiButton.';
        el.props.appendChild(bhint);
    }
}

/*
 * LAYER: which side of the game's own HUD this widget draws on.
 *
 * The engine property is UIDepth, and it is per WIDGET: every AddUI* call
 * takes one, and SetUIWidgetDepth writes one on any widget at any time. The
 * builder adds Inherit on top of the engine's two, so a container can be moved
 * and its whole subtree goes with it; inheritance is resolved on export, and
 * the word Inherit never reaches a Portal script.
 */
function drawLayerControl(n) {
    var head = document.createElement('div');
    head.className = 'sect';
    head.textContent = 'Layer';
    el.props.appendChild(head);

    var resolved = UI.resolveDepths(S.design)[n.id] || 'AboveGameUI';
    var parent = UI.parentOf(S.design.widgets, n.id);

    var box = document.createElement('div');
    box.className = 'layer';
    [['AboveGameUI', 'Over the game HUD'],
     ['BelowGameUI', 'Under the game HUD'],
     ['Inherit', parent ? 'Same as parent' : 'Design default']].forEach(function (pair) {
        var b = document.createElement('button');
        b.textContent = pair[1];
        b.title = pair[0] === 'Inherit'
            ? (parent
                ? 'Follow ' + parent.name + '. Move the parent and this moves with it.'
                : 'Follow the design default below.')
            : 'mod.UIDepth.' + pair[0];
        if (n.depth === pair[0]) b.className = 'on';
        b.onclick = function () {
            pushUndo();
            UI.setDepth(S.design, S.sel, pair[0], false);
            redrawAll();
        };
        box.appendChild(b);
    });
    el.props.appendChild(rowEl('This widget', box));

    var hint = document.createElement('div');
    hint.className = 'layerhint';
    hint.textContent = 'Drawing ' + (resolved === 'BelowGameUI' ? 'UNDER' : 'OVER') +
        ' the game HUD (mod.UIDepth.' + resolved + ').' +
        (n.depth === 'Inherit' ? ' Inherited.' : '');
    el.props.appendChild(hint);

    if ((n.children || []).length || S.sel.length > 1) {
        var bulk = document.createElement('div');
        bulk.className = 'layer';
        [['AboveGameUI', 'All over'], ['BelowGameUI', 'All under']].forEach(function (pair) {
            var b = document.createElement('button');
            b.textContent = pair[1];
            b.title = 'Set the selection to ' + UI.DEPTH_LABEL[pair[0]].toLowerCase() +
                      ' and put every widget inside it back to Inherit, so the whole tree follows.';
            b.onclick = function () {
                pushUndo();
                var n2 = UI.setDepth(S.design, S.sel, pair[0], true);
                redrawAll();
                say('Moved ' + n2 + ' widget(s) ' + UI.DEPTH_LABEL[pair[0]].toLowerCase() + '.');
            };
            bulk.appendChild(b);
        });
        el.props.appendChild(rowEl('Whole subtree', bulk));
    }

    // The design-level default, which every Inherit root falls back to.
    var def = document.createElement('div');
    def.className = 'layer';
    UI.DEPTHS.forEach(function (dv) {
        var b = document.createElement('button');
        b.textContent = UI.DEPTH_LABEL[dv];
        b.title = 'Every widget set to Inherit, all the way to the roots, moves with this.';
        if (S.design.defaultDepth === dv) b.className = 'on';
        b.onclick = function () {
            pushUndo();
            S.design.defaultDepth = dv;
            redrawAll();
        };
        def.appendChild(b);
    });
    el.props.appendChild(rowEl('Design default', def));

    // The other half of the answer, when a design and the HUD both want the
    // same corner: switch that HUD element off on the site.
    var link = document.createElement('div');
    link.className = 'layerlink';
    var a = document.createElement('a');
    a.textContent = 'Hide game HUD elements...';
    a.title = 'Open the tool settings on the Modifiers > UI page, where the compass, ' +
              'the minimap and the rest can be switched off for the experience.';
    a.onclick = function () { send({ op: 'openSettings', page: 'ui' }); };
    link.appendChild(a);
    el.props.appendChild(link);
}

// A colour and its alpha: the BF palette, three 0..1 channels and a slider.
function colorRows(label, node, colorKey, alphaKey) {
    var c = node[colorKey] || [1, 1, 1];

    var sw = document.createElement('div');
    sw.className = 'swatches';
    UI.PALETTE.forEach(function (p) {
        var s = document.createElement('div');
        s.className = 'sw';
        s.style.background = p.hex;
        s.title = p.name + (p.group ? '  (' + p.group + ')' : '') + '  ' + JSON.stringify(p.rgb) + '  ' + p.hex;
        s.onclick = function () { applyAll(function (x) { x[colorKey] = p.rgb.slice(); }); };
        sw.appendChild(s);
    });
    var picker = document.createElement('input');
    picker.type = 'color';
    picker.style.width = '24px'; picker.style.height = '20px'; picker.style.padding = '0';
    picker.value = '#' + c.map(function (v) {
        var h = Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16);
        return h.length < 2 ? '0' + h : h;
    }).join('');
    picker.oninput = function () {
        var hex = picker.value;
        var rgbv = [parseInt(hex.substr(1, 2), 16) / 255, parseInt(hex.substr(3, 2), 16) / 255,
                    parseInt(hex.substr(5, 2), 16) / 255].map(function (v) { return Math.round(v * 1000) / 1000; });
        selNodes().forEach(function (x) { x[colorKey] = rgbv.slice(); });
        drawStage();
    };
    picker.onchange = function () { pushUndo(); redrawAll(); };
    sw.appendChild(picker);
    el.props.appendChild(rowEl(label, sw));

    var ch = document.createElement('div');
    ch.style.display = 'flex'; ch.style.gap = '3px'; ch.style.flex = '1';
    [0, 1, 2].forEach(function (i) {
        ch.appendChild(numberInput(c[i], function (v) {
            selNodes().forEach(function (x) {
                var arr = (x[colorKey] || [1, 1, 1]).slice();
                arr[i] = Math.max(0, Math.min(1, v));
                x[colorKey] = arr;
            });
        }, '0.01'));
    });
    el.props.appendChild(rowEl('r g b 0..1', ch));

    if (alphaKey) {
        var wrap = document.createElement('div');
        wrap.style.display = 'flex'; wrap.style.gap = '4px'; wrap.style.flex = '1';
        var sl = document.createElement('input');
        sl.type = 'range'; sl.min = '0'; sl.max = '1'; sl.step = '0.01';
        sl.value = node[alphaKey] === undefined ? 1 : node[alphaKey];
        var val = document.createElement('span');
        val.className = 'val';
        val.textContent = Number(sl.value).toFixed(2);
        sl.oninput = function () {
            val.textContent = Number(sl.value).toFixed(2);
            selNodes().forEach(function (x) { x[alphaKey] = Number(sl.value); });
            drawStage();
        };
        sl.onchange = function () { pushUndo(); redrawAll(); };
        wrap.appendChild(sl); wrap.appendChild(val);
        el.props.appendChild(rowEl('Alpha', wrap));
    }
}

/* ===================================================================== *
 * Live values
 * ===================================================================== */

function drawLive() {
    el.live.innerHTML = '';
    var texts = [];
    UI.walk(S.design.widgets, function (n) {
        if (n.type !== 'Text') return;
        var tmpl = S.design.strings[n.name];
        if (tmpl === undefined || UI.countSlots(tmpl) === 0) return;
        texts.push(n);
    });
    el.liveHint.style.display = texts.length ? 'none' : 'block';
    texts.forEach(function (n) {
        var slots = Math.min(3, UI.countSlots(S.design.strings[n.name]));
        var wrap = document.createElement('div');
        wrap.style.display = 'flex'; wrap.style.gap = '3px'; wrap.style.flex = '1';
        for (var i = 0; i < slots; i++) {
            (function (idx) {
                var inp = document.createElement('input');
                inp.type = 'text';
                inp.style.width = '100%';
                inp.value = (n.textArgs || [])[idx] || '';
                inp.oninput = function () {
                    n.textArgs = n.textArgs || [];
                    n.textArgs[idx] = inp.value;
                    drawStage();
                };
                wrap.appendChild(inp);
            })(i);
        }
        el.live.appendChild(rowEl(n.name, wrap));
    });
}

/* ===================================================================== *
 * The check
 * ===================================================================== */

function runCheck(quiet) {
    S.issues = UI.auditAll(S.design);
    el.pillIssues.textContent = S.issues.length + ' issue' + (S.issues.length === 1 ? '' : 's');
    el.pillIssues.className = 'pill' + (S.issues.length ? ' bad' : ' good');
    drawIssues();
    if (!quiet) {
        say(S.issues.length
            ? S.issues.length + ' finding(s) across ' + UI.ASPECTS.length + ' aspect presets.'
            : 'Clean on every aspect preset.');
    }
}

function drawIssues() {
    el.issues.innerHTML = '';
    if (!S.issues.length) {
        var h = document.createElement('div');
        h.className = 'hint';
        h.textContent = 'Nothing clips, spills, overlaps, drifts or lands on the game HUD, on 16:9, 21:9, 32:9 or 16:10.';
        el.issues.appendChild(h);
        return;
    }
    // Widget-to-widget faults first, then the game HUD, because the fixes are
    // different: one is a layout problem, the other is a layering decision.
    var ordered = S.issues.filter(function (f) { return f.kind !== 'hud'; })
        .concat(S.issues.filter(function (f) { return f.kind === 'hud'; }));

    ordered.slice(0, 200).forEach(function (f) {
        var d = document.createElement('div');
        d.className = 'issue ' + f.kind;
        d.innerHTML = '<span class="k">' + f.kind + '</span><span class="asp">' + f.aspect + '</span>';
        d.appendChild(document.createTextNode(f.text));
        d.onclick = function () {
            for (var i = 0; i < UI.ASPECTS.length; i++) {
                if (UI.ASPECTS[i].id === f.aspect) { S.aspect = UI.ASPECTS[i].ratio; el.aspect.value = f.aspect; }
            }
            select(f.id);
        };
        el.issues.appendChild(d);
    });
}

/* ===================================================================== *
 * The timeline
 * ===================================================================== */

function ensureClip() {
    if (!S.design.animations) S.design.animations = [];
    if (!S.clip || S.design.animations.indexOf(S.clip) < 0) {
        S.clip = S.design.animations[0] || null;
    }
    return S.clip;
}

function newClip() {
    pushUndo();
    var c = { name: 'clip' + ((S.design.animations || []).length + 1), duration: 60, loop: false, tracks: [] };
    S.design.animations = S.design.animations || [];
    S.design.animations.push(c);
    S.clip = c;
    redrawAll();
}

function keyAtPlayhead() {
    var n = first();
    if (!n) { say('Select the widget to key first.'); return; }
    if (!ensureClip()) { newClip(); }
    pushUndo();
    var prop = S.keyProp;
    var track = null;
    (S.clip.tracks || []).forEach(function (t) {
        if (t.widget === n.name && t.property === prop) track = t;
    });
    if (!track) { track = { widget: n.name, property: prop, keys: [] }; S.clip.tracks.push(track); }

    var v = n[prop];
    if (Array.isArray(v)) v = v.slice();
    var t = Math.round(S.playhead);
    var existing = null;
    track.keys.forEach(function (k) { if (k.t === t) existing = k; });
    if (existing) { existing.v = v; existing.ease = S.keyEase; }
    else track.keys.push({ t: t, v: v, ease: S.keyEase });
    track.keys.sort(function (a, b) { return a.t - b.t; });
    redrawAll();
    say('Keyed ' + n.name + '.' + prop + ' at tick ' + t + '.');
}

function deleteKey() {
    if (!S.selKey || !S.clip) return;
    pushUndo();
    var tr = S.clip.tracks[S.selKey.track];
    if (!tr) return;
    tr.keys.splice(S.selKey.index, 1);
    if (!tr.keys.length) S.clip.tracks.splice(S.selKey.track, 1);
    S.selKey = null;
    redrawAll();
}

function drawTimeline() {
    var c = ensureClip();
    el.clipPick.innerHTML = '';
    (S.design.animations || []).forEach(function (x, i) {
        var o = document.createElement('option');
        o.value = String(i); o.textContent = x.name;
        if (x === c) o.selected = true;
        el.clipPick.appendChild(o);
    });
    if (!c) {
        el.tlrows.innerHTML = '<div class="hint">No clip yet. NEW CLIP starts one; then select a widget, pick a property and press KEY at each tick that matters.</div>';
        return;
    }
    el.clipLoop.checked = !!c.loop;
    el.clipDur.value = c.duration;

    el.tlrows.innerHTML = '';
    var dur = Math.max(1, Number(c.duration) || 60);

    (c.tracks || []).forEach(function (tr, ti) {
        var row = document.createElement('div');
        row.className = 'tlrow';
        var nm = document.createElement('div');
        nm.className = 'tlname';
        nm.innerHTML = '<span class="prop">' + tr.property + '</span> ';
        nm.appendChild(document.createTextNode(tr.widget));
        row.appendChild(nm);

        var track = document.createElement('div');
        track.className = 'tltrack';
        track.onclick = function (ev) {
            var r = track.getBoundingClientRect();
            S.playhead = Math.round(((ev.clientX - r.left) / r.width) * dur);
            drawTimeline(); drawStage();
        };
        (tr.keys || []).forEach(function (k, ki) {
            var d = document.createElement('div');
            d.className = 'key' + (S.selKey && S.selKey.track === ti && S.selKey.index === ki ? ' sel' : '');
            d.style.left = ((k.t / dur) * 100) + '%';
            d.title = 'tick ' + k.t + '  ' + JSON.stringify(k.v) + '  ' + (k.ease || 'linear');
            d.onclick = function (ev) {
                ev.stopPropagation();
                S.selKey = { track: ti, index: ki };
                S.playhead = k.t;
                S.keyEase = k.ease || 'linear';
                el.keyEase.value = S.keyEase;
                drawTimeline(); drawStage();
            };
            track.appendChild(d);
        });
        var ph = document.createElement('div');
        ph.id = 'playhead';
        ph.style.left = ((Math.min(S.playhead, dur) / dur) * 100) + '%';
        track.appendChild(ph);
        row.appendChild(track);
        el.tlrows.appendChild(row);
    });

    if (!(c.tracks || []).length) {
        el.tlrows.innerHTML += '<div class="hint">This clip has no tracks. Select a widget, choose a property and press KEY.</div>';
    }
}

var playTimer = null;
function togglePlay() {
    if (playTimer) { clearInterval(playTimer); playTimer = null; el.btnPlay.textContent = 'Play'; return; }
    var c = ensureClip();
    if (!c) return;
    el.btnPlay.textContent = 'Stop';
    playTimer = setInterval(function () {
        var dur = Math.max(1, Number(c.duration) || 60);
        S.playhead = S.playhead + 1;
        if (S.playhead > dur) S.playhead = c.loop ? 0 : dur;
        drawStage();
        drawTimeline();
    }, 1000 / 30);
}

/* ===================================================================== *
 * Export and import
 * ===================================================================== */

var FORMATS = [
    { id: 'typescript', label: 'ParseUI TypeScript', note: 'self-contained mod.AddUI tree, plus getUi and the button router' },
    { id: 'strings', label: 'strings.json', note: 'the templates the text widgets need' },
    { id: 'anim', label: 'Animation clips', note: 'pairs with runtime/ui-anim.ts' },
    { id: 'deluca', label: 'UI module', note: 'bf6-portal-utils classes' },
    { id: 'solid', label: 'solid-ui', note: 'reactive h() tree' },
    { id: 'blocks', label: 'Blocks snippet', note: 'AddUI and SetUI tree, ready to paste' },
    { id: 'community', label: 'Community builder', note: 'loads in tools.bfportal.gg' },
    { id: 'design', label: 'Design file', note: 'this tool owns it' }
];

function openSheet(format) {
    S.design.name = el.designName.value || S.design.name;
    S.exports = UI.exportAll(S.design);
    S.sheetFormat = format || S.sheetFormat;
    el.fmtcol.innerHTML = '';
    FORMATS.forEach(function (f) {
        var d = document.createElement('div');
        d.className = 'fmt' + (f.id === S.sheetFormat ? ' on' : '');
        d.innerHTML = f.label + '<small>' + f.note + '</small>';
        d.onclick = function () { S.sheetFormat = f.id; openSheet(f.id); };
        el.fmtcol.appendChild(d);
    });
    el.sheettext.value = S.exports[S.sheetFormat] || '';
    el.warns.innerHTML = '';
    (S.exports.warnings || []).forEach(function (w) {
        var d = document.createElement('div');
        d.textContent = w;
        el.warns.appendChild(d);
    });
    el.sheet.classList.add('on');
}

function doImport() {
    var text = prompt('Paste a design file, a community builder export, or a ParseUI tree.');
    if (!text) return;
    var strings = null;
    if (text.indexOf('mod.stringkeys') >= 0) {
        strings = prompt('Paste the matching strings.json, or leave this empty.') || null;
    }
    try {
        var d = UI.importAny(text, strings, el.designName.value || 'imported');
        pushUndo();
        S.design = d;
        S.sel = [];
        el.designName.value = d.name;
        redrawAll();
        say('Imported ' + UI.allNames(d.widgets).length + ' widget(s).');
    } catch (e) {
        say('That did not import: ' + e.message);
    }
}

function loadTemplate(name) {
    try {
        pushUndo();
        S.design = UI.template(name);
        S.sel = [];
        S.clip = null;
        el.designName.value = S.design.name;
        redrawAll();
        say('Loaded the ' + name + ' template.');
    } catch (e) { say(e.message); }
}

/* ===================================================================== *
 * Wiring
 * ===================================================================== */

/* TELLING THE TOOL THERE IS WORK WORTH KEEPING.
 *
 * A design lived only in this page between one deliberate SAVE TO FILE and the
 * next, so closing the panel, reloading it, or an editor restart lost
 * everything since the last save with nothing written anywhere.
 *
 * The host keeps what arrives here as a RECOVERY copy, deliberately not as the
 * saved design: an autosave is not somebody pressing save, and it must never
 * overwrite the file they did press save on. It offers the recovery copy back
 * when it is newer.
 *
 * Sent from redrawAll and not from pushUndo, because pushUndo runs BEFORE a
 * change is applied and would file the state the user just moved away from.
 * Debounced, because dragging a widget redraws every frame and none of those
 * frames is worth a write.
 */
var designStateTimer = null;
var designStateLast = '';
function noteDesignChanged() {
    if (designStateTimer) { clearTimeout(designStateTimer); }
    designStateTimer = setTimeout(function () {
        designStateTimer = null;
        try {
            var json = JSON.stringify(S.design);
            /* Selection and redraws that changed nothing are common; writing an
             * identical copy would churn the disk for no gain. */
            if (json === designStateLast) { return; }
            designStateLast = json;
            send({ op: 'designState', name: S.design.name || 'design', json: json });
        } catch (e) { /* a design that cannot be serialised is not worth a crash here */ }
    }, 1500);
}

function redrawAll() {
    S.design.name = el.designName.value || S.design.name;
    drawStage();
    drawTree();
    drawProps();
    drawLive();
    drawTimeline();
    runCheck(true);
    noteDesignChanged();
}

function fillSelect(sel, items, value) {
    sel.innerHTML = '';
    items.forEach(function (it) {
        var o = document.createElement('option');
        o.value = it.value === undefined ? it : it.value;
        o.textContent = it.label === undefined ? it : it.label;
        if (o.value === String(value)) o.selected = true;
        sel.appendChild(o);
    });
}

fillSelect(el.aspect, UI.ASPECTS.map(function (a) { return { value: a.id, label: a.label }; }), '16:9');
fillSelect(el.zoom, ['0.35', '0.5', '0.65', '0.8', '1'], '0.5');
fillSelect(el.tplPick, [{ value: '', label: 'Template...' }].concat(
    UI.templateNames().map(function (n) { return { value: n, label: n }; })), '');
fillSelect(el.keyProp, UI.ANIMATABLE, 'bgAlpha');
fillSelect(el.keyEase, ['linear', 'easeIn', 'easeOut', 'easeInOut', 'step'], 'linear');

el.aspect.onchange = function () {
    for (var i = 0; i < UI.ASPECTS.length; i++) {
        if (UI.ASPECTS[i].id === el.aspect.value) S.aspect = UI.ASPECTS[i].ratio;
    }
    redrawAll();
};
el.zoom.onchange = function () { S.zoom = Number(el.zoom.value); redrawAll(); };
el.snapGrid.onchange = function () { S.snap = el.snapGrid.checked; };
el.tplPick.onchange = function () { if (el.tplPick.value) { loadTemplate(el.tplPick.value); el.tplPick.value = ''; } };
el.designName.onchange = function () { S.design.name = el.designName.value; };

el.btnNew.onclick = function () {
    pushUndo();
    S.design = UI.newDesign(el.designName.value || 'design');
    S.sel = []; S.clip = null;
    redrawAll();
};
el.btnUndo.onclick = undo;
el.btnRedo.onclick = redo;
el.btnDel.onclick = deleteSelection;
el.btnDup.onclick = duplicateSelection;
el.btnGroup.onclick = groupSelection;
el.btnUp.onclick = function () { reorder(-1); };
el.btnDown.onclick = function () { reorder(1); };
el.btnCheck.onclick = function () { runCheck(false); };
el.btnHud.onclick = function () {
    S.showHud = !S.showHud;
    el.btnHud.className = S.showHud ? 'primary' : 'ghost';
    drawStage();
};
el.btnHud.className = 'primary';
el.btnExport.onclick = function () { openSheet(S.sheetFormat); };
el.btnImport.onclick = doImport;
el.btnSheetClose.onclick = function () { el.sheet.classList.remove('on'); };

var addButtons = document.querySelectorAll('[data-add]');
for (var i = 0; i < addButtons.length; i++) {
    (function (b) { b.onclick = function () { addWidget(b.dataset.add); }; })(addButtons[i]);
}

el.btnClipNew.onclick = newClip;
el.btnPlay.onclick = togglePlay;
el.btnKey.onclick = keyAtPlayhead;
el.btnKeyDel.onclick = deleteKey;
el.keyProp.onchange = function () { S.keyProp = el.keyProp.value; };
el.keyEase.onchange = function () {
    S.keyEase = el.keyEase.value;
    if (S.selKey && S.clip) {
        pushUndo();
        var tr = S.clip.tracks[S.selKey.track];
        if (tr && tr.keys[S.selKey.index]) tr.keys[S.selKey.index].ease = S.keyEase;
        redrawAll();
    }
};
el.clipPick.onchange = function () {
    S.clip = (S.design.animations || [])[Number(el.clipPick.value)] || null;
    S.selKey = null;
    redrawAll();
};
el.clipLoop.onchange = function () { if (S.clip) { pushUndo(); S.clip.loop = el.clipLoop.checked; redrawAll(); } };
el.clipDur.onchange = function () { if (S.clip) { pushUndo(); S.clip.duration = Number(el.clipDur.value) || 60; redrawAll(); } };

el.btnCopy.onclick = function () {
    el.sheettext.select();
    try { document.execCommand('copy'); say('Copied the ' + S.sheetFormat + ' export.'); }
    catch (e) { say('Select the text and copy it by hand.'); }
};
el.btnSaveAs.onclick = function () {
    send({ op: 'exportFile', format: S.sheetFormat, text: el.sheettext.value, name: S.design.name });
    say('Asked the tool to write the ' + S.sheetFormat + ' export.');
};
el.btnToScript.onclick = function () {
    if (!S.exports) openSheet(S.sheetFormat);
    send({ op: 'insertScript', name: S.design.name,
           ts: S.exports.typescript, strings: S.exports.strings, anim: S.exports.anim });
    say('Handed the TypeScript to the script editor.');
};
el.btnToBlocks.onclick = function () {
    if (!S.exports) openSheet(S.sheetFormat);
    send({ op: 'insertBlocks', name: S.design.name, snippet: S.exports.blocks });
    say('Handed the block tree to the blocks editor.');
};

/* ===================================================================== *
 * THE SLIDE-OUT SHELF
 *
 * The design is the canvas and it fills the window. Everything that helps you
 * build it sits behind one small labelled handle on the left strip and slides
 * out over the canvas, the same way the scene tree's symbol legend sits on the
 * viewport: closed by default, one panel at a time, Escape closes it, and the
 * tool comes back the way you left it.
 *
 * ANIMATE is in the same group even though its panel is the strip along the
 * bottom, because "one thing open at a time" is the whole point and an
 * exception would undo it.
 * ===================================================================== */

var shelf = { open: false, pane: 'start', pinned: false, prefs: {}, applied: false };

var PANE_NAME = {
    start: 'Start here', layers: 'Layers', props: 'Settings',
    screen: 'Screen', anim: 'Animate', issues: 'What went wrong'
};

function setPref(name, value) {
    shelf.prefs[name] = String(value);
    send({ op: 'pref', name: name, value: String(value) });
}

function setTimeline(on) {
    if (S.timelineOpen === !!on) return;    // a full redraw is not free
    el.timeline.classList.toggle('on', !!on);
    S.timelineOpen = !!on;
    if (!S.timelineOpen && playTimer) togglePlay();
    redrawAll();
}

function markHandles(name) {
    var b = el.rail.querySelectorAll('.railbtn');
    for (var i = 0; i < b.length; i++) b[i].classList.toggle('on', b[i].dataset.pane === name);
}

function showPane(name) {
    shelf.pane = name;
    shelf.open = true;
    var panes = document.querySelectorAll('.pane');
    for (var i = 0; i < panes.length; i++) panes[i].classList.remove('on');
    markHandles(name);
    if (name === 'anim') {
        // Its panel is the bottom strip, so the drawer stays shut.
        el.side.classList.remove('open');
        el.stagewrap.classList.remove('padded');
        setTimeline(true);
    } else {
        setTimeline(false);
        var p = document.getElementById('pane-' + name);
        if (p) p.classList.add('on');
        el.side.classList.add('open');
        el.stagewrap.classList.add('padded');
        el.sideTitle.textContent = PANE_NAME[name] || name;
        if (name === 'props') drawProps();
        if (name === 'issues') drawIssues();
    }
    setPref('shelf', name);
    setPref('shelfOpen', true);
}

function closeShelf(remember) {
    shelf.open = false;
    var panes = document.querySelectorAll('.pane');
    for (var i = 0; i < panes.length; i++) panes[i].classList.remove('on');
    markHandles(null);
    el.side.classList.remove('open');
    el.stagewrap.classList.remove('padded');
    setTimeline(false);
    if (remember !== false) setPref('shelfOpen', false);
}

function toggleShelf(name) {
    if (shelf.open && shelf.pane === name) { closeShelf(); return; }
    showPane(name);
}

function setPinned(on) {
    shelf.pinned = !!on;
    el.sidePin.classList.toggle('on', shelf.pinned);
    el.sidePin.textContent = shelf.pinned ? 'Stays open' : 'Keep open';
    setPref('shelfPinned', shelf.pinned);
}

(function wireShelf() {
    var b = el.rail.querySelectorAll('.railbtn');
    for (var i = 0; i < b.length; i++) {
        (function (btn) { btn.onclick = function () { toggleShelf(btn.dataset.pane); }; })(b[i]);
    }
    el.sideClose.onclick = function () { closeShelf(); };
    el.sidePin.onclick = function () { setPinned(!shelf.pinned); };

    // Click away closes it, unless it is pinned. The strip, the panel and the
    // top bar are not "away".
    document.addEventListener('mousedown', function (ev) {
        if (!shelf.open || shelf.pinned) return;
        if (el.side.contains(ev.target) || el.rail.contains(ev.target)) return;
        if (el.timeline.contains(ev.target)) return;
        if (el.sheet.contains(ev.target)) return;
        var bar = ev.target.closest ? ev.target.closest('.bar') : null;
        if (bar) return;
        closeShelf();
    }, true);
})();

// The issue count is the one readout left on the bar, so it is also the way in
// to the panel that explains it.
el.pillIssues.style.cursor = 'pointer';
el.pillIssues.onclick = function () { showPane('issues'); };

function applyShelfPrefs(p) {
    if (shelf.applied) return;
    shelf.applied = true;
    shelf.prefs = p || {};
    setPinned(shelf.prefs.shelfPinned === 'true');
    var name = shelf.prefs.shelf && PANE_NAME[shelf.prefs.shelf] ? shelf.prefs.shelf : 'start';
    // Nothing remembered means a first run: open START HERE, so the window is
    // never a blank canvas with no way in.
    if (shelf.prefs.shelfOpen === undefined || shelf.prefs.shelfOpen === 'true') showPane(name);
    else { shelf.pane = name; closeShelf(false); }
}

// If the tool never answers, still open on the first-run panel.
setTimeout(function () { applyShelfPrefs(null); }, 2000);

document.addEventListener('keydown', function (ev) {
    if (ev.key !== 'Escape' || ev.defaultPrevented) return;
    if (el.sheet.classList.contains('on')) { el.sheet.classList.remove('on'); ev.preventDefault(); return; }
    if (shelf.open) { closeShelf(); ev.preventDefault(); }
});

document.addEventListener('keydown', function (ev) {
    if (ev.target.tagName === 'INPUT' || ev.target.tagName === 'TEXTAREA') return;
    var ctrl = ev.ctrlKey || ev.metaKey;
    if (ctrl && ev.key.toLowerCase() === 'z') { ev.shiftKey ? redo() : undo(); ev.preventDefault(); return; }
    if (ctrl && ev.key.toLowerCase() === 'y') { redo(); ev.preventDefault(); return; }
    if (ctrl && ev.key.toLowerCase() === 'd') { duplicateSelection(); ev.preventDefault(); return; }
    if (ctrl && ev.key.toLowerCase() === 'c') {
        S.clipboard = selNodes().map(function (n) {
            var strings = {};
            UI.walk([n], function (x) {
                if (S.design.strings[x.name] !== undefined) strings[x.name] = S.design.strings[x.name];
            });
            return { node: UI.clone(n), strings: strings };
        });
        say('Copied ' + S.clipboard.length + ' widget(s).');
        return;
    }
    if (ctrl && ev.key.toLowerCase() === 'v') {
        if (!S.clipboard || !S.clipboard.length) return;
        pushUndo();
        var made = [];
        S.clipboard.forEach(function (entry) {
            var copy = UI.duplicateNode(entry.node, S.design.widgets);
            (function pair(a, b) {
                if (entry.strings[a.name] !== undefined) S.design.strings[b.name] = entry.strings[a.name];
                (a.children || []).forEach(function (c, i) { pair(c, b.children[i]); });
            })(entry.node, copy);
            copy.position = [copy.position[0] + 16, copy.position[1] + 16];
            var target = first();
            (target && target.type === 'Container' ? target.children : S.design.widgets).push(copy);
            made.push(copy.id);
        });
        S.sel = made;
        redrawAll();
        return;
    }
    if (ev.key === 'Delete') { deleteSelection(); ev.preventDefault(); return; }
    if (ev.key.indexOf('Arrow') === 0) {
        var step = ev.shiftKey ? S.grid : 1;
        var dx = ev.key === 'ArrowLeft' ? -step : (ev.key === 'ArrowRight' ? step : 0);
        var dy = ev.key === 'ArrowUp' ? -step : (ev.key === 'ArrowDown' ? step : 0);
        var nodes = selNodes();
        if (!nodes.length) return;
        pushUndo();
        nodes.forEach(function (n) {
            var box = boxFor(n);
            var r = UI.clone(layoutCache[n.id]);
            r.left += dx; r.top += dy;
            n.position = rectToPosition(r, n, box);
        });
        redrawAll();
        ev.preventDefault();
    }
});

/* ===================================================================== */

el.designName.value = S.design.name;
redrawAll();
send({ op: 'ready', v: UI.VERSION });
say('BF6 UI Builder ' + UI.VERSION + '. The canvas is the 1920 by 1080 safe area; a position is an inward inset from the anchored edge.');

})();
