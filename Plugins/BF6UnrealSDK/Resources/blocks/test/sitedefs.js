// Proof that the SITE's own definitions are what gets drawn.
//
// The capture builds one real block of every type on the site and writes down
// what it is made of. That reading is not Blockly JSON, so for as long as it
// was handed straight to jsonInit every one of the 604 types threw and fell
// back to the mined approximation - silently, because the fallback works. The
// blocks were the right blocks in the right places and all of them drawn
// wrong, which is exactly what "it does not match the site at all" was.
//
// What this holds down, each one visible in the side-by-side screenshots:
//   1. a value block wears the round type symbol at its head (ICON-0).
//   2. ForVariable says From, To and By BETWEEN its sockets.
//   3. Number, Text and Boolean show their value and NOT their type name.
//   4. Text is quoted, Number is not.
//   5. Boolean reads true/false and writes TRUE/FALSE.
//   6. no socket is left saying VALUE-0.
//   7. the mod catalogue is never mistaken for a block type.
//
// It reads the capture this machine actually has. Without one it says so and
// passes: a fresh clone has never read the site, and that is not a failure.
//
// Run by roundtrip.js. Also runs alone:  node sitedefs.js

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');
const { loadBlockly } = require(path.join(HERE, 'style.js'));

// Saved/BF6UnrealSDK/portal/blockly, up out of Plugins/BF6UnrealSDK/Resources.
const CACHE = path.join(BLOCKS, '..', '..', '..', '..',
  'Saved', 'BF6UnrealSDK', 'portal', 'blockly');

function readJson(p) {
  try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch (e) { return null; }
}

// The site's own art, read off the mirror the way the plugin packs it: every
// icon by its name, and the three block pictures by their file name. Without
// it there is nothing for a picture to resolve to and the conversion correctly
// refuses to draw one, so the test would be checking the wrong thing.
function mirrorArt() {
  const project = path.resolve(BLOCKS, '..', '..', '..', '..');
  const bf6 = path.join(project, 'Saved', 'BF6UnrealSDK', 'portalstyle',
    'portal.battlefield.com', 'bf6');
  if (!fs.existsSync(bf6)) return null;
  const builds = fs.readdirSync(bf6)
    .filter(d => fs.existsSync(path.join(bf6, d, 'assets', 'blockly', 'icons')))
    .sort();
  if (!builds.length) return null;
  const dir = path.join(bf6, builds[builds.length - 1], 'assets', 'blockly');

  const categoryIcons = {};
  fs.readdirSync(path.join(dir, 'icons')).filter(f => f.endsWith('.svg')).forEach(f => {
    const svg = fs.readFileSync(path.join(dir, 'icons', f), 'utf8');
    categoryIcons[f.slice(0, -4)] = { dataUrl: 'data:image/svg+xml,' + encodeURIComponent(svg) };
  });

  const blockImages = {};
  ['1x1.png', 'quote0.png', 'quote1.png'].forEach(f => {
    const p = path.join(dir, f);
    if (!fs.existsSync(p)) return;
    blockImages[f] = 'data:image/png;base64,' + fs.readFileSync(p).toString('base64');
  });

  return { build: builds[builds.length - 1], categoryIcons, blockImages };
}

// FieldImage keeps its address as its value. getSrc is the site's own bundle,
// not Blockly's, so asking for it here reads back empty and proves nothing.
function srcOf(f) {
  try { if (typeof f.getValue === 'function') return String(f.getValue() || ''); } catch (e) {}
  try { if (typeof f.getSrc === 'function') return String(f.getSrc() || ''); } catch (e) {}
  return '';
}

// Every field on a block, in the order it is drawn, flattened out of the rows.
function fieldsOf(b) {
  const out = [];
  (b.inputList || []).forEach(inp => {
    (inp.fieldRow || []).forEach(f => out.push({ input: inp.name, field: f }));
  });
  return out;
}

function textOf(b) {
  return fieldsOf(b)
    .map(x => {
      const f = x.field;
      if (typeof f.getText === 'function') { try { return f.getText(); } catch (e) { return ''; } }
      return '';
    })
    .filter(s => s !== '')
    .join(' ');
}

function imagesOf(b, Blockly) {
  return fieldsOf(b).filter(x => x.field instanceof Blockly.FieldImage);
}

function run(Blockly, BF6) {
  let checks = 0;
  const fails = [];
  const notes = [];
  function check(cond, what) { checks++; if (!cond) fails.push(what); }

  const synth = readJson(path.join(CACHE, 'definitions_synth.json'));
  const cat = readJson(path.join(CACHE, 'definitions.json'));

  if (!synth) {
    console.log('sitedefs checks    0 run, 0 failed');
    console.log('  no capture on this machine, so there is nothing to check.',
      'BF6.Blocks.CaptureDefs reads the site.');
    return { ok: true, checks: 0, failures: [] };
  }

  // ---- 7: the catalogue is not a block ------------------------------------
  //
  // definitions.json holds eight arrays - objects, events, values, actions and
  // the rest - describing every function the game exposes. It used to be
  // merged in as though each of those eight were a block type, which is where
  // "632 types" came from when the truth is 604.
  if (cat) {
    Object.keys(cat).forEach(k => {
      const v = cat[k];
      check(!BF6.isProbeRecord(v), 'the catalogue section ' + k + ' was taken for a block');
    });
    notes.push('  catalogue: ' + Object.keys(cat).join(', ') +
      ', none of them mistaken for a block');
  }

  // The style has to be in place first: the conversion resolves every picture
  // through the mirror, and with no mirror there is no picture to resolve to.
  const raw = JSON.parse(fs.readFileSync(path.join(BLOCKS, 'site_style_default.json'), 'utf8'));
  raw.op = 'style';
  raw.source = 'default';
  const art = mirrorArt();
  if (art) {
    raw.categoryIcons = Object.assign({}, raw.categoryIcons || {}, art.categoryIcons);
    raw.blockImages = Object.assign({}, raw.blockImages || {}, art.blockImages);
    notes.push('  mirror build ' + art.build + ': ' + Object.keys(art.categoryIcons).length +
      ' symbols and ' + Object.keys(art.blockImages).length + ' block pictures');
  } else {
    notes.push('  no site mirror on this machine, so the picture checks are skipped');
  }
  BF6.setStyle(raw);

  const installed = BF6.installDefinitions(synth, 'cache');
  check(installed > 500, 'only ' + installed + ' of the site definitions installed');
  notes.push('  ' + installed + ' site definitions installed, converted from the reading');

  const ws = new Blockly.Workspace();

  // ---- 1: the round type symbol at the head of a value block --------------
  const BADGED = ['AbsoluteValue', 'CountOf', 'CreateVector', 'GetVariable'];
  let badged = 0;
  BADGED.forEach(type => {
    if (!Blockly.Blocks[type]) return;
    let b = null;
    try { b = ws.newBlock(type); } catch (e) { fails.push(type + ' would not build: ' + e.message); checks++; return; }
    const head = imagesOf(b, Blockly).filter(x => /^ICON/.test(x.field.name || ''));
    check(head.length >= 1, type + ' has no type symbol at its head');
    if (head.length) {
      const src = srcOf(head[0].field);
      check(!!src && !/^\/bf6\//.test(src),
        type + ' draws its symbol from the site address ' + src + ' rather than our copy');
      badged++;
    }
    // and the block still says its own name
    check(textOf(b).indexOf(type) >= 0, type + ' does not say its own name');
    try { b.dispose(false); } catch (e) {}
  });
  notes.push('  ' + badged + ' of ' + BADGED.length +
    ' sampled value blocks carry the round type symbol, drawn from our copy');

  // ---- 2: the words between the sockets -----------------------------------
  if (Blockly.Blocks.ForVariable) {
    const b = ws.newBlock('ForVariable');
    const words = fieldsOf(b)
      .filter(x => x.field instanceof Blockly.FieldLabel)
      .map(x => x.field.getText());
    ['From', 'To', 'By'].forEach(w => {
      check(words.indexOf(w) >= 0, 'ForVariable does not say ' + w);
    });
    // and in that order, after its name
    const order = words.filter(w => ['ForVariable', 'From', 'To', 'By'].indexOf(w) >= 0);
    check(order.join(' ') === 'ForVariable From To By',
      'ForVariable reads "' + order.join(' ') + '"');
    const slots = (b.inputList || []).filter(i => i.connection).map(i => i.name);
    check(slots.length >= 5, 'ForVariable has ' + slots.length + ' sockets');
    notes.push('  ForVariable reads "' + order.join(' ') + '" across ' + slots.length + ' sockets');
    try { b.dispose(false); } catch (e) {}
  }

  // ---- 3 and 4: the literals show a value, not a type ---------------------
  const LITERAL = { Number: '0', Text: '', Boolean: 'false' };
  Object.keys(LITERAL).forEach(type => {
    if (!Blockly.Blocks[type]) return;
    const b = ws.newBlock(type);
    const t = textOf(b);
    check(t.indexOf(type) < 0, type + ' is labelled with its own type: "' + t + '"');
    const quotes = imagesOf(b, Blockly).filter(x => /^VARICON/.test(x.field.name || ''));
    if (type === 'Text') {
      check(quotes.length === 2, 'Text has ' + quotes.length + ' quote marks, not 2');
    } else if (type === 'Number') {
      // The site draws a 1x1 spacer here, not a quote mark. Whichever way it
      // is resolved, what must never happen is Number appearing in quotes.
      quotes.forEach(q => {
        const src = srcOf(q.field);
        check(src.indexOf('quote') < 0, 'Number is drawn in quote marks');
      });
    }
    try { b.dispose(false); } catch (e) {}
  });

  if (Blockly.Blocks.Boolean) {
    const b = ws.newBlock('Boolean');
    const dd = fieldsOf(b).map(x => x.field)
      .filter(f => f instanceof Blockly.FieldDropdown)[0];
    check(!!dd, 'Boolean has no dropdown');
    if (dd) {
      const opts = dd.getOptions(false);
      const shown = opts.map(o => o[0]).join(',');
      const written = opts.map(o => o[1]).join(',');
      check(shown === 'true,false', 'Boolean shows ' + shown);
      check(written === 'TRUE,FALSE', 'Boolean writes ' + written);
      notes.push('  Boolean reads [' + shown + '] and writes [' + written + ']');
    }
    try { b.dispose(false); } catch (e) {}
  }

  // ---- 8: the symbol goes INSIDE the socket, where the site puts it -------
  //
  // Measured off the live site across 13 empty sockets on four blocks, every
  // one identical: the hole is 32 tall with a 16 radius, the icon group sits
  // at (holeLeft - 16, holeCentreY) and the image is 24.6154 square at
  // (24, -12.3077) inside it. Those are the numbers this has to reproduce.
  // Checked as a rule against a radius, not as three magic numbers, so it
  // still holds if the site changes its corner radius.
  {
    const p = BF6.valueIconPlacement(16, 438.6875, 25);
    const near = (a, b) => Math.abs(a - b) < 0.001;
    check(near(p.gx, 438.6875), `the icon group sits at ${p.gx}, the site puts it on the connection point`);
    check(near(p.gy, 25), `the icon group sits at y ${p.gy}, the site puts it at 25`);
    check(near(p.size, 24.615384615384613), `the icon is ${p.size} across, the site draws it 24.6154`);
    check(near(p.ix, 24), `the icon is inset ${p.ix}, the site insets it 24`);
    check(near(p.iy, -12.307692307692307), `the icon is offset ${p.iy}, the site offsets it -12.3077`);

    // and it scales with the radius rather than being pinned to 16
    const q = BF6.valueIconPlacement(8, 100, 50);
    check(near(q.size, 12.307692307692307), `at radius 8 the icon is ${q.size}, not half of 24.6154`);
    check(near(q.gx, 100) && near(q.ix, 12), 'the placement does not follow the radius');

    // THE RADIUS COMES FROM THE SOCKET, NOT FROM THE BLOCK'S CORNER.
    // Reading CORNER_RADIUS gave 2 on our provider and 1 on the site, so the
    // symbol was drawn three pixels across: present in the svg and invisible.
    // The site's socket is 32 tall with a 16 radius, and its
    // EMPTY_INLINE_INPUT_HEIGHT is 32. Anything under about 12 is unreadable.
    const tiny = BF6.valueIconPlacement(2, 0, 0);
    check(tiny.size < 4,
      'the radius is meant to be half the socket height; a block corner radius gives a symbol nobody can see');
    notes.push('  socket symbol placement matches the site at radius 16 and follows the radius');
  }

  // ---- 9: a symbol that could not resolve YET is not lost for good --------
  //
  // The definitions arrive before the style, and the style is what carries the
  // mirrored symbols, so on the first pass every badge resolves to nothing.
  // The conversion has happened by then and never corrects itself, which is
  // why blocks sat on the canvas with no head badge while this very file
  // passed: the test had the mirror in hand and the running editor did not.
  // What makes it recoverable is that the raw readings are kept and the misses
  // are counted, so the arrival of the style can convert again.
  {
    // A style with NO art at all: every badge must fail, and be counted.
    BF6.setStyle({ op: 'style', source: 'default', renderer: 'zelos' });
    BF6.installDefinitions(synth, 'cache');
    const blind = BF6.state.iconsDropped || 0;
    check(blind > 100,
      `with no art, only ${blind} symbols reported as unresolved; they are being lost silently`);
    check(!!BF6.state.rawReadings,
      'the raw readings were not kept, so the conversion can never be done again');

    const w2 = new Blockly.Workspace();
    let noIcon = null;
    try {
      noIcon = w2.newBlock('FindUIWidgetWithName');
      check(imagesOf(noIcon, Blockly).length === 0,
        'a symbol was drawn from art that is not there');
      noIcon.dispose(false);
    } catch (e) { fails.push('a block would not build without art: ' + e.message); checks++; }

    // Now the art arrives, and converting again brings the badges back.
    BF6.setStyle(raw);
    BF6.installDefinitions(BF6.state.rawReadings, 'cache');
    const after = BF6.state.iconsDropped || 0;
    check(after < blind,
      `converting again with the art in hand still lost ${after} of ${blind} symbols`);
    if (art) {
      const back = w2.newBlock('FindUIWidgetWithName');
      const head = imagesOf(back, Blockly).filter(x => /^ICON/.test(x.field.name || ''));
      check(head.length === 1,
        'FindUIWidgetWithName did not get its head badge back after the art arrived');
      try { back.dispose(false); } catch (e) {}
    }
    try { w2.dispose(); } catch (e) {}
    notes.push('  ' + blind + ' symbols unresolved with no art, ' + after + ' with it: they come back');
  }

  // ---- 10: the seven we write ourselves take the site's SHAPE -------------
  //
  // These carry mutators, which json cannot express, so their init was written
  // by hand - and a hand-written init is a guess at a block somebody else
  // designed. Measured against the site, every one of them was wrong: labels
  // the site does not draw, rows in the wrong place, ARG where the site says
  // GetSubroutineArgument. The shape now comes from the reading and only the
  // BEHAVIOUR is ours, so this checks both halves: the site's wording arrived,
  // and the mutator still mutates.
  {
    BF6.setStyle(raw);
    BF6.installDefinitions(synth, 'cache');
    check(BF6.state.mutatorsFromSite >= 5,
      `only ${BF6.state.mutatorsFromSite} of the hand-written blocks took the site's shape`);

    const w3 = new Blockly.Workspace();
    const words = t => { const b = w3.newBlock(t); const s = textOf(b); b.dispose(false); return s; };

    // the site's own wording, not ours
    check(!/\bRULES\b/.test(words('modBlock')),
      'modBlock still draws a RULES label the site does not have');
    const rule = words('ruleBlock');
    check(/Event/.test(rule) && !/\bEVENT\b/.test(rule),
      `ruleBlock says "${rule}", the site writes Event`);
    check(/Conditions/.test(rule) && /Actions/.test(rule),
      'ruleBlock lost the site wording for its two statement rows');
    check(/GetSubroutineArgument/.test(words('subroutineArgumentBlock')),
      'subroutineArgumentBlock is still labelled ARG');
    check(!/\bCALL\b/.test(words('subroutineInstanceBlock')),
      'subroutineInstanceBlock still draws a CALL label the site does not have');

    // the site's own input names, which the behaviour has to target
    const sub = w3.newBlock('subroutineBlock');
    check(!!sub.getInput('PARAMETERS'), 'subroutineBlock has no PARAMETERS row to put labels in');
    // THE SITE WRITES THE BARE NAME, AND ONLY THE NAME. Counted off the live
    // page across all 34 subroutines: a single field named PARAMETER_LABELS
    // holding 'Spawner, Team' or 'Player, Captured'. Never a type, never one
    // field per parameter. Ours used to print 'who : Player', which is a
    // second statement of what the socket's own symbol already says.
    sub.loadExtraState({ subroutineName: 'S', parameters: [{ name: 'who', types: 'Player' }] });
    const one = textOf(sub);
    check(one.indexOf('who') >= 0,
      'a subroutine parameter label did not reach the row the site puts it in');
    check(one.indexOf('who : Player') < 0,
      `the parameter label still prints its type, which the site never does: "${one}"`);
    check(!!sub.getInput('PARAMETERS').fieldRow.some(f => f.name === 'PARAMETER_LABELS'),
      'the parameter label is not in the field the site names PARAMETER_LABELS');

    // two parameters are ONE field, joined - not one field each
    sub.loadExtraState({ subroutineName: 'S',
      parameters: [{ name: 'Spawner', types: 'Number' }, { name: 'Team', types: 'Team' }] });
    const two = textOf(sub);
    check(two.indexOf('Spawner, Team') >= 0,
      `two parameters should read "Spawner, Team" in one label, got "${two}"`);
    const labels = sub.getInput('PARAMETERS').fieldRow
      .filter(f => f.name === 'PARAMETER_LABELS' || String(f.name || '').indexOf('BF6P') === 0);
    check(labels.length === 1,
      `two parameters produced ${labels.length} label fields, the site draws exactly one`);
    sub.dispose(false);

    // and the mutators still mutate
    const call = w3.newBlock('subroutineInstanceBlock');
    call.loadExtraState({ subroutineName: 'S', parameters: [{ name: 'a', types: 'Number' }, { name: 'b', types: 'Number' }] });
    check(!!call.getInput('PARAM-0') && !!call.getInput('PARAM-1'),
      'a subroutine call did not grow a socket per parameter');
    if (call.getInput('ENDPARAMS')) {
      const order = call.inputList.map(i => i.name);
      check(order.indexOf('PARAM-1') < order.indexOf('ENDPARAMS'),
        'the parameter sockets were put after the marker the site ends them with');
    }
    call.dispose(false);

    const iff = w3.newBlock('If');
    iff.loadExtraState({ elseif: 2, else: 1 });
    check(!!iff.getInput('IF1') && !!iff.getInput('IF2') && !!iff.getInput('ELSE'),
      'If did not grow its ELSE IF and ELSE rows');
    check(JSON.stringify(iff.saveExtraState()) === '{"elseif":2,"else":1}',
      'If did not write its mutation back the way the site does');

    // THE GEAR, AND THE THREE METHODS BEHIND IT. Asked on the live site, the
    // If block's icon opens controls_if_elseif and controls_if_else and the
    // block carries decompose, compose and saveConnections: Blockly's own
    // standard mutator. The site hangs one on all 105 of its If blocks.
    check(typeof iff.decompose === 'function', 'If has no decompose, so its bubble has nothing to show');
    check(typeof iff.compose === 'function', 'If has no compose, so its bubble cannot change it');
    check(typeof iff.saveConnections === 'function',
      'If has no saveConnections, so opening the bubble would empty the block');
    check(!!Blockly.Blocks.controls_if_elseif && !!Blockly.Blocks.controls_if_else,
      'the mutator sub-blocks the site uses are not defined here');

    // and the bubble really does describe the block it was opened on
    const bubble = new Blockly.Workspace();
    let container = null, built = '';
    try {
      container = iff.decompose(bubble);
      let c = container.nextConnection && container.nextConnection.targetBlock();
      while (c) { built += c.type + ' '; c = c.nextConnection && c.nextConnection.targetBlock(); }
    } catch (e) { built = 'threw ' + e.message; }
    check(built.trim() === 'controls_if_elseif controls_if_elseif controls_if_else',
      'the bubble for two else-ifs and an else described [' + built.trim() + ']');
    try { bubble.dispose(); } catch (e) {}
    iff.dispose(false);

    const r2 = w3.newBlock('ruleBlock');
    r2.loadExtraState({ isOngoingEvent: false });
    const rowOff = r2.getInput('OBJECTTYPE_DUMMY');
    check(!rowOff || !rowOff.isVisible(),
      'a rule that is not Ongoing still shows its object row');
    r2.loadExtraState({ isOngoingEvent: true });
    check(!!r2.getInput('OBJECTTYPE_DUMMY') && r2.getInput('OBJECTTYPE_DUMMY').isVisible(),
      'an Ongoing rule lost its object row');
    check(JSON.stringify(r2.saveExtraState()) === '{"isOngoingEvent":true}',
      'ruleBlock did not write its mutation back the way the site does');
    r2.dispose(false);

    // a dropdown from the site must still accept a value from an older save
    const arg = w3.newBlock('subroutineArgumentBlock');
    let took = '';
    try { arg.setFieldValue('3', 'ARGUMENT_INDEX'); took = arg.getFieldValue('ARGUMENT_INDEX'); }
    catch (e) { took = 'threw ' + e.message; }
    check(took === '3',
      `an argument index from an older save read back as ${took}; the site's one-option list refused it`);
    arg.dispose(false);

    // AND THE SITE'S SHAPE HAS TO SURVIVE A FILE BEING OPENED.
    //
    // defineMutators is the offline fallback for these seven and is called
    // from more places than the one that installs the site versions: opening
    // a file calls growDefinitions, which calls installFallback, which calls
    // it. That quietly put all seven hand-written blocks back, and the only
    // visible trace was an argument block offering 0,1,2,3 instead of the
    // parameter names of the subroutine it sits in.
    BF6.installFallback(require(path.join(BLOCKS, 'vendor', 'types_fallback.js')),
      BF6.observe({ blocks: { blocks: [] } }));
    check(BF6.state.types.subroutineArgumentBlock === 'site-mutator',
      'opening a file put the hand-written subroutineArgumentBlock back');
    // AND THE OTHER 597 HAVE TO SURVIVE IT TOO. installFallback used to
    // replace every type it had a mined record for, so opening a project
    // threw away the site definitions and rebuilt the workspace from the
    // approximation: every head badge gone and every block 54px narrower.
    check(BF6.state.types.AbsoluteValue === 'json',
      'opening a file replaced the site definition for AbsoluteValue with the mined one');
    const kept = w3.newBlock('AbsoluteValue');
    check(imagesOf(kept, Blockly).filter(x => /^ICON/.test(x.field.name || '')).length === 1,
      'AbsoluteValue lost its head badge when a file was opened');
    try { kept.dispose(false); } catch (e) {}
    check(BF6.state.types.ruleBlock === 'site-mutator',
      'opening a file put the hand-written ruleBlock back');
    const after = w3.newBlock('subroutineArgumentBlock');
    check(textOf(after).indexOf('GetSubroutineArgument') >= 0,
      'after a file open the argument block is labelled ' + JSON.stringify(textOf(after)));
    try { after.dispose(false); } catch (e) {}

    try { w3.dispose(); } catch (e) {}
    notes.push('  ' + BF6.state.mutatorsFromSite +
      " hand-written blocks now take the site's shape and keep their mutator");
  }

  // ---- 6: nothing anywhere reads VALUE-0 ----------------------------------
  //
  // Across a wide sample, not one or two: the plumbing label is the single
  // most visible way a block stops looking like the site's block.
  const sample = Object.keys(synth).slice(0, 250);
  let plumbing = 0, built = 0;
  sample.forEach(type => {
    if (!Blockly.Blocks[type]) return;
    let b = null;
    try { b = ws.newBlock(type); } catch (e) { return; }
    built++;
    fieldsOf(b).forEach(x => {
      const f = x.field;
      if (!(f instanceof Blockly.FieldLabel)) return;
      let t = '';
      try { t = f.getText(); } catch (e) {}
      // The number is what makes it plumbing. "If" is a word.
      if (/^(VALUE|PARAM|IF|ARG|INPUT)[\s_-]*\d+$/i.test(String(t).trim())) plumbing++;
    });
    try { b.dispose(false); } catch (e) {}
  });
  check(plumbing === 0, plumbing + ' sockets still read their internal name');
  notes.push('  ' + built + ' types built from the site reading, ' + plumbing +
    ' sockets reading an internal name');

  try { ws.dispose(); } catch (e) {}

  console.log('sitedefs checks   ', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log(n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run };

if (require.main === module) {
  const Blockly = loadBlockly();
  const BF6 = require(path.join(BLOCKS, 'editor.js'));
  BF6.attach(Blockly);
  const r = run(Blockly, BF6);
  console.log(r.ok ? 'SITEDEFS OK' : 'SITEDEFS FAILED');
  process.exit(r.ok ? 0 : 1);
}
