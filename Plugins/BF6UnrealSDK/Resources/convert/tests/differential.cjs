#!/usr/bin/env node
/*
 * DIFFERENTIAL TEST: does a workspace still DO what the script did?
 *
 *   node tests/differential.cjs <projectDir> [--level <name>] [-v]
 *
 * Everything else in this folder checks SHAPE - that a round trip produces the
 * same rules, the same names, the same text. None of it can tell you whether the
 * blocks behave. A compiler that turns `x -= 1` into `x += 1` passes every one
 * of those tests.
 *
 * So this runs the mod twice on the SDK simulator and compares what it DID:
 *
 *   A   the project's own TypeScript
 *   B   the same TypeScript after a round trip through the converter
 *       (tsToBlocks -> a real block workspace -> blocksToTs)
 *
 * and diffs the ordered list of mod.* calls each one made. B is the thing the
 * block editor would hold, so a difference here is a compiler bug, not a
 * simulator quirk.
 *
 * modsim cannot run a block workspace - it has no block interpreter, and its
 * LoadLevel takes a loaded JS module whose exports it looks up by event name.
 * blocksToTs emits exactly that shape, so the workspace is executed by turning
 * it back into TypeScript. The blocks are still the thing under test: every
 * lowering, every pooled slot and every packed array is baked into them by then.
 *
 * NOTHING IS WRITTEN INSIDE THE SDK. It is read only; all output goes to the
 * system temp folder.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const { spawnSync } = require('child_process');

const CONV = path.resolve(__dirname, '..');
const BF6Convert = require(path.join(CONV, 'convert.js'));

/* ---- where things are -------------------------------------------------- */
/* An SDK is only usable here if it has BOTH the simulator and a TypeScript to
 * compile with. Several versions sit side by side on this machine and the older
 * ones keep modsim while their template has never had npm install run, so
 * taking the first match found an SDK that could not compile anything.
 * Newest first, and both halves checked. */
function findSdk() {
    const found = [];
    for (const drive of ['C:', 'D:', 'E:', 'F:']) {
        if (!fs.existsSync(drive + '/')) continue;
        for (const name of fs.readdirSync(drive + '/')) {
            if (!/^PortalSDK/i.test(name)) continue;
            const root = path.join(drive + '/', name);
            if (!fs.existsSync(path.join(root, 'unsupported', 'modsim', 'modsim.ts'))) continue;
            if (!templateTsc(root)) continue;
            found.push(root);
        }
    }
    found.sort();
    return found.length ? found[found.length - 1] : null;
}

/* The tsc that ships inside whichever template the SDK carries. */
function templateTsc(root) {
    const projects = path.join(root, 'GodotProject', 'User_Created', 'projects');
    if (!fs.existsSync(projects)) return null;
    const templates = fs.readdirSync(projects).filter((d) => /^_template/.test(d)).sort();
    for (let i = templates.length - 1; i >= 0; i--) {
        const lib = path.join(projects, templates[i], 'node_modules', 'typescript');
        if (fs.existsSync(path.join(lib, 'bin', 'tsc'))) return lib;
    }
    return null;
}

const argv = process.argv.slice(2);
const VERBOSE = argv.includes('-v');
const levelAt = argv.indexOf('--level');
const LEVEL = levelAt >= 0 ? argv[levelAt + 1] : null;
const PROJ = argv.find((a) => !a.startsWith('-') && a !== LEVEL);
if (!PROJ) {
    console.error('usage: node tests/differential.cjs <projectDir> [--level <name>] [-v]');
    process.exit(2);
}

const SDK = findSdk();
if (!SDK) { console.error('no PortalSDK with unsupported/modsim found'); process.exit(2); }
const MODSIM_DIR = path.join(SDK, 'unsupported', 'modsim');
const TS_LIB = templateTsc(SDK);
const TSC = path.join(TS_LIB, 'bin', 'tsc');
const OUT = path.join(os.tmpdir(), 'bf6-differential');

function rmrf(p) { try { fs.rmSync(p, { recursive: true, force: true }); } catch (e) {} }
function mkdirp(p) { fs.mkdirSync(p, { recursive: true }); }

/* ---- compiling ---------------------------------------------------------- */
const TSC_FLAGS = ['--noCheck', '--skipLibCheck', '--module', 'commonjs', '--target', 'es2022',
    '--moduleResolution', 'node', '--esModuleInterop'];

function tsc(args, label) {
    const r = spawnSync(process.execPath, [TSC, ...args], { encoding: 'utf-8' });
    if (r.status !== 0) {
        console.error('[build] tsc failed for ' + label + ':');
        console.error((r.stdout || '').split('\n').slice(0, 14).join('\n'));
        console.error(r.stderr || '');
        process.exit(1);
    }
}

/* The converter writes `import ... from './runtime.ts'`, which the bundler
 * resolves and tsc refuses. Only the specifier is touched; nothing else about
 * the generated code is edited, so what runs is what the block editor would
 * export. */
function stripTsExtensions(dir) {
    for (const f of fs.readdirSync(dir)) {
        if (!f.endsWith('.ts')) continue;
        const p = path.join(dir, f);
        const src = fs.readFileSync(p, 'utf8');
        const out = src.replace(/(from\s+['"]\.\/[^'"]+)\.ts(['"])/g, '$1$2');
        if (out !== src) fs.writeFileSync(p, out);
    }
}

function compileDir(srcDir, outDir, label) {
    mkdirp(outDir);
    const entries = fs.readdirSync(srcDir).filter((f) => f.endsWith('.ts'))
        .map((f) => path.join(srcDir, f));
    if (!entries.length) { console.error('[build] nothing to compile in ' + srcDir); process.exit(1); }
    tsc([...TSC_FLAGS, '--rootDir', srcDir, '--outDir', outDir, ...entries], label);
    return outDir;
}

/* ---- what counts as behaviour ------------------------------------------- */
/* A SCRIPT HAS VARIABLES AND OPERATORS. A BLOCK WORKSPACE DOES NOT.
 *
 * TypeScript keeps a local in a JS variable and adds with `+`, so neither is
 * visible to this recorder. Portal has no locals and no operators: every read,
 * every write and every `!` is a mod call. So the converted mod always makes
 * strictly more calls than the original, and a flat comparison reports a
 * difference on the first variable it stores - which says nothing about whether
 * the mod behaves the same.
 *
 * These names were not chosen up front. They are what a vocabulary diff of the
 * two traces on TDM showed appearing ONLY on the converted side: value plumbing
 * and pure operators, none of which touch the game. Everything else - anything
 * that moves a player, scores a point or draws a widget - is still compared, so
 * work lost in conversion still fails the run.
 */
const PLUMBING = {
    SetVariable: 1, GetVariable: 1, SetVariableAtIndex: 1,
    /* Addressing, not an effect. ObjectVariable(obj, slot) just names which
     * slot on which object is meant; a collection kept per player calls it
     * once per read and once per write, and the original keeps the same state
     * in a JS Map that this recorder cannot see at all. */
    ObjectVariable: 1,
    AppendToArray: 1, ValueInArray: 1, EmptyArray: 1, CountOf: 1,
    RemoveFromArray: 1, FirstOf: 1, LastOf: 1, IndexOfArrayValue: 1,
    Not: 1, And: 1, Or: 1, Equals: 1, NotEqualTo: 1,
    GreaterThan: 1, GreaterThanEqualTo: 1, LessThan: 1, LessThanEqualTo: 1,
    Add: 1, Subtract: 1, Multiply: 1, Divide: 1, Modulo: 1,
    IfThenElse: 1,
    /* Arithmetic, for the same reason - and one more. Portal has no lazy
     * operators: IfThenElse is a value block, so BOTH arms are computed before
     * the choice is made. `d === 0 ? k : Math.round(k / d * 100) / 100` runs the
     * division on the converted side even when d is 0, so a RoundToInteger(NaN)
     * appears that the original never performed. The chosen result was already
     * identical; only the discarded arm showed up. */
    Floor: 1, Ceiling: 1, RoundToInteger: 1, AbsoluteValue: 1,
    SquareRoot: 1, RaiseToPower: 1, Max: 1, RandomReal: 1
};

function isPlumbing(line) {
    const m = /^!?(\w+)\(/.exec(line);
    return !!(m && PLUMBING[m[1]]);
}

/* ---- value blocks modsim does not implement ------------------------------ */
/* These compute a value and touch nothing. modsim covers most of them; the ones
 * it misses fell to the no-op shim and returned undefined, which then travelled
 * into a real call - `IfThenElse(true, 0, NaN)` handing back nothing turned a
 * correct scoreboard update into SetScoreboardPlayerValues(p, 0, 0, 0, undef)
 * and looked like the converter had lost the value. Arithmetic is arithmetic,
 * so it is filled in here rather than left to report a false difference.
 *
 * Only pure functions belong in this table. Anything that changes game state
 * must stay a recorded no-op, so that a call the converter dropped still shows
 * up as a missing line rather than being quietly satisfied here. */
const PURE = {
    IfThenElse: (c, a, b) => (c ? a : b),
    Not: (a) => !a,
    And: (a, b) => !!(a && b),
    Or: (a, b) => !!(a || b),
    Equals: (a, b) => a === b,
    NotEqualTo: (a, b) => a !== b,
    GreaterThan: (a, b) => a > b,
    GreaterThanEqualTo: (a, b) => a >= b,
    LessThan: (a, b) => a < b,
    LessThanEqualTo: (a, b) => a <= b,
    Add: (a, b) => a + b,
    Subtract: (a, b) => a - b,
    Multiply: (a, b) => a * b,
    Divide: (a, b) => a / b,
    Modulo: (a, b) => a % b,
    Max: (a, b) => Math.max(a, b),
    Floor: (a) => Math.floor(a),
    Ceiling: (a) => Math.ceil(a),
    RoundToInteger: (a) => Math.round(a),
    AbsoluteValue: (a) => Math.abs(a),
    SquareRoot: (a) => Math.sqrt(a),
    RaiseToPower: (a, b) => Math.pow(a, b)
};

/* ---- the recorder ------------------------------------------------------- */
/* One shim serves BOTH runs. A function modsim does not implement is then
 * missing on both sides, so it shows up as a no-op in each trace rather than as
 * a difference between them. The mod global has to be a Proxy over modsim
 * rather than a copy, because modsim reassigns some of its own exports. */
function makeRecorder(modsim) {
    const trace = [];
    let recording = false;
    const shim = new Proxy(modsim, {
        get(target, prop) {
            const v = target[prop];
            /* A NAME MODSIM DOES NOT HAVE IS A NO-OP, NOT A CRASH.
             *
             * modsim implements a subset: the original TDM died on its second
             * call at mod.SetRedeployTime, which simply is not there. Letting
             * that throw ends the run and compares two lines of trace. Missing
             * functions have to behave the same on BOTH sides, so an unknown
             * name becomes a recorded no-op and the comparison keeps going.
             * Real non-function exports (uiRoot, and the string table modsim
             * reassigns) still pass straight through. */
            /* WRITING ONE SLOT OF AN ARRAY IS NOT OPTIONAL.
             *
             * modsim has SetVariable and GetVariable but no SetVariableAtIndex,
             * so it fell to the no-op above and every `stats[id] = ...` in the
             * converted mod silently did nothing - then the read a line later
             * threw, and the failure looked like a converter bug. modsim lives
             * under the read only SDK, so it is filled in here instead, over
             * modsim's own variable store and array type. */
            if (prop === 'SetVariableAtIndex' && !(prop in target)) {
                return function (variable, index, value) {
                    if (recording) trace.push('SetVariableAtIndex(' + summarise([variable, index, value]) + ')');
                    let arr = target.GetVariable(variable);
                    if (!arr || !Array.isArray(arr.array)) { arr = target.EmptyArray(); }
                    while (arr.array.length <= index) { arr.array.push(undefined); }
                    arr.array[index] = value;
                    target.SetVariable(variable, arr);
                    return undefined;
                };
            }
            if (Object.prototype.hasOwnProperty.call(PURE, prop) && !(prop in target)) {
                return function (...args) {
                    const out = PURE[prop](...args);
                    if (recording) trace.push(String(prop) + '(' + summarise(args) + ')');
                    return out;
                };
            }
            if (v === undefined && !(prop in target)) {
                return function (...args) {
                    if (recording) trace.push(String(prop) + '(' + summarise(args) + ') [not simulated]');
                    return undefined;
                };
            }
            if (typeof v !== 'function') return v;
            return function (...args) {
                if (recording) trace.push(String(prop) + '(' + summarise(args) + ')');
                try { return v.apply(target, args); }
                catch (e) {
                    if (recording) trace.push('!' + String(prop) + ' threw ' + shortMsg(e));
                    return undefined;
                }
            };
        }
    });
    return {
        mod: shim,
        trace,
        start() { recording = true; },
        stop() { recording = false; }
    };
}

function shortMsg(e) { return String((e && e.message) || e).slice(0, 60); }

/* Arguments are summarised, not serialised. Two runs allocate different object
 * identities for the same player, so comparing them by reference would report a
 * difference on every call; what has to match is the SHAPE of the argument and
 * any primitive in it. */
function summarise(args) {
    return args.map(one).join(',');
}
function one(a) {
    if (a === null) return 'null';
    if (a === undefined) return 'undef';
    const t = typeof a;
    if (t === 'number') return Number.isInteger(a) ? String(a) : a.toFixed(3);
    if (t === 'string') return JSON.stringify(a.length > 24 ? a.slice(0, 24) + '~' : a);
    if (t === 'boolean') return a ? 'true' : 'false';
    if (Array.isArray(a)) return '[' + a.length + ']';
    if (t === 'object') {
        if (a.id !== undefined) return (a.constructor && a.constructor.name || 'obj') + '#' + a.id;
        if (a.type !== undefined) return 'obj:' + a.type;
        return 'obj';
    }
    return t;
}

/* ---- one run ------------------------------------------------------------ */
function runOnce(label, moduleDir, spatial, stringsPath) {
    /* modsim is required fresh for each run: it keeps the whole world in module
     * level state, so a second run against the same instance would start from
     * wherever the first one stopped. */
    for (const k of Object.keys(require.cache)) {
        if (k.indexOf('bf6-differential') >= 0) delete require.cache[k];
    }
    const modsim = require(path.join(OUT, 'modsim', 'modsim.js'));
    const rec = makeRecorder(modsim);
    globalThis.mod = rec.mod;

    if (stringsPath && fs.existsSync(stringsPath)) {
        try { modsim.SetStrings(JSON.parse(fs.readFileSync(stringsPath, 'utf8'))); } catch (e) {}
    }

    const entry = path.join(moduleDir, 'index.js');
    if (!fs.existsSync(entry)) return { label, error: 'no index.js in ' + moduleDir };
    let bundle;
    try { bundle = require(entry); }
    catch (e) { return { label, error: 'load failed: ' + shortMsg(e) }; }

    const handlers = Object.keys(bundle).filter((k) => typeof bundle[k] === 'function');

    try { modsim.LoadLevel(bundle, spatial); } catch (e) {
        return { label, error: 'LoadLevel failed: ' + shortMsg(e) };
    }

    /* THE SCENARIO IS RUN IN PHASES, AND COMPARED PHASE BY PHASE.
     *
     * A flat stream cannot line up. The converted mod has a round start phase
     * the original does not: a workspace has no module load, so every
     * `const X = [...]` becomes assignments inside an OnGameModeStarted rule,
     * and the packed variable arrays are built there too. Diffing one long
     * trace reports a difference at call 1 and tells you nothing.
     *
     * Per phase, the question is the useful one: given the same event, did the
     * two mods do the same things? Setup being different is expected and is
     * reported rather than failed. */
    const phases = {};
    const raw = {};
    function phase(name, fn) {
        rec.trace.length = 0;
        rec.start();
        try { fn(); } catch (e) { rec.trace.push('!threw ' + shortMsg(e)); }
        rec.stop();
        /* Both are kept: the game-effect trace is what decides the run, the raw
         * one is there for -v when the plumbing itself is the suspect. */
        raw[name] = rec.trace.slice();
        phases[name] = rec.trace.filter(function (t) { return !isPlumbing(t); });
    }

    /* START COMES FIRST BECAUSE START IS WHERE MODULE LOAD WENT.
     *
     * The original runs `const playersStats = {}` when the file is imported,
     * before any event. A block workspace has no module load phase and Portal
     * variables carry no initial value - the exported workspace format has only
     * name, id and type - so the converter has one place to put that work: the
     * round start rule. Running join before start therefore asked the converted
     * mod to use state its only initialiser had not reached yet, and reported a
     * conversion failure for something no block workspace can express.
     */
    const players = [];
    phase('start', () => modsim.StartGameMode());
    phase('join', () => { for (let i = 0; i < 2; i++) players.push(modsim.AddPlayer()); });
    phase('deploy', () => { for (const p of players) modsim.DeployPlayer(p); });
    phase('kill', () => { if (players.length > 1) modsim.KillPlayer(players[0], players[1]); });

    return { label, phases, raw, handlers };
}

/* ---- main --------------------------------------------------------------- */
function main() {
    rmrf(OUT);
    mkdirp(OUT);

    const srcDir = path.join(PROJ, 'src');
    if (!fs.existsSync(srcDir)) { console.error('no src/ in ' + PROJ); process.exit(2); }

    /* modsim, once, shared by both runs */
    console.log('[build] modsim');
    const modsimSrcs = [
        path.join(MODSIM_DIR, 'modsim.ts'),
        path.join(MODSIM_DIR, 'enums', 'index.ts'),
        path.join(MODSIM_DIR, 'enums', 'audio.ts'),
        path.join(MODSIM_DIR, 'enums', 'weapons.ts'),
        path.join(MODSIM_DIR, 'enums', 'runtime-spawn.ts')
    ];
    tsc([...TSC_FLAGS, '--rootDir', MODSIM_DIR, '--outDir', path.join(OUT, 'modsim'), ...modsimSrcs], 'modsim');

    /* A: the project as written */
    console.log('[build] A: the original TypeScript');
    const aSrc = path.join(OUT, 'a-src');
    mkdirp(aSrc);
    for (const f of fs.readdirSync(srcDir)) {
        if (f.endsWith('.ts')) fs.copyFileSync(path.join(srcDir, f), path.join(aSrc, f));
    }
    stripTsExtensions(aSrc);
    compileDir(aSrc, path.join(OUT, 'a'), 'A');

    /* B: the same thing after a round trip through blocks */
    console.log('[build] B: through the converter');
    BF6Convert.setTypeScript(require(TS_LIB));
    try { BF6Convert.setCatalog(JSON.parse(fs.readFileSync(path.join(CONV, 'catalog.json'), 'utf8'))); } catch (e) {}
    try { BF6Convert.setEvents(JSON.parse(fs.readFileSync(path.join(CONV, 'events.json'), 'utf8'))); } catch (e) {}

    const sources = {};
    (function walk(d, base) {
        for (const e of fs.readdirSync(d, { withFileTypes: true })) {
            const p = path.join(d, e.name);
            if (e.isDirectory()) { walk(p, base); continue; }
            if (e.name.endsWith('.ts')) sources[path.relative(base, p).split(path.sep).join('/')] = fs.readFileSync(p, 'utf8');
        }
    })(srcDir, srcDir);

    const toBlocks = BF6Convert.tsToBlocks(sources, {});
    const back = BF6Convert.blocksToTs(toBlocks.workspace, {});
    const bSrc = path.join(OUT, 'b-src');
    mkdirp(bSrc);
    for (const name of Object.keys(back.files)) {
        fs.writeFileSync(path.join(bSrc, path.basename(name)), back.files[name]);
    }
    stripTsExtensions(bSrc);
    compileDir(bSrc, path.join(OUT, 'b'), 'B');

    /* the level */
    const levels = path.join(SDK, 'export', 'levels');
    let levelFile = LEVEL ? path.join(levels, LEVEL) : null;
    if (!levelFile || !fs.existsSync(levelFile)) {
        const guess = fs.readdirSync(levels).filter((f) => f.endsWith('.spatial.json'));
        const named = guess.find((f) => f.toLowerCase().indexOf(path.basename(PROJ).toLowerCase().split('-')[0]) >= 0);
        levelFile = path.join(levels, named || guess[0]);
    }
    console.log('[level] ' + path.basename(levelFile));
    const spatial = JSON.parse(fs.readFileSync(levelFile, 'utf8'));

    const stringsA = path.join(PROJ, 'dist', 'bundle.strings.json');

    const a = runOnce('A original', path.join(OUT, 'a'), spatial, stringsA);
    /* BOTH SIDES GET THE MOD OWN MESSAGE TABLE.
     *
     * The converter also writes a strings.json, and it is a different thing
     * with the same name: Portal interns every string token a script mentions
     * as s0..sN, so that file is { s0: "BOT_T1_01", ... }. Handing it to
     * SetStrings, which wants { SCOREBOARD_HEADER: "{} KILLS" }, left every
     * mod.stringkeys.X undefined and made Message throw on the converted side
     * only. The localisation table belongs to the mod and conversion does not
     * rewrite it, so A and B are given the same one. */
    const b = runOnce('B round trip', path.join(OUT, 'b'), spatial, stringsA);

    report(a, b, toBlocks.report, back.report);
}

function report(a, b, toB, toT) {
    console.log('');
    console.log('conversion: ' + toB.counts.rules + ' rules, ' + toB.counts.subroutines +
        ' subroutines, ' + toB.counts.variables + ' variables, ' +
        (toB.unconvertible || []).length + ' unconvertible');
    console.log('');
    for (const r of [a, b]) {
        if (r.error) { console.log(r.label + ': ERROR ' + r.error); }
        else {
            const total = Object.keys(r.phases).reduce((n, k) => n + r.phases[k].length, 0);
            console.log(r.label + ': ' + r.handlers.length + ' handler(s), ' + total + ' mod call(s)');
        }
    }
    if (a.error || b.error) { console.log(''); console.log('DIFFERENTIAL INCONCLUSIVE'); process.exit(1); }
    console.log('  (handler counts differ legitimately: the original exports its own named');
    console.log('   functions, the converted mod exports one entry point per EVENT)');
    var plumbA = 0, plumbB = 0;
    for (const k of Object.keys(a.raw || {})) { plumbA += a.raw[k].length - a.phases[k].length; }
    for (const k of Object.keys(b.raw || {})) { plumbB += b.raw[k].length - b.phases[k].length; }
    console.log('  (counts are GAME EFFECTS: ' + plumbA + ' variable and operator calls on A, ' +
        plumbB + ' on B, are not behaviour and are left out. -v shows them.)');
    console.log('');

    /* The round start phase is where the converted mod does the setup a script
     * does at module load, so it is reported and not failed on. Every other
     * phase has to match. */
    const SETUP = { start: true };
    let bad = 0, shown = 0;
    for (const name of Object.keys(a.phases)) {
        const ta = a.phases[name] || [], tb = b.phases[name] || [];
        let at = -1;
        const n = Math.max(ta.length, tb.length);
        for (let i = 0; i < n; i++) { if (ta[i] !== tb[i]) { at = i; break; } }
        const same = at < 0;
        const tag = same ? 'same' : (SETUP[name] ? 'differs (setup, expected)' : 'DIFFERS');
        console.log('  ' + name.padEnd(7) + ' A ' + String(ta.length).padStart(4) +
            '   B ' + String(tb.length).padStart(4) + '   ' + tag);
        if (!same && !SETUP[name]) {
            bad++;
            if (shown < 2) {
                shown++;
                for (let i = Math.max(0, at - 1); i < Math.min(n, at + 3); i++) {
                    const mark = i === at ? '     <-- ' : '         ';
                    console.log(mark + 'A ' + (ta[i] === undefined ? '(nothing)' : ta[i]));
                    console.log(mark + 'B ' + (tb[i] === undefined ? '(nothing)' : tb[i]));
                }
            }
        }
        if (VERBOSE && !same) {
            const ra = (a.raw && a.raw[name]) || ta, rb = (b.raw && b.raw[name]) || tb;
            console.log('   --- A ' + name + ' ---'); ra.forEach((t, i) => console.log('   ' + (i + 1) + ' ' + t));
            console.log('   --- B ' + name + ' ---'); rb.forEach((t, i) => console.log('   ' + (i + 1) + ' ' + t));
        }
    }
    console.log('');
    if (!bad) { console.log('BEHAVIOUR MATCHES on every phase but setup.'); process.exit(0); }
    console.log('DIFFERENTIAL FAILED on ' + bad + ' phase(s)');
    process.exit(1);
}

main();
