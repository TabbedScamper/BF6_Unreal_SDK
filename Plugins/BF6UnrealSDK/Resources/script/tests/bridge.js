/* CORE-01, as executable checks against the real bridge bodies.
 *
 * THE BUG. editor.js is one long function. It declared "var pending = {}" for
 * the table of calls awaiting a reply, and, three thousand lines later,
 * "var pending = { state: 'none' }" for the build banner. var hoists, so those
 * were never two variables. The banner's declaration wiped the request table on
 * the way past, and onPending() replaced it outright on every status event.
 *
 * A call in flight when a status event arrived therefore lost its resolve and
 * reject, and its promise never settled: the build finished and the buttons
 * stayed disabled with nothing to press.
 *
 * These run the real call/settle/reply/onPending out of editor.js, so a rename
 * that reintroduced the collision would fail here.
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

/* The two declarations, taken verbatim so their names are whatever the source
 * actually says. If they collide again, they collide here too. */
function grabDecl(re, what) {
  const m = src.match(re);
  if (!m) throw new Error('no ' + what);
  return m[0];
}
const declPending = grabDecl(/var pending = \{\};/, 'the request table');
const declBuild = grabDecl(/var \w+ = \{ state: 'none' \};/, 'the banner state');

function makeEnv() {
  const env = { sent: [], banner: 0 };
  const body = [
    'var seq = 1;',
    declPending,
    grab('settle').replace(/^function/, 'function'),
    'var CALL_TIMEOUT_MS = 50;',            // short, so the timeout case is testable
    'var heldCalls = [];',
    // The binding can be absent, which is the case that queues a call. `online`
    // flips it so a test can start offline and connect later.
    'var online = true;',
    'function bridge() { return online ? { call: function (m) { sent.push(JSON.parse(m)); } } : null; }',
    // FIN-01. call() stamps the open project onto every request that means "in
    // the project I have open". The real stampProject body is used, and the
    // variable it reads is declared here rather than faked, so a change to
    // either shows up in these checks.
    'var project = null;',
    grab('stampProject'),
    grab('call'),
    grab('drainHeld'),
    declBuild,
    'function refreshBanner() { banner.n++; }',
    grab('onPending'),
    'function reply(r) { settle(r.id, r.ok === false ? "reject" : "resolve", r); }',
    'return { call: call, reply: reply, onPending: onPending, drainHeld: drainHeld,',
    '         setProject: function (p) { project = p; },',
    '         goOffline: function () { online = false; }, goOnline: function () { online = true; },',
    '         heldCount: function () { return heldCalls.length; },',
    '         pendingCount: function () { return Object.keys(pending).length; } };',
  ].join('\n');
  const banner = { n: 0 };
  const f = new Function("sent", "banner", "Promise", "setTimeout", "clearTimeout", body);
  return { env, banner, api: f(env.sent, banner, Promise, setTimeout, clearTimeout) };
}

let pass = 0, fail = 0;
const ok = (n, c) => { if (c) { pass++; console.log('  PASS  ' + n); } else { fail++; console.log('  FAIL  ' + n); } };

(async () => {
  /* The declarations must not be the same name. This is the defect itself. */
  {
    const nameOf = d => d.match(/var (\w+)/)[1];
    ok('CORE-01 the request table and the banner state are different variables',
      nameOf(declPending) !== nameOf(declBuild));
  }

  /* A status event mid-flight must not strand the call. */
  {
    const { api } = makeEnv();
    const p = api.call('build');
    ok('CORE-01 the call is waiting', api.pendingCount() === 1);
    api.onPending({ state: 'built', at: 'now' });          // the interleaved event
    ok('CORE-01 a status event does not discard the waiting call',
      api.pendingCount() === 1);
    api.reply({ id: 1, ok: true, out: 'built' });
    const r = await p;
    ok('CORE-01 the build promise settles after the status event', r.out === 'built');
  }

  /* Interleave a whole build, save and push against repeated status events. */
  {
    const { api } = makeEnv();
    const build = api.call('build');
    api.onPending({ state: 'built' });
    const save = api.call('save');
    api.onPending({ state: 'built' });
    const push = api.call('push');
    api.onPending({ state: 'pushed' });
    ok('CORE-01 three calls survive three status events', api.pendingCount() === 3);
    api.reply({ id: 2, ok: true });
    api.reply({ id: 1, ok: true });
    api.reply({ id: 3, ok: true });
    await Promise.all([build, save, push]);
    ok('CORE-01 every interleaved promise settled', api.pendingCount() === 0);
  }

  /* A failure releases the caller rather than hanging it. */
  {
    const { api } = makeEnv();
    const p = api.call('push');
    api.reply({ id: 1, ok: false, error: 'no session' });
    let why = null;
    await p.then(() => {}, e => { why = e.error; });
    ok('CORE-01 a refused call rejects so busy controls can be released',
      why === 'no session');
  }

  /* Settled once, whatever arrives afterwards. */
  {
    const { api } = makeEnv();
    const p = api.call('build');
    api.reply({ id: 1, ok: true, out: 'first' });
    api.reply({ id: 1, ok: false, error: 'late duplicate' });
    const r = await p;
    ok('CORE-01 a duplicate reply cannot re-settle the promise', r.out === 'first');
  }

  /* And a reply that never comes ends, rather than waiting forever. */
  {
    const { api } = makeEnv();
    const p = api.call('build');
    let timedOut = false;
    await p.then(() => {}, e => { timedOut = !!e.timedOut; });
    ok('CORE-01 a call the tool never answers rejects instead of hanging', timedOut);
    ok('CORE-01 the timed-out call left the table', api.pendingCount() === 0);
  }

  /* INT-06. A call made before the bridge exists is queued. If it then times
   * out, it must leave the queue too: replaying it after the user was told it
   * never happened is the worst outcome available here. */
  {
    const { api, env } = makeEnv();
    api.goOffline();
    const p = api.call('write');
    ok('CORE-01 an unsent call is queued', api.heldCount() === 1);
    let msg = null, claimedSent = null;
    await p.then(() => {}, e => { msg = e.error; claimedSent = e.sent; });
    ok('INT-06 the expired call left the queue', api.heldCount() === 0);
    api.goOnline();
    api.drainHeld();
    ok('INT-06 draining does not send the expired write', env.sent.length === 0);
    ok('INT-06 an unsent call may say nothing changed',
      claimedSent === false && /never reached/.test(msg || ''));
  }

  /* And the other half: a call that WAS sent must not claim nothing changed,
   * because we have no evidence either way. */
  {
    const { api } = makeEnv();
    const p = api.call('write');
    let msg = null, claimedSent = null;
    await p.then(() => {}, e => { msg = e.error; claimedSent = e.sent; });
    ok('INT-06 a sent call reports its outcome as unknown',
      claimedSent === true && /unknown/.test(msg || '') && !/Nothing was changed/.test(msg || ''));
  }

  /* A queued call that is delivered normally still settles. The guard must not
   * break the ordinary offline-then-connected path it sits in. */
  {
    const { api, env } = makeEnv();
    api.goOffline();
    const p = api.call('save');
    api.goOnline();
    api.drainHeld();
    ok('INT-06 a queued call is still delivered when the bridge appears',
      env.sent.length === 1 && env.sent[0].op === 'save');
    api.reply({ id: env.sent[0].id, ok: true, out: 'saved' });
    const r = await p;
    ok('INT-06 and it settles normally', r.out === 'saved');
  }

  /* FIN-01. A request that means "in the project I have open" has to SAY which
   * project that is. Without it the host resolved a bare rel path against
   * whatever it had open, which is a different project whenever the two halves
   * have drifted apart, and a save then landed in a project the user was not
   * looking at. The host refuses a request naming a project it has moved past,
   * so the stamp is the whole of the page's side of that. */
  {
    const { api, env } = makeEnv();
    api.setProject({ name: 'A', path: 'P1', gen: 4 });
    api.call('write', { rel: 'a.ts', text: 'x' });
    const w = env.sent[env.sent.length - 1];
    ok('FIN-01 a write names the project it is for',
      w.op === 'write' && w.proj === 'P1' && w.projGen === 4);

    api.call('build');
    const b = env.sent[env.sent.length - 1];
    ok('FIN-01 so does everything else that acts on the open project',
      b.proj === 'P1' && b.projGen === 4);

    // open, new, projects and status are how a project gets CHOSEN. Stamping
    // them would make the host refuse the very request that moves it on.
    api.call('open', { path: 'P2' });
    const o = env.sent[env.sent.length - 1];
    ok('FIN-01 the request that CHANGES project is not stamped',
      o.op === 'open' && o.proj === undefined && o.projGen === undefined);

    api.call('status');
    const s = env.sent[env.sent.length - 1];
    ok('FIN-01 nor is status, which is how the page asks what the host has open',
      s.op === 'status' && s.proj === undefined);
  }

  /* A generation of zero is a real generation: the host's first open. Sending
   * it as "no generation" would leave the very first project's writes
   * unstamped, and the host refuses an unstamped write. */
  {
    const { api, env } = makeEnv();
    api.setProject({ name: 'A', path: 'P1', gen: 0 });
    api.call('write', { rel: 'a.ts', text: 'x' });
    const w = env.sent[env.sent.length - 1];
    ok('FIN-01 generation zero is still stamped', w.proj === 'P1' && w.projGen === 0);
  }

  console.log('');
  console.log(pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
