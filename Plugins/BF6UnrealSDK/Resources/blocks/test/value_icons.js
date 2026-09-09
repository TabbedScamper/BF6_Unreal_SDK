// Empty sockets must display every accepted type, using Portal's type mapping.
// Node checks the emitted SVG and serialized block; browser evidence also lives
// with the September 9 repair artifacts.
const assert = require('assert');
const path = require('path');
const ROOT = path.resolve(__dirname, '..');

function run(Blockly, BF6) {
  let checks = 0;
  const failures = [];
  const check = (fn, label) => { checks++; try { fn(); } catch (e) { failures.push(label + ': ' + e.message); } };
  BF6.setStyle(require(path.join(ROOT, 'site_style_default.json')));
  const mappings = { Team: 'teamid', Squad: 'squadid', PrefabSpawner: 'lootspawner', Variable: 'variables',
    Enum_Weapons: 'itemindex', Player: 'player', String: 'string', Number: 'number', 'Any Type': 'any', UnknownFutureType: 'null' };
  for (const [type, icon] of Object.entries(mappings)) {
    check(() => assert.strictEqual(BF6.typeIconName(type), 'type-' + icon), type + ' icon');
  }
  function node(tag) {
    return { tag, attrs: {}, style: {}, childNodes: [], parentNode: null, events: {},
      setAttribute(k, v) { this.attrs[k] = v; }, setAttributeNS(ns, k, v) { this.attrs[k] = v; },
      appendChild(n) { this.childNodes.push(n); n.parentNode = this; return n; },
      removeChild(n) { this.childNodes.splice(this.childNodes.indexOf(n), 1); n.parentNode = null; },
      addEventListener(k, fn) { this.events[k] = fn; },
      querySelectorAll() { return this.childNodes.filter(n => n.tag === 'g' && n.attrs.class === 'blocklyValueIcon'); }
    };
  }
  const oldDocument = global.document;
  const oldClick = BF6.onSocketClick;
  const ws = new Blockly.Workspace();
  try {
    global.document = { createElementNS: (ns, tag) => node(tag) };
    const msg = ws.newBlock('Message');
    const root = node('svg');
    msg.getSvgRoot = () => root;
    msg.inputList.forEach(i => { if (i.connection) i.connection.getOffsetInBlock = () => ({ x: 0, y: 0 }); });
    const before = JSON.stringify(Blockly.serialization.blocks.save(msg));
    BF6.paintValueIcons(msg);
    const groups = root.querySelectorAll();
    check(() => assert.strictEqual(groups.length, 4), 'four empty Message sockets');
    for (const g of groups) {
      check(() => assert.deepStrictEqual(g.childNodes.map(i => i.attrs['data-bf6-type']), ['String', 'Number', 'Player']), 'ordered union icons');
      check(() => assert.deepStrictEqual(g.childNodes.map(i => i.attrs.href), ['String', 'Number', 'Player'].map(BF6.typeIconUrl)), 'exact type artwork');
    }
    check(() => assert.strictEqual(JSON.stringify(Blockly.serialization.blocks.save(msg)), before), 'drawing preserves serialized mode');
    let clicked = 0;
    BF6.onSocketClick = (b, input) => { clicked++; assert.strictEqual(b, msg); assert.ok(input.connection); };
    groups[0].events.mousedown({ stopPropagation() {} });
    check(() => assert.strictEqual(clicked, 1), 'socket help remains clickable');
    BF6.paintValueIcons(msg);
    check(() => assert.strictEqual(root.querySelectorAll().length, 4), 'redraw removes previous icons');
    const player = ws.newBlock('EventPlayer');
    msg.getInput('VALUE-1').connection.connect(player.outputConnection);
    BF6.paintValueIcons(msg);
    check(() => assert.strictEqual(root.querySelectorAll().length, 3), 'connected socket has no empty icons');
    player.outputConnection.disconnect();
    BF6.paintValueIcons(msg);
    check(() => assert.strictEqual(root.querySelectorAll().length, 4), 'disconnect restores all accepted types');
  } finally {
    ws.dispose(); BF6.onSocketClick = oldClick;
    if (oldDocument === undefined) delete global.document; else global.document = oldDocument;
  }
  console.log('value icon checks ', checks, 'run,', failures.length, 'failed');
  failures.forEach(f => console.log(' FAIL', f));
  return { ok: failures.length === 0, checks, failures };
}
module.exports = { run };
if (require.main === module) {
  const Blockly = require('./style.js').loadBlockly();
  const BF6 = require(path.join(ROOT, 'editor.js'));
  BF6.attach(Blockly);
  BF6.installFallback(require(path.join(ROOT, 'vendor/types_fallback.js')), BF6.observe({ blocks: { blocks: [] } }));
  BF6.buildSignatures(null);
  process.exit(run(Blockly, BF6).ok ? 0 : 1);
}
