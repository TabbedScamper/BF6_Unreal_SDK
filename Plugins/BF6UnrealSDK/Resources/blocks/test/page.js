// Proof for the page and for the curated answers.
//
// Two things a browser would catch on the first click and a node test can catch
// before anyone opens the editor:
//
//   1. THE MARKUP AND THE SCRIPT AGREE. The page was rebuilt around one canvas
//      and a strip of handles, which moved almost every control into a panel. An
//      id that moved and a $('...') that did not is a null on the first press,
//      so every id the script reaches for is checked against the markup, every
//      handle is checked for the panel behind it, and every panel is checked for
//      a handle in front of it.
//
//   2. THE CURATED ANSWERS ARE WHAT THEY CLAIM. Each one says it carries a block
//      example that has been loaded and checked, and the panel offers to drop it
//      on the canvas in one click. Every one of them is deserialized into a real
//      headless workspace here, so an example that cannot load is a failed test
//      rather than a beginner's first press doing nothing.
//
// Run by roundtrip.js. Also runs alone:  node page.js

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');

// ---- the page --------------------------------------------------------------
function pageCheck(fails, note) {
  const html = fs.readFileSync(path.join(BLOCKS, 'editor.html'), 'utf8');
  const js = fs.readFileSync(path.join(BLOCKS, 'editor_ui.js'), 'utf8');
  let checks = 0;
  const check = (cond, what) => { checks++; if (!cond) fails.push(what); };

  // Every id the markup declares.
  const have = {};
  (html.match(/\bid="[^"]+"/g) || []).forEach(m => { have[m.slice(4, -1)] = 1; });

  // Every id the script reaches for. bf6-site-css is made by the script itself.
  const MADE_BY_SCRIPT = { 'bf6-site-css': 1 };
  const wanted = [...new Set((js.match(/\$\('([A-Za-z0-9_-]+)'\)/g) || [])
    .map(m => m.slice(3, -2)))].filter(id => !MADE_BY_SCRIPT[id]);
  const missing = wanted.filter(id => !have[id]);
  check(missing.length === 0, 'ids the script uses that the page does not have: ' + missing.join(', '));
  note('  ' + wanted.length + ' ids reached for by the script, ' +
    (wanted.length - missing.length) + ' present in the markup');

  // Every handle has a panel, and every panel has a handle.
  const handles = [...new Set((html.match(/data-pane="([a-z]+)"/g) || [])
    .map(m => m.slice(11, -1)))];
  const panes = [...new Set((html.match(/id="pane-([a-z]+)"/g) || [])
    .map(m => m.slice(9, -1)))];
  const noPane = handles.filter(h => panes.indexOf(h) < 0);
  const noHandle = panes.filter(p => handles.indexOf(p) < 0);
  check(noPane.length === 0, 'handles with no panel behind them: ' + noPane.join(', '));
  check(noHandle.length === 0, 'panels with no handle in front of them: ' + noHandle.join(', '));

  // The handle name lives in one place, so the strip and the panel header can
  // never disagree. Every handle must be in it.
  const named = (js.match(/var PANE_NAME = \{[\s\S]*?\};/) || [''])[0];
  const unnamed = handles.filter(h => named.indexOf(h + ':') < 0);
  check(unnamed.length === 0, 'handles with no name in PANE_NAME: ' + unnamed.join(', '));
  note('  ' + handles.length + ' handles, each with a panel and a name');

  // Every panel opens with one sentence saying what it is for.
  const paneBlocks = html.split(/id="pane-/).slice(1);
  const noLede = paneBlocks
    .filter(b => b.slice(0, b.indexOf('</div>') + 6).indexOf('class="lede"') < 0)
    .map(b => b.slice(0, b.indexOf('"')));
  check(noLede.length === 0, 'panels that open with no sentence saying what they are for: ' + noLede.join(', '));

  // ---- which side the shelf is on ------------------------------------------
  // Blockly owns the left edge: the category column is down the left of the
  // canvas and the flyout opens over the canvas beside it, so a panel on the
  // left lands under the categories. This was seen in the built editor and is
  // asserted here rather than eyeballed again.
  const rule = name => {
    const at = html.indexOf(name + ' {');
    if (at < 0) return '';
    return html.slice(at, html.indexOf('}', at));
  };
  const side = rule('#side');
  const rail = rule('#rail');
  const railbtn = rule('.railbtn');

  check(/right:\s*106px/.test(side), 'the panel is not anchored to the right edge');
  check(!/(^|[^-])left:/.test(side), 'the panel still carries a left anchor: ' + side.replace(/\s+/g, ' '));
  check(/border-left/.test(side), 'the panel draws its edge on the wrong side');
  check(/border-left/.test(rail), 'the handle strip draws its edge on the wrong side');
  check(/border-right:\s*3px/.test(railbtn), 'a handle marks itself on the wrong side');

  // The canvas comes first in the flex row and the strip after it, which is
  // what puts the strip on the right and keeps the two from overlapping.
  const bodyAt = html.indexOf('<div id="body">');
  const centreAt = html.indexOf('<div id="centre">', bodyAt);
  const railAt = html.indexOf('<div id="rail">', bodyAt);
  const sideAt = html.indexOf('<div id="side">', bodyAt);
  check(centreAt > 0 && railAt > centreAt, 'the handle strip is not after the canvas, so it is not on the right');
  check(sideAt > railAt, 'the panel paints before the strip');

  // An open panel must not resize or reflow the workspace. It is absolutely
  // positioned over the canvas, and nothing in the open state may touch the
  // width of the canvas, the row it sits in, or the strip.
  check(/position:\s*absolute/.test(side), 'the panel is in the flow, so opening it would resize the canvas');
  const openRules = (html.match(/body\.shelf-open[^{]*\{[^}]*\}/g) || []);
  const resizes = openRules.filter(r =>
    /#centre|#canvas|#body|#rail\b/.test(r.slice(0, r.indexOf('{'))) ||
    /\bwidth\s*:|\bflex\s*:|\bmargin(-right|-left)?\s*:|\binset\s*:/.test(r));
  check(resizes.length === 0,
    'an open panel changes the canvas geometry: ' + resizes.join(' | '));
  note('  panel anchored right, ' + openRules.length +
    ' open-state rules, none of which touches the canvas geometry');

  // The three things that already live in that corner have to step aside.
  const openCss = openRules.join('\n');
  check(/#minimap/.test(openCss), 'the minimap is not moved out from under the panel');
  check(/blocklyZoom/.test(openCss) && /blocklyTrash/.test(openCss),
    'Blockly\'s zoom controls and trashcan are not moved out from under the panel');
  check(/translate:/.test(openCss),
    'the workspace furniture is moved with something other than the composing translate property');
  check(/#slotpop/.test(openCss),
    'the what-fits-here popup and the panel can still want the same corner');

  // Blockly's own layers must stay above the panel: a field editor opened on a
  // block near the right edge cannot render behind it.
  const sideZ = Number((side.match(/z-index:\s*(\d+)/) || [0, 0])[1]);
  ['blocklyWidgetDiv', 'blocklyDropDownDiv', 'blocklyTooltipDiv'].forEach(cls => {
    const r = rule('.' + cls);
    const z = Number((r.match(/z-index:\s*(\d+)/) || [0, 0])[1]);
    check(z > sideZ, cls + ' is not pinned above the panel (' + z + ' against ' + sideZ + ')');
  });
  const slotZ = Number((rule('#slotpop').match(/z-index:\s*(\d+)/) || [0, 0])[1]);
  check(slotZ > sideZ, 'the what-fits-here popup would open behind the panel');
  note('  stacking: panel ' + sideZ + ', popup ' + slotZ +
    ', Blockly field editors and menus above both');

  // A panel cannot open under the toolbar: it is inside #body, and #body is
  // the row below the bar, so its top is the bar's bottom.
  const barEndAt = html.indexOf('</div>', html.indexOf('<div class="bar">'));
  check(bodyAt > barEndAt, 'the panel row starts before the toolbar ends');
  check(/top:\s*0/.test(side) && /bottom:\s*0/.test(side),
    'the panel is not pinned to its own row, so it could ride over the toolbar');

  // TWO GROUPS, AND THE LEFT ONE STAYS AT FIVE.
  //
  // The rule was a flat count, which is the right rule for the left of the
  // bar: that is where sprawl happens, and everything that does not earn a
  // place there belongs in More. It is the wrong rule for the right, because
  // the site itself keeps the workspace actions - search, the two script
  // conversions, import, export and reset - in the top right corner, and
  // matching the site is the whole point of this editor.
  //
  // So the count is split. The left group is still capped at five. The right
  // group is named, has to be pinned right, and every control in it has to say
  // what it does on hover, because they are destructive or slow or both.
  const barStart = html.indexOf('<div class="bar">');
  const barEnd = html.indexOf('</div>', barStart);
  const bar = html.slice(barStart, barEnd);
  const toolsAt = bar.indexOf('<span class="wstools"');
  check(toolsAt > 0, 'the workspace tools group is not in the toolbar');
  const left = toolsAt > 0 ? bar.slice(0, toolsAt) : bar;
  const right = toolsAt > 0 ? bar.slice(toolsAt) : '';
  const leftControls = (left.match(/<button/g) || []).length + (left.match(/<select/g) || []).length;
  check(leftControls <= 5,
    'the left of the toolbar has ' + leftControls + ' controls at rest, which is more than five');
  const rightControls = (right.match(/<button/g) || []).length + (right.match(/<input/g) || []).length;
  check(rightControls >= 6,
    'the workspace tools group has only ' + rightControls + ' controls; search, export to script, ' +
    'import script, import workspace, export workspace and reset are all meant to be there');
  check(new RegExp('margin-left:' + String.fromCharCode(92) + 's*auto').test(html.slice(html.indexOf('.wstools'), html.indexOf('.wstools') + 200)),
    'the workspace tools are not pinned to the right of the bar');
  ['btn-exportscript', 'btn-importscript', 'btn-importws', 'btn-exportws', 'btn-resetws'].forEach(function (id) {
    const at = right.indexOf('id="' + id + '"');
    check(at > 0, id + ' is missing from the workspace tools');
    if (at > 0) {
      const tag = right.slice(right.lastIndexOf('<button', at), right.indexOf('>', at) + 1);
      check(/title="/.test(tag), id + ' has no hover text saying what it does');
    }
  });
  note('  toolbar: ' + leftControls + ' on the left, ' + rightControls + ' workspace tools on the right');
  note('  toolbar at rest: ' + leftControls + ' on the left plus ' + rightControls + ' workspace tools, then the canvas and the handle strip');

  return checks;
}

// ---- the curated answers ---------------------------------------------------
function answersCheck(Blockly, BF6, fails, note) {
  let checks = 0;
  const check = (cond, what) => { checks++; if (!cond) fails.push(what); };

  const dir = path.join(BLOCKS, '..', 'faq', 'answers');
  if (!fs.existsSync(dir)) {
    note('  no curated answer sets on this machine, skipped');
    return checks;
  }
  const files = fs.readdirSync(dir).filter(f => f.endsWith('.json')).sort();
  check(files.length > 0, 'the answers folder holds no json');

  let total = 0, forBlocks = 0, withWorkspace = 0, withTs = 0, loaded = 0;
  const badLoad = [];
  const badSchema = [];

  files.forEach(f => {
    const set = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
    check(set.v === 1, f + ' is not schema v1');
    const entries = set.entries || [];
    entries.forEach(e => {
      total++;
      const eds = [].concat(e.editors || []).map(x => String(x).toLowerCase());
      if (eds.indexOf('blocks') < 0) return;
      forBlocks++;
      // The fields the panel actually shows. A missing one is a blank row.
      if (!e.id || !e.question || !e.short || !e.answer) badSchema.push(e.id || f);
      if (e.ts && e.ts.code) withTs++;
      const ws = e.blocks && e.blocks.workspace;
      if (!ws) return;
      withWorkspace++;
      // The one that matters: it must load. This is the same call the INSERT
      // button makes through the snippet path.
      const w = new Blockly.Workspace();
      try {
        BF6.loadWorkspace(w, ws);
        const n = w.getAllBlocks(false).length;
        if (n > 0) loaded++; else badLoad.push(e.id + ' (loaded nothing)');
      } catch (err) {
        badLoad.push(e.id + ' (' + String(err.message || err) + ')');
      }
      try { w.dispose(); } catch (err) { /* headless workspaces need no teardown */ }
    });
  });

  check(badSchema.length === 0, 'answers missing a field the panel shows: ' + badSchema.join(', '));
  check(badLoad.length === 0, 'block examples that will not load: ' + badLoad.join(', '));
  check(withWorkspace > 0, 'no curated answer carries a block example at all');
  check(loaded === withWorkspace,
    loaded + ' of ' + withWorkspace + ' block examples loaded into a workspace');

  note('  ' + files.length + ' answer set(s), ' + total + ' entries, ' + forBlocks +
    ' for this editor, ' + withWorkspace + ' with a block example, ' + withTs + ' with code');
  note('  ' + loaded + ' of ' + withWorkspace + ' block examples loaded headlessly and drew blocks');

  // The search the panel runs, on the real entries.
  const all = [];
  files.forEach(f => {
    const set = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
    (set.entries || []).forEach(e => {
      const eds = [].concat(e.editors || []).map(x => String(x).toLowerCase());
      if (eds.length && eds.indexOf('blocks') < 0) return;
      all.push(e);
    });
  });
  const hits = all.filter(e => {
    const hay = ((e.question || '') + ' ' + (e.short || '') + ' ' + (e.answer || '') + ' ' +
      [].concat(e.gotchas || []).join(' ')).toLowerCase();
    return hay.indexOf('team') >= 0;
  });
  check(hits.length > 0, 'searching the answers for a word that is certainly in them found nothing');
  note('  a search across question, answer and gotchas for "team" matches ' +
    hits.length + ' of ' + all.length);

  return checks;
}

function run(Blockly, BF6) {
  const fails = [];
  const notes = [];
  const note = n => notes.push(n);
  let checks = 0;
  checks += pageCheck(fails, note);
  checks += answersCheck(Blockly, BF6, fails, note);

  console.log('page checks       ', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log(n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run };

if (require.main === module) {
  const STYLE = require('./style.js');
  const Blockly = STYLE.loadBlockly();
  const BF6 = require(path.join(BLOCKS, 'editor.js'));
  const TYPES = require(path.join(BLOCKS, 'vendor', 'types_fallback.js'));
  BF6.attach(Blockly);
  BF6.installFallback(TYPES, BF6.observe({ blocks: { blocks: [] } }));
  const r = run(Blockly, BF6);
  console.log(r.ok ? 'PAGE OK' : 'PAGE FAILED');
  process.exit(r.ok ? 0 : 1);
}
