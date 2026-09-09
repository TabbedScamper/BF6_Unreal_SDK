/* CORE-04, as executable checks against the real project-switch bodies.
 *
 * THE BUG. Changing project is two round trips: write the outgoing files, then
 * ask the host to open the new one. The editor stayed live in between. Type in
 * that gap and the flush had already been and gone, so nothing was going to
 * save that keystroke, and afterOpen disposed the freshly dirty model as soon
 * as the open came back. The text was on screen and nowhere else.
 *
 * These run the real openProject, afterOpen, saveOne, saveAllDirty and
 * openFile out of editor.js, so a change that reopens the gap fails here.
 */
const fs = require('fs');
const path = require('path');

const F = path.join(__dirname, '..', 'editor.js');
const src = fs.readFileSync(F, 'utf8');

function grab(name) {
  const at = src.indexOf('function ' + name + '(');
  if (at < 0) throw new Error('no ' + name);
  let d = 0;
  const i = src.indexOf('{', at);
  for (let j = i; j < src.length; j++) {
    if (src[j] === '{') d++;
    else if (src[j] === '}') { d--; if (!d) return src.slice(at, j + 1); }
  }
  throw new Error('unbalanced ' + name);
}

function makeEnv() {
  const env = {
    ops: [],              // every op the page sent the tool, in order
    said: [],
    els: {},
    timers: [],
    readOnlyWhenOpened: null,
    editor: { opts: { readOnly: false }, updateOptions(o) { Object.assign(this.opts, o); }, setModel() {} },
    open: null,           // resolve/reject of the held 'open' call
    // And of the 'status' the page asks when an open goes unanswered. It starts
    // as a pair that does nothing so that a build which never asks reports the
    // checks that catch that, rather than throwing here and hiding the rest of
    // the suite. Whether it was asked at all is its own check.
    status: { res() {}, rej() {} }
  };
  env.$ = id => env.els[id] || (env.els[id] = { textContent: '', value: '', classList: { contains: () => false } });

  // The tool. Writes land immediately unless a test says otherwise; the open
  // is always held so a test can act inside the transition.
  env.writeFails = false;
  function call(op, a) {
    env.ops.push({ op, a, readOnly: env.editor.opts.readOnly });
    if (op === 'write') {
      return env.writeFails ? Promise.reject({ why: 'locked' }) : Promise.resolve({});
    }
    if (op === 'open') {
      return new Promise((res, rej) => { env.open = { res, rej }; });
    }
    // Held for the same reason as the open: reconcileOpen asks the host what
    // project it has, and a test has to be able to act before it answers.
    if (op === 'status') {
      return new Promise((res, rej) => { env.status = { res, rej }; });
    }
    return Promise.resolve({});
  }

  const monaco = {
    Uri: { parse: s => s },
    editor: {
      getModel: () => null,
      createModel: v => ({ v, getValue() { return this.v; }, setValue(x) { this.v = x; }, dispose() { this.disposed = true; } })
    }
  };

  const body = [
    // PAGE HELPERS THIS HARNESS DOES NOT EXERCISE.
    //
    // The functions under test call into the rest of the page. Naming each
    // dependency in the injection list means the suite stops running the day
    // somebody adds one - which is what happened: it died on strFlush, then
    // on findInvalidate, without reaching a single assertion. Declared here as
    // no-ops instead, so a new call site is inert rather than fatal.
    'var note = function () {}, strFlush = function () {},',
    'applyTemplateHiding = function () {}, buildGutter = function () {},',
    'setPref = function () {}, showStrings = function () {}, showQuick = function () {},',
    'looksFlat = function () { return false; },',
    'reindentFlat = function (t) { return t; }, findInvalidate = function () {},',
    'closeAsk = function () {}, drawAi = function () {}, aiMode = "explain",',
    'askAnchor = null, drawStrings = function () {}, drawQuick = function () {},',
    'loadStrings = function () {}, loadQuick = function () {},',
    'isStringsOpen = function () { return false; }, isQuickOpen = function () { return false; },',
    'strTree = null, quickList = null;',
    'var files = {}, activeRel = null, project = null, NOTE_OVERRIDES = {};',
    'var switching = false;',
    'var stranded = {};',
    'var unresolvedOpen = null;',
    'var autosaveTimer = null;',
    grab('writesAreAllowed'),
    grab('scheduleAutosave'),
    grab('saveOne'),
    grab('saveAllDirty'),
    grab('openFile'),
    grab('freezeEditing'),
    grab('thawEditing'),
    grab('restoreSelector'),
    grab('reconcileOpen'),
    grab('openProject'),
    grab('afterOpen'),
    grab('recoverStranded'),
    'return { openProject: openProject, scheduleAutosave: scheduleAutosave,',
    '  saveAllDirty: saveAllDirty,',
    '  writesAllowed: function () { return writesAreAllowed(); },',
    '  projectGen: function () { return project && project.gen; },',
    '  isSwitching: function () { return switching; },',
    '  files: function () { return files; },',
    '  stranded: function () { return stranded; },',
    '  setProject: function (p) { project = p; },',
    '  put: function (rel, text, dirty) { files[rel] = { model: monaco.editor.createModel(text), dirty: !!dirty }; },',
    '  dirty: function (rel) { files[rel].dirty = true; },',
    '  text: function (rel) { return files[rel] && files[rel].model.getValue(); } };'
  ].join('\n');

  const f = new Function(
    'call', 'say', 'drawTabs', 'refreshStringKeys', '$', 'editor', 'monaco',
    'refreshFirstRun', 'refreshFiles', 'refreshTypes', 'drawWalk', 'scheduleGutter',
    'strFlush',
    'Promise', 'Error', 'setTimeout', 'clearTimeout', body);

  const setT = (fn, ms) => { env.timers.push({ fn, ms }); return env.timers.length; };
  const clearT = id => { if (env.timers[id - 1]) env.timers[id - 1] = { fn: null }; };

  const api = f(call, (t, l) => env.said.push([l, t]), () => {}, () => {}, env.$, env.editor, monaco,
    () => {}, () => Promise.resolve(), () => Promise.resolve(), () => {}, () => {},
    () => {},
    Promise, Error, setT, clearT);

  env.runTimers = () => { const l = env.timers; env.timers = []; l.forEach(t => t.fn && t.fn()); };
  return { env, api };
}

const tick = () => new Promise(r => setTimeout(r, 0));
/* A promise that always fulfils, with { err } when the original rejected. An
 * openProject that rejects before the test awaits it is an unhandled rejection
 * and kills the run, which hides the failure the check is there to report. */
const settled = p => p.then(v => ({ v: v }), e => ({ err: e }));
let pass = 0, fail = 0;
const ok = (n, c) => { if (c) { pass++; console.log('  PASS  ' + n); } else { fail++; console.log('  FAIL  ' + n); } };

(async () => {
  /* THE DEFECT. An edit made after the flush and before the open comes back
   * used to be disposed without a word. It must survive, and it must come back
   * when its own project is opened again. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1' });
    api.put('a.ts', 'typed before', true);
    const p = api.openProject('P2');
    await tick(); await tick();
    ok('CORE-04 the outgoing project was written before the open',
      env.ops[0] && env.ops[0].op === 'write');
    ok('CORE-04 the editor is read only for the whole transition, not just the save',
      env.ops.some(o => o.op === 'open') && env.ops.find(o => o.op === 'open').readOnly === true);

    // The gap. Read only closes it in the real editor; this is the belt and
    // braces behind that, and it is what actually loses work if it goes.
    api.files()['a.ts'].model.setValue('typed during the switch');
    api.dirty('a.ts');
    env.open.res({ project: { name: 'B', path: 'P2' } });
    await p;
    ok('CORE-04 an edit made during the transition is not discarded',
      (api.stranded()['P1'] || {})['a.ts'] === 'typed during the switch');
    ok('CORE-04 the user is told where it went',
      env.said.some(s => /still had unsaved text/.test(s[1])));
    ok('CORE-04 the editor is writable again afterwards', env.editor.opts.readOnly === false);
    ok('CORE-04 the switch itself completed', api.isSwitching() === false);

    // And back again.
    const back = api.openProject('P1');
    await tick();
    env.open.res({ project: { name: 'A', path: 'P1' } });
    await back;
    ok('CORE-04 reopening the project puts the text back', api.text('a.ts') === 'typed during the switch');
    ok('CORE-04 and it is dirty, so it gets written', api.files()['a.ts'].dirty === true);
    ok('CORE-04 nothing is left stranded once it is recovered', api.stranded()['P1'] === undefined);
  }

  /* A flush that fails must not switch at all. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1' });
    api.put('a.ts', 'work', true);
    env.writeFails = true;
    env.$('projSel').value = 'P2';
    let rejected = false;
    await api.openProject('P2').then(() => {}, () => { rejected = true; });
    ok('CORE-04 a failed flush refuses the switch', rejected);
    ok('CORE-04 and never asked the host to open anything', !env.ops.some(o => o.op === 'open'));
    ok('CORE-04 the file is still open and still dirty',
      api.text('a.ts') === 'work' && api.files()['a.ts'].dirty === true);
    ok('CORE-04 the editor is not left read only', env.editor.opts.readOnly === false);
    ok('CORE-04 the selector goes back to the project we are in', env.$('projSel').value === 'P1');
  }

  /* Two switches must not interleave. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1' });
    const first = api.openProject('P2');
    await tick();
    let busy = false;
    await api.openProject('P3').then(() => {}, e => { busy = !!e.busy; });
    ok('CORE-04 a second switch during the first is refused', busy);
    ok('CORE-04 only one open was sent', env.ops.filter(o => o.op === 'open').length === 1);
    env.open.res({ project: { name: 'B', path: 'P2' } });
    await first;
  }

  /* Autosave must not fire into the middle of a transition: the rel paths in
   * the table belong to the project being left. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1' });
    api.put('a.ts', 'x', false);
    api.openProject('P2');
    await tick();
    api.dirty('a.ts');
    api.scheduleAutosave();
    env.runTimers();
    await tick();
    ok('CORE-04 no write is sent while the project is changing',
      env.ops.filter(o => o.op === 'write').length === 0);
    env.open.res({ project: { name: 'B', path: 'P2' } });
  }

  /* A refused open leaves us where we were, with the editor usable. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1' });
    api.put('a.ts', 'work', false);
    env.$('projSel').value = 'P2';
    const p = api.openProject('P2');
    await tick();
    env.open.rej({ why: 'no such project' });
    await p.then(() => {}, () => {});
    ok('CORE-04 a refused open keeps the files it had', api.text('a.ts') === 'work');
    ok('CORE-04 a refused open hands the editor back', env.editor.opts.readOnly === false);
    ok('CORE-04 a refused open restores the selector', env.$('projSel').value === 'P1');
  }

  /* FIN-01. THE RESIDUAL CASE: AN OPEN THAT WORKED AND WAS NOT HEARD.
   *
   * Freezing the whole transition closed the gap where an edit could be made
   * that nothing would save. It did not close the transition ENDING on an
   * unknown outcome. A sent open that times out is not a refusal: the host may
   * have done it and lost the reply. The page used to treat that as "we stayed
   * put", restore the old project in the selector, thaw, and let the next save
   * go out as a bare rel path, which the host resolved against the project it
   * had actually moved to. Project A's text, written into project B, reported
   * as saved, with the screen still saying A.
   *
   * These run the real openProject, reconcileOpen, afterOpen and both save
   * paths, so a change that starts guessing again fails here. */

  /* Drop only the successful open reply. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1', gen: 1 });
    api.put('index.ts', 'project A text', false);
    env.$('projSel').value = 'P2';
    const opening = settled(api.openProject('P2'));
    await tick();
    env.open.rej({ why: 'No reply; outcome unknown', timedOut: true, sent: true });
    await tick(); await tick();

    ok('FIN-01 an unanswered open does not hand the editor back',
      env.editor.opts.readOnly === true);
    ok('FIN-01 nothing may be written while the host project is unknown',
      api.writesAllowed() === false);
    ok('FIN-01 the host is asked which project it actually has open',
      env.ops.some(o => o.op === 'status'));

    // The user types on, and both save paths are tried, which is the exact
    // sequence that used to put A's text into B.
    api.files()['index.ts'].model.setValue('new edits to the still-visible project A');
    api.dirty('index.ts');
    let held = false;
    await api.saveAllDirty().then(() => {}, e => { held = !!e.held; });
    api.scheduleAutosave();
    env.runTimers();
    await tick();
    ok('FIN-01 a save in that window is refused rather than sent', held);
    ok('FIN-01 and no write reached the host at all',
      env.ops.filter(o => o.op === 'write').length === 0);

    // No second switch may start on top of an unfinished one either.
    // Raced against a couple of ticks: a build that lets this switch through
    // holds the second open for ever, and awaiting it plainly would stop the
    // suite here instead of reporting the failure.
    let busy = false;
    await Promise.race([
      api.openProject('P3').then(() => {}, e => { busy = !!e.busy; }),
      tick().then(tick)
    ]);
    ok('FIN-01 no second switch can start while the host project is unknown', busy);
    ok('FIN-01 and no second open was sent',
      env.ops.filter(o => o.op === 'open').length === 1);

    // The host answers: it had opened P2 all along.
    env.status.res({ project: { name: 'B', path: 'P2', gen: 2 } });
    await opening;
    ok('FIN-01 the page ends up in the project the host really has',
      api.projectGen() === 2);
    ok('FIN-01 the text typed in A is kept against A, not written into B',
      (api.stranded()['P1'] || {})['index.ts'] === 'new edits to the still-visible project A');
    ok('FIN-01 nothing of A was ever written after the host moved',
      env.ops.filter(o => o.op === 'write').length === 0);
    ok('FIN-01 the editor is usable again once the host has answered',
      env.editor.opts.readOnly === false && api.writesAllowed() === true);
  }

  /* The same lost reply, but the host really did NOT switch. Now we are still
   * in A, and staying frozen would be its own bug: a dead read only window. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1', gen: 1 });
    api.put('a.ts', 'work', false);
    env.$('projSel').value = 'P2';
    const opening = settled(api.openProject('P2'));
    await tick();
    env.open.rej({ why: 'No reply; outcome unknown', timedOut: true, sent: true });
    await tick();
    env.status.res({ project: { name: 'A', path: 'P1', gen: 1 } });
    let refused = false;
    refused = !!(await opening).err;
    ok('FIN-01 a host that never moved leaves the switch refused', refused);
    ok('FIN-01 and hands the editor back', env.editor.opts.readOnly === false);
    ok('FIN-01 and puts the project we are in back in the selector',
      env.$('projSel').value === 'P1');
    ok('FIN-01 and saving works again', api.writesAllowed() === true);
    ok('FIN-01 with the file untouched', api.text('a.ts') === 'work');
  }

  /* And the host that will not answer at all. There is no safe guess left, so
   * the editor stays locked with every character still on screen. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1', gen: 1 });
    api.put('a.ts', 'work', true);
    const opening = settled(api.openProject('P2'));
    await tick(); await tick();
    env.open.rej({ why: 'No reply; outcome unknown', timedOut: true, sent: true });
    await tick();
    env.status.rej({ why: 'No reply; outcome unknown', timedOut: true, sent: true });
    await opening;
    ok('FIN-01 a host that answers nothing leaves the editor locked',
      env.editor.opts.readOnly === true && api.writesAllowed() === false);
    ok('FIN-01 and throws nothing away', api.text('a.ts') === 'work');
    ok('FIN-01 and the user is told why, not left guessing',
      env.said.some(s => /nothing has been thrown away/.test(s[1])));
  }

  /* A/B/A. The lost acknowledgment happens on the way to B; the text typed in A
   * during that switch must come back when A is opened again, and must never
   * appear anywhere near B. */
  {
    const { env, api } = makeEnv();
    api.setProject({ name: 'A', path: 'P1', gen: 1 });
    api.put('a.ts', 'A text', true);
    const toB = settled(api.openProject('P2'));
    await tick(); await tick();
    api.files()['a.ts'].model.setValue('typed in A during the switch');
    api.dirty('a.ts');
    env.open.rej({ why: 'No reply; outcome unknown', timedOut: true, sent: true });
    await tick();
    ok('FIN-01 A/B/A the host is asked what it did before anything is decided',
      env.ops.some(o => o.op === 'status'));
    env.status.res({ project: { name: 'B', path: 'P2', gen: 2 } });
    await toB;
    ok('FIN-01 A/B/A the text typed in A is kept against A',
      (api.stranded()['P1'] || {})['a.ts'] === 'typed in A during the switch');
    ok('FIN-01 A/B/A that text was never written anywhere',
      !env.ops.some(o => o.op === 'write' && o.a.text === 'typed in A during the switch'));

    const toA = settled(api.openProject('P1'));
    await tick();
    env.open.res({ project: { name: 'A', path: 'P1', gen: 3 } });
    await toA;
    ok('FIN-01 A/B/A reopening A puts that text back',
      api.text('a.ts') === 'typed in A during the switch');
    ok('FIN-01 A/B/A and the page is now stamping the newest generation',
      api.projectGen() === 3);
  }

  console.log('');
  console.log(pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
