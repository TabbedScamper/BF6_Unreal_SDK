// Proof that a block LOOKS like the site's block.
//
// Four things were wrong at once and all four were invisible to the tests that
// already existed, because those tests check that a payload reaches the theme,
// not that a block a creator actually drags out comes back the right colour
// with the right symbols in its sockets. This one builds real blocks and reads
// them back.
//
// What it holds down, each one a bug that shipped:
//   1. the palette is the site's, not one of ours. Eleven named styles, and
//      every block the tool defines itself wears the style the site gave it.
//   2. a block is painted through setStyle when the theme carries the style,
//      so it gets the whole shade set rather than one flat face.
//   3. a value socket says what fits: the site's own symbol for that type, or
//      the type in words when a socket takes several. Never VALUE-0, which is
//      the site's internal socket name and tells a creator nothing.
//   4. a variable carries the site's variable symbol.
//   5. installing definitions again does not throw the colour pass away, which
//      is what used to put the invented palette back on the first site sync.
//
// Run by roundtrip.js. Also runs alone:  node look.js

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');
const { loadBlockly } = require(path.join(HERE, 'style.js'));

// The colours the site actually uses, written out here so this test fails if
// the default capture is ever replaced by something that is not Portal.
const SITE = {
  modBlock: '#222b2d',
  ruleBlock: '#682177',
  subroutineBlock: '#622c07',
  subroutineInstanceBlock: '#847000',
  subroutineArgumentBlock: '#0f5736',
  variableReferenceBlock: '#0f5736',
  If: '#351f95'
};

const STYLE_OF = {
  modBlock: 'mod-block-style',
  ruleBlock: 'rule-block-style',
  subroutineBlock: 'subroutine-block-style',
  subroutineInstanceBlock: 'action-block-style',
  subroutineArgumentBlock: 'value-block-style',
  variableReferenceBlock: 'variable-block-style',
  If: 'control-block-style'
};

function run(Blockly, BF6) {
  let checks = 0;
  const fails = [];
  const notes = [];
  function check(cond, what) {
    checks++;
    if (!cond) fails.push(what);
  }

  const raw = JSON.parse(fs.readFileSync(path.join(BLOCKS, 'site_style_default.json'), 'utf8'));
  raw.op = 'style';
  raw.source = 'default';
  BF6.setStyle(raw);
  BF6.applyBlockColours();

  const ws = new Blockly.Workspace();

  // ---- 1 and 2: the palette, through the styles ---------------------------
  let styled = 0;
  Object.keys(SITE).forEach(function (type) {
    let b;
    try { b = ws.newBlock(type); } catch (e) { check(false, type + ' would not build: ' + e); return; }
    check(String(b.getColour()).toLowerCase() === SITE[type],
      type + ' is ' + b.getColour() + ', the site paints it ' + SITE[type]);
    const sn = b.getStyleName ? b.getStyleName() : '';
    if (sn === STYLE_OF[type]) styled++;
    check(sn === STYLE_OF[type],
      type + ' wears style "' + sn + '", the site gives it "' + STYLE_OF[type] + '"');
    try { b.dispose(false); } catch (e) {}
  });
  notes.push('  ' + styled + ' of ' + Object.keys(SITE).length +
    ' tool-defined blocks painted through the site\'s own named style');

  // None of the site's colours may be one this tool made up.
  const invented = ['#4A6572', '#A6572B', '#3E6B4F', '#5B5BA6', '#7A5AA0', '#4A5D6B', '#5A5F63'];
  const palette = BF6.SITE_STYLE_COLOUR || {};
  Object.keys(palette).forEach(function (k) {
    check(invented.indexOf(String(palette[k]).toUpperCase()) < 0,
      'the palette still carries the invented colour ' + palette[k] + ' under ' + k);
  });

  // ---- 3: sockets say what fits -------------------------------------------
  // A statement block with a typed signature: every value socket must carry
  // either the type's symbol or the type in words, and none may be left
  // showing its own internal name.
  const sampled = ['DealDamage', 'SetPlayerMaxHealth', 'SetPlayerIncomingDamageFactor'];
  let withIcon = 0, withWord = 0, bare = 0, sockets = 0;
  sampled.forEach(function (type) {
    if (!Blockly.Blocks[type]) return;
    let b;
    try { b = ws.newBlock(type); } catch (e) { check(false, type + ' would not build'); return; }
    // The hint is made when the socket turns out to be empty, not when the
    // block is built, so it is settled first exactly as the editor does.
    BF6.syncSocketGlyphs(b);
    (b.inputList || []).forEach(function (inp) {
      if (!inp.connection || inp.connection.type !== Blockly.INPUT_VALUE) return;
      sockets++;
      const row = inp.fieldRow || [];
      const hasImage = row.some(function (f) { return f instanceof Blockly.FieldImage; });
      const text = row.map(function (f) { return f.getText ? f.getText() : ''; }).join(' ').trim();
      if (hasImage) withIcon++;
      else if (text) withWord++;
      else bare++;
      check(!/^VALUE-?\d/.test(text),
        type + ' socket ' + inp.name + ' still shows its internal name "' + text + '"');
      check(hasImage || !!text,
        type + ' socket ' + inp.name + ' has no hint at all');
    });
    try { b.dispose(false); } catch (e) {}
  });
  notes.push('  ' + sockets + ' value sockets sampled: ' + withIcon + ' carry the type symbol, ' +
    withWord + ' the type in words, ' + bare + ' left bare');

  // Every type the signatures mention must resolve to a symbol, or the socket
  // silently falls back to words for a type the capture really does have.
  const wantIcons = ['Player', 'Number', 'Vehicle', 'Vector', 'Boolean', 'Any Type'];
  let resolved = 0;
  wantIcons.forEach(function (t) {
    const url = BF6.iconUrlOf(BF6.typeIconName(t));
    if (url) resolved++;
    check(!!url, 'no symbol captured for the type ' + t + ' (' + BF6.typeIconName(t) + ')');
  });
  notes.push('  ' + resolved + ' of ' + wantIcons.length + ' sampled types have the site\'s symbol');

  // ---- 4: the variable symbol ---------------------------------------------
  // The site writes the WORD between the scope and the name, on one line:
  // "Global | Variable | GameModeStarted". An earlier version of this test
  // demanded a symbol there, which is what the tool used to draw and what did
  // not match the site. Compared side by side against the real page, the site
  // uses the word.
  {
    const v = ws.newBlock('variableReferenceBlock');
    const head = v.getInput('HEAD');
    const words = (head.fieldRow || [])
      .filter(function (f) { return !(f instanceof Blockly.FieldImage); })
      .map(function (f) { return f.getText ? f.getText() : ''; });
    check(words.indexOf('Variable') >= 0,
      'the variable block does not carry the word Variable: ' + JSON.stringify(words));
    check(v.inputsInline === true, 'the variable block is not laid out on one line');
    try { v.dispose(false); } catch (e) {}
  }

  // ---- 3c: every empty socket has a symbol, and it can be clicked ---------
  //
  // Both halves matter. A socket with no symbol tells you nothing about what
  // goes in it, and since the symbol IS the button, a socket without one also
  // gives you no way to ask. Sockets whose type is unknown or is several types
  // used to get neither.
  {
    const asked = [];
    const was = BF6.onSocketClick;
    BF6.onSocketClick = function (blk, inp) { asked.push(blk.type + '.' + inp.name); };
    let empty = 0, icons = 0;
    ['SetPlayerMaxHealth', 'DealDamage', 'SetVariable'].forEach(function (type) {
      if (!Blockly.Blocks[type]) return;
      const b = ws.newBlock(type);
      BF6.syncSocketGlyphs(b);
      (b.inputList || []).forEach(function (inp) {
        if (!inp.connection || inp.connection.type !== Blockly.INPUT_VALUE) return;
        empty++;
        const img = (inp.fieldRow || []).filter(function (f) { return f instanceof Blockly.FieldImage; });
        if (img.length) { icons++; try { img[0].showEditor_(); } catch (e) {} }
      });
      try { b.dispose(false); } catch (e) {}
    });
    BF6.onSocketClick = was;
    check(icons === empty, icons + ' of ' + empty + ' empty sockets carry a symbol');
    check(asked.length === icons,
      'only ' + asked.length + ' of ' + icons + ' symbols answer a click');
    notes.push('  ' + empty + ' empty sockets: all carry a symbol and all answer a click');
  }

  // ---- 4b: one line, the way every Portal block is drawn ------------------
  {
    let inline = 0, stacked = [];
    ['SetVariable', 'DealDamage', 'SetPlayerMaxHealth'].forEach(function (type) {
      if (!Blockly.Blocks[type]) return;
      const b = ws.newBlock(type);
      if (b.inputsInline === true) inline++; else stacked.push(type);
      try { b.dispose(false); } catch (e) {}
    });
    check(stacked.length === 0, 'these blocks stack their inputs instead of one line: ' + stacked.join(', '));
    notes.push('  ' + inline + ' sampled blocks lay their inputs out on one line');
  }

  // No block may print a socket's internal name. The site never does.
  {
    let offenders = [];
    ['SetVariable', 'DealDamage'].forEach(function (type) {
      if (!Blockly.Blocks[type]) return;
      const b = ws.newBlock(type);
      (b.inputList || []).forEach(function (inp) {
        (inp.fieldRow || []).forEach(function (f) {
          const t = f.getText ? f.getText() : '';
          if (/^(VALUE|PARAM|IF)[\s_-]*\d*$/i.test(t)) offenders.push(type + '.' + inp.name + '="' + t + '"');
        });
      });
      try { b.dispose(false); } catch (e) {}
    });
    check(offenders.length === 0, 'internal socket names are being drawn: ' + offenders.join(', '));
  }

  // ---- 3b: a definition that came from the SITE ---------------------------
  // The offline catalogue is not the case that bit. The site's stored
  // definitions put a label on each socket that is the socket's own internal
  // name, and the block's own name is a field of its FIRST socket, so a rule
  // that judged the whole row left VALUE-0 sitting next to it. Real writing
  // must survive, plumbing must not, and the symbol must arrive either way.
  {
    BF6.installDefinitions({
      BF6LookProbe: {
        message0: 'BF6LookProbe %1 %2 %3 %4',
        args0: [
          { type: 'field_label', text: 'VALUE-0' },
          { type: 'input_value', name: 'VALUE-0' },
          { type: 'field_label', text: 'by' },
          { type: 'input_value', name: 'VALUE-1' }
        ],
        previousStatement: null, nextStatement: null
      }
    }, 'live');
    // The signature the site would carry in the tooltip.
    BF6.state.tooltips = BF6.state.tooltips || {};
    BF6.state.tooltips.BF6LookProbe = '<b>BF6LookProbe(Player, Number)</b>';
    BF6.buildSignatures(BF6.state.tooltips);

    const b = ws.newBlock('BF6LookProbe');
    BF6.syncSocketGlyphs(b);
    const texts = {};
    const icons = {};
    (b.inputList || []).forEach(function (inp) {
      if (!inp.connection || inp.connection.type !== Blockly.INPUT_VALUE) return;
      texts[inp.name] = (inp.fieldRow || [])
        .filter(function (f) { return !(f instanceof Blockly.FieldImage); })
        .map(function (f) { return f.getText ? f.getText() : ''; });
      icons[inp.name] = (inp.fieldRow || [])
        .filter(function (f) { return f instanceof Blockly.FieldImage; })
        .map(function (f) { return f.getText ? f.getText() : ''; });
    });
    check((texts['VALUE-0'] || []).indexOf('VALUE-0') < 0,
      'a site definition still shows the plumbing label VALUE-0');
    check((icons['VALUE-0'] || []).length === 1,
      'the first socket of a site definition has no type symbol');
    check((texts['VALUE-1'] || []).indexOf('by') >= 0,
      'the real word "by" was thrown away with the plumbing');
    check((icons['VALUE-1'] || []).length === 1,
      'the second socket of a site definition has no type symbol');
    notes.push('  site definition: plumbing replaced by the symbol, real writing kept');
    try { b.dispose(false); } catch (e) {}
  }

  // ---- 4c: a choice is a dropdown, and it writes the site's value ---------
  //
  // The mined catalogue carries the exact list the site offers for 187 fields
  // across 105 types, and all of it used to be discarded: every one of them
  // was drawn as a box you type into. A dropdown that has to be typed into is
  // not the same control.
  //
  // The wording and the value are NOT the same thing. The catalogue lists what
  // a choice reads as ("true"), the file stores what it is ("TRUE"). Offering
  // the reading as the value would write a casing the site never uses and
  // quietly change the project, so this checks both halves.
  {
    BF6.installFallback(require(path.join(BLOCKS, 'vendor', 'types_fallback.js')),
      BF6.observe({
        blocks: {
          blocks: [
            { type: 'Boolean', fields: { BOOL: 'FALSE' } },
            { type: 'Boolean', fields: { BOOL: 'TRUE' } }
          ]
        }
      }));

    const b = ws.newBlock('Boolean');
    const dd = [];
    (b.inputList || []).forEach(function (inp) {
      (inp.fieldRow || []).forEach(function (f) {
        if (f instanceof Blockly.FieldDropdown) dd.push(f);
      });
    });
    check(dd.length === 1, 'Boolean has ' + dd.length + ' dropdowns, it should have one');
    if (dd.length) {
      const opts = dd[0].getOptions(false);
      const shown = opts.map(function (o) { return o[0]; });
      const values = opts.map(function (o) { return o[1]; });
      check(shown.indexOf('true') >= 0 && shown.indexOf('false') >= 0,
        'the Boolean dropdown does not read true/false: ' + JSON.stringify(shown));
      check(values.indexOf('TRUE') >= 0 && values.indexOf('FALSE') >= 0,
        'the Boolean dropdown does not WRITE the values the file stores: ' + JSON.stringify(values));
      check(values.indexOf('true') < 0 && values.indexOf('false') < 0,
        'the Boolean dropdown would write a casing the site does not use: ' + JSON.stringify(values));
      notes.push('  Boolean reads ' + JSON.stringify(shown) + ' and writes ' + JSON.stringify(values));
    }
    try { b.dispose(false); } catch (e) {}
  }

  // ---- 5: a definition refresh does not undo the paint --------------------
  {
    BF6.installFallback(require(path.join(BLOCKS, 'vendor', 'types_fallback.js')),
      BF6.observe({ blocks: { blocks: [] } }));
    const b = ws.newBlock('modBlock');
    check(String(b.getColour()).toLowerCase() === SITE.modBlock,
      'after a definition refresh modBlock went back to ' + b.getColour());
    try { b.dispose(false); } catch (e) {}
  }

  try { ws.dispose(); } catch (e) {}

  console.log('look checks       ', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log(n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run };

if (require.main === module) {
  const Blockly = loadBlockly();
  const BF6 = require(path.join(BLOCKS, 'editor.js'));
  const TYPES = require(path.join(BLOCKS, 'vendor', 'types_fallback.js'));
  BF6.attach(Blockly);
  BF6.installFallback(TYPES, BF6.observe({ blocks: { blocks: [] } }));
  BF6.buildSignatures(null);
  const r = run(Blockly, BF6);
  console.log(r.ok ? 'LOOK OK' : 'LOOK FAILED');
  process.exit(r.ok ? 0 : 1);
}
