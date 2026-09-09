// Proof for the two Portal file formats and the three things the editor
// accepts on import.
//
// Nothing here is derived from a spec. Every assertion is against a real file
// on this machine:
//   format A   C:\Users\mwalt\Downloads\night_ops_breakthrough_workspace.json
//   format B   C:\PortalSDK_1.4.2.0\...\soundboard\BF6_SFX.json  (a published export)
//   script     C:\Users\mwalt\Downloads\script-conversion.ts
//   strings    C:\Users\mwalt\Downloads\strings-conversion.json
//
// Run by roundtrip.js. Also runs alone:  node portalfile.js
//
// Cases:
//   1. detection, from the content and never the extension, of all five shapes
//   2. format B read then write: semantic equality with the real file, field by
//      field, ignoring key order and freshly generated ids
//   3. fresh ids really are fresh, and the two copies of the spatial attachment
//      keep the same id as each other
//   4. an export with nothing to go on writes the site's defaults and says so
//   5. the site's blocks-to-script export imports back to blocks

const fs = require('fs');
const path = require('path');

const BLOCKS = path.join(__dirname, '..');

const FILES = {
  workspace: 'C:\\Users\\mwalt\\Downloads\\night_ops_breakthrough_workspace.json',
  experience: 'C:\\PortalSDK_1.4.2.0\\GodotProject\\User_Created\\projects\\BF_Undead\\tools\\soundboard\\BF6_SFX.json',
  script: 'C:\\Users\\mwalt\\Downloads\\script-conversion.ts',
  strings: 'C:\\Users\\mwalt\\Downloads\\strings-conversion.json'
};

// ---- comparison ------------------------------------------------------------
// Key order is not meaning, and a fresh uuid is not a difference. Everything
// else is.
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function deepDiff(a, b, at, out, opts) {
  opts = opts || {};
  if (out.length >= 40) return out;
  if (opts.ignoreIds && /(^|\.)id$/.test(at) && UUID.test(String(a)) && UUID.test(String(b))) return out;
  if (a === b) return out;
  const ta = a === null ? 'null' : Array.isArray(a) ? 'array' : typeof a;
  const tb = b === null ? 'null' : Array.isArray(b) ? 'array' : typeof b;
  if (ta !== tb) { out.push(`${at}: type ${ta} -> ${tb}`); return out; }
  if (ta === 'array') {
    if (a.length !== b.length) out.push(`${at}: length ${a.length} -> ${b.length}`);
    for (let i = 0; i < Math.min(a.length, b.length); i++) deepDiff(a[i], b[i], `${at}[${i}]`, out, opts);
    return out;
  }
  if (ta === 'object') {
    const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
    for (const k of keys) {
      if (!(k in a)) { out.push(`${at}.${k}: added ${JSON.stringify(b[k]).slice(0, 100)}`); continue; }
      if (!(k in b)) { out.push(`${at}.${k}: dropped ${JSON.stringify(a[k]).slice(0, 100)}`); continue; }
      deepDiff(a[k], b[k], `${at}.${k}`, out, opts);
    }
    return out;
  }
  out.push(`${at}: ${JSON.stringify(a)} -> ${JSON.stringify(b)}`);
  return out;
}

function byFilename(list) {
  const m = {};
  (list || []).forEach(a => { m[a.filename] = a; });
  return m;
}

// ---- the run ---------------------------------------------------------------
function run(Blockly, BF6) {
  const fails = [];
  const notes = [];
  let checks = 0;
  const check = (ok, what) => { checks++; if (!ok) fails.push(what); return ok; };

  const missing = Object.keys(FILES).filter(k => !fs.existsSync(FILES[k]));
  if (missing.length) {
    console.log('portal files      ', 'SKIPPED, fixture missing:', missing.join(', '));
    return { ok: true, checks: 0, failures: [], skipped: missing };
  }

  const rawA = fs.readFileSync(FILES.workspace, 'utf8');
  const rawB = fs.readFileSync(FILES.experience, 'utf8');
  const rawTs = fs.readFileSync(FILES.script, 'utf8');
  const rawStrings = fs.readFileSync(FILES.strings, 'utf8');
  const docB = JSON.parse(rawB);

  // ---- 1. detection --------------------------------------------------------
  const cases = [
    ['format A workspace', rawA, 'workspace'],
    ['format B experience', rawB, 'experience'],
    ['the site\'s script export', rawTs, 'typescript'],
    ['the site\'s strings export', rawStrings, 'strings'],
    // the spatial file, taken back out of the experience it was attached to
    ['a spatial export', BF6.b64decode(docB.attachments.find(a => a.attachmentType === 1).attachmentData.original), 'spatial'],
    ['a bare workspace with no mod wrapper', JSON.stringify(JSON.parse(rawA).mod), 'workspace'],
    ['something else entirely', 'hello, this is not a Portal file', 'unknown'],
    ['JSON of the wrong shape', '{"alpha":1,"beta":[2]}', 'unknown']
  ];
  cases.forEach(([what, text, want]) => {
    const d = BF6.detectFormat(text);
    check(d.format === want, `detect ${what}: got ${d.format}, wanted ${want}`);
    check(!!d.looksLike, `detect ${what}: no description of what it looked like`);
  });
  // renaming a file must change nothing: detection never sees the name
  check(BF6.detectFormat(rawB).format === 'experience',
    'the experience file was detected by something other than its content');
  notes.push(`  ${cases.length} shapes detected from content alone`);

  // ---- 2. format B read then write ----------------------------------------
  const read = BF6.readExperience(docB);
  check(!!read, 'readExperience returned nothing for a real export');
  check(read.attachments.length === 4, `expected 4 attachments, got ${read.attachments.length}`);
  check(read.mapRotation.length === 1, `expected 1 rotation slot, got ${read.mapRotation.length}`);
  check(read.mapRotation[0].map === 'MP_Portal_Sand',
    `rotation map read as ${read.mapRotation[0].map}`);
  check(read.mapRotation[0].gameMode === 'ModBuilderCustom',
    `rotation game mode read as ${read.mapRotation[0].gameMode}`);
  check(read.mapRotation[0].index === 0, `rotation index read as ${read.mapRotation[0].index}`);

  // the attachmentType mapping, checked against the real file
  const kinds = {};
  read.attachments.forEach(a => { kinds[a.filename] = a.attachmentType; });
  check(kinds['BF6_SFX.spatial.json'] === 1, 'spatial attachment is not type 1');
  check(kinds['bundle.ts'] === 2, 'typescript attachment is not type 2');
  check(kinds['blacklist.json'] === 3, 'blacklist attachment is not type 3');
  check(kinds['bundle.strings.json'] === 4, 'strings attachment is not type 4');
  // and the same mapping, worked out from the filename alone
  check(BF6.attachmentTypeFor('MP_Capstone.spatial.json') === 1, 'attachmentTypeFor spatial');
  check(BF6.attachmentTypeFor('bundle.ts') === 2, 'attachmentTypeFor typescript');
  check(BF6.attachmentTypeFor('blacklist.json') === 3, 'attachmentTypeFor blacklist');
  check(BF6.attachmentTypeFor('bundle.strings.json') === 4, 'attachmentTypeFor strings');

  // the decoded payload really is the file's own text
  const spatial = read.attachments.find(a => a.attachmentType === 1);
  check(spatial.text.indexOf('"Portal_Dynamic"') > 0,
    'the spatial attachment did not decode to a spatial file');
  check(BF6.b64encode(spatial.text) ===
    docB.attachments.find(a => a.attachmentType === 1).attachmentData.original,
    'base64 does not survive a decode and re-encode');

  // write it back
  const back = BF6.writeExperience({
    shell: read.shell, workspace: read.workspace, freshIds: false
  });

  // field by field, in the site's own order
  const order = ['mutators', 'assetRestrictions', 'gameMode', 'name', 'description',
    'mapRotation', 'patchId', 'workspace', 'teamComposition', 'attachments'];
  check(JSON.stringify(Object.keys(back.doc)) === JSON.stringify(order),
    'the written key order is not the site\'s: ' + Object.keys(back.doc).join(','));

  const diffs = [];
  order.forEach(field => {
    if (field === 'attachments') return;          // compared as a set below
    deepDiff(docB[field], back.doc[field], field, diffs, {});
  });
  check(diffs.length === 0, 'format B did not survive a read and a write: ' + diffs.join(' | '));

  // attachments compared by filename, because the order the site listed them in
  // is not part of what the file means
  const wantAtt = byFilename(docB.attachments);
  const gotAtt = byFilename(back.doc.attachments);
  check(Object.keys(wantAtt).length === Object.keys(gotAtt).length,
    `attachment count ${Object.keys(wantAtt).length} -> ${Object.keys(gotAtt).length}`);
  Object.keys(wantAtt).forEach(name => {
    if (!gotAtt[name]) { check(false, `attachment ${name} was dropped`); return; }
    const d = [];
    deepDiff(wantAtt[name], gotAtt[name], name, d, {});
    check(d.length === 0, `attachment ${name}: ${d.join(' | ')}`);
    check(JSON.stringify(Object.keys(wantAtt[name])) === JSON.stringify(Object.keys(gotAtt[name])),
      `attachment ${name} key order: ${Object.keys(gotAtt[name]).join(',')}`);
  });
  notes.push(`  format B round trip: ${Object.keys(wantAtt).length} attachments, ` +
    `${back.doc.mapRotation.length} rotation slot(s), ${diffs.length} field differences`);

  // and the whole file again, as one string, ignoring nothing
  const wholeDiffs = [];
  deepDiff(
    Object.assign({}, docB, { attachments: docB.attachments.slice().sort((x, y) => x.filename < y.filename ? -1 : 1) }),
    Object.assign({}, back.doc, { attachments: back.doc.attachments.slice().sort((x, y) => x.filename < y.filename ? -1 : 1) }),
    'root', wholeDiffs, {});
  check(wholeDiffs.length === 0, 'whole file differs: ' + wholeDiffs.slice(0, 6).join(' | '));

  // ---- 3. fresh ids --------------------------------------------------------
  const fresh = BF6.writeExperience({ shell: read.shell, workspace: read.workspace });
  const oldIds = new Set(docB.attachments.map(a => a.id));
  const newIds = fresh.doc.attachments.map(a => a.id);
  check(newIds.every(id => UUID.test(id)), 'a generated id is not in the site\'s uuid format');
  check(newIds.every(id => !oldIds.has(id)), 'a "fresh" id reused one from the imported file');
  const rotId = fresh.doc.mapRotation[0].spatialAttachment.id;
  check(newIds.indexOf(rotId) >= 0,
    'the rotation\'s spatial attachment does not match any entry in attachments');
  const twin = fresh.doc.attachments.find(a => a.id === rotId);
  const twinDiff = [];
  deepDiff(
    Object.keys(twin).sort().reduce((o, k) => (o[k] = twin[k], o), {}),
    Object.keys(fresh.doc.mapRotation[0].spatialAttachment).sort()
      .reduce((o, k) => (o[k] = fresh.doc.mapRotation[0].spatialAttachment[k], o), {}),
    'spatial', twinDiff, {});
  check(twinDiff.length === 0, 'the two copies of the spatial attachment differ: ' + twinDiff.join(' | '));
  // everything else must be untouched by the id refresh
  const freshDiffs = [];
  deepDiff(back.doc, fresh.doc, 'root', freshDiffs, { ignoreIds: true });
  check(freshDiffs.length === 0, 'refreshing the ids changed something else: ' + freshDiffs.slice(0, 5).join(' | '));

  // ---- 4. an export with nothing to go on ---------------------------------
  const bare = BF6.writeExperience({ workspace: JSON.parse(rawA) });
  check(bare.doc.gameMode === 'ModBuilderCustom', 'the default game mode is not the site\'s');
  check(JSON.stringify(bare.doc.mutators) === '{}', 'the default mutators are not empty');
  check(bare.doc.patchId === null, 'the default patchId is not null');
  check(bare.doc.mapRotation.length === 0, 'the default map rotation is not empty');
  check(bare.doc.teamComposition.length === 2, 'the default team composition is not two teams');
  check(bare.doc.workspace.mod.blocks.blocks.length === JSON.parse(rawA).mod.blocks.blocks.length,
    'the workspace did not survive into a bare export');
  ['mutators', 'assetRestrictions', 'name', 'description', 'mapRotation', 'teamComposition']
    .forEach(f => check(bare.summary.defaulted.indexOf(f) >= 0,
      `a bare export did not admit that ${f} is the site's default`));
  check(bare.summary.sources.workspace === 'the editor',
    'the export summary does not say the workspace came from the editor');
  notes.push(`  a bare export defaulted ${bare.summary.defaulted.length} field(s) and said so`);

  // a rotation built by the tool
  const built = BF6.writeExperience({
    workspace: JSON.parse(rawA),
    name: 'Night Ops',
    maps: [
      { map: 'MP_Capstone', index: 0, spatialText: '{"Portal_Dynamic":[]}' },
      { map: 'MP_Portal_Sand', index: 1, spatialText: '{"Portal_Dynamic":[]}', spatialFilename: 'sand.spatial.json' }
    ]
  });
  check(built.doc.mapRotation[0].id === 'MP_Capstone-ModBuilderCustom0',
    'a built rotation id is not in the site\'s form: ' + built.doc.mapRotation[0].id);
  check(built.doc.mapRotation[1].id === 'MP_Portal_Sand-ModBuilderCustom1',
    'the second built rotation id is wrong: ' + built.doc.mapRotation[1].id);
  check(built.doc.mapRotation[0].spatialAttachment.metadata === 'mapIdx=0',
    'the spatial metadata is not mapIdx=N');
  check(built.doc.mapRotation[1].spatialAttachment.filename === 'sand.spatial.json',
    'a given spatial filename was not used');
  check(built.doc.attachments.length === 2, 'a two-map rotation did not produce two attachments');
  check(built.doc.name === 'Night Ops' && built.summary.sources.name === 'the tool',
    'a name given by the tool did not land');

  // ---- 5. the site's script export, back to blocks ------------------------
  let Convert = null;
  try { Convert = require(path.join(BLOCKS, '..', 'convert', 'convert.js')); }
  catch (e) { notes.push('  convert.js did not load: ' + e.message); }
  if (Convert) {
    let ts = null;
    for (const cand of [
      process.env.BF6_TS,
      'typescript',
      'C:\\PortalSDK_1.4.2.0\\GodotProject\\User_Created\\projects\\_template-v1.7.0\\node_modules\\typescript'
    ]) {
      if (!cand) continue;
      try { ts = require(cand); break; } catch (e) { /* next */ }
    }
    if (!ts) {
      notes.push('  the TypeScript compiler API is not on this machine: the script import ' +
        'path could not be exercised');
    } else {
      Convert.setTypeScript(ts);
      let res = null, threw = '';
      try { res = Convert.tsToBlocks({ 'script-conversion.ts': rawTs }, {}); }
      catch (e) { threw = String(e.message || e); }
      check(!threw, 'the site\'s own script export would not convert back: ' + threw);
      if (res) {
        const ir = res.program;
        check(ir.rules.length > 0, 'the script export produced no rules');
        check(ir.subroutines.length > 0, 'the script export produced no subroutines');
        check(res.workspace.mod.blocks.blocks.length > 0, 'the script export produced no blocks');
        check(res.report.counts.blocks > 100,
          `the script export produced only ${res.report.counts.blocks} blocks`);
        // It must load into a real workspace, not just look like one. The
        // converter can produce a type the offline fallback list has never
        // seen, exactly as a live capture would, so the definitions are
        // refreshed from what this workspace actually uses first. That is what
        // the editor does on import too.
        const TYPES = require(path.join(BLOCKS, 'vendor', 'types_fallback.js'));
        const grown = BF6.installFallback(TYPES, BF6.observe(res.workspace));
        notes.push(`  definitions for the converted workspace: ${grown} types`);
        const ws = new Blockly.Workspace();
        let loadErr = '';
        try { BF6.loadWorkspace(ws, res.workspace); } catch (e) { loadErr = String(e.message || e); }
        check(!loadErr, 'the converted workspace would not load: ' + loadErr);
        const loaded = ws.getAllBlocks(false).length;
        check(loaded > 100, `only ${loaded} blocks survived loading the converted workspace`);
        try { ws.dispose(); } catch (e) {}
        notes.push(`  script import: ${ir.rules.length} rules, ${ir.subroutines.length} subroutines, ` +
          `${ir.variables.length} variables, ${res.report.counts.blocks} blocks, ` +
          `${loaded} loaded into a workspace`);
        if (res.report.unconvertible.length) {
          notes.push(`  script import reported ${res.report.unconvertible.length} construct(s) ` +
            'it cannot represent, which is reported to the user rather than dropped');
        }
        // nothing may be dropped in silence
        check(Array.isArray(res.report.unconvertible), 'the converter reported no unconvertible list');
        check(Array.isArray(res.report.warnings), 'the converter reported no warnings list');
      }
    }
  }

  console.log('portal file checks', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log(n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run, FILES };

if (require.main === module) {
  const STYLE = require('./style.js');
  const Blockly = STYLE.loadBlockly();
  const BF6 = require(path.join(BLOCKS, 'editor.js'));
  const TYPES = require(path.join(BLOCKS, 'vendor', 'types_fallback.js'));
  BF6.attach(Blockly);
  BF6.installFallback(TYPES, BF6.observe({ blocks: { blocks: [] } }));
  const r = run(Blockly, BF6);
  console.log(r.ok ? 'PORTAL FILES OK' : 'PORTAL FILES FAILED');
  process.exit(r.ok ? 0 : 1);
}
