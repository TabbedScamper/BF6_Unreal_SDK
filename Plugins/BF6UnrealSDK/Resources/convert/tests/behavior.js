/*
 * BF6Convert behaviour suite.
 *
 *   node tests/behavior.js            run every case
 *   node tests/behavior.js reentry    run the cases whose name contains "reentry"
 *
 * WHY THIS EXISTS, SEPARATELY FROM run.js
 *
 * run.js checks that conversion produces a workspace and that the workspace
 * converts back. That is a shape test: every one of its six cases passed while
 * the converter was silently dropping three of Undead Ground Zero's event
 * registrations, sharing one global between two live invocations of the same
 * function, and evaluating the branch of a conditional it had already decided
 * not to take.
 *
 * This suite asks a different question: does the converted program DO the same
 * thing. Each case is TypeScript. It is executed directly, then converted to
 * blocks, back to TypeScript, and executed again against the same mock of the
 * Portal API. The two effect traces must match.
 *
 * A case can also assert that something is REPORTED. A construct the converter
 * cannot express is acceptable; a construct it drops without saying so is not,
 * because nothing tells the author to look. Those cases assert on the report
 * rather than on the trace.
 *
 * The mock is not Portal. It proves the converter disagrees with its own input,
 * which is enough to find these bugs and not enough to prove a mode works in
 * game. See FINDINGS.md, "Test limitations".
 */
'use strict';

var fs = require('fs');
var path = require('path');
var vm = require('vm');

var ROOT = path.resolve(__dirname, '..');
var C = require(path.join(ROOT, 'convert.js'));

/* TypeScript comes from whichever project has one; we do not ship an install. */
var TS_CANDIDATES = [
    process.env.BF6_TS,
    'C:\\PortalSDK_1.4.2.0\\GodotProject\\User_Created\\projects\\BF6-Undead-Ground-Zero\\node_modules\\typescript',
    'C:\\PortalSDK_1.4.2.0\\GodotProject\\User_Created\\projects\\_template-v1.7.0\\node_modules\\typescript'
];
var ts = null, tsWhere = '';
for (var t = 0; t < TS_CANDIDATES.length; t++) {
    if (!TS_CANDIDATES[t]) { continue; }
    try { ts = require(TS_CANDIDATES[t]); tsWhere = TS_CANDIDATES[t]; break; }
    catch (e) { /* try the next one */ }
}
if (!ts) {
    console.log('SKIP  no TypeScript compiler found. Set BF6_TS to a typescript install.');
    process.exit(0);
}
C.setTypeScript(ts);

/* ------------------------------------------------------------------ the mock */

/* A MOCK, DELIBERATELY SMALL.
 *
 * Only the operations the cases actually use. An unimplemented one throws by
 * name rather than returning undefined, so a case that quietly starts calling
 * something new fails loudly instead of comparing two piles of undefined.
 */
function newEnv() {
    var vars = new Map(), pending = [], trace = [], randomCalls = 0, now = 0;

    function key(v) { return typeof v === 'string' ? v : JSON.stringify(v); }

    var api = {
        GlobalVariable: function (i) { return 'g' + i; },
        ObjectVariable: function (o, i) { return 'o' + (o && o.id) + ':' + i; },
        SetVariable: function (v, x) { vars.set(key(v), x); },
        GetVariable: function (v) { return vars.has(key(v)) ? vars.get(key(v)) : 0; },
        SetVariableAtIndex: function (v, i, x) {
            var a = vars.get(key(v)); a = Array.isArray(a) ? a.slice() : [];
            a[i] = x; vars.set(key(v), a);
        },
        EmptyArray: function () { return []; },
        AppendToArray: function (a, b) { return (a || []).concat(b); },
        ValueInArray: function (a, i) { return (a || [])[i]; },
        ArraySlice: function (a, s, e) { return (a || []).slice(s, e); },
        CountOf: function (a) { return (a || []).length; },

        GetObjId: function (p) { return p && p.id; },
        GetPlayer: function (i) { return { id: i }; },
        Wait: function () { return new Promise(function (r) { pending.push(r); }); },

        /* Every observable effect lands here, in order. This is the trace. */
        SetGameModeTargetScore: function () {
            trace.push(Array.prototype.slice.call(arguments));
        },
        DisplayHighlightedWorldLogMessage: function () {
            trace.push(['msg'].concat(Array.prototype.slice.call(arguments)));
        },

        Add: function (a, b) { return a + b; },
        Subtract: function (a, b) { return a - b; },
        Multiply: function (a, b) { return a * b; },
        Divide: function (a, b) { return a / b; },
        Modulo: function (a, b) { return a % b; },
        Equals: function (a, b) { return a === b; },
        NotEqualTo: function (a, b) { return a !== b; },
        LessThan: function (a, b) { return a < b; },
        LessThanEqualTo: function (a, b) { return a <= b; },
        GreaterThan: function (a, b) { return a > b; },
        GreaterThanEqualTo: function (a, b) { return a >= b; },
        Not: function (a) { return !a; },
        And: function (a, b) { return Boolean(a && b); },
        Or: function (a, b) { return Boolean(a || b); },
        IfThenElse: function (c, a, b) { return c ? a : b; },
        IsUndefined: function (a) { return a === undefined; },

        /* Counted, not random: a case can then assert how many times the
         * converted program called it, which is how eager evaluation shows. */
        RandomReal: function () { randomCalls++; return randomCalls; },
        RandomInt: function () { randomCalls++; return randomCalls; },

        GetGameModeTime: function () { return now; }
    };

    var guarded = new Proxy(api, {
        get: function (o, p) {
            if (!(p in o)) { throw new Error('the mock has no mod.' + String(p)); }
            return o[p];
        }
    });
    return {
        mod: guarded, trace: trace, pending: pending, vars: vars,
        randomCalls: function () { return randomCalls; }
    };
}

/* Run a set of .ts sources as modules, with mod bound to the mock. */
function loadModules(files, mod) {
    var cache = {};
    var context = vm.createContext({ mod: mod, console: console, Promise: Promise });
    function req(name) {
        name = path.posix.normalize(String(name))
            .replace(/^\.\//, '').replace(/\.js$/, '.ts');
        if (cache[name]) { return cache[name].exports; }
        if (files[name] === undefined) { throw new Error('missing module ' + name); }
        var m = { exports: {} };
        cache[name] = m;
        var js = ts.transpileModule(files[name], {
            compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 }
        }).outputText;
        var f = vm.runInContext('(function(require,module,exports){' + js + '\n})',
            context, { filename: name });
        f(req, m, m.exports);
        return m.exports;
    }
    return req('index.ts');
}

/* ------------------------------------------------------------------- cases */

/* Each case is either:
 *   trace:  drive(api, env) on both sides, traces must match
 *   report: assert(report, workspace) returns '' to pass or a reason to fail
 */
var CASES = [];

function traceCase(name, sources, drive, opts) {
    CASES.push({ name: name, kind: 'trace', sources: sources, drive: drive,
                 opts: opts || {} });
}
function reportCase(name, sources, assert) {
    CASES.push({ name: name, kind: 'report', sources: sources, assert: assert });
}

async function callStart(api) { await api.OnGameModeStarted(); }

/* --- 1. nothing may disappear without being reported --------------------- */

/* THE ONE THAT STARTED THIS. Three of UGZ's registrations vanished with no
 * diagnostic because their handler is an inline arrow with more than one
 * statement. A converter may refuse a construct. It may not forget it. */
reportCase('report/inline-arrow-subscription', {
    'index.ts':
        'Events.OnPlayerDeployed.subscribe((player) => {\n' +
        '    const id = mod.GetObjId(player);\n' +
        '    mod.SetGameModeTargetScore(id);\n' +
        '});\n'
}, function (rep, ws) {
    var seen = JSON.stringify(ws) + JSON.stringify(rep.unconvertible || []) +
               JSON.stringify(rep.warnings || []);
    if (seen.indexOf('OnPlayerDeployed') < 0) {
        return 'the registration is in neither the workspace nor any diagnostic';
    }
    return '';
});

reportCase('report/multi-statement-arrow-keeps-body', {
    'index.ts':
        'Events.OnRayCastHit.subscribe((player, point, normal) => {\n' +
        '    mod.SetGameModeTargetScore(mod.GetObjId(player));\n' +
        '    mod.SetGameModeTargetScore(point);\n' +
        '});\n'
}, function (rep, ws) {
    var seen = JSON.stringify(ws) + JSON.stringify(rep.unconvertible || []) +
               JSON.stringify(rep.warnings || []);
    if (seen.indexOf('OnRayCastHit') < 0) {
        return 'OnRayCastHit is nowhere: not converted and not reported';
    }
    return '';
});

/* A class is not supported. That is allowed. Silently keeping the call that
 * used it, with an empty socket, is not. */
reportCase('report/dropped-class-is-a-blocker', {
    'index.ts':
        'class Wallet { static cash = 10; }\n' +
        'export function OnGameModeStarted() { mod.SetGameModeTargetScore(Wallet.cash); }\n'
}, function (rep) {
    var all = JSON.stringify(rep.unconvertible || []);
    if (all.indexOf('Wallet') < 0) { return 'the class was dropped with no diagnostic naming it'; }
    return '';
});

/* --- 2. event handlers and their arguments ------------------------------- */

traceCase('events/inline-arrow-runs', {
    'index.ts':
        'export function OnPlayerDeployed(eventPlayer: mod.Player) {\n' +
        '    mod.SetGameModeTargetScore(mod.GetObjId(eventPlayer));\n' +
        '}\n'
}, async function (api) {
    await api.OnPlayerDeployed({ id: 4 });
});

/* The handler names its parameter "player", not the canonical "eventPlayer".
 * A parameter that exists at runtime must not become a missing expression
 * because of what the author called it. */
traceCase('events/parameter-named-freely', {
    'index.ts':
        'export function OnPlayerDeployed(player: mod.Player) {\n' +
        '    mod.SetGameModeTargetScore(mod.GetObjId(player));\n' +
        '}\n'
}, async function (api) {
    await api.OnPlayerDeployed({ id: 9 });
});

/* --- 3. semantics -------------------------------------------------------- */

/* Two live invocations of one function must not share a slot. */
traceCase('semantics/overlapping-invocations', {
    'index.ts':
        'export async function OnPlayerDeployed(eventPlayer: mod.Player) {\n' +
        '    const id = mod.GetObjId(eventPlayer);\n' +
        '    await mod.Wait(1);\n' +
        '    mod.SetGameModeTargetScore(id);\n' +
        '}\n'
}, async function (api, env) {
    api.OnPlayerDeployed({ id: 1 });
    api.OnPlayerDeployed({ id: 2 });
    while (env.pending.length) {
        env.pending.shift()();
        await new Promise(function (r) { setImmediate(r); });
    }
});

traceCase('semantics/recursion-keeps-its-own-copy', {
    'index.ts':
        'function visit(n: number) {\n' +
        '    const copy = n;\n' +
        '    if (n > 0) { visit(n - 1); }\n' +
        '    mod.SetGameModeTargetScore(copy);\n' +
        '}\n' +
        'export function OnGameModeStarted() { visit(2); }\n'
}, callStart);

/* A branch that was not taken must not run. IfThenElse evaluates both. */
traceCase('semantics/untaken-branch-does-not-run', {
    'index.ts':
        'export function OnGameModeStarted() {\n' +
        '    let enabled = false;\n' +
        '    let x = enabled ? mod.RandomReal(1, 10) : 5;\n' +
        '    mod.SetGameModeTargetScore(x);\n' +
        '    mod.SetGameModeTargetScore(mod.RandomReal(1, 10));\n' +
        '}\n'
}, callStart, { compareRandom: true });

traceCase('semantics/and-short-circuits', {
    'index.ts':
        'export function OnGameModeStarted() {\n' +
        '    let on = false;\n' +
        '    if (on && mod.RandomReal(1, 10) > 0) { mod.SetGameModeTargetScore(1); }\n' +
        '    mod.SetGameModeTargetScore(2);\n' +
        '}\n'
}, callStart, { compareRandom: true });

/* Initialisation must follow dependencies, not file names. */
traceCase('semantics/init-order-follows-imports', {
    'index.ts': "import './z.ts';\nimport './a.ts';\n" +
                'export function OnGameModeStarted() { mod.SetGameModeTargetScore(result); }\n',
    'z.ts': 'let base = 7;\n',
    'a.ts': 'let result = base + 1;\n'
}, callStart, {
    /* Executed directly, the modules run in import order; the converted side
     * must reach the same answer however it chooses to order its own init. */
    originalFiles: {
        'index.ts': 'let base = 7;\nlet result = base + 1;\n' +
                    'export function OnGameModeStarted() { mod.SetGameModeTargetScore(result); }\n'
    }
});

/* --- 4. the vertical slice ----------------------------------------------- */

/* THE MILESTONE, IN ONE CASE.
 *
 * Configuration built at load, a player accepted, starting cash assigned, a
 * wait, then a HUD value updated, with two players overlapping across the
 * wait. Every failure above shows up here at once, which is why it is the
 * thing to keep green rather than the individual cases. */
traceCase('slice/two-players-through-a-wait', {
    'index.ts':
        'const START_CASH = 500;\n' +
        'const BONUS = [0, 10, 20];\n' +
        'let awarded = 0;\n' +
        'export async function OnPlayerDeployed(eventPlayer: mod.Player) {\n' +
        '    const id = mod.GetObjId(eventPlayer);\n' +
        '    const cash = START_CASH + BONUS[id];\n' +
        '    awarded = awarded + 1;\n' +
        '    await mod.Wait(1);\n' +
        '    mod.SetGameModeTargetScore(id, cash, awarded);\n' +
        '}\n'
}, async function (api, env) {
    /* A BLOCK WORKSPACE HAS NO MODULE LOAD PHASE.
     *
     * Module level constants are initialised by a synthesised
     * OnGameModeStarted rule, because that is the only place a workspace can
     * put them. In a match that rule fires before anybody deploys, so the
     * harness fires it too. The original has no such export and skips it.
     */
    if (api.OnGameModeStarted) { await api.OnGameModeStarted(); }
    api.OnPlayerDeployed({ id: 1 });
    api.OnPlayerDeployed({ id: 2 });
    while (env.pending.length) {
        env.pending.shift()();
        await new Promise(function (r) { setImmediate(r); });
    }
});

/* --- 5. what the author put on the canvas, not in the code --------------- */

/* AUTHORING INTENT HAS NO SOURCE TO COME FROM.
 *
 * A comment, or a block switched off, exists only in the workspace. Nothing in
 * the original TypeScript can be compared against it, so these cases convert,
 * decorate the workspace the way a person would in the editor, convert to
 * TypeScript, and assert on the text that comes out. */
function authoringCase(name, sources, decorate, assert) {
    CASES.push({ name: name, kind: 'authoring', sources: sources,
                 decorate: decorate, assert: assert });
}

var AUTHORING_SRC = {
    'index.ts':
        'let score = 0;\n' +
        'export function OnPlayerDeployed(eventPlayer: mod.Player) {\n' +
        '    score = score + 1;\n' +
        '    mod.SetGameModeTargetScore(mod.Add(score, 10));\n' +
        '}\n'
};

function eachBlock(ws, fn) {
    (function walk(n, role) {
        if (!n || typeof n !== 'object') { return; }
        if (Array.isArray(n)) { n.forEach(function (x) { walk(x, role); }); return; }
        if (n.type) { fn(n, role); }
        if (n.inputs) {
            for (var k in n.inputs) {
                if (n.inputs[k] && n.inputs[k].block) { walk(n.inputs[k].block, 'value'); }
            }
        }
        if (n.next && n.next.block) { walk(n.next.block, 'statement'); }
        for (var k2 in n) {
            if (k2 === 'inputs' || k2 === 'next') { continue; }
            if (n[k2] && typeof n[k2] === 'object') { walk(n[k2], n.type ? 'statement' : role); }
        }
    }(ws, 'top'));
}
function comment(b, text) {
    b.icons = b.icons || {};
    b.icons.comment = { text: text, pinned: false, height: 80, width: 160 };
}

authoringCase('authoring/comment-sits-above-its-statement', AUTHORING_SRC,
    function (ws) {
        eachBlock(ws, function (b, role) {
            if (role !== 'value' && /^SetGameModeTargetScore/.test(b.type || '')) {
                comment(b, 'WATCH_THIS');
            }
        });
    },
    function (text) {
        var at = text.indexOf('WATCH_THIS');
        if (at < 0) { return 'the comment did not survive at all'; }
        var line = text.indexOf('mod.SetGameModeTargetScore', at);
        var before = text.lastIndexOf('mod.SetGameModeTargetScore', at);
        /* It must appear ABOVE its statement, not after it. */
        if (line < 0 || (before >= 0 && at - before < line - at)) {
            return 'the comment came out below the statement it belongs to';
        }
        return '';
    });

authoringCase('authoring/comment-on-a-value-block-survives', AUTHORING_SRC,
    function (ws) {
        eachBlock(ws, function (b) { if (b.type === 'Add') { comment(b, 'ABOUT_THE_MATHS'); } });
    },
    function (text) {
        return text.indexOf('ABOUT_THE_MATHS') >= 0 ? ''
            : 'a comment on an expression block was dropped';
    });

/* The one that is a correctness bug rather than a lost note. */
authoringCase('authoring/disabled-block-does-not-run', AUTHORING_SRC,
    function (ws) {
        eachBlock(ws, function (b, role) {
            if (role !== 'value' && /^SetGameModeTargetScore/.test(b.type || '')) {
                b.enabled = false;
            }
        });
    },
    function (text) {
        /* It may appear in a comment; it may not appear as a call. */
        var live = /^\s*mod\.SetGameModeTargetScore\(/m.test(text);
        if (live) { return 'a block the author switched off still runs in the export'; }
        if (text.indexOf('disabled in the block editor') < 0) {
            return 'the disabled block vanished with no trace of why';
        }
        return '';
    });

/* ------------------------------------------------------------------ runner */

function sameTrace(a, b) { return JSON.stringify(a) === JSON.stringify(b); }

async function runOne(c) {
    var converted;
    try { converted = C.tsToBlocks(c.sources, {}); }
    catch (e) { return { ok: false, why: 'conversion threw: ' + e.message }; }

    var rep = converted.report || {};
    var ws = converted.workspace || converted.json;

    if (c.kind === 'report') {
        var why = c.assert(rep, ws);
        return { ok: !why, why: why };
    }

    if (c.kind === 'authoring') {
        try { c.decorate(ws); }
        catch (e) { return { ok: false, why: 'could not decorate: ' + e.message }; }
        var written;
        try { written = C.blocksToTs(ws, {}); }
        catch (e) { return { ok: false, why: 'blocksToTs threw: ' + e.message }; }
        var joined = Object.keys(written.files).map(function (f) {
            return written.files[f];
        }).join('\n');
        var bad = c.assert(joined);
        return { ok: !bad, why: bad };
    }

    var back;
    try { back = C.blocksToTs(ws, {}); }
    catch (e) { return { ok: false, why: 'blocksToTs threw: ' + e.message }; }

    var out = {};
    for (var side = 0; side < 2; side++) {
        var isConverted = side === 1;
        var files = isConverted ? back.files
                  : (c.opts.originalFiles || c.sources);
        var env = newEnv();
        try {
            var api = loadModules(files, env.mod);
            await c.drive(api, env);
            await new Promise(function (r) { setImmediate(r); });
        } catch (e) {
            out[isConverted ? 'converted' : 'original'] =
                { error: (e && e.message) || String(e), trace: env.trace };
            continue;
        }
        out[isConverted ? 'converted' : 'original'] =
            { trace: env.trace.slice(), randomCalls: env.randomCalls() };
    }

    if (out.original.error) { return { ok: false, why: 'the ORIGINAL failed to run: ' + out.original.error }; }
    if (out.converted.error) { return { ok: false, why: 'converted failed to run: ' + out.converted.error }; }
    if (!sameTrace(out.original.trace, out.converted.trace)) {
        return { ok: false, why: 'traces differ\n        original  ' +
            JSON.stringify(out.original.trace) + '\n        converted ' +
            JSON.stringify(out.converted.trace) };
    }
    if (c.opts.compareRandom && out.original.randomCalls !== out.converted.randomCalls) {
        return { ok: false, why: 'the converted program made ' + out.converted.randomCalls +
            ' random call(s), the original made ' + out.original.randomCalls };
    }
    return { ok: true };
}

async function main() {
    var filter = process.argv[2] || '';
    var pass = 0, fail = 0;
    console.log('BF6Convert behaviour suite   (TypeScript from ' + tsWhere + ')');
    console.log('');
    for (var i = 0; i < CASES.length; i++) {
        var c = CASES[i];
        if (filter && c.name.indexOf(filter) < 0) { continue; }
        var r = await runOne(c);
        if (r.ok) { pass++; console.log('  PASS  ' + c.name); }
        else { fail++; console.log('  FAIL  ' + c.name + '\n        ' + r.why); }
    }
    console.log('');
    console.log(pass + ' passed, ' + fail + ' failed');
    process.exitCode = fail ? 1 : 0;
}

main().catch(function (e) { console.error(e); process.exitCode = 1; });
