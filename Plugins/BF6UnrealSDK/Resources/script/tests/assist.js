// Asking the user's own AI from the editor.
//   node tests/assist.js ../editor.js
//
// The seam in the host (BF6Assist) has worked since it was written and no page
// could reach it. What is tested here is the page half: what gets attached to a
// question, what comes back and how much of it may touch the file.
//
// Most of these are about CONTEXT. An answer written without the other files,
// the settings, the log or what has already been said is confident about the
// wrong project, and confidently wrong is the failure mode that costs somebody
// an afternoon.
var fs = require('fs');
var path = require('path');
var src = fs.readFileSync(process.argv[2], 'utf8');
var here = path.dirname(path.resolve(process.argv[2]));

function extract(name) {
    var i = src.indexOf('    function ' + name + '(');
    if (i < 0) { throw new Error('not found: ' + name); }
    var j = src.indexOf('\n    function ', i + 10);
    if (j < 0) { j = src.length; }
    return src.slice(i, j);
}
function extractVar(name) {
    var i = src.indexOf('    var ' + name + ' = ');
    if (i < 0) { throw new Error('not found: var ' + name); }
    return src.slice(i, src.indexOf('\n    function ', i));
}

// The real context pack, not a copy of it: the whole point of the briefing is
// that it carries the rules, so a stand-in would test nothing.
var PACK = require(path.resolve(here, '..', 'assist', 'contextpack.js'));

// The real command and event tables too, when they are on disk, because "the
// events are in the briefing" is only worth asserting against the actual file.
var API = null;
try { API = JSON.parse(fs.readFileSync(path.join(here, 'api.json'), 'utf8')); } catch (e) { API = null; }

var API_FN = {}, API_EV = {};
if (API) {
    (API.functions || []).forEach(function (f) { API_FN[f.name] = f; });
    (API.events || []).forEach(function (e) { API_EV[e.name] = e; });
}
var SYM = { KillPlayer: { sig: '(player: Player): void' } };

// The page state a briefing draws on. Each test sets what it needs.
var project = { name: 'gunmaster', path: 'C:/x/gunmaster' };
var files = {};
var lastFiles = {};
var strTree = null;
var quickList = null, quickValues = {};
var selectedObj = null;
var portalVerdict = null;
var logLines = [];
var aiTurns = [];
var showTemplateCode = false;
var GETTER_FOR = { spatial: 'GetSpatialObject', capture: 'GetCapturePoint' };

function model(text) { return { model: { getValue: function () { return text; } }, dirty: false }; }

var api = new Function(
    'BF6ContextPack', 'SYM', 'API_FN', 'API_EV', 'monaco', 'GETTER_FOR',
    'getState',
    'var project, files, lastFiles, strTree, quickList, quickValues, selectedObj,' +
    ' portalVerdict, logLines, aiTurns, showTemplateCode;' +
    'function sync() { var s = getState();' +
    ' project = s.project; files = s.files; lastFiles = s.lastFiles; strTree = s.strTree;' +
    ' quickList = s.quickList; quickValues = s.quickValues; selectedObj = s.selectedObj;' +
    ' portalVerdict = s.portalVerdict; logLines = s.logLines; aiTurns = s.aiTurns;' +
    ' showTemplateCode = s.showTemplateCode; }\n' +
    extractVar('BRIEF_OTHER_FILE_KB') + '\n' +
    extract('firstLine') + '\n' +
    extract('briefCatalog') + '\n' +
    extract('briefEvents') + '\n' +
    extract('briefingFor') + '\n' +
    extract('splitAnswer') + '\n' +
    extract('strFlatten') + '\n' +
    extract('quickKey') + '\n' +
    'return { brief: function (q, a) { sync(); return briefingFor(q, a); },' +
    ' splitAnswer: splitAnswer, events: function () { sync(); return briefEvents(); } };')(
        PACK, SYM, API_FN, API_EV,
        { editor: { getModelMarkers: function () { return []; } } }, GETTER_FOR,
        function () {
            return { project: project, files: files, lastFiles: lastFiles, strTree: strTree,
                     quickList: quickList, quickValues: quickValues, selectedObj: selectedObj,
                     portalVerdict: portalVerdict, logLines: logLines, aiTurns: aiTurns,
                     showTemplateCode: showTemplateCode };
        });

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

var anchor = { rel: 'src/rules.ts', code: 'const KILLS = 5;',
               fileText: 'const KILLS = 5;\nmore();\n', lines: 1 };

console.log('the spine of it:');
files = { 'src/rules.ts': model(anchor.fileText) };
lastFiles = { 'src/rules.ts': 1 };
var b = api.brief('Why does this not reset between rounds?', anchor);
check('the question is in it', b.indexOf('Why does this not reset between rounds?') > 0);
check('the selected code is in it', b.indexOf('const KILLS = 5;') > 0);
check('the open file is named', b.indexOf('src/rules.ts') > 0);
check('the project is named', b.indexOf('gunmaster') > 0);
check('the rules that decide whether Portal code works are in it',
      b.indexOf('An array cannot contain an array') > 0);
check('and the script-mode rule, not the blocks one',
      b.indexOf('genuine per-invocation local') > 0);

console.log('');
console.log('the commands and the events - it is asked to write handlers:');
check('there IS an event table', b.indexOf('(no event table was available)') < 0);
if (API) {
    check('a real event is listed with its parameters',
          /OnPlayerDied\(.*Player/.test(b));
    check('all 74 events are there',
          Object.keys(api.events() || {}).length === (API.events || []).length);
    check('commands come from api.json, with parameter names',
          b.indexOf('AbsoluteValue(number: number): number') > 0);
    check('and with what they do', b.indexOf('Returns the unsigned value') > 0);
} else {
    console.log('  (api.json not on disk beside editor.js; those four skipped)');
}

console.log('');
console.log('nothing about the machine it runs on goes with it:');
check('no environment variable names', b.indexOf('BF6_AI_KEY') < 0);
check('no absolute paths from this machine', b.indexOf('C:/x/gunmaster') < 0);

console.log('');
console.log('the rest of the mod, not just the file in front of them:');
files = {
    'src/rules.ts': model(anchor.fileText),
    'src/index.ts': model('export function OnPlayerDied() { scoreKill(); }\n'),
    'src/helpers/score.ts': model('export function scoreKill() { return 1; }\n')
};
lastFiles = { 'src/rules.ts': 1, 'src/index.ts': 1, 'src/helpers/score.ts': 1, 'src/unopened.ts': 1 };
var multi = api.brief('Where does the score come from?', anchor);
check('another open file is included whole', multi.indexOf('function scoreKill() { return 1; }') > 0);
check('the handler in a third file is included', multi.indexOf('OnPlayerDied() { scoreKill(); }') > 0);
check('a file in the project but not open is at least named',
      multi.indexOf('src/unopened.ts') > 0);
check('the file being asked about is not duplicated as an "other" file',
      multi.split('### src/rules.ts').length === 1);

console.log('');
console.log('an imported experience, which is the case that misleads:');
files = { 'dist/bundle.ts': model('const A = 1;\n') };
lastFiles = { 'dist/bundle.ts': 1, 'src/index.ts': 1, 'src/helpers/index.ts': 1 };
var imported = api.brief('Change the round time', { rel: 'dist/bundle.ts', code: '',
                                                    fileText: 'const A = 1;\n' });
// The index is not open, so the file list is the only evidence there is. It is
// stated as evidence, and the conclusion it usually supports is given without
// being asserted as fact.
check('it is pointed at the built file', imported.indexOf('dist/bundle.ts IS the mod') > 0);
check('and told not to assume from the file list alone',
      imported.indexOf('Do not assume; the file list cannot tell you') > 0);

// With the scaffold index open and visibly a scaffold, it IS a verdict.
files = { 'dist/bundle.ts': model('const A = 1;\n'),
          'src/index.ts': model("import { Events } from 'bf6-portal-utils/events/index.ts';\n") };
var scaffoldSeen = api.brief('Change the round time', { rel: 'dist/bundle.ts', code: '',
                                                        fileText: 'const A = 1;\n' });
check('a scaffold index settles it', scaffoldSeen.indexOf('IT WAS IMPORTED AS A BUILT FILE') > 0);
check('and it is told not to send them to src/',
      scaffoldSeen.indexOf('Do not suggest editing src/') > 0);

// A real mod written entirely in src/index.ts must NOT be called an import.
var realIndex = 'export function OnPlayerDied() {\n';
for (var pad = 0; pad < 120; pad++) { realIndex += '    // a line of the author\'s own\n'; }
realIndex += '}\n';
files = { 'dist/bundle.ts': model('const A = 1;\n'), 'src/index.ts': model(realIndex) };
var authored = api.brief('Change the round time', { rel: 'src/index.ts', code: '', fileText: realIndex });
check('a mod written only in src/index.ts is not called an import',
      authored.indexOf('IT WAS IMPORTED AS A BUILT FILE') < 0);
check('and that author is sent to their source',
      authored.indexOf('Change the source') > 0);
files = { 'dist/bundle.ts': model('const A = 1;\n') };

files = { 'dist/bundle.ts': model('const A = 1;\n'), 'src/rules.ts': model('const B = 2;\n') };
lastFiles = { 'dist/bundle.ts': 1, 'src/rules.ts': 1 };
var both = api.brief('Change the round time', anchor);
check('with real source present, the advice reverses',
      both.indexOf('Change the source') > 0 && both.indexOf('IT WAS IMPORTED') < 0);

console.log('');
console.log('what the mod says, and what the author tunes:');
files = { 'src/rules.ts': model(anchor.fileText) };
lastFiles = { 'src/rules.ts': 1 };
strTree = { hud: { win: 'You held the point', lose: 'Point lost' } };
quickList = [{ file: 'src/rules.ts', name: 'ROUND_TIME', kind: 'number', nth: 0 }];
quickValues = { 'src/rules.ts|ROUND_TIME|0': '300' };
var rich = api.brief('Make the round shorter', anchor);
check('the strings are listed with their keys', rich.indexOf('hud.win: You held the point') > 0);
check('the Values list is there', rich.indexOf('ROUND_TIME (number) in src/rules.ts = 300') > 0);
check('and it is told to prefer changing one of those',
      rich.indexOf('usually a better answer than changing the code') > 0);
strTree = null; quickList = null; quickValues = {};

console.log('');
console.log('the scene next door:');
selectedObj = { objid: 41, label: 'Capture Point B', kind: 'capture' };
var scene = api.brief('Make this one worth double', anchor);
check('the selected object is named', scene.indexOf('Capture Point B') > 0);
check('with the id a script needs', scene.indexOf('object id 41') > 0);
check('and the exact call that reaches it',
      scene.indexOf('mod.GetCapturePoint(41)') > 0);
selectedObj = null;

console.log('');
console.log("what Portal and the game said - the two compilers that count:");
portalVerdict = { ok: false, mapped: [{ rel: 'src/rules.ts', line: 12, message: "Type 'string' is not assignable to type 'number'." }], unmapped: [] };
var refused = api.brief('Why will it not upload?', anchor);
check('the site\'s refusal is in the briefing',
      refused.indexOf("Type 'string' is not assignable") > 0);
portalVerdict = null;

check('no log section when nothing has been read', api.brief('x', anchor).indexOf('## The game log') < 0);
logLines = [{ raw: '[12:00:01] TypeError: player is undefined' }];
var logged = api.brief('What went wrong?', anchor);
check('the log is attached once it has', logged.indexOf('## The game log') > 0);
check('and the actual lines are in it', logged.indexOf('TypeError: player is undefined') > 0);
logLines = [];

console.log('');
console.log('the conversation, so a follow-up is a follow-up:');
check('nothing said yet means no history section',
      api.brief('First question', anchor).indexOf('## This conversation so far') < 0);
aiTurns = [
    { q: 'What does this handler do?', a: 'It scores a kill and ends the round at 25.', waiting: false },
    { q: 'Is that configurable?', a: 'Yes, through ROUND_TIME.', waiting: false },
    { q: 'still waiting', a: '', waiting: true }
];
var follow = api.brief('Now make it faster', anchor);
check('the earlier questions are carried', follow.indexOf('What does this handler do?') > 0);
check('and the earlier answers', follow.indexOf('It scores a kill and ends the round at 25.') > 0);
check('a turn still in flight is not passed off as an answer',
      follow.indexOf('still waiting') < 0);
check('and it is told which one is the new question',
      follow.indexOf('The question below is the newest one') > 0);
aiTurns = [];

console.log('');
console.log('size, because a briefing that does not fit is a failed request:');
var big = '';
for (var i = 0; i < 4000; i++) { big += 'const LINE' + i + ' = ' + i + ';\n'; }
files = { 'src/rules.ts': model(anchor.fileText), 'src/big1.ts': model(big),
          'src/big2.ts': model(big), 'src/big3.ts': model(big), 'src/big4.ts': model(big),
          'src/big5.ts': model(big), 'src/big6.ts': model(big) };
lastFiles = files;
var huge = api.brief('What is going on here?', anchor);
check('the other files are capped, not sent whole',
      huge.length < 400000);
check('and it says when a file was clipped', huge.indexOf('(first part only)') > 0);
files = { 'src/rules.ts': model(anchor.fileText) };
lastFiles = { 'src/rules.ts': 1 };

console.log('');
console.log('taking an answer apart - only fenced code may touch the file:');
var one = api.splitAnswer('Here is why.\n\n```ts\nconst A = 1;\n```\n\nThat is all.');
check('three parts', one.length === 3);
check('the middle one is the code', one[1].code === true && one[1].text === 'const A = 1;\n');
check('the prose is not code', one[0].code === false && one[2].code === false);
var two = api.splitAnswer('```ts\nfirst();\n```\nand\n```\nsecond();\n```');
check('two code blocks are both found', two.filter(function (p) { return p.code; }).length === 2);
check('a fence with no language still counts',
      two.filter(function (p) { return p.code; })[1].text === 'second();\n');
check('an answer with no code has nothing to apply',
      api.splitAnswer('No, that will not work.').every(function (p) { return !p.code; }));
check('an unterminated fence is not treated as code',
      api.splitAnswer('```ts\nconst A = 1;').every(function (p) { return !p.code; }));

console.log('');
console.log('the guards, in the shipped source:');
check('an answer is never applied on arrival',
      /NOTHING IS APPLIED WITHOUT BEING ASKED FOR/.test(src));
check('applying checks the lines have not moved since the question',
      /those lines have changed since you asked/.test(src));
check('applying goes through executeEdits, so Ctrl\\+Z takes it back',
      /executeEdits\('bf6-ai'/.test(src));
check('the answer only applies to the file it was asked about',
      /that answer was about/.test(src));
check('the box says where the question is going',
      /This leaves your machine/.test(src) && /Staying on this machine/.test(src));
check('and it works with nothing linked at all', /Copy the briefing works anyway/.test(src));
check('the size shown to the user is measured, not guessed',
      /briefingFor\('', askAnchor\)\.length/.test(src));

console.log('');
console.log('the host route exists:');
var cpp = fs.readFileSync(path.resolve(here, '..', '..', 'Source', 'BF6UnrealSDK',
    'Private', 'BF6Script.cpp'), 'utf8');
check('assistStatus is answered', cpp.indexOf('Op == TEXT("assistStatus")') > 0);
check('assistAsk is answered', cpp.indexOf('Op == TEXT("assistAsk")') > 0);
check('and it goes to BF6Assist, which holds the key', /BF6Assist::Ask\(/.test(cpp));
check('the key is never put in a reply to the page',
      cpp.indexOf('SetStringField(TEXT("key")') < 0);

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
