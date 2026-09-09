// Sounds and effects are quick values too, and most settings live as
// properties of one config object rather than as loose consts.
//   node tests/sound-values.js ../editor.js <a-real-bundle.ts>
//
// Both of those were invisible to the old scan: it read literal initialisers of
// const/let/var only, so an enum member was "no simple control" and a property
// was not a value at all.
var fs = require('fs');
var src = fs.readFileSync(process.argv[2], 'utf8');

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
    var j = src.indexOf('\n    function ', i);
    return src.slice(i, j);
}

var mod = new Function(
    extractVar('VALUE_RE') + '\n' +
    extract('scanFileForValues') + '\n' +
    extract('withOccurrences') + '\n' +
    extract('scanDeclarationAll') + '\n' +
    extract('quickKindOf') + '\n' +
    extract('spawnKindOf') + '\n' +
    extract('scanDeclaration') + '\n' +
    extractVar('REGEX_AFTER_WORD') + '\n' +
    extract('regexCanStart') + '\n' +
    extract('skipRegex') + '\n' +
    extract('scanInitializerEnd') + '\n' +
    extract('scanPropertyEnd') + '\n' +
    extract('quickReadFrom') + '\n' +
    extract('quickWriteInto') + '\n' +
    extract('suggestRank') + '\n' +
    extract('sfxLiteral') + '\n' +
    'return { scan: scanFileForValues, kind: quickKindOf, spawn: spawnKindOf,' +
    ' read: quickReadFrom, write: quickWriteInto, rank: suggestRank,' +
    ' literal: sfxLiteral };')();

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

console.log('what a sound looks like in a real script:');
check('the full path, as this project writes it',
      mod.spawn('mod.RuntimeSpawn_Common.SFX_UI_Notification_Primary_D_2D') === 'sfx');
check('enum and member only', mod.spawn('RuntimeSpawn_Common.SFX_Alarm') === 'sfx');
check('the bare member', mod.spawn('SFX_Alarm') === 'sfx');
check('an effect is told apart from a sound',
      mod.spawn('mod.RuntimeSpawn_Common.VFX_Explosion_Large') === 'fx');
check('FX_ counts as an effect', mod.spawn('FX_Smoke') === 'fx');
check('an ordinary call is not a sound', mod.spawn('mod.GetPlayers()') === '');
check('an ordinary property path is not a sound', mod.spawn('mod.Teams.One') === '');
check('quickKindOf agrees, so the picker is offered',
      mod.kind('mod.RuntimeSpawn_Common.SFX_Alarm') === 'sfx');
check('and an object still has no simple control', mod.kind('{ a: 1 }') === '');

console.log('');
console.log('the two shapes settings are written in:');
var FILE = [
    'export const AMPED_HIT_SFX = mod.RuntimeSpawn_Common.SFX_UI_LeadChange',   // 1
    'export const SFX_CONFIG = {',                                             // 2
    '    READY_UP: mod.RuntimeSpawn_Common.SFX_Gadgets_Defib,',                // 3
    '    KILLS_PER_WEAPON: 5,',                                                // 4
    '    FRIENDLY_FIRE: false,',                                               // 5
    '    TITLE: "Gunmaster",',                                                 // 6
    '}',                                                                       // 7
    'switch (n) {',                                                            // 8
    "    case 'x':",                                                           // 9
    '        return 1;',                                                       // 10
    '}',                                                                       // 11
    'function f(count: number) { return count; }'                              // 12
].join('\n');
var rows = mod.scan('src/index.ts', FILE);
function by(n) { return rows.filter(function (r) { return r.name === n; })[0]; }
check('the loose const sound is found', !!by('AMPED_HIT_SFX') && by('AMPED_HIT_SFX').kind === 'sfx');
check('the sound inside the config object is found', !!by('READY_UP') && by('READY_UP').kind === 'sfx');
check('so is a number beside it', !!by('KILLS_PER_WEAPON') && by('KILLS_PER_WEAPON').kind === 'number');
check('so is an on/off', !!by('FRIENDLY_FIRE') && by('FRIENDLY_FIRE').kind === 'boolean');
check('so is text', !!by('TITLE') && by('TITLE').kind === 'string');
check('a case label is not a setting', !by('x') && !rows.some(function (r) { return r.name === 'case'; }));
check('a type annotation is not a setting', !by('count'));
check('the config object itself is not offered', !by('SFX_CONFIG'));
check('nothing else was invented', rows.length === 5);

console.log('');
console.log('changing one writes back where it was read:');
var afterProp = mod.write(FILE, 'READY_UP', 'mod.RuntimeSpawn_Common.SFX_UI_New');
check('the property is rewritten', /READY_UP: mod\.RuntimeSpawn_Common\.SFX_UI_New,/.test(afterProp));
check('its neighbours are untouched', /KILLS_PER_WEAPON: 5,/.test(afterProp)
      && /FRIENDLY_FIRE: false,/.test(afterProp));
var afterConst = mod.write(FILE, 'AMPED_HIT_SFX', 'mod.RuntimeSpawn_Common.SFX_Other');
check('the loose const is rewritten',
      /^export const AMPED_HIT_SFX = mod\.RuntimeSpawn_Common\.SFX_Other$/m.test(afterConst));
check('a sound reads back whole',
      mod.read(FILE, 'READY_UP').literal === 'mod.RuntimeSpawn_Common.SFX_Gadgets_Defib');

console.log('');
console.log('the same name three times - difficulty presets:');
var PRESETS = [
    'const EASY = {',                 // 1
    '    killsPerWeapon: 3,',         // 2
    '}',                              // 3
    'const NORMAL = {',               // 4
    '    killsPerWeapon: 5,',         // 5
    '}',                              // 6
    'const HARD = {',                 // 7
    '    killsPerWeapon: 9,',         // 8
    '}'                               // 9
].join('\n');
var three = mod.scan('src/index.ts', PRESETS).filter(function (r) { return r.name === 'killsPerWeapon'; });
check('all three are offered', three.length === 3);
check('and each knows which it is',
      three[0].nth === 0 && three[1].nth === 1 && three[2].nth === 2);
check('each reads its own value',
      mod.read(PRESETS, 'killsPerWeapon', 0).literal === '3' &&
      mod.read(PRESETS, 'killsPerWeapon', 1).literal === '5' &&
      mod.read(PRESETS, 'killsPerWeapon', 2).literal === '9');
var hardOnly = mod.write(PRESETS, 'killsPerWeapon', '12', 2);
check('changing the hard preset changes the hard preset',
      /HARD = \{\n    killsPerWeapon: 12,/.test(hardOnly));
check('and leaves the other two alone',
      /killsPerWeapon: 3,/.test(hardOnly) && /killsPerWeapon: 5,/.test(hardOnly));
check('no occurrence means the first, as before',
      mod.write(PRESETS, 'killsPerWeapon', '1').indexOf('killsPerWeapon: 1,') === PRESETS.indexOf('killsPerWeapon'));

console.log('');
console.log('the swap is written the way the file already writes them:');
var picked = { name: 'SFX_New_Sound', 'enum': 'RuntimeSpawn_Other', kind: 'sfx' };
check('a mod-qualified reference stays mod-qualified',
      mod.literal('mod.RuntimeSpawn_Common.SFX_Old', picked)
      === 'mod.RuntimeSpawn_Other.SFX_New_Sound');
check('a bare enum reference stays bare',
      mod.literal('RuntimeSpawn_Common.SFX_Old', picked)
      === 'RuntimeSpawn_Other.SFX_New_Sound');
check('a member with no enum in front gets one',
      mod.literal('SFX_Old', picked) === 'RuntimeSpawn_Other.SFX_New_Sound');
check('and an empty value still compiles',
      mod.literal('', picked) === 'RuntimeSpawn_Other.SFX_New_Sound');

console.log('');
console.log('ranking - a sound is there to be swapped:');
check('a sound outranks a plain number',
      mod.rank({ name: 'READY_UP', kind: 'sfx', line: 3 }) >
      mod.rank({ name: 'tickCount', kind: 'number', line: 3 }));

console.log('');
console.log('the list shows all of them, and filters:');
check('nothing is cropped at sixty', src.indexOf('rows.slice(0, 60)') < 0);
check('there is a filter box', /class = 'sgFind'|sgFind/.test(src));
check('and kind buttons for sounds, effects, on/off and numbers',
      /label: 'Sounds'/.test(src) && /label: 'Effects'/.test(src)
      && /label: 'On\/off'/.test(src) && /label: 'Numbers'/.test(src));
check('the filter resets when a new scan runs', /suggestKind = '';/.test(src));

if (process.argv[3]) {
    console.log('');
    console.log('the real bundle:');
    var bundle = fs.readFileSync(process.argv[3], 'utf8');
    var all = mod.scan('dist/bundle.ts', bundle);
    var sounds = all.filter(function (r) { return r.kind === 'sfx' || r.kind === 'fx'; });
    var props = all.filter(function (r) { return r.property; });
    console.log('  values found        : ' + all.length);
    console.log('  of those, sounds/fx : ' + sounds.length);
    console.log('  of those, properties: ' + props.length);
    check('the sounds this script uses are now offered', sounds.length >= 8);
    check('and every one is editable by name',
          sounds.every(function (r) { return !!mod.read(bundle, r.name); }));
    // The presets are real: this script holds killsPerWeapon three times.
    var seen = {};
    all.forEach(function (r) { seen[r.name] = (seen[r.name] || 0) + 1; });
    var repeated = all.filter(function (r) { return seen[r.name] > 1 && r.nth > 0; });
    console.log('  repeated names      : ' + Object.keys(seen).filter(function (k) { return seen[k] > 1; }).length
                + ' (' + repeated.length + ' rows past the first)');
    check('a repeated name is anchored past the first', repeated.length > 0);
    check('and the edit lands on ITS line, not the first one', (function () {
        var r = repeated[0];
        if (!r) { return false; }
        var next = mod.write(bundle, r.name, '"CHANGED_BY_TEST"', r.nth);
        if (next === null) { return false; }
        var a = bundle.split('\n'), b = next.split('\n');
        for (var i = 0; i < a.length; i++) { if (a[i] !== b[i]) { return i + 1 === r.line; } }
        return false;
    })());

    var t0 = Date.now();
    mod.scan('dist/bundle.ts', bundle);
    console.log('  scan time           : ' + (Date.now() - t0) + 'ms');
    check('fast enough to run when the tab opens', Date.now() - t0 < 3000);

    check('swapping the first one changes exactly it', (function () {
        var s = sounds[0];
        var next = mod.write(bundle, s.name, 'mod.RuntimeSpawn_Common.SFX_Test');
        if (next === null) { return false; }
        var a = bundle.split('\n'), b = next.split('\n'), diff = 0;
        for (var i = 0; i < a.length; i++) { if (a[i] !== b[i]) { diff++; } }
        return diff === 1;
    })());
}

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
