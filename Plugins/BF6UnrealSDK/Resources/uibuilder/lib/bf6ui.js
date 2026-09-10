/*
 * BF6UiBuilder core - the model, the layout maths, the exporters and the
 * importers for the BF6 UI BUILDER.
 *
 * Everything in here is pure: no DOM, no engine, no file system. The editor
 * page uses it, and so do the node tests, so the numbers the canvas draws and
 * the numbers the tests assert are produced by the same code.
 *
 * Loads in Node (module.exports) and in a browser (window.BF6Ui) with no
 * bundler. ES5 syntax only, so it also runs inside embedded script engines.
 *
 * GROUND TRUTH, and where it came from:
 *   element types, the nine anchors, the nine fills, the two depths, the
 *   built-in image types, the six button events and every per-widget property
 *     -> the community builder's own src/models/types.ts
 *        (github.com/battlefield-portal-community/ui_builder)
 *     -> research/guides/portal-ui-guide.md sections 1 to 4
 *   the block type names, their argument order and their field names
 *     -> research/_tools/portal-site/raw/blockly-blocks-141.json
 *     -> the real fixture night_ops_breakthrough_workspace.json
 *   the anchor law (offsets are INWARD insets from the anchored edge)
 *     -> memory bf6-ui-anchor-convention.md
 */
;(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module && module.exports) {
        module.exports = factory();
    } else {
        root.BF6Ui = factory();
    }
})(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var VERSION = '1.0.0';

    /* ==================================================================== *
     * The surface
     * ==================================================================== */

    var TYPES = ['Container', 'Text', 'Image', 'Button', 'WeaponImage', 'GadgetImage'];

    // The nine anchors, in the order a 3x3 picker reads them.
    var ANCHORS = [
        'TopLeft', 'TopCenter', 'TopRight',
        'CenterLeft', 'Center', 'CenterRight',
        'BottomLeft', 'BottomCenter', 'BottomRight'
    ];

    var BG_FILLS = [
        'Solid', 'Blur', 'None',
        'OutlineThin', 'OutlineThick',
        'GradientTop', 'GradientBottom', 'GradientLeft', 'GradientRight'
    ];

    /*
     * LAYERING AGAINST THE GAME'S OWN HUD.
     *
     * UIDepth is a per-WIDGET property, not a per-root one. Both halves of the
     * evidence say so:
     *   every AddUI* call takes a UIDepth of its own as its last argument
     *     (blockly-blocks-141.json: AddUIContainer/AddUIText/AddUIImage/
     *      AddUIButton all have a UIDepth overload);
     *   SetUIWidgetDepth(UIWidget, UIDepth) and GetUIWidgetDepth(UIWidget)
     *     exist, so it can be read and written on any widget at any time.
     * The community builder does not model it at all: types.ts declares the
     * UIDepth enum and no field on UIParams ever uses it.
     *
     * WHAT IS NOT CONFIRMED is whether a CHILD's depth is honoured against its
     * own parent's, or whether a subtree is drawn at whatever layer its root
     * container sits on. The builder models it the way the API is shaped: a
     * per-widget value, inherited from the parent when the widget does not set
     * one of its own, and falling back to the design's default at the root.
     * Resolving inheritance before export means the exported code names an
     * explicit layer on EVERY widget, so it comes out right either way.
     */
    var DEPTHS = ['AboveGameUI', 'BelowGameUI'];

    // What a widget can be set to in the builder. Inherit is ours, not the
    // engine's; it never leaves an export.
    var DEPTH_CHOICES = ['Inherit', 'AboveGameUI', 'BelowGameUI'];
    var DEPTH_LABEL = {
        Inherit: 'Same as the parent',
        AboveGameUI: 'Over the game HUD',
        BelowGameUI: 'Under the game HUD'
    };

    var IMAGE_TYPES = [
        'None', 'CrownOutline', 'CrownSolid', 'QuestionMark',
        'RifleAmmo', 'SelfHeal', 'SpawnBeacon', 'TEMP_PortalIcon'
    ];

    var BUTTON_EVENTS = ['HoverIn', 'HoverOut', 'FocusIn', 'FocusOut', 'ButtonDown', 'ButtonUp'];

    // The community builder numbers its enums ALPHABETICALLY and exports those
    // numbers in its JSON. Its TypeScript export always writes the NAME, so the
    // name is the interop currency; these tables exist only so its JSON reads
    // and writes correctly.
    var COMMUNITY_ORDER = {
        UIAnchor: ['BottomCenter', 'BottomLeft', 'BottomRight', 'Center', 'CenterLeft',
                   'CenterRight', 'TopCenter', 'TopLeft', 'TopRight'],
        UIBgFill: ['Blur', 'GradientBottom', 'GradientLeft', 'GradientRight', 'GradientTop',
                   'None', 'OutlineThick', 'OutlineThin', 'Solid'],
        UIButtonEvent: ['ButtonDown', 'ButtonUp', 'FocusIn', 'FocusOut', 'HoverIn', 'HoverOut'],
        UIDepth: ['AboveGameUI', 'BelowGameUI'],
        UIImageType: ['CrownOutline', 'CrownSolid', 'None', 'QuestionMark', 'RifleAmmo',
                      'SelfHeal', 'SpawnBeacon', 'TEMP_PortalIcon']
    };

    var CANVAS_WIDTH = 1920;
    var CANVAS_HEIGHT = 1080;

    /*
     * The palette, from the owner's BF6 UI style guide. Every colour is the
     * hex the guide names and the [r, g, b] on 0..1 the engine's Set*Color
     * calls actually take. palette.json beside this file carries the same
     * table, so the tool can hand the page an updated one without a rebuild;
     * setPalette replaces what is here when it does.
     *
     * The conventions that come with it:
     *   panels        080B0B or D5EBF9 at 0.75 or 0.50
     *   accent bands  0.75
     *   text          always over a blur
     */
    var PALETTE = [
        { name: 'White',       group: 'bright', rgb: [1.000, 1.000, 1.000], hex: '#FFFFFF' },
        { name: 'Cyan',        group: 'bright', rgb: [0.439, 0.922, 1.000], hex: '#70EBFF' },
        { name: 'Red',         group: 'bright', rgb: [1.000, 0.514, 0.380], hex: '#FF8361' },
        { name: 'Green',       group: 'bright', rgb: [0.678, 0.992, 0.525], hex: '#ADFD86' },
        { name: 'Yellow',      group: 'bright', rgb: [1.000, 0.988, 0.612], hex: '#FFFC9C' },
        { name: 'Ice',         group: 'light',  rgb: [0.835, 0.922, 0.976], hex: '#D5EBF9' },
        { name: 'Dark cyan',   group: 'dark',   rgb: [0.075, 0.184, 0.247], hex: '#132F3F' },
        { name: 'Dark red',    group: 'dark',   rgb: [0.251, 0.094, 0.067], hex: '#401811' },
        { name: 'Dark green',  group: 'dark',   rgb: [0.278, 0.447, 0.212], hex: '#477236' },
        { name: 'Dark yellow', group: 'dark',   rgb: [0.443, 0.376, 0.000], hex: '#716000' },
        { name: 'Steel',       group: 'grey',   rgb: [0.329, 0.369, 0.388], hex: '#545E63' },
        { name: 'Slate',       group: 'grey',   rgb: [0.212, 0.224, 0.235], hex: '#36393C' },
        { name: 'Ink',         group: 'grey',   rgb: [0.031, 0.043, 0.043], hex: '#080B0B' }
    ];

    // The alpha the style guide uses for each job.
    var ALPHA = { panel: 0.75, panelLight: 0.50, band: 0.75 };

    function colour(name) {
        for (var i = 0; i < PALETTE.length; i++) if (PALETTE[i].name === name) return PALETTE[i].rgb.slice();
        return [1, 1, 1];
    }

    // The tool hands the page palette.json at startup; this takes it.
    function setPalette(json) {
        var p = json;
        if (typeof p === 'string') { try { p = JSON.parse(p); } catch (e) { return false; } }
        if (!p || !p.swatches || !p.swatches.length) return false;
        PALETTE.length = 0;
        p.swatches.forEach(function (s) { PALETTE.push(s); });
        if (p.alphas && p.alphas.length) {
            for (var i = 0; i < p.alphas.length; i++) {
                if (p.alphas[i].use && p.alphas[i].use.indexOf('dark panel') === 0) ALPHA.panel = p.alphas[i].alpha;
            }
        }
        return true;
    }

    // The aspect presets the clip check runs. The canvas is the 1920 x 1080
    // safe area; a wider screen shows MORE either side of it, a narrower one
    // shows LESS, and the height is what stays fixed.
    var ASPECTS = [
        { id: '16:9',  label: '16:9',  ratio: 16 / 9 },
        { id: '21:9',  label: '21:9',  ratio: 21 / 9 },
        { id: '32:9',  label: '32:9',  ratio: 32 / 9 },
        { id: '16:10', label: '16:10', ratio: 16 / 10 }
    ];

    /* ==================================================================== *
     * Defaults
     * ==================================================================== */

    // Matched to the community builder's DEFAULT_UI_PARAMS so a design made
    // here and a design made there start from the same place.
    function defaults(type) {
        var d = {
            name: '',
            type: type || 'Container',
            position: [0, 0],
            size: [100, 50],
            anchor: 'TopLeft',
            visible: true,
            padding: 0,
            bgColor: [0.2, 0.2, 0.2],
            bgAlpha: 1,
            bgFill: 'None',
            depth: 'Inherit',
            children: []
        };
        if (type === 'WeaponImage') { d.weapon = 'Carbine_M4A1'; d.attachments = []; }
        if (type === 'GadgetImage') { d.gadget = 'C4'; }
        if (type === 'Text') {
            d.textLabel = '';
            d.textArgs = [];
            d.textColor = [1, 1, 1];
            d.textAlpha = 1;
            d.textSize = 24;
            d.textAnchor = 'Center';
            d.textBgBlur = false;
        }
        if (type === 'Image') {
            d.imageType = 'None';
            d.imageColor = [1, 1, 1];
            d.imageAlpha = 1;
        }
        if (type === 'Button') {
            d.bgColor = [1, 1, 1];
            d.bgFill = 'Solid';
            d.buttonEnabled = true;
            d.buttonColorBase = [1, 1, 1];
            d.buttonAlphaBase = 1;
            d.buttonColorDisabled = [0.1, 0.1, 0.1];
            d.buttonAlphaDisabled = 0.5;
            d.buttonColorPressed = [0.2, 0.2, 0.2];
            d.buttonAlphaPressed = 1;
            d.buttonColorHover = [0.4, 0.4, 0.4];
            d.buttonAlphaHover = 1;
            d.buttonColorFocused = [0.5, 0.5, 0.5];
            d.buttonAlphaFocused = 1;
        }
        return d;
    }

    function clone(v) { return JSON.parse(JSON.stringify(v)); }

    function assign(dst, src) {
        for (var k in src) if (Object.prototype.hasOwnProperty.call(src, k)) dst[k] = src[k];
        return dst;
    }

    var idSeed = 0;
    function newId() { idSeed++; return 'w' + idSeed + '_' + Math.floor(Math.random() * 0x10000).toString(16); }
    function resetIds() { idSeed = 0; }

    // A node with every property its type needs, an id and children.
    function makeNode(type, over) {
        var n = defaults(type);
        n.id = newId();
        if (over) assign(n, clone(over));
        if (!n.children) n.children = [];
        if (!n.name) n.name = (type || 'Container').toLowerCase() + n.id.slice(1, 4);
        return n;
    }

    /* ==================================================================== *
     * Tree helpers
     * ==================================================================== */

    function walk(nodes, fn, parent) {
        for (var i = 0; i < nodes.length; i++) {
            if (fn(nodes[i], parent, i) === false) return false;
            var kids = nodes[i].children || [];
            if (kids.length && walk(kids, fn, nodes[i]) === false) return false;
        }
        return true;
    }

    function findById(nodes, id) {
        var hit = null;
        walk(nodes, function (n) { if (n.id === id) { hit = n; return false; } });
        return hit;
    }

    function findByName(nodes, name) {
        var hit = null;
        walk(nodes, function (n) { if (n.name === name) { hit = n; return false; } });
        return hit;
    }

    function parentOf(nodes, id) {
        var hit = null;
        walk(nodes, function (n, p) { if (n.id === id) { hit = p; return false; } });
        return hit;
    }

    function allNames(nodes) {
        var out = [];
        walk(nodes, function (n) { out.push(n.name); });
        return out;
    }

    // Every widget needs a name that is unique across the whole design, because
    // FindUIWidgetWithName is how a script or a block reaches it again.
    function uniqueName(nodes, wanted, exceptId) {
        var base = String(wanted || 'widget').replace(/[^A-Za-z0-9_]/g, '');
        if (!base) base = 'widget';
        var taken = {};
        walk(nodes, function (n) { if (n.id !== exceptId) taken[n.name] = 1; });
        if (!taken[base]) return base;
        var i = 1;
        while (taken[base + i]) i++;
        return base + i;
    }

    function duplicateNode(node, nodes) {
        var copy = clone(node);
        (function stamp(n) {
            n.id = newId();
            n.name = uniqueName(nodes, n.name, n.id);
            (n.children || []).forEach(stamp);
        })(copy);
        return copy;
    }

    /* ==================================================================== *
     * The anchor law
     *
     * The canvas is the 1920 x 1080 safe area. A widget's position is NOT a
     * top-left corner: it is an INWARD inset from the edge its anchor names.
     * Right anchors run +x to the left, bottom anchors run +y upwards, and the
     * widget then aligns its OWN matching point to that spot:
     *
     *   left = anchorPointX - ax * w
     *   top  = anchorPointY - ay * h
     *
     * A child anchors inside its parent's content box (the parent rect inset
     * by the parent's padding), not inside the canvas.
     * ==================================================================== */

    function anchorFrac(name) {
        var a = String(name || 'TopLeft');
        var ax = a.indexOf('Left') >= 0 ? 0 : (a.indexOf('Right') >= 0 ? 1 : 0.5);
        var ay = a.indexOf('Top') >= 0 ? 0 : (a.indexOf('Bottom') >= 0 ? 1 : 0.5);
        return { ax: ax, ay: ay };
    }

    function anchorName(ax, ay) {
        var v = ay === 0 ? 'Top' : (ay === 1 ? 'Bottom' : 'Center');
        var h = ax === 0 ? 'Left' : (ax === 1 ? 'Right' : 'Center');
        if (v === 'Center' && h === 'Center') return 'Center';
        return v + h;
    }

    function rect(left, top, w, h) {
        return {
            left: left, top: top, width: w, height: h,
            right: left + w, bottom: top + h,
            centerX: left + w / 2, centerY: top + h / 2
        };
    }

    // The rect a node occupies inside a given container rect.
    function placeIn(node, box) {
        var f = anchorFrac(node.anchor);
        var px = (node.position && node.position[0]) || 0;
        var py = (node.position && node.position[1]) || 0;
        var w = (node.size && node.size[0]) || 0;
        var h = (node.size && node.size[1]) || 0;

        // Inward inset from the anchored edge.
        var apx = f.ax === 0 ? box.left + px
                : (f.ax === 1 ? box.right - px : box.left + box.width / 2 + px);
        var apy = f.ay === 0 ? box.top + py
                : (f.ay === 1 ? box.bottom - py : box.top + box.height / 2 + py);

        return rect(apx - f.ax * w, apy - f.ay * h, w, h);
    }

    // The box a node's children anchor inside.
    function contentBox(node, r) {
        var p = Number(node.padding) || 0;
        return rect(r.left + p, r.top + p, Math.max(0, r.width - 2 * p), Math.max(0, r.height - 2 * p));
    }

    // The screen rect, in canvas units, for an aspect ratio. Height is the
    // constant; a wider screen simply reveals more canvas either side.
    function screenRect(ratio) {
        var w = Math.round(CANVAS_HEIGHT * (ratio || (16 / 9)));
        return rect((CANVAS_WIDTH - w) / 2, 0, w, CANVAS_HEIGHT);
    }

    // id -> rect for every node in the design, at one aspect ratio.
    function layout(nodes, ratio) {
        var out = {};
        var root = screenRect(ratio);
        (function lay(list, box) {
            for (var i = 0; i < list.length; i++) {
                var r = placeIn(list[i], box);
                out[list[i].id] = r;
                var kids = list[i].children || [];
                if (kids.length) lay(kids, contentBox(list[i], r));
            }
        })(nodes, root);
        return out;
    }

    function intersects(a, b) {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    }

    function contains(outer, inner) {
        return inner.left >= outer.left - 0.5 && inner.right <= outer.right + 0.5 &&
               inner.top >= outer.top - 0.5 && inner.bottom <= outer.bottom + 0.5;
    }

    // A widget that paints something. A Container with fill None and no
    // children is a layout box, and two of those overlapping is not a fault.
    function paints(node) {
        if (node.type === 'Text' || node.type === 'Image' || node.type === 'Button') return true;
        return node.bgFill !== 'None';
    }

    /*
     * OVERLAP AND CLIP, per aspect preset.
     *   clip    the widget leaves the screen at this aspect
     *   spill   the widget leaves its own parent's content box
     *   overlap two painting siblings cover each other
     *   drift   an edge-anchored widget moved more than 8 px from where 16:9
     *           put it, which is the ultrawide failure the community builder
     *           has no answer for
     */
    // Takes either a widget array or a whole design. A design is what the HUD
    // half needs, because a widget's layer can be inherited.
    function audit(nodesOrDesign, ratio) {
        var design = (nodesOrDesign && nodesOrDesign.widgets)
            ? nodesOrDesign
            : { widgets: nodesOrDesign || [], defaultDepth: 'AboveGameUI' };
        var nodes = design.widgets;
        var depths = resolveDepths(design);

        var findings = [];
        var here = layout(nodes, ratio);
        var base = layout(nodes, 16 / 9);
        var screen = screenRect(ratio);
        var hud = hudLayout(ratio);

        walk(nodes, function (n, parent) {
            var r = here[n.id];
            if (!r || n.visible === false) return;
            if (!contains(screen, r)) {
                findings.push({ kind: 'clip', id: n.id, name: n.name,
                    text: n.name + ' leaves the screen at ' + ratioLabel(ratio) + '.' });
            }
            if (parent) {
                var pbox = contentBox(parent, here[parent.id]);
                if (!contains(pbox, r)) {
                    findings.push({ kind: 'spill', id: n.id, name: n.name,
                        text: n.name + ' sticks out of ' + parent.name + '.' });
                }
            }
            var b = base[n.id];
            if (b) {
                var d = Math.max(Math.abs(r.left - b.left), Math.abs(r.top - b.top));
                if (d > 8) {
                    findings.push({ kind: 'drift', id: n.id, name: n.name, amount: Math.round(d),
                        text: n.name + ' moves ' + Math.round(d) + ' px at ' + ratioLabel(ratio) +
                              '. Anchor it to the edge it belongs to.' });
                }
            }

            // The game's own HUD, reported apart from widget-to-widget overlap
            // because the fix is a different one: move it, change its layer, or
            // switch that HUD element off on the site's Modifiers > UI page.
            if (!paints(n)) return;
            var layer = depths[n.id] || 'AboveGameUI';
            for (var h = 0; h < hud.length; h++) {
                if (!intersects(r, hud[h].rect)) continue;
                var under = (layer === 'BelowGameUI');
                findings.push({
                    kind: 'hud', id: n.id, name: n.name,
                    region: hud[h].id, regionLabel: hud[h].label,
                    covers: !under, depth: layer,
                    text: under
                        ? n.name + ' sits under the ' + hud[h].label + ', which will cover it.'
                        : n.name + ' covers the ' + hud[h].label + '.'
                });
            }
        });

        (function siblings(list) {
            for (var i = 0; i < list.length; i++) {
                for (var j = i + 1; j < list.length; j++) {
                    var a = list[i], b = list[j];
                    if (a.visible === false || b.visible === false) continue;
                    if (!paints(a) || !paints(b)) continue;
                    if (intersects(here[a.id], here[b.id])) {
                        findings.push({ kind: 'overlap', id: b.id, name: b.name,
                            text: a.name + ' and ' + b.name + ' cover each other.' });
                    }
                }
                if ((list[i].children || []).length) siblings(list[i].children);
            }
        })(nodes);

        return findings;
    }

    /* ==================================================================== *
     * Depth: resolving it, and setting it in bulk
     * ==================================================================== */

    // id -> the engine value every widget actually ends up at. A widget set to
    // Inherit takes its parent's resolved layer, and a root takes the design's
    // default. Every exporter runs this first, so no export ever writes the
    // word Inherit and no widget leaves without an explicit layer.
    function resolveDepths(design) {
        var d = design && design.widgets ? design : { widgets: design || [], defaultDepth: 'AboveGameUI' };
        var fallback = DEPTHS.indexOf(d.defaultDepth) >= 0 ? d.defaultDepth : 'AboveGameUI';
        var out = {};
        (function go(list, inherited) {
            for (var i = 0; i < list.length; i++) {
                var n = list[i];
                var own = DEPTHS.indexOf(n.depth) >= 0 ? n.depth : inherited;
                out[n.id] = own;
                if ((n.children || []).length) go(n.children, own);
            }
        })(d.widgets, fallback);
        return out;
    }

    function depthOf(design, node) {
        return resolveDepths(design)[node.id] || 'AboveGameUI';
    }

    /*
     * The bulk action behind the LAYER control.
     *   ids       the widgets to change; empty means every root
     *   value     'Inherit', 'AboveGameUI' or 'BelowGameUI'
     *   recurse   also clear every descendant back to Inherit, so the whole
     *             subtree follows the one widget that was just set
     * Returns how many widgets changed.
     */
    function setDepth(design, ids, value, recurse) {
        if (DEPTH_CHOICES.indexOf(value) < 0) return 0;
        var targets = (ids && ids.length)
            ? ids.map(function (id) { return findById(design.widgets, id); }).filter(Boolean)
            : design.widgets.slice();
        var n = 0;
        targets.forEach(function (node) {
            if (node.depth !== value) { node.depth = value; n++; }
            if (recurse) {
                (node.children || []).forEach(function clear(c) {
                    if (c.depth !== 'Inherit') { c.depth = 'Inherit'; n++; }
                    (c.children || []).forEach(clear);
                });
            }
        });
        return n;
    }

    /* ==================================================================== *
     * The game's own HUD
     *
     * APPROXIMATE. These are the places the default Battlefield HUD puts its
     * furniture, written as anchored rects on the 1920 x 1080 safe area so
     * they move with the screen shape the same way a real HUD element does.
     * They are here to answer one question - what does Under hide, and what
     * does Over cover - not to be a pixel-accurate copy, and they have not
     * been measured against a running match.
     *
     * The site's Modifiers > UI page can switch several of these off for real
     * (CompassAllowed_PerTeam, MinimapAllowed_PerTeam, limited_hud and the
     * rest), which is the other half of the answer when a design and the HUD
     * cannot both have the same corner.
     * ==================================================================== */

    var HUD_REGIONS = [
        { id: 'minimap',   label: 'Minimap',       anchor: 'TopLeft',     position: [24, 24],  size: [260, 260] },
        { id: 'objective', label: 'Objective bar', anchor: 'TopCenter',   position: [0, 8],    size: [1000, 56] },
        { id: 'compass',   label: 'Compass',       anchor: 'TopCenter',   position: [0, 72],   size: [760, 36] },
        { id: 'killfeed',  label: 'Kill feed',     anchor: 'TopRight',    position: [24, 24],  size: [420, 220] },
        { id: 'squad',     label: 'Squad list',    anchor: 'CenterLeft',  position: [24, 40],  size: [300, 260] },
        { id: 'health',    label: 'Health',        anchor: 'BottomLeft',  position: [24, 24],  size: [320, 120] },
        { id: 'ammo',      label: 'Ammo',          anchor: 'BottomRight', position: [24, 24],  size: [360, 120] }
    ];

    // The HUD's rects for one screen shape, in canvas units.
    function hudLayout(ratio) {
        var screen = screenRect(ratio);
        return HUD_REGIONS.map(function (r) {
            return { id: r.id, label: r.label, rect: placeIn(r, screen) };
        });
    }

    function ratioLabel(ratio) {
        for (var i = 0; i < ASPECTS.length; i++) {
            if (Math.abs(ASPECTS[i].ratio - ratio) < 0.001) return ASPECTS[i].label;
        }
        return ratio.toFixed(2) + ':1';
    }

    function auditAll(nodesOrDesign) {
        var out = [];
        for (var i = 0; i < ASPECTS.length; i++) {
            var f = audit(nodesOrDesign, ASPECTS[i].ratio);
            for (var j = 0; j < f.length; j++) { f[j].aspect = ASPECTS[i].id; out.push(f[j]); }
        }
        return out;
    }

    /* ==================================================================== *
     * The design document
     * ==================================================================== */

    /*
     * SCHEMA - bf6-ui-design v1
     * {
     *   "format": "bf6-ui-design",
     *   "version": 1,
     *   "name": "hud",
     *   "canvas": { "width": 1920, "height": 1080 },
     *   "strings": { "<widget name>": "template with up to three {}" },
     *   "widgets": [ <node>, ... ],
     *   "animations": [ <clip>, ... ]
     * }
     *
     * node = every property in defaults(type) plus "id" and "children".
     *        Enums are stored by NAME, never by index, so a change to any
     *        engine or builder ordering cannot silently repaint a design.
     *        "textArgs" holds up to three sample values for the {} slots.
     *
     * clip = { "name": "fadeIn", "duration": <ticks>, "loop": <bool>,
     *          "tracks": [ { "widget": "<name>", "property": "<prop>",
     *                        "keys": [ { "t": <tick>, "v": <number|array>,
     *                                    "ease": "linear|easeIn|easeOut|easeInOut|step" } ] } ] }
     */

    function newDesign(name) {
        return {
            format: 'bf6-ui-design',
            version: 1,
            name: name || 'design',
            canvas: { width: CANVAS_WIDTH, height: CANVAS_HEIGHT },
            strings: {},
            // The layer a root widget takes when it does not name one. Change
            // it once and every Inherit widget in the design moves with it.
            defaultDepth: 'AboveGameUI',
            widgets: [],
            animations: []
        };
    }

    // Fill in anything an older or hand-written file left out, so the rest of
    // the code never has to test for a missing property.
    function normalise(design) {
        var d = design || {};
        var out = newDesign(d.name);
        out.strings = d.strings || {};
        out.defaultDepth = DEPTHS.indexOf(d.defaultDepth) >= 0 ? d.defaultDepth : 'AboveGameUI';
        out.animations = d.animations || [];
        out.widgets = (d.widgets || []).map(function fix(n) {
            var base = defaults(n.type || 'Container');
            var node = assign(base, n);
            node.id = n.id || newId();
            node.children = (n.children || []).map(fix);
            if (!node.name) node.name = 'widget' + node.id.slice(1, 5);
            return node;
        });
        return out;
    }

    /* ==================================================================== *
     * Import and export: the community builder's own format
     * ==================================================================== */

    function enumToIndex(kind, name) {
        var order = COMMUNITY_ORDER[kind];
        var i = order.indexOf(name);
        return i < 0 ? 0 : i;
    }
    function enumFromIndex(kind, index) {
        var order = COMMUNITY_ORDER[kind];
        return order[index] === undefined ? order[0] : order[index];
    }

    // design -> the array of UIParams the community builder writes as its JSON.
    // depth has no home in their type, so it is dropped and reported.
    function toCommunityParams(design) {
        var d = normalise(design);
        var depths = resolveDepths(d);
        var dropped = [];
        function conv(n) {
            var p = {
                name: n.name,
                type: n.type,
                position: n.position.slice(),
                size: n.size.slice(),
                anchor: enumToIndex('UIAnchor', n.anchor),
                visible: n.visible !== false,
                padding: Number(n.padding) || 0,
                bgColor: n.bgColor.slice(),
                bgAlpha: Number(n.bgAlpha),
                bgFill: enumToIndex('UIBgFill', n.bgFill),
                children: (n.children || []).map(conv)
            };
            // Their UIParams has no depth field at all, so a widget that has
            // to be under the game HUD loses that on the way out.
            if (depths[n.id] === 'BelowGameUI') dropped.push(n.name + '.depth');
            if (n.type === 'WeaponImage' || n.type === 'GadgetImage') {
                p.type = 'Container';
                dropped.push(n.name + '.' + n.type + ' art (exported as an empty container)');
            }
            if (n.type === 'Text') {
                p.textLabel = d.strings[n.name] !== undefined ? d.strings[n.name] : (n.textLabel || '');
                p.textColor = (n.textColor || [1, 1, 1]).slice();
                p.textAlpha = Number(n.textAlpha === undefined ? 1 : n.textAlpha);
                p.textSize = Number(n.textSize === undefined ? 24 : n.textSize);
                p.textAnchor = enumToIndex('UIAnchor', n.textAnchor || 'Center');
                if (n.textBgBlur) dropped.push(n.name + '.textBgBlur');
                if ((n.textArgs || []).length) dropped.push(n.name + '.textArgs');
            }
            if (n.type === 'Image') {
                p.imageType = enumToIndex('UIImageType', n.imageType || 'None');
                p.imageColor = (n.imageColor || [1, 1, 1]).slice();
                p.imageAlpha = Number(n.imageAlpha === undefined ? 1 : n.imageAlpha);
            }
            if (n.type === 'Button') {
                p.buttonEnabled = n.buttonEnabled !== false;
                ['Base', 'Disabled', 'Pressed', 'Hover', 'Focused'].forEach(function (s) {
                    p['buttonColor' + s] = (n['buttonColor' + s] || [1, 1, 1]).slice();
                    p['buttonAlpha' + s] = Number(n['buttonAlpha' + s] === undefined ? 1 : n['buttonAlpha' + s]);
                });
            }
            return p;
        }
        return { params: d.widgets.map(conv), dropped: dropped };
    }

    // The community builder's JSON (or its params array) -> a design.
    function fromCommunityParams(params, name) {
        var list = params;
        if (list && !Array.isArray(list) && list.params) list = list.params;
        if (!Array.isArray(list)) list = [list];
        var d = newDesign(name);
        function conv(p) {
            var n = makeNode(p.type || 'Container');
            n.name = p.name || n.name;
            n.position = (p.position || [0, 0]).slice();
            n.size = (p.size || [100, 50]).slice();
            n.anchor = typeof p.anchor === 'number' ? enumFromIndex('UIAnchor', p.anchor) : (p.anchor || 'TopLeft');
            n.visible = p.visible !== false;
            n.padding = Number(p.padding) || 0;
            n.bgColor = (p.bgColor || [0.2, 0.2, 0.2]).slice();
            n.bgAlpha = p.bgAlpha === undefined ? 1 : Number(p.bgAlpha);
            n.bgFill = typeof p.bgFill === 'number' ? enumFromIndex('UIBgFill', p.bgFill) : (p.bgFill || 'None');
            if (n.type === 'Text') {
                var label = p.textLabel === undefined ? '' : String(p.textLabel);
                if (label) d.strings[n.name] = label;
                n.textLabel = label;
                n.textColor = (p.textColor || [1, 1, 1]).slice();
                n.textAlpha = p.textAlpha === undefined ? 1 : Number(p.textAlpha);
                n.textSize = p.textSize === undefined ? 24 : Number(p.textSize);
                n.textAnchor = typeof p.textAnchor === 'number'
                    ? enumFromIndex('UIAnchor', p.textAnchor) : (p.textAnchor || 'Center');
            }
            if (n.type === 'Image') {
                n.imageType = typeof p.imageType === 'number'
                    ? enumFromIndex('UIImageType', p.imageType) : (p.imageType || 'None');
                n.imageColor = (p.imageColor || [1, 1, 1]).slice();
                n.imageAlpha = p.imageAlpha === undefined ? 1 : Number(p.imageAlpha);
            }
            if (n.type === 'Button') {
                n.buttonEnabled = p.buttonEnabled !== false;
                ['Base', 'Disabled', 'Pressed', 'Hover', 'Focused'].forEach(function (s) {
                    if (p['buttonColor' + s]) n['buttonColor' + s] = p['buttonColor' + s].slice();
                    if (p['buttonAlpha' + s] !== undefined) n['buttonAlpha' + s] = Number(p['buttonAlpha' + s]);
                });
            }
            n.children = (p.children || []).map(conv);
            return n;
        }
        d.widgets = list.map(conv);
        return d;
    }

    /* ==================================================================== *
     * Export (a): ParseUI TypeScript + strings.json
     *
     * Property order, spacing and the mod.stringkeys form are matched to the
     * community builder's serializeParamToTypescript so a design exported here
     * imports there and the other way round. The one addition is depth, which
     * ParseUI takes and their type does not carry.
     * ==================================================================== */

    function fmtNum(v) {
        var n = Number(v);
        if (!isFinite(n)) return '0';
        return String(Math.round(n * 1e6) / 1e6);
    }
    function fmtArr(a) {
        return '[' + (a || []).map(fmtNum).join(', ') + ']';
    }
    // mod.Vector is opaque in the typings, so nothing structural is assignable
    // to it: a colour written as a bare [r, g, b] is a compile error wherever
    // the engine or the UI module asks for a Vector. Only exports that feed a
    // typed API need this; the ParseUI tree keeps its arrays and converts them
    // in its own builder, and the blocks format has a CreateVector block.
    function fmtVec(a) {
        var v = a || [];
        return 'mod.CreateVector(' + fmtNum(v[0]) + ', ' + fmtNum(v[1]) + ', ' + fmtNum(v[2]) + ')';
    }
    function fmtStr(s) {
        return JSON.stringify(String(s === undefined || s === null ? '' : s));
    }
    function fmtEnum(kind, name) {
        return 'mod.' + kind + '.' + name;
    }

    function identifier(label, used) {
        var parts = String(label || '').replace(/[^A-Za-z0-9]+/g, ' ').trim().split(/\s+/);
        var name = parts.map(function (p, i) {
            p = p.toLowerCase();
            return i === 0 ? p : p.charAt(0).toUpperCase() + p.slice(1);
        }).join('');
        if (!name || /^[0-9]/.test(name)) name = 'ui' + (name || 'Root');
        if (used) {
            var base = name, i = 2;
            while (used[name]) { name = base + i; i++; }
            used[name] = 1;
        }
        return name;
    }

    // A text label becomes mod.stringkeys.<name> when the design has a string
    // for that widget, and mod.Message(mod.stringkeys.<name>, a, b) when the
    // template carries {} slots. Free text has no home in the engine, so a
    // widget with no string entry falls back to a quoted literal exactly the
    // way the community builder does.
    function textLabelExpr(node, strings) {
        var key = node.name;
        if (Object.prototype.hasOwnProperty.call(strings || {}, key)) {
            var expr = 'mod.stringkeys.' + String(key).replace(/ /g, '_');
            var slots = argSlots(strings[key]);
            if (slots > 0) {
                var args = [];
                for (var i = 0; i < slots; i++) {
                    var a = (node.textArgs || [])[i];
                    args.push(a === undefined || a === '' ? '0' : JSON.stringify(String(a)));
                }
                return 'mod.Message(' + expr + ', ' + args.join(', ') + ')';
            }
            return expr;
        }
        return fmtStr(node.textLabel || '');
    }

    // How many {} slots a template has. mod.Message takes at most three, so
    // anything past that is a warning rather than a silently dropped argument.
    function countSlots(template) {
        var m = String(template || '').match(/\{\}/g);
        return m ? m.length : 0;
    }
    function argSlots(template) { return Math.min(3, countSlots(template)); }

    // A name that survives being a TypeScript identifier or a block variable
    // without losing the camelCase the user typed.
    function safeIdent(name) {
        var s = String(name || '').replace(/[^A-Za-z0-9_]/g, '');
        if (!s || /^[0-9]/.test(s)) s = 'ui' + s;
        return s;
    }

    function paramsToTs(node, indentLevel, strings, depths) {
        var indent = new Array(indentLevel + 1).join('  ');
        var pi = new Array(indentLevel + 2).join('  ');
        var lines = [];
        function push(k, v) { lines.push(pi + k + ': ' + v); }

        push('name', fmtStr(node.name));
        push('type', fmtStr(node.type));
        push('position', fmtArr(node.position));
        push('size', fmtArr(node.size));
        push('anchor', fmtEnum('UIAnchor', node.anchor));
        push('visible', node.visible === false ? 'false' : 'true');
        push('padding', fmtNum(node.padding));
        push('bgColor', fmtArr(node.bgColor));
        push('bgAlpha', fmtNum(node.bgAlpha));
        push('bgFill', fmtEnum('UIBgFill', node.bgFill));
        // Always written, and always a real engine value: inheritance is
        // resolved here so the exported tree names a layer on every widget.
        push('depth', fmtEnum('UIDepth', (depths && depths[node.id]) || 'AboveGameUI'));

        if (node.type === 'WeaponImage') {
            push('weapon', fmtEnum('Weapons', node.weapon));
            push('attachments', '[' + (node.attachments || []).map(function (a) { return fmtEnum('WeaponAttachments', a); }).join(', ') + ']');
        }
        if (node.type === 'GadgetImage') push('gadget', fmtEnum('Gadgets', node.gadget));
        if (node.type === 'Text') {
            push('textLabel', textLabelExpr(node, strings));
            push('textColor', fmtArr(node.textColor));
            push('textAlpha', fmtNum(node.textAlpha));
            push('textSize', fmtNum(node.textSize));
            push('textAnchor', fmtEnum('UIAnchor', node.textAnchor || 'Center'));
        }
        if (node.type === 'Image') {
            push('imageType', fmtEnum('UIImageType', node.imageType || 'None'));
            push('imageColor', fmtArr(node.imageColor));
            push('imageAlpha', fmtNum(node.imageAlpha));
        }
        if (node.type === 'Button') {
            push('buttonEnabled', node.buttonEnabled === false ? 'false' : 'true');
            ['Base', 'Disabled', 'Pressed', 'Hover', 'Focused'].forEach(function (s) {
                push('buttonColor' + s, fmtArr(node['buttonColor' + s]));
                push('buttonAlpha' + s, fmtNum(node['buttonAlpha' + s]));
            });
        }

        var kids = node.children || [];
        if (kids.length) {
            var block = [pi + 'children: ['];
            kids.forEach(function (c, i) {
                var t = paramsToTs(c, indentLevel + 2, strings, depths).split('\n');
                if (i < kids.length - 1) t[t.length - 1] = t[t.length - 1] + ',';
                block = block.concat(t);
            });
            block.push(pi + ']');
            lines.push(block.join('\n'));
        }

        return indent + '{\n' + lines.join(',\n') + '\n' + indent + '}';
    }

    // The builder that goes at the top of every ParseUI export. Kept as one
    // block of literal lines rather than assembled from pieces: it is code a
    // person reads in their own project, and it should look hand written.
    //
    // Two ways in, because the engine has two shapes of AddUI*. The full arity
    // overloads all take a parent widget, so only a child can be made in one
    // call; a root has to be made with the four argument overload and then set,
    // which is why uiAddRoot exists at all. Visibility is set last there so a
    // root that is meant to start hidden never shows a half built frame.
    function parseUiRuntime() {
        return [
            'type UiPoint = [number, number];',
            'type UiColor = [number, number, number];',
            '',
            'type UiSpec = {',
            '    name: string;',
            '    type: "Container" | "Text" | "Image" | "Button" | "WeaponImage" | "GadgetImage";',
            '    position: UiPoint;',
            '    size: UiPoint;',
            '    anchor: mod.UIAnchor;',
            '    visible: boolean;',
            '    padding: number;',
            '    bgColor: UiColor;',
            '    bgAlpha: number;',
            '    bgFill: mod.UIBgFill;',
            '    depth: mod.UIDepth;',
            '    textLabel?: mod.Message | string;',
            '    textColor?: UiColor;',
            '    textAlpha?: number;',
            '    textSize?: number;',
            '    textAnchor?: mod.UIAnchor;',
            '    weapon?: mod.Weapons;',
            '    gadget?: mod.Gadgets;',
            '    attachments?: mod.WeaponAttachments[];',
            '    imageType?: mod.UIImageType;',
            '    imageColor?: UiColor;',
            '    imageAlpha?: number;',
            '    buttonEnabled?: boolean;',
            '    buttonColorBase?: UiColor;',
            '    buttonAlphaBase?: number;',
            '    buttonColorDisabled?: UiColor;',
            '    buttonAlphaDisabled?: number;',
            '    buttonColorPressed?: UiColor;',
            '    buttonAlphaPressed?: number;',
            '    buttonColorHover?: UiColor;',
            '    buttonAlphaHover?: number;',
            '    buttonColorFocused?: UiColor;',
            '    buttonAlphaFocused?: number;',
            '    children?: UiSpec[];',
            '};',
            '',
            '// Positions and sizes are two numbers in the design and three in the',
            '// engine: every UI vector carries an unused Z.',
            'function uiPoint(p: UiPoint): mod.Vector { return mod.CreateVector(p[0], p[1], 0); }',
            '',
            'function uiColor(c: UiColor | undefined): mod.Vector {',
            '    return c === undefined ? mod.CreateVector(1, 1, 1) : mod.CreateVector(c[0], c[1], c[2]);',
            '}',
            '',
            '// A label written as mod.stringkeys.<key> is `any` to the compiler and a',
            '// plain string at runtime, so it still has to be wrapped before the engine',
            '// will take it. A label already built with mod.Message passes straight',
            '// through.',
            'function uiMessage(label: mod.Message | string | undefined): mod.Message {',
            '    if (label === undefined) return mod.Message("");',
            '    return typeof label === "string" ? mod.Message(label) : label;',
            '}',
            '',
            '// Buttons raise nothing until each event is switched on by name.',
            'function uiEnableButtonEvents(widget: mod.UIWidget): void {',
            '    mod.EnableUIButtonEvent(widget, mod.UIButtonEvent.HoverIn, true);',
            '    mod.EnableUIButtonEvent(widget, mod.UIButtonEvent.HoverOut, true);',
            '    mod.EnableUIButtonEvent(widget, mod.UIButtonEvent.FocusIn, true);',
            '    mod.EnableUIButtonEvent(widget, mod.UIButtonEvent.FocusOut, true);',
            '    mod.EnableUIButtonEvent(widget, mod.UIButtonEvent.ButtonDown, true);',
            '    mod.EnableUIButtonEvent(widget, mod.UIButtonEvent.ButtonUp, true);',
            '}',
            '',
            'function uiAddChild(spec: UiSpec, parent: mod.UIWidget): void {',
            '    const position = uiPoint(spec.position);',
            '    const size = uiPoint(spec.size);',
            '    if (spec.type === "WeaponImage" || spec.type === "GadgetImage") {',
            '        if (spec.type === "WeaponImage") {',
            '            const pkg = mod.CreateNewWeaponPackage();',
            '            for (const attachment of spec.attachments ?? []) mod.AddAttachmentToWeaponPackage(attachment, pkg);',
            '            mod.AddUIWeaponImage(spec.name, position, size, spec.anchor, spec.weapon ?? mod.Weapons.Carbine_M4A1, parent, pkg);',
            '        } else {',
            '            if (spec.gadget === undefined) throw new Error("Choose a gadget for " + spec.name);',
            '            mod.AddUIGadgetImage(spec.name, position, size, spec.anchor, spec.gadget, parent);',
            '        }',
            '        const image = mod.FindUIWidgetWithName(spec.name);',
            '        mod.SetUIWidgetPadding(image, spec.padding);',
            '        mod.SetUIWidgetBgColor(image, uiColor(spec.bgColor));',
            '        mod.SetUIWidgetBgAlpha(image, spec.bgAlpha);',
            '        mod.SetUIWidgetBgFill(image, spec.bgFill);',
            '        mod.SetUIWidgetDepth(image, spec.depth);',
            '        mod.SetUIWidgetVisible(image, spec.visible);',
            '    } else if (spec.type === "Text") {',
            '        mod.AddUIText(spec.name, position, size, spec.anchor, parent, spec.visible, spec.padding,',
            '            uiColor(spec.bgColor), spec.bgAlpha, spec.bgFill, uiMessage(spec.textLabel),',
            '            spec.textSize ?? 24, uiColor(spec.textColor), spec.textAlpha ?? 1,',
            '            spec.textAnchor ?? mod.UIAnchor.Center, spec.depth);',
            '    } else if (spec.type === "Image") {',
            '        mod.AddUIImage(spec.name, position, size, spec.anchor, parent, spec.visible, spec.padding,',
            '            uiColor(spec.bgColor), spec.bgAlpha, spec.bgFill,',
            '            spec.imageType ?? mod.UIImageType.None, uiColor(spec.imageColor),',
            '            spec.imageAlpha ?? 1, spec.depth);',
            '    } else if (spec.type === "Button") {',
            '        mod.AddUIButton(spec.name, position, size, spec.anchor, parent, spec.visible, spec.padding,',
            '            uiColor(spec.bgColor), spec.bgAlpha, spec.bgFill, spec.buttonEnabled !== false,',
            '            uiColor(spec.buttonColorBase), spec.buttonAlphaBase ?? 1,',
            '            uiColor(spec.buttonColorDisabled), spec.buttonAlphaDisabled ?? 1,',
            '            uiColor(spec.buttonColorPressed), spec.buttonAlphaPressed ?? 1,',
            '            uiColor(spec.buttonColorHover), spec.buttonAlphaHover ?? 1,',
            '            uiColor(spec.buttonColorFocused), spec.buttonAlphaFocused ?? 1, spec.depth);',
            '    } else {',
            '        mod.AddUIContainer(spec.name, position, size, spec.anchor, parent, spec.visible,',
            '            spec.padding, uiColor(spec.bgColor), spec.bgAlpha, spec.bgFill, spec.depth);',
            '    }',
            '}',
            '',
            'function uiAddRoot(spec: UiSpec): mod.UIWidget {',
            '    if (spec.type === "WeaponImage" || spec.type === "GadgetImage") {',
            '        uiAddChild(spec, mod.GetUIRoot());',
            '        return mod.FindUIWidgetWithName(spec.name);',
            '    }',
            '    const position = uiPoint(spec.position);',
            '    const size = uiPoint(spec.size);',
            '    if (spec.type === "Text") {',
            '        mod.AddUIText(spec.name, position, size, spec.anchor, uiMessage(spec.textLabel));',
            '    } else if (spec.type === "Image") {',
            '        mod.AddUIImage(spec.name, position, size, spec.anchor,',
            '            spec.imageType ?? mod.UIImageType.None);',
            '    } else if (spec.type === "Button") {',
            '        mod.AddUIButton(spec.name, position, size, spec.anchor);',
            '    } else {',
            '        mod.AddUIContainer(spec.name, position, size, spec.anchor);',
            '    }',
            '    const root: mod.UIWidget = mod.FindUIWidgetWithName(spec.name);',
            '    mod.SetUIWidgetPadding(root, spec.padding);',
            '    mod.SetUIWidgetBgColor(root, uiColor(spec.bgColor));',
            '    mod.SetUIWidgetBgAlpha(root, spec.bgAlpha);',
            '    mod.SetUIWidgetBgFill(root, spec.bgFill);',
            '    mod.SetUIWidgetDepth(root, spec.depth);',
            '    if (spec.type === "Text") {',
            '        mod.SetUITextSize(root, spec.textSize ?? 24);',
            '        mod.SetUITextColor(root, uiColor(spec.textColor));',
            '        mod.SetUITextAlpha(root, spec.textAlpha ?? 1);',
            '        mod.SetUITextAnchor(root, spec.textAnchor ?? mod.UIAnchor.Center);',
            '    } else if (spec.type === "Image") {',
            '        mod.SetUIImageColor(root, uiColor(spec.imageColor));',
            '        mod.SetUIImageAlpha(root, spec.imageAlpha ?? 1);',
            '    } else if (spec.type === "Button") {',
            '        mod.SetUIButtonEnabled(root, spec.buttonEnabled !== false);',
            '        mod.SetUIButtonColorBase(root, uiColor(spec.buttonColorBase));',
            '        mod.SetUIButtonAlphaBase(root, spec.buttonAlphaBase ?? 1);',
            '        mod.SetUIButtonColorDisabled(root, uiColor(spec.buttonColorDisabled));',
            '        mod.SetUIButtonAlphaDisabled(root, spec.buttonAlphaDisabled ?? 1);',
            '        mod.SetUIButtonColorPressed(root, uiColor(spec.buttonColorPressed));',
            '        mod.SetUIButtonAlphaPressed(root, spec.buttonAlphaPressed ?? 1);',
            '        mod.SetUIButtonColorHover(root, uiColor(spec.buttonColorHover));',
            '        mod.SetUIButtonAlphaHover(root, spec.buttonAlphaHover ?? 1);',
            '        mod.SetUIButtonColorFocused(root, uiColor(spec.buttonColorFocused));',
            '        mod.SetUIButtonAlphaFocused(root, spec.buttonAlphaFocused ?? 1);',
            '    }',
            '    mod.SetUIWidgetVisible(root, spec.visible);',
            '    return root;',
            '}',
            '',
            '// Depth first, with an explicit stack rather than a recursive call: a UI',
            '// tree is the one place this tool will happily nest a dozen levels, and',
            '// the Portal script runtime promises nothing about recursion depth.',
            'function ParseUI(spec: UiSpec): mod.UIWidget {',
            '    const root = uiAddRoot(spec);',
            '    if (spec.type === "Button") uiEnableButtonEvents(root);',
            '    const pending: { spec: UiSpec; parent: mod.UIWidget }[] = [];',
            '    const seed = spec.children ?? [];',
            '    for (let i = seed.length - 1; i >= 0; i--) pending.push({ spec: seed[i], parent: root });',
            '    while (pending.length > 0) {',
            '        const item = pending.pop() as { spec: UiSpec; parent: mod.UIWidget };',
            '        uiAddChild(item.spec, item.parent);',
            '        const widget: mod.UIWidget = mod.FindUIWidgetWithName(item.spec.name);',
            '        if (item.spec.type === "Button") uiEnableButtonEvents(widget);',
            '        const kids = item.spec.children ?? [];',
            '        for (let i = kids.length - 1; i >= 0; i--) pending.push({ spec: kids[i], parent: widget });',
            '    }',
            '    return root;',
            '}'
        ];
    }

    function exportParseUi(design, opts) {
        var d = normalise(design);
        opts = opts || {};
        // The builder below occupies these names at module scope. Claim them up
        // front so a widget called "uiPoint" or "getUi" gets a numbered variable
        // instead of shadowing the function the export depends on.
        var used = { uiPoint: 1, uiColor: 1, uiMessage: 1, uiEnableButtonEvents: 1,
                     uiAddChild: 1, uiAddRoot: 1, ParseUI: 1, uiCache: 1, getUi: 1,
                     onUiButton: 1 };
        var out = [];
        var vars = [];
        var depths = resolveDepths(d);

        out.push('// BF6 UI BUILDER export: plain TypeScript over the mod UI API');
        out.push('// Widgets are created once. Update them with mod.Set* and');
        out.push('// mod.FindUIWidgetWithName, never by rebuilding the tree.');
        out.push('');
        /* THE BUILDER SHIPS IN THE FILE BECAUSE THERE IS NOWHERE TO IMPORT IT FROM.
         *
         * This exporter used to open with `import * as modlib from "modlib"` and
         * call modlib.ParseUI(...) on the tree literal below. No package called
         * modlib exists - not in the Portal scripting template, not on npm - so
         * every file this backend produced failed to compile the moment it was
         * pasted into a project, and the tool was telling people to paste it.
         *
         * The tree literal is the part worth keeping: it is what the round trip
         * importer reads back, and it is far easier to edit by hand than a
         * hundred positional AddUI* arguments. So the walk that turns it into
         * widgets is written out here, against mod.AddUI* and mod.SetUI* only.
         * The file now depends on nothing but the engine typings.
         *
         * Before changing any call below, check the name and its argument order
         * against bf6-portal-mod-types. Every one of them was taken from there. */
        parseUiRuntime().forEach(function (line) { out.push(line); });
        out.push('');

        d.widgets.forEach(function (root) {
            var v = identifier(root.name, used);
            vars.push({ variable: v, name: root.name });
            out.push('export const ' + v + ' = ParseUI(');
            out.push(paramsToTs(root, 1, d.strings, depths));
            out.push(');');
            out.push('');
        });

        // Typed accessors, so logic can reach every named widget without
        // guessing at a string.
        var names = allNames(d.widgets);
        out.push('// ---- runtime bindings ----');
        out.push('// One stable name per widget. getUi() resolves them once and');
        out.push('// hands back the same object every call.');
        out.push('let uiCache: Record<string, mod.UIWidget> | null = null;');
        out.push('export function getUi(): Record<string, mod.UIWidget> {');
        out.push('    if (uiCache) return uiCache;');
        out.push('    uiCache = {');
        names.forEach(function (n) {
            out.push('        ' + JSON.stringify(n) + ': mod.FindUIWidgetWithName(' + JSON.stringify(n) + '),');
        });
        out.push('    };');
        out.push('    return uiCache;');
        out.push('}');
        out.push('');

        var buttons = [];
        walk(d.widgets, function (n) { if (n.type === 'Button') buttons.push(n.name); });
        if (buttons.length) {
            out.push('// ---- button events ----');
            out.push('// Route by widget name. mod.SetUIButton* carries the per-state');
            out.push('// colours, so this only has to carry what the press MEANS.');
            out.push('export function onUiButton(player: mod.Player, widget: mod.UIWidget, event: mod.UIButtonEvent): void {');
            out.push('    if (event !== mod.UIButtonEvent.ButtonUp) return;');
            out.push('    const ui = getUi();');
            buttons.forEach(function (b, i) {
                out.push('    ' + (i ? 'else if' : 'if') + ' (widget === ui[' + JSON.stringify(b) + ']) {');
                out.push('        // ' + b + ' pressed');
                out.push('    }');
            });
            out.push('}');
            out.push('');
        }

        var strings = {};
        Object.keys(d.strings).forEach(function (k) { strings[k] = d.strings[k]; });

        return {
            typescript: out.join('\n'),
            strings: strings,
            stringsJson: JSON.stringify(strings, null, 2),
            roots: vars,
            warnings: asciiWarnings(d)
        };
    }

    // The Portal font is ASCII only: anything else renders as boxes.
    function asciiWarnings(design) {
        var w = [];
        Object.keys(design.strings || {}).forEach(function (k) {
            var v = design.strings[k];
            if (/[^\x20-\x7E]/.test(String(v))) {
                w.push('The string for "' + k + '" has characters outside ASCII. The Portal font renders those as boxes.');
            }
            if (countSlots(v) > 3) {
                w.push('The string for "' + k + '" has more than three {} slots. mod.Message takes at most three arguments.');
            }
        });
        walk(design.widgets || [], function (n) {
            if (/[^A-Za-z0-9_]/.test(n.name)) {
                w.push('Widget name "' + n.name + '" is not plain: FindUIWidgetWithName wants letters, digits and underscores.');
            }
        });
        return w;
    }

    /* ==================================================================== *
     * Export (b): De Luca UI module / solid-ui
     * ==================================================================== */

    var DELUCA_CLASS = {
        Container: 'UIContainer',
        Text: 'UIText',
        Image: 'UIImage',
        Button: 'UIButton'
    };

    // Where each class lives. The package has no barrel file for the
    // components, so every one is its own module path.
    var DELUCA_MODULE = {
        UIContainer: 'bf6-portal-utils/ui/components/container',
        UIText: 'bf6-portal-utils/ui/components/text',
        UIImage: 'bf6-portal-utils/ui/components/image',
        UIButton: 'bf6-portal-utils/ui/components/button',
        UIContainerButton: 'bf6-portal-utils/ui/components/container-button'
    };

    /* A PLAIN UIButton CANNOT HOLD ANYTHING.
     *
     * UIButton is a leaf: it does not implement UI.Parent and its Params have
     * no childrenParams, so a design that puts a label inside a button - which
     * every button in the built-in templates does - has no way to say so. The
     * module's answer is UIContainerButton, a button wrapped around a real
     * UIContainer, and it takes childrenParams and exposes .innerContainer as
     * a parent. Pick it whenever a Button has children, and only then, so a
     * bare button stays the cheaper widget. */
    function delucaClass(node) {
        if (node.type === 'Button' && (node.children || []).length) return 'UIContainerButton';
        return DELUCA_CLASS[node.type];
    }

    // What a child of this node has to name as its parent. A container button
    // is not itself a UI.Parent; its inner container is.
    function delucaParentExpr(node, variable) {
        return delucaClass(node) === 'UIContainerButton' ? variable + '.innerContainer' : variable;
    }

    // What the UI module has no home for. The community format already reports
    // its losses this way and these are the same kind of thing: silently
    // dropping them would have the export look right and draw wrong.
    function delucaWarnings(design) {
        var padded = [], hovered = [], w = [];
        walk(design.widgets, function (n) {
            if (n.type !== 'Text' && Number(n.padding)) padded.push(n.name);
            if (n.type === 'Button' && n.buttonColorHover) hovered.push(n.name);
        });
        function list(names) {
            if (names.length <= 6) return names.join(', ');
            return names.slice(0, 6).join(', ') + ' and ' + (names.length - 6) + ' more';
        }
        if (padded.length) {
            w.push('The UI module gives padding to text only and hardcodes zero everywhere else, ' +
                   'so the padding on ' + list(padded) + ' is not exported.');
        }
        if (hovered.length) {
            w.push('The UI module has no hover colour: ' + list(hovered) +
                   ' will use the focused colour when hovered.');
        }
        return w;
    }

    function delucaImports(design) {
        var wanted = {};
        walk(design.widgets, function (n) { wanted[delucaClass(n)] = 1; });
        return Object.keys(DELUCA_MODULE).filter(function (c) { return wanted[c]; }).map(function (c) {
            return "import { " + c + " } from '" + DELUCA_MODULE[c] + "';";
        });
    }

    /* EVERY PROPERTY HERE HAS TO EXIST ON THE PARAMS TYPE IT LANDS IN.
     *
     * These object literals go straight into a constructor argument, so
     * TypeScript applies an excess property check and a single invented field
     * fails the whole file. This used to write four that bf6-portal-utils has
     * no home for, and the exports would not compile:
     *
     *   name      the UI module names its own widgets with UI.makeName, from
     *             the parent and receiver. A name passed in is ignored.
     *   padding   only UIText.Params has it. UIContainer, UIImage and UIButton
     *             hardcode a padding of 0 in the AddUI* call they make.
     *   hoverColor / hoverAlpha
     *             UIButton.Params has base, disabled, pressed and focused only.
     *             The module passes the focused pair through for hover as well.
     *
     * Colours are the other half: mod.Vector is an opaque type, so a three
     * number array is not one. It has to be built with mod.CreateVector. */
    function delucaProps(node, strings, indent, depths) {
        var l = [];
        function push(s) { l.push(indent + s); }
        push('position: { x: ' + fmtNum(node.position[0]) + ', y: ' + fmtNum(node.position[1]) + ' },');
        push('size: { width: ' + fmtNum(node.size[0]) + ', height: ' + fmtNum(node.size[1]) + ' },');
        push('anchor: ' + fmtEnum('UIAnchor', node.anchor) + ',');
        push('visible: ' + (node.visible === false ? 'false' : 'true') + ',');
        push('bgColor: ' + fmtVec(node.bgColor) + ',');
        push('bgAlpha: ' + fmtNum(node.bgAlpha) + ',');
        push('bgFill: ' + fmtEnum('UIBgFill', node.bgFill) + ',');
        push('depth: ' + fmtEnum('UIDepth', (depths && depths[node.id]) || 'AboveGameUI') + ',');
        if (node.type === 'Text') {
            push('padding: ' + fmtNum(node.padding) + ',');
            push('message: ' + messageExpr(node, strings) + ',');
            push('textColor: ' + fmtVec(node.textColor) + ',');
            push('textAlpha: ' + fmtNum(node.textAlpha) + ',');
            push('textSize: ' + fmtNum(node.textSize) + ',');
            push('textAnchor: ' + fmtEnum('UIAnchor', node.textAnchor || 'Center') + ',');
        }
        if (node.type === 'Image') {
            push('imageType: ' + fmtEnum('UIImageType', node.imageType || 'None') + ',');
            push('imageColor: ' + fmtVec(node.imageColor) + ',');
            push('imageAlpha: ' + fmtNum(node.imageAlpha) + ',');
        }
        if (node.type === 'Button') {
            push('baseColor: ' + fmtVec(node.buttonColorBase) + ',');
            push('pressedColor: ' + fmtVec(node.buttonColorPressed) + ',');
            push('focusedColor: ' + fmtVec(node.buttonColorFocused) + ',');
            push('disabledColor: ' + fmtVec(node.buttonColorDisabled) + ',');
            push('onClickUp: (player: mod.Player) => { /* ' + node.name + ' */ },');
        }
        return l;
    }

    function messageExpr(node, strings) {
        var key = node.name;
        if (Object.prototype.hasOwnProperty.call(strings || {}, key)) {
            var e = 'mod.stringkeys.' + String(key).replace(/ /g, '_');
            var slots = argSlots(strings[key]);
            if (slots > 0) {
                var args = [];
                for (var i = 0; i < slots; i++) {
                    var a = (node.textArgs || [])[i];
                    args.push(a === undefined || a === '' ? '0' : JSON.stringify(String(a)));
                }
                return 'mod.Message(' + e + ', ' + args.join(', ') + ')';
            }
            return 'mod.Message(' + e + ')';
        }
        return 'mod.Message(mod.stringkeys.' + String(key).replace(/ /g, '_') + ')';
    }

    function hasWeaponImage(d) {
        var found = false;
        walk(d.widgets, function(n) { if (n.type === 'WeaponImage' || n.type === 'GadgetImage') found = true; });
        return found;
    }

    function exportDeluca(design, opts) {
        var d = normalise(design);
        if (hasWeaponImage(d)) {
            var native = exportParseUi(d);
            native.warnings.push('Configured weapon images use the native mod API. This export uses the plain TypeScript backend to preserve the weapon package.');
            return native;
        }
        var mode = (opts && opts.mode) === 'solid' ? 'solid' : 'classes';
        var depths = resolveDepths(d);
        var out = [];

        /* THESE WIDGETS DO NOT ANSWER TO THE NAMES IN THE DESIGN.
         *
         * Both of these formats hand construction to bf6-portal-utils, and the
         * module names every widget itself with UI.makeName(parent, receiver).
         * A name passed in is ignored, so mod.FindUIWidgetWithName("panel")
         * finds nothing in a tree built this way. This export used to end with
         * an ensureLayers() helper that did exactly that lookup on every widget
         * to re-apply depth, so it was a no-op at best. It is gone: the module
         * has forwarded depth into its own AddUI* call since v6, the props
         * below name it, and there is nothing left to patch up afterwards.
         * Hold the objects these functions return and use their setters. */
        if (mode === 'solid') {
            out.push('// BF6 UI BUILDER export: bf6-portal-utils solid-ui');
            out.push('// Reactive props are accessor functions; SolidUI updates only the');
            out.push('// property that changed, on the tick its signal settles.');
            out.push('// The module names its own widgets, so keep the objects this returns:');
            out.push('// mod.FindUIWidgetWithName will not find them by the names below.');
            out.push("import { SolidUI } from 'bf6-portal-utils/solid-ui';");
            delucaImports(d).forEach(function (s) { out.push(s); });
            out.push('');
            out.push('export function build' + identifier(d.name).replace(/^./, function (c) { return c.toUpperCase(); }) +
                     '(player: mod.Player) {');
            var made = {};
            (function emit(list, parentExpr, depth) {
                var ind = new Array(depth + 2).join('    ');
                list.forEach(function (n) {
                    var v = identifier(n.name, made);
                    out.push(ind + 'const ' + v + ' = SolidUI.h(' + delucaClass(n) + ', {');
                    if (parentExpr) out.push(ind + '    parent: ' + parentExpr + ',');
                    else out.push(ind + '    receiver: player,');
                    delucaProps(n, d.strings, ind + '    ', depths).forEach(function (s) { out.push(s); });
                    out.push(ind + '});');
                    if ((n.children || []).length) emit(n.children, delucaParentExpr(n, v), depth);
                });
            })(d.widgets, null, 1);
            out.push('}');
        } else {
            out.push('// BF6 UI BUILDER export: bf6-portal-utils UI module');
            out.push('// The UI module owns OnPlayerUIButtonEvent through Events, so do not');
            out.push('// export a Portal event handler of your own for buttons.');
            out.push('// The module names its own widgets, so keep the objects this returns:');
            out.push('// mod.FindUIWidgetWithName will not find them by the names below.');
            delucaImports(d).forEach(function (s) { out.push(s); });
            out.push('');
            var made2 = {};
            d.widgets.forEach(function (rootNode) {
                var v = identifier(rootNode.name, made2);
                out.push('export function make' + v.replace(/^./, function (c) { return c.toUpperCase(); }) +
                         '(player: mod.Player): ' + delucaClass(rootNode) + ' {');
                out.push('    return new ' + delucaClass(rootNode) + '({');
                out.push('        receiver: player,');
                delucaProps(rootNode, d.strings, '        ', depths).forEach(function (s) { out.push(s); });
                if ((rootNode.children || []).length) {
                    out.push('        childrenParams: [');
                    (function kids(list, ind) {
                        list.forEach(function (n) {
                            out.push(ind + '{');
                            out.push(ind + '    type: ' + delucaClass(n) + ',');
                            delucaProps(n, d.strings, ind + '    ', depths).forEach(function (s) { out.push(s); });
                            if ((n.children || []).length) {
                                out.push(ind + '    childrenParams: [');
                                kids(n.children, ind + '        ');
                                out.push(ind + '    ],');
                            }
                            out.push(ind + '},');
                        });
                    })(rootNode.children, '            ');
                    out.push('        ],');
                }
                out.push('    });');
                out.push('}');
                out.push('');
            });
        }

        return { typescript: out.join('\n'), warnings: asciiWarnings(d).concat(delucaWarnings(d)) };
    }

    /* ==================================================================== *
     * Export (c): a blocks snippet
     *
     * Block shapes taken from the live catalogue and the real fixture:
     *   ruleBlock   fields NAME / EVENTTYPE (+ OBJECTTYPE on Ongoing),
     *               extraState.isOngoingEvent, inputs.ACTIONS, chained by next
     *   AddUI*      VALUE-0 .. VALUE-n in the engine's own argument order
     *   CreateVector / Number / Text / Boolean / Message / UIAnchorItem /
     *   UIBgFillItem / UIDepthItem / UIImageTypeItem / UIButtonEventItem
     * ==================================================================== */

    function bText(s) { return { type: 'Text', fields: { TEXT: String(s) } }; }
    function bNum(n) { return { type: 'Number', fields: { NUM: Number(n) } }; }
    function bBool(b) { return { type: 'Boolean', fields: { BOOL: b ? 'TRUE' : 'FALSE' } }; }
    function bVec(a) {
        return { type: 'CreateVector', inputs: {
            'VALUE-0': { block: bNum(a[0] || 0) },
            'VALUE-1': { block: bNum(a[1] || 0) },
            'VALUE-2': { block: bNum(a[2] || 0) }
        } };
    }
    function bEnum(kind, name) {
        var f = {}; f['VALUE-0'] = kind; f['VALUE-1'] = name;
        return { type: kind + 'Item', fields: f };
    }
    function bMessage(text, args) {
        var inp = { 'VALUE-0': { block: bText(text) } };
        (args || []).slice(0, 3).forEach(function (a, i) {
            inp['VALUE-' + (i + 1)] = { block: bText(String(a)) };
        });
        return { type: 'Message', inputs: inp };
    }
    function bFind(name) {
        return { type: 'FindUIWidgetWithName', inputs: { 'VALUE-0': { block: bText(name) } } };
    }
    function bVarRef(name) {
        return { type: 'variableReferenceBlock', extraState: { isObjectVar: false },
                 fields: { OBJECTTYPE: 'Global', VAR: '{{VAR:' + name + ':}}' } };
    }
    function bGetVar(name) {
        return { type: 'GetVariable', inputs: { 'VALUE-0': { block: bVarRef(name) } } };
    }
    function bSetVar(name, valueBlock) {
        return { type: 'SetVariable', inputs: {
            'VALUE-0': { block: bVarRef(name) },
            'VALUE-1': { block: valueBlock }
        } };
    }
    function bIn(list) {
        // A statement chain: [a, b, c] -> a.next = b, b.next = c
        var head = null, cur = null;
        list.forEach(function (b) {
            if (!b) return;
            if (!head) { head = b; cur = b; return; }
            cur.next = { block: b };
            cur = b;
        });
        return head;
    }
    function bRule(name, eventType, actions, ongoing) {
        var fields = { NAME: name, EVENTTYPE: eventType };
        if (ongoing) fields.OBJECTTYPE = 'Global';
        var r = {
            type: 'ruleBlock',
            extraState: { isOngoingEvent: !!ongoing },
            fields: fields
        };
        var head = bIn(actions);
        if (head) r.inputs = { ACTIONS: { block: head } };
        return r;
    }

    // The full-arity AddUI* call for a widget, with the parent widget slot
    // filled by FindUIWidgetWithName so nesting survives a paste.
    function addWidgetBlocks(node, parentName, strings, depths) {
        var stmts = [];
        var inputs = {};
        function put(i, block) { inputs['VALUE-' + i] = { block: block }; }

        put(0, bText(node.name));
        put(1, bVec([node.position[0], node.position[1], 0]));
        put(2, bVec([node.size[0], node.size[1], 0]));
        put(3, bEnum('UIAnchor', node.anchor));

        var type = node.type;
        if (type === 'WeaponImage' || type === 'GadgetImage') {
            var packageName = 'weaponPackage_' + node.id;
            if (type === 'WeaponImage') {
            stmts.push(bSetVar(packageName, {type:'CreateNewWeaponPackage'}));
            (node.attachments || []).forEach(function (a) {
                stmts.push({type:'AddAttachmentToWeaponPackage', inputs:{
                    'VALUE-0':{block:bEnum('WeaponAttachments', a)}, 'VALUE-1':{block:bGetVar(packageName)} }});
            });
            put(4, bEnum('Weapons', node.weapon));
            put(5, parentName ? bFind(parentName) : {type:'GetUIRoot'});
            put(6, bGetVar(packageName));
            stmts.push({type:'AddUIWeaponImage', inputs:inputs});
            } else {
                put(4, bEnum('Gadgets', node.gadget));
                put(5, parentName ? bFind(parentName) : {type:'GetUIRoot'});
                stmts.push({type:'AddUIGadgetImage', inputs:inputs});
            }
            [['Padding', bNum(node.padding || 0)], ['BgColor', bVec(node.bgColor)],
             ['BgAlpha', bNum(node.bgAlpha)], ['BgFill', bEnum('UIBgFill', node.bgFill)]].forEach(function (p) {
                stmts.push({type:'SetUIWidget' + p[0], inputs:{'VALUE-0':{block:bFind(node.name)}, 'VALUE-1':{block:p[1]}}});
            });
            stmts.push({type:'SetUIWidgetDepth', inputs:{'VALUE-0':{block:bFind(node.name)}, 'VALUE-1':{block:bEnum('UIDepth', depths[node.id] || 'AboveGameUI')}}});
            stmts.push({type:'SetUIWidgetVisible', inputs:{'VALUE-0':{block:bFind(node.name)}, 'VALUE-1':{block:bBool(node.visible !== false)}}});
            return stmts;
        }
        if (type === 'Container' || type === 'Text' || type === 'Image' || type === 'Button') {
            // parent, visible, padding, bgColor, bgAlpha, bgFill
            put(4, parentName ? bFind(parentName) : {type:'GetUIRoot'});
            put(5, bBool(node.visible !== false));
            put(6, bNum(node.padding || 0));
            put(7, bVec(node.bgColor));
            put(8, bNum(node.bgAlpha));
            put(9, bEnum('UIBgFill', node.bgFill));
        }

        var blockType = 'AddUIContainer';
        if (type === 'Text') {
            blockType = 'AddUIText';
            var tmpl = strings[node.name] !== undefined ? strings[node.name] : (node.textLabel || '');
            put(10, bMessage(tmpl, node.textArgs));
            put(11, bNum(node.textSize));
            put(12, bVec(node.textColor));
            put(13, bNum(node.textAlpha));
            put(14, bEnum('UIAnchor', node.textAnchor || 'Center'));
            put(15, bEnum('UIDepth', (depths && depths[node.id]) || 'AboveGameUI'));
        } else if (type === 'Image') {
            blockType = 'AddUIImage';
            put(10, bEnum('UIImageType', node.imageType || 'None'));
            put(11, bVec(node.imageColor));
            put(12, bNum(node.imageAlpha));
            put(13, bEnum('UIDepth', (depths && depths[node.id]) || 'AboveGameUI'));
        } else if (type === 'Button') {
            blockType = 'AddUIButton';
            put(10, bBool(node.buttonEnabled !== false));
            put(11, bVec(node.buttonColorBase));   put(12, bNum(node.buttonAlphaBase));
            put(13, bVec(node.buttonColorDisabled)); put(14, bNum(node.buttonAlphaDisabled));
            put(15, bVec(node.buttonColorPressed)); put(16, bNum(node.buttonAlphaPressed));
            put(17, bVec(node.buttonColorHover));   put(18, bNum(node.buttonAlphaHover));
            put(19, bVec(node.buttonColorFocused)); put(20, bNum(node.buttonAlphaFocused));
            put(21, bEnum('UIDepth', (depths && depths[node.id]) || 'AboveGameUI'));
        } else {
            put(10, bEnum('UIDepth', (depths && depths[node.id]) || 'AboveGameUI'));
        }

        stmts.push({ type: blockType, inputs: inputs });

        if (type === 'Button') {
            // Buttons are dead until their events are switched on.
            BUTTON_EVENTS.forEach(function (e) {
                stmts.push({ type: 'EnableUIButtonEvent', inputs: {
                    'VALUE-0': { block: bFind(node.name) },
                    'VALUE-1': { block: bEnum('UIButtonEvent', e) },
                    'VALUE-2': { block: bBool(true) }
                } });
            });
        }
        return stmts;
    }

    function exportBlocks(design, opts) {
        var d = normalise(design);
        opts = opts || {};
        var depths = resolveDepths(d);
        var actions = [];
        (function emit(list, parentName) {
            list.forEach(function (n) {
                addWidgetBlocks(n, parentName, d.strings, depths).forEach(function (b) { actions.push(b); });
                if ((n.children || []).length) emit(n.children, n.name);
            });
        })(d.widgets, null);

        var rules = [bRule(d.name + ' UI', opts.event || 'OnGameModeStarted', actions, false)];
        rules[0].x = 40; rules[0].y = 40;

        // One Ongoing rule per animation.
        var y = 40;
        (d.animations || []).forEach(function (clip) {
            y += 520;
            var r = animationRule(clip, d);
            r.x = 720; r.y = y - 480;
            rules.push(r);
        });

        return {
            name: 'BF6 UI BUILDER: ' + d.name,
            note: 'Built from the design "' + d.name + '". Every widget is created once; ' +
                  'update it later with the Set* blocks and FindUIWidgetWithName.',
            blocks: { languageVersion: 0, blocks: rules },
            warnings: asciiWarnings(d)
        };
    }

    /*
     * WHAT BLOCKS CAN EXPRESS, exactly.
     * An Ongoing rule steps a frame variable and applies Set* blocks. The
     * engine's block set has Add, Subtract, Multiply, Divide, Modulo, Max,
     * comparisons and controls_if, and nothing else: there is no pow, no
     * smoothstep and no easing function. So:
     *   linear   exact
     *   step     exact (the value jumps at the key)
     *   easeIn   exact for the quadratic form, because u*u is one Multiply
     *   easeOut  exact, 1 - (1-u)*(1-u) is two Subtracts and one Multiply
     *   easeInOut approximated by the same quadratics either side of the
     *             midpoint, which is what the runtime uses too
     * Colour tracks step per segment rather than interpolating channel by
     * channel, because a CreateVector of three lerps per segment turns a
     * four-key track into a wall of blocks. Use the TypeScript runtime when a
     * colour has to slide.
     */
    function animationRule(clip, design) {
        var frameVar = 'ui_' + safeIdent(clip.name) + '_frame';
        var dur = Math.max(1, Number(clip.duration) || 60);
        var actions = [];

        // frame = (frame + 1) modulo duration when it loops, otherwise it
        // stops at the last tick.
        var stepped = { type: 'Add', inputs: {
            'VALUE-0': { block: bGetVar(frameVar) },
            'VALUE-1': { block: bNum(1) }
        } };
        if (clip.loop) {
            actions.push(bSetVar(frameVar, { type: 'Modulo', inputs: {
                'VALUE-0': { block: stepped },
                'VALUE-1': { block: bNum(dur) }
            } }));
        } else {
            actions.push(bSetVar(frameVar, { type: 'Max', inputs: {
                'VALUE-0': { block: bGetVar(frameVar) },
                'VALUE-1': { block: bNum(0) }
            } }));
            actions.push({ type: 'controls_if',
                inputs: {
                    IF0: { block: { type: 'LessThan', inputs: {
                        'VALUE-0': { block: bGetVar(frameVar) },
                        'VALUE-1': { block: bNum(dur) }
                    } } },
                    DO0: { block: bSetVar(frameVar, stepped) }
                } });
        }

        (clip.tracks || []).forEach(function (track) {
            var keys = (track.keys || []).slice().sort(function (a, b) { return a.t - b.t; });
            for (var i = 0; i + 1 < keys.length; i++) {
                var k0 = keys[i], k1 = keys[i + 1];
                var span = Math.max(1, k1.t - k0.t);
                var setter = setterFor(track.property);
                if (!setter) continue;

                var value;
                if (setter.vector) {
                    // Colour and position vectors step per segment.
                    value = setter.vector === 'xy'
                        ? bVec([k1.v[0], k1.v[1], 0])
                        : bVec(k1.v);
                } else if ((k0.ease || 'linear') === 'step') {
                    value = bNum(k1.v);
                } else {
                    // v0 + (v1 - v0) * u, u = (frame - t0) / span
                    var u = { type: 'Divide', inputs: {
                        'VALUE-0': { block: { type: 'Subtract', inputs: {
                            'VALUE-0': { block: bGetVar(frameVar) },
                            'VALUE-1': { block: bNum(k0.t) }
                        } } },
                        'VALUE-1': { block: bNum(span) }
                    } };
                    var shaped = easeBlocks(k0.ease || 'linear', u);
                    value = { type: 'Add', inputs: {
                        'VALUE-0': { block: bNum(k0.v) },
                        'VALUE-1': { block: { type: 'Multiply', inputs: {
                            'VALUE-0': { block: bNum(Number(k1.v) - Number(k0.v)) },
                            'VALUE-1': { block: shaped }
                        } } }
                    } };
                }

                actions.push({ type: 'controls_if', inputs: {
                    IF0: { block: { type: 'And', inputs: {
                        'VALUE-0': { block: { type: 'GreaterThanEqualTo', inputs: {
                            'VALUE-0': { block: bGetVar(frameVar) },
                            'VALUE-1': { block: bNum(k0.t) }
                        } } },
                        'VALUE-1': { block: { type: 'LessThan', inputs: {
                            'VALUE-0': { block: bGetVar(frameVar) },
                            'VALUE-1': { block: bNum(k1.t) }
                        } } }
                    } } },
                    DO0: { block: { type: setter.block, inputs: {
                        'VALUE-0': { block: bFind(track.widget) },
                        'VALUE-1': { block: value }
                    } } }
                } });
            }
        });

        return bRule(clip.name + ' animation', 'Ongoing', actions, true);
    }

    function easeBlocks(kind, u) {
        if (kind === 'easeIn') {
            return { type: 'Multiply', inputs: { 'VALUE-0': { block: u }, 'VALUE-1': { block: u } } };
        }
        if (kind === 'easeOut') {
            var inv = { type: 'Subtract', inputs: { 'VALUE-0': { block: bNum(1) }, 'VALUE-1': { block: u } } };
            return { type: 'Subtract', inputs: {
                'VALUE-0': { block: bNum(1) },
                'VALUE-1': { block: { type: 'Multiply', inputs: { 'VALUE-0': { block: inv }, 'VALUE-1': { block: inv } } } }
            } };
        }
        if (kind === 'easeInOut') {
            // 3u^2 - 2u^3 as (u*u) * (3 - 2u): two Multiplies and one Subtract.
            var uu = { type: 'Multiply', inputs: { 'VALUE-0': { block: u }, 'VALUE-1': { block: u } } };
            var tail = { type: 'Subtract', inputs: {
                'VALUE-0': { block: bNum(3) },
                'VALUE-1': { block: { type: 'Multiply', inputs: { 'VALUE-0': { block: bNum(2) }, 'VALUE-1': { block: u } } } }
            } };
            return { type: 'Multiply', inputs: { 'VALUE-0': { block: uu }, 'VALUE-1': { block: tail } } };
        }
        return u;
    }

    // property -> the Set* that writes it
    var SETTERS = {
        position:   { block: 'SetUIWidgetPosition', vector: 'xy', fn: 'SetUIWidgetPosition' },
        size:       { block: 'SetUIWidgetSize',     vector: 'xy', fn: 'SetUIWidgetSize' },
        bgAlpha:    { block: 'SetUIWidgetBgAlpha',  fn: 'SetUIWidgetBgAlpha' },
        bgColor:    { block: 'SetUIWidgetBgColor',  vector: 'rgb', fn: 'SetUIWidgetBgColor' },
        textColor:  { block: 'SetUITextColor',      vector: 'rgb', fn: 'SetUITextColor' },
        textAlpha:  { block: 'SetUITextAlpha',      fn: 'SetUITextAlpha' },
        textSize:   { block: 'SetUITextSize',       fn: 'SetUITextSize' },
        visible:    { block: 'SetUIWidgetVisible',  bool: true, fn: 'SetUIWidgetVisible' },
        imageAlpha: { block: 'SetUIImageAlpha',     fn: 'SetUIImageAlpha' },
        padding:    { block: 'SetUIWidgetPadding',  fn: 'SetUIWidgetPadding' }
    };
    function setterFor(prop) { return SETTERS[prop] || null; }

    var ANIMATABLE = ['position', 'size', 'bgAlpha', 'bgColor', 'textColor', 'textSize', 'visible'];

    /* ==================================================================== *
     * Animation sampling
     * ==================================================================== */

    function ease(kind, u) {
        u = u < 0 ? 0 : (u > 1 ? 1 : u);
        switch (kind) {
            case 'step': return 0;
            case 'easeIn': return u * u;
            case 'easeOut': return 1 - (1 - u) * (1 - u);
            case 'easeInOut': return u * u * (3 - 2 * u);
            default: return u;
        }
    }

    function lerp(a, b, u) {
        if (Array.isArray(a) && Array.isArray(b)) {
            var out = [];
            for (var i = 0; i < Math.max(a.length, b.length); i++) {
                out.push((a[i] || 0) + ((b[i] || 0) - (a[i] || 0)) * u);
            }
            return out;
        }
        if (typeof a === 'boolean' || typeof b === 'boolean') return u >= 1 ? b : a;
        return Number(a) + (Number(b) - Number(a)) * u;
    }

    // The value of one track at tick t. Before the first key it holds the
    // first value; after the last it holds the last.
    function sampleTrack(track, t) {
        var keys = (track.keys || []).slice().sort(function (a, b) { return a.t - b.t; });
        if (!keys.length) return undefined;
        if (t <= keys[0].t) return keys[0].v;
        if (t >= keys[keys.length - 1].t) return keys[keys.length - 1].v;
        for (var i = 0; i + 1 < keys.length; i++) {
            var k0 = keys[i], k1 = keys[i + 1];
            if (t >= k0.t && t <= k1.t) {
                var span = k1.t - k0.t;
                var u = span <= 0 ? 1 : (t - k0.t) / span;
                return lerp(k0.v, k1.v, ease(k0.ease || 'linear', u));
            }
        }
        return keys[keys.length - 1].v;
    }

    // { widgetName: { property: value } } for a clip at tick t.
    function sampleClip(clip, t) {
        var time = Number(t) || 0;
        var dur = Math.max(1, Number(clip.duration) || 60);
        if (clip.loop) { time = time % dur; if (time < 0) time += dur; }
        else time = time < 0 ? 0 : (time > dur ? dur : time);

        var out = {};
        (clip.tracks || []).forEach(function (tr) {
            var v = sampleTrack(tr, time);
            if (v === undefined) return;
            if (!out[tr.widget]) out[tr.widget] = {};
            out[tr.widget][tr.property] = v;
        });
        return out;
    }

    // The design with one clip's values applied, for the canvas preview.
    function applySample(design, sample) {
        var d = normalise(design);
        Object.keys(sample || {}).forEach(function (name) {
            var node = findByName(d.widgets, name);
            if (!node) return;
            var props = sample[name];
            Object.keys(props).forEach(function (p) { node[p] = props[p]; });
        });
        return d;
    }

    /* ==================================================================== *
     * The animation runtime export (TypeScript)
     * ==================================================================== */

    function exportAnimTs(design) {
        var d = normalise(design);
        var out = [];
        out.push('// BF6 UI BUILDER export: animation clips');
        out.push('// Pair this with runtime/ui-anim.ts. Widgets are created once by the');
        out.push('// ParseUI export; this only writes properties that changed.');
        out.push("import { UiAnim, UiClip } from './ui-anim';");
        out.push('');
        out.push('export const clips: UiClip[] = ' + JSON.stringify(d.animations || [], null, 4) + ';');
        out.push('');
        out.push('export const anim = new UiAnim(clips);');
        out.push('');
        out.push('// Call once per tick. Events.OngoingGlobal is the usual home; a');
        out.push('// utils Timers interval works too when a slower rate is enough.');
        out.push('export function tickUi(): void { anim.tick(); }');
        return out.join('\n');
    }

    /* ==================================================================== *
     * Import: a ParseUI tree out of TypeScript
     * ==================================================================== */

    // Find each ParseUI( { ... } ) payload by balancing parentheses, then
    // turn the object literal into JSON the sane way: enum member expressions
    // become tagged strings, string keys get quoted, trailing commas go.
    //
    // The call site must be followed by an object literal to count. Two reasons.
    // The export now writes its own `function ParseUI(spec: UiSpec)` at the top
    // of the file, and a bare name search would try to read that declaration's
    // parameter list as a widget tree. And older exports called it as
    // `modlib.ParseUI(` - a package that never existed, see exportParseUi - so
    // matching on the bare name is also what keeps those files importable.
    function extractParseUi(source) {
        var out = [];
        var marker = 'ParseUI';
        var at = 0;
        while (true) {
            var i = source.indexOf(marker, at);
            if (i < 0) break;
            var open = i + marker.length;
            while (open < source.length && /\s/.test(source[open])) open++;
            if (source[open] !== '(') { at = i + marker.length; continue; }
            var probe = open + 1;
            while (probe < source.length && /\s/.test(source[probe])) probe++;
            if (source[probe] !== '{') { at = i + marker.length; continue; }
            var depth = 0, j = open, inStr = null;
            for (; j < source.length; j++) {
                var c = source[j];
                if (inStr) { if (c === '\\') j++; else if (c === inStr) inStr = null; continue; }
                if (c === '"' || c === "'" || c === '`') { inStr = c; continue; }
                if (c === '(') depth++;
                else if (c === ')') { depth--; if (depth === 0) break; }
            }
            if (depth !== 0) break;
            out.push(source.slice(open + 1, j));
            at = j + 1;
        }
        return out;
    }

    function literalToJson(text) {
        var s = text;
        // mod.Message(mod.stringkeys.foo, "a", "b") -> "@msg:foo|a|b"
        s = s.replace(/mod\.Message\(\s*mod\.stringkeys\.([A-Za-z0-9_.]+)([^)]*)\)/g, function (m, key, rest) {
            var args = (rest.match(/"([^"]*)"|'([^']*)'|([0-9.]+)/g) || []).map(function (a) {
                return a.replace(/^['"]|['"]$/g, '');
            });
            return '"@msg:' + key + (args.length ? '|' + args.join('|') : '') + '"';
        });
        // mod.stringkeys.foo -> "@key:foo"
        s = s.replace(/mod\.stringkeys\.([A-Za-z0-9_.]+)/g, '"@key:$1"');
        // mod.UIAnchor.TopLeft -> "TopLeft"
        s = s.replace(/mod\.(UIAnchor|UIBgFill|UIDepth|UIImageType|UIButtonEvent)\.([A-Za-z0-9_]+)/g, '"$2"');
        // single-quoted strings -> double
        s = s.replace(/'([^'\\]*(?:\\.[^'\\]*)*)'/g, function (m, inner) { return JSON.stringify(inner); });
        // unquoted keys -> quoted
        s = s.replace(/([{,]\s*)([A-Za-z_][A-Za-z0-9_]*)\s*:/g, '$1"$2":');
        // trailing commas
        s = s.replace(/,(\s*[}\]])/g, '$1');
        return JSON.parse(s);
    }

    function importParseUi(source, stringsJson, name) {
        var payloads = extractParseUi(source);
        if (!payloads.length) throw new Error('No ParseUI call with a widget tree was found in that code.');
        var d = newDesign(name || 'imported');
        var given = {};
        if (stringsJson) {
            try { given = typeof stringsJson === 'string' ? JSON.parse(stringsJson) : stringsJson; }
            catch (e) { given = {}; }
        }

        function conv(p) {
            var n = makeNode(p.type || 'Container');
            n.name = p.name || n.name;
            n.position = (p.position || [0, 0]).slice();
            n.size = (p.size || [100, 50]).slice();
            n.anchor = p.anchor || 'TopLeft';
            n.visible = p.visible !== false;
            n.padding = Number(p.padding) || 0;
            n.bgColor = (p.bgColor || [0.2, 0.2, 0.2]).slice();
            n.bgAlpha = p.bgAlpha === undefined ? 1 : Number(p.bgAlpha);
            n.bgFill = p.bgFill || 'None';
            n.depth = DEPTHS.indexOf(p.depth) >= 0 ? p.depth : 'Inherit';
            if (n.type === 'Text') {
                var raw = p.textLabel;
                if (typeof raw === 'string' && raw.indexOf('@msg:') === 0) {
                    var parts = raw.slice(5).split('|');
                    var key = parts.shift();
                    d.strings[n.name] = given[key] !== undefined ? given[key]
                        : (given[n.name] !== undefined ? given[n.name] : '{}');
                    n.textArgs = parts;
                } else if (typeof raw === 'string' && raw.indexOf('@key:') === 0) {
                    var k = raw.slice(5);
                    d.strings[n.name] = given[k] !== undefined ? given[k]
                        : (given[n.name] !== undefined ? given[n.name] : k);
                } else if (raw) {
                    d.strings[n.name] = String(raw);
                }
                n.textLabel = d.strings[n.name] || '';
                n.textColor = (p.textColor || [1, 1, 1]).slice();
                n.textAlpha = p.textAlpha === undefined ? 1 : Number(p.textAlpha);
                n.textSize = p.textSize === undefined ? 24 : Number(p.textSize);
                n.textAnchor = p.textAnchor || 'Center';
            }
            if (n.type === 'Image') {
                n.imageType = p.imageType || 'None';
                n.imageColor = (p.imageColor || [1, 1, 1]).slice();
                n.imageAlpha = p.imageAlpha === undefined ? 1 : Number(p.imageAlpha);
            }
            if (n.type === 'Button') {
                n.buttonEnabled = p.buttonEnabled !== false;
                ['Base', 'Disabled', 'Pressed', 'Hover', 'Focused'].forEach(function (s) {
                    if (p['buttonColor' + s]) n['buttonColor' + s] = p['buttonColor' + s].slice();
                    if (p['buttonAlpha' + s] !== undefined) n['buttonAlpha' + s] = Number(p['buttonAlpha' + s]);
                });
            }
            n.children = (p.children || []).map(conv);
            return n;
        }

        payloads.forEach(function (text) {
            d.widgets.push(conv(literalToJson(text)));
        });
        return d;
    }

    // Anything the tool is handed: our design, the community's params array,
    // a workspace-shaped object, or TypeScript source.
    function importAny(text, stringsJson, name) {
        var trimmed = String(text || '').trim();
        if (!trimmed) throw new Error('Nothing to import.');
        if (trimmed.charAt(0) === '{' || trimmed.charAt(0) === '[') {
            var obj = JSON.parse(trimmed);
            if (obj && obj.format === 'bf6-ui-design') return normalise(obj);
            if (Array.isArray(obj)) return fromCommunityParams(obj, name);
            if (obj && obj.params) return fromCommunityParams(obj.params, name);
            if (obj && obj.widgets) return normalise(obj);
            throw new Error('That JSON is not a design, and not a community builder export.');
        }
        return importParseUi(trimmed, stringsJson, name);
    }

    /* ==================================================================== *
     * Templates
     * ==================================================================== */

    // Every template is built out of the style guide's own colours and its
    // own alphas: dark panels at 0.75, accent bands at 0.75, text over blur.
    var INK = [0.031, 0.043, 0.043];        // 080B0B
    var ICE = [0.835, 0.922, 0.976];        // D5EBF9
    var CYAN = [0.439, 0.922, 1.000];       // 70EBFF
    var RED = [1.000, 0.514, 0.380];        // FF8361
    var GREEN = [0.678, 0.992, 0.525];      // ADFD86
    var YELLOW = [1.000, 0.988, 0.612];     // FFFC9C
    var WHITE = [1, 1, 1];
    var DARK_CYAN = [0.075, 0.184, 0.247];  // 132F3F
    var DARK_RED = [0.251, 0.094, 0.067];   // 401811
    var DARK_GREEN = [0.278, 0.447, 0.212]; // 477236
    var DARK_YELLOW = [0.443, 0.376, 0.000];// 716000
    var SLATE = [0.212, 0.224, 0.235];      // 36393C
    var TEXT = ICE;
    var ACCENT = CYAN;

    function t(type, over) { return makeNode(type, over); }

    var TEMPLATES = {
        'frosted-panel': function () {
            var d = newDesign('frostedPanel');
            var root = t('Container', { name: 'panel', position: [16, 16], size: [360, 220],
                anchor: 'TopLeft', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 12,
                depth: 'AboveGameUI' });
            var title = t('Text', { name: 'panelTitle', position: [0, 0], size: [336, 26],
                anchor: 'TopLeft', textAnchor: 'CenterLeft', textSize: 20, textColor: TEXT, bgAlpha: 0 , textBgBlur: true });
            d.strings.panelTitle = 'PANEL';
            root.children = [title];
            d.widgets = [root];
            return d;
        },
        'header-accent': function () {
            var d = newDesign('header');
            var root = t('Container', { name: 'header', position: [0, 0], size: [640, 64],
                anchor: 'TopCenter', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 0 });
            var bar = t('Container', { name: 'headerAccent', position: [0, 0], size: [640, 4],
                anchor: 'TopCenter', bgFill: 'GradientLeft', bgColor: ACCENT, bgAlpha: ALPHA.band });
            var label = t('Text', { name: 'headerLabel', position: [0, 0], size: [640, 60],
                anchor: 'BottomCenter', textAnchor: 'Center', textSize: 32, textColor: TEXT, bgAlpha: 0 , textBgBlur: true });
            d.strings.headerLabel = 'OPERATION NAME';
            root.children = [bar, label];
            d.widgets = [root];
            return d;
        },
        'tab-menu': function () {
            var d = newDesign('tabMenu');
            var root = t('Container', { name: 'menuRoot', position: [0, 0], size: [900, 560],
                anchor: 'Center', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 16 });
            var tabs = t('Container', { name: 'tabBar', position: [0, 0], size: [868, 48],
                anchor: 'TopLeft', bgFill: 'None', bgAlpha: 0 });
            var pages = [];
            for (var i = 0; i < 3; i++) {
                var b = t('Button', { name: 'tab' + i, position: [i * 292, 0], size: [284, 48],
                    anchor: 'TopLeft', bgFill: 'Solid', bgColor: [1, 1, 1], bgAlpha: 1,
                    buttonColorBase: [0.10, 0.11, 0.12], buttonAlphaBase: 1,
                    buttonColorHover: [0.18, 0.20, 0.22], buttonAlphaHover: 1,
                    buttonColorPressed: ACCENT, buttonAlphaPressed: 1,
                    buttonColorFocused: [0.24, 0.26, 0.28], buttonAlphaFocused: 1 });
                var bl = t('Text', { name: 'tab' + i + 'Label', position: [0, 0], size: [284, 48],
                    anchor: 'Center', textAnchor: 'Center', textSize: 20, textColor: TEXT, bgAlpha: 0 , textBgBlur: true });
                d.strings['tab' + i + 'Label'] = 'TAB ' + (i + 1);
                b.children = [bl];
                tabs.children.push(b);
                var page = t('Container', { name: 'page' + i, position: [0, 64], size: [868, 448],
                    anchor: 'TopLeft', bgFill: 'OutlineThin', bgColor: TEXT, bgAlpha: 0.35,
                    visible: i === 0, padding: 12 });
                pages.push(page);
            }
            root.children = [tabs].concat(pages);
            d.widgets = [root];
            return d;
        },
        'counter': function () {
            var d = newDesign('counter');
            var root = t('Container', { name: 'counter', position: [24, 24], size: [220, 96],
                anchor: 'TopRight', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 10 });
            var cap = t('Text', { name: 'counterCaption', position: [0, 0], size: [200, 22],
                anchor: 'TopLeft', textAnchor: 'CenterLeft', textSize: 16, textColor: TEXT, bgAlpha: 0 , textBgBlur: true });
            var val = t('Text', { name: 'counterValue', position: [0, 0], size: [200, 44],
                anchor: 'BottomLeft', textAnchor: 'CenterLeft', textSize: 40, textColor: GREEN, bgAlpha: 0,
                textArgs: ['0'] , textBgBlur: true });
            d.strings.counterCaption = 'SCORE';
            d.strings.counterValue = '{}';
            root.children = [cap, val];
            d.widgets = [root];
            return d;
        },
        'scoreboard': function () {
            var d = newDesign('scoreboard');
            var root = t('Container', { name: 'board', position: [0, 0], size: [760, 480],
                anchor: 'Center', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 16 });
            var head = t('Container', { name: 'boardHead', position: [0, 0], size: [728, 4],
                anchor: 'TopLeft', bgFill: 'GradientLeft', bgColor: ACCENT, bgAlpha: ALPHA.band });
            root.children.push(head);
            for (var i = 0; i < 8; i++) {
                var row = t('Container', { name: 'row' + i, position: [0, 16 + i * 52], size: [728, 44],
                    anchor: 'TopLeft', bgFill: 'Solid', bgColor: [0.09, 0.10, 0.11],
                    bgAlpha: i % 2 ? 0.55 : 0.30, padding: 8 });
                var nameT = t('Text', { name: 'row' + i + 'Name', position: [0, 0], size: [460, 28],
                    anchor: 'CenterLeft', textAnchor: 'CenterLeft', textSize: 20, textColor: TEXT,
                    bgAlpha: 0, textArgs: ['PLAYER'] , textBgBlur: true });
                var scoreT = t('Text', { name: 'row' + i + 'Score', position: [0, 0], size: [180, 28],
                    anchor: 'CenterRight', textAnchor: 'CenterRight', textSize: 20, textColor: GREEN,
                    bgAlpha: 0, textArgs: ['0'] , textBgBlur: true });
                d.strings['row' + i + 'Name'] = '{}';
                d.strings['row' + i + 'Score'] = '{}';
                row.children = [nameT, scoreT];
                root.children.push(row);
            }
            d.widgets = [root];
            return d;
        },
        'button-menu': function () {
            var d = newDesign('buttonMenu');
            var root = t('Container', { name: 'menu', position: [0, 0], size: [320, 296],
                anchor: 'Center', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 12 });
            for (var i = 0; i < 4; i++) {
                var b = t('Button', { name: 'menuBtn' + i, position: [0, i * 68], size: [296, 56],
                    anchor: 'TopLeft', bgFill: 'Solid', bgColor: [1, 1, 1], bgAlpha: 1,
                    buttonColorBase: [0.10, 0.11, 0.12], buttonAlphaBase: 1,
                    buttonColorHover: [0.20, 0.22, 0.24], buttonAlphaHover: 1,
                    buttonColorPressed: ACCENT, buttonAlphaPressed: 1,
                    buttonColorFocused: [0.26, 0.28, 0.30], buttonAlphaFocused: 1,
                    buttonColorDisabled: [0.08, 0.08, 0.08], buttonAlphaDisabled: 0.5 });
                var l = t('Text', { name: 'menuBtn' + i + 'Label', position: [0, 0], size: [296, 56],
                    anchor: 'Center', textAnchor: 'Center', textSize: 22, textColor: TEXT, bgAlpha: 0 , textBgBlur: true });
                d.strings['menuBtn' + i + 'Label'] = 'OPTION ' + (i + 1);
                b.children = [l];
                root.children.push(b);
            }
            d.widgets = [root];
            return d;
        },
        'objective-banner': function () {
            var d = newDesign('objective');
            var root = t('Container', { name: 'banner', position: [0, 96], size: [720, 88],
                anchor: 'TopCenter', bgFill: 'GradientBottom', bgColor: INK, bgAlpha: ALPHA.panel, padding: 8 });
            var bar = t('Container', { name: 'bannerAccent', position: [0, 0], size: [720, 3],
                anchor: 'TopCenter', bgFill: 'GradientLeft', bgColor: RED, bgAlpha: ALPHA.band });
            var head = t('Text', { name: 'bannerHead', position: [0, 6], size: [700, 24],
                anchor: 'TopCenter', textAnchor: 'Center', textSize: 18, textColor: RED, bgAlpha: 0 , textBgBlur: true });
            var body = t('Text', { name: 'bannerBody', position: [0, 8], size: [700, 40],
                anchor: 'BottomCenter', textAnchor: 'Center', textSize: 30, textColor: TEXT, bgAlpha: 0 , textBgBlur: true });
            d.strings.bannerHead = 'OBJECTIVE';
            d.strings.bannerBody = 'CAPTURE THE SITE';
            root.children = [bar, head, body];
            d.widgets = [root];
            d.animations = [{
                name: 'bannerIn', duration: 45, loop: false,
                tracks: [
                    { widget: 'banner', property: 'bgAlpha',
                      keys: [{ t: 0, v: 0, ease: 'easeOut' }, { t: 20, v: ALPHA.panel }] },
                    { widget: 'banner', property: 'position',
                      keys: [{ t: 0, v: [0, 60], ease: 'easeOut' }, { t: 20, v: [0, 96] }] }
                ]
            }];
            return d;
        },
        'timer': function () {
            var d = newDesign('timer');
            var root = t('Container', { name: 'timer', position: [0, 24], size: [240, 72],
                anchor: 'TopCenter', bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 6 });
            var v = t('Text', { name: 'timerValue', position: [0, 0], size: [228, 60],
                anchor: 'Center', textAnchor: 'Center', textSize: 44, textColor: TEXT, bgAlpha: 0,
                textArgs: ['5', '00'] , textBgBlur: true });
            d.strings.timerValue = '{}:{}';
            root.children = [v];
            d.widgets = [root];
            d.animations = [{
                name: 'timerPulse', duration: 60, loop: true,
                tracks: [{ widget: 'timerValue', property: 'textSize',
                    keys: [{ t: 0, v: 44, ease: 'easeInOut' }, { t: 30, v: 52, ease: 'easeInOut' }, { t: 60, v: 44 }] }]
            }];
            return d;
        }
,

        /*
         * The style guide's own two examples, rebuilt as designs.
         *
         * OPTIONS TILE GRID. Five colour families down, and for each one an
         * OPTION tile and a SELECTED tile: the option is the dark fill with a
         * bright icon, the selected one flips to the bright fill with a dark
         * icon. Both columns are shown so the two states can be compared while
         * they are being coloured; delete the ones you do not ship, or drive
         * the states at runtime with SetUIButtonColorBase and the per-state
         * setters the export already writes.
         */
        'options-grid': function () {
            var d = newDesign('optionsGrid');
            var families = [
                { key: 'White',  bright: WHITE,  dark: SLATE },
                { key: 'Red',    bright: RED,    dark: DARK_RED },
                { key: 'Cyan',   bright: CYAN,   dark: DARK_CYAN },
                { key: 'Green',  bright: GREEN,  dark: DARK_GREEN },
                { key: 'Yellow', bright: YELLOW, dark: DARK_YELLOW }
            ];
            var TILE = 84, GAP = 8, COLS = 4;
            var gridW = COLS * TILE + (COLS - 1) * GAP;
            var gridH = families.length * TILE + (families.length - 1) * GAP;

            var root = t('Container', { name: 'optionsPanel', position: [0, 0],
                size: [gridW + 32, gridH + 64], anchor: 'Center',
                bgFill: 'Blur', bgColor: INK, bgAlpha: ALPHA.panel, padding: 16 });

            // Column captions, so the two states read as what they are.
            for (var c = 0; c < COLS; c++) {
                var cap = t('Text', { name: 'optHead' + c, position: [c * (TILE + GAP), 0],
                    size: [TILE, 20], anchor: 'TopLeft', textAnchor: 'Center', textSize: 13,
                    textColor: ICE, textAlpha: 0.8, bgAlpha: 0, textBgBlur: true });
                d.strings['optHead' + c] = (c % 2 === 0) ? 'OPTION' : 'SELECTED';
                root.children.push(cap);
            }

            families.forEach(function (fam, r) {
                for (var c2 = 0; c2 < COLS; c2++) {
                    var selected = (c2 % 2 === 1);
                    var nm = 'tile' + fam.key + (selected ? 'Sel' : 'Opt') + c2;
                    var tile = t('Button', {
                        name: nm,
                        position: [c2 * (TILE + GAP), 28 + r * (TILE + GAP)],
                        size: [TILE, TILE], anchor: 'TopLeft',
                        bgFill: 'Solid', bgColor: WHITE, bgAlpha: 1,
                        buttonColorBase: selected ? fam.bright : fam.dark,
                        buttonAlphaBase: selected ? 1 : ALPHA.band,
                        buttonColorHover: fam.bright, buttonAlphaHover: ALPHA.band,
                        buttonColorPressed: fam.bright, buttonAlphaPressed: 1,
                        buttonColorFocused: fam.bright, buttonAlphaFocused: ALPHA.panelLight,
                        buttonColorDisabled: SLATE, buttonAlphaDisabled: 0.5
                    });
                    var icon = t('Image', { name: nm + 'Icon', position: [0, 0], size: [44, 44],
                        anchor: 'Center', bgFill: 'None', bgAlpha: 0,
                        imageType: 'SpawnBeacon',
                        imageColor: selected ? INK : fam.bright, imageAlpha: 1 });
                    tile.children = [icon];
                    root.children.push(tile);
                }
            });

            d.widgets = [root];
            return d;
        },

        /*
         * ROUND WON banner. A full-width band over a blur, the headline in the
         * family's bright colour, the subtitle in white and a small timer under
         * it. Swap the two colours for the red or the green family and the same
         * banner reads as a loss or an objective.
         */
        'round-banner': function () {
            var d = newDesign('roundBanner');
            var band = t('Container', { name: 'roundBand', position: [0, 0], size: [1920, 190],
                anchor: 'Center', bgFill: 'Blur', bgColor: DARK_CYAN, bgAlpha: ALPHA.band, padding: 0 });
            var rule = t('Container', { name: 'roundRule', position: [0, 0], size: [1920, 3],
                anchor: 'TopCenter', bgFill: 'Solid', bgColor: CYAN, bgAlpha: ALPHA.band });
            var head = t('Text', { name: 'roundHead', position: [0, 26], size: [1200, 76],
                anchor: 'TopCenter', textAnchor: 'Center', textSize: 68, textColor: CYAN,
                bgAlpha: 0, textBgBlur: true });
            var sub = t('Text', { name: 'roundSub', position: [0, 104], size: [1200, 34],
                anchor: 'TopCenter', textAnchor: 'Center', textSize: 30, textColor: WHITE,
                bgAlpha: 0, textBgBlur: true });
            var timer = t('Text', { name: 'roundTimer', position: [0, 18], size: [400, 26],
                anchor: 'BottomCenter', textAnchor: 'Center', textSize: 20, textColor: ICE,
                textAlpha: 0.85, bgAlpha: 0, textBgBlur: true, textArgs: ['0', '09'] });
            d.strings.roundHead = 'ROUND WON!';
            d.strings.roundSub = 'NEXT ROUND';
            d.strings.roundTimer = '{}:{}';
            band.children = [rule, head, sub, timer];
            d.widgets = [band];
            d.animations = [{
                name: 'roundIn', duration: 40, loop: false,
                tracks: [
                    { widget: 'roundBand', property: 'bgAlpha',
                      keys: [{ t: 0, v: 0, ease: 'easeOut' }, { t: 18, v: ALPHA.band }] },
                    { widget: 'roundBand', property: 'size',
                      keys: [{ t: 0, v: [1920, 0], ease: 'easeOut' }, { t: 18, v: [1920, 190] }] },
                    { widget: 'roundHead', property: 'textSize',
                      keys: [{ t: 12, v: 40, ease: 'easeOut' }, { t: 32, v: 68 }] }
                ]
            }];
            return d;
        }
    };

    function templateNames() { return Object.keys(TEMPLATES); }
    function template(name) {
        var f = TEMPLATES[name];
        if (!f) throw new Error('No template named ' + name);
        return f();
    }

    /* ==================================================================== *
     * The whole export set, for one button and for the console
     * ==================================================================== */

    function exportAll(design) {
        var d = normalise(design);
        var parse = exportParseUi(d);
        var comm = toCommunityParams(d);
        return {
            design: JSON.stringify(d, null, 2),
            typescript: parse.typescript,
            strings: parse.stringsJson,
            deluca: exportDeluca(d, { mode: 'classes' }).typescript,
            solid: exportDeluca(d, { mode: 'solid' }).typescript,
            blocks: JSON.stringify(exportBlocks(d), null, 1),
            community: JSON.stringify(comm.params, null, 2),
            anim: exportAnimTs(d),
            // parse.warnings already carries the ASCII and slot checks, which
            // are about the design rather than any one format, so the UI module
            // warnings are added on their own to avoid repeating them.
            warnings: parse.warnings
                .concat(delucaWarnings(d))
                .concat(hasWeaponImage(d) ? ['Weapon cards export through native TypeScript and blocks. The DeLuca exports use the native TypeScript backend; the community format cannot carry weapon packages.'] : [])
                .concat(comm.dropped.length
                    ? ['The community builder format has no field for: ' + comm.dropped.join(', ') + '.'] : [])
        };
    }

    /* ==================================================================== */
    return {
        VERSION: VERSION,
        TYPES: TYPES,
        ANCHORS: ANCHORS,
        BG_FILLS: BG_FILLS,
        DEPTHS: DEPTHS,
        DEPTH_CHOICES: DEPTH_CHOICES,
        DEPTH_LABEL: DEPTH_LABEL,
        HUD_REGIONS: HUD_REGIONS,
        hudLayout: hudLayout,
        resolveDepths: resolveDepths,
        depthOf: depthOf,
        setDepth: setDepth,
        IMAGE_TYPES: IMAGE_TYPES,
        BUTTON_EVENTS: BUTTON_EVENTS,
        ANIMATABLE: ANIMATABLE,
        PALETTE: PALETTE,
        ALPHA: ALPHA,
        colour: colour,
        setPalette: setPalette,
        ASPECTS: ASPECTS,
        CANVAS_WIDTH: CANVAS_WIDTH,
        CANVAS_HEIGHT: CANVAS_HEIGHT,
        COMMUNITY_ORDER: COMMUNITY_ORDER,
        SETTERS: SETTERS,

        defaults: defaults,
        makeNode: makeNode,
        newDesign: newDesign,
        normalise: normalise,
        resetIds: resetIds,
        newId: newId,
        clone: clone,

        walk: walk,
        findById: findById,
        findByName: findByName,
        parentOf: parentOf,
        allNames: allNames,
        uniqueName: uniqueName,
        duplicateNode: duplicateNode,

        anchorFrac: anchorFrac,
        anchorName: anchorName,
        placeIn: placeIn,
        contentBox: contentBox,
        screenRect: screenRect,
        layout: layout,
        audit: audit,
        auditAll: auditAll,
        rect: rect,

        toCommunityParams: toCommunityParams,
        fromCommunityParams: fromCommunityParams,
        exportParseUi: exportParseUi,
        exportDeluca: exportDeluca,
        exportBlocks: exportBlocks,
        exportAnimTs: exportAnimTs,
        exportAll: exportAll,
        importParseUi: importParseUi,
        importAny: importAny,
        extractParseUi: extractParseUi,

        ease: ease,
        lerp: lerp,
        sampleTrack: sampleTrack,
        sampleClip: sampleClip,
        applySample: applySample,
        setterFor: setterFor,
        countSlots: countSlots,
        argSlots: argSlots,
        safeIdent: safeIdent,

        templateNames: templateNames,
        template: template
    };
});
