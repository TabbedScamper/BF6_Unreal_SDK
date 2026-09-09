// ============================================================================
// BF6 Script: the half that runs on portal.battlefield.com.
//
// Injected into EVERY page the panel loads, by the tool, after every
// navigation. It does three things and nothing else:
//
//   1. reports what page the panel is on, so the tool can enable or grey out
//      PUSH, PULL and SAVE ON PORTAL;
//   2. on the experience's Script page, reads the site editor's text out and
//      writes the tool's bundle in, when the tool asks;
//   3. presses the site's own Save button, when the tool asks.
//
// IT NEVER ACTS ON ITS OWN. Every write and every press starts with a click
// the user made in the tool. Nothing here reads a cookie, a token, a session
// id or a password, and nothing here talks to the site's API. The template's
// command line deploy needs an EA session id; this does not, and must not,
// go anywhere near that path.
//
// The site is somebody else's page and it changes. Every selector below is
// tried in order, widest last, and each attempt is logged with the selector
// that worked, so a break is one log line away from a fix.
// ============================================================================

(function () {
    'use strict';

    if (window.BF6ScriptSync && window.BF6ScriptSync.v === 1) {
        // Re-injected into a page that already has us: refresh the report and
        // leave the rest alone.
        window.BF6ScriptSync.report();
        return;
    }

    var TAG = 'BF6SCRIPTSYNC';
    function log(msg) { try { console.log(TAG + ': ' + msg); } catch (e) { } }

    function bridge() {
        return (window.ue && window.ue.bf6script) ? window.ue.bf6script : null;
    }
    function send(obj) {
        var b = bridge();
        if (!b) { log('no bridge yet, dropped ' + obj.op); return; }
        obj.src = 'site';
        try { b.call(JSON.stringify(obj)); } catch (e) { log('send failed: ' + e.message); }
    }

    // ------------------------------------------------------------------------
    // WHERE ARE WE
    // ------------------------------------------------------------------------
    var SCRIPT_PATH = /\/experience\/rules\/script/i;
    var UUID = /[?&]id=([0-9a-fA-F-]{36})/;

    function pageInfo() {
        var url = location.href;
        var m = url.match(UUID);
        return {
            url: url,
            onScriptPage: SCRIPT_PATH.test(location.pathname),
            experience: m ? m[1] : ''
        };
    }

    // ------------------------------------------------------------------------
    // THE SITE'S EDITOR
    //
    // The page is Monaco. window.monaco is usually present, but a build that
    // hides it behind a module scope is not impossible, so every read and
    // every write falls through to the textarea Monaco keeps for accessibility
    // (its own label is "Editor content") and then to a contenteditable.
    // ------------------------------------------------------------------------
    function monacoModels() {
        try {
            if (!window.monaco || !monaco.editor || !monaco.editor.getModels) return [];
            return monaco.editor.getModels() || [];
        } catch (e) { return []; }
    }

    function monacoEditors() {
        try {
            if (!window.monaco || !monaco.editor || !monaco.editor.getEditors) return [];
            return monaco.editor.getEditors() || [];
        } catch (e) { return []; }
    }

    // The model the user is looking at. Prefer the focused editor's model,
    // then a TypeScript model, then the biggest one.
    function activeModel() {
        var eds = monacoEditors();
        for (var i = 0; i < eds.length; i++) {
            try {
                if (eds[i].hasTextFocus && eds[i].hasTextFocus()) return eds[i].getModel();
            } catch (e) { }
        }
        for (i = 0; i < eds.length; i++) {
            try { var m = eds[i].getModel(); if (m) return m; } catch (e) { }
        }
        var models = monacoModels();
        var best = null;
        for (i = 0; i < models.length; i++) {
            try {
                var mm = models[i];
                var id = (mm.getLanguageId ? mm.getLanguageId() : '') || '';
                if (id === 'typescript' || id === 'javascript') return mm;
                if (!best || mm.getValueLength() > best.getValueLength()) best = mm;
            } catch (e) { }
        }
        return best;
    }

    var TEXTAREA_SELECTORS = [
        'textarea[aria-label="Editor content"]',
        'textarea.inputarea',
        '.monaco-editor textarea',
        'textarea'
    ];

    function findTextarea() {
        for (var i = 0; i < TEXTAREA_SELECTORS.length; i++) {
            var n = document.querySelector(TEXTAREA_SELECTORS[i]);
            if (n) return { node: n, selector: TEXTAREA_SELECTORS[i] };
        }
        return null;
    }

    // ------------------------------------------------------------------------
    // FILE TABS
    //
    // The Script page shows the attachments as tabs: a script tab such as
    // "blank.ts" and, when the experience has one, a strings tab. There is no
    // stable class to key off, so tabs are found by their LABEL and the
    // selector that matched is logged and reported.
    // ------------------------------------------------------------------------
    var TAB_SELECTORS = [
        '[role="tab"]',
        'button[class*="tab" i]',
        'div[class*="tab" i] button',
        '[class*="fileTab" i]',
        'nav button'
    ];

    function findTabs() {
        for (var s = 0; s < TAB_SELECTORS.length; s++) {
            var nodes = document.querySelectorAll(TAB_SELECTORS[s]);
            var out = [];
            for (var i = 0; i < nodes.length; i++) {
                var txt = (nodes[i].textContent || '').trim();
                if (!txt || txt.length > 60) continue;
                out.push({ node: nodes[i], label: txt, selector: TAB_SELECTORS[s] });
            }
            // A tab strip that carries a .ts or a .json is the one we want.
            for (i = 0; i < out.length; i++) {
                if (/\.(ts|json)$/i.test(out[i].label)) return out;
            }
        }
        return [];
    }

    function pickTab(tabs, re) {
        for (var i = 0; i < tabs.length; i++) if (re.test(tabs[i].label)) return tabs[i];
        return null;
    }

    function stringsTab(tabs) {
        return pickTab(tabs, /strings/i) || pickTab(tabs, /\.json$/i);
    }
    function scriptTab(tabs) {
        return pickTab(tabs, /\.ts$/i);
    }

    // ------------------------------------------------------------------------
    // READING AND WRITING
    // ------------------------------------------------------------------------
    function readEditor() {
        var m = activeModel();
        if (m) {
            try { return { ok: true, how: 'monaco model', text: m.getValue() }; } catch (e) { }
        }
        var ta = findTextarea();
        if (ta) {
            // Monaco's own textarea holds only what is near the cursor, so this
            // is a fallback that can come back short. Say so rather than hand
            // back a truncated file as if it were the whole thing.
            return {
                ok: true, how: 'textarea ' + ta.selector, partial: true,
                text: ta.node.value || ''
            };
        }
        return { ok: false, why: 'No editor found on this page. None of these matched: window.monaco, ' + TEXTAREA_SELECTORS.join(', ') };
    }

    function writeEditor(text) {
        var m = activeModel();
        if (m) {
            try {
                // Through the editor when we have one, so the site's own change
                // handlers fire and the Save button enables itself.
                var eds = monacoEditors();
                for (var i = 0; i < eds.length; i++) {
                    try {
                        if (eds[i].getModel && eds[i].getModel() === m && eds[i].executeEdits) {
                            eds[i].executeEdits('bf6', [{ range: m.getFullModelRange(), text: text }]);
                            eds[i].pushUndoStop && eds[i].pushUndoStop();
                            return { ok: true, how: 'monaco executeEdits' };
                        }
                    } catch (e) { }
                }
                m.setValue(text);
                return { ok: true, how: 'monaco setValue' };
            } catch (e) {
                log('monaco write failed: ' + e.message);
            }
        }
        var ta = findTextarea();
        if (ta) {
            try {
                var setter = Object.getOwnPropertyDescriptor(window.HTMLTextAreaElement.prototype, 'value').set;
                setter.call(ta.node, text);
                ta.node.dispatchEvent(new Event('input', { bubbles: true }));
                ta.node.dispatchEvent(new Event('change', { bubbles: true }));
                return { ok: true, how: 'textarea ' + ta.selector, partial: true };
            } catch (e) {
                return { ok: false, why: 'textarea write failed: ' + e.message };
            }
        }
        return { ok: false, why: 'No editor found to write into.' };
    }

    // ------------------------------------------------------------------------
    // THE SAVE BUTTON
    // ------------------------------------------------------------------------
    var SAVE_SELECTORS = [
        'button[data-testid*="save" i]',
        'button[aria-label*="save" i]',
        'button[title*="save" i]'
    ];

    function findSave() {
        for (var i = 0; i < SAVE_SELECTORS.length; i++) {
            var n = document.querySelector(SAVE_SELECTORS[i]);
            if (n && !n.disabled) return { node: n, selector: SAVE_SELECTORS[i] };
        }
        var btns = document.querySelectorAll('button');
        for (i = 0; i < btns.length; i++) {
            var t = (btns[i].textContent || '').trim();
            if (/^save\b/i.test(t) && t.length < 24) {
                return { node: btns[i], selector: 'button with the text "' + t + '"' };
            }
        }
        return null;
    }

    // What the page says after a save. Read once, a moment later, and handed
    // back as text: the tool reports it and never interprets it.
    function resultText() {
        var pick = ['[role="alert"]', '[class*="toast" i]', '[class*="notification" i]', '[class*="snackbar" i]'];
        for (var i = 0; i < pick.length; i++) {
            var n = document.querySelector(pick[i]);
            if (n && (n.textContent || '').trim()) return (n.textContent || '').trim().slice(0, 300);
        }
        return '';
    }

    // ------------------------------------------------------------------------
    // WHAT THE SITE SAID ABOUT THE SAVE.
    //
    // The site answers a save over gRPC-web, and a refusal is not an HTTP
    // error: the request comes back 200 with the real verdict in the TRAILER
    // frame. A frame is one flags byte, four big endian length bytes, then the
    // payload; the trailer is the frame with 0x80 set in its flags, and its
    // payload is plain text of the form "grpc-status:3\r\ngrpc-message:...".
    // Status 3 is INVALID_ARGUMENT, which is what a script the site will not
    // accept comes back as.
    //
    // We watch for it by wrapping fetch and XMLHttpRequest here, in our own
    // file. NOTHING IS READ BUT THE STATUS AND THE MESSAGE: no headers, no
    // request bodies, no cookies, and nothing is sent anywhere except the
    // tool's own log and panel.
    // ------------------------------------------------------------------------
    var lastVerdict = null;   // { status, message, url, at }

    function looksBase64(s) {
        return s.length > 8 && /^[A-Za-z0-9+/\r\n]+={0,2}\s*$/.test(s.slice(0, 512));
    }

    function toBytes(text) {
        // grpc-web-text is base64 over the wire; grpc-web is binary and comes
        // back through .text() as one byte per character.
        var raw = text;
        if (looksBase64(text)) {
            try { raw = atob(text.replace(/\s+/g, '')); } catch (e) { raw = text; }
        }
        var out = new Uint8Array(raw.length);
        for (var i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i) & 0xff;
        return out;
    }

    function readTrailer(text) {
        var b = toBytes(text);
        var i = 0;
        while (i + 5 <= b.length) {
            var flags = b[i];
            var len = (b[i + 1] << 24) | (b[i + 2] << 16) | (b[i + 3] << 8) | b[i + 4];
            var start = i + 5;
            if (len < 0 || start + len > b.length) break;
            if (flags & 0x80) {
                var s = '';
                for (var j = start; j < start + len; j++) s += String.fromCharCode(b[j]);
                return s;
            }
            i = start + len;
        }
        // No well formed trailer. The text itself sometimes carries the pair
        // anyway, so look for it rather than give up.
        return /grpc-status/i.test(text) ? text : '';
    }

    function noteResponseText(url, text) {
        if (!text) return;
        var trailer = readTrailer(text);
        if (!trailer) return;
        var st = trailer.match(/grpc-status\s*:\s*(\d+)/i);
        if (!st) return;
        var msg = trailer.match(/grpc-message\s*:\s*([^\r\n]*)/i);
        lastVerdict = {
            status: parseInt(st[1], 10),
            message: msg ? decodeURIComponent((msg[1] || '').replace(/\+/g, ' ')) : '',
            url: String(url).slice(0, 200),
            at: Date.now()
        };
        log('grpc-status ' + lastVerdict.status + ' ' + lastVerdict.message);
    }

    function isSaveCall(url) {
        var u = String(url || '');
        return /updatePlayElement|UpdatePlayElement|playElement/i.test(u);
    }

    (function hookNetwork() {
        try {
            var realFetch = window.fetch;
            if (realFetch && !realFetch.__bf6) {
                var wrapped = function (input, init) {
                    var url = (typeof input === 'string') ? input : (input && input.url) || '';
                    return realFetch.apply(this, arguments).then(function (res) {
                        if (isSaveCall(url)) {
                            try {
                                res.clone().text().then(function (t) { noteResponseText(url, t); },
                                    function () { });
                            } catch (e) { }
                        }
                        return res;
                    });
                };
                wrapped.__bf6 = true;
                window.fetch = wrapped;
            }
        } catch (e) { log('fetch hook failed: ' + e.message); }

        try {
            var open = XMLHttpRequest.prototype.open;
            if (open && !open.__bf6) {
                var newOpen = function (m, u) { this.__bf6url = u; return open.apply(this, arguments); };
                newOpen.__bf6 = true;
                XMLHttpRequest.prototype.open = newOpen;
                var send = XMLHttpRequest.prototype.send;
                XMLHttpRequest.prototype.send = function () {
                    var self = this;
                    this.addEventListener('load', function () {
                        if (!isSaveCall(self.__bf6url)) return;
                        try { noteResponseText(self.__bf6url, self.responseText || ''); } catch (e) { }
                    });
                    return send.apply(this, arguments);
                };
            }
        } catch (e) { log('xhr hook failed: ' + e.message); }
    })();

    // The site's own squiggles. Whatever owner it puts them under, they carry
    // a line and a message and that is what the tool needs.
    function siteMarkers() {
        var out = [];
        try {
            if (!window.monaco || !monaco.editor || !monaco.editor.getModelMarkers) return out;
            var all = monaco.editor.getModelMarkers({}) || [];
            for (var i = 0; i < all.length && i < 200; i++) {
                var m = all[i];
                out.push({
                    line: m.startLineNumber || 1,
                    column: m.startColumn || 1,
                    severity: m.severity || 8,
                    message: String(m.message || '').slice(0, 600),
                    owner: String(m.owner || '')
                });
            }
        } catch (e) { log('markers unreadable: ' + e.message); }
        return out;
    }

    var TOAST_SELECTORS = ['[role="alert"]', '[role="dialog"]', '[class*="toast" i]',
        '[class*="notification" i]', '[class*="snackbar" i]', '[class*="error" i]'];

    function siteToast() {
        for (var i = 0; i < TOAST_SELECTORS.length; i++) {
            var n = document.querySelector(TOAST_SELECTORS[i]);
            var t = n && (n.textContent || '').trim();
            if (t && t.length > 2) return { text: t.slice(0, 600), where: TOAST_SELECTORS[i], node: n };
        }
        return null;
    }

    // ------------------------------------------------------------------------
    // THE API THE TOOL CALLS
    // ------------------------------------------------------------------------
    var API = {
        v: 1,

        // Everything the site said about the last save, gathered for three
        // seconds because the toast and the trailer do not arrive together.
        verdict: function () {
            lastVerdict = null;
            var startedAt = Date.now();
            var tries = 0;
            var timer = setInterval(function () {
                tries++;
                var toast = siteToast();
                var markers = siteMarkers();
                var haveSomething = lastVerdict || toast || markers.length;
                if (!haveSomething && tries < 12) return;
                clearInterval(timer);
                send({
                    op: 'siteverdict',
                    grpcStatus: lastVerdict ? lastVerdict.status : -1,
                    grpcMessage: lastVerdict ? lastVerdict.message : '',
                    toast: toast ? toast.text : '',
                    where: (lastVerdict ? 'grpc trailer' : '') +
                        (toast ? (lastVerdict ? ', ' : '') + toast.where : '') +
                        (markers.length ? ', ' + markers.length + ' site markers' : ''),
                    markers: markers,
                    waitedMs: Date.now() - startedAt
                });
            }, 250);
        },

        // Called before a fresh push, so the user is never looking at the
        // site's complaint about the version they just replaced.
        clearVerdict: function () {
            lastVerdict = null;
            var cleared = 0, owners = {};
            try {
                var all = monaco.editor.getModelMarkers({}) || [];
                for (var i = 0; i < all.length; i++) owners[all[i].owner] = true;
                var models = monacoModels();
                for (var o in owners) {
                    for (var j = 0; j < models.length; j++) {
                        try { monaco.editor.setModelMarkers(models[j], o, []); cleared++; } catch (e) { }
                    }
                }
            } catch (e) { log('could not clear site markers: ' + e.message); }

            var closed = '';
            var pick = ['[role="dialog"] button[aria-label*="close" i]',
                '[role="dialog"] button[class*="close" i]',
                '[class*="toast" i] button', '[role="alert"] button'];
            for (var k = 0; k < pick.length; k++) {
                var n = document.querySelector(pick[k]);
                if (!n) continue;
                try { n.click(); closed = pick[k]; } catch (e) { }
                break;
            }
            log('cleared ' + cleared + ' marker set(s)' + (closed ? ', dismissed via ' + closed : ', nothing to dismiss'));
        },

        report: function () {
            var info = pageInfo();
            var tabs = findTabs();
            var st = stringsTab(tabs);
            var sc = scriptTab(tabs);
            send({
                op: 'sitestate',
                url: info.url,
                onScriptPage: info.onScriptPage,
                experience: info.experience,
                hasMonaco: !!activeModel(),
                monacoGlobal: !!window.monaco,
                models: monacoModels().length,
                tabs: tabs.map(function (t) { return t.label; }),
                tabSelector: tabs.length ? tabs[0].selector : '',
                scriptTab: sc ? sc.label : '',
                stringsTab: st ? st.label : '',
                hasSave: !!findSave()
            });
        },

        // Replace the site editor's content. reqId is echoed so the tool can
        // match the answer to the button the user pressed.
        push: function (reqId, script, strings) {
            var info = pageInfo();
            if (!info.onScriptPage) {
                send({ op: 'siteresult', reqId: reqId, ok: false, why: 'The panel is not on the experience\'s Script page.' });
                return;
            }
            // The site's complaint about the version we are replacing must not
            // outlive it, or the user is reading an error about code that is
            // no longer there.
            API.clearVerdict();

            var tabs = findTabs();
            var sc = scriptTab(tabs);
            if (sc) { try { sc.node.click(); } catch (e) { } }

            var r = writeEditor(script);
            if (!r.ok) {
                send({ op: 'siteresult', reqId: reqId, ok: false, why: r.why });
                return;
            }
            log('script written via ' + r.how);

            var st = stringsTab(tabs);
            var stringsWritten = false, stringsWhy = '';
            if (strings && st) {
                try {
                    st.node.click();
                    // The site swaps the model when the tab changes, so give it
                    // a moment and then write the second file.
                    var pending = strings;
                    setTimeout(function () {
                        var r2 = writeEditor(pending);
                        log('strings written via ' + (r2.how || r2.why));
                        try { if (sc) sc.node.click(); } catch (e) { }
                        send({
                            op: 'siteresult', reqId: reqId, ok: true,
                            how: r.how, partial: !!r.partial,
                            stringsTab: st.label, stringsWritten: !!r2.ok, stringsWhy: r2.why || '',
                            tabSelector: st.selector
                        });
                    }, 400);
                    return;
                } catch (e) { stringsWhy = e.message; }
            } else if (strings && !st) {
                stringsWhy = 'This page shows no separate strings tab. Tabs seen: ' +
                    (tabs.length ? tabs.map(function (t) { return t.label; }).join(', ') : 'none') + '.';
            }

            send({
                op: 'siteresult', reqId: reqId, ok: true, how: r.how, partial: !!r.partial,
                stringsTab: st ? st.label : '', stringsWritten: stringsWritten, stringsWhy: stringsWhy,
                tabSelector: tabs.length ? tabs[0].selector : ''
            });
        },

        // Read the site editor's content back out.
        pull: function (reqId) {
            var info = pageInfo();
            if (!info.onScriptPage) {
                send({ op: 'siteresult', reqId: reqId, ok: false, why: 'The panel is not on the experience\'s Script page.' });
                return;
            }
            var r = readEditor();
            if (!r.ok) { send({ op: 'siteresult', reqId: reqId, ok: false, why: r.why }); return; }
            var tabs = findTabs();
            var sc = scriptTab(tabs);
            send({
                op: 'siteresult', reqId: reqId, ok: true, how: r.how, partial: !!r.partial,
                fileName: sc ? sc.label : 'script.ts',
                content: r.text
            });
        },

        // Press the site's own Save. The user asked for it in the tool; the
        // press still goes through the site's own button and its own checks.
        save: function (reqId) {
            var b = findSave();
            if (!b) {
                send({ op: 'siteresult', reqId: reqId, ok: false, why: 'No Save button found on this page.' });
                return;
            }
            try { b.node.click(); } catch (e) {
                send({ op: 'siteresult', reqId: reqId, ok: false, why: 'The Save button refused the click: ' + e.message });
                return;
            }
            log('save pressed via ' + b.selector);
            // Start listening for the verdict at the same moment as the press,
            // so a fast refusal is not missed while we wait to report the
            // press itself.
            API.verdict();
            setTimeout(function () {
                send({
                    op: 'siteresult', reqId: reqId, ok: true,
                    how: 'clicked ' + b.selector,
                    text: resultText() || 'The site did not put a message on screen. The verdict follows separately.'
                });
            }, 1400);
        }
    };

    window.BF6ScriptSync = API;

    // The site is a single page app: it swaps sections without a load, so the
    // report has to follow the router as well as the navigation.
    var lastUrl = location.href;
    setInterval(function () {
        if (location.href === lastUrl) return;
        lastUrl = location.href;
        API.report();
    }, 500);

    // The Script page mounts its editor after the route resolves, so the first
    // report can be too early. Say hello now, and again once the editor is up.
    API.report();
    var tries = 0;
    var settle = setInterval(function () {
        tries++;
        if (activeModel() || tries > 40) { clearInterval(settle); API.report(); }
    }, 250);

    log('ready on ' + location.href);
})();
