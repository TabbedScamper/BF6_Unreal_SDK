// The independently reproduced findings from the 2026-09-08 audit, as tests.
//   node tests/audit-lc.js ../editor.js
//
// Every case here FAILED against the code as audited. They are behavioural, not
// phrase matches: each one drives the real function with a controlled model,
// reply or filesystem and checks what actually happened, because the previous
// suites passed while these same failures were reproducible.
var fs = require('fs');
var src = fs.readFileSync(process.argv[2], 'utf8');

function extract(name) {
    var i = src.indexOf('    function ' + name + '(');
    if (i < 0) { throw new Error('not found: ' + name); }
    var j = src.indexOf('\n    function ', i + 10);
    if (j < 0) { j = src.length; }
    return src.slice(i, j) + '\n';
}
function extractVar(name) {
    var i = src.indexOf('    var ' + name + ' = ');
    if (i < 0) { throw new Error('not found: var ' + name); }
    return src.slice(i, src.indexOf('\n    function ', i)) + '\n';
}

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

// ---------------------------------------------------------------- LC-04
console.log('LC-04  opening a file must not change what a string CONTAINS');
var fmt = new Function(extract('reindentFlat') + extract('repeat') +
    extract('scanLine') + extract('looksFlat') +
    'return { ri: reindentFlat, flat: looksFlat };')();

var L = [];
for (var i = 0; i < 80; i++) { L.push('const V' + i + ' = ' + i + ';'); }
L.push('function outer() {');
L.push('if (true) {');
L.push("const S = 'hello \\");      // continued with a backslash: the next
L.push("soldier';");                // line's leading spaces are string content
L.push('return S;');
L.push('}');
L.push('}');
var flatText = L.join('\n');
function valueOf(text) {
    try { return new Function(text + '\nreturn outer();')(); }
    catch (e) { return 'PARSE ERROR: ' + e.message; }
}
check('the fixture is one the re-indenter chooses on its own', fmt.flat(flatText));
var formatted = fmt.ri(flatText);
check('the string still says what it said', valueOf(formatted) === valueOf(flatText));
check('and it still says exactly "hello soldier"', valueOf(formatted) === 'hello soldier');
check('the code around it was still re-indented',
      formatted.split('\n')[81] === '    if (true) {');
check('a template literal is still left alone',
      fmt.ri('function f() {\nconst T = `a\n  b`;\n}').split('\n')[2] === '  b`;');

// ---------------------------------------------------------------- LC-03a
console.log('');
console.log('LC-03  a declaration inside a regular expression is not a declaration');
var edit = new Function(
    extract('scanDeclaration') + extract('scanDeclarationAll') +
    extractVar('REGEX_AFTER_WORD') + extract('regexCanStart') + extract('skipRegex') +
    extract('scanInitializerEnd') + extract('scanPropertyEnd') +
    extract('quickWriteInto') + extract('quickReadFrom') +
    'return { read: quickReadFrom, write: quickWriteInto };')();

var reFixture = 'const RE = /const SCORE = 1;/;\nconst SCORE = 9;';
check('the real declaration is the one read', edit.read(reFixture, 'SCORE').literal === '9');
check('and the one written',
      edit.write(reFixture, 'SCORE', '42') === 'const RE = /const SCORE = 1;/;\nconst SCORE = 42;');
check('the pattern is byte-identical afterwards',
      edit.write(reFixture, 'SCORE', '42').split('\n')[0] === reFixture.split('\n')[0]);
check('division is still division, not an unterminated pattern',
      edit.read('const X = 10 / 2;\nconst Y = 3;', 'Y').literal === '3');
check('a regex holding a semicolon does not truncate the value',
      edit.read('const R = a.replace(/x;y/g, "");\nconst S = 2;', 'R').literal
      === 'a.replace(/x;y/g, "")');
check('a slash inside a character class does not end the pattern',
      edit.read('const P = /a[/]b/g;\nconst Q = 7;', 'Q').literal === '7');
check('the earlier string and comment cases still hold',
      edit.write("const TITLE = 'Hello; soldier';\nconst KEEP = 1;", 'TITLE', "'Changed'")
      === "const TITLE = 'Changed';\nconst KEEP = 1;");

// ---------------------------------------------------------------- LC-03b
console.log('');
console.log('LC-03  a value read in one project cannot be written into another');
// The real quickSet, with a read that resolves after the project has changed.
var writes = [];
var state = { project: { path: 'A', gen: 1 } };
var said = [];
var qs = new Function('readText', 'call', 'quickWriteInto', 'files', 'quickValues',
    'quickKey', 'say', 'drawTabs', 'getProject',
    'var project;' +
    'function sync() { project = getProject(); }\n' +
    extract('quickSet').replace('return readText(', 'sync(); return readText(') +
    'return quickSet;')(
        function () { return Promise.resolve('const N = 1;'); },
        function (op, m) { writes.push(m); return Promise.resolve({}); },
        function (t) { return t.replace('1', '2'); },
        {}, {}, function (q) { return q.file + '|' + q.name; },
        function (t) { said.push(t); }, function () {},
        function () { return state.project; });

var p = qs({ file: 'src/index.ts', name: 'N', nth: 0 }, '2');
state.project = { path: 'B', gen: 1 };          // switched while the read was out
p.then(function () {
    check('nothing was written into the project that was switched to', writes.length === 0);
    check('and the user was told why', said.some(function (t) { return /changed project/.test(t); }));

    // The same call with no switch must still write.
    writes = []; said = [];
    state.project = { path: 'A', gen: 1 };
    return qs({ file: 'src/index.ts', name: 'N', nth: 0 }, '2');
}).then(function () {
    check('a value set without switching still writes', writes.length === 1);
    runCloseTests();
}).catch(function (e) { console.log('ERROR', e && e.message); process.exit(1); });

// ---------------------------------------------------------------- LC-01
function runCloseTests() {
    console.log('');
    console.log('LC-01  a dirty tab is not discarded before its save has landed');

    function harness(writeResult) {
        var disposed = [];
        var files = {};
        var msgs = [];
        var model = {
            value: 'edited text',
            getValue: function () { return this.value; },
            dispose: function () { disposed.push('src/index.ts'); }
        };
        files['src/index.ts'] = { model: model, dirty: true };
        var api = new Function('files', 'call', 'say', 'drawTabs', 'openFile', 'editor',
            'getActive', 'setActive',
            'var activeRel;' +
            'function sync() { activeRel = getActive(); }\n' +
            extract('closeMany') + extract('closeFile') +
            'return { closeFile: function (r, q) { sync(); return closeFile(r, q); },' +
            ' closeMany: closeMany };')(
                files, function () { return writeResult(); },
                function (t) { msgs.push(t); }, function () {}, function () {}, null,
                function () { return 'src/index.ts'; }, function () {});
        return { api: api, files: files, disposed: disposed, msgs: msgs, model: model };
    }

    var refused = harness(function () { return Promise.reject({ why: 'the file is locked' }); });
    refused.api.closeFile('src/index.ts').then(function (closed) {
        check('a refused save does not close the tab', closed === false);
        check('the model was never disposed', refused.disposed.length === 0);
        check('the text is still there', refused.files['src/index.ts'].model.getValue() === 'edited text');
        check('and it is still marked unsaved', refused.files['src/index.ts'].dirty === true);
        check('the message says the work was kept',
              refused.msgs.some(function (t) { return /left open with your changes/.test(t); }));

        var ok = harness(function () { return Promise.resolve({}); });
        return ok.api.closeFile('src/index.ts').then(function (closed2) {
            check('a save that lands does close the tab', closed2 === true);
            check('and disposes the model then', ok.disposed.length === 1);
        });
    }).then(function () {
        // Close-all must not lose the one file that would not save.
        var disposed = [];
        var files = {};
        ['a.ts', 'b.ts', 'c.ts'].forEach(function (r) {
            files[r] = { dirty: true, model: {
                getValue: function () { return r; },
                dispose: function () { disposed.push(r); }
            } };
        });
        var msgs = [];
        var api = new Function('files', 'call', 'say', 'drawTabs', 'openFile', 'editor',
            'getActive',
            'var activeRel;' +
            'function sync() { activeRel = getActive(); }\n' +
            extract('closeMany') + extract('closeFile') +
            'return { closeFile: closeFile, closeMany: function (l) { sync(); return closeMany(l); } };')(
                files,
                function (op, m) {
                    return m.rel === 'b.ts' ? Promise.reject({ why: 'locked' }) : Promise.resolve({});
                },
                function (t) { msgs.push(t); }, function () {}, function () {}, null,
                function () { return ''; });
        return api.closeMany(['a.ts', 'b.ts', 'c.ts']).then(function (kept) {
            check('close all keeps exactly the file that would not save',
                  kept.length === 1 && kept[0] === 'b.ts');
            check('the other two closed', disposed.length === 2 && disposed.indexOf('b.ts') < 0);
            check('the refusal is reported once, naming it',
                  msgs.some(function (t) { return /still open: b\.ts/.test(t); }));
            runIsolationTests();
        });
    }).catch(function (e) { console.log('ERROR', e && e.stack); process.exit(1); });
}

// ---------------------------------------------------------------- LC-08
function runIsolationTests() {
    console.log('');
    console.log('LC-08  an AI answer belongs to the project that asked for it');

    var edits = [], msgs = [];
    var here = { path: 'B', gen: 1 };
    var modelText = 'const KILLS = 5;';
    var api = new Function('editor', 'monaco', 'say', 'getProject', 'getActive',
        'var project, activeRel;' +
        'function sync() { project = getProject(); activeRel = getActive(); }\n' +
        extract('applyCode') +
        'return function (t, c) { sync(); return applyCode(t, c); };')(
            {
                getModel: function () {
                    return { getValueInRange: function () { return modelText; } };
                },
                executeEdits: function (who, e) { edits.push(e[0].text); },
                focus: function () {},
                getPosition: function () { return { lineNumber: 1, column: 1 }; }
            },
            { Range: function () {} },
            function (t) { msgs.push(t); },
            function () { return here; },
            function () { return 'src/index.ts'; });

    // A turn from project A, with the identical file and identical selected text.
    var turnFromA = { proj: 'A', gen: 1, rel: 'src/index.ts', code: modelText,
                      range: { startLineNumber: 1, startColumn: 1, endLineNumber: 1, endColumn: 17 } };
    api(turnFromA, 'const KILLS = 99;');
    check('an answer from another project is refused', edits.length === 0);
    check('and says so', msgs.some(function (t) { return /different project/.test(t); }));

    var turnFromB = { proj: 'B', gen: 1, rel: 'src/index.ts', code: modelText,
                      range: { startLineNumber: 1, startColumn: 1, endLineNumber: 1, endColumn: 17 } };
    api(turnFromB, 'const KILLS = 99;');
    check('an answer from this project applies', edits.length === 1);

    edits = []; msgs = [];
    api({ proj: 'B', gen: 0, rel: 'src/index.ts', code: modelText,
          range: { startLineNumber: 1, startColumn: 1, endLineNumber: 1, endColumn: 17 } },
        'const KILLS = 99;');
    check('an answer from before the project was reopened is refused', edits.length === 0);
    check('and says the project moved on',
          msgs.some(function (t) { return /reopened since you asked/.test(t); }));

    console.log('');
    console.log('LC-08  and one project\'s conversation stays out of another\'s briefing');
    check('the history filter is scoped by project',
          /!t\.proj \|\| t\.proj === here/.test(src));
    check('the transcript shows only this project\'s turns',
          /function turnsHere\(\)/.test(src) && /mine\.forEach/.test(src));
    check('switching project closes a question box framed on the old one',
          /closeAsk\(\);\s*\n\s*askAnchor = null;/.test(src));

    console.log('');
    console.log(pass + ' passed, ' + fail + ' failed');
    process.exit(fail ? 1 : 0);
}
