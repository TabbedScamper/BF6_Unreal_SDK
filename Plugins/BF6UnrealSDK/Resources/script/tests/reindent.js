// Re-indenting a bundle must change layout and nothing else.
//   node tests/reindent.js ../editor.js <some-bundle.ts>
var fs = require('fs');
var src = fs.readFileSync(process.argv[2], 'utf8');

// The functions live inside the page's IIFE, so they are lifted out by name
// rather than by evaluating the whole file, which would need a browser.
// Extracting by counting braces is defeated by a regex containing a brace in a
// character class - which scanDeclaration has. These functions are siblings at
// one indent inside the page's IIFE, so the next sibling is the real boundary.
function extract(name) {
    var i = src.indexOf('    function ' + name + '(');
    if (i < 0) { throw new Error('not found: ' + name); }
    var j = src.indexOf('\n    function ', i + 10);
    if (j < 0) { j = src.length; }
    return src.slice(i, j);
}

var mod = new Function(
    extract('reindentFlat') + '\n' + extract('repeat') + '\n' +
    extract('scanLine') + '\n' + extract('looksFlat') + '\n' +
    'return { reindentFlat: reindentFlat, looksFlat: looksFlat };')();

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

console.log('shape:');
var flat = [
    'export class Logging {',
    'public willLog(l: number): boolean {',
    'return this._logger !== undefined && l >= this._logLevel',
    '}',
    '}'
].join('\n');
var got = mod.reindentFlat(flat).split('\n');
check('nested body indents', got[2] === '        return this._logger !== undefined && l >= this._logLevel');
check('closer comes back out', got[3] === '    }');
check('outer closer at column 0', got[4] === '}');

console.log('');
console.log('safety - meaning must not change:');
var tricky = [
    'const A = "a { b } c";',
    'const B = `line one',
    'still in the template { }',
    'end`;',
    '/* a comment with { */',
    'const C = 1; // trailing } comment',
    'function f() {',
    'return 2;',
    '}'
].join('\n');
var out = mod.reindentFlat(tricky);
function strip(t) { return t.split('\n').map(function (l) { return l.replace(/^[ \t]+/, ''); }).join('\n'); }
check('only leading whitespace changed', strip(out) === strip(tricky));
check('template literal body untouched', out.split('\n')[2] === 'still in the template { }');

console.log('');
console.log('detection:');
check('already-indented source is left alone', !mod.looksFlat(src));

if (process.argv[3]) {
    console.log('');
    console.log('a real bundle:');
    var bundle = fs.readFileSync(process.argv[3], 'utf8');
    check('a flat bundle is detected', mod.looksFlat(bundle));
    var t0 = Date.now();
    var fixed = mod.reindentFlat(bundle);
    var ms = Date.now() - t0;
    var before = bundle.split('\n'), after = fixed.split('\n');
    check('line count unchanged', before.length === after.length);
    check('every line identical once leading space is removed', strip(bundle) === strip(fixed));
    var indented = after.filter(function (l) { return /^\s+\S/.test(l); }).length;
    console.log('  lines now indented   : ' + indented + ' of ' + after.length + ' in ' + ms + 'ms');
    check('most of the file is now indented', indented > after.length * 0.3);
    check('fast enough to do on open', ms < 1500);
}

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
