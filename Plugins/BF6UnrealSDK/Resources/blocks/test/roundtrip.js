// Round-trip proof for the tool's Blockly workspace.
//
// Loads a real Portal project into a headless workspace built from editor.js,
// saves it again, and reports every semantic difference. Zero differences is
// the bar, and block ids must survive.
//
//   node roundtrip.js [path-to-workspace.json]
//
// Default fixture: the Night Ops Breakthrough project (5,088 blocks, 43 rules,
// 34 subroutines, 104 variables).

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');
const FIXTURE = process.argv[2] ||
  'C:\\Users\\mwalt\\Downloads\\night_ops_breakthrough_workspace.json';

// ---- Blockly, headless -----------------------------------------------------
// One loader for both test files, kept in style.js so there is one copy of the
// DOM stubs to maintain.
const STYLE = require('./style.js');
const loadBlockly = STYLE.loadBlockly;

// ---- semantic diff ---------------------------------------------------------
function diff(a, b, at, out, limit) {
  if (out.length >= limit) return out;
  if (a === b) return out;
  const ta = a === null ? 'null' : Array.isArray(a) ? 'array' : typeof a;
  const tb = b === null ? 'null' : Array.isArray(b) ? 'array' : typeof b;
  if (ta !== tb) { out.push(`${at}: type ${ta} -> ${tb}`); return out; }
  if (ta === 'number') {
    if (Math.abs(a - b) > 1e-9) out.push(`${at}: ${a} -> ${b}`);
    return out;
  }
  if (ta !== 'object' && ta !== 'array') {
    out.push(`${at}: ${JSON.stringify(a)} -> ${JSON.stringify(b)}`);
    return out;
  }
  if (ta === 'array') {
    if (a.length !== b.length) out.push(`${at}: length ${a.length} -> ${b.length}`);
    for (let i = 0; i < Math.min(a.length, b.length); i++) diff(a[i], b[i], `${at}[${i}]`, out, limit);
    return out;
  }
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const k of keys) {
    if (!(k in a)) { out.push(`${at}.${k}: missing in original, added ${JSON.stringify(b[k]).slice(0, 120)}`); continue; }
    if (!(k in b)) { out.push(`${at}.${k}: dropped ${JSON.stringify(a[k]).slice(0, 120)}`); continue; }
    diff(a[k], b[k], `${at}.${k}`, out, limit);
  }
  return out;
}

function countBlocks(node, acc) {
  if (!node || typeof node !== 'object') return acc;
  if (Array.isArray(node)) { node.forEach(n => countBlocks(n, acc)); return acc; }
  if (node.type && node.id) { acc.n++; acc.ids.add(node.id); }
  for (const k of Object.keys(node)) countBlocks(node[k], acc);
  return acc;
}

// ---- run -------------------------------------------------------------------
const Blockly = loadBlockly();
const BF6 = require(path.join(BLOCKS, 'editor.js'));
const TYPES = require(path.join(BLOCKS, 'vendor', 'types_fallback.js'));

BF6.attach(Blockly);

const raw = JSON.parse(fs.readFileSync(FIXTURE, 'utf8'));
const original = BF6.unwrap(raw);

const observed = BF6.observe(original);
const nDefs = BF6.installFallback(TYPES, observed);

const ws = new Blockly.Workspace();
BF6.loadWorkspace(ws, original);
const saved = BF6.saveWorkspace(ws);

const a = countBlocks(original, { n: 0, ids: new Set() });
const b = countBlocks(saved, { n: 0, ids: new Set() });
const lostIds = [...a.ids].filter(id => !b.ids.has(id));

const differences = diff(original, saved, 'root', [], 60);

console.log('fixture           ', FIXTURE);
console.log('definitions       ', nDefs, 'types (fallback) +', BF6.MUTATOR_TYPES.length, 'hand written');
console.log('blocks in / out   ', a.n, '/', b.n);
console.log('ids preserved     ', a.ids.size - lostIds.length, 'of', a.ids.size);
console.log('top level in / out',
  original.blocks.blocks.length, '/', saved.blocks.blocks.length);
console.log('variables in / out', (original.variables || []).length, '/', (saved.variables || []).length);
console.log('differences       ', differences.length);
differences.slice(0, 40).forEach(d => console.log('   ', d));
if (lostIds.length) console.log('lost ids          ', lostIds.slice(0, 20).join(', '));

// unit round trip: every sync unit must survive a replace with itself
// Replacing the mod shell rebuilds every rule under it, so each unit is looked
// up again by id: the objects from the first pass are gone by then.
const unitIds = BF6.unitsOf(ws).map(u => u.id);
let unitFail = 0;
const unitErrors = [];
unitIds.forEach(id => {
  const u = ws.getBlockById(id);
  if (!u) { unitFail++; unitErrors.push(id + ': vanished before replace'); return; }
  const json = BF6.saveUnit(u);
  const before = JSON.stringify(json);
  BF6.applyReplace(ws, { id: id, json: json, anchor: BF6.captureAnchor(u) });
  const nb = ws.getBlockById(id);
  if (!nb) { unitFail++; unitErrors.push(id + ': gone after replace'); return; }
  if (JSON.stringify(BF6.saveUnit(nb)) !== before) {
    unitFail++;
    unitErrors.push(id + ' (' + nb.type + '): unit changed');
  }
});
unitErrors.slice(0, 8).forEach(e => console.log('    unit', e));
console.log('sync units        ', unitIds.length, 'replace-with-self failures:', unitFail);

const after = BF6.saveWorkspace(ws);
const afterDiff = diff(original, after, 'root', [], 20);
console.log('after unit replay ', afterDiff.length, 'differences');
afterDiff.slice(0, 10).forEach(d => console.log('   ', d));

// ---- slot search -----------------------------------------------------------
// Clicking an empty input has to answer "what fits here". The answer comes from
// the typed signature in each block's tooltip, so it is checked offline.
BF6.buildSignatures(null);
const sigCount = Object.keys(BF6.state.signatures).length;
console.log('signatures        ', sigCount, 'types typed from tooltips');
console.log('  GetCapturePoint ', JSON.stringify(BF6.state.signatures['GetCapturePoint']));
console.log('  EventPlayer     ', JSON.stringify(BF6.state.signatures['EventPlayer']));
console.log('  GetObjId        ', JSON.stringify(BF6.state.signatures['GetObjId']));

function slotCheck(type, input, kind, mustHave) {
  const r = BF6.slotCandidates(type, input, kind, '');
  const names = new Set(r.exact.concat(r.loose).map(x => x.type));
  const missing = mustHave.filter(m => !names.has(m));
  console.log(`slot ${type}.${input} wants ${JSON.stringify(r.wanted)}: ` +
    `${r.exact.length} exact, ${r.loose.length} loose, ${r.unknown.length} untyped` +
    (missing.length ? `  MISSING ${missing.join(', ')}` : '  has ' + mustHave.join(', ')));
  return missing.length === 0;
}
let slotOk = true;
slotOk = slotCheck('GetCapturePoint', 'VALUE-0', 'value', ['Number', 'GetObjId']) && slotOk;
slotOk = slotCheck('GetTeam', 'VALUE-0', 'value', ['EventPlayer']) && slotOk;
slotOk = slotCheck('ruleBlock', 'ACTIONS', 'statement', ['SetVariable', 'If', 'Wait']) && slotOk;

// ---- zoom floor ------------------------------------------------------------
const bounds = ws.getBlocksBoundingBox ? ws.getBlocksBoundingBox() : null;
if (bounds) {
  const w = (bounds.right - bounds.left) || bounds.width;
  const h = (bounds.bottom - bounds.top) || bounds.height;
  const scale = BF6.fitScale({ width: w, height: h }, 1600, 900, 40);
  console.log('fixture extent    ', Math.round(w), 'x', Math.round(h),
    'fits a 1600x900 view at scale', scale.toFixed(4));
}

// ---- snippets --------------------------------------------------------------
// Every shipped snippet must fill its placeholders and connect, with the same
// definitions an offline session has.
const SNIPDIR = path.join(BLOCKS, 'snippets');
let snipOk = true;
fs.readdirSync(SNIPDIR).filter(f => f.endsWith('.json')).forEach(f => {
  const doc = JSON.parse(fs.readFileSync(path.join(SNIPDIR, f), 'utf8'));
  const ctx = {
    objId: () => 101,
    variable: (name, type) => {
      let v = ws.getVariable(name, type || '');
      if (!v) v = ws.createVariable(name, type || '');
      return v.getId();
    },
    text: label => label
  };
  const filled = BF6.fillPlaceholders(doc, ctx);
  const list = filled.json.blocks.blocks;
  let placed = 0, err = '';
  try {
    list.forEach(b => {
      const nb = Blockly.serialization.blocks.append(BF6.stripIds(JSON.parse(JSON.stringify(b))), ws);
      placed += nb.getDescendants(false).length;
      nb.dispose(false);
    });
  } catch (e) { err = String(e.message || e); }
  const bad = err || placed === 0;
  if (bad) snipOk = false;
  console.log('snippet ' + f.padEnd(30), bad ? 'FAILED ' + err : placed + ' blocks placed, ' +
    (filled.missing.length ? filled.missing.length + ' placeholders left' : 'all placeholders filled'));
});

// ---- journal replay after a lost session -----------------------------------
// Signed out mid-edit: five rules changed locally, the site still holding the
// old ones and one rule changed by someone else. Coming back must re-apply
// exactly our five, take exactly their one, and touch nothing else.
const staleSite = JSON.parse(JSON.stringify(BF6.saveWorkspace(ws)));
const localBefore = BF6.saveWorkspace(ws);
const localUnits = BF6.unitsOfJson(localBefore);
const ruleIds = Object.keys(localUnits).filter(id => localUnits[id].kind === 'rule');

// the site moved one rule we never touched
const siteUnits = BF6.unitsOfJson(staleSite);
const siteOnlyId = ruleIds[ruleIds.length - 1];
(function bumpSiteOnly(node) {
  (function walk(n) {
    if (!n || typeof n !== 'object') return;
    if (n.id === siteOnlyId && n.fields && n.fields.NAME) { n.fields.NAME = n.fields.NAME + ' (theirs)'; return; }
    for (const k of Object.keys(n)) walk(n[k]);
  })(node);
})(staleSite);

// five local edits while signed out
const edited = ruleIds.slice(0, 5);
edited.forEach((id, i) => {
  const b = ws.getBlockById(id);
  b.setFieldValue('Edited ' + i, 'NAME');
});
const localAfter = BF6.saveWorkspace(ws);
const r = BF6.reconcile(localAfter, staleSite, edited);

const pushedIds = r.toSite.map(u => u.id).sort();
const pulledIds = r.toLocal.map(u => u.id).sort();
const wantPush = edited.slice().sort();
const journalOk =
  JSON.stringify(pushedIds) === JSON.stringify(wantPush) &&
  JSON.stringify(pulledIds) === JSON.stringify([siteOnlyId]);
console.log('journal replay    ', 'pushed', pushedIds.length, 'pulled', pulledIds.length,
  'unchanged', r.stats.unchanged, journalOk ? 'OK' : 'MISMATCH');
if (!journalOk) {
  console.log('    expected push', wantPush.join(', '));
  console.log('    got push     ', pushedIds.join(', '));
  console.log('    expected pull', siteOnlyId, 'got', pulledIds.join(', '));
}

// applying the reconcile locally must leave the five edits alone and adopt theirs
r.toLocal.forEach(u => BF6.applyReplace(ws, { id: u.id, json: u.json, anchor: u.anchor }));
const afterReconcile = BF6.saveWorkspace(ws);
const units2 = BF6.unitsOfJson(afterReconcile);
let keptOk = edited.every((id, i) =>
  JSON.stringify(units2[id].json).indexOf('Edited ' + i) >= 0);
keptOk = keptOk && JSON.stringify(units2[siteOnlyId].json).indexOf('(theirs)') >= 0;
console.log('after reconcile   ', keptOk ? 'local edits kept, their rule adopted' : 'FAILED');

// ---- Portal verdict relay --------------------------------------------------
// A refusal names blocks; the warning has to land on those blocks and clear
// again on the next good save.
const targets = ruleIds.slice(0, 2);
const relay = BF6.portalRelay(ws, {
  status: 3,
  message: 'INVALID_ARGUMENT: rule references an object that does not exist',
  blocks: targets.map(id => ({ id: id, text: 'This rule points at ObjId 999' }))
});
// Headless blocks have no warning icon (Blockly's base Block.setWarningText is
// a no-op), so what Portal said is checked where the editor keeps it.
const warned = targets.filter(id => (BF6.state.portalWarnings || {})[id]);
const cleared = BF6.clearPortalWarnings(ws);
const stillWarned = Object.keys(BF6.state.portalWarnings || {});
const relayOk = relay.applied.length === 2 && warned.length === 2 &&
  cleared === 2 && stillWarned.length === 0;
console.log('portal relay      ', 'warned', warned.length, 'cleared', cleared,
  relayOk ? 'OK' : 'FAILED');

// ---- the two Portal file formats -------------------------------------------
// Runs after the workspace assertions because it reinstalls the definitions
// from a converted workspace, the way an import does.
const fileResult = require('./portalfile.js').run(Blockly, BF6);

// ---- the site's look -------------------------------------------------------
// Runs last: it wraps the definitions' init functions to put the site's colours
// on them, and nothing above should be measured through that.
const styleResult = STYLE.run(Blockly, BF6);

// ---- the toolbox icons -----------------------------------------------------
// The site's own art, joined to the toolbox by name and merged by tier. Runs
// after the style checks because it reads the style the same way the page does.
const iconResult = require('./icons.js').run(Blockly, BF6);

// ---- the page, and the curated answers -------------------------------------
// That every id the script reaches for is in the markup, that every handle has
// a panel behind it, and that every curated answer's block example really does
// load into a workspace the way its INSERT button claims.
const pageResult = require('./page.js').run(Blockly, BF6);

// ---- what a block actually looks like --------------------------------------
// The colour it comes back, the style it wears, and whether its sockets say
// what fits. Runs last because it needs the style, the icons and the
// definitions all in place, which is the state the editor opens in.
const lookResult = require('./look.js').run(Blockly, BF6);

// ---- the site's own definitions, drawn ------------------------------------
// Everything above works from the mined catalogue, which is the offline path
// and has to keep working. This one takes the reading the capture brought back
// off the real site and checks that what it produces is the site's block: the
// type symbol at its head, the words between its sockets, and a literal that
// shows its value rather than its type name. Runs last of all because it
// replaces the definitions with the site's.
const siteDefsResult = require('./sitedefs.js').run(Blockly, BF6);
const valueIconsResult = require('./value_icons.js').run(Blockly, BF6);

// ---- moving the view around -------------------------------------------------
// Panning is arithmetic against Blockly's own clamp, and getting the convention
// wrong made a drag jump to the edge and stick there. This runs the real
// scroll() out of the vendored bundle, so it needs nothing set up above it.
const panResult = require('./pan.js').run();

const ok = differences.length === 0 && lostIds.length === 0 && unitFail === 0 &&
  afterDiff.length === 0 && slotOk && snipOk && journalOk && keptOk && relayOk &&
  fileResult.ok && styleResult.ok && iconResult.ok && pageResult.ok &&
  lookResult.ok && siteDefsResult.ok && valueIconsResult.ok && panResult.ok;
console.log(ok ? 'ROUND TRIP OK' : 'ROUND TRIP FAILED');
process.exit(ok ? 0 : 1);
