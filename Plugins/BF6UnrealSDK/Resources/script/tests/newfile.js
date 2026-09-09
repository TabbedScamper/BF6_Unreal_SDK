/* CORE-06, as executable checks against the real NEW FILE handler and the real
 * openFile/onFileText out of editor.js.
 *
 * THE BUG, in two halves.
 *
 * One: NEW FILE wrote a one line stub at whatever name was typed. Reading the
 * file first was added, but a read that FAILS is not evidence that a file is
 * absent - it fails for a locked file, a refused permission, a path the host
 * would not take and a reply that never came - and every one of those fell
 * through to the write. Type index.ts and the project's entry point became
 * "// src/index.ts".
 *
 * Two: a read that SUCCEEDS delivers its text as a payload, not in its reply,
 * and that payload lands in openFile. openFile called setValue on whatever
 * model was already there, so opening a file with unsaved edits replaced them
 * with the older bytes from disk, in one undo step, with nothing said.
 */
const fs = require('fs');
const path = require('path');

const F = path.join(__dirname, '..', 'editor.js');
const src = fs.readFileSync(F, 'utf8');

function braces(from) {
  let d = 0;
  const i = src.indexOf('{', from);
  for (let j = i; j < src.length; j++) {
    if (src[j] === '{') d++;
    else if (src[j] === '}') { d--; if (!d) return src.slice(from, j + 1); }
  }
  throw new Error('unbalanced at ' + from);
}
function grab(name) {
  const at = src.indexOf('function ' + name + '(');
  if (at < 0) throw new Error('no ' + name);
  return braces(at);
}
/* The button handlers are assignments, not declarations, so they are taken by
 * their left hand side. Renaming the button would fail here rather than
 * silently testing nothing. */
function grabHandler(id) {
  const marker = "$('" + id + "').onclick = function () ";
  const at = src.indexOf(marker);
  if (at < 0) throw new Error('no handler for ' + id);
  return 'function handler() ' + braces(at + marker.length - 1).replace(/^[^{]*/, '');
}

function makeEnv(opts) {
  const env = { ops: [], said: [], created: [], typed: opts.typed, listing: opts.listing,
    listingFails: !!opts.listingFails, readFails: !!opts.readFails };

  function call(op, a) {
    env.ops.push({ op, a });
    if (op === 'files') {
      return env.listingFails
        ? Promise.reject({ why: 'the tool did not answer' })
        : Promise.resolve({ ok: true, files: env.listing });
    }
    /* The host op that decides existence next to the write, so there is no gap
     * between asking and writing. It refuses with reason "exists" and never
     * replaces anything; `listingFails` stands in for the host being unable to
     * answer at all, which must still create nothing. */
    if (op === 'newfile') {
      if (env.listingFails) { return Promise.reject({ why: 'the tool did not answer' }); }
      const taken = (env.listing || []).some(
        f => String(f).toLowerCase() === String(a.rel).toLowerCase());
      if (taken) {
        return Promise.reject({ ok: false, reason: 'exists',
                                why: a.rel + ' already exists in this project, and was not touched.' });
      }
      /* Records what the host would actually have put on disk. The refusal
       * checks assert against THIS rather than against "no write op was sent":
       * once creating moved into a single op, asserting the absence of the old
       * one passed no matter what the code did. */
      env.created.push(a.rel);
      return Promise.resolve({ ok: true });
    }
    if (op === 'read') {
      // Exactly what the host does: the text comes back as a payload, and the
      // reply itself carries no content at all. It fails for a locked file or a
      // refused permission just as readily as for one that is not there.
      return env.readFails
        ? Promise.reject({ why: 'Could not read ' + a.rel + '.' })
        : Promise.resolve({ ok: true });
    }
    return Promise.resolve({ ok: true });
  }

  const monaco = {
    Uri: { parse: s => s },
    editor: {
      getModel: () => null,
      createModel: v => ({ v, getValue() { return this.v; }, setValue(x) { this.v = x; } })
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
    'var files = {}, activeRel = null, quietReads = {}, reloadReads = {};',
    'var project = { name: "A", path: "P1" };',
    grab('openFile'),
    grab('onFileText'),
    grabHandler('btnNewFile'),
    'return { newFile: handler, onFileText: onFileText, openFile: openFile,',
    '  files: function () { return files; }, active: function () { return activeRel; },',
    '  reload: function (rel) { reloadReads[rel] = 1; },',
    '  put: function (rel, text, dirty) { files[rel] = { model: monaco.editor.createModel(text), dirty: !!dirty }; },',
    '  text: function (rel) { return files[rel] && files[rel].model.getValue(); },',
    '  noProject: function () { project = null; } };'
  ].join('\n');

  const f = new Function('call', 'say', 'drawTabs', 'scheduleGutter', 'editor', 'monaco',
    'refreshFiles', 'window', 'Promise', body);

  const api = f(call, (t, l) => env.said.push([l, t]), () => {}, () => {},
    { setModel() {} }, monaco, () => Promise.resolve(),
    { prompt: () => env.typed, __bf6PendingReads: {} }, Promise);
  return { env, api };
}

const settle = () => new Promise(r => setTimeout(r, 0));
let pass = 0, fail = 0;
const ok = (n, c) => { if (c) { pass++; console.log('  PASS  ' + n); } else { fail++; console.log('  FAIL  ' + n); } };

(async () => {
  /* THE DEFECT ITSELF. An existing name must never be written. */
  {
    const { env, api } = makeEnv({ typed: 'index.ts', listing: ['src/index.ts', 'src/strings.json'] });
    api.newFile();
    await settle(); await settle();
    ok('CORE-06 an existing file is not written over',
      env.created.length === 0);
    ok('CORE-06 it is opened instead, and the user is told why',
      env.said.some(s => /already exists/.test(s[1])));
  }

  /* THE ORIGINAL PATH, exactly. The file is there and the read of it fails,
   * which is what a locked file, a refused permission or a host that will not
   * take the path all look like from here. This is the case that used to fall
   * straight through to the write. */
  {
    const { env, api } = makeEnv({ typed: 'index.ts', listing: ['src/index.ts'], readFails: true });
    api.newFile();
    await settle(); await settle(); await settle();
    ok('CORE-06 a file that exists but cannot be read is still not written over',
      env.created.length === 0);
    ok('CORE-06 and the failure is reported rather than swallowed',
      env.said.some(s => s[0] === 'e' && /could not open/.test(s[1])));
  }

  /* Windows does not care about case, so neither may this. */
  {
    const { env, api } = makeEnv({ typed: 'INDEX.TS', listing: ['src/index.ts'] });
    api.newFile();
    await settle(); await settle();
    ok('CORE-06 a case-equivalent Windows name is the same file',
      env.created.length === 0);
  }

  /* And the same, typed with the src/ prefix and a backslash. */
  {
    const { env, api } = makeEnv({ typed: 'src\\index.ts', listing: ['src/index.ts'] });
    api.newFile();
    await settle(); await settle();
    ok('CORE-06 src\\index.ts is src/index.ts', env.created.length === 0);
  }

  /* Not being able to check is not permission to write. */
  {
    const { env, api } = makeEnv({ typed: 'index.ts', listing: [], listingFails: true });
    api.newFile();
    await settle(); await settle();
    ok('CORE-06 a failed existence check writes nothing',
      env.created.length === 0);
    ok('CORE-06 and says so rather than reporting a new file',
      env.said.some(s => s[0] === 'e' && /Nothing was changed/.test(s[1])));
  }

  /* Any extension is fine now that the host answers for the path. The old
   * restriction to .ts and .json existed only because the file listing could
   * not speak for anything else, and the newfile op can. */
  {
    const { env, api } = makeEnv({ typed: 'notes.md', listing: [] });
    api.newFile();
    await settle(); await settle();
    ok('CORE-06 an extension the old listing could not cover is now allowed',
      env.ops.some(o => o.op === 'newfile' && o.a.rel === 'src/notes.md'));
  }

  /* The ordinary case still works, and still works with the extension left off. */
  {
    const { env, api } = makeEnv({ typed: 'rules/scoring.ts', listing: ['src/index.ts'] });
    api.newFile();
    await settle(); await settle();
    const w = env.ops.filter(o => o.op === 'newfile');
    ok('CORE-06 a genuinely new file is created', w.length === 1 && w[0].a.rel === 'src/rules/scoring.ts');
    ok('CORE-06 and opens on its stub', api.text('src/rules/scoring.ts') === '// src/rules/scoring.ts\n');
    /* The whole point of the op: one call, so nothing can slip in between a
     * check and a write. */
    ok('CORE-06 creating a file is a single host call',
      !env.ops.some(o => o.op === 'write' || o.op === 'files'));
  }
  {
    const { env, api } = makeEnv({ typed: 'rules/scoring', listing: [] });
    api.newFile();
    await settle(); await settle();
    ok('CORE-06 a name with no extension becomes a .ts file',
      env.ops.some(o => o.op === 'newfile' && o.a.rel === 'src/rules/scoring.ts'));
  }

  /* A path that leaves src is still refused, as it always was. */
  {
    const { env, api } = makeEnv({ typed: '../../secrets.ts', listing: [] });
    api.newFile();
    await settle();
    ok('CORE-06 a path that climbs out of src is refused',
      !env.ops.length && env.said.some(s => s[0] === 'e'));
  }

  /* THE SECOND HALF. The payload from a read must not replace unsaved text. */
  {
    const { api } = makeEnv({ typed: '', listing: [] });
    api.put('src/index.ts', 'what the user typed', true);
    api.onFileText('src/index.ts', 'the older copy on disk');
    ok('CORE-06 a disk payload does not replace an unsaved model',
      api.text('src/index.ts') === 'what the user typed');
    ok('CORE-06 the file stays dirty, so it is still going to be saved',
      api.files()['src/index.ts'].dirty === true);
    ok('CORE-06 and it is still focused, which is what opening it meant',
      api.active() === 'src/index.ts');
  }

  /* A model with no unsaved edits is a copy of the disk, so loading is fine. */
  {
    const { api } = makeEnv({ typed: '', listing: [] });
    api.put('src/index.ts', 'stale', false);
    api.onFileText('src/index.ts', 'newer from disk');
    ok('CORE-06 a clean model still loads the file', api.text('src/index.ts') === 'newer from disk');
  }

  /* The one exception: the tool rewrote the file itself, so the disk copy is
   * the newer one and is allowed past. The permission is consumed. */
  {
    const { api } = makeEnv({ typed: '', listing: [] });
    api.put('src/index.ts', 'what the user typed', true);
    api.reload('src/index.ts');
    api.onFileText('src/index.ts', 'rewritten by the tool');
    ok('CORE-06 a rewrite the tool made itself may replace the model',
      api.text('src/index.ts') === 'rewritten by the tool');
    api.files()['src/index.ts'].dirty = true;
    api.onFileText('src/index.ts', 'a later ordinary read');
    ok('CORE-06 that permission is used once and not kept',
      api.text('src/index.ts') === 'rewritten by the tool');
  }

  console.log('');
  console.log(pass + ' passed, ' + fail + ' failed');
  process.exit(fail ? 1 : 0);
})();
