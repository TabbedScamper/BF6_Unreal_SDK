/* The area recipe, run rather than read.
 *
 * THE BUG. The recipe warns a player who walks into the enemy spawn, waits five
 * seconds, and kills them if they are still inside. The wait does not cancel
 * itself, so the handler has to ask on waking whether the punishment still
 * applies. It used to ask "are they inside?", which is the wrong question:
 *
 *   t=0  enter        warned, handler A sleeps until t=5
 *   t=2  leave        "you are clear"
 *   t=3  enter again  warned, handler B sleeps until t=8
 *   t=5  A wakes      they ARE inside, so A kills them
 *
 * That kill lands two seconds into a grace period that is supposed to be five,
 * for a player who did exactly what they were told. The question has to be "is
 * this still MY visit?", which is what the visit generation answers.
 *
 * This lifts the recipe's own code out of guide.js and runs it, so the test is
 * about what the tool actually hands people rather than a copy of it.
 *
 * Run:  node recipes.js
 */
const fs = require('fs');
const path = require('path');

const GUIDE = path.join(__dirname, '..', 'guide.js');
const src = fs.readFileSync(GUIDE, 'utf8');

/* The recipe is R('areas', title, blurb, provenance, [[code, note], ...], {...}).
 * Take the code column of the pairs, in order. */
function recipeCode(id) {
  const at = src.indexOf("R('" + id + "'");
  if (at < 0) throw new Error('no recipe ' + id);
  // The code/note pairs are the first array-of-arrays after the marker.
  const start = src.indexOf('[\n', src.indexOf('[{ label:', at));
  let depth = 0, end = -1;
  for (let i = start; i < src.length; i++) {
    if (src[i] === '[') depth++;
    else if (src[i] === ']') { depth--; if (!depth) { end = i; break; } }
  }
  if (end < 0) throw new Error('unbalanced recipe body for ' + id);
  const block = src.slice(start, end + 1);
  const lines = [];
  // Each pair opens with the code string. Both quote styles appear, and code
  // lines contain escaped quotes, so this walks the string properly rather
  // than trying to match the whole pair with one expression.
  const re = /\n\s*\[('(?:[^'\\]|\\.)*'|"(?:[^"\\]|\\.)*")\s*,/g;
  let m;
  while ((m = re.exec(block)) !== null) {
    // eslint-disable-next-line no-eval
    lines.push(eval(m[1]));
  }
  return lines.join('\n');
}

/* Enough of TypeScript removed to run it. Deliberately narrow: if the recipe
 * grows syntax this does not handle, the test fails loudly rather than testing
 * something that is not the recipe. */
function stripTypes(ts) {
  return ts
    .split('\n')
    .filter(l => !/^\s*import\b/.test(l))
    .map(l => l
      .replace(/new (Map|Set)<[^>]*>\(\)/g, 'new $1()')
      .replace(/\basync function (\w+)\(([^)]*)\):\s*Promise<void>\s*\{/,
        (_, n, args) => 'async function ' + n + '(' + args.replace(/:\s*[\w.]+/g, '') + ') {')
      .replace(/\bfunction (\w+)\(([^)]*)\):\s*\w+\s*\{/,
        (_, n, args) => 'function ' + n + '(' + args.replace(/:\s*[\w.]+/g, '') + ') {'))
    .join('\n');
}

let pass = 0, fail = 0;
const ok = (n, c) => { if (c) { pass++; console.log('  PASS  ' + n); } else { fail++; console.log('  FAIL  ' + n); } };

(async () => {
  const code = recipeCode('areas');

  /* First, the shape. A membership-only check is the defect itself, so it is
   * named here as well as caught behaviourally below. */
  ok('CORE-16 the wait is guarded by a visit number, not by membership',
    /visit\.get\(id\) !== mine/.test(code) && !/inside\.has\(/.test(code));

  /* Now run it. Time is simulated, so the test is instant and exact. */
  const enemyTeam = { t: 2 };
  let now = 0;
  const sleepers = [];
  const killed = [];
  const said = [];

  const mod = {
    GetObjId: o => o.id,
    GetTeam: p => (typeof p === 'number' ? { t: p } : p.team),
    Equals: (a, b) => a.t === b.t,
    IsPlayerValid: () => true,
    Message: s => s,
    stringkeys: { myMod: { leaveArea: 'leave', areaClear: 'clear' } },
    DisplayNotificationMessage: (m, p) => said.push([now, p.id, m]),
    Kill: p => killed.push([now, p.id]),
    Wait: s => new Promise(res => sleepers.push({ at: now + s, res })),
  };
  const handlers = {};
  const Events = {
    OnPlayerEnterAreaTrigger: { subscribe: f => { handlers.enter = f; } },
    OnPlayerExitAreaTrigger: { subscribe: f => { handlers.exit = f; } },
  };

  // eslint-disable-next-line no-new-func
  new Function('mod', 'Events', stripTypes(code))(mod, Events);
  ok('CORE-16 the recipe wires both events',
    typeof handlers.enter === 'function' && typeof handlers.exit === 'function');

  /* Advance simulated time, releasing any wait that has come due. */
  async function advance(to) {
    while (now < to) {
      now++;
      const due = sleepers.filter(s => s.at <= now);
      for (const s of due) { sleepers.splice(sleepers.indexOf(s), 1); s.res(); }
      await new Promise(r => setTimeout(r, 0));
      await new Promise(r => setTimeout(r, 0));
    }
  }

  const area = { id: 201 };
  const intruder = { id: 7, team: enemyTeam };

  // THE CASE THAT WAS BROKEN: leave, then come back.
  handlers.enter(intruder, area);
  await advance(2);
  handlers.exit(intruder, area);
  await advance(3);
  handlers.enter(intruder, area);      // second visit begins at t=3
  await advance(5);
  ok('CORE-16 the first visit does not kill after a re-entry', killed.length === 0);
  await advance(7);
  ok('CORE-16 and the second visit is still in its own grace period', killed.length === 0);
  await advance(8);
  ok('CORE-16 the second visit kills five seconds after IT began',
    killed.length === 1 && killed[0][0] === 8);

  /* The ordinary cases still behave. */
  {
    now = 0; sleepers.length = 0; killed.length = 0;
    const p = { id: 9, team: enemyTeam };
    handlers.enter(p, area);
    await advance(2);
    handlers.exit(p, area);
    await advance(9);
    ok('CORE-16 leaving and staying out is never punished', killed.length === 0);
  }
  {
    now = 0; sleepers.length = 0; killed.length = 0;
    const p = { id: 11, team: enemyTeam };
    handlers.enter(p, area);
    await advance(5);
    ok('CORE-16 standing in it is punished, on time',
      killed.length === 1 && killed[0][0] === 5);
  }
  {
    now = 0; sleepers.length = 0; killed.length = 0;
    const owner = { id: 12, team: { t: 1 } };   // OWNER_TEAM
    handlers.enter(owner, area);
    await advance(9);
    ok('CORE-16 the owning team is left alone', killed.length === 0);
  }
  {
    now = 0; sleepers.length = 0; killed.length = 0;
    const p = { id: 13, team: enemyTeam };
    handlers.enter(p, { id: 999 });             // a different trigger
    await advance(9);
    ok('CORE-16 another area trigger does not fire this rule', killed.length === 0);
  }

  console.log('');
  console.log(pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
