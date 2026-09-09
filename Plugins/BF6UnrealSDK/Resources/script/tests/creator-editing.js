// Acceptance for CW-01/02/03, run against the functions as they now stand in
// editor.js rather than against a copy of them.
var fs = require('fs');
var src = fs.readFileSync(process.argv[2], 'utf8');

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
    extract('scanDeclaration') + '\n' +
    extract('scanInitializerEnd') + '\n' +
    extract('scanPropertyEnd') + '\n' +
    extract('quickWriteInto') + '\n' +
    extract('quickReadFrom') + '\n' +
    'return { write: quickWriteInto, read: quickReadFrom };')();

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

console.log('CW-02 acceptance: only the chosen literal changes');
var a = "const TITLE = 'Hello; soldier';\nconst KEEP = 1;";
var ao = mod.write(a, 'TITLE', "'Changed'");
check('semicolon in a string does not corrupt', ao === "const TITLE = 'Changed';\nconst KEEP = 1;");

var b = "// note: const SCORE = 10 was the old value\nconst SCORE = 250;";
var bo = mod.write(b, 'SCORE', '999');
check('the comment is byte-identical', bo.split('\n')[0] === b.split('\n')[0]);
check('the declaration is the thing that changed', bo.split('\n')[1] === 'const SCORE = 999;');

var c = "const A = 1;\nfunction setA(n) { return n; }\nconst B = A + 1;";
var co = mod.write(c, 'A', '42');
check('unrelated source untouched',
      co === "const A = 42;\nfunction setA(n) { return n; }\nconst B = A + 1;");

var d = "const CFG = {\n  a: 1,\n  b: 'x; y'\n};\nconst Z = 2;";
check('multiline initialiser read whole',
      mod.read(d, 'CFG').literal === "{\n  a: 1,\n  b: 'x; y'\n}");

check('an unsupported name is refused, not guessed', mod.read(c, 'NOPE') === null);
check('writing an absent name returns null', mod.write(c, 'NOPE', '1') === null);

console.log('');
console.log('object properties - how real mods group their settings');
var cfg = [
    'export const GUNMASTER_CONFIG = {',
    'KILLS_PER_WEAPON: 5,',
    'DEATH_SETBACK: 3,',
    'DEBUG_SKIP_TO_END: false',
    '}'
].join('\n');
check('a property is found', !!mod.read(cfg, 'KILLS_PER_WEAPON')
      && mod.read(cfg, 'KILLS_PER_WEAPON').literal === '5');
var cfgo = mod.write(cfg, 'KILLS_PER_WEAPON', '9');
check('only that property changed',
      cfgo === cfg.replace('KILLS_PER_WEAPON: 5', 'KILLS_PER_WEAPON: 9'));
check('a later property is untouched', /DEATH_SETBACK: 3,/.test(cfgo));
check('a boolean property reads', mod.read(cfg, 'DEBUG_SKIP_TO_END').literal === 'false');
check('the last property, with no trailing comma, writes',
      /DEBUG_SKIP_TO_END: true/.test(mod.write(cfg, 'DEBUG_SKIP_TO_END', 'true')));
check('a type annotation is not mistaken for a property',
      mod.read('function f(x: number) { return x; }', 'x') === null);
check('a nested object value is read whole',
      mod.read('const C = {\n  inner: { a: 1, b: 2 },\n  after: 3\n}', 'inner').literal === '{ a: 1, b: 2 }');

console.log('');
console.log('CW-01/03 acceptance: guards present in the shipped source');
check('strings load is project-scoped', /strProject = forProject/.test(src));
check('a stale strings timer is dropped', /that text was not saved into the new one/.test(src));
check('an unreadable strings file is not replaced', /could not be read as JSON/.test(src));
check('the + refuses on an unreadable file', /nothing can be added to it yet/.test(src));
check('text is flushed before a build', /strFlush\(\);/.test(src));
check('quick write only clears dirty for the text it sent',
      /f\.model\.getValue\(\) === sent/.test(src));
check('a reply from another project is ignored',
      /!== sentProject/.test(src));

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
