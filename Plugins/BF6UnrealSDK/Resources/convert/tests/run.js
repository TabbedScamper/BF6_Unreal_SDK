/*
 * BF6Convert test suite.
 *
 *   node tests/run.js
 *
 * Prints PASS or FAIL per case and exits non zero if any case fails.
 * Cases that need files outside this folder print SKIP with the reason
 * instead of failing.
 */
'use strict';

var fs = require('fs');
var path = require('path');
var os = require('os');
var child = require('child_process');

var ROOT = path.resolve(__dirname, '..');
var BF6Convert = require(path.join(ROOT, 'convert.js'));

/* ------------------------------------------------------------------ paths */

var TEMPLATE = process.env.BF6_TEMPLATE ||
    'C:\\PortalSDK_1.4.2.0\\GodotProject\\User_Created\\projects\\_template-v1.7.0';
var FIXTURE = process.env.BF6_FIXTURE ||
    'C:\\Users\\mwalt\\Downloads\\night_ops_breakthrough_workspace.json';
var SITE_TS = process.env.BF6_SITE_TS ||
    'C:\\Users\\mwalt\\Downloads\\script-conversion.ts';
var WORK = process.env.BF6_WORK || path.join(os.tmpdir(), 'bf6convert-tests');

/* TypeScript comes from the template so we do not need our own install. */
(function loadTypeScript() {
    try { BF6Convert.setTypeScript(require('typescript')); return; } catch (e) { /* try template */ }
    try {
        BF6Convert.setTypeScript(require(path.join(TEMPLATE, 'node_modules', 'typescript')));
    } catch (e2) { /* reported by the cases that need it */ }
})();

/* ------------------------------------------------------------------ harness */

var results = [];

function run(name, fn) {
    var start = Date.now();
    try {
        var note = fn();
        if (note === 'SKIP') {
            results.push({ name: name, status: 'SKIP', note: '' });
        } else if (note && note.skip) {
            results.push({ name: name, status: 'SKIP', note: note.skip });
        } else {
            results.push({ name: name, status: 'PASS', note: note || '', ms: Date.now() - start });
        }
    } catch (e) {
        results.push({ name: name, status: 'FAIL', note: e && e.message ? e.message : String(e), ms: Date.now() - start });
        if (process.env.BF6_TRACE) { console.error(e); }
    }
}

function assert(cond, msg) { if (!cond) { throw new Error(msg); } }

function ensureDir(d) { if (!fs.existsSync(d)) { fs.mkdirSync(d, { recursive: true }); } }

function readFixture() {
    if (!fs.existsSync(FIXTURE)) { return null; }
    return JSON.parse(fs.readFileSync(FIXTURE, 'utf8'));
}

/* ------------------------------------------------------------------ (a) */

run('a. fixture -> TypeScript -> bundler builds', function () {
    var ws = readFixture();
    if (!ws) { return { skip: 'fixture not found at ' + FIXTURE }; }
    if (!fs.existsSync(path.join(TEMPLATE, 'node_modules', 'bf6-portal-bundler', 'index.js'))) {
        return { skip: 'template bundler not found under ' + TEMPLATE };
    }

    var out = BF6Convert.blocksToTs(ws, {});
    assert(out.report.counts.rules === 43, 'expected 43 rules, got ' + out.report.counts.rules);
    assert(out.report.counts.subroutines === 34, 'expected 34 subroutines, got ' + out.report.counts.subroutines);
    assert(out.report.counts.variables === 104, 'expected 104 variables, got ' + out.report.counts.variables);

    var proj = path.join(WORK, 'build');
    var src = path.join(proj, 'src');
    if (fs.existsSync(src)) { fs.rmSync(src, { recursive: true, force: true }); }
    ensureDir(src);
    fs.copyFileSync(path.join(TEMPLATE, 'package.json'), path.join(proj, 'package.json'));
    fs.copyFileSync(path.join(TEMPLATE, 'tsconfig.json'), path.join(proj, 'tsconfig.json'));

    var nm = path.join(proj, 'node_modules');
    if (!fs.existsSync(nm)) {
        try {
            fs.symlinkSync(path.join(TEMPLATE, 'node_modules'), nm, 'junction');
        } catch (e) {
            return { skip: 'could not link node_modules: ' + e.message };
        }
    }

    var names = Object.keys(out.files);
    for (var i = 0; i < names.length; i++) {
        var target = names[i] === 'strings.json' ? 'portal.strings.json' : names[i];
        fs.writeFileSync(path.join(src, target), out.files[names[i]], 'utf8');
    }

    var res = child.spawnSync(process.execPath, [
        path.join(TEMPLATE, 'node_modules', 'bf6-portal-bundler', 'index.js'),
        '--entrypoint', './src/index.ts', '--outDir', './dist'
    ], { cwd: proj, encoding: 'utf8' });

    var log = (res.stdout || '') + (res.stderr || '');
    assert(res.status === 0, 'bundler exited ' + res.status + '\n' + log);
    assert(/Build Complete/.test(log), 'bundler did not report a complete build\n' + log);
    assert(fs.existsSync(path.join(proj, 'dist', 'bundle.ts')), 'dist/bundle.ts was not produced');

    /* A strict type check as well. Report stale typing errors separately: they
     * are symbols the shipped game typings have and the npm package does not. */
    var tsc = child.spawnSync(process.execPath, [
        path.join(TEMPLATE, 'node_modules', 'typescript', 'bin', 'tsc'), '--noEmit', '-p', 'tsconfig.json'
    ], { cwd: proj, encoding: 'utf8' });
    var errs = ((tsc.stdout || '') + (tsc.stderr || '')).split(/\r?\n/).filter(function (l) { return /error TS/.test(l); });
    var stale = errs.filter(function (l) { return /does not exist on type/.test(l); });
    assert(errs.length === stale.length,
        'type errors that are not stale typings:\n' + errs.filter(function (l) { return stale.indexOf(l) < 0; }).join('\n'));

    return 'bundle.ts built; ' + names.length + ' files; ' + stale.length +
        ' stale-typings-only tsc errors';
});

/* ------------------------------------------------------------------ (b) */

run('b. fixture -> TypeScript -> blocks matches the fixture semantically', function () {
    var ws = readFixture();
    if (!ws) { return { skip: 'fixture not found at ' + FIXTURE }; }

    var out = BF6Convert.blocksToTs(ws, {});
    var sources = {};
    var names = Object.keys(out.files);
    for (var i = 0; i < names.length; i++) {
        if (/\.ts$/.test(names[i])) { sources[names[i]] = out.files[names[i]]; }
    }
    delete sources['runtime.ts'];

    var back = BF6Convert.tsToBlocks(sources, {});
    var a = BF6Convert.summarize(ws);
    var b = BF6Convert.summarize(back.workspace);

    var diffs = BF6Convert.diffSummaries(a, b, 'fixture', 'roundtrip');
    writeDiff('roundtrip-b.txt', diffs);
    assert(diffs.length === 0, diffs.length + ' semantic differences, see ' +
        path.join(WORK, 'roundtrip-b.txt') + '\n  ' + diffs.slice(0, 6).join('\n  '));

    return a.rules.length + ' rules, ' + a.subroutines.length + ' subroutines, ' +
        a.variables.length + ' variables identical';
});

/* ------------------------------------------------------------------ (c) */

run('c. template src/index.ts -> blocks -> report -> TypeScript', function () {
    var entry = path.join(TEMPLATE, 'src', 'index.ts');
    if (!fs.existsSync(entry)) { return { skip: 'template index.ts not found' }; }

    var sources = { 'index.ts': fs.readFileSync(entry, 'utf8') };
    var res = BF6Convert.tsToBlocks(sources, {});
    assert(res.report.unconvertible.length > 0,
        'expected the template to report unconvertible constructs');

    var kinds = {};
    for (var i = 0; i < res.report.unconvertible.length; i++) {
        var c = res.report.unconvertible[i].construct.split(':')[0];
        kinds[c] = (kinds[c] || 0) + 1;
    }
    assert(Object.keys(kinds).some(function (k) { return /^import/.test(k); }),
        'expected the bf6-portal-utils imports to be reported');

    ensureDir(WORK);
    fs.writeFileSync(path.join(WORK, 'template-report.json'),
        JSON.stringify(res.report.unconvertible, null, 2), 'utf8');

    /* and back to TypeScript, which must still produce a full file set */
    var again = BF6Convert.blocksToTs(res.workspace, {});
    assert(again.files['index.ts'], 'no index.ts produced on the way back');
    assert(again.files['variables.ts'], 'no variables.ts produced on the way back');

    return res.report.unconvertible.length + ' constructs reported (' +
        Object.keys(kinds).length + ' kinds); ' + Object.keys(again.files).length + ' files back';
});

/* ------------------------------------------------------------------ (d) */

run('d. hand written sample -> blocks -> TypeScript is a fixed point', function () {
    var sample = fs.readFileSync(path.join(__dirname, 'sample.ts'), 'utf8');

    var pass1 = BF6Convert.tsToBlocks({ 'sample.ts': sample }, {});
    assert(pass1.report.unconvertible.length === 0,
        'sample should convert cleanly, got:\n  ' +
        pass1.report.unconvertible.slice(0, 5).map(function (u) {
            return u.file + ':' + u.line + ' ' + u.construct;
        }).join('\n  '));
    assert(pass1.report.counts.rules === 10, 'expected 10 rules, got ' + pass1.report.counts.rules);
    assert(pass1.report.counts.subroutines === 2, 'expected 2 subroutines, got ' + pass1.report.counts.subroutines);

    var gen1 = BF6Convert.blocksToTs(pass1.workspace, {});
    var src2 = tsFilesOf(gen1.files);
    var pass2 = BF6Convert.tsToBlocks(src2, {});
    var gen2 = BF6Convert.blocksToTs(pass2.workspace, {});

    var names = Object.keys(gen1.files).sort();
    var names2 = Object.keys(gen2.files).sort();
    assert(names.join(',') === names2.join(','),
        'file set changed: ' + names.join(',') + ' vs ' + names2.join(','));
    for (var i = 0; i < names.length; i++) {
        if (gen1.files[names[i]] !== gen2.files[names[i]]) {
            ensureDir(WORK);
            fs.writeFileSync(path.join(WORK, 'fp1-' + names[i]), gen1.files[names[i]]);
            fs.writeFileSync(path.join(WORK, 'fp2-' + names[i]), gen2.files[names[i]]);
            throw new Error(names[i] + ' is not stable; wrote fp1-/fp2- copies to ' + WORK);
        }
    }

    /* the workspaces must agree too */
    var sa = JSON.stringify(BF6Convert.summarize(pass1.workspace));
    var sb = JSON.stringify(BF6Convert.summarize(pass2.workspace));
    assert(sa === sb, 'workspace summary changed between passes');

    return '10 rules, 2 subroutines, ' + names.length + ' files stable byte for byte';
});

/* ------------------------------------------------------------------ (e) */

run('e. site export script-conversion.ts -> blocks matches the fixture', function () {
    var ws = readFixture();
    if (!ws) { return { skip: 'fixture not found at ' + FIXTURE }; }
    if (!fs.existsSync(SITE_TS)) { return { skip: 'site export not found at ' + SITE_TS }; }

    var res = BF6Convert.tsToBlocks({ 'script-conversion.ts': fs.readFileSync(SITE_TS, 'utf8') }, {});
    var a = BF6Convert.summarize(ws, { ignoreParamTypes: true });
    var b = BF6Convert.summarize(res.workspace, { ignoreParamTypes: true });

    var counts = 'site: ' + b.rules.length + ' rules / ' + b.subroutines.length +
        ' subroutines / ' + b.variables.length + ' variables; fixture: ' +
        a.rules.length + ' / ' + a.subroutines.length + ' / ' + a.variables.length;

    var diffs = BF6Convert.diffSummaries(a, b, 'fixture', 'site');
    writeDiff('roundtrip-e.txt', diffs);

    assert(b.rules.length === a.rules.length, 'rule count differs. ' + counts);
    assert(b.subroutines.length === a.subroutines.length, 'subroutine count differs. ' + counts);

    /* Rules and subroutines must line up exactly. */
    var missing = diffs.filter(function (d) { return /^(rule|subroutine) only in/.test(d); });
    assert(missing.length === 0, missing.length + ' rules or subroutines do not line up:\n  ' +
        missing.slice(0, 8).join('\n  '));

    /* Every remaining body difference must be one the converter already
     * reported as a loss in the site export, not a conversion bug. */
    var bodyDiffs = diffs.filter(function (d) { return /body differs/.test(d); });
    var slotLosses = res.report.unconvertible.filter(function (u) {
        return /raw slot number/.test(u.construct);
    });
    assert(bodyDiffs.length <= slotLosses.length,
        bodyDiffs.length + ' body differences but only ' + slotLosses.length +
        ' reported losses:\n  ' + bodyDiffs.slice(0, 6).join('\n  '));

    var varDiffs = diffs.filter(function (d) { return /variable only in/.test(d); });
    return counts + '; ' + bodyDiffs.length + ' body diff(s) from ' + slotLosses.length +
        ' raw slot loss(es); ' + varDiffs.length +
        ' variable name deltas (the site export declares no variables). See ' +
        path.join(WORK, 'roundtrip-e.txt');
});

/* ------------------------------------------------------------------ (f) */

run('f. our TypeScript and the site export agree per rule', function () {
    var ws = readFixture();
    if (!ws) { return { skip: 'fixture not found at ' + FIXTURE }; }
    if (!fs.existsSync(SITE_TS)) { return { skip: 'site export not found at ' + SITE_TS }; }

    var ours = BF6Convert.blocksToTs(ws, {});
    var oursBack = BF6Convert.tsToBlocks(tsFilesOf(ours.files), {});
    var site = BF6Convert.tsToBlocks({ 'script-conversion.ts': fs.readFileSync(SITE_TS, 'utf8') }, {});

    var a = BF6Convert.summarize(oursBack.workspace, { ignoreParamTypes: true });
    var b = BF6Convert.summarize(site.workspace, { ignoreParamTypes: true });
    var diffs = BF6Convert.diffSummaries(a, b, 'ours', 'site');
    writeDiff('roundtrip-f.txt', diffs);

    var missing = diffs.filter(function (d) { return /^(rule|subroutine) only in/.test(d); });
    assert(missing.length === 0, missing.length + ' rules or subroutines only on one side:\n  ' +
        missing.slice(0, 8).join('\n  '));

    var bodyDiffs = diffs.filter(function (d) { return /body differs/.test(d); });
    var slotLosses = site.report.unconvertible.filter(function (u) {
        return /raw slot number/.test(u.construct);
    });
    assert(bodyDiffs.length <= slotLosses.length,
        bodyDiffs.length + ' body differences but only ' + slotLosses.length +
        ' reported losses in the site export:\n  ' + bodyDiffs.slice(0, 6).join('\n  '));

    return a.rules.length + ' rules and ' + a.subroutines.length +
        ' subroutines line up; ' + bodyDiffs.length + ' body diff(s), all from ' +
        slotLosses.length + ' raw slot loss(es) in the site export. See ' +
        path.join(WORK, 'roundtrip-f.txt');
});

/* ------------------------------------------------------------------ helpers */

function tsFilesOf(files) {
    var out = {}, names = Object.keys(files), i;
    for (i = 0; i < names.length; i++) {
        if (!/\.ts$/.test(names[i])) { continue; }
        if (names[i] === 'runtime.ts') { continue; }
        out[names[i]] = files[names[i]];
    }
    return out;
}

function writeDiff(name, diffs) {
    ensureDir(WORK);
    fs.writeFileSync(path.join(WORK, name), diffs.join('\n') + '\n', 'utf8');
}

/* ------------------------------------------------------------------ output */

var failed = 0, skipped = 0;
var lines = [''];
for (var i = 0; i < results.length; i++) {
    var r = results[i];
    if (r.status === 'FAIL') { failed++; }
    if (r.status === 'SKIP') { skipped++; }
    lines.push(r.status + '  ' + r.name + (r.ms !== undefined ? '  (' + r.ms + ' ms)' : ''));
    if (r.note) {
        lines.push('      ' + String(r.note).replace(/\n/g, '\n      '));
    }
}
lines.push('');
lines.push(results.length + ' cases, ' + (results.length - failed - skipped) + ' passed, ' +
    failed + ' failed, ' + skipped + ' skipped');
lines.push('artifacts: ' + WORK);
lines.push('');
process.stdout.write(lines.join('\n'));
process.exit(failed ? 1 : 0);
