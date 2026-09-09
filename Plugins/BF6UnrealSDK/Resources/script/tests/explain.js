// The right hand panel explains where the caret IS. It replaced a column of
// one note per line, which could not stay aligned once a region was hidden and
// said nothing useful on an 8,000 line bundle.
//   node tests/explain.js ../editor.js
//
// What is tested here is the reading, not the drawing: which function encloses
// this line, where a name is set, and where else it is used. Get those wrong
// and the panel is confidently wrong, which is worse than blank.
var fs = require('fs');
var src = fs.readFileSync(process.argv[2], 'utf8');

function extract(name) {
    var i = src.indexOf('    function ' + name + '(');
    if (i < 0) { throw new Error('not found: ' + name); }
    var j = src.indexOf('\n    function ', i + 10);
    if (j < 0) { j = src.length; }
    return src.slice(i, j);
}

var mod = new Function(
    extract('ownerAt') + '\n' + extract('declarationOf') + '\n' +
    extract('usesOf') + '\n' + extract('caretName') + '\n' +
    extract('scanLine') + '\n' + extract('quickKindOf') + '\n' +
    extract('spawnKindOf') + '\n' +
    'return { ownerAt: ownerAt, declarationOf: declarationOf, usesOf: usesOf,' +
    ' caretName: caretName, quickKindOf: quickKindOf };')();

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

var FILE = [
    "import { Events } from 'bf6-portal-utils/events/index.ts';",   // 1
    '',                                                             // 2
    'const KILLS_TO_WIN = 25;',                                     // 3
    'const GREETING = "welcome { soldier }";',                      // 4
    '',                                                             // 5
    'export function OnPlayerDied(e) {',                            // 6
    '    if (score(e) >= KILLS_TO_WIN) {',                          // 7
    '        endRound();',                                          // 8
    '    }',                                                        // 9
    '}',                                                            // 10
    '',                                                             // 11
    'function endRound() {',                                        // 12
    '    mod.EndRound();',                                          // 13
    '}',                                                            // 14
    '',                                                             // 15
    'export function OngoingPlayer(p) {',                           // 16
    '    show(GREETING, KILLS_TO_WIN);',                            // 17
    '}'                                                             // 18
].join('\n').split('\n');

console.log('what encloses this line:');
check('inside a handler', (mod.ownerAt(FILE, 7) || {}).name === 'OnPlayerDied');
check('and it is read as a handler', (mod.ownerAt(FILE, 7) || {}).kind === 'handler');
check('two levels in, still that handler', (mod.ownerAt(FILE, 8) || {}).name === 'OnPlayerDied');
check('a later function is its own owner', (mod.ownerAt(FILE, 13) || {}).name === 'endRound');
check('and it is a plain function', (mod.ownerAt(FILE, 13) || {}).kind === 'function');
check('top level belongs to nothing', mod.ownerAt(FILE, 3) === null);
check('after a function closes, nothing again', mod.ownerAt(FILE, 15) === null);

console.log('');
console.log('a brace inside a string must not open a scope:');
// Line 4 holds "{ soldier }". If those counted, every owner after it is wrong.
check('the handler is still found past that line',
      (mod.ownerAt(FILE, 7) || {}).name === 'OnPlayerDied');
var TRICKY = [
    'const A = "} } }";',
    'function realOwner() {',
    '    doIt();',
    '}'
];
check('unbalanced braces in a string are ignored',
      (mod.ownerAt(TRICKY, 3) || {}).name === 'realOwner');

console.log('');
console.log('where a name is set:');
check('a const', mod.declarationOf(FILE, 'KILLS_TO_WIN').line === 3);
check('a function', mod.declarationOf(FILE, 'endRound').kind === 'function');
check('a name that is not here is not invented', mod.declarationOf(FILE, 'nowhere') === null);
check('a property of an object of settings',
      mod.declarationOf(['const C = {', '  ROUND_TIME: 300,', '}'], 'ROUND_TIME').line === 2);
check('a type annotation is not a declaration',
      mod.declarationOf(['function f(x: number) { return x; }'], 'x') === null);

console.log('');
console.log('where else it is used:');
var u = mod.usesOf(FILE, 'KILLS_TO_WIN', 3, 6);
check('both real uses are found', u.total === 2);
check('the declaration is not counted as a use',
      u.rows.every(function (r) { return r.line !== 3; }));
check('the lines are the ones that mention it',
      u.rows[0].line === 7 && u.rows[1].line === 17);
check('a name inside a longer word does not match',
      mod.usesOf(['const AB = 1;', 'use(ABC);'], 'AB', 1, 6).total === 0);
check('the list is capped but the count is not',
      mod.usesOf(['x;', 'x;', 'x;', 'x;'], 'x', 0, 2).rows.length === 2 &&
      mod.usesOf(['x;', 'x;', 'x;', 'x;'], 'x', 0, 2).total === 4);

console.log('');
console.log('what the caret is on:');
function model(word) {
    return {
        getValueInRange: function (r) { return r.text; },
        getWordAtPosition: function () { return word ? { word: word } : null; }
    };
}
var empty = { isEmpty: function () { return true; } };
function selOf(t) { return { isEmpty: function () { return false; }, text: t }; }
check('with no selection, the word under the caret',
      mod.caretName(model('KILLS_TO_WIN'), empty, { lineNumber: 3 }) === 'KILLS_TO_WIN');
check('a selected name is used',
      mod.caretName(model('other'), selOf('  endRound '), null) === 'endRound');
check('a selected paragraph is not a name',
      mod.caretName(model('x'), selOf('if (a) { b(); }'), null) === '');
check('on whitespace, nothing', mod.caretName(model(null), empty, { lineNumber: 2 }) === '');

console.log('');
console.log('quickKindOf - it was CALLED in two places and DEFINED in none,');
console.log('so every scan for values threw before it could return one:');
check('a number', mod.quickKindOf('25') === 'number');
check('a negative decimal', mod.quickKindOf('-0.5') === 'number');
check('an on/off', mod.quickKindOf('false') === 'boolean');
check('text', mod.quickKindOf("'hello'") === 'string');
check('text with an escaped quote', mod.quickKindOf("'it\\'s here'") === 'string');
check('an object has no simple control', mod.quickKindOf('{ a: 1 }') === '');
check('a call has no simple control', mod.quickKindOf('makeIt()') === '');
check('nothing at all is refused, not guessed', mod.quickKindOf(null) === '');

console.log('');
console.log('and it is defined in the shipped source, not only in the test:');
check('editor.js declares it', /function quickKindOf\(/.test(src));
check('the panel follows the selection too',
      /onDidChangeCursorSelection\(scheduleGutter\)/.test(src));
check('nothing is positioned against a line any more',
      src.indexOf('layoutGutter') < 0);

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
