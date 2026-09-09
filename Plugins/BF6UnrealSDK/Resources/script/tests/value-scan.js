// Where Quick Values looks for settings.
//   node tests/value-scan.js ../editor.js <a-real-bundle.ts>
//
// The case that matters is an IMPORTED experience: the template scaffold means
// src/ is never empty, so a fallback keyed on "no source files" never fires and
// the list stays blank on exactly the projects that need it.
var fs = require('fs');
var src = fs.readFileSync(process.argv[2], 'utf8');

function extract(name) {
    var i = src.indexOf('    function ' + name + '(');
    if (i < 0) { throw new Error('not found: ' + name); }
    var j = src.indexOf('\n    function ', i + 10);
    if (j < 0) { j = src.length; }
    return src.slice(i, j);
}

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

// Files the harness will serve, by relative path.
var DISK = {};
function readText(rel) {
    return Object.prototype.hasOwnProperty.call(DISK, rel)
        ? Promise.resolve(DISK[rel])
        : Promise.reject(new Error('no ' + rel));
}

// quickKindOf is EXTRACTED, never supplied. It used to be passed in here as a
// stand-in, and because the page never had one of its own, this suite passed
// green while every scan in the editor threw on the first literal it read.
// A test that provides the thing under test is not testing anything.
function extractVar(name) {
    var i = src.indexOf('    var ' + name + ' = ');
    if (i < 0) { throw new Error('not found: var ' + name); }
    return src.slice(i, src.indexOf('\n    function ', i));
}

var mod = new Function('readText',
    extract('valueSourceFiles') + '\n' +
    extract('scanForValues') + '\n' +
    extractVar('VALUE_RE') + '\n' +
    extract('scanFileForValues') + '\n' +
    extract('withOccurrences') + '\n' +
    extract('scanDeclaration') + '\n' +
    extract('scanDeclarationAll') + '\n' +
    extractVar('REGEX_AFTER_WORD') + '\n' +
    extract('regexCanStart') + '\n' +
    extract('skipRegex') + '\n' +
    extract('scanInitializerEnd') + '\n' +
    extract('scanPropertyEnd') + '\n' +
    extract('quickKindOf') + '\n' +
    extract('spawnKindOf') + '\n' +
    'return { scanForValues: scanForValues, valueSourceFiles: valueSourceFiles };')(
        readText);

// The template starter, exactly as a scaffold leaves it: real files, no
// settings of the author's in them.
var TEMPLATE_INDEX = "import { Events } from 'bf6-portal-utils/events/index.ts';\nlet adminDebugTool;\n";
var TEMPLATE_HELPERS = "export function getPlayerStateVectorString(p) { return ''; }\n";

var bundlePath = process.argv[3];
var BUNDLE = bundlePath ? fs.readFileSync(bundlePath, 'utf8')
    : "// --- BUNDLED TYPESCRIPT OUTPUT ---\nexport const INSTANT_START = false\nexport const MIN_PLAYERS_TO_START = 1\n";

console.log('an imported experience: template src plus the built file');
DISK = {
    'src/index.ts': TEMPLATE_INDEX,
    'src/helpers/index.ts': TEMPLATE_HELPERS,
    'dist/bundle.ts': BUNDLE
};
var files = Object.keys(DISK);
var where = mod.valueSourceFiles(files);
check('src is not empty, so an "is src empty" test would never fall back',
      where.src.length > 0);

mod.scanForValues(files).then(function (res) {
    check('values were found anyway', res.rows.length > 0);
    check('they came from the built file', res.fromBundle === true);
    check('and they are attributed to it',
          res.rows.every(function (r) { return r.file === 'dist/bundle.ts'; }));
    console.log('   found ' + res.rows.length + ' value(s)');

    console.log('');
    console.log('a project with real source of its own');
    DISK = {
        'src/index.ts': TEMPLATE_INDEX,
        'src/config.ts': 'export const START_CASH = 500;\nexport const FRIENDLY_FIRE = false;\n',
        'dist/bundle.ts': BUNDLE
    };
    return mod.scanForValues(Object.keys(DISK));
}).then(function (res2) {
    check('source values are used', res2.rows.length === 2);
    check('the built file is NOT scanned when source has settings', res2.fromBundle === false);
    check('a value edited here survives a build',
          res2.rows.every(function (r) { return r.file.indexOf('src/') === 0; }));

    console.log('');
    console.log('a project with neither');
    DISK = { 'src/index.ts': TEMPLATE_INDEX };
    return mod.scanForValues(Object.keys(DISK));
}).then(function (res3) {
    check('nothing found, and nothing invented', res3.rows.length === 0 && !res3.fromBundle);

    console.log('');
    console.log(pass + ' passed, ' + fail + ' failed');
    process.exit(fail ? 1 : 0);
}).catch(function (e) {
    console.log('ERROR', e && e.message);
    process.exit(1);
});
