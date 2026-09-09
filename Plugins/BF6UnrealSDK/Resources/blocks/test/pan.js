// Proof that a pan moves the view by the distance the mouse moved.
//
// THE BUG THIS EXISTS TO CATCH.
//
// Blockly's workspace.scroll(x, y) takes a canvas TRANSLATION. That is the
// quantity in ws.scrollX / ws.scrollY, and it is negative once the view has
// left the origin. It is NOT metrics.scrollLeft, which is the positive distance
// from the left edge of the content.
//
// The pan gesture used to start from metrics.scrollLeft and feed that back into
// scroll(). The first thing scroll() does is clamp with
// Math.min(x, -metrics.scrollLeft), so a positive input collapses onto the
// boundary every time. On a large workspace the view jumped once and then
// refused to move: "asked for +200,+120 and the view moved 0,0".
//
// Because the failure is entirely in scroll()'s own clamping arithmetic, a
// hand-written model of that arithmetic could agree with a wrong fix. So this
// lifts the real scroll() body out of the vendored bundle and runs it.
//
// Run by roundtrip.js. Also runs alone:  node pan.js

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');

// ---- the real scroll(), taken out of the shipped bundle ---------------------
function realScroll() {
  const src = fs.readFileSync(path.join(BLOCKS, 'vendor', 'blockly_compressed.js'), 'utf8');
  const at = src.indexOf('scroll(a,b){');
  if (at < 0) { throw new Error('could not find scroll(a,b) in the vendored Blockly'); }
  // Brace-match from the opening brace so the whole body comes out whatever
  // the minifier did inside it.
  const open = src.indexOf('{', at);
  let depth = 0, end = -1;
  for (let i = open; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}') { depth--; if (depth === 0) { end = i; break; } }
  }
  if (end < 0) { throw new Error('scroll(a,b) body is not brace-balanced'); }
  const body = src.slice(open + 1, end);
  // eslint-disable-next-line no-new-func
  return new Function('a', 'b', body);
}

// A workspace with just enough on it for scroll() to run: the metrics it reads,
// and the two calls it makes on the way through.
function fakeWorkspace(metrics, translation) {
  return {
    scrollX: translation.x,
    scrollY: translation.y,
    scrollbar: null,
    hideChaff() {},
    translate() {},
    getMetrics: () => metrics,
  };
}

// A workspace far larger than the viewport, which is the case that broke. The
// numbers are the shape of the Undead Ground Zero workspace, not a tidy fixture.
//
// WHAT scrollLeft ACTUALLY IS, since mistaking it is the whole bug: it is the
// left edge of the SCROLLABLE AREA, a property of where the content sits, and
// it does not change as the user scrolls. scroll() uses it to build the range
// the translation is allowed into:
//
//     -(scrollLeft + scrollWidth - viewWidth)  <=  translation  <=  -scrollLeft
//
// So it is a bound, never a position. Feeding it in as though it were the
// current position asks to be clamped, which is what happened.
function bigMetrics() {
  return {
    viewWidth: 1200, viewHeight: 800,
    scrollWidth: 9000, scrollHeight: 6000,
    contentWidth: 9000, contentHeight: 6000,
    absoluteLeft: 0, absoluteTop: 0,
    scrollLeft: 1000, scrollTop: 500,
    viewLeft: 2000, viewTop: 1500,
  };
}

function panCheck(fails, note) {
  const scroll = realScroll();
  let checks = 0;
  const check = (cond, what) => { checks++; if (!cond) fails.push(what); };

  // 1. THE CONVENTION. Starting from the translation moves by the drag delta;
  //    starting from metrics.scrollLeft does not move at all. Both are run
  //    through the same real scroll(), so this is a statement about Blockly,
  //    not about our arithmetic.
  const m = bigMetrics();
  const startTranslation = { x: -2000, y: -1500 };

  const good = fakeWorkspace(m, startTranslation);
  scroll.call(good, startTranslation.x + 200, startTranslation.y + 120);
  check(good.scrollX === -1800 && good.scrollY === -1380,
    'a +200,+120 drag from the canvas translation should land at -1800,-1380, got ' +
    good.scrollX + ',' + good.scrollY);

  const bad = fakeWorkspace(m, startTranslation);
  scroll.call(bad, m.scrollLeft + 200, m.scrollTop + 120);
  check(bad.scrollX === -1000 && bad.scrollY === -500,
    'the old metrics.scrollLeft convention should clamp to the -scrollLeft edge, got ' +
    bad.scrollX + ',' + bad.scrollY);
  note('the old convention turned a +200,+120 drag into a jump of ' +
    (bad.scrollX - startTranslation.x) + ',' + (bad.scrollY - startTranslation.y) +
    '; the translation convention moves 200,120');

  // 2. AT SEVERAL SCROLL POSITIONS. The clamp only bites near an edge, so a fix
  //    that happened to work in the middle is not a fix. The content stays put,
  //    which is why the bounds do not move with the view.
  [[-1200, -700], [-2000, -1500], [-8000, -5000]].forEach(([tx, ty]) => {
    const ws = fakeWorkspace(bigMetrics(), { x: tx, y: ty });
    scroll.call(ws, tx - 150, ty - 90);   // drag the other way too
    check(ws.scrollX === tx - 150 && ws.scrollY === ty - 90,
      'a -150,-90 drag at translation ' + tx + ',' + ty + ' should land at ' +
      (tx - 150) + ',' + (ty - 90) + ', got ' + ws.scrollX + ',' + ws.scrollY);
  });

  // 3. THE SOURCE ACTUALLY USES IT. The arithmetic above is only worth
  //    anything if the gesture reads the translation, so the editor is checked
  //    for the old pattern rather than trusted.
  const js = fs.readFileSync(path.join(BLOCKS, 'editor_ui.js'), 'utf8');
  check(!/atX\s*=\s*m\.scrollLeft/.test(js),
    'the pan gesture still starts from metrics.scrollLeft');
  check(/atX\s*=\s*t\.x/.test(js),
    'the pan gesture should start from the canvas translation');
  check(/UI\.ws\.scroll\(t0x \+ 200, t0y \+ 120\)/.test(js),
    'the pan self test should drive the translation, not metrics.scrollLeft');

  // 4. THE MINIMAP JUMP. viewWidth is already in the translation's pixel space,
  //    so scaling the half-extent as well threw the jump off by the zoom.
  check(!/viewWidth \* UI\.ws\.scale \/ 2/.test(js),
    'the minimap jump still scales the viewport half-extent by the zoom');

  return checks;
}

// ---- runner ----------------------------------------------------------------
// Needs no Blockly instance and no workspace: it lifts scroll() out of the
// bundle itself. The arguments are accepted so it drops into roundtrip.js
// beside the suites that do need them.
function run() {
  const fails = [];
  const notes = [];
  const checks = panCheck(fails, m => notes.push(m));

  console.log('pan checks        ', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log('    note', n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run, panCheck };

if (require.main === module) {
  const r = run();
  console.log(r.ok ? 'PAN OK' : 'PAN FAILED');
  process.exit(r.ok ? 0 : 1);
}
