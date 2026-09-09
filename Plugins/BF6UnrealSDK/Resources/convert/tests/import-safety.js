// CW-07 acceptance: import twice over a handwritten project, force a backup
// failure, and confirm nothing manual is silently replaced.
var fs = require('fs');
var path = require('path');
var os = require('os');
var cp = require('child_process');

var CLI = process.argv[2];
var root = fs.mkdtempSync(path.join(os.tmpdir(), 'bf6imp-'));
var proj = path.join(root, 'proj');
fs.mkdirSync(path.join(proj, 'src'), { recursive: true });

// A minimal workspace the converter will accept.
var ws = { mod: { blocks: { languageVersion: 0, blocks: [
    { type: 'modBlock', id: 'm1', x: 10, y: 10, deletable: false, inputs: { RULES: { block: {
        type: 'ruleBlock', id: 'r1',
        extraState: { isOngoingEvent: true },
        fields: { NAME: 'Rule One', EVENTTYPE: 'Ongoing', OBJECTTYPE: 'Global' } } } } }
] } } };
var wsFile = path.join(root, 'ws.json');
fs.writeFileSync(wsFile, JSON.stringify(ws));

function run() {
    var r = cp.spawnSync(process.execPath, [CLI, 'blocks2template', wsFile, proj], { encoding: 'utf8' });
    var line = (r.stdout || '').split('\n').filter(function (l) { return l.indexOf('BF6CONVERT ') === 0; })[0];
    return { code: r.status, json: line ? JSON.parse(line.slice(11)) : null, err: r.stderr || '' };
}

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

// The author's own index.ts, which must survive everything below.
var HAND = '// my own work\nexport function mine() { return 1; }\n';
var idx = path.join(proj, 'src', 'index.ts');
fs.writeFileSync(idx, HAND);

console.log('first import over a handwritten file:');
var a = run();
check('import succeeded', a.code === 0 && a.json && a.json.ok);
check('handwritten file kept as .orig', fs.existsSync(idx + '.orig'));
check('.orig holds the ORIGINAL work', fs.readFileSync(idx + '.orig', 'utf8') === HAND);
check('a generated manifest was written', fs.existsSync(path.join(proj, '.bf6-generated.json')));

console.log('');
console.log('second import (the rotation bug):');
var afterFirst = fs.readFileSync(idx, 'utf8');
var b = run();
check('import succeeded', b.code === 0 && b.json && b.json.ok);
check('.orig STILL holds the original, not the first generated file',
      fs.readFileSync(idx + '.orig', 'utf8') === HAND);
check('the generated file was replaced without a new backup',
      !b.json.kept || b.json.kept.indexOf('src/index.ts') < 0);

console.log('');
console.log('a hand edit made AFTER an import, with .orig still there:');
// THE CASE THIS SUITE USED TO DELETE ITS WAY PAST. Removing .orig before the
// hand-edit test skipped the normal state of a project somebody keeps working
// in - and in that state the edit was replaced with nothing kept anywhere.
var LATER = fs.readFileSync(idx, 'utf8') + '\n// a later hand edit that must survive\n';
fs.writeFileSync(idx, LATER);
var later = run();
check('import succeeded', later.code === 0 && later.json && later.json.ok);
check('the first .orig still holds the ORIGINAL work',
      fs.readFileSync(idx + '.orig', 'utf8') === HAND);
var survives = fs.readdirSync(path.join(proj, 'src')).filter(function (f) {
    return /^index\.ts\.orig/.test(f)
        && /a later hand edit that must survive/.test(
            fs.readFileSync(path.join(proj, 'src', f), 'utf8'));
});
check('the later edit is kept in its own snapshot', survives.length === 1);
check('and the import says it kept something',
      !!later.json.kept && later.json.kept.length > 0);
var again = run();
check('importing again does not pile up duplicate copies of the same text',
      fs.readdirSync(path.join(proj, 'src')).filter(function (f) {
          return /^index\.ts\.orig/.test(f);
      }).length === 2);

console.log('');
console.log('a hand edit to a generated file is protected:');
fs.writeFileSync(idx, afterFirst + '\n// I changed this by hand\n');
fs.unlinkSync(idx + '.orig');
var c = run();
check('import succeeded', c.code === 0 && c.json && c.json.ok);
check('the hand-edited file was backed up again', fs.existsSync(idx + '.orig'));
check('.orig holds the hand-edited version',
      /I changed this by hand/.test(fs.readFileSync(idx + '.orig', 'utf8')));

console.log('');
console.log('backup failure must stop the import:');
fs.writeFileSync(idx, HAND);
if (fs.existsSync(idx + '.orig')) { fs.unlinkSync(idx + '.orig'); }
// A directory where the .orig must go makes copyFileSync fail.
fs.mkdirSync(idx + '.orig');
var before = fs.readFileSync(idx, 'utf8');
var d = run();
check('the import refused', d.code !== 0);
check('the handwritten file is untouched', fs.readFileSync(idx, 'utf8') === before);
check('it said why', /could not keep a copy|cannot be backed up|nothing was imported/.test(d.err));

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
try { fs.rmSync(root, { recursive: true, force: true }); } catch (e) {}
process.exit(fail ? 1 : 0);
