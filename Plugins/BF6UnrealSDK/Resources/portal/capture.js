// ===========================================================================
// BF6 Unreal SDK - Portal page capture.
//
// Injected into every page the tool's embedded browser loads. Two jobs:
//   1. tell the tool what page it is on (window.ue.bf6portal.pagestate)
//   2. hand the tool the RESPONSE BODIES of the site's own gRPC-web calls
//      (window.ue.bf6portal.capture)
//
// THE API IS THE NORMAL PATH. Reading the experience list, reading one
// experience and writing one back are all done by REPLAYING a request the site
// has already made this session, from inside the page, with the page's own
// headers and the page's own credentials. Clicking the site's pages is the last
// resort, for when the site has not made that call yet.
//
// WHAT NEVER LEAVES THIS PAGE. No password, no token, no cookie, no request
// header and no request body ever reaches the tool. Each WebPlay method's
// headers and last request body are remembered in variables INSIDE this page
// purely so a replay can be issued by the page itself, the way the site would
// have issued it. Only response bytes cross the bridge.
// ===========================================================================
(function () {
  var V = 2;
  if (window.BF6PortalCapture && window.BF6PortalCapture.v === V) {
    // Injected twice into the same document (a re-register, or the tool asking
    // for a fresh probe). Do not wrap fetch a second time.
    window.BF6PortalCapture.rearm();
    return;
  }

  var WEBPLAY = '/santiago.web.play.WebPlay/';
  var CHUNK = 512 * 1024;          // one bridge call carries at most this much base64
  var noop = function () {};
  var queue = [];
  var seqId = 0;
  var lastPlayInit = null;         // page-local only: headers + credentials
  var lastPlayUrl = '';

  // ---- the bridge ---------------------------------------------------------

  function bridge() {
    try { return (window.ue && window.ue.bf6portal) ? window.ue.bf6portal : null; }
    catch (e) { return null; }
  }

  function drain() {
    var b = bridge();
    if (!b) { setTimeout(drain, 250); return; }
    while (queue.length) {
      var it = queue.shift();
      try { b[it[0]](it[1]); }
      catch (e) { console.log('BF6CAPTURE bridge call failed: ' + e); }
    }
  }

  function send(fn, obj) {
    var s;
    try { s = JSON.stringify(obj); } catch (e) { return; }
    queue.push([fn, s]);
    drain();
  }

  // ---- response bodies ----------------------------------------------------

  function b64(buf) {
    var bytes = new Uint8Array(buf), bin = '', n = bytes.length, i = 0;
    for (; i + 0x8000 < n; i += 0x8000) bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    bin += String.fromCharCode.apply(null, bytes.subarray(i));
    return btoa(bin);
  }

  function methodOf(url) {
    var at = url.indexOf(WEBPLAY);
    if (at < 0) return '';
    var tail = url.slice(at + WEBPLAY.length);
    var cut = tail.search(/[?#\/]/);
    return cut < 0 ? tail : tail.slice(0, cut);
  }

  function emit(method, status, url, buf) {
    // THE SITE'S OWN VERDICT on an import, rather than the tool assuming one.
    // The import is finished when the site itself writes the experience back,
    // and this is that call going past. Nothing is inferred from the click.
    try {
      if (importPending && /^(update|save|create)/i.test(method)) {
        var waited = Date.now() - importPending.at;
        send('pagestate', {
          v: 2, kind: pageKind(location.pathname), url: location.href,
          importVerdict: method + ':' + status, importDetail: 'after ' + waited + ' ms'
        });
        if (status >= 200 && status < 300) importPending = null;
      }
    } catch (e) {}
    var body;
    try { body = b64(buf); } catch (e) { console.log('BF6CAPTURE could not encode ' + method + ': ' + e); return; }
    if (body.length <= CHUNK) {
      send('capture', { v: V, method: method, status: status, url: url, bodyB64: body });
      console.log('BF6CAPTURE ' + method + ' ' + status + ' ' + body.length + ' b64 chars');
      return;
    }
    var id = 'c' + (++seqId) + '-' + Date.now();
    var of = Math.ceil(body.length / CHUNK);
    for (var i = 0; i < of; i++) {
      send('capture', {
        v: V, method: method, status: status, url: url,
        id: id, seq: i, of: of, bodyB64: body.substr(i * CHUNK, CHUNK)
      });
    }
    console.log('BF6CAPTURE ' + method + ' ' + status + ' in ' + of + ' chunks (' + body.length + ' b64 chars)');
  }

  // ---- remembering how the site asks (page-local, never forwarded) ---------

  function headersOf(input, init) {
    var out = {};
    function add(h) {
      if (!h) return;
      try {
        if (typeof h.forEach === 'function' && typeof h.get === 'function') { h.forEach(function (v, k) { out[k] = v; }); return; }
        if (Object.prototype.toString.call(h) === '[object Array]') { for (var i = 0; i < h.length; i++) out[h[i][0]] = h[i][1]; return; }
        for (var k in h) if (Object.prototype.hasOwnProperty.call(h, k)) out[k] = h[k];
      } catch (e) {}
    }
    try { if (input && input.headers) add(input.headers); } catch (e) {}
    if (init && init.headers) add(init.headers);
    return out;
  }

  // Remembered PER METHOD, so any WebPlay call the site makes can be replayed
  // the way the site made it. All of this stays inside the page.
  //
  // THIS IS THE API SEAM. Everything the tool reads and writes goes through a
  // replay of a call the site has already made, with the site's own headers and
  // the site's own credentials, issued by the page itself. Nothing is composed
  // out of thin air and no credential is ever read, stored or forwarded.
  var lastInit = {};       // method -> {headers, credentials, url}
  var lastBody = {};       // method -> the raw request body the site sent, page-local

  function remember(input, init, url) {
    var cred = 'include';
    try { cred = (init && init.credentials) || (input && input.credentials) || 'include'; } catch (e) {}
    var m = methodOf(url);
    var rec = { headers: headersOf(input, init), credentials: cred, url: url };
    lastInit[m] = rec;
    if (m === 'getPlayElement') { lastPlayInit = rec; lastPlayUrl = url; }
    // The FULL message the site last sent for this method. It never leaves this
    // page; it is here so a change can be sent as the whole message with one
    // field different, rather than as a partial update - a partial
    // updatePlayElement wipes the experience's attachments.
    try {
      var body = (init && init.body) || (input && input.body) || null;
      if (body instanceof ArrayBuffer) lastBody[m] = new Uint8Array(body.slice(0));
      else if (body && body.buffer instanceof ArrayBuffer) lastBody[m] = new Uint8Array(body.buffer.slice(0));
    } catch (e) {}
  }

  // The methods the site has issued this session, so the tool knows whether the
  // API path is open or whether it still has to drive the page.
  function apiMethods() {
    var out = [];
    for (var k in lastInit) if (Object.prototype.hasOwnProperty.call(lastInit, k)) out.push(k);
    return out;
  }

  // Which pagestate field a replay reports on. Push has its own so a whole
  // experience save is never read as a thumbnail step.
  function channelFor(tag) { return tag === 'push' ? 'push' : 'thumb'; }

  function replayNote(tag, text) {
    var m = { v: V, kind: pageKind(location.pathname), url: location.href, api: apiMethods() };
    m[channelFor(tag)] = tag + ':' + text;
    send('pagestate', m);
  }

  function replay(method, bytes, tag) {
    var rec = lastInit[method];
    if (!rec) {
      console.log('BF6CAPTURE cannot replay ' + method + ': the site has not made that call yet');
      replayNote(tag, 'no-request-observed');
      return false;
    }
    var h = {};
    for (var k in rec.headers) if (Object.prototype.hasOwnProperty.call(rec.headers, k)) h[k] = rec.headers[k];
    h['content-type'] = 'application/grpc-web+proto';
    try {
      realFetch(rec.url, { method: 'POST', headers: h, body: bytes, credentials: rec.credentials || 'include' })
        .then(function (res) {
          res.arrayBuffer().then(function (buf) { emit(method, res.status, rec.url, buf); }, noop);
          replayNote(tag, String(res.status));
        }, function (e) {
          replayNote(tag, 'failed ' + e);
        });
    } catch (e) {
      replayNote(tag, 'threw ' + e);
      return false;
    }
    return true;
  }

  // ---- fetch --------------------------------------------------------------

  var realFetch = window.fetch;
  if (realFetch && !realFetch.__bf6) {
    var wrapped = function (input, init) {
      var url = '';
      try { url = (typeof input === 'string') ? input : ((input && input.url) || ''); } catch (e) {}
      var isPlay = url.indexOf(WEBPLAY) >= 0;
      if (isPlay) remember(input, init, url);
      var p = realFetch.apply(this, arguments);
      if (isPlay) {
        try {
          p.then(function (res) {
            // A redirect on an API call is a sign-out in disguise: the body is
            // then a login page, not a gRPC frame. Say so rather than letting
            // the parser fail on it.
            if (res.redirected || res.type === 'opaqueredirect') {
              send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, apiRedirect: res.url || url });
            }
            try { res.clone().arrayBuffer().then(function (buf) { emit(methodOf(url), res.status, url, buf); }, noop); }
            catch (e) {}
          }, noop);
        } catch (e) {}
      }
      return p;
    };
    wrapped.__bf6 = true;
    window.fetch = wrapped;
  }

  // ---- XMLHttpRequest -----------------------------------------------------

  function readXhr(x) {
    var url = x.__bf6url || '', m = methodOf(url), r = null;
    try { r = x.response; } catch (e) {}
    if (r instanceof ArrayBuffer) { emit(m, x.status, url, r); return; }
    if (typeof Blob !== 'undefined' && r instanceof Blob) {
      var fr = new FileReader();
      fr.onload = function () { emit(m, x.status, url, fr.result); };
      fr.readAsArrayBuffer(r);
      return;
    }
    var s = '';
    try { s = (typeof r === 'string') ? r : (x.responseText || ''); } catch (e) { return; }
    if (!s) return;
    var b = new Uint8Array(s.length);
    for (var i = 0; i < s.length; i++) b[i] = s.charCodeAt(i) & 0xFF;
    emit(m, x.status, url, b.buffer);
  }

  var XP = window.XMLHttpRequest && window.XMLHttpRequest.prototype;
  if (XP && !XP.__bf6) {
    var ro = XP.open, rs = XP.send;
    XP.open = function (method, u) {
      try { this.__bf6url = u || ''; } catch (e) {}
      return ro.apply(this, arguments);
    };
    XP.send = function () {
      var self = this;
      try {
        if ((self.__bf6url || '').indexOf(WEBPLAY) >= 0) {
          self.addEventListener('load', function () { try { readXhr(self); } catch (e) {} });
        }
      } catch (e) {}
      return rs.apply(this, arguments);
    };
    XP.__bf6 = true;
  }

  // ---- the replay ---------------------------------------------------------
  //
  // One gRPC-web frame carrying a message whose field 1 is the uuid string:
  //   0x0a, len, bytes   then the 5 byte frame header (flags + big-endian len).

  function uuidFrame(id) {
    var body = [0x0a, id.length];
    for (var i = 0; i < id.length; i++) body.push(id.charCodeAt(i) & 0x7F);
    var n = body.length;
    var out = new Uint8Array(5 + n);
    out[0] = 0;
    out[1] = (n >>> 24) & 255; out[2] = (n >>> 16) & 255; out[3] = (n >>> 8) & 255; out[4] = n & 255;
    out.set(body, 5);
    return out;
  }

  // NO CONSTRUCTED URLS. An "?id=<uuid>" experience address was inferred, not
  // observed, and the site answers it with the login page: the real site keeps
  // the open experience in app state, so the only reliable way to make it issue
  // getPlayElement is to do what a user does - stand on the experiences list and
  // click the card. Everything below finds that card in the site's own DOM.

  // WHAT THE PAGE IS COMPLAINING ABOUT.
  //
  // The tool no longer drives the site's Blockly and script editors live: it
  // downloads the experience, fleshes out a TypeScript template project from
  // it, and imports over the top to update. That trade is fine, but it costs
  // the one thing the live page gave for free - the site telling you your rules
  // are wrong. So the warnings are collected and handed back, and the tool
  // shows them beside the experience they belong to.
  //
  // Read from the page's own accessibility surface rather than from class names
  // chosen by whoever styled it: role="alert" and aria-invalid are what the
  // site uses to tell a screen reader something is wrong, and they change far
  // less often than CSS does. Anything found by a class name is a bonus on top.
  function collectNotices() {
    var out = [];
    var seen = {};
    function add(text, kind) {
      if (!text) { return; }
      var t = String(text).replace(/\s+/g, ' ').trim();
      // Long enough to mean something, short enough not to be the whole page.
      if (t.length < 4 || t.length > 400) { return; }
      var key = kind + '|' + t;
      if (seen[key]) { return; }
      seen[key] = 1;
      out.push({ kind: kind, text: t });
    }
    function sweep(sel, kind) {
      var els;
      try { els = document.querySelectorAll(sel); } catch (e) { return; }
      for (var i = 0; i < els.length && out.length < 40; i++) {
        var el = els[i];
        // Something hidden is not something the user is being warned about.
        try {
          if (el.offsetParent === null && el.getAttribute('role') !== 'alert') { continue; }
        } catch (e) {}
        add(el.textContent, kind);
      }
    }
    sweep('[role="alert"]', 'error');
    sweep('[aria-invalid="true"]', 'error');
    sweep('[aria-errormessage]', 'error');
    sweep('[role="status"][aria-live="assertive"]', 'warning');
    // The styled surfaces, tried after the semantic ones so a page that marks
    // things up properly never depends on these.
    sweep('[class*="error" i]:not(:has([class*="error" i]))', 'error');
    sweep('[class*="warning" i]:not(:has([class*="warning" i]))', 'warning');
    return out;
  }

  // Reported on a change, not on a timer: the same three warnings resent every
  // few seconds would be noise in the log and would keep re-alarming the panel.
  var lastNoticeKey = '';
  function reportNotices(force) {
    var list;
    try { list = collectNotices(); } catch (e) { return; }
    var key = JSON.stringify(list);
    if (!force && key === lastNoticeKey) { return; }
    lastNoticeKey = key;
    send('pagestate', {
      v: V, kind: pageKind(location.pathname), url: location.href, notices: list
    });
  }

  function reportOpen(id, how, selector, before) {
    // The site routes without a reload, so the address is read a beat later.
    setTimeout(function () {
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        open: { id: id, how: how, selector: selector || '', before: before, after: location.href }
      });
    }, 1500);
  }

  function findCard(id, name) {
    var wanted = String(id || '').toLowerCase(), res = null;
    function note(el, sel) { if (!res && el) res = { el: el, sel: sel }; }
    try {
      // 1. the uuid in a link target
      if (wanted) {
        var as = document.querySelectorAll('a[href]');
        for (var i = 0; i < as.length && !res; i++) {
          if ((as[i].getAttribute('href') || '').toLowerCase().indexOf(wanted) >= 0) {
            note(as[i], 'a[href*="' + wanted.slice(0, 8) + '"]');
          }
        }
      }
      // 2. the uuid in any attribute of any element
      if (!res && wanted) {
        var all = document.querySelectorAll('[id],[data-id],[data-testid],[data-playelementid],[data-experience-id],[aria-labelledby]');
        for (var j = 0; j < all.length && !res; j++) {
          var at = all[j].attributes;
          for (var k = 0; k < at.length; k++) {
            if ((at[k].value || '').toLowerCase().indexOf(wanted) >= 0) {
              note(all[j], '[' + at[k].name + '*="' + wanted.slice(0, 8) + '"]');
              break;
            }
          }
        }
      }
      // 3. THE NAME AS A TEXT NODE, which is how the site actually writes it.
      //
      // Observed on the live list: the uuid appears NOWHERE in the page, and
      // the name sits as a bare text node in a div that also holds the tile's
      // icon element, so no element's own text equals the name and matching on
      // elements finds nothing at all. Walking text nodes finds it every time.
      if (!res && name) {
        var want = String(name).replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
        var w = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, null), tn;
        while ((tn = w.nextNode())) {
          var tv = (tn.nodeValue || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
          if (tv && tv === want) { note(tn.parentElement, 'name text node'); break; }
        }
      }
      // 4. the same name, allowing an element whose whole text is the name
      if (!res && name) {
        var cands = document.querySelectorAll('a,article,li,button,[role="button"],h1,h2,h3,h4,span,div');
        for (var m = 0; m < cands.length && !res; m++) {
          var t = '';
          try { t = (cands[m].innerText || cands[m].textContent || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, ''); } catch (e) {}
          if (t && t === name) note(cands[m], 'exact card text');
        }
      }
    } catch (e) {}
    return res;
  }

  // The tile a matched element belongs to, and the control on it that opens the
  // experience for editing.
  //
  // Observed on the live list: a tile is <section class="experience-tile-module_
  // tile__..."> and carries three buttons, "Publish", "Modify" and one with no
  // text. MODIFY is the one that opens the experience; there is no anchor, no
  // role and no tabindex anywhere on the tile, so a generic "closest clickable"
  // lands on nothing.
  function tileControl(el) {
    var t = el, i = 0;
    while (t && i++ < 12) {
      var cn = '';
      try { cn = String(t.className || ''); } catch (e) {}
      if (/experience-tile-module_tile/.test(cn)) break;
      t = t.parentElement;
    }
    if (!t) return null;
    var btns = [];
    try { btns = t.querySelectorAll('button,a,[role="button"]'); } catch (e) { return null; }
    for (var j = 0; j < btns.length; j++) {
      var tx = '';
      try { tx = (btns[j].innerText || btns[j].textContent || '').replace(/^\s+|\s+$/g, ''); } catch (e) {}
      if (/^(modify|edit|continue editing|open)$/i.test(tx)) return { el: btns[j], sel: 'tile button "' + tx + '"' };
    }
    // No named control: the tile itself may open on a click.
    return { el: t, sel: 'the tile itself' };
  }

  function openExperience(id, name) {
    var before = location.href;
    if (location.pathname.indexOf('/bf6/experiences') !== 0) {
      console.log('BF6CAPTURE not on the experiences list, cannot click a card here');
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        open: { id: id, how: 'need-list', selector: '', before: before, after: before }
      });
      return false;
    }
    var hit = findCard(id, name);
    if (!hit) {
      console.log('BF6CAPTURE no card on the experiences list for ' + String(id).slice(0, 8) + (name ? ' / ' + name : ''));
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        open: { id: id, how: 'no-card', selector: '', before: before, after: before }
      });
      return false;
    }
    var ctl = tileControl(hit.el);
    var how = ctl ? ctl.sel : hit.sel;
    console.log('BF6CAPTURE opening ' + String(id).slice(0, 8) + ': found by ' + hit.sel + ', clicking ' + how);
    try {
      var target = ctl ? ctl.el : hit.el;
      if (!ctl && target.closest) target = target.closest('a,button,[role="button"]') || hit.el;
      target.click();
    } catch (e) {
      console.log('BF6CAPTURE the card would not take a click: ' + e);
    }
    reportOpen(id, 'click', hit.sel + ' -> ' + how, before);
    return true;
  }

  function getPlayElement(id, name) {
    id = String(id || '').replace(/^\s+|\s+$/g, '');
    if (!/^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$/.test(id)) {
      console.log('BF6CAPTURE replay refused: not a uuid');
      return false;
    }
    if (!lastPlayUrl || !lastPlayInit) {
      // Never seen the site issue one, so there is nothing to imitate. Do what a
      // user does instead: click the card. NO url is invented.
      console.log('BF6CAPTURE no getPlayElement seen yet, opening the experience the way a user does');
      return openExperience(id, name);
    }
    var h = {};
    for (var k in lastPlayInit.headers) if (Object.prototype.hasOwnProperty.call(lastPlayInit.headers, k)) h[k] = lastPlayInit.headers[k];
    h['content-type'] = 'application/grpc-web+proto';
    try {
      realFetch(lastPlayUrl, {
        method: 'POST', headers: h, body: uuidFrame(id),
        credentials: lastPlayInit.credentials || 'include'
      }).then(function (res) {
        res.arrayBuffer().then(function (buf) { emit('getPlayElement', res.status, lastPlayUrl, buf); }, noop);
      }, function (e) { console.log('BF6CAPTURE replay failed: ' + e); });
    } catch (e) {
      console.log('BF6CAPTURE replay threw: ' + e);
      return false;
    }
    console.log('BF6CAPTURE replaying getPlayElement ' + id.slice(0, 8));
    return true;
  }

  // THE LIST, ON DEMAND. The site asks for it once, when the experiences page
  // first mounts, and never again unless the user goes back there. Replaying
  // that exact request - the site's own url, headers and body, byte for byte -
  // gets a fresh list from wherever the panel happens to be standing, with no
  // navigation and nothing invented. No remembered request means no replay, and
  // the tool is told so rather than being given a guess.
  function getOwnedList() {
    var rec = lastInit['getOwnedPlayElementsV2'];
    var body = lastBody['getOwnedPlayElementsV2'];
    if (!rec || !body) {
      console.log('BF6CAPTURE cannot refresh the list: the site has not asked for it yet this session');
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        api: apiMethods(), list: 'no-request-observed'
      });
      return false;
    }
    var h = {};
    for (var k in rec.headers) if (Object.prototype.hasOwnProperty.call(rec.headers, k)) h[k] = rec.headers[k];
    h['content-type'] = 'application/grpc-web+proto';
    try {
      realFetch(rec.url, { method: 'POST', headers: h, body: body, credentials: rec.credentials || 'include' })
        .then(function (res) {
          res.arrayBuffer().then(function (buf) { emit('getOwnedPlayElementsV2', res.status, rec.url, buf); }, noop);
          send('pagestate', {
            v: V, kind: pageKind(location.pathname), url: location.href,
            api: apiMethods(), list: 'replayed:' + res.status
          });
        }, function (e) {
          send('pagestate', {
            v: V, kind: pageKind(location.pathname), url: location.href,
            api: apiMethods(), list: 'failed:' + e
          });
        });
    } catch (e) {
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        api: apiMethods(), list: 'threw:' + e
      });
      return false;
    }
    console.log('BF6CAPTURE replaying getOwnedPlayElementsV2');
    return true;
  }

  // ---- the site's own Export, caught before it reaches disk ----------------
  //
  // THE COMPLETE PAYLOAD. The experience tile's Export item is the only route
  // that hands over an experience's spatial, script and workspace data whole,
  // and it is entirely CLIENT SIDE: the page builds a Blob and clicks an anchor
  // with a download attribute. Nothing goes to the server.
  //
  // A CEF download cannot be intercepted from an editor plugin, so it is caught
  // one level up, in the page, by remembering the Blob that
  // URL.createObjectURL was handed and the name the anchor asked to save it
  // under. The wrappers are installed once, are transparent, and are never
  // removed; an unrelated blob is ignored, because only a download whose name
  // ends in "_experience.json" is taken. The file never reaches disk.
  //
  // The tile's controls, read off the live page: three buttons, all class
  // icon-button-module_button__KJTSK, reading "Publish", "Modify" and one with
  // no text and no aria-label at all. The LAST one opens a menu whose items
  // are, in order, Export, Duplicate, Delete, Publish.

  var lastBlob = null;             // {blob, url}
  var lastDownloadName = '';
  var exportWait = null;           // {id, name, deadline}

  var realCreateObjectURL = (window.URL && window.URL.createObjectURL) ? window.URL.createObjectURL : null;
  if (realCreateObjectURL && !realCreateObjectURL.__bf6) {
    var wrapCreate = function (obj) {
      var url = realCreateObjectURL.apply(this, arguments);
      try { if (obj && typeof Blob !== 'undefined' && obj instanceof Blob) lastBlob = { blob: obj, url: url }; } catch (e) {}
      return url;
    };
    wrapCreate.__bf6 = true;
    try { window.URL.createObjectURL = wrapCreate; } catch (e) {}
  }

  var AP = window.HTMLAnchorElement && window.HTMLAnchorElement.prototype;
  if (AP && !AP.__bf6click) {
    var realAClick = AP.click;
    AP.click = function () {
      try {
        var dl = this.getAttribute && this.getAttribute('download');
        if (dl) {
          lastDownloadName = dl;
          if (exportWait && /_experience\.json$/i.test(dl) && lastBlob) {
            // Ours. Read it here and let nothing reach the disk.
            var pending = exportWait;
            exportWait = null;
            takeExportBlob(lastBlob.blob, dl, pending);
            return;   // the download is deliberately not started
          }
        }
      } catch (e) {}
      return realAClick.apply(this, arguments);
    };
    AP.__bf6click = true;
  }

  function sendExportText(text, name, pending) {
    var id = 'x' + (++seqId) + '-' + Date.now();
    var of = Math.ceil(text.length / CHUNK) || 1;
    for (var i = 0; i < of; i++) {
      send('capture', {
        v: V, method: 'exportExperience', status: 200, url: location.href,
        experienceId: pending ? pending.id : '', name: name, text: true,
        id: id, seq: i, of: of, bodyB64: text.substr(i * CHUNK, CHUNK)
      });
    }
    console.log('BF6CAPTURE export "' + name + '" caught, ' + text.length + ' characters in ' + of + ' chunk(s)');
  }

  function takeExportBlob(blob, name, pending) {
    try {
      blob.text().then(function (t) { sendExportText(t, name, pending); }, function (e) {
        send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'blob-read-failed:' + e });
      });
    } catch (e) {
      // Older engines have no Blob.text.
      try {
        var fr = new FileReader();
        fr.onload = function () { sendExportText(String(fr.result || ''), name, pending); };
        fr.readAsText(blob);
      } catch (e2) {
        send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'blob-unreadable:' + e2 });
      }
    }
  }

  // The tile a name text node belongs to, and its LAST button, which is the one
  // with no text that opens the menu.
  function tileMenuButton(el) {
    var t = el, i = 0;
    while (t && i++ < 12) {
      var cn = '';
      try { cn = String(t.className || ''); } catch (e) {}
      if (/experience-tile-module_tile/.test(cn)) break;
      t = t.parentElement;
    }
    if (!t) return null;
    var btns = [];
    try { btns = t.querySelectorAll('button,[role="button"]'); } catch (e) { return null; }
    if (!btns.length) return null;
    // The unlabelled one, by preference; otherwise the last.
    for (var j = btns.length - 1; j >= 0; j--) {
      var tx = '';
      try { tx = (btns[j].innerText || btns[j].textContent || '').replace(/^\s+|\s+$/g, ''); } catch (e) {}
      var al = '';
      try { al = btns[j].getAttribute('aria-label') || ''; } catch (e) {}
      if (!tx && !al) return btns[j];
    }
    return btns[btns.length - 1];
  }

  function clickMenuItem(text) {
    var want = String(text).toLowerCase();
    var items = [];
    try { items = document.querySelectorAll('[role="menuitem"],[role="option"],li,button,a'); } catch (e) { return false; }
    for (var i = 0; i < items.length; i++) {
      var tx = '';
      try { tx = (items[i].innerText || items[i].textContent || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, ''); } catch (e) {}
      if (tx.toLowerCase() === want) {
        try { items[i].click(); } catch (e) { return false; }
        return true;
      }
    }
    return false;
  }

  // Export one experience by driving its own tile menu. The name is what finds
  // the tile, because the uuid appears nowhere in the list's DOM.
  function exportExperience(id, name) {
    if (location.pathname.indexOf('/bf6/experiences') !== 0) {
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        exportState: 'need-list'
      });
      return false;
    }
    var hit = findCard(id, name);
    if (!hit) {
      send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'no-card' });
      return false;
    }
    var btn = tileMenuButton(hit.el);
    if (!btn) {
      send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'no-menu-button' });
      return false;
    }
    exportWait = { id: String(id || ''), name: String(name || ''), deadline: Date.now() + 20000 };
    try { btn.click(); } catch (e) {
      exportWait = null;
      send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'menu-button-refused:' + e });
      return false;
    }
    // The menu is rendered a beat after the click; Export is its first item.
    var tries = 0;
    (function tryItem() {
      tries++;
      if (clickMenuItem('Export')) {
        send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'clicked' });
        return;
      }
      if (tries < 12) { setTimeout(tryItem, 250); return; }
      exportWait = null;
      send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, exportState: 'no-export-item' });
    })();
    return true;
  }

  // ---- PUSH BY IMPORT, the site's own way in -------------------------------
  //
  // THE RIGHT CONTROL. The import on the experiences front page creates a NEW
  // experience and asks the user to name it, so it is never touched here. The
  // one that replaces an open experience's contents lives INSIDE the experience
  // editor, which is reached the way a user reaches it: the tile's "Modify"
  // button, which the tool already clicks.
  //
  // THE FILE, WITHOUT A DIALOG. A CEF file dialog cannot be intercepted from an
  // editor plugin, so none is opened. A page can be handed a file directly, and
  // that is what happens: a DataTransfer carrying a real File is assigned to the
  // control's own input[type=file] and a change event is dispatched, which is
  // what the browser does when a person picks a file. If the control turns out
  // to want a drag instead, the same DataTransfer goes out as a drop event on
  // its drop zone. No dialog, no credential, and the site's own importer does
  // all of the work.

  // ---- THE IMPORT SELECTORS -----------------------------------------------
  //
  // Every anchor the push depends on lives here and NOWHERE else, so a site
  // redesign is re-anchored in one block. Each is logged with how many elements
  // it matched on every use, which is what makes a moved anchor one line in the
  // Output Log rather than a feature that silently stops working.
  //
  // Probed live on the owner's signed-in editor, 2026-09-06, on
  // /bf6/experience/settings/mode?id=<uuid>&teams=1%2C2 :
  //   input[type=file][accept="application/json"]   exactly one, class
  //     "visuallyHidden", not disabled, multiple false, no form, hidden
  //     (offsetParent null). No click is needed to bring it into being.
  //   label._fileInputLabel_1itju_6                 fronts it, text "Import"
  //   button.menu-item-module_item__S1V0Y           a menu item, text "Import"
  var ISEL = {
    fileInput:  'input[type="file"][accept="application/json"]',
    anyFile:    'input[type="file"]',
    label:      'label[class*="_fileInputLabel_"]',
    menuItem:   'button[class*="menu-item-module_item"]',
    importText: /^\s*import\s*$/i
  };

  var toolInbox = {};        // transfer id -> {parts, of}
  var importPending = null;  // {id, name, at}

  // THE HANDSHAKE. A navigation replaces the document, and this script is
  // injected into the new one a beat AFTER the address changes. Anything the
  // tool sends in that gap lands on nothing: window.BF6PortalCapture is not
  // there yet and every call throws. So the tool never sends first. It probes
  // with a token, and only a live document can answer, because only a live
  // document has this function in it. A stale flag cannot pass for a ready
  // page, because the token is new for every push.
  function pushReady(token) {
    send('pagestate', {
      v: V, kind: pageKind(location.pathname), url: location.href,
      pushReady: String(token || ''), importDetail: selectorReport()
    });
    return true;
  }

  // One transfer at a time, keyed by its own id. Starting a transfer drops
  // every other one: chunks from a previous document, or from a push that was
  // abandoned, are meaningless and must never be spliced into this one.
  function toolBegin(id, of) {
    toolInbox = {};
    toolInbox[id] = { parts: new Array(of), of: of, got: 0 };
    return true;
  }

  function toolChunk(id, seq, of, b64) {
    var box = toolInbox[id];
    if (!box) {
      // No receiver for this id. The transfer it belongs to was never begun in
      // THIS document, so it is refused rather than half-assembled.
      console.log('BF6CAPTURE chunk for an unknown transfer ' + id + ', refused');
      send('pagestate', {
        v: V, kind: pageKind(location.pathname), url: location.href,
        transferState: 'no-receiver', importDetail: id
      });
      return false;
    }
    if (box.parts[seq] === undefined) box.got++;
    box.parts[seq] = b64;
    return true;
  }

  function toolMissing(id) {
    var box = toolInbox[id];
    if (!box) return -1;
    return box.of - box.got;
  }

  function toolText(id) {
    var box = toolInbox[id];
    if (!box) return null;
    for (var i = 0; i < box.of; i++) if (box.parts[i] === undefined) return null;
    var joined = box.parts.join('');
    delete toolInbox[id];
    try {
      var bin = atob(joined), a = new Uint8Array(bin.length);
      for (var j = 0; j < bin.length; j++) a[j] = bin.charCodeAt(j) & 0xFF;
      return new TextDecoder('utf-8').decode(a);
    } catch (e) {
      console.log('BF6CAPTURE import payload did not decode: ' + e);
      return null;
    }
  }

  function importNote(state, detail) {
    send('pagestate', {
      v: V, kind: pageKind(location.pathname), url: location.href,
      importState: state, importDetail: detail || ''
    });
  }

  function uuidIn(s) {
    var m = String(s || '').match(/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/);
    return m ? m[0].toLowerCase() : '';
  }

  // Everything on this page that could be the import control, reported whole so
  // the first live run pins the selector down in one pass rather than ten.
  function describeControls() {
    var out = [];
    try {
      var all = document.querySelectorAll('button,[role="menuitem"],[role="button"],label,a,input[type="file"]');
      for (var i = 0; i < all.length && out.length < 80; i++) {
        var e = all[i], t = '';
        try { t = (e.innerText || e.textContent || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '').slice(0, 40); } catch (x) {}
        var cn = '';
        try { cn = String(e.className || '').slice(0, 60); } catch (x) {}
        var al = '';
        try { al = e.getAttribute('aria-label') || ''; } catch (x) {}
        out.push(e.tagName.toLowerCase() + (e.type ? '[' + e.type + ']' : '') + ' "' + t + '"' + (al ? ' aria="' + al + '"' : '') + ' .' + cn);
      }
    } catch (e) {}
    return out.join(' | ');
  }


  // Anything whose own text is exactly "import". The label and the menu item
  // are both found this way, and both are named in the selector block above.
  function findImportControl() {
    var hits = [];
    try {
      var all = document.querySelectorAll('button,[role="menuitem"],[role="button"],label,a');
      for (var i = 0; i < all.length; i++) {
        var t = '';
        try { t = (all[i].innerText || all[i].textContent || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, ''); } catch (e) {}
        if (ISEL.importText.test(t)) {
          var cn = '';
          try { cn = String(all[i].className || '').slice(0, 50); } catch (e) {}
          hits.push({ el: all[i], sel: all[i].tagName.toLowerCase() + '.' + cn + ' "' + t + '"' });
          continue;
        }
        var al = '';
        try { al = all[i].getAttribute('aria-label') || ''; } catch (e) {}
        if (ISEL.importText.test(al)) hits.push({ el: all[i], sel: all[i].tagName.toLowerCase() + ' aria-label="' + al + '"' });
      }
    } catch (e) {}
    // The label that fronts the hidden input first, then the menu item.
    hits.sort(function (a, b) {
      var la = /^label/.test(a.sel) ? 0 : 1, lb = /^label/.test(b.sel) ? 0 : 1;
      return la - lb;
    });
    return hits;
  }

  // The unlabelled overflow button, same pattern the list page uses: its menu
  // there holds Export / Duplicate / Delete / Publish, and the editor's is
  // expected to hold Import beside them.
  function overflowButtons() {
    var out = [];
    try {
      var btns = document.querySelectorAll('button,[role="button"]');
      for (var i = 0; i < btns.length; i++) {
        var t = '', al = '';
        try { t = (btns[i].innerText || btns[i].textContent || '').replace(/^\s+|\s+$/g, ''); } catch (e) {}
        try { al = btns[i].getAttribute('aria-label') || ''; } catch (e) {}
        if (!t && !al) out.push(btns[i]);
      }
    } catch (e) {}
    return out;
  }

  // The verified anchor first, then anything that takes a file. Both counts are
  // reported, so a moved anchor shows up as a number rather than as silence.
  function fileInputs(outCounts) {
    var exact = [], any = [];
    try { exact = document.querySelectorAll(ISEL.fileInput); } catch (e) {}
    try { any = document.querySelectorAll(ISEL.anyFile); } catch (e) {}
    if (outCounts) { outCounts.exact = exact.length; outCounts.any = any.length; }
    return exact.length ? exact : any;
  }

  function selectorReport() {
    var c = {};
    fileInputs(c);
    var labels = 0, items = 0;
    try { labels = document.querySelectorAll(ISEL.label).length; } catch (e) {}
    try { items = document.querySelectorAll(ISEL.menuItem).length; } catch (e) {}
    return 'fileInput=' + c.exact + ' anyFileInput=' + c.any + ' fileInputLabel=' + labels + ' menuItem=' + items;
  }

  // Hand the file over the way the browser does when a person picks one.
  function handFile(input, text, name) {
    try {
      var dt = new DataTransfer();
      dt.items.add(new File([text], name, { type: 'application/json' }));
      input.files = dt.files;
      input.dispatchEvent(new Event('change', { bubbles: true }));
      return true;
    } catch (e) {
      console.log('BF6CAPTURE could not hand the file to the input: ' + e);
      return false;
    }
  }

  // The same file, as a drop, for a control that wants a drag instead.
  function dropFile(zone, text, name) {
    try {
      var dt = new DataTransfer();
      dt.items.add(new File([text], name, { type: 'application/json' }));
      ['dragenter', 'dragover', 'drop'].forEach(function (type) {
        var ev = new DragEvent(type, { bubbles: true, cancelable: true, dataTransfer: dt });
        zone.dispatchEvent(ev);
      });
      return true;
    } catch (e) {
      console.log('BF6CAPTURE could not drop the file: ' + e);
      return false;
    }
  }

  function dropZones() {
    var out = [];
    try {
      var all = document.querySelectorAll('div,section,form,label');
      for (var i = 0; i < all.length && out.length < 6; i++) {
        var t = '';
        try { t = (all[i].innerText || '').toLowerCase(); } catch (e) {}
        if (!t || t.length > 400) continue;
        if (t.indexOf('drag') >= 0 || t.indexOf('drop') >= 0 || t.indexOf('browse') >= 0) out.push(all[i]);
      }
    } catch (e) {}
    return out;
  }

  // chunkId names the payload already sent across; expectedId is the experience
  // the tool believes is open, and a page that says otherwise is refused.
  function importExperience(chunkId, expectedId, name) {
    // NEVER the front page. Its import makes a new experience and asks for a
    // name; the one that replaces an open experience is inside the editor.
    if (location.pathname.indexOf('/bf6/experiences') === 0) {
      importNote('front-page', 'the experiences list import creates a new experience, so it is never used');
      return false;
    }
    if (location.pathname.indexOf('/bf6/experience/') !== 0) {
      importNote('not-in-editor', location.pathname);
      return false;
    }
    var open = uuidIn(location.href);
    if (expectedId && open && open !== String(expectedId).toLowerCase()) {
      importNote('wrong-experience', 'the page is on ' + open + ' but the tool is pushing ' + expectedId);
      return false;
    }
    if (expectedId && !open) {
      importNote('no-id-on-page', 'the address does not name an experience, so the tool will not import into it');
      return false;
    }
    var text = toolText(chunkId);
    if (!text) {
      var missing = toolMissing(chunkId);
      importNote('no-payload', missing < 0
        ? ('transfer ' + chunkId + ' never reached this document')
        : (missing + ' chunk(s) of transfer ' + chunkId + ' are missing'));
      return false;
    }
    var fileName = (name || 'experience') + '_experience.json';

    importPending = { id: expectedId || open, name: fileName, at: Date.now() };
    var anchors = selectorReport();
    console.log('BF6CAPTURE import: ' + text.length + ' characters into ' + (expectedId || open) + ' [' + anchors + ']');

    // 1. THE VERIFIED PATH. The control is a plain hidden file input that is
    // already on the page; no menu and no click are needed to bring it into
    // being. Probed live 2026-09-06.
    var counts = {};
    var ins = fileInputs(counts);
    if (ins.length && handFile(ins[0], text, fileName)) {
      importNote('handed', (counts.exact ? ISEL.fileInput : ISEL.anyFile) + ' already on the page [' + anchors + ']');
      return true;
    }

    // 2. a control whose text is exactly "import", clicked, then look again.
    // Only reached if a future build hides the input behind a click.
    var hits = findImportControl();
    var tried = [];
    function afterClick(sel, depth) {
      setTimeout(function () {
        var again = fileInputs();
        if (again.length && handFile(again[0], text, fileName)) {
          importNote('handed', 'after clicking ' + sel + ' [' + selectorReport() + ']');
          return;
        }
        var zones = dropZones();
        if (zones.length && dropFile(zones[0], text, fileName)) {
          importNote('dropped', 'after clicking ' + sel);
          return;
        }
        if (depth < 8) { afterClick(sel, depth + 1); return; }
        importNote('no-input', 'clicked ' + sel + ' but no file input or drop zone appeared. Controls seen: ' + describeControls());
      }, 300);
    }
    if (hits.length) {
      tried.push(hits[0].sel);
      try { hits[0].el.click(); } catch (e) {}
      afterClick(hits[0].sel, 0);
      return true;
    }

    // 3. the unlabelled overflow button, then an Import item in its menu
    var ovs = overflowButtons();
    for (var i = 0; i < ovs.length && i < 4; i++) {
      try { ovs[i].click(); } catch (e) { continue; }
      tried.push('overflow button ' + i);
    }
    setTimeout(function () {
      var menu = findImportControl();
      if (menu.length) {
        try { menu[0].el.click(); } catch (e) {}
        afterClick('menu item ' + menu[0].sel, 0);
        return;
      }
      importPending = null;
      importNote('no-control',
        'no control reading "import" on this page. Tried: ' + tried.join(', ') + '. Controls seen: ' + describeControls());
    }, 600);
    return true;
  }

  // ---- the experience thumbnail -------------------------------------------
  //
  // Three page-side jobs, none of which ever sees a credential of the tool's:
  //   uploadThumbnail   replay UploadExperienceThumbnail with the JPEG bytes
  //   pollVerification  ask the verification url whether the scan passed
  //   selectImage       drive the site's own Image Select dialog
  //   setThumbnailUrl   last resort: resend the site's OWN last updatePlayElement
  //                     message with the thumbnail string swapped, nothing else

  function varint(n, out) {
    while (n > 0x7F) { out.push((n & 0x7F) | 0x80); n >>>= 7; }
    out.push(n & 0x7F);
  }

  function frame(bodyArr) {
    var n = bodyArr.length;
    var out = new Uint8Array(5 + n);
    out[0] = 0;
    out[1] = (n >>> 24) & 255; out[2] = (n >>> 16) & 255; out[3] = (n >>> 8) & 255; out[4] = n & 255;
    out.set(bodyArr, 5);
    return out;
  }

  function bytesFromB64(b64) {
    var bin = atob(b64), n = bin.length, a = new Uint8Array(n);
    for (var i = 0; i < n; i++) a[i] = bin.charCodeAt(i) & 0xFF;
    return a;
  }

  // UploadExperienceThumbnail(image: bytes, mimeType: string) -> url, verificationUrl
  function uploadThumbnail(b64, mime) {
    var img;
    try { img = bytesFromB64(b64); } catch (e) { console.log('BF6CAPTURE thumbnail base64 did not decode'); return false; }
    mime = mime || 'image/jpeg';
    var body = [];
    body.push(0x0a); varint(img.length, body);
    for (var i = 0; i < img.length; i++) body.push(img[i]);
    body.push(0x12); varint(mime.length, body);
    for (var j = 0; j < mime.length; j++) body.push(mime.charCodeAt(j) & 0x7F);
    console.log('BF6CAPTURE uploading thumbnail, ' + img.length + ' bytes ' + mime);
    return replay('UploadExperienceThumbnail', frame(body), 'upload');
  }

  function pollVerification(url, tries) {
    tries = tries || 10;
    var n = 0;
    function once() {
      n++;
      try {
        realFetch(url, { method: 'GET', credentials: 'include', cache: 'no-store' }).then(function (res) {
          res.text().then(function (t) {
            var low = (t || '').toLowerCase();
            var done = res.status === 200 && low.indexOf('pending') < 0 && low.indexOf('processing') < 0;
            send('pagestate', {
              v: V, kind: pageKind(location.pathname), url: location.href,
              thumbVerify: (done ? 'ok:' : 'waiting:') + res.status + ' try ' + n + ' ' + low.slice(0, 120)
            });
            if (!done && n < tries) setTimeout(once, 2000);
          }, noop);
        }, function (e) {
          send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, thumbVerify: 'failed:' + e });
        });
      } catch (e) {
        send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, thumbVerify: 'threw:' + e });
      }
    }
    once();
    return true;
  }

  // The Image Select dialog on publish step two. Every option's displayed src
  // and label, so the tool can list the pre-approved generic set and, after an
  // upload, click the one whose src matches what came back.
  function imageOptions() {
    var out = [], seen = {};
    try {
      var scopes = document.querySelectorAll('[role="dialog"], [role="alertdialog"], [aria-modal="true"], main');
      for (var s = 0; s < scopes.length; s++) {
        var t = '';
        try { t = (scopes[s].innerText || '').toLowerCase(); } catch (e) {}
        var isPublish = location.pathname.indexOf('/bf6/experience/publish') === 0;
        if (!isPublish && t.indexOf('image select') < 0 && t.indexOf('experience images') < 0
            && t.indexOf('select an image to represent') < 0) continue;
        var imgs = scopes[s].querySelectorAll('img');
        for (var i = 0; i < imgs.length && out.length < 120; i++) {
          var src = imgs[i].currentSrc || imgs[i].src || '';
          if (!src || seen[src]) continue;
          seen[src] = 1;
          var label = imgs[i].getAttribute('alt') || '';
          if (!label) {
            try {
              var owner = imgs[i].closest('button,[role="button"],li,label') || imgs[i].parentElement;
              label = ((owner && (owner.getAttribute('aria-label') || owner.innerText)) || '').replace(/\s+/g, ' ').slice(0, 80);
            } catch (e) {}
          }
          out.push({ src: src, label: label });
        }
      }
    } catch (e) {}
    return out;
  }

  function selectImage(src) {
    var hit = null;
    try {
      var imgs = document.querySelectorAll('img');
      for (var i = 0; i < imgs.length; i++) {
        var s = imgs[i].currentSrc || imgs[i].src || '';
        if (s === src || (src && s.indexOf(src) >= 0) || (s && src.indexOf(s) >= 0)) { hit = imgs[i]; break; }
      }
      if (hit) {
        var target = hit.closest('button,[role="button"],label,li,a') || hit;
        target.click();
      }
    } catch (e) {}
    send('pagestate', {
      v: V, kind: pageKind(location.pathname), url: location.href,
      thumbSelect: hit ? 'ok' : 'notfound'
    });
    return !!hit;
  }

  // THE WRITE PATH, and the careful one.
  //
  // The community warns that a PARTIAL updatePlayElement wipes the experience's
  // attachments, so nothing here builds a message: it takes the site's own last
  // updatePlayElement body, FINDS the old string inside it, and rebuilds only
  // the messages that enclose it with corrected lengths. No field number is
  // guessed - the path is discovered from a real request - and every other byte
  // of the message is carried through untouched. If an old string is not in
  // there, it refuses rather than sending a message it does not understand.
  //
  // pairs is [[oldText, newText], ...]. Every field whose bytes are exactly an
  // old string becomes the matching new one; everything else is copied.

  function rdVarint(a, p) {
    var v = 0, sh = 0;
    while (p.i < a.length) { var b = a[p.i++]; v |= (b & 0x7F) << sh; if (!(b & 0x80)) return v; sh += 7; if (sh > 35) break; }
    return -1;
  }
  function copyRange(out, a, from, to) { for (var i = from; i < to; i++) out.push(a[i]); }

  // The site's last updatePlayElement message, without its grpc-web frame.
  function lastUpdateMessage() {
    var buf = lastBody['updatePlayElement'];
    if (!buf) return null;
    var off = 0;
    if (buf.length > 5 && buf[0] === 0) {
      var flen = (buf[1] << 24) | (buf[2] << 16) | (buf[3] << 8) | buf[4];
      if (flen === buf.length - 5) off = 5;
    }
    return buf.subarray(off);
  }

  // Returns {bytes, hits:{old -> count}} or null when nothing in this message
  // matched, so the caller can tell "not here" from "rewritten".
  function rewrite(a, pairs, hits, depth) {
    if (depth > 12) return null;
    var out = [], p = { i: 0 }, did = false;
    while (p.i < a.length) {
      var keyStart = p.i;
      var key = rdVarint(a, p);
      if (key < 0) return null;
      var wire = key & 7;
      var afterKey = p.i;
      if (wire === 0) { if (rdVarint(a, p) < 0) return null; copyRange(out, a, keyStart, p.i); continue; }
      if (wire === 1) { p.i += 8; if (p.i > a.length) return null; copyRange(out, a, keyStart, p.i); continue; }
      if (wire === 5) { p.i += 4; if (p.i > a.length) return null; copyRange(out, a, keyStart, p.i); continue; }
      if (wire !== 2) return null;
      var len = rdVarint(a, p);
      if (len < 0 || p.i + len > a.length) return null;
      var start = p.i;
      var sub = a.subarray(start, start + len);
      p.i = start + len;

      var matched = -1, txt = '';
      try { txt = new TextDecoder('utf-8', { fatal: false }).decode(sub); } catch (e) {}
      if (txt) for (var q = 0; q < pairs.length; q++) if (pairs[q][0] === txt) { matched = q; break; }
      if (matched >= 0) {
        var nb = new TextEncoder().encode(pairs[matched][1]);
        did = true;
        hits[matched] = (hits[matched] || 0) + 1;
        copyRange(out, a, keyStart, afterKey);      // the key, unchanged
        varint(nb.length, out);
        for (var n = 0; n < nb.length; n++) out.push(nb[n]);
        continue;
      }
      var inner = rewrite(sub, pairs, hits, depth + 1);
      if (inner) {
        did = true;
        copyRange(out, a, keyStart, afterKey);
        varint(inner.length, out);
        for (var m = 0; m < inner.length; m++) out.push(inner[m]);
        continue;
      }
      copyRange(out, a, keyStart, p.i);
    }
    return did ? out : null;
  }

  // Send the whole experience back with only the given strings different.
  // pairsJson is a JSON array of [old, new]; tag names the reporting channel.
  function pushStrings(pairsJson, tag) {
    tag = tag || 'push';
    function bail(why) {
      console.log('BF6CAPTURE ' + tag + ' refused: ' + why);
      var m = { v: V, kind: pageKind(location.pathname), url: location.href };
      m[tag === 'set' ? 'thumbSet' : 'push'] = 'refused:' + why;
      send('pagestate', m);
      return false;
    }
    var pairs;
    try { pairs = (typeof pairsJson === 'string') ? JSON.parse(pairsJson) : pairsJson; }
    catch (e) { return bail('the change list was not JSON'); }
    if (!pairs || !pairs.length) return bail('nothing changed');
    for (var i = 0; i < pairs.length; i++) {
      if (!pairs[i] || pairs[i].length !== 2 || typeof pairs[i][0] !== 'string' || typeof pairs[i][1] !== 'string') {
        return bail('a change was not a pair of strings');
      }
      if (!pairs[i][0]) return bail('a change had no current value to find');
    }
    var msg = lastUpdateMessage();
    if (!msg) return bail('the site has not saved this experience itself yet, so there is no message to resend');

    var hits = {};
    var rebuilt = rewrite(msg, pairs, hits, 0);
    if (!rebuilt) return bail('none of the current values are in the message the site last sent');
    var missing = [];
    for (var j = 0; j < pairs.length; j++) if (!hits[j]) missing.push(j);
    if (missing.length) {
      return bail(missing.length + ' of ' + pairs.length
        + ' change(s) had no matching value in the message the site last sent');
    }
    var total = 0;
    for (var h in hits) if (Object.prototype.hasOwnProperty.call(hits, h)) total += hits[h];
    console.log('BF6CAPTURE resending the site\'s own updatePlayElement message with ' + pairs.length
      + ' field(s) changed in ' + total + ' place(s) (' + msg.length + ' -> ' + rebuilt.length + ' bytes)');
    return replay('updatePlayElement', frame(rebuilt), tag);
  }

  function setThumbnailUrl(oldUrl, newUrl) {
    if (!oldUrl || !newUrl) {
      console.log('BF6CAPTURE setThumbnailUrl refused: no old or new thumbnail url');
      send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, thumbSet: 'refused:no old or new thumbnail url' });
      return false;
    }
    return pushStrings([[oldUrl, newUrl]], 'set');
  }

  // ---- WATCH THE SITE: the script editor -----------------------------------
  //
  // The site's Script page is Monaco, and Monaco keeps its text in a model that
  // announces every change. Attaching to that is the whole watcher: no polling,
  // no DOM reading, and no writing of any kind.
  //
  // Cost: one onDidChangeContent subscription, throttled to one report every
  // three seconds, and the text only crosses when it has actually changed.
  //
  // What it cannot see: anything while the panel is not on the Script page,
  // because the model does not exist until Monaco mounts; a second model if the
  // site ever opens two; and edits made in the tool's own editor, which are the
  // other direction and belong to the script module.
  var scriptWatch = null;      // {sub, timer, last, seen}

  function monacoModel() {
    try {
      if (!window.monaco || !window.monaco.editor) return null;
      var ms = window.monaco.editor.getModels();
      return (ms && ms.length) ? ms[0] : null;
    } catch (e) { return null; }
  }

  function scriptReport(text) {
    var id = 'sc' + (++seqId) + '-' + Date.now();
    var of = Math.ceil(text.length / CHUNK) || 1;
    for (var i = 0; i < of; i++) {
      send('capture', {
        v: V, method: 'siteScript', status: 200, url: location.href,
        text: true, id: id, seq: i, of: of, bodyB64: text.substr(i * CHUNK, CHUNK)
      });
    }
    console.log('BF6CAPTURE watch: the site script changed, ' + text.length + ' characters');
  }

  function startScriptWatch() {
    if (scriptWatch) return true;
    var m = monacoModel();
    if (!m) {
      // Not on the Script page. Tried again on the next probe.
      return false;
    }
    scriptWatch = { sub: null, timer: null, last: '', seen: 0 };
    try {
      scriptWatch.last = m.getValue();
      scriptWatch.sub = m.onDidChangeContent(function () {
        if (scriptWatch.timer) return;
        scriptWatch.timer = setTimeout(function () {
          scriptWatch.timer = null;
          var mm = monacoModel();
          if (!mm) return;
          var t = '';
          try { t = mm.getValue(); } catch (e) { return; }
          if (t === scriptWatch.last) return;
          scriptWatch.last = t;
          scriptWatch.seen++;
          scriptReport(t);
        }, 3000);
      });
    } catch (e) {
      console.log('BF6CAPTURE script watch could not attach: ' + e);
      scriptWatch = null;
      return false;
    }
    console.log('BF6CAPTURE watch: attached to the site script editor');
    send('pagestate', {
      v: V, kind: pageKind(location.pathname), url: location.href, watchState: 'script:attached'
    });
    return true;
  }

  function stopScriptWatch() {
    if (!scriptWatch) return true;
    try { if (scriptWatch.sub && scriptWatch.sub.dispose) scriptWatch.sub.dispose(); } catch (e) {}
    if (scriptWatch.timer) clearTimeout(scriptWatch.timer);
    scriptWatch = null;
    send('pagestate', {
      v: V, kind: pageKind(location.pathname), url: location.href, watchState: 'script:detached'
    });
    return true;
  }

  var watchWanted = false;
  function setWatch(on) {
    watchWanted = !!on;
    if (!watchWanted) return stopScriptWatch();
    return startScriptWatch();
  }

  // ---- the page probe -----------------------------------------------------

  function pageKind(p) {
    if (p.indexOf('/bf6/login') === 0) return 'login';
    if (p.indexOf('/bf6/experiences') === 0) return 'experiences';
    if (p.indexOf('/bf6/experience/') === 0) {
      return p.indexOf('/bf6/experience/rules/blocks') === 0 ? 'blocks' : 'editor';
    }
    return 'other';
  }

  // The account's display name AS THE PAGE SHOWS IT. Never an id, never an
  // address: a candidate carrying "@" is dropped rather than reported.
  function accountName() {
    var sel = ['[data-testid*="account-name"]', '[data-testid*="user-name"]', '[data-testid*="username"]',
               'header [class*="username"]', 'header [class*="userName"]', '[class*="AccountName"]'];
    for (var i = 0; i < sel.length; i++) {
      try {
        var e = document.querySelector(sel[i]);
        if (!e) continue;
        var t = (e.textContent || '').replace(/^\s+|\s+$/g, '');
        if (t && t.length <= 64 && t.indexOf('@') < 0) return t;
      } catch (ex) {}
    }
    return '';
  }

  // The rendered experience cards. The <img> the browser actually displays is
  // the RESOLVED thumbnail url, which is what the tool wants: the raw field can
  // be a "[BB_PREFIX]/..." placeholder only the site knows how to expand.
  function thumbs() {
    var out = [], seen = {};
    try {
      var links = document.querySelectorAll('a[href*="/bf6/experience"]');
      for (var i = 0; i < links.length && out.length < 200; i++) {
        var a = links[i];
        var m = (a.getAttribute('href') || '').match(/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/);
        if (!m) continue;
        var id = m[0].toLowerCase();
        if (seen[id]) continue;
        var card = null;
        try { card = a.closest ? (a.closest('article') || a.closest('li') || a.parentElement || a) : a; } catch (e) { card = a; }
        var img = null;
        try { img = (card && card.querySelector) ? card.querySelector('img') : null; } catch (e) {}
        if (!img) { try { img = a.querySelector ? a.querySelector('img') : null; } catch (e) {} }
        var src = img ? (img.currentSrc || img.src || '') : '';
        var name = '';
        try { name = (a.getAttribute('aria-label') || (img && img.getAttribute('alt')) || a.textContent || '').replace(/^\s+|\s+$/g, '').slice(0, 120); } catch (e) {}
        if (!src && !name) continue;
        seen[id] = 1;
        out.push({ id: id, name: name, src: src });
      }
    } catch (e) {}
    return out;
  }

  // The site's own "your session expired" dialog, if it has one. Matched on
  // MEANING rather than on a class name, because a class name is the first
  // thing a redesign changes: a dialog-role element whose text says both
  // "session" and one of expired / sign in / log in. The selector that matched
  // is reported so it can be read out of the log and hard-anchored later.
  function expiredDialog() {
    var sels = ['[role="alertdialog"]', '[role="dialog"]', 'dialog[open]', '[aria-modal="true"]'];
    for (var i = 0; i < sels.length; i++) {
      var list;
      try { list = document.querySelectorAll(sels[i]); } catch (e) { continue; }
      for (var j = 0; j < list.length; j++) {
        var t = '';
        try { t = (list[j].innerText || list[j].textContent || '').toLowerCase(); } catch (e) {}
        if (!t || t.length > 4000) continue;
        if (t.indexOf('session') < 0) continue;
        if (t.indexOf('expired') < 0 && t.indexOf('sign in') < 0 && t.indexOf('log in') < 0 && t.indexOf('signed out') < 0) continue;
        return sels[i] + ' [' + t.replace(/\s+/g, ' ').slice(0, 120) + ']';
      }
    }
    return '';
  }

  // IS THE SIGNED-IN LIST ON THE PAGE?
  //
  // The experiences page renders "MY EXPERIENCES (37/64)" and the owned tiles
  // only for a signed-in account looking at its own list. A signed-out visitor
  // gets the marketing page or a redirect to login, and never this.
  //
  // It exists because the request the tool would rather rely on cannot always
  // be seen: the site takes its reference to fetch and XHR before anything we
  // inject can run, so on a page the tool merely LOADS, every call goes past
  // unseen. This is the page saying, in its own rendering, that the session is
  // good. It is weaker evidence than a parsed response and it is used only
  // where that is unavailable.
  function ownedListShown() {
    try {
      // textContent, NOT innerText. innerText reports only what the browser
      // actually RENDERS, and this page runs clipped to a single pixel, so
      // innerText comes back nearly empty however well the site is doing.
      // That is what made the check report "signed-in list not present" 107
      // times in a row on a page that was signed in and finished.
      var t = (document.body ? document.body.textContent : "") || "";
      if (/MY\s+EXPERIENCES/i.test(t)) return true;
      if (/UNPUBLISHED\s*\(\s*\d+\s*\)/i.test(t) && /PUBLISHED\s*\(\s*\d+\s*\)/i.test(t)) return true;
      // A structural signal that needs no layout and no text at all: links to
      // the user's own experiences only exist on a signed-in list.
      try {
        if (document.querySelectorAll('a[href*="/bf6/experience/"]').length > 0) return true;
      } catch (e2) {}
      return false;
    } catch (e) { return false; }
  }

  function report() {
    send('pagestate', {
      v: V,
      kind: pageKind(location.pathname),
      url: location.href,
      hasBlockly: !!window._Blockly,
      signedInHint: accountName(),
      ownedListShown: ownedListShown(),
      expiredDialog: expiredDialog(),
      imageOptions: imageOptions(),
      api: apiMethods(),
      thumbs: thumbs()
    });
  }

  // The heartbeat. Same message without the card scrape, so the tool can tell
  // "the page is fine and just quiet" from "the page stopped answering", which
  // is one of the ways a sign-out shows up.
  // Monaco mounts after the page does, so the watch keeps trying while it is
  // wanted and not yet attached. One cheap check per probe, no polling loop.
  function watchTick() { if (watchWanted && !scriptWatch) startScriptWatch(); }

  function beat() {
    send('pagestate', {
      v: V,
      kind: pageKind(location.pathname),
      url: location.href,
      hasBlockly: !!window._Blockly,
      expiredDialog: expiredDialog(),
      api: apiMethods(),
      watching: watchWanted ? (scriptWatch ? 'script' : 'waiting for the script page') : '',
      beat: true
    });
  }

  // KEEP-ALIVE. A harmless request that costs the site nothing and tells it the
  // user is still here. The site's own idle reset is used when the page exposes
  // one; otherwise it is a no-store GET of the page the user is already on.
  // No headers of ours, no body, nothing read but the status.
  function keepAlive() {
    var names = ['__portalResetIdle', 'resetIdleTimer', '__resetIdle'];
    for (var i = 0; i < names.length; i++) {
      try {
        if (typeof window[names[i]] === 'function') {
          window[names[i]]();
          send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, keepAlive: 'site:' + names[i] });
          return true;
        }
      } catch (e) {}
    }
    try {
      realFetch(location.href, { method: 'GET', credentials: 'include', cache: 'no-store' })
        .then(function (res) {
          send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, keepAlive: 'fetch:' + res.status });
        }, function () {
          send('pagestate', { v: V, kind: pageKind(location.pathname), url: location.href, keepAlive: 'fetch:failed' });
        });
    } catch (e) {
      return false;
    }
    return true;
  }

  var reportTimer = null;
  function reportSoon() {
    if (reportTimer) clearTimeout(reportTimer);
    reportTimer = setTimeout(function () { reportTimer = null; report(); }, 400);
  }

  // The site is a single-page app: the URL moves without a load, and the cards
  // arrive after the data does. Watch both.
  var lastSeen = '';
  setInterval(function () {
    if (location.href !== lastSeen) { lastSeen = location.href; reportSoon(); }
  }, 500);
  setTimeout(report, 100);
  setTimeout(report, 900);
  setTimeout(report, 2500);
  setTimeout(report, 6000);
  // Every five seconds, so twenty seconds of silence is unambiguous.
  //
  // A TIMER IN A HIDDEN PAGE IS NOT A TIMER. This panel works offscreen, which
  // is the state browsers throttle hardest: a five second interval becomes one
  // a minute, and after a few minutes hidden it can be slower again. The tool
  // read that silence as a sign-out and threw the user back to the login page
  // every three minutes or so while the session was perfectly good.
  //
  // A worker's timer is not throttled that way, so the beat is driven from one
  // when the browser has workers, and by the ordinary interval when it does
  // not. Both call the same function; whichever fires first wins and the other
  // is harmless.
  setInterval(function () { watchTick(); beat(); }, 5000);
  try {
    var tickSrc = 'setInterval(function(){postMessage(1)},5000)';
    var tickUrl = URL.createObjectURL(new Blob([tickSrc], { type: 'text/javascript' }));
    var ticker = new Worker(tickUrl);
    ticker.onmessage = function () { watchTick(); beat(); };
  } catch (e) {
    // No workers, or a policy that forbids blob workers. The interval above is
    // still there, and the tool asks directly when a page goes quiet.
  }
  // The page's own warnings, on the same cadence. reportNotices only sends when
  // the set has CHANGED, so a page sitting on three errors costs one message,
  // not one every five seconds.
  setInterval(function () { reportNotices(false); }, 5000);
  setTimeout(function () { reportNotices(true); }, 3000);

  window.BF6PortalCapture = {
    v: V,
    getPlayElement: getPlayElement,
    getOwnedList: getOwnedList,
    exportExperience: exportExperience,
    setWatch: setWatch,
    pushReady: pushReady,
    toolBegin: toolBegin,
    toolChunk: toolChunk,
    importExperience: importExperience,
    describeControls: describeControls,
    apiMethods: apiMethods,
    pushStrings: pushStrings,
    openExperience: openExperience,
    report: report,
    beat: beat,
    keepAlive: keepAlive,
    reportNotices: reportNotices,
    uploadThumbnail: uploadThumbnail,
    pollVerification: pollVerification,
    selectImage: selectImage,
    setThumbnailUrl: setThumbnailUrl,
    imageOptions: imageOptions,
    rearm: function () { lastSeen = ''; report(); }
  };
  console.log('BF6CAPTURE ready v' + V);
})();
