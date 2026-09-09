// ===========================================================================
// BF6 Unreal SDK - Portal SETTINGS page half.
//
// Injected into every page the tool's embedded browser loads, beside
// capture.js. Three jobs, and only three:
//
//   1. SCRAPE   read the settings page the panel is on - every control's
//               data-testid, kind, current value, range, options and caption -
//               so a site update that adds or renames a setting reaches the
//               tool without a tool release.
//   2. APPLY    set a control THE WAY A USER WOULD: click the toggle, type in
//               the slider's number box (or drive the range input's own value
//               setter and fire the events React listens for), open the
//               dropdown and click the option. Then read the page back and
//               report what it actually became.
//   3. SAVE     click the site's OWN save button and report the verdict: the
//               gRPC trailer status of the updatePlayElement the SITE sent,
//               and the toast the site showed.
//
// WHAT THIS FILE NEVER DOES. It never builds an updatePlayElement message. A
// partial one wipes the experience's attachments, so every change goes through
// the site's own controls and the site composes its own save. It never reads,
// stores or forwards a password, a token, a cookie or a request header.
// Publishing is never automated: the publish button is the user's click.
//
// It also decodes the mutators out of the getPlayElement response the page
// already received, so the tool can show the experience's LIVE values without
// a second request. Only the decoded {key: {default, teams}} map crosses the
// bridge - never the body, never a header.
// ===========================================================================
(function () {
  var V = 1;
  if (window.BF6PortalSettings && window.BF6PortalSettings.v === V) {
    window.BF6PortalSettings.rearm();
    return;
  }

  var WEBPLAY = '/santiago.web.play.WebPlay/';
  var noop = function () {};
  var queue = [];

  // ---- the bridge ---------------------------------------------------------

  function bridge() {
    try { return (window.ue && window.ue.bf6portalsettings) ? window.ue.bf6portalsettings : null; }
    catch (e) { return null; }
  }

  function drain() {
    var b = bridge();
    if (!b) { setTimeout(drain, 250); return; }
    while (queue.length) {
      var it = queue.shift();
      try { b.report(it); }
      catch (e) { console.log('BF6SETTINGS bridge call failed: ' + e); }
    }
  }

  function send(obj) {
    var s;
    try { obj.v = V; obj.url = location.href; s = JSON.stringify(obj); }
    catch (e) { return; }
    queue.push(s);
    drain();
  }

  // ---- THE SELECTORS ------------------------------------------------------
  //
  // Every anchor the apply path depends on lives here and NOWHERE else, so a
  // site redesign is re-anchored in one block. Each is logged once per page by
  // selftest() with how many elements it matched, which is what makes a moved
  // anchor one line in the Output Log rather than a feature that silently
  // stops working.
  var SEL = {
    // a control's row, by the data-testid the site puts on it (the mutator key)
    byTestId:      '[data-testid="{k}"]',
    // a per-team toggle: the site suffixes the team column index
    switchN:       '[data-testid="{k}-switch-{t}"]',
    switchAny:     '[data-testid^="{k}-switch-"]',
    // the controls themselves, looked for inside the row
    slider:        'input[type="range"], [role="slider"]',
    numberBox:     'input[type="text"], input[type="number"], input:not([type])',
    toggle:        '[role="switch"], input[type="checkbox"]',
    dropdown:      '[role="combobox"], select',
    option:        '[role="option"], [role="listbox"] li, [role="menuitem"]',
    textInput:     'input, textarea',
    // the label the site shows above a control, used when a control has no testid
    label:         '[data-testid], label, h3, h4',
    // restrictions
    checkbox:      '[role="checkbox"], input[type="checkbox"]',
    // the site's own buttons
    save:          '[data-testid="save-button"], [data-testid="save"]',
    saveByText:    'button',
    saveText:      /^\s*(save|save changes|save experience|apply)\s*$/i,
    next:          '[data-testid="next-button"]',
    publish:       '[data-testid="publish-button"]',
    mapAdd:        '[data-testid="add-{m}"]',
    mapRemove:     '[data-testid="remove-{m}:{i}"]',
    mapAttach:     '[data-testid="attach-{m}:{i}"]',
    // the site's own feedback
    toast:         '[role="status"], [role="alert"], [class*="toast"], [class*="Toast"], [class*="snackbar"], [class*="Snackbar"]'
  };

  function fill(t, vals) {
    return t.replace(/\{(\w+)\}/g, function (_, k) { return vals[k] === undefined ? '' : String(vals[k]); });
  }

  function qsa(sel, root) {
    try { return Array.prototype.slice.call((root || document).querySelectorAll(sel)); }
    catch (e) { return []; }
  }

  function textOf(el) {
    if (!el) return '';
    var t = '';
    try { t = el.innerText || el.textContent || ''; } catch (e) {}
    return t.replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
  }

  // ---- reading a control --------------------------------------------------

  // Every element the site tagged with this key, in DOM order. A per-team
  // setting renders one control per team column, so the ORDER is the team
  // order: index 0 is team 1.
  function rowsFor(key) {
    if (!key) return [];
    if (key.indexOf('title:') === 0) return rowsByTitle(key.slice(6));
    var exact = qsa(fill(SEL.byTestId, { k: key }));
    if (exact.length) return exact;
    var sw = qsa(fill(SEL.switchAny, { k: key }));
    if (sw.length) return sw;
    // the site sometimes suffixes the team index straight onto the key
    var pre = qsa('[data-testid^="' + key + '-"]');
    return pre;
  }

  // A control the site renders WITHOUT a data-testid (the Teams page setup
  // block is all of these). Anchored on the label the user reads instead.
  function rowsByTitle(title) {
    var want = String(title || '').toUpperCase();
    var out = [];
    var all = qsa('input[type="range"], [role="slider"], [role="switch"], [role="combobox"], input[type="checkbox"], select');
    for (var i = 0; i < all.length; i++) {
      var el = all[i];
      var name = (el.getAttribute('aria-label') || el.getAttribute('name') || '').toUpperCase();
      if (name === want) { out.push(el); continue; }
      var box = null;
      try { box = el.closest('[class],div'); } catch (e) {}
      for (var d = 0; box && d < 4; d++) {
        if (textOf(box).toUpperCase().indexOf(want) === 0) { out.push(el); break; }
        try { box = box.parentElement; } catch (e) { break; }
      }
    }
    return out;
  }

  function controlIn(row, kind) {
    if (!row) return null;
    var sel = kind === 'toggle' ? SEL.toggle
            : kind === 'slider' ? SEL.slider
            : kind === 'dropdown' ? SEL.dropdown
            : kind === 'text' ? SEL.textInput : null;
    if (!sel) return null;
    if (row.matches && row.matches(sel)) return row;
    var inner = qsa(sel, row);
    return inner.length ? inner[0] : null;
  }

  function kindOf(row) {
    if (!row) return '';
    if (row.matches) {
      if (row.matches(SEL.toggle)) return 'toggle';
      if (row.matches(SEL.slider)) return 'slider';
      if (row.matches(SEL.dropdown)) return 'dropdown';
    }
    if (qsa(SEL.toggle, row).length) return 'toggle';
    if (qsa(SEL.slider, row).length) return 'slider';
    if (qsa(SEL.dropdown, row).length) return 'dropdown';
    if (qsa(SEL.textInput, row).length) return 'text';
    return '';
  }

  function numFrom(s) {
    if (s === null || s === undefined) return null;
    var m = String(s).replace(/,/g, '').match(/-?\d+(\.\d+)?/);
    return m ? parseFloat(m[0]) : null;
  }

  function readControl(row, kind) {
    kind = kind || kindOf(row);
    var el = controlIn(row, kind);
    if (kind === 'toggle') {
      if (!el) return null;
      var a = el.getAttribute('aria-checked');
      if (a !== null) return a === 'true';
      return !!el.checked;
    }
    if (kind === 'slider') {
      if (el && el.value !== undefined && el.value !== '') return numFrom(el.value);
      if (el) return numFrom(el.getAttribute('aria-valuenow'));
      return null;
    }
    if (kind === 'dropdown') {
      if (!el) return null;
      if (el.tagName === 'SELECT') return el.value;
      return textOf(el);
    }
    if (kind === 'text') {
      return el ? (el.value !== undefined ? el.value : textOf(el)) : null;
    }
    return null;
  }

  function rangeOf(row) {
    var el = controlIn(row, 'slider');
    if (!el) return {};
    var mn = numFrom(el.getAttribute('min') !== null ? el.getAttribute('min') : el.getAttribute('aria-valuemin'));
    var mx = numFrom(el.getAttribute('max') !== null ? el.getAttribute('max') : el.getAttribute('aria-valuemax'));
    var st = numFrom(el.getAttribute('step'));
    var o = {};
    if (mn !== null) o.min = mn;
    if (mx !== null) o.max = mx;
    if (st !== null) o.step = st;
    return o;
  }

  // The caption the site shows behind the "Open more information for: X"
  // button. Read from the button's own accessible name and any tooltip text
  // already in the DOM; never opened, because opening it is a click the user
  // did not ask for.
  function captionOf(row) {
    if (!row) return '';
    var host = row;
    try { host = row.closest('div') || row; } catch (e) {}
    for (var d = 0; host && d < 5; d++) {
      var b = qsa('[aria-describedby], [title]', host);
      for (var i = 0; i < b.length; i++) {
        var t = b[i].getAttribute('title') || '';
        if (t && t.length > 12) return t;
      }
      try { host = host.parentElement; } catch (e) { break; }
    }
    return '';
  }

  function titleOf(row, key) {
    if (!row) return key;
    var host = row;
    for (var d = 0; host && d < 5; d++) {
      var b = qsa('button[aria-label^="Open more information for:"]', host);
      if (b.length) return b[0].getAttribute('aria-label').replace(/^Open more information for:\s*/, '');
      try { host = host.parentElement; } catch (e) { break; }
    }
    var al = row.getAttribute && row.getAttribute('aria-label');
    return al || key;
  }

  // ---- 1. the scrape ------------------------------------------------------

  // Every data-testid on the page that looks like a setting, plus what the
  // control under it currently is. Merged over the shipped catalogue by the
  // tool, so a renamed or brand new setting shows up without a release.
  function scrape() {
    var seen = {};
    var out = [];
    var tagged = qsa('[data-testid]');
    for (var i = 0; i < tagged.length && out.length < 400; i++) {
      var el = tagged[i];
      var id = el.getAttribute('data-testid') || '';
      if (!id) continue;
      // -switch-<n> is a per-team column of the key in front of it
      var base = id.replace(/-switch-\d+$/, '');
      if (seen[base]) continue;
      var kind = kindOf(el);
      if (!kind) continue;
      seen[base] = 1;
      var rows = rowsFor(base);
      var vals = [];
      for (var t = 0; t < rows.length && t < 8; t++) vals.push(readControl(rows[t], kind));
      var rec = {
        testId: base, kind: kind, title: titleOf(rows[0] || el, base),
        caption: captionOf(rows[0] || el), teams: rows.length, values: vals,
        perTeam: /_PerTeam$/.test(base)
      };
      var r = rangeOf(rows[0] || el);
      for (var k in r) if (Object.prototype.hasOwnProperty.call(r, k)) rec[k] = r[k];
      if (kind === 'dropdown') rec.options = optionsOf(rows[0] || el);
      out.push(rec);
    }
    return out;
  }

  // A dropdown's choices WITHOUT opening it where the site renders them into
  // the DOM already; opening one is a click, and a scrape must not click.
  function optionsOf(row) {
    var el = controlIn(row, 'dropdown');
    if (!el) return [];
    if (el.tagName === 'SELECT') {
      return qsa('option', el).map(function (o) { return { value: o.value, label: textOf(o) }; });
    }
    var listId = el.getAttribute('aria-controls') || el.getAttribute('aria-owns');
    var list = listId ? document.getElementById(listId) : null;
    if (!list) return [];
    return qsa(SEL.option, list).map(function (o, i) {
      return { value: o.getAttribute('data-value') || i, label: textOf(o) };
    });
  }

  // The restriction pages: category groups with their counts, and each item's
  // per-team checkbox state.
  function scrapeRestrictions() {
    var cats = [];
    var heads = qsa('h1,h2,h3,h4,button,[role="button"],div');
    var seen = {};
    for (var i = 0; i < heads.length; i++) {
      var t = textOf(heads[i]);
      var m = t.match(/^([A-Z][A-Z0-9 .\-]{2,40})\s*(\d+)\s*\/\s*(\d+)$/);
      if (!m) continue;
      var name = m[1].replace(/\s+$/, '');
      if (seen[name]) continue;
      seen[name] = 1;
      var section = heads[i];
      try { section = heads[i].closest('section,li,div') || heads[i]; } catch (e) {}
      var boxes = qsa(SEL.checkbox, section);
      var items = [];
      for (var b = 0; b < boxes.length; b++) {
        var st = boxes[b].getAttribute('aria-checked');
        items.push({
          index: b,
          label: boxes[b].getAttribute('aria-label') || textOf(boxes[b].parentElement) || '',
          on: st !== null ? st === 'true' : !!boxes[b].checked
        });
      }
      cats.push({ name: name, on: parseInt(m[2], 10), count: parseInt(m[3], 10), items: items });
    }
    return cats;
  }

  // ---- 2. applying a change ----------------------------------------------
  //
  // React owns these inputs, so writing .value directly is ignored: the native
  // setter has to be called on the prototype and the events React listens for
  // fired by hand. Everything else here is a real click.

  function nativeSet(el, value) {
    try {
      var proto = Object.getPrototypeOf(el);
      var d = Object.getOwnPropertyDescriptor(proto, 'value');
      if (d && d.set) { d.set.call(el, String(value)); return true; }
    } catch (e) {}
    try { el.value = String(value); return true; } catch (e) {}
    return false;
  }

  function fire(el, names) {
    for (var i = 0; i < names.length; i++) {
      try { el.dispatchEvent(new Event(names[i], { bubbles: true })); } catch (e) {}
    }
  }

  function setToggle(row, want) {
    var el = controlIn(row, 'toggle');
    if (!el) return 'no toggle under that key';
    var now = readControl(row, 'toggle');
    if (now === !!want) return '';
    try { el.click(); } catch (e) { return 'the toggle refused a click: ' + e; }
    return '';
  }

  function setSlider(row, want) {
    var el = controlIn(row, 'slider');
    if (!el) return 'no slider under that key';
    // What a user actually does: type it in the number box beside the slider.
    var boxes = [];
    var host = row;
    try { host = row.closest('div') || row; } catch (e) {}
    for (var d = 0; host && d < 4 && !boxes.length; d++) {
      boxes = qsa(SEL.numberBox, host).filter(function (b) { return b !== el; });
      try { host = host.parentElement; } catch (e) { break; }
    }
    if (boxes.length) {
      var box = boxes[0];
      try { box.focus(); } catch (e) {}
      nativeSet(box, want);
      fire(box, ['input', 'change']);
      try { box.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); } catch (e) {}
      try { box.blur(); } catch (e) {}
    }
    // and the range input itself, which is what the site actually binds to
    if (el.tagName === 'INPUT') {
      nativeSet(el, want);
      fire(el, ['input', 'change']);
      return '';
    }
    // a div with role=slider: the keyboard is the only honest way in
    var now = numFrom(el.getAttribute('aria-valuenow'));
    var step = numFrom(el.getAttribute('step')) || 1;
    if (now === null) return 'the slider does not report a value';
    var steps = Math.round((want - now) / step);
    if (Math.abs(steps) > 500) return 'that value is more than 500 steps away';
    try { el.focus(); } catch (e) {}
    var key = steps > 0 ? 'ArrowRight' : 'ArrowLeft';
    for (var i = 0; i < Math.abs(steps); i++) {
      try { el.dispatchEvent(new KeyboardEvent('keydown', { key: key, bubbles: true })); } catch (e) {}
    }
    return '';
  }

  function setDropdown(row, wantLabel, done) {
    var el = controlIn(row, 'dropdown');
    if (!el) { done('no dropdown under that key'); return; }
    if (el.tagName === 'SELECT') {
      var opts = qsa('option', el);
      for (var i = 0; i < opts.length; i++) {
        if (textOf(opts[i]).toUpperCase() === String(wantLabel).toUpperCase()
            || String(opts[i].value) === String(wantLabel)) {
          nativeSet(el, opts[i].value);
          fire(el, ['input', 'change']);
          done('');
          return;
        }
      }
      done('no option labelled ' + wantLabel);
      return;
    }
    try { el.click(); } catch (e) { done('the dropdown refused a click: ' + e); return; }
    // The list is portalled to the end of the body, so it is searched from the
    // document rather than from inside the row.
    setTimeout(function () {
      var opts = qsa(SEL.option);
      var want = String(wantLabel).toUpperCase();
      for (var i = 0; i < opts.length; i++) {
        if (textOf(opts[i]).toUpperCase() === want) {
          try { opts[i].click(); } catch (e) { done('the option refused a click: ' + e); return; }
          done('');
          return;
        }
      }
      // nothing matched: close it again rather than leaving the page open
      try { document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); } catch (e) {}
      done('no option labelled ' + wantLabel + ' (' + opts.length + ' options on the page)');
    }, 220);
  }

  function setText(row, want) {
    var el = controlIn(row, 'text');
    if (!el) return 'no text field under that key';
    try { el.focus(); } catch (e) {}
    nativeSet(el, want);
    fire(el, ['input', 'change']);
    try { el.blur(); } catch (e) {}
    return '';
  }

  // One change: {testId, team, kind, value, label}. team -1 means "the only
  // control", otherwise it is the team COLUMN index (0 = team 1).
  function applyOne(ch, done) {
    var rows = rowsFor(ch.testId);
    if (!rows.length) { done({ testId: ch.testId, team: ch.team, ok: false, reason: 'the page has no control with that key' }); return; }
    var idx = (ch.team === undefined || ch.team === null || ch.team < 0) ? 0 : ch.team;
    if (idx >= rows.length) { done({ testId: ch.testId, team: ch.team, ok: false, reason: 'the page shows ' + rows.length + ' team column(s), not ' + (idx + 1) }); return; }
    var row = rows[idx];
    var kind = ch.kind || kindOf(row);

    function verify(why) {
      setTimeout(function () {
        var now = readControl(row, kind);
        var ok = false;
        if (why) ok = false;
        else if (kind === 'toggle') ok = (!!now === !!ch.value);
        else if (kind === 'slider') ok = (now !== null && Math.abs(now - Number(ch.value)) < 0.001);
        else if (kind === 'dropdown') ok = String(now).toUpperCase().indexOf(String(ch.label || ch.value).toUpperCase()) >= 0;
        else ok = (String(now) === String(ch.value));
        done({
          testId: ch.testId, team: ch.team, kind: kind, ok: ok,
          value: now,
          reason: why || (ok ? '' : 'the page did not take the value (it still reads ' + now + ')')
        });
      }, 260);
    }

    if (kind === 'toggle') { verify(setToggle(row, ch.value)); return; }
    if (kind === 'slider') { verify(setSlider(row, Number(ch.value))); return; }
    if (kind === 'text')   { verify(setText(row, ch.value)); return; }
    if (kind === 'dropdown') { setDropdown(row, ch.label !== undefined ? ch.label : ch.value, verify); return; }
    done({ testId: ch.testId, team: ch.team, ok: false, reason: 'kind ' + kind + ' is not something this page half can set' });
  }

  // A whole page's worth of changes, one after another, each reported back as
  // it lands. The tool has already put the page on the right section.
  function applySet(json) {
    var list;
    try { list = JSON.parse(json); } catch (e) { send({ kind: 'apply-error', reason: 'bad json: ' + e }); return false; }
    if (!list || !list.length) { send({ kind: 'apply-done', applied: 0 }); return true; }
    var i = 0, ok = 0;
    function step() {
      if (i >= list.length) { send({ kind: 'apply-done', applied: ok, of: list.length }); return; }
      var ch = list[i++];
      noteSelfApply();   // the watcher must not report our own change back
      applyOne(ch, function (res) {
        res.kind = 'applied';
        if (res.ok) ok++;
        send(res);
        setTimeout(step, 90);
      });
    }
    step();
    return true;
  }

  // ---- restrictions -------------------------------------------------------

  function findCategory(name) {
    var want = String(name || '').toUpperCase();
    var heads = qsa('h1,h2,h3,h4,button,[role="button"],div');
    for (var i = 0; i < heads.length; i++) {
      var t = textOf(heads[i]).toUpperCase();
      if (t.indexOf(want) !== 0) continue;
      if (!/\d+\s*\/\s*\d+$/.test(t)) continue;
      try { return heads[i].closest('section,li,div') || heads[i]; } catch (e) { return heads[i]; }
    }
    return null;
  }

  // One item's checkbox in a category, by its position in that category. The
  // site repeats a label when two variants share a display name, so the index
  // is the address and the label is only the check.
  function setRestriction(cat, index, team, on) {
    var section = findCategory(cat);
    if (!section) return 'no category called ' + cat + ' on this page';
    var boxes = qsa(SEL.checkbox, section);
    // per-team pages render one column of boxes per team, interleaved by row
    var teams = Math.max(1, Number(team) >= 0 ? 0 : 1);
    var at = index;
    if (Number(team) >= 0) at = index * Math.max(1, teamColumns(section)) + Number(team);
    if (at >= boxes.length) return 'that category shows ' + boxes.length + ' item(s)';
    var el = boxes[at];
    var now = el.getAttribute('aria-checked') !== null ? el.getAttribute('aria-checked') === 'true' : !!el.checked;
    if (now === !!on) return '';
    try { el.click(); } catch (e) { return 'the item refused a click: ' + e; }
    return '';
  }

  // How many checkbox columns one item row carries: the team count the page is
  // showing. Read off the header row rather than assumed to be two.
  function teamColumns(section) {
    var rows = qsa('[role="row"], li', section);
    if (rows.length) {
      var n = qsa(SEL.checkbox, rows[0]).length;
      if (n > 0) return n;
    }
    return 1;
  }

  function setCategory(cat, team, on) {
    var section = findCategory(cat);
    if (!section) return 'no category called ' + cat + ' on this page';
    var boxes = qsa(SEL.checkbox, section);
    var cols = teamColumns(section);
    var touched = 0;
    for (var i = 0; i < boxes.length; i++) {
      if (Number(team) >= 0 && cols > 1 && (i % cols) !== Number(team)) continue;
      var el = boxes[i];
      var now = el.getAttribute('aria-checked') !== null ? el.getAttribute('aria-checked') === 'true' : !!el.checked;
      if (now === !!on) continue;
      try { el.click(); touched++; } catch (e) {}
    }
    send({ kind: 'restriction-bulk', category: cat, team: team, on: !!on, touched: touched });
    return '';
  }

  // ---- map rotation -------------------------------------------------------

  function mapAction(what, mapKey, idx) {
    var sel = what === 'add' ? fill(SEL.mapAdd, { m: mapKey })
            : what === 'remove' ? fill(SEL.mapRemove, { m: mapKey, i: idx })
            : fill(SEL.mapAttach, { m: mapKey, i: idx });
    var els = qsa(sel);
    if (!els.length) {
      send({ kind: 'map', what: what, map: mapKey, index: idx, ok: false, reason: 'no button matching ' + sel });
      return false;
    }
    try { els[0].click(); }
    catch (e) {
      send({ kind: 'map', what: what, map: mapKey, index: idx, ok: false, reason: 'the button refused a click: ' + e });
      return false;
    }
    setTimeout(function () {
      send({ kind: 'map', what: what, map: mapKey, index: idx, ok: true, slots: mapSlots() });
    }, 400);
    return true;
  }

  // The rotation as the page currently shows it, read off the remove buttons:
  // remove-<Map>:<size>:<slot>.
  function mapSlots() {
    var out = [];
    var els = qsa('[data-testid^="remove-"]');
    for (var i = 0; i < els.length; i++) {
      var m = (els[i].getAttribute('data-testid') || '').match(/^remove-(.+):([sml]):(\d+)$/);
      if (m) out.push({ map: m[1], size: m[2], slot: parseInt(m[3], 10) });
    }
    out.sort(function (a, b) { return a.slot - b.slot; });
    return out;
  }

  // ---- 3. SAVE ON PORTAL --------------------------------------------------
  //
  // The site's own save button, the site's own request. The verdict comes from
  // two places: the gRPC trailer of the updatePlayElement the SITE sent, and
  // the toast the site put on screen.

  var lastVerdict = null;

  function findSave() {
    var direct = qsa(SEL.save);
    if (direct.length) return direct[0];
    var btns = qsa(SEL.saveByText);
    for (var i = 0; i < btns.length; i++) {
      if (SEL.saveText.test(textOf(btns[i]))) return btns[i];
    }
    return null;
  }

  function toastText() {
    var t = qsa(SEL.toast);
    for (var i = t.length - 1; i >= 0; i--) {
      var s = textOf(t[i]);
      if (s && s.length > 1 && s.length < 400) return s;
    }
    return '';
  }

  function save() {
    var btn = findSave();
    if (!btn) {
      send({ kind: 'save', ok: false, reason: 'no save button on this page (the site may save on navigation instead)' });
      return false;
    }
    lastVerdict = null;
    try { btn.click(); }
    catch (e) { send({ kind: 'save', ok: false, reason: 'the save button refused a click: ' + e }); return false; }
    var tries = 0;
    (function watch() {
      tries++;
      if (lastVerdict) {
        lastVerdict.kind = 'save';
        lastVerdict.toast = toastText();
        send(lastVerdict);
        lastVerdict = null;
        return;
      }
      if (tries > 30) {
        send({ kind: 'save', ok: false, toast: toastText(), reason: 'the site did not send an update in 15 seconds' });
        return;
      }
      setTimeout(watch, 500);
    })();
    return true;
  }

  // ---- watching the site's own calls --------------------------------------
  //
  // A second wrapper over fetch, on top of capture.js's. It reads nothing but
  // the RESPONSE: the status, the gRPC trailer, and (for getPlayElement) the
  // mutator values the tool shows.

  function methodOf(url) {
    var at = url.indexOf(WEBPLAY);
    if (at < 0) return '';
    var tail = url.slice(at + WEBPLAY.length);
    var cut = tail.search(/[?#\/]/);
    return cut < 0 ? tail : tail.slice(0, cut);
  }

  // ---- a small protobuf reader -------------------------------------------
  //
  // The getPlayElement response carries the experience's MUTATORS, which is
  // where the panel's live values come from. Decoded HERE so the body itself
  // never has to cross the bridge: what crosses is the decoded
  // {key: {tags, default, teams}} map and nothing else.
  //
  // Shape, verified against the site's own responses:
  //   msg.f2                 the play element
  //   msg.f2.f7 (repeated)   one mutator each
  //     f1 key string, f2 applicability tags, f3 value container, f4 id
  //   container -> one typed submessage:
  //     f1 = the scalar default
  //     f3 (repeated) = per-team { f1 team id, f2 value }

  function Rd(buf, from, to) { this.b = buf; this.i = from; this.e = to; }

  // A varint has to be read as a real 64 bit value, not accumulated in a
  // double: the site stores a negative enum (FactionID_PerTeam's PAX is
  // -1865993703) as a 64 bit two's complement, and above 2^53 a double loses
  // the low bits and the enum comes out as a nonsense positive. The two halves
  // are kept in 32 bit integers and only combined at the end, with the sign
  // applied the way protobuf means it.
  Rd.prototype.varint = function () {
    var lo = 0, hi = 0, s = 0, got = false;
    while (this.i < this.e) {
      var x = this.b[this.i++];
      var c = x & 0x7F;
      if (s < 28) lo |= c << s;
      else if (s === 28) { lo |= (c & 0x0F) << 28; hi |= (c >>> 4); }
      else hi |= c << (s - 32);
      if (!(x & 0x80)) { got = true; break; }
      s += 7;
      if (s > 63) return null;
    }
    if (!got) return null;   // null is "ran off the end", never a value
    var ulo = lo >>> 0, uhi = hi >>> 0;
    if (uhi & 0x80000000) {
      // two's complement: -( ~v + 1 )
      var nlo = (~ulo) >>> 0, nhi = (~uhi) >>> 0;
      return -(nhi * 4294967296 + nlo + 1);
    }
    return uhi * 4294967296 + ulo;
  };

  // fn(fieldNumber, wireType, value, start, end)
  function walk(buf, from, to, fn) {
    var r = new Rd(buf, from, to);
    while (r.i < r.e) {
      var key = r.varint();
      if (key === null || key <= 0) return false;
      var f = Math.floor(key / 8), w = key & 7;
      if (w === 0) { var v = r.varint(); if (v === null) return false; fn(f, 0, v, 0, 0); }
      else if (w === 1) { if (r.i + 8 > r.e) return false; fn(f, 1, dbl(buf, r.i), 0, 0); r.i += 8; }
      else if (w === 5) { if (r.i + 4 > r.e) return false; fn(f, 5, flt(buf, r.i), 0, 0); r.i += 4; }
      else if (w === 2) {
        var n = r.varint();
        if (n === null || n < 0 || r.i + n > r.e) return false;
        fn(f, 2, null, r.i, r.i + n);
        r.i += n;
      } else return false;
    }
    return true;
  }

  var scratch = new DataView(new ArrayBuffer(8));
  function flt(b, at) {
    for (var i = 0; i < 4; i++) scratch.setUint8(i, b[at + i]);
    return scratch.getFloat32(0, true);
  }
  function dbl(b, at) {
    for (var i = 0; i < 8; i++) scratch.setUint8(i, b[at + i]);
    return scratch.getFloat64(0, true);
  }
  function str(b, from, to) {
    var s = '';
    for (var i = from; i < to; i++) {
      var c = b[i];
      if (c < 0x20 && c !== 9 && c !== 10 && c !== 13) return '';
      if (c > 0x7E) return '';
      s += String.fromCharCode(c);
    }
    return s;
  }

  // default + per-team values, found wherever the typed submessage put them
  function readValues(buf, from, to) {
    var def = null, teams = {};
    (function rec(f0, t0, depth) {
      if (depth > 6) return;
      var fields = [];
      walk(buf, f0, t0, function (f, w, v, s, e) { fields.push([f, w, v, s, e]); });
      var f1 = null, f2 = null;
      for (var i = 0; i < fields.length; i++) {
        if (fields[i][0] === 1 && f1 === null) f1 = fields[i];
        if (fields[i][0] === 2 && f2 === null) f2 = fields[i];
      }
      // a per-team entry: {1: team id, 2: value}
      if (f1 && f2 && f1[1] === 0 && f1[2] >= 0 && f1[2] < 100 && (f2[1] === 0 || f2[1] === 1 || f2[1] === 5)) {
        teams[f1[2]] = f2[2];
        return;
      }
      for (var j = 0; j < fields.length; j++) {
        var fd = fields[j];
        if (fd[0] === 1 && (fd[1] === 1 || fd[1] === 5) && def === null) def = fd[2];
        else if (fd[0] === 1 && fd[1] === 0 && def === null && fields.length === 1 && depth === 1) def = fd[2];
        if (fd[1] === 2) rec(fd[3], fd[4], depth + 1);
      }
      // a false-valued per-team entry is {1: team id} on its own
      if (fields.length === 1 && fields[0][0] === 1 && fields[0][1] === 0
          && fields[0][2] >= 0 && fields[0][2] < 100 && depth >= 1 && teams[fields[0][2]] === undefined) {
        teams[fields[0][2]] = 0;
      }
    })(from, to, 0);
    return { def: def, teams: teams };
  }

  function readMutators(bytes) {
    var out = {};
    var off = 0;
    while (off + 5 <= bytes.length) {
      var flags = bytes[off];
      var len = (bytes[off + 1] * 16777216) + (bytes[off + 2] * 65536) + (bytes[off + 3] * 256) + bytes[off + 4];
      off += 5;
      if (off + len > bytes.length) break;
      if (!(flags & 0x80) && len > 0) {
        (function (from, to) {
          walk(bytes, from, to, function (f, w, v, s, e) {
            if (f !== 2 || w !== 2) return;              // the play element
            walk(bytes, s, e, function (g, gw, gv, gs, ge) {
              if (g !== 7 || gw !== 2) return;           // one mutator
              var key = '', tags = '', cs = -1, ce = -1;
              walk(bytes, gs, ge, function (h, hw, hv, hs, he) {
                if (hw !== 2) return;
                if (h === 1) key = str(bytes, hs, he);
                else if (h === 2) tags = str(bytes, hs, he);
                else if (h === 3) { cs = hs; ce = he; }
              });
              if (!key || cs < 0) return;
              var vals = readValues(bytes, cs, ce);
              var prev = out[key];
              if (prev && Object.keys(prev.teams).length && !Object.keys(vals.teams).length) return;
              out[key] = { tags: tags, def: vals.def, teams: vals.teams };
            });
          });
        })(off, off + len);
      }
      off += len;
    }
    return out;
  }

  // The gRPC trailer frame: flags & 0x80, payload "grpc-status:0\r\ngrpc-message:.."
  function trailerOf(bytes) {
    var off = 0, out = { status: null, message: '' };
    while (off + 5 <= bytes.length) {
      var flags = bytes[off];
      var len = (bytes[off + 1] * 16777216) + (bytes[off + 2] * 65536) + (bytes[off + 3] * 256) + bytes[off + 4];
      off += 5;
      if (off + len > bytes.length) break;
      if (flags & 0x80) {
        var t = str(bytes, off, off + len);
        var m = t.match(/grpc-status:\s*(-?\d+)/i);
        if (m) out.status = parseInt(m[1], 10);
        var g = t.match(/grpc-message:\s*([^\r\n]*)/i);
        if (g) out.message = decodeURIComponent(g[1] || '').slice(0, 300);
      }
      off += len;
    }
    return out;
  }

  function onBody(method, status, buf) {
    var bytes;
    try { bytes = new Uint8Array(buf); } catch (e) { return; }
    if (method === 'getPlayElement' && status === 200) {
      var muts;
      try { muts = readMutators(bytes); } catch (e) { console.log('BF6SETTINGS mutator decode failed: ' + e); return; }
      var n = 0;
      for (var k in muts) if (Object.prototype.hasOwnProperty.call(muts, k)) n++;
      console.log('BF6SETTINGS decoded ' + n + ' mutator(s) from getPlayElement');
      send({ kind: 'mutators', count: n, mutators: muts });
      return;
    }
    if (method === 'updatePlayElement') {
      var tr = {};
      try { tr = trailerOf(bytes); } catch (e) {}
      lastVerdict = {
        ok: status === 200 && (tr.status === 0 || tr.status === null),
        status: status, grpcStatus: tr.status, grpcMessage: tr.message
      };
      console.log('BF6SETTINGS updatePlayElement ' + status + ' grpc-status ' + tr.status);
    }
  }

  var realFetch = window.fetch;
  if (realFetch && !realFetch.__bf6settings) {
    var wrapped = function (input, init) {
      var url = '';
      try { url = (typeof input === 'string') ? input : ((input && input.url) || ''); } catch (e) {}
      var p = realFetch.apply(this, arguments);
      if (url.indexOf(WEBPLAY) >= 0) {
        try {
          p.then(function (res) {
            try { res.clone().arrayBuffer().then(function (buf) { onBody(methodOf(url), res.status, buf); }, noop); }
            catch (e) {}
          }, noop);
        } catch (e) {}
      }
      return p;
    };
    wrapped.__bf6settings = true;
    window.fetch = wrapped;
  }

  // ---- the page report ----------------------------------------------------

  function section(p) {
    if (p.indexOf('/bf6/experience/settings/mode') === 0) return 'mode';
    if (p.indexOf('/bf6/experience/choose-maps') === 0) return 'map-rotation';
    if (p.indexOf('/bf6/experience/teams') === 0) return 'teams';
    if (p.indexOf('/bf6/experience/modifiers/gameplay') === 0) return 'mod-gameplay';
    if (p.indexOf('/bf6/experience/modifiers/soldier') === 0) return 'mod-soldier';
    if (p.indexOf('/bf6/experience/modifiers/vehicle') === 0) return 'mod-vehicle';
    if (p.indexOf('/bf6/experience/modifiers/ui') === 0) return 'mod-ui';
    if (p.indexOf('/bf6/experience/modifiers/bots') === 0) return 'mod-ai';
    if (p.indexOf('/bf6/experience/restrictions/classes') === 0) return 'restrict-chars';
    if (p.indexOf('/bf6/experience/restrictions/weapons') === 0) return 'restrict-weapons';
    if (p.indexOf('/bf6/experience/restrictions/vehicles') === 0) return 'restrict-vehicles';
    if (p.indexOf('/bf6/experience/restrictions/gadgets') === 0) return 'restrict-gadgets';
    if (p.indexOf('/bf6/experience/publish/step-one') === 0) return 'publish-1';
    if (p.indexOf('/bf6/experience/publish/step-two') === 0) return 'publish-2';
    if (p.indexOf('/bf6/experience/publish/step-three') === 0) return 'publish-3';
    return '';
  }

  // The selectors, and how many elements each matched. Logged once per page so
  // a site redesign is re-anchored from the Output Log rather than from a guess.
  function selftest() {
    var checks = {
      'data-testid': qsa('[data-testid]').length,
      'role=switch': qsa('[role="switch"]').length,
      'input[type=range]': qsa('input[type="range"]').length,
      'role=slider': qsa('[role="slider"]').length,
      'role=combobox': qsa('[role="combobox"]').length,
      'role=checkbox': qsa('[role="checkbox"], input[type="checkbox"]').length,
      'save': findSave() ? 1 : 0,
      'next-button': qsa(SEL.next).length,
      'publish-button': qsa(SEL.publish).length,
      'remove-<map>': qsa('[data-testid^="remove-"]').length,
      'toast': qsa(SEL.toast).length
    };
    return checks;
  }

  var reported = '';
  function report(force) {
    var sec = section(location.pathname);
    if (!force && sec === reported) return;
    reported = sec;
    if (!sec) { send({ kind: 'page', section: '' }); return; }
    var msg = { kind: 'page', section: sec, selectors: selftest() };
    if (sec.indexOf('restrict') === 0) msg.categories = scrapeRestrictions();
    else if (sec === 'map-rotation') msg.slots = mapSlots();
    else msg.settings = scrape();
    send(msg);
  }

  var lastSeen = '';
  setInterval(function () {
    if (location.href !== lastSeen) { lastSeen = location.href; reported = ''; setTimeout(function () { report(false); }, 700); }
  }, 600);
  setTimeout(function () { report(true); }, 900);
  setTimeout(function () { report(true); }, 2600);

  // ---- WATCH THE SITE ------------------------------------------------------
  //
  // The panel can follow what a person does on the site, live, so the tool's
  // toggles move as the site's move. It OBSERVES AND NEVER WRITES: no click, no
  // value set, no request. Off unless the tool turns it on.
  //
  // Two halves, because a React site changes a control in two different ways:
  //   input / change events, for a control a person is operating right now
  //   a MutationObserver, for a value the site itself rewrites (a slider whose
  //   number box is redrawn, a toggle flipped by another setting)
  //
  // Cost: one observer on the settings section with attributes and subtree,
  // filtered to the four attributes a control's value actually lives in, plus
  // two bubbling listeners on the document. Everything is debounced per control
  // for 400 ms and reported as ONE burst, so dragging a slider is one message
  // and not sixty.
  //
  // What it cannot see: anything not rendered as a control on the page the
  // panel is standing on. Settings on the other four pages are invisible until
  // the panel goes there, and so is anything the site keeps only in its own
  // state. It also cannot see a change the site makes with no DOM change at all.
  var watching = false;
  var watchObs = null;
  var watchDirty = {};        // testId -> true
  var watchTimer = null;
  var watchSelfUntil = 0;     // ignore our own applies for a moment after one
  var watchSeen = 0;

  function noteSelfApply() { watchSelfUntil = Date.now() + 1500; }

  function watchFlush() {
    watchTimer = null;
    if (Date.now() < watchSelfUntil) { watchDirty = {}; return; }
    var ids = Object.keys(watchDirty);
    watchDirty = {};
    if (!ids.length) return;
    var out = [];
    for (var i = 0; i < ids.length && i < 40; i++) {
      var base = ids[i];
      var rows = rowsFor(base);
      if (!rows.length) continue;
      var kind = kindOf(rows[0]);
      if (!kind) continue;
      var vals = [];
      for (var t = 0; t < rows.length && t < 8; t++) vals.push(readControl(rows[t], kind));
      out.push({ testId: base, kind: kind, values: vals });
    }
    if (!out.length) return;
    watchSeen += out.length;
    console.log('BF6SETTINGS watch: ' + out.length + ' control(s) changed on the site');
    send({ kind: 'watch', section: section(location.pathname), changes: out, seen: watchSeen });
  }

  function watchMark(el) {
    var row = el, i = 0, id = '';
    while (row && i++ < 10) {
      try { id = row.getAttribute && row.getAttribute('data-testid'); } catch (e) { id = ''; }
      if (id) break;
      row = row.parentElement;
    }
    if (!id) return;
    watchDirty[String(id).replace(/-switch-\d+$/, '')] = true;
    if (!watchTimer) watchTimer = setTimeout(watchFlush, 400);
  }

  function onWatchEvent(e) { try { if (e && e.target) watchMark(e.target); } catch (x) {} }

  function startWatch() {
    if (watching) return true;
    watching = true;
    try {
      document.addEventListener('change', onWatchEvent, true);
      document.addEventListener('input', onWatchEvent, true);
      watchObs = new MutationObserver(function (recs) {
        for (var i = 0; i < recs.length && i < 200; i++) {
          var t = recs[i].target;
          if (t && t.nodeType === 1) watchMark(t);
        }
      });
      watchObs.observe(document.body, {
        subtree: true, attributes: true, childList: false, characterData: false,
        attributeFilter: ['aria-checked', 'aria-valuenow', 'value', 'data-state', 'checked']
      });
    } catch (e) {
      console.log('BF6SETTINGS watch could not start: ' + e);
      watching = false;
      return false;
    }
    console.log('BF6SETTINGS watch: on');
    send({ kind: 'watch', section: section(location.pathname), started: true, changes: [] });
    return true;
  }

  function stopWatch() {
    if (!watching) return true;
    watching = false;
    try { document.removeEventListener('change', onWatchEvent, true); } catch (e) {}
    try { document.removeEventListener('input', onWatchEvent, true); } catch (e) {}
    try { if (watchObs) watchObs.disconnect(); } catch (e) {}
    watchObs = null;
    if (watchTimer) { clearTimeout(watchTimer); watchTimer = null; }
    watchDirty = {};
    console.log('BF6SETTINGS watch: off');
    send({ kind: 'watch', stopped: true, changes: [] });
    return true;
  }

  window.BF6PortalSettings = {
    startWatch: startWatch,
    stopWatch: stopWatch,
    noteSelfApply: noteSelfApply,
    v: V,
    scrape: function () { report(true); return true; },
    applySet: applySet,
    save: save,
    setRestriction: function (cat, i, team, on) {
      var why = setRestriction(cat, i, team, on);
      send({ kind: 'restriction', category: cat, index: i, team: team, on: !!on, ok: !why, reason: why });
      return !why;
    },
    setCategory: setCategory,
    mapAction: mapAction,
    selftest: function () { send({ kind: 'selftest', selectors: selftest() }); return true; },
    rearm: function () { lastSeen = ''; reported = ''; report(true); }
  };
  console.log('BF6SETTINGS ready v' + V);
})();
