/* Personal block-family colors. Never stored in a workspace or Portal export. */
(function (root) {
  'use strict';
  var hues = {}, host = null, dialog = null;
  var families = {
    'mod-block-style': ['Mod', 'The experience container.'],
    'rule-block-style': ['Rules', 'An event and the actions to run when it happens.'],
    'condition-block-style': ['Conditions', 'Checks that decide whether a rule can run.'],
    'action-block-style': ['Actions', 'Do something, in order from top to bottom.'],
    'value-block-style': ['Values', 'Produce something to plug into a socket.'],
    'variable-block-style': ['Variables', 'References to your named, stored values.'],
    'subroutine-block-style': ['Subroutines', 'Named actions you can call from other rules.'],
    'control-block-style': ['Control', 'Choose which actions run: If, While and loops.'],
    'comment-block-style': ['Comments', 'Notes for people reading the blocks.']
  };
  function group(key) { return key === 'control-block-alt-style' ? 'control-block-style' : key; }
  function valid(value) {
    var out = {};
    try {
      if (typeof value === 'string') value = JSON.parse(value);
      if (!value || typeof value !== 'object' || Array.isArray(value)) return out;
      Object.keys(families).forEach(function (key) {
        var v = value[key];
        if (typeof v === 'number' && Number.isFinite(v) && v >= 0 && v < 360) out[key] = Math.round(v) % 360;
      });
    } catch (_) {}
    return out;
  }
  function hsl(hex) {
    var m = /^#([0-9a-f]{6})$/i.exec(hex || '');
    if (!m) return [0, .65, .25];
    var n = parseInt(m[1], 16), r = (n >> 16) / 255, g = ((n >> 8) & 255) / 255, b = (n & 255) / 255;
    var max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min, l = (max + min) / 2, h = 0;
    if (d) h = (max === r ? ((g - b) / d + (g < b ? 6 : 0)) : max === g ? (b - r) / d + 2 : (r - g) / d + 4) * 60;
    return [h, d ? d / (1 - Math.abs(2 * l - 1)) : 0, l];
  }
  function rgb(h, s, l) {
    var c = (1 - Math.abs(2 * l - 1)) * s, x = c * (1 - Math.abs(h / 60 % 2 - 1)), m = l - c / 2;
    var v = h < 60 ? [c,x,0] : h < 120 ? [x,c,0] : h < 180 ? [0,c,x] : h < 240 ? [0,x,c] : h < 300 ? [x,0,c] : [c,0,x];
    return v.map(function (a) { return Math.round((a + m) * 255); });
  }
  function color(h, original, primary) {
    var v = hsl(original), s = primary ? Math.max(.6, v[1]) : v[1], l = primary ? Math.max(.22, v[2]) : v[2];
    var pixels;
    do {
      pixels = rgb(h, s, l);
      var linear = pixels.map(function (a) { a /= 255; return a <= .04045 ? a / 12.92 : Math.pow((a + .055) / 1.055, 2.4); });
      var contrast = 1.05 / (.2126 * linear[0] + .7152 * linear[1] + .0722 * linear[2] + .05);
      if (!primary || contrast >= 4.5) break;
      l *= .94;
    } while (l > .01);
    return '#' + pixels.map(function (a) { return a.toString(16).padStart(2, '0'); }).join('');
  }
  function apply(spec, palette) {
    var result = Object.assign({}, spec, { blockStyles: Object.assign({}, spec.blockStyles), categoryStyles: Object.assign({}, spec.categoryStyles) });
    palette = palette || hues;
    Object.keys(result.blockStyles).forEach(function (key) {
      var h = palette[group(key)];
      if (h === undefined) return;
      var style = Object.assign({}, result.blockStyles[key]);
      ['colourPrimary', 'colourSecondary', 'colourTertiary'].forEach(function (part) {
        style[part] = color(h, style[part], part === 'colourPrimary');
      });
      result.blockStyles[key] = style;
    });
    var categories = {'rules-category':'rule-block-style', 'values-category':'value-block-style',
      'actions-category':'action-block-style', 'controls-category':'control-block-style', 'subroutines-category':'subroutine-block-style'};
    Object.keys(categories).forEach(function (key) {
      var h = palette[categories[key]], original = result.categoryStyles[key];
      if (h !== undefined && original) result.categoryStyles[key] = Object.assign({}, original, {colour:color(h, original.colour, false)});
    });
    // Variables share Values' border on Portal, but need their own palette key.
    var originalVariable = (spec.categoryStyles && spec.categoryStyles['values-category']) || {colour:'#1d8c58'};
    result.categoryStyles['bf6-variables-category'] = Object.assign({}, originalVariable, {
      colour: palette['variable-block-style'] === undefined ? originalVariable.colour : color(palette['variable-block-style'], originalVariable.colour, false)
    });
    return result;
  }
  function family(block) {
    var key = block && block.getStyleName && group(block.getStyleName());
    return Object.prototype.hasOwnProperty.call(families, key) ? key : null;
  }
  function rows(spec) {
    return Object.keys(families).filter(function (key) { return spec.blockStyles && spec.blockStyles[key]; }).map(function (key) {
      return { key: key, name: families[key][0], meaning: families[key][1],
        color: spec.blockStyles[key].colourPrimary, custom: hues[key] !== undefined };
    });
  }
  function load(prefs) {
    if (!prefs) return;
    // A saved empty palette explicitly resets the legacy red preset too.
    if (prefs.categoryColors !== undefined) hues = valid(prefs.categoryColors);
    else if (prefs.redVariables !== undefined) hues = String(prefs.redVariables) === 'true' ? { 'variable-block-style': 0 } : {};
  }
  function node(tag, text) { var e = document.createElement(tag); if (text) e.textContent = text; return e; }
  function close() {
    if (!dialog) return;
    var focus = dialog.returnFocus;
    dialog.remove(); dialog = null;
    if (!focus || !focus.isConnected || !focus.getClientRects().length) focus = document.getElementById('btnMore');
    if (focus) focus.focus();
  }
  function open(key) {
    if (!host) return;
    close();
    var base = host.base(), options = rows(base);
    if (!options.length) return;
    var draft = Object.assign({}, hues), chosen = options.some(function (r) { return r.key === key; }) ? key : options[0].key;
    var shade = 0, changed = false;
    dialog = node('div'); dialog.id = 'category-color-dialog'; dialog.className = 'bf6-colors-backdrop';
    dialog.returnFocus = document.activeElement;
    var panel = node('section'); panel.className = 'bf6-colors-panel'; panel.setAttribute('role', 'dialog');
    panel.setAttribute('aria-modal', 'true'); panel.setAttribute('aria-labelledby', 'category-color-title');
    var title = node('h2', 'Block category colors'); title.id = 'category-color-title'; panel.appendChild(title);
    var label = node('label', 'Category '), select = node('select'); select.id = 'category-color-group';
    options.forEach(function (r) { var o = node('option', r.name); o.value = r.key; select.appendChild(o); });
    label.appendChild(select); panel.appendChild(label);
    var meaning = node('p'); panel.appendChild(meaning);
    var wheel = node('div'); wheel.className = 'bf6-hue-wheel'; wheel.tabIndex = 0;
    wheel.setAttribute('role', 'slider'); wheel.setAttribute('aria-label', 'Category hue');
    wheel.setAttribute('aria-valuemin', '0'); wheel.setAttribute('aria-valuemax', '359');
    var marker = node('span'); marker.className = 'bf6-hue-marker'; wheel.appendChild(marker); panel.appendChild(wheel);
    var rangeLabel = node('label', 'Hue '), range = node('input'); range.type = 'range'; range.min = 0; range.max = 359;
    range.id = 'category-color-hue'; rangeLabel.appendChild(range); panel.appendChild(rangeLabel);
    var preview = node('div'); preview.className = 'bf6-color-preview'; preview.id = 'category-color-preview'; panel.appendChild(preview);
    panel.appendChild(node('p', 'Changes every block in this color category, including new blocks and help examples. Shape, socket symbols and block behavior stay the same. Your colors apply only in this editor; Portal uses its own palette.'));
    var state = node('p'); state.setAttribute('role', 'status'); panel.appendChild(state);
    function paint() {
      var actual = apply(base, draft).blockStyles[chosen];
      meaning.textContent = families[chosen][1];
      preview.style.backgroundColor = actual.colourPrimary; preview.style.borderColor = actual.colourTertiary;
      preview.textContent = families[chosen][0] + '  ' + actual.colourPrimary;
      range.value = shade; wheel.setAttribute('aria-valuenow', String(shade)); wheel.setAttribute('aria-valuetext', shade + ' degrees');
      var rad = shade * Math.PI / 180;
      marker.style.left = (50 + Math.cos(rad) * 42) + '%'; marker.style.top = (50 + Math.sin(rad) * 42) + '%';
      state.textContent = changed ? 'Preview only. Apply updates your blocks and help colors.' : 'Choose a hue, or reset to Portal colors.';
    }
    function change(value) { shade = (Math.round(value) + 360) % 360; draft[chosen] = shade; changed = true; paint(); }
    select.value = chosen;
    select.onchange = function () { chosen = select.value; shade = draft[chosen] === undefined ? Math.round(hsl(base.blockStyles[chosen].colourPrimary)[0]) % 360 : draft[chosen]; paint(); };
    range.oninput = function () { change(Number(range.value)); };
    function point(ev) {
      var r = wheel.getBoundingClientRect(), dx = ev.clientX - r.left - r.width / 2, dy = ev.clientY - r.top - r.height / 2;
      if (dx * dx + dy * dy < 25) return;
      change(Math.atan2(dy, dx) * 180 / Math.PI);
    }
    wheel.onpointerdown = function (ev) { if (ev.button !== 0) return; ev.preventDefault(); wheel.focus(); wheel.setPointerCapture(ev.pointerId); point(ev); };
    wheel.onpointermove = function (ev) { if (wheel.hasPointerCapture(ev.pointerId)) point(ev); };
    wheel.onpointerup = function (ev) { if (wheel.hasPointerCapture(ev.pointerId)) wheel.releasePointerCapture(ev.pointerId); };
    wheel.onkeydown = function (ev) {
      if (['ArrowLeft','ArrowDown','ArrowRight','ArrowUp','Home','End'].indexOf(ev.key) < 0) return;
      ev.preventDefault(); ev.stopPropagation();
      change(ev.key === 'Home' ? 0 : ev.key === 'End' ? 359 : shade + (ev.key === 'ArrowLeft' || ev.key === 'ArrowDown' ? -1 : 1) * (ev.shiftKey ? 10 : 1));
    };
    var buttons = node('div'); buttons.className = 'bf6-color-buttons';
    function button(text, action) { var b = node('button', text); b.type = 'button'; b.onclick = action; buttons.appendChild(b); }
    button('Reset category', function () { delete draft[chosen]; changed = true; select.onchange(); });
    button('Reset all', function () { draft = {}; changed = true; select.onchange(); });
    button('Cancel', close);
    button('Apply', function () { if (changed) { hues = valid(draft); host.save(JSON.stringify(hues)); host.refresh(); } close(); });
    panel.appendChild(buttons); dialog.appendChild(panel); document.body.appendChild(dialog);
    dialog.onpointerdown = function (ev) { if (ev.target === dialog) close(); };
    dialog.onkeydown = function (ev) {
      ev.stopPropagation();
      if (ev.key === 'Escape') { ev.preventDefault(); close(); return; }
      if (ev.key === 'Tab') {
        var focusable = Array.from(panel.querySelectorAll('button,select,input,[tabindex="0"]'));
        var i = focusable.indexOf(document.activeElement);
        if (ev.shiftKey && i <= 0) { ev.preventDefault(); focusable[focusable.length - 1].focus(); }
        else if (!ev.shiftKey && i === focusable.length - 1) { ev.preventDefault(); focusable[0].focus(); }
      }
    };
    select.onchange(); select.focus();
  }
  function install(callbacks, Blockly) {
    host = callbacks;
    var registry = Blockly.ContextMenuRegistry.registry;
    if (registry.getItem('bf6_category_color')) return;
    registry.register({ id: 'bf6_category_color', scopeType: Blockly.ContextMenuRegistry.ScopeType.BLOCK, weight: 148,
      displayText: function (scope) { var key = family(scope.block); return 'Change ' + (key ? families[key][0] : 'category') + ' color...'; },
      preconditionFn: function (scope) { return family(scope.block) ? 'enabled' : 'hidden'; },
      callback: function (scope) { open(family(scope.block)); }
    });
  }
  root.BF6BlockColors = { apply: apply, load: load, rows: rows, family: family, open: open, install: install, validate: valid };
})(typeof window !== 'undefined' ? window : globalThis);
