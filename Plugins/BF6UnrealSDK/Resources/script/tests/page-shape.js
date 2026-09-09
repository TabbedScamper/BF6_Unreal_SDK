// NOTHING IN THE PAGE MAY BE DEFINED TWICE.
//   node tests/page-shape.js ../editor.js
//
// A whole 367 line region of editor.js existed twice. The later copy was the
// older code, and in JavaScript the later definition wins, so a fix made in the
// first copy did nothing at all: values scanned with the old rules, the offer
// box gave the old message, and every test passed, because the tests lift a
// function out by name and find the FIRST one.
//
// This is the check that would have caught it in a second.
var fs = require('fs');
var lines = fs.readFileSync(process.argv[2], 'utf8').split('\n');

var pass = 0, fail = 0;
function check(label, cond) {
    if (cond) { pass++; console.log('  ok   ' + label); }
    else { fail++; console.log('  FAIL ' + label); }
}

// Declarations at one indent are the page's own top level, inside its IIFE.
var where = {};
lines.forEach(function (l, i) {
    var m = l.match(/^    (?:function ([A-Za-z_$][\w$]*)\(|var ([A-Za-z_$][\w$]*) = )/);
    if (!m) { return; }
    var name = m[1] || m[2];
    (where[name] = where[name] || []).push(i + 1);
});

var names = Object.keys(where);
var dup = names.filter(function (n) { return where[n].length > 1; });

console.log('top-level names: ' + names.length);
dup.forEach(function (n) { console.log('  DUPLICATE ' + n + ' at lines ' + where[n].join(', ')); });
check('every function and variable is defined once', dup.length === 0);

// The same shadowing, one level down: two identical long runs of the file.
var seen = {}, runs = 0;
var RUN = 40;
for (var i = 0; i + RUN < lines.length; i++) {
    var body = lines.slice(i, i + RUN).join('\n');
    if (!/\S/.test(body.replace(/\s/g, ''))) { continue; }
    if (body.trim().length < 400) { continue; }
    if (seen[body] !== undefined) { runs++; console.log('  lines ' + (i + 1) + ' repeat lines ' + (seen[body] + 1)); i += RUN; continue; }
    seen[body] = i;
}
check('no run of ' + RUN + ' lines appears twice', runs === 0);

console.log('');
console.log(pass + ' passed, ' + fail + ' failed');
process.exit(fail ? 1 : 0);
