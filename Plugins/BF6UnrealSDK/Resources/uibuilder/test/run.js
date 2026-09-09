/*
 * BF6 UI BUILDER tests.
 *
 *   node Resources/uibuilder/test/run.js
 *
 * Three things are worth a test here, because all three are places where a
 * quiet mistake would look like a working design right up until it is in the
 * game: the anchor law, the export round trips, and the animation sampler.
 */
'use strict';

var path = require('path');
var UI = require(path.join(__dirname, '..', 'lib', 'bf6ui.js'));

var pass = 0, fail = 0;
var failures = [];

function ok(name, cond, detail) {
    if (cond) { pass++; return; }
    fail++;
    failures.push(name + (detail ? '  ->  ' + detail : ''));
}
function eq(name, got, want) {
    var a = JSON.stringify(got), b = JSON.stringify(want);
    ok(name, a === b, 'got ' + a + ', wanted ' + b);
}
function near(name, got, want, tol) {
    ok(name, Math.abs(got - want) <= (tol === undefined ? 1e-6 : tol),
        'got ' + got + ', wanted ' + want);
}

/* ===================================================================== *
 * 1. The anchor law
 *
 * Four 512 x 512 boxes at (16, 16) under the four corner anchors must land
 * tucked into their own corner of the 1920 x 1080 safe area. Offsets are
 * INWARD insets, so a right anchor runs +x to the LEFT and a bottom anchor
 * runs +y UPWARDS.
 * ===================================================================== */
(function anchorLaw() {
    var d = UI.newDesign('anchors');
    var corners = ['TopLeft', 'TopRight', 'BottomLeft', 'BottomRight'];
    corners.forEach(function (a) {
        d.widgets.push(UI.makeNode('Container', {
            name: 'box' + a, position: [16, 16], size: [512, 512], anchor: a,
            bgFill: 'Solid', bgColor: [1, 1, 1], bgAlpha: 1
        }));
    });

    var rects = UI.layout(d.widgets, 16 / 9);
    var by = {};
    d.widgets.forEach(function (w) { by[w.anchor] = rects[w.id]; });

    eq('TopLeft lands at 16,16',
        [by.TopLeft.left, by.TopLeft.top], [16, 16]);
    eq('TopLeft ends at 528,528',
        [by.TopLeft.right, by.TopLeft.bottom], [528, 528]);

    eq('TopRight lands at 1392,16',
        [by.TopRight.left, by.TopRight.top], [1920 - 16 - 512, 16]);
    eq('TopRight right edge is 16 in from the screen edge',
        by.TopRight.right, 1920 - 16);

    eq('BottomLeft lands at 16,552',
        [by.BottomLeft.left, by.BottomLeft.top], [16, 1080 - 16 - 512]);
    eq('BottomLeft bottom edge is 16 up from the screen edge',
        by.BottomLeft.bottom, 1080 - 16);

    eq('BottomRight lands at 1392,552',
        [by.BottomRight.left, by.BottomRight.top], [1920 - 16 - 512, 1080 - 16 - 512]);
    eq('BottomRight far corner is 16 in on both axes',
        [by.BottomRight.right, by.BottomRight.bottom], [1920 - 16, 1080 - 16]);

    // No two of the four touch: 512 + 512 + 32 is less than either dimension.
    ok('the four corner boxes do not overlap',
        UI.audit(d.widgets, 16 / 9).filter(function (f) { return f.kind === 'overlap'; }).length === 0);

    // Centre anchors put the widget's own centre on the offset point.
    var c = UI.makeNode('Container', { name: 'mid', position: [0, 0], size: [400, 200], anchor: 'Center' });
    var cr = UI.layout([c], 16 / 9)[c.id];
    eq('Center at 0,0 centres the widget', [cr.centerX, cr.centerY], [960, 540]);

    // A child anchors inside its parent's content box, not the canvas.
    var parent = UI.makeNode('Container', { name: 'p', position: [100, 100], size: [400, 300],
        anchor: 'TopLeft', padding: 20 });
    var child = UI.makeNode('Container', { name: 'c', position: [0, 0], size: [50, 50], anchor: 'BottomRight' });
    parent.children = [child];
    var r2 = UI.layout([parent], 16 / 9);
    eq('a child bottom-right sits inside the parent padding',
        [r2[child.id].right, r2[child.id].bottom],
        [100 + 400 - 20, 100 + 300 - 20]);
})();

/* ===================================================================== *
 * 2. Aspect presets and the clip check
 * ===================================================================== */
(function aspects() {
    var s16 = UI.screenRect(16 / 9);
    eq('16:9 screen is the whole canvas', [s16.left, s16.width], [0, 1920]);
    var s32 = UI.screenRect(32 / 9);
    ok('32:9 reveals canvas either side', s32.left < 0 && s32.width > 1920, 'left ' + s32.left);
    var s1610 = UI.screenRect(16 / 10);
    ok('16:10 hides canvas either side', s1610.left > 0 && s1610.width < 1920, 'left ' + s1610.left);

    // A 1920-wide centred bar fits 16:9 and clips on 16:10.
    var bar = UI.makeNode('Container', { name: 'wide', position: [0, 0], size: [1920, 40],
        anchor: 'Center', bgFill: 'Solid' });
    ok('a full-width bar is clean at 16:9',
        UI.audit([bar], 16 / 9).filter(function (f) { return f.kind === 'clip'; }).length === 0);
    ok('a full-width bar clips at 16:10',
        UI.audit([bar], 16 / 10).filter(function (f) { return f.kind === 'clip'; }).length === 1);

    // An edge-anchored widget drifts on ultrawide. That is the real thing
    // people hit, and the check is meant to name it.
    var edge = UI.makeNode('Container', { name: 'edge', position: [16, 16], size: [200, 60],
        anchor: 'TopRight', bgFill: 'Solid' });
    var drift = UI.audit([edge], 32 / 9).filter(function (f) { return f.kind === 'drift'; });
    ok('an edge-anchored widget is reported as drifting at 32:9', drift.length === 1,
        JSON.stringify(drift));
    var centred = UI.makeNode('Container', { name: 'mid', position: [0, 0], size: [200, 60],
        anchor: 'Center', bgFill: 'Solid' });
    ok('a centre-anchored widget does not drift',
        UI.audit([centred], 32 / 9).filter(function (f) { return f.kind === 'drift'; }).length === 0);
})();

/* ===================================================================== *
 * 3. Export round trips
 * ===================================================================== */
(function roundTrips() {
    var d = UI.template('tab-menu');

    // (a) design -> ParseUI TypeScript -> design
    var ts = UI.exportParseUi(d);
    ok('the ParseUI export calls its own ParseUI on a tree', ts.typescript.indexOf('= ParseUI(') > 0);
    // The export used to open with `import * as modlib from "modlib"`. No such
    // package exists, so nothing this backend produced would compile in a real
    // project. It builds widgets with mod.AddUI* now and imports nothing.
    ok('the ParseUI export imports nothing', ts.typescript.indexOf('import ') < 0);
    ok('the ParseUI export builds widgets with the engine calls',
        ts.typescript.indexOf('mod.AddUIContainer(') > 0 && ts.typescript.indexOf('mod.AddUIText(') > 0);
    ok('the ParseUI export writes enum members by name',
        ts.typescript.indexOf('mod.UIAnchor.') > 0 && ts.typescript.indexOf('mod.UIBgFill.') > 0);
    ok('the ParseUI export carries depth, which the community type does not',
        ts.typescript.indexOf('mod.UIDepth.AboveGameUI') > 0);

    var back = UI.importParseUi(ts.typescript, ts.stringsJson, 'tabMenu');
    eq('round trip keeps the root count', back.widgets.length, d.widgets.length);

    function shape(nodes) {
        return nodes.map(function (n) {
            return {
                name: n.name, type: n.type,
                position: n.position, size: n.size, anchor: n.anchor,
                bgFill: n.bgFill, bgAlpha: n.bgAlpha, padding: n.padding,
                visible: n.visible !== false,
                children: shape(n.children || [])
            };
        });
    }
    eq('round trip keeps every widget, name, box, anchor and fill',
        shape(back.widgets), shape(d.widgets));
    eq('round trip keeps the strings', back.strings, d.strings);

    // (b) design -> community builder format -> design
    var comm = UI.toCommunityParams(d);
    ok('the community export numbers its enums',
        typeof comm.params[0].anchor === 'number' && typeof comm.params[0].bgFill === 'number');
    eq('Center maps to the community index 3',
        comm.params[0].anchor, UI.COMMUNITY_ORDER.UIAnchor.indexOf('Center'));
    ok('the community export reports what it had to drop', Array.isArray(comm.dropped));

    var back2 = UI.fromCommunityParams(comm.params, 'tabMenu');
    eq('community round trip keeps every widget, name, box, anchor and fill',
        shape(back2.widgets), shape(d.widgets));
    eq('community round trip keeps the strings', back2.strings, d.strings);

    // (c) a community-shaped file lands through the one importer
    var viaAny = UI.importAny(JSON.stringify(comm.params), null, 'x');
    eq('importAny reads the community params array', shape(viaAny.widgets), shape(d.widgets));
    var viaDesign = UI.importAny(JSON.stringify(d), null, 'x');
    eq('importAny reads our own design file', shape(viaDesign.widgets), shape(d.widgets));
    var viaTs = UI.importAny(ts.typescript, ts.stringsJson, 'x');
    eq('importAny reads ParseUI TypeScript', shape(viaTs.widgets), shape(d.widgets));

    // (d) a hand-written ParseUI tree, the way the guide writes one
    var handWritten = [
        'const root = modlib.ParseUI({',
        '  type: "Container", name: "dbgRoot",',
        '  position: [16, 16], size: [360, 220], anchor: mod.UIAnchor.TopLeft,',
        '  bgFill: mod.UIBgFill.Blur, bgColor: [0.05, 0.06, 0.07], bgAlpha: 0.85,',
        '  depth: mod.UIDepth.AboveGameUI,',
        '  children: [',
        '    { type: "Text", name: "row0", position:[10,14], size:[340,22], anchor: mod.UIAnchor.TopLeft,',
        '      textAnchor: mod.UIAnchor.CenterLeft, textSize: 18, textColor:[0.678,0.992,0.525], bgAlpha:0,',
        '      textLabel: mod.Message(mod.stringkeys.dbg.row) },',
        '  ],',
        '});'
    ].join('\n');
    var hw = UI.importParseUi(handWritten, '{"dbg.row":"row {}"}', 'dbg');
    eq('a guide-shaped tree imports with its root', hw.widgets[0].name, 'dbgRoot');
    eq('a guide-shaped tree imports its blur fill', hw.widgets[0].bgFill, 'Blur');
    eq('a guide-shaped tree imports its depth', hw.widgets[0].depth, 'AboveGameUI');
    eq('a guide-shaped tree imports its child', hw.widgets[0].children[0].name, 'row0');
    eq('a mod.Message label resolves through strings.json', hw.strings.row0, 'row {}');
})();

/* ===================================================================== *
 * 4. The blocks snippet
 * ===================================================================== */
(function blocks() {
    var d = UI.template('counter');
    var snip = UI.exportBlocks(d);
    ok('the snippet has the shape the blocks editor pastes',
        snip.blocks && snip.blocks.languageVersion === 0 && Array.isArray(snip.blocks.blocks));

    var rule = snip.blocks.blocks[0];
    eq('the build rule is a ruleBlock', rule.type, 'ruleBlock');
    eq('the build rule runs on game mode start', rule.fields.EVENTTYPE, 'OnGameModeStarted');

    // Walk the statement chain and collect the block types in order.
    var chain = [];
    var b = rule.inputs.ACTIONS.block;
    while (b) { chain.push(b.type); b = b.next && b.next.block; }
    eq('the container is added before its children', chain[0], 'AddUIContainer');
    ok('a text widget is added', chain.indexOf('AddUIText') > 0);

    var container = rule.inputs.ACTIONS.block;
    eq('VALUE-0 is the widget name', container.inputs['VALUE-0'].block.fields.TEXT, 'counter');
    eq('VALUE-1 is the position vector', container.inputs['VALUE-1'].block.type, 'CreateVector');
    eq('VALUE-3 is the anchor enum item', container.inputs['VALUE-3'].block.type, 'UIAnchorItem');
    eq('the anchor enum names its enum in VALUE-0',
        container.inputs['VALUE-3'].block.fields['VALUE-0'], 'UIAnchor');
    eq('the anchor enum names its member in VALUE-1',
        container.inputs['VALUE-3'].block.fields['VALUE-1'], 'TopRight');
    eq('VALUE-9 is the fill enum item', container.inputs['VALUE-9'].block.type, 'UIBgFillItem');

    // A child names its parent through FindUIWidgetWithName.
    var text = null;
    b = rule.inputs.ACTIONS.block;
    while (b) { if (b.type === 'AddUIText') { text = b; break; } b = b.next && b.next.block; }
    eq('a child widget names its parent', text.inputs['VALUE-4'].block.type, 'FindUIWidgetWithName');
    eq('the parent lookup uses the parent name',
        text.inputs['VALUE-4'].block.inputs['VALUE-0'].block.fields.TEXT, 'counter');
    eq('the text label is a Message block', text.inputs['VALUE-10'].block.type, 'Message');

    // Buttons switch their events on, or nothing ever fires.
    var menu = UI.exportBlocks(UI.template('button-menu'));
    var types = [];
    b = menu.blocks.blocks[0].inputs.ACTIONS.block;
    while (b) { types.push(b.type); b = b.next && b.next.block; }
    ok('buttons enable their events', types.indexOf('EnableUIButtonEvent') > 0);
    ok('a button is added with the full-arity call', types.indexOf('AddUIButton') >= 0);

    // An animation becomes an Ongoing rule with a frame variable.
    var timer = UI.exportBlocks(UI.template('timer'));
    var anim = timer.blocks.blocks[1];
    eq('an animation is an Ongoing rule', anim.fields.EVENTTYPE, 'Ongoing');
    eq('an Ongoing rule declares its object type', anim.fields.OBJECTTYPE, 'Global');
    ok('an Ongoing rule is marked as one', anim.extraState.isOngoingEvent === true);
    var text2 = JSON.stringify(anim);
    ok('the animation rule steps a frame variable', text2.indexOf('{{VAR:ui_timerPulse_frame:}}') > 0);
    ok('a looping animation wraps with Modulo', text2.indexOf('"Modulo"') > 0);
    ok('the animation writes the property it animates', text2.indexOf('SetUITextSize') > 0);
})();

/* ===================================================================== *
 * 5. The animation sampler
 * ===================================================================== */
(function sampler() {
    var track = { widget: 'panel', property: 'bgAlpha',
        keys: [{ t: 0, v: 0, ease: 'linear' }, { t: 30, v: 1, ease: 'linear' }] };

    near('a linear track is 0 at its first key', UI.sampleTrack(track, 0), 0);
    near('a linear track is 1 at its last key', UI.sampleTrack(track, 30), 1);
    near('a linear track is half way at the midpoint', UI.sampleTrack(track, 15), 0.5);
    near('before the first key the value holds', UI.sampleTrack(track, -10), 0);
    near('after the last key the value holds', UI.sampleTrack(track, 999), 1);

    // Keyframes at tick boundaries: sampling at every whole tick must hit the
    // key values exactly, or an animation drifts a frame over a long clip.
    var multi = { widget: 'x', property: 'textSize',
        keys: [{ t: 0, v: 10 }, { t: 10, v: 20 }, { t: 20, v: 10 }] };
    near('a multi-key track hits its middle key exactly', UI.sampleTrack(multi, 10), 20);
    near('a multi-key track hits its last key exactly', UI.sampleTrack(multi, 20), 10);
    near('a multi-key track interpolates in the first segment', UI.sampleTrack(multi, 5), 15);
    near('a multi-key track interpolates in the second segment', UI.sampleTrack(multi, 15), 15);

    // Easing, at the same boundaries.
    near('easeIn is 0.25 at the midpoint', UI.ease('easeIn', 0.5), 0.25);
    near('easeOut is 0.75 at the midpoint', UI.ease('easeOut', 0.5), 0.75);
    near('easeInOut is 0.5 at the midpoint', UI.ease('easeInOut', 0.5), 0.5);
    eq('step holds until the next key', UI.ease('step', 0.99), 0);
    near('every easing is 0 at 0', UI.ease('easeInOut', 0), 0);
    near('every easing is 1 at 1', UI.ease('easeInOut', 1), 1);

    // Vectors interpolate channel by channel.
    var vec = { widget: 'x', property: 'position',
        keys: [{ t: 0, v: [0, 60] }, { t: 20, v: [0, 100] }] };
    eq('a vector track interpolates each channel', UI.sampleTrack(vec, 10), [0, 80]);

    // A clip loops on its duration and clamps when it does not.
    var loop = { name: 'l', duration: 20, loop: true, tracks: [multi] };
    near('a looping clip wraps at its duration', UI.sampleClip(loop, 25).x.textSize,
        UI.sampleClip(loop, 5).x.textSize);
    var once = { name: 'o', duration: 20, loop: false, tracks: [multi] };
    near('a one-shot clip holds past its duration', UI.sampleClip(once, 500).x.textSize, 10);

    // The preview applies a sample onto the design without touching the file.
    var d = UI.template('objective-banner');
    var s = UI.sampleClip(d.animations[0], 0);
    var at0 = UI.applySample(d, s);
    near('the banner starts fully transparent',
        UI.findByName(at0.widgets, 'banner').bgAlpha, 0);
    var at20 = UI.applySample(d, UI.sampleClip(d.animations[0], 20));
    near('the banner ends at its authored alpha',
        UI.findByName(at20.widgets, 'banner').bgAlpha, UI.ALPHA.band);
    eq('applying a sample does not mutate the design',
        UI.findByName(d.widgets, 'banner').bgAlpha, UI.ALPHA.band);
})();

/* ===================================================================== *
 * 6. Templates, names and warnings
 * ===================================================================== */
(function templates() {
    UI.templateNames().forEach(function (name) {
        var d = UI.template(name);
        ok('template ' + name + ' has widgets', d.widgets.length > 0);
        var names = {};
        var dupe = null;
        UI.walk(d.widgets, function (n) {
            if (names[n.name]) dupe = n.name;
            names[n.name] = 1;
        });
        ok('template ' + name + ' has unique widget names', dupe === null, 'duplicate ' + dupe);
        ok('template ' + name + ' exports without throwing', (function () {
            try { UI.exportAll(d); return true; } catch (e) { return e.message; }
        })() === true);
        var clipped = UI.audit(d.widgets, 16 / 9).filter(function (f) { return f.kind === 'clip'; });
        ok('template ' + name + ' fits the 16:9 safe area', clipped.length === 0,
            JSON.stringify(clipped));
    });

    // ASCII only: the font renders anything else as boxes, so it is a warning
    // and not a silent pass.
    var d = UI.newDesign('warn');
    d.widgets = [UI.makeNode('Text', { name: 'row' })];
    d.strings.row = 'café';
    var w = UI.exportParseUi(d).warnings;
    ok('a non-ASCII string is warned about', w.length === 1 && w[0].indexOf('ASCII') > 0,
        JSON.stringify(w));

    d.strings.row = '{} {} {} {}';
    ok('more than three {} slots is warned about',
        UI.exportParseUi(d).warnings.some(function (x) { return x.indexOf('three') > 0; }));

    eq('a template with three {} slots counts three', UI.countSlots('{} of {} on {}'), 3);
    eq('a template with no slots counts none', UI.countSlots('READY'), 0);

    // Names have to survive FindUIWidgetWithName.
    var nodes = [UI.makeNode('Container', { name: 'panel' })];
    eq('a free name comes back unchanged', UI.uniqueName(nodes, 'header'), 'header');
    eq('a taken name gets a number', UI.uniqueName(nodes, 'panel'), 'panel1');
})();

/* ===================================================================== *
 * 6b. The palette and the style-guide templates
 * ===================================================================== */
(function palette() {
    var byName = {};
    UI.PALETTE.forEach(function (p) { byName[p.name] = p; });
    ['White', 'Cyan', 'Red', 'Green', 'Yellow', 'Ice', 'Dark cyan', 'Dark red',
     'Dark green', 'Dark yellow', 'Steel', 'Slate', 'Ink'].forEach(function (n) {
        ok('the palette carries ' + n, !!byName[n]);
    });
    eq('cyan is the 0..1 form of 70EBFF', byName.Cyan.rgb, [0.439, 0.922, 1.000]);
    eq('ink is the 0..1 form of 080B0B', byName.Ink.rgb, [0.031, 0.043, 0.043]);
    eq('every swatch is a triple on 0..1', UI.PALETTE.filter(function (p) {
        return p.rgb.length !== 3 || p.rgb.some(function (v) { return v < 0 || v > 1; });
    }).length, 0);
    near('panels run at 75 percent', UI.ALPHA.panel, 0.75);
    near('accent bands run at 75 percent', UI.ALPHA.band, 0.75);

    // The palette the tool ships must be the palette the library holds.
    var fs = require('fs');
    var shipped = JSON.parse(fs.readFileSync(
        path.join(__dirname, '..', 'palette.json'), 'utf8'));
    eq('palette.json and the library agree on the swatch names',
        shipped.swatches.map(function (s) { return s.name; }),
        UI.PALETTE.map(function (s) { return s.name; }));
    ok('setPalette takes the shipped file', UI.setPalette(shipped) === true);

    // The two style-guide examples.
    var grid = UI.template('options-grid');
    var tiles = 0, icons = 0;
    UI.walk(grid.widgets, function (n) {
        if (n.type === 'Button') tiles++;
        if (n.type === 'Image') icons++;
    });
    eq('the options grid has five families in four columns', tiles, 20);
    eq('every tile carries an icon', icons, tiles);
    ok('a SELECTED tile uses the bright fill and a dark icon', (function () {
        var t = UI.findByName(grid.widgets, 'tileCyanSel1');
        var i = UI.findByName(grid.widgets, 'tileCyanSel1Icon');
        return JSON.stringify(t.buttonColorBase) === JSON.stringify([0.439, 0.922, 1.000]) &&
               JSON.stringify(i.imageColor) === JSON.stringify([0.031, 0.043, 0.043]);
    })());
    ok('an OPTION tile uses the dark fill and a bright icon', (function () {
        var t = UI.findByName(grid.widgets, 'tileCyanOpt0');
        var i = UI.findByName(grid.widgets, 'tileCyanOpt0Icon');
        return JSON.stringify(t.buttonColorBase) === JSON.stringify([0.075, 0.184, 0.247]) &&
               JSON.stringify(i.imageColor) === JSON.stringify([0.439, 0.922, 1.000]);
    })());

    var banner = UI.template('round-banner');
    eq('the round banner headline is the guide text',
        banner.strings.roundHead, 'ROUND WON!');
    eq('the round banner has a subtitle', banner.strings.roundSub, 'NEXT ROUND');
    eq('the round banner timer is a two-slot template', UI.countSlots(banner.strings.roundTimer), 2);
    near('the round band runs at the band alpha',
        UI.findByName(banner.widgets, 'roundBand').bgAlpha, UI.ALPHA.band);
    ok('the round banner blurs behind every line of text', (function () {
        var all = true;
        UI.walk(banner.widgets, function (n) { if (n.type === 'Text' && !n.textBgBlur) all = false; });
        return all;
    })());
    ok('the round banner ships an entrance animation',
        banner.animations.length === 1 && banner.animations[0].name === 'roundIn');
})();

/* ===================================================================== *
 * 7. Runtime bindings
 * ===================================================================== */
(function bindings() {
    var d = UI.template('button-menu');
    var out = UI.exportParseUi(d);
    ok('the export ships a getUi accessor', out.typescript.indexOf('export function getUi()') > 0);
    ok('every named widget is in the accessor', UI.allNames(d.widgets).every(function (n) {
        return out.typescript.indexOf('mod.FindUIWidgetWithName("' + n + '")') > 0;
    }));
    ok('buttons get an event router', out.typescript.indexOf('export function onUiButton') > 0);
    ok('the router filters on ButtonUp', out.typescript.indexOf('mod.UIButtonEvent.ButtonUp') > 0);

    var plain = UI.exportParseUi(UI.template('frosted-panel'));
    ok('a design with no buttons ships no router',
        plain.typescript.indexOf('export function onUiButton') < 0);
})();

/* ===================================================================== *
 * 7b. The exports against the API they claim to target
 *
 * Every one of the thirty template/format combinations failed to typecheck
 * against the installed Portal template once, and the unit tests here did not
 * notice, because none of them asked what the generated code named. These do.
 * Each check below stands for one thing that was wrong and one line in
 * bf6-portal-mod-types or bf6-portal-utils that says so. A full typecheck
 * against a real template is still the only complete answer, but nothing on
 * this list should ever come back.
 * ===================================================================== */
(function apiShape() {
    ['deluca', 'solid'].forEach(function (mode) {
        var out = UI.exportDeluca(UI.template('tab-menu'), { mode: mode }).typescript;
        // mod.Vector is opaque, so a bare [r, g, b] is not assignable to it.
        ok('the ' + mode + ' export builds colours with mod.CreateVector',
            out.indexOf('bgColor: mod.CreateVector(') > 0);
        ok('the ' + mode + ' export writes no bare colour array', !/bgColor: \[/.test(out));
        // UI.makeName decides the widget name, so passing one does nothing and
        // the params type has no field for it.
        ok('the ' + mode + ' export passes no name', !/^\s*name: /m.test(out));
        // Only UIText.Params carries padding.
        ok('the ' + mode + ' export only sets padding on text',
            (out.match(/padding: /g) || []).length === (out.match(/message: /g) || []).length);
        // UIButton.Params has base, disabled, pressed and focused. No hover.
        ok('the ' + mode + ' export sets no hover colour', out.indexOf('hoverColor') < 0);
        // The module forwards depth itself, and the lookup this helper used
        // could never have found a module-named widget anyway.
        ok('the ' + mode + ' export ships no ensureLayers helper', out.indexOf('ensureLayers') < 0);
        // A plain UIButton is a leaf; a button with children needs the wrapper.
        ok('the ' + mode + ' export nests button labels in a UIContainerButton',
            out.indexOf('UIContainerButton') > 0);
        ok('the ' + mode + ' export imports every class it names',
            (out.match(/\bUI(?:Container|Text|Image|Button|ContainerButton)\b/g) || []).every(function (c) {
                return out.indexOf("import { " + c + " } from 'bf6-portal-utils/ui/components/") > 0;
            }));
    });

    var solid = UI.exportDeluca(UI.template('tab-menu'), { mode: 'solid' }).typescript;
    // UI is a namespace of types and helpers; the widget classes are not on it.
    ok('the solid export does not look for classes on UI', solid.indexOf('SolidUI.h(UI.') < 0);
    // UIContainerButton is not a UI.Parent. Its inner container is.
    ok('the solid export parents into the inner container',
        solid.indexOf('.innerContainer,') > 0);

    var classes = UI.exportDeluca(UI.template('tab-menu'), { mode: 'classes' }).typescript;
    ok('the classes export nests through childrenParams', classes.indexOf('childrenParams: [') > 0);

    // Padding and hover are dropped rather than mistranslated, so they have to
    // be reported the way the community format reports what it cannot carry.
    var warned = UI.exportDeluca(UI.template('tab-menu'), { mode: 'classes' }).warnings;
    ok('the UI module export reports the padding it cannot carry',
        warned.some(function (x) { return x.indexOf('padding') > 0; }), warned.join(' | '));
    ok('the UI module export reports the hover colour it cannot carry',
        warned.some(function (x) { return x.indexOf('hover') > 0; }), warned.join(' | '));
    ok('exportAll surfaces those warnings too',
        UI.exportAll(UI.template('tab-menu')).warnings.some(function (x) {
            return x.indexOf('The UI module') === 0;
        }));

    // A file written against the old, nonexistent modlib package still reads.
    var legacy = [
        'import * as modlib from "modlib";',
        'export const root = modlib.ParseUI({',
        '  type: "Container", name: "legacyRoot",',
        '  position: [8, 8], size: [200, 100], anchor: mod.UIAnchor.TopLeft,',
        '  bgFill: mod.UIBgFill.Solid, bgColor: [0, 0, 0], bgAlpha: 1,',
        '  depth: mod.UIDepth.AboveGameUI',
        '});'
    ].join('\n');
    eq('a modlib-era export still imports', UI.importParseUi(legacy, null, 'old').widgets[0].name,
        'legacyRoot');

    // The builder the export now writes out declares `function ParseUI(spec:
    // UiSpec)`. Reading that declaration as a widget tree is the obvious way
    // to break the importer, so the round trip has to survive it.
    var fresh = UI.exportParseUi(UI.template('counter'));
    eq('the builder declaration is not mistaken for a tree',
        UI.importParseUi(fresh.typescript, fresh.stringsJson, 'x').widgets.length,
        UI.template('counter').widgets.length);
})();

/* ===================================================================== *
 * 8. Layering against the game's own HUD
 *
 * UIDepth is a per-widget property in the API, so the builder models it as
 * one, with inheritance resolved before anything is exported. The thing worth
 * testing is that the word Inherit never leaves the tool: an export that said
 * mod.UIDepth.Inherit would not compile, and one that quietly dropped the
 * layer would draw in the wrong place with nothing to show for it.
 * ===================================================================== */
(function layering() {
    var d = UI.newDesign('layers');
    eq('a new design defaults to over the game HUD', d.defaultDepth, 'AboveGameUI');

    var root = UI.makeNode('Container', { name: 'root' });
    var mid = UI.makeNode('Container', { name: 'mid' });
    var leaf = UI.makeNode('Text', { name: 'leaf' });
    mid.children = [leaf];
    root.children = [mid];
    d.widgets = [root];

    eq('a new widget inherits by default', root.depth, 'Inherit');

    var r = UI.resolveDepths(d);
    eq('an inheriting root takes the design default', r[root.id], 'AboveGameUI');
    eq('an inheriting child takes its parent', r[leaf.id], 'AboveGameUI');

    d.defaultDepth = 'BelowGameUI';
    r = UI.resolveDepths(d);
    eq('changing the design default moves the whole tree', r[leaf.id], 'BelowGameUI');

    mid.depth = 'AboveGameUI';
    r = UI.resolveDepths(d);
    eq('an explicit widget wins over the design default', r[mid.id], 'AboveGameUI');
    eq('its children follow it, not the root', r[leaf.id], 'AboveGameUI');
    eq('its parent is untouched', r[root.id], 'BelowGameUI');

    // The bulk action.
    d.defaultDepth = 'AboveGameUI';
    var n = UI.setDepth(d, [root.id], 'BelowGameUI', true);
    ok('setting a subtree changes the widget and clears its descendants', n >= 2, 'changed ' + n);
    r = UI.resolveDepths(d);
    eq('the whole subtree is now under the HUD', r[leaf.id], 'BelowGameUI');
    eq('setDepth rejects a value the engine does not have',
        UI.setDepth(d, [root.id], 'Sideways', false), 0);

    // Nothing named Inherit may reach an export.
    var mixed = UI.template('scoreboard');
    mixed.defaultDepth = 'BelowGameUI';
    var out = UI.exportAll(mixed);
    ['typescript', 'deluca', 'solid', 'blocks'].forEach(function (f) {
        ok('the ' + f + ' export never writes Inherit', out[f].indexOf('Inherit') < 0);
        ok('the ' + f + ' export names the resolved layer', out[f].indexOf('BelowGameUI') > 0);
    });

    // Every widget in the ParseUI export carries a depth of its own.
    var names = UI.allNames(mixed.widgets);
    var depthLines = out.typescript.match(/depth: mod\.UIDepth\.[A-Za-z]+/g) || [];
    eq('one depth line per widget', depthLines.length, names.length);

    // The block call carries it as the UIDepth item the engine expects.
    var snip = JSON.parse(out.blocks);
    var text = JSON.stringify(snip);
    ok('the block tree uses UIDepthItem', text.indexOf('"UIDepthItem"') > 0);
    ok('the block tree names the enum in VALUE-0',
        text.indexOf('"VALUE-0":"UIDepth"') > 0 || text.indexOf('"VALUE-0": "UIDepth"') > 0);

    // The community format has nowhere to put it, and says so.
    var comm = UI.toCommunityParams(mixed);
    ok('the community export reports the layer it had to drop',
        comm.dropped.some(function (x) { return x.indexOf('.depth') > 0; }), JSON.stringify(comm.dropped));
    // Over the HUD is their implicit behaviour, so nothing about the LAYER is
    // lost. Their format still has no home for textBgBlur or {} arguments, and
    // those are reported separately.
    var above = UI.template('scoreboard');
    eq('an over-the-HUD design loses no layer on the way to the community format',
        UI.toCommunityParams(above).dropped.filter(function (x) {
            return x.indexOf('.depth') > 0;
        }).length, 0);
    ok('what it does lose is named',
        UI.toCommunityParams(above).dropped.some(function (x) { return x.indexOf('.textBgBlur') > 0; }));

    // A ParseUI tree read back in keeps its explicit layers.
    var back = UI.importParseUi(out.typescript, out.strings, 'x');
    eq('an imported tree keeps the layer it was written with',
        UI.resolveDepths(back)[back.widgets[0].id], 'BelowGameUI');
})();

/* ===================================================================== *
 * 9. The game HUD mock and the HUD half of the check
 * ===================================================================== */
(function hud() {
    var regions = UI.hudLayout(16 / 9);
    eq('the mock has seven regions', regions.length, 7);
    ['minimap', 'objective', 'compass', 'killfeed', 'squad', 'health', 'ammo'].forEach(function (id) {
        ok('the mock has the ' + id, regions.some(function (r) { return r.id === id; }));
    });
    var screen = UI.screenRect(16 / 9);
    regions.forEach(function (r) {
        ok('the ' + r.id + ' sits inside the safe area at 16:9',
            r.rect.left >= screen.left - 0.5 && r.rect.right <= screen.right + 0.5 &&
            r.rect.top >= 0 && r.rect.bottom <= 1080);
    });

    // The HUD is edge-anchored, so it spreads out on a wider screen the same
    // way a real one does.
    var wide = UI.hudLayout(32 / 9);
    function byId(list, id) {
        for (var i = 0; i < list.length; i++) if (list[i].id === id) return list[i];
        return null;
    }
    ok('the minimap follows the left edge on ultrawide',
        byId(wide, 'minimap').rect.left < byId(regions, 'minimap').rect.left);
    ok('the ammo readout follows the right edge on ultrawide',
        byId(wide, 'ammo').rect.right > byId(regions, 'ammo').rect.right);
    near('the compass stays centred', byId(wide, 'compass').rect.centerX,
        byId(regions, 'compass').rect.centerX);

    // A widget over the minimap is reported, and what the report SAYS depends
    // on which way it is layered.
    var d = UI.newDesign('clash');
    var panel = UI.makeNode('Container', { name: 'panel', position: [24, 24], size: [300, 200],
        anchor: 'TopLeft', bgFill: 'Solid' });
    d.widgets = [panel];

    var over = UI.audit(d, 16 / 9).filter(function (f) { return f.kind === 'hud'; });
    ok('an over-the-HUD widget on the minimap is reported', over.length >= 1, JSON.stringify(over));
    eq('it names the region', over[0].regionLabel, 'Minimap');
    ok('it says the widget covers the HUD', over[0].covers === true);
    ok('the wording is about covering', over[0].text.indexOf('covers') > 0);

    d.defaultDepth = 'BelowGameUI';
    var under = UI.audit(d, 16 / 9).filter(function (f) { return f.kind === 'hud'; });
    ok('an under-the-HUD widget on the minimap is reported', under.length >= 1);
    ok('it says the HUD covers the widget', under[0].covers === false);
    ok('the wording is about being covered', under[0].text.indexOf('under the') > 0);

    // HUD findings are their own kind, not mixed in with widget overlaps.
    ok('a HUD clash is not counted as a widget overlap',
        UI.audit(d, 16 / 9).filter(function (f) { return f.kind === 'overlap'; }).length === 0);

    // Somewhere the HUD is not, is clean.
    var clear = UI.newDesign('clear');
    clear.widgets = [UI.makeNode('Container', { name: 'mid', position: [0, 0], size: [400, 200],
        anchor: 'Center', bgFill: 'Solid' })];
    eq('a widget in the middle of the screen clashes with nothing',
        UI.audit(clear, 16 / 9).filter(function (f) { return f.kind === 'hud'; }).length, 0);

    // A layout box with no fill and no children paints nothing, so it cannot
    // cover the HUD and is not reported.
    var ghost = UI.newDesign('ghost');
    ghost.widgets = [UI.makeNode('Container', { name: 'ghost', position: [24, 24], size: [300, 200],
        anchor: 'TopLeft', bgFill: 'None' })];
    eq('an empty layout box is not reported against the HUD',
        UI.audit(ghost, 16 / 9).filter(function (f) { return f.kind === 'hud'; }).length, 0);

    // auditAll still takes a plain widget array, the way the older callers do.
    ok('auditAll accepts a widget array', Array.isArray(UI.auditAll(clear.widgets)));
    ok('auditAll accepts a design', Array.isArray(UI.auditAll(clear)));
})();

/* ===================================================================== */
console.log('');
console.log('BF6 UI BUILDER tests: ' + pass + ' passed, ' + fail + ' failed.');
if (fail) {
    console.log('');
    failures.forEach(function (f) { console.log('  FAILED  ' + f); });
    process.exit(1);
}
process.exit(0);
