'use strict';
// Execute both exported backends against the same Portal-shaped widget store.
// Node 24's type stripper keeps the test independent of an installed TS package.
const assert = require('node:assert/strict');
const vm = require('node:vm');
const { stripTypeScriptTypes } = require('node:module');
const UI = require('../uibuilder/lib/bf6ui.js');
const design = UI.newDesign('equipment');
const properties = { padding: 13, bgColor: [0.1, 0.4, 0.7], bgAlpha: 0.35,
    bgFill: 'Solid', visible: false, depth: 'BelowGameUI' };
design.widgets = [UI.makeNode('Container', { name: 'frame', children: [
    UI.makeNode('WeaponImage', { ...properties, name: 'weapon', weapon: 'Carbine_M4A1',
        attachments: ['Scope_A_P2_175x'] }),
    UI.makeNode('GadgetImage', { ...properties, name: 'gadget', gadget: 'C4' })
]}), UI.makeNode('WeaponImage', { ...properties, name: 'rootWeapon' }),
UI.makeNode('GadgetImage', { ...properties, name: 'rootGadget', gadget: 'C4' })];
function runtime() {
    const widgets = new Map();
    const mod = {};
    for (const kind of ['UIAnchor', 'UIBgFill', 'UIDepth', 'Weapons', 'Gadgets', 'WeaponAttachments'])
        mod[kind] = new Proxy({}, { get: (_, key) => kind + '.' + key });
    mod.GetUIRoot = () => 'UIRoot';
    mod.CreateVector = (...args) => args;
    mod.CreateNewWeaponPackage = () => [];
    mod.AddAttachmentToWeaponPackage = (attachment, pkg) => pkg.push(attachment);
    mod.FindUIWidgetWithName = name => {
        assert(widgets.has(name), 'widget exists before its properties are applied: ' + name);
        return widgets.get(name);
    };
    mod.AddUIContainer = name => widgets.set(name, { name });
    for (const kind of ['Weapon', 'Gadget']) mod['AddUI' + kind + 'Image'] =
        (name, position, size, anchor, item, parent, pkg) => {
            assert(!widgets.has(name), 'no duplicate creation');
            widgets.set(name, { name, position, size, anchor, item, parent: parent.name || parent, pkg });
        };
    for (const prop of ['Padding', 'BgColor', 'BgAlpha', 'BgFill', 'Depth', 'Visible'])
        mod['SetUIWidget' + prop] = (widget, value) => { widget[prop] = value; };
    return { mod, widgets };
}
const tsRun = runtime();
const source = UI.exportParseUi(design).typescript;
vm.runInNewContext(stripTypeScriptTypes(source).replace(/^export /gm, ''),
    { mod: tsRun.mod, console });
const blockRun = runtime();
const vars = new Map();
function evaluate(block) {
    const fields = block.fields || {};
    if (block.type === 'Text') return fields.TEXT;
    if (block.type === 'Number') return fields.NUM;
    if (block.type === 'Boolean') return fields.BOOL === true || fields.BOOL === 'TRUE';
    if (block.type === 'variableReferenceBlock') return fields.VAR;
    if (block.type.endsWith('Item')) return blockRun.mod[fields['VALUE-0']][fields['VALUE-1']];
    const args = Object.keys(block.inputs || {}).sort((a, b) => Number(a.slice(6)) - Number(b.slice(6)))
        .map(key => evaluate(block.inputs[key].block));
    if (block.type === 'SetVariable') { vars.set(args[0], args[1]); return; }
    if (block.type === 'GetVariable') return vars.get(args[0]);
    assert.equal(typeof blockRun.mod[block.type], 'function', 'supported Portal call: ' + block.type);
    return blockRun.mod[block.type](...args);
}
for (const rule of UI.exportBlocks(design).blocks.blocks) {
    let block = rule.inputs.ACTIONS.block;
    while (block) { evaluate(block); block = block.next && block.next.block; }
}
for (const run of [tsRun, blockRun]) {
    for (const name of ['weapon', 'gadget', 'rootWeapon', 'rootGadget']) {
        const widget = run.widgets.get(name);
        assert.equal(widget.Padding, 13);
        assert.deepEqual(Array.from(widget.BgColor), properties.bgColor);
        assert.equal(widget.BgAlpha, 0.35);
        assert.equal(widget.BgFill, 'UIBgFill.Solid');
        assert.equal(widget.Depth, 'UIDepth.BelowGameUI');
        assert.equal(widget.Visible, false);
    }
    assert.deepEqual(Array.from(run.widgets.get('weapon').pkg), ['WeaponAttachments.Scope_A_P2_175x']);
    assert.equal(run.widgets.get('gadget').item, 'Gadgets.C4');
    assert.equal(run.widgets.get('weapon').parent, 'frame');
    assert.equal(run.widgets.get('rootWeapon').parent, 'UIRoot');
    assert.equal(run.widgets.get('gadget').pkg, undefined);
}
console.log('Equipment image runtime: TypeScript and blocks preserve widget properties and packages.');
