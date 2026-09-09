// Proof for the toolbox icons.
//
// The site names a category's icon once, in cssconfig.icon, and that name is
// three things at the same time: the css class on the category row, the key a
// live capture writes, and the base name of the site's own svg. The join is by
// NAME. Nothing here hashes anything, and nothing here fetches anything.
//
// What is checked:
//   1. every icon key the toolbox names has a file in the local mirror, by
//      name, and any that does not is reported by name
//   2. the three tiers merge in the right order: a live capture wins, the
//      mirror fills whatever a partial capture missed rather than being
//      dropped, and the offline default file is the last word
//   3. the coverage number that matters: how many of the toolbox's categories
//      end up with a real picture, and how many fell back
//
// Run by roundtrip.js. Also runs alone:  node icons.js
//
// A machine with no mirror is not a failure: the mirror is user local and
// generated, and its absence is reported and skipped. The tier checks run
// either way, because they run on values, not on files.

const fs = require('fs');
const path = require('path');

const HERE = __dirname;
const BLOCKS = path.join(HERE, '..');

// ---- the mirror ------------------------------------------------------------
// Saved/BF6UnrealSDK/portalstyle/portal.battlefield.com/bf6/<build>. The plugin
// sits at Plugins/BF6UnrealSDK, so the project root is four levels up.
function findMirror() {
  const project = path.resolve(BLOCKS, '..', '..', '..', '..');
  const root = path.join(project, 'Saved', 'BF6UnrealSDK', 'portalstyle');
  const bf6 = path.join(root, 'portal.battlefield.com', 'bf6');
  if (!fs.existsSync(bf6)) return null;

  const hasIcons = dir => fs.existsSync(path.join(dir, 'assets', 'blockly', 'icons'));

  // The manifest names the build the downloader last wrote.
  try {
    const m = JSON.parse(fs.readFileSync(path.join(root, 'mirror_manifest.json'), 'utf8'));
    if (m && m.build_id) {
      const dir = path.join(bf6, String(m.build_id));
      if (hasIcons(dir)) return { dir, build: String(m.build_id), root };
    }
  } catch (e) { /* no manifest: fall through to the folders themselves */ }

  const builds = fs.readdirSync(bf6).filter(d => hasIcons(path.join(bf6, d))).sort();
  if (!builds.length) return null;
  const build = builds[builds.length - 1];
  return { dir: path.join(bf6, build), build, root };
}

// Every icon key the toolbox names, with whether that category is one the user
// ever sees. The site keeps a hidden SEARCH RESULTS bin whose icon it draws
// nowhere, and our own toolbox drops it.
function toolboxIconKeys(toolbox) {
  const out = [];
  const seen = {};
  (function walk(node, hiddenAbove) {
    if (!node) return;
    if (Array.isArray(node)) { node.forEach(n => walk(n, hiddenAbove)); return; }
    if (typeof node !== 'object') return;
    const hidden = hiddenAbove || String(node.hidden) === 'true';
    const css = node.cssconfig || node.cssConfig;
    const key = css && (css.icon || css.Icon);
    if (key && !seen[key]) {
      seen[key] = 1;
      out.push({ key: String(key), name: node.name || '', hidden: !!hidden });
    }
    if (node.contents) walk(node.contents, hidden);
  })(toolbox && toolbox.contents ? toolbox.contents : toolbox, false);
  return out;
}

// The mirror, read the way the tool reads it: one svg per name, inlined.
function mirrorIcons(dir) {
  const iconDir = path.join(dir, 'assets', 'blockly', 'icons');
  const out = { icons: {}, types: {}, files: 0 };
  fs.readdirSync(iconDir).filter(f => f.endsWith('.svg')).forEach(f => {
    const name = f.slice(0, -4);
    const svg = fs.readFileSync(path.join(iconDir, f), 'utf8');
    const url = 'data:image/svg+xml,' + encodeURIComponent(svg);
    out.files++;
    if (name.indexOf('type-') === 0) out.types[name] = url;
    else out.icons[name] = url;
  });
  return out;
}

function run(Blockly, BF6) {
  let checks = 0;
  const fails = [];
  const notes = [];
  const check = (cond, what) => { checks++; if (!cond) fails.push(what); };

  // toolbox_fallback.js binds to the page's global and exports itself in node.
  const toolbox = require(path.join(BLOCKS, 'vendor', 'toolbox_fallback.js'));
  check(!!(toolbox && toolbox.contents), 'toolbox_fallback.js carries no toolbox');
  if (!toolbox) return { ok: false, checks, failures: fails };

  const keys = toolboxIconKeys(toolbox);
  const shown = keys.filter(k => !k.hidden);
  notes.push('  ' + keys.length + ' icon keys in the toolbox, ' + shown.length + ' on categories the user sees');

  const mirror = findMirror();
  let mirrorMap = { icons: {}, types: {}, files: 0 };
  if (!mirror) {
    notes.push('  no local site mirror on this machine: the file check is skipped, ' +
      'the style downloader fills the icons in');
  } else {
    mirrorMap = mirrorIcons(mirror.dir);
    notes.push('  mirror build ' + mirror.build + ': ' + mirrorMap.files + ' svg files (' +
      Object.keys(mirrorMap.icons).length + ' toolbox, ' + Object.keys(mirrorMap.types).length + ' value type)');

    // 1. every key the toolbox names, against a file of that name
    const missing = shown.filter(k => !mirrorMap.icons[k.key] && !mirrorMap.types[k.key]);
    check(missing.length === 0,
      'toolbox keys with no svg in the mirror: ' + missing.map(m => m.key + ' (' + m.name + ')').join(', '));
    notes.push('  ' + (shown.length - missing.length) + ' of ' + shown.length +
      ' visible category keys have a file of that name in the mirror');

    const hiddenMissing = keys.filter(k => k.hidden && !mirrorMap.icons[k.key] && !mirrorMap.types[k.key]);
    if (hiddenMissing.length) {
      notes.push('  hidden categories with no file, which the toolbox never draws: ' +
        hiddenMissing.map(m => m.key).join(', '));
    }

    // Nothing in the mirror is orphaned either: a name the toolbox never asks
    // for is worth knowing about, because it means the toolbox moved.
    const asked = {};
    keys.forEach(k => { asked[k.key] = 1; });
    const unasked = Object.keys(mirrorMap.icons).filter(n => !asked[n]);
    if (unasked.length) notes.push('  mirror icons no toolbox category asks for: ' + unasked.join(', '));
  }

  // ---- 2. the tiers ---------------------------------------------------------
  // A live capture that got two categories and nothing else. The mirror must
  // fill the rest, and must not overwrite the two.
  const liveOnly = {
    'class:toolbox-rules': { backgroundImage: 'url("data:image/svg+xml;base64,LIVE")', width: '20px', height: '20px' },
    'row:RULES': { background: 'rgb(17,19,20)', colour: 'rgb(174,192,204)', selected: false },
    'class:toolbox-actions-ai': { backgroundImage: 'url("data:image/svg+xml;base64,LIVE2")' }
  };
  const fallbackTier = { 'toolbox-rules': 'data:image/svg+xml;base64,OLD', 'toolbox-values-math': 'data:image/svg+xml;base64,OLD2' };

  const merged = BF6.mergeIconTiers({ live: liveOnly, mirror: mirrorMap.icons, fallback: fallbackTier });
  check(merged.by['toolbox-rules'] === 'live', 'a live icon lost to a lower tier');
  check(merged.icons['class:toolbox-rules'].backgroundImage === 'url("data:image/svg+xml;base64,LIVE")',
    'the live icon was overwritten');
  check(merged.tiers.live === 2, 'live tier count: expected 2, got ' + merged.tiers.live);
  check(!!merged.icons['row:RULES'], 'the live row colours were dropped by the merge');
  check(merged.source === 'live', 'a merge with live icons in it did not say live');

  if (mirror) {
    check(merged.by['toolbox-values-math'] === 'mirror',
      'the mirror did not fill a key the live capture missed');
    check(merged.tiers.mirror === Object.keys(mirrorMap.icons).length - 2,
      'mirror tier count: expected ' + (Object.keys(mirrorMap.icons).length - 2) +
      ', got ' + merged.tiers.mirror);
    check(merged.tiers.fallback === 0,
      'the offline default was used for a key the mirror already answered');
  }

  // The mirror alone, with no capture at all.
  const noLive = BF6.mergeIconTiers({ live: null, mirror: mirrorMap.icons, fallback: fallbackTier });
  if (mirror) {
    check(noLive.source === 'mirror', 'with no capture the merge did not say mirror');
    check(noLive.by['toolbox-rules'] === 'mirror', 'the mirror did not answer with no capture');
  }

  // Neither: the offline default is the last word, and it must still answer.
  const last = BF6.mergeIconTiers({ live: null, mirror: null, fallback: fallbackTier });
  check(last.source === 'fallback', 'with nothing but the offline default the merge did not say fallback');
  check(last.tiers.fallback === 2, 'the offline default answered ' + last.tiers.fallback + ' of 2 keys');
  check(!!last.icons['class:toolbox-rules'].backgroundImage.match(/^url\("data:/),
    'a bare data url was not wrapped for css');

  // Nothing at all, which is a machine with no mirror and no capture ever.
  const none = BF6.mergeIconTiers({});
  check(none.source === 'none', 'an empty merge did not say none');
  check(Object.keys(none.icons).length === 0, 'an empty merge invented icons');

  // ---- 3. coverage ----------------------------------------------------------
  // The number worth reporting: what the toolbox asked for, against what the
  // tiers could answer.
  const style = BF6.normalizeStyle({
    source: 'default',
    categoryIcons: liveOnly,
    mirrorIcons: mirrorMap.icons,
    mirrorValueTypeIcons: mirrorMap.types,
    icons: fallbackTier
  });
  const cov = BF6.iconCoverage(toolbox, style);
  check(cov.categories === keys.length,
    'coverage counted ' + cov.categories + ' categories, the toolbox names ' + keys.length);
  notes.push('  coverage: ' + cov.resolved + ' of ' + cov.categories + ' categories drawn (' +
    cov.byTier.live + ' site, ' + cov.byTier.mirror + ' mirror, ' + cov.byTier.fallback +
    ' offline default), ' + cov.missing.length + ' fell through' +
    (cov.missing.length ? ': ' + cov.missing.join(', ') : ''));
  if (mirror) {
    check(cov.resolved >= shown.length,
      'only ' + cov.resolved + ' of ' + shown.length + ' visible categories resolved to a picture');
  }

  // The style payload carries the rest of what the page needs, and normalizing
  // must not lose it.
  const withMedia = BF6.normalizeStyle({
    source: 'mirror',
    mirrorIcons: mirrorMap.icons,
    blockImages: { '1x1.png': 'data:image/png;base64,AAA', 'quote0.png': 'data:image/png;base64,BBB' },
    mediaPath: 'file:///C:/mirror/assets/blockly/',
    fontCss: '@font-face{font-family:"BFText-Regular";src:url("file:///C:/f.woff2") format("woff2")}',
    fonts: ['BFText-Regular', 'BFText-Bold'],
    fontsMissing: ['Purista-Semibold'],
    fontAliases: { 'Purista-Semibold': 'BF_TITLE_SEMI-BOLD' }
  });
  check(withMedia.mediaPath === 'file:///C:/mirror/assets/blockly/', 'the media path was lost');
  check(withMedia.blockImages && withMedia.blockImages['quote0.png'] === 'data:image/png;base64,BBB',
    'a block image was lost');
  check(withMedia.fontCss.indexOf('@font-face') === 0, 'the font css was lost');
  check(withMedia.fontsMissing[0] === 'Purista-Semibold',
    'the unmirrorable face was not reported');
  check(withMedia.fontAliases['Purista-Semibold'] === 'BF_TITLE_SEMI-BOLD',
    'the substitute face was lost');

  const line = BF6.iconSourceLine(BF6.styleSummary());
  check(typeof line === 'string' && line.length > 0, 'the icon source line came out empty');

  console.log('icon checks       ', checks, 'run,', fails.length, 'failed');
  notes.forEach(n => console.log(n));
  fails.slice(0, 20).forEach(f => console.log('    FAIL', f));
  return { ok: fails.length === 0, checks: checks, failures: fails };
}

module.exports = { run, findMirror, toolboxIconKeys, mirrorIcons };

if (require.main === module) {
  const STYLE = require('./style.js');
  const Blockly = STYLE.loadBlockly();
  const BF6 = require(path.join(BLOCKS, 'editor.js'));
  BF6.attach(Blockly);
  const r = run(Blockly, BF6);
  console.log(r.ok ? 'ICONS OK' : 'ICONS FAILED');
  process.exit(r.ok ? 0 : 1);
}
