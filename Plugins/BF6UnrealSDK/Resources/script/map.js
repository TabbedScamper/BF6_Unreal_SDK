// ============================================================================
// BF6 Script: HOW IT FITS TOGETHER.
//
// The panel behind that handle answers the one question the rest of the editor
// never answers: a mod is four or five files, and nothing on screen says which
// piece the game starts, what that piece reaches, and what is sitting there
// wired to nothing. A beginner opens src/index.ts, sees three imports and two
// subscribe calls, and has no way to find out what actually runs.
//
// So this file reads the open files, works out four things from the source
// itself, and draws them:
//
//   ENTRY POINTS   Events.X.subscribe(...) and exported On*/Ongoing* functions.
//                  Portal starts the mod at these and nowhere else.
//   FUNCTIONS      what the user declared, and which function calls which.
//   VALUES         module level variables, and who reads or changes each one.
//   NEVER RUNS     a function nothing calls and no event points at.
//
// TWO RULES GOVERN EVERY LINE BELOW.
//
//   1. NOTHING HERE IS INVENTED. Every word on screen is either a name taken
//      out of the user's own source, a count of edges we actually found, or a
//      sentence lifted from guide.js. Where the reading would have to guess,
//      it says what it could not work out instead. A structure map that
//      confidently describes behaviour it did not parse is worse than no map,
//      because it is believed.
//
//   2. IT WORKS IN BOTH MODES. The page runs with a real TypeScript worker
//      (worker mode) or with regexes only (index mode) depending on whether a
//      Blob worker could be constructed from file://. The graph is built by
//      scanning text, which needs neither. When the worker IS there we ask it
//      for the navigation tree afterwards and fold in any declaration our
//      patterns missed, then redraw. The panel therefore never waits on the
//      worker and never breaks without it; the header says which reading you
//      are looking at.
//
// Public entry points, all called from editor.js and nowhere else:
//
//   BF6ScriptMap.attach(ctx)       once, with the handles this file cannot
//                                  reach on its own (see ATTACH below)
//   BF6ScriptMap.draw()            rebuild from the open files and render
//   BF6ScriptMap.invalidate()      drop the cache; next draw() re-reads
//   BF6ScriptMap.buildGraph(srcs)  the parser on its own, no DOM, for testing
//   BF6ScriptMap.describe(node, g) the plain-language reading, no DOM
// ============================================================================

window.BF6ScriptMap = (function () {
    'use strict';

    // ---- limits -------------------------------------------------------------
    // A runaway file must not lock the window. These exist only so a pasted
    // bundle cannot hang the panel.
    //
    // MAX_NODES WAS 400, AND THE COMMENT CLAIMED THAT WAS FAR ABOVE ANY REAL
    // PROJECT. IT IS BELOW ONE.
    //
    // Undead Ground Zero has 986 declarations across 33 files. The old cap was
    // used up by 370 module values in config.ts and game-state.ts before the
    // scan ever reached src/systems, so 3 of 33 files were mapped and the panel
    // announced 0 starting points for a mod that has 21. Not a truncated
    // answer, a confidently wrong one, which is the worst thing this panel can
    // do: someone who cannot read the source has no way to tell.
    //
    // 5000 covers that project with room to spare and builds in about 760 ms.
    var MAX_FILES = 60;
    var MAX_TEXT = 400000;
    var MAX_NODES = 5000;
    var MAX_LAYERS = 14;

    // ---- geometry -----------------------------------------------------------
    var COL = 168, NW = 148, NH = 26, ROW = 34, HEAD = 18, PAD = 8, GAP = 12;

    var ctx = null;          // handed over by editor.js at attach time
    var graph = null;        // the last graph built, or null when stale
    var selected = null;     // id of the node whose reading is on screen
    var pollTimer = null;
    var lastSig = '';
    var decorations = [];    // our own reveal highlight, so we can clear it

    // ========================================================================
    // SMALL HELPERS
    // ========================================================================
    function $(id) { return document.getElementById(id); }
    function el(tag, cls, text) {
        var n = document.createElement(tag);
        if (cls) n.className = cls;
        if (text != null) n.textContent = text;
        return n;
    }
    function esc(s) {
        return String(s).replace(/[&<>"]/g, function (c) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
        });
    }
    function reEsc(s) { return String(s).replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

    // "A", "A and B", "A, B and C". Written out because a list joined with
    // commas alone reads as a fragment to somebody who is already lost.
    function joinList(a) {
        if (!a.length) return '';
        if (a.length === 1) return a[0];
        if (a.length === 2) return a[0] + ' and ' + a[1];
        return a.slice(0, -1).join(', ') + ' and ' + a[a.length - 1];
    }

    function short(s, n) {
        s = String(s);
        return s.length <= n ? s : s.slice(0, n - 1) + '…';
    }

    // OnPlayerDied -> "player died". This is the identifier re-spelled, not an
    // explanation of the event: the map has no source for what an event means
    // beyond guide.js, so anything it does not find there is only ever the
    // name in ordinary words, and it is presented that way on screen.
    function humanName(name) {
        var s = String(name).replace(/^Ongoing/, '').replace(/^On/, '');
        var parts = s.match(/[A-Z][a-z]+|[A-Z]+(?![a-z])|[a-z]+|\d+/g);
        if (!parts || !parts.length) return '';
        return parts.join(' ').toLowerCase();
    }

    // ========================================================================
    // READING THE SOURCE
    //
    // Everything below works on a BLANKED copy of the file: same length, same
    // offsets, same line numbers, but every comment and every string body
    // replaced by spaces. Without that step a call named in a comment, or a
    // brace inside a message string, would be counted as real code, and the
    // one thing this panel must not do is report structure that is not there.
    // ========================================================================

    function blankOut(text) {
        var out = new Array(text.length);
        var i = 0, n = text.length;
        // prev is the last character that could end an expression. It is how a
        // regex literal is told from a division: /x/ after `(` or `=` is a
        // regex, / after an identifier or `)` is a divide. Getting this wrong
        // is not cosmetic. A literal such as /["']/ would otherwise open a
        // string that never closes and blank out the whole rest of the file.
        var prev = '';
        while (i < n) {
            var c = text[i];
            if (c === '/' && text[i + 1] === '/') {
                while (i < n && text[i] !== '\n') { out[i] = ' '; i++; }
                continue;
            }
            if (c === '/' && text[i + 1] === '*') {
                out[i] = ' '; out[i + 1] = ' '; i += 2;
                while (i < n && !(text[i] === '*' && text[i + 1] === '/')) {
                    out[i] = text[i] === '\n' ? '\n' : ' '; i++;
                }
                if (i < n) { out[i] = ' '; out[i + 1] = ' '; i += 2; }
                continue;
            }
            if (c === '"' || c === "'" || c === '`') {
                var q = c;
                out[i] = q; i++;
                while (i < n) {
                    if (text[i] === '\\') { out[i] = ' '; out[i + 1] = ' '; i += 2; continue; }
                    if (text[i] === q) { out[i] = q; i++; break; }
                    out[i] = text[i] === '\n' ? '\n' : ' ';
                    i++;
                }
                prev = 'x';
                continue;
            }
            if (c === '/' && !/[\w$)\]]/.test(prev)) {
                // A regex literal. Skip its body so the quotes inside it cannot
                // be mistaken for the start of a string.
                var j = i + 1, ok = false;
                while (j < n && text[j] !== '\n') {
                    if (text[j] === '\\') { j += 2; continue; }
                    if (text[j] === '[') { while (j < n && text[j] !== ']' && text[j] !== '\n') j++; }
                    if (text[j] === '/') { ok = true; break; }
                    j++;
                }
                if (ok) {
                    for (var k = i; k <= j; k++) out[k] = ' ';
                    i = j + 1;
                    prev = 'x';
                    continue;
                }
            }
            out[i] = c;
            if (!/\s/.test(c)) prev = c;
            i++;
        }
        return out.join('');
    }

    // Line starts, so an offset can become a line number without re-splitting
    // the file for every declaration we find.
    function lineIndex(text) {
        var starts = [0];
        for (var i = 0; i < text.length; i++) if (text[i] === '\n') starts.push(i + 1);
        return starts;
    }
    function lineAt(starts, off) {
        var lo = 0, hi = starts.length - 1, ans = 0;
        while (lo <= hi) {
            var mid = (lo + hi) >> 1;
            if (starts[mid] <= off) { ans = mid; lo = mid + 1; } else hi = mid - 1;
        }
        return ans + 1;
    }

    // Brace depth at any offset. Needed because only a declaration at depth 0
    // is module level, and a helper declared inside another function is not a
    // piece of the mod's structure, it is a detail of that one function.
    function braceIndex(blank) {
        var pos = [], dep = [], d = 0;
        for (var i = 0; i < blank.length; i++) {
            var c = blank[i];
            if (c === '{') { d++; pos.push(i); dep.push(d); }
            else if (c === '}') { d--; pos.push(i); dep.push(d); }
        }
        return { pos: pos, dep: dep };
    }
    function depthAt(bi, off) {
        var lo = 0, hi = bi.pos.length - 1, ans = -1;
        while (lo <= hi) {
            var mid = (lo + hi) >> 1;
            if (bi.pos[mid] < off) { ans = mid; lo = mid + 1; } else hi = mid - 1;
        }
        return ans === -1 ? 0 : bi.dep[ans];
    }

    function matchBrace(blank, at) {
        var d = 0;
        for (var i = at; i < blank.length; i++) {
            if (blank[i] === '{') d++;
            else if (blank[i] === '}') { d--; if (d === 0) return i + 1; }
        }
        return blank.length;
    }

    // The stretch of code that belongs to a declaration starting at `from`.
    // Beginners write all four shapes and all four have to be handled, or the
    // call edges out of half the project are simply missing:
    //   function f(a) { ... }        block after the parameter list
    //   class C { ... }              block after the name
    //   const f = () => { ... }      block after the arrow
    //   const f = (a) => a + 1       no block at all, ends with the statement
    function spanFrom(blank, from) {
        var d = 0;
        for (var i = from; i < blank.length; i++) {
            var c = blank[i];
            if (c === '(' || c === '[') d++;
            else if (c === ')' || c === ']') d--;
            else if (c === '{' && d <= 0) return [from, matchBrace(blank, i)];
            else if (c === ';' && d <= 0 && i > from) return [from, i];
            else if (c === '\n' && d <= 0 && i > from) {
                // A line that ends mid expression carries on. Anything else is
                // the end of this declaration, and swallowing the next one
                // would invent calls that the user never wrote.
                var t = blank.slice(from, i).replace(/\s+$/, '');
                var last = t.charAt(t.length - 1);
                if ('=,(+-*/&|?:<>'.indexOf(last) !== -1) continue;
                return [from, i];
            }
        }
        return [from, blank.length];
    }

    // A variable whose value is a function is a function, however it was
    // written. This is the test, applied to whatever follows the `=`.
    var RE_IS_FN = /^\s*(?:async\s+)?(?:function\b|\(|<[^<>]*>\s*\(|[A-Za-z_$][\w$]*\s*=>)/;

    var RE_FUNC = /(?:^|[\n;}{])[ \t]*(export\s+)?(?:default\s+)?(?:async\s+)?function\s*\*?\s+([A-Za-z_$][\w$]*)/g;
    var RE_CLASS = /(?:^|[\n;}{])[ \t]*(export\s+)?(?:default\s+)?(?:abstract\s+)?class\s+([A-Za-z_$][\w$]*)/g;
    var RE_VAR = /(?:^|[\n;}{])[ \t]*(export\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)/g;
    var RE_SUB = /Events\s*\.\s*([A-Za-z_$][\w$]*)\s*\.\s*subscribe\s*\(/g;
    // The open bracket is looked at but not eaten. Portal code nests calls
    // constantly (ShowEventGameModeMessage(mod.Message(...))) and consuming the
    // bracket would put the scan past the inner call's own "mod.", so half the
    // commands in a line like that were never reported.
    var RE_MOD = /(?:^|[^\w$.])mod\s*\.\s*([A-Za-z_$][\w$]*)(?=\s*\()/g;

    function scanFile(rel, text) {
        if (text.length > MAX_TEXT) text = text.slice(0, MAX_TEXT);
        var blank = blankOut(text);
        var starts = lineIndex(blank);
        var bi = braceIndex(blank);
        var found = [];
        var seen = {};
        var m;

        function add(node) {
            if (seen[node.name] && node.kind !== 'handler') return;
            seen[node.name] = 1;
            found.push(node);
        }

        RE_FUNC.lastIndex = 0;
        while ((m = RE_FUNC.exec(blank)) !== null) {
            var at = m.index + m[0].indexOf(m[1] ? 'export' : 'function');
            if (depthAt(bi, at) !== 0) continue;
            var name = m[2];
            var sp = spanFrom(blank, at);
            // Portal calls exported On*/Ongoing* functions by name. They are
            // started by the game, not by the user's own code, which is what
            // makes them entry points even with nothing calling them.
            var isEvt = !!m[1] && /^(On|Ongoing)[A-Z]/.test(name);
            add({
                rel: rel, name: name, kind: isEvt ? 'handler' : 'function',
                exported: !!m[1], event: isEvt ? name : null, wiring: isEvt ? 'exported' : null,
                line: lineAt(starts, at), start: sp[0], end: sp[1]
            });
        }

        RE_CLASS.lastIndex = 0;
        while ((m = RE_CLASS.exec(blank)) !== null) {
            var cat = m.index + m[0].search(/(?:export|default|abstract|class)/);
            if (depthAt(bi, cat) !== 0) continue;
            var csp = spanFrom(blank, cat);
            add({
                rel: rel, name: m[2], kind: 'class', exported: !!m[1], event: null,
                line: lineAt(starts, cat), start: csp[0], end: csp[1]
            });
        }

        RE_VAR.lastIndex = 0;
        while ((m = RE_VAR.exec(blank)) !== null) {
            var vat = m.index + m[0].search(/(?:export|const|let|var)/);
            if (depthAt(bi, vat) !== 0) continue;
            var vsp = spanFrom(blank, vat);
            var eq = blank.indexOf('=', m.index + m[0].length);
            var init = (eq !== -1 && eq < vsp[1]) ? blank.slice(eq + 1, Math.min(eq + 80, vsp[1])) : '';
            var isFn = eq !== -1 && eq < vsp[1] && RE_IS_FN.test(init);
            add({
                rel: rel, name: m[2], kind: isFn ? 'function' : 'value',
                exported: !!m[1], event: null,
                line: lineAt(starts, vat), start: vsp[0], end: vsp[1]
            });
        }

        // ---- the subscriptions ---------------------------------------------
        // Three shapes turn up in real projects and they are not the same
        // thing, so the reading must not pretend they are:
        //   subscribe(onDeployed)      names a function we already found
        //   subscribe(() => { ... })   the code lives in the call itself
        //   subscribe(this.x.bind(y))  an expression we cannot follow
        var subs = [];
        RE_SUB.lastIndex = 0;
        while ((m = RE_SUB.exec(blank)) !== null) {
            var open = m.index + m[0].length - 1;
            var argSpan = spanFrom(blank, open);
            var close = blank.indexOf(')', argSpan[1] - 1);
            var arg = blank.slice(open + 1, close === -1 ? argSpan[1] : close);
            var ref = /^\s*([A-Za-z_$][\w$]*)\s*[,)]?\s*$/.exec(arg);
            subs.push({
                rel: rel, event: m[1], line: lineAt(starts, m.index),
                ref: ref ? ref[1] : null,
                inline: !ref && /^\s*(?:async\s+)?(?:function\b|\(|[A-Za-z_$][\w$]*\s*=>|<)/.test(arg),
                start: open, end: argSpan[1]
            });
        }

        return { rel: rel, text: text, blank: blank, starts: starts, decls: found, subs: subs };
    }

    // ========================================================================
    // THE GRAPH
    //
    // buildGraph takes { rel: text } and gives back nodes, edges and files.
    // No DOM, no editor, no worker: everything the panel shows comes out of
    // here, which is also what makes it possible to check by hand.
    // ========================================================================
    function buildGraph(sources) {
        var rels = Object.keys(sources).filter(function (rel) {
            // A generated bundle is not the user's structure, it is the same
            // structure flattened, and showing both would double every node.
            if (/^dist\//.test(rel)) return false;
            if (/node_modules/.test(rel)) return false;
            if (/\.d\.ts$/.test(rel)) return false;
            return /\.(ts|tsx|js|mjs)$/i.test(rel);
        }).sort(function (a, b) {
            // The file the game starts in goes first, then the rest by path.
            var ai = a === 'src/index.ts' ? 0 : 1, bi2 = b === 'src/index.ts' ? 0 : 1;
            return ai - bi2 || (a < b ? -1 : a > b ? 1 : 0);
        }).slice(0, MAX_FILES);

        var scans = rels.map(function (rel) { return scanFile(rel, sources[rel] || ''); });

        var nodes = [];
        var byId = {};
        var byName = {};      // last declaration wins for a cross file call
        /* Set when a declaration was dropped for want of room. It has to reach
         * the header: a graph that quietly stops early reads as a complete map
         * of a smaller mod. */
        var capped = false;

        function nid(rel, name, line) { return rel + '#' + name + (line ? '#' + line : ''); }

        scans.forEach(function (s) {
            s.decls.forEach(function (d) {
                if (nodes.length >= MAX_NODES) { capped = true; return; }
                var node = {
                    id: nid(d.rel, d.name), rel: d.rel, name: d.name, kind: d.kind,
                    exported: d.exported, event: d.event, wiring: d.wiring || null,
                    line: d.line, start: d.start, end: d.end,
                    calls: [], calledBy: [], reads: [], writes: [], readBy: [], writtenBy: [],
                    mod: [], subscribes: [], layer: 0, dead: false, from: 'pattern'
                };
                nodes.push(node);
                byId[node.id] = node;
                if (!byName[node.name]) byName[node.name] = node;
            });
        });

        // Wire the subscriptions. A subscribe that names a function turns that
        // function into an entry point in place; the alternative would be a
        // second box for the same lines of code.
        scans.forEach(function (s) {
            s.subs.forEach(function (sub) {
                var target = sub.ref ? (byId[nid(sub.rel, sub.ref)] || byName[sub.ref]) : null;
                if (target) {
                    target.kind = 'handler';
                    target.event = target.event || sub.event;
                    target.wiring = 'subscribe';
                    target.subLine = sub.line;
                    target.subRel = sub.rel;
                    return;
                }
                if (nodes.length >= MAX_NODES) { capped = true; return; }
                var node = {
                    id: nid(sub.rel, sub.event, sub.line), rel: sub.rel, name: sub.event,
                    kind: 'handler', exported: false, event: sub.event,
                    wiring: sub.inline ? 'inline' : 'expression',
                    line: sub.line, start: sub.start, end: sub.end,
                    calls: [], calledBy: [], reads: [], writes: [], readBy: [], writtenBy: [],
                    mod: [], subscribes: [], layer: 0, dead: false, from: 'pattern'
                };
                nodes.push(node);
                byId[node.id] = node;
            });
        });

        var scanByRel = {};
        scans.forEach(function (s) { scanByRel[s.rel] = s; });

        // ---- edges ----------------------------------------------------------
        // Everything is read out of the blanked body of each node, so a name
        // that only appears in a comment or a message string never becomes an
        // edge. `[^\w$.]` in front of the name is what keeps other.reset() from
        // being counted as a call to the user's own reset.
        var callable = nodes.filter(function (n) {
            return n.kind === 'function' || n.kind === 'class' || n.kind === 'handler';
        });
        var values = nodes.filter(function (n) { return n.kind === 'value'; });

        nodes.forEach(function (n) {
            var s = scanByRel[n.rel];
            if (!s) return;
            var body = s.blank.slice(n.start, n.end);
            if (!body) return;

            callable.forEach(function (t) {
                if (t === n) return;
                var re = new RegExp('(?:^|[^\\w$.])' + reEsc(t.name) + '(?=\\s*\\()', 'g');
                if (!re.test(body)) return;
                if (n.calls.indexOf(t.id) === -1) n.calls.push(t.id);
                if (t.calledBy.indexOf(n.id) === -1) t.calledBy.push(n.id);
            });

            values.forEach(function (v) {
                if (v === n) return;
                var re = new RegExp('(?:^|[^\\w$.])' + reEsc(v.name) + '(?![\\w$])', 'g');
                var mm, read = false, wrote = false;
                while ((mm = re.exec(body)) !== null) {
                    var at = mm.index + mm[0].length;
                    var after = body.slice(at, at + 48);
                    // "delete playersStats[id]" changes the value, and the
                    // giveaway is in front of the name rather than after it,
                    // which is why this one case looks backwards.
                    var before = body.slice(Math.max(0, at - v.name.length - 10), at - v.name.length);
                    if (isWrite(after) || /\bdelete\s*$/.test(before)) wrote = true; else read = true;
                    if (read && wrote) break;
                }
                if (wrote) {
                    if (n.writes.indexOf(v.id) === -1) n.writes.push(v.id);
                    if (v.writtenBy.indexOf(n.id) === -1) v.writtenBy.push(n.id);
                }
                if (read) {
                    if (n.reads.indexOf(v.id) === -1) n.reads.push(v.id);
                    if (v.readBy.indexOf(n.id) === -1) v.readBy.push(n.id);
                }
            });

            var mm2;
            RE_MOD.lastIndex = 0;
            while ((mm2 = RE_MOD.exec(body)) !== null) {
                if (n.mod.indexOf(mm2[1]) === -1 && n.mod.length < 40) n.mod.push(mm2[1]);
            }
            RE_SUB.lastIndex = 0;
            var mm3;
            while ((mm3 = RE_SUB.exec(body)) !== null) {
                if (n.subscribes.indexOf(mm3[1]) === -1) n.subscribes.push(mm3[1]);
            }
        });

        layerAndFlag(nodes, byId);

        return {
            files: rels, nodes: nodes, byId: byId,
            source: 'pattern',
            capped: capped,
            counts: countKinds(nodes)
        };
    }

    // A name followed by any of these is being changed, not merely read. The
    // negative lookahead on `=` is what keeps `==`, `===` and `=>` out of it;
    // without that every comparison would be reported as a write and the
    // reading would tell people their code changes things it only checks.
    function isWrite(after) {
        return /^\s*(?:\+\+|--)/.test(after)
            || /^\s*(?:\*\*|<<|>>>?|\|\||&&|\?\?|[-+*/%&|^])?=(?!=|>)/.test(after)
            || /^\s*\[[^\]]*\]\s*(?:[-+*/%&|^]?)=(?!=|>)/.test(after)
            || /^\s*\.\s*[\w$]+\s*(?:[-+*/%&|^]?)=(?!=|>)/.test(after)
            || /^\s*\.\s*(?:push|pop|shift|unshift|splice|set|add|delete|clear|sort|reverse|fill|length\s*=)\s*\(?/.test(after);
    }

    function countKinds(nodes) {
        var c = { entry: 0, fn: 0, value: 0, dead: 0 };
        nodes.forEach(function (n) {
            if (n.kind === 'handler') c.entry++;
            else if (n.kind === 'value') c.value++;
            else c.fn++;
            if (n.dead) c.dead++;
        });
        return c;
    }

    // Columns, left to right: what the game starts, then what that reaches,
    // then what that reaches. A function nobody calls has no column of its own
    // to sit in, so it stays in the first one and is flagged instead: it is a
    // starting point in shape, just not one the game will ever take.
    function layerAndFlag(nodes, byId) {
        nodes.forEach(function (n) {
            n.layer = 0;
            n.dead = (n.kind === 'function' || n.kind === 'class') && n.calledBy.length === 0;
        });
        var rounds = Math.min(nodes.length, MAX_LAYERS);
        for (var r = 0; r < rounds; r++) {
            var moved = false;
            nodes.forEach(function (n) {
                if (n.kind === 'value') return;
                n.calls.forEach(function (id) {
                    var t = byId[id];
                    if (!t || t.kind === 'value') return;
                    if (t.layer < n.layer + 1 && t.layer < MAX_LAYERS) {
                        t.layer = Math.min(n.layer + 1, MAX_LAYERS);
                        moved = true;
                    }
                });
            });
            if (!moved) break;
        }
        // A flagged function must not be pushed right by a caller that is
        // itself unreachable, or the column stops meaning "the game gets here".
        var maxL = 0;
        nodes.forEach(function (n) { if (n.kind !== 'value' && n.layer > maxL) maxL = n.layer; });
        nodes.forEach(function (n) { if (n.kind === 'value') n.layer = maxL + 1; });
    }

    // ========================================================================
    // WORKER MODE: fold in what the language service knows
    //
    // The graph above is already complete enough to draw. When the TypeScript
    // worker did start, its navigation tree is the compiler's own list of what
    // this file declares, so anything in there that our patterns did not find
    // is a real declaration we would otherwise have shown as missing: a
    // declaration written across several lines, a generic arrow, an overload.
    // We add those and redraw. We do NOT let it delete nodes: a disagreement
    // between the two readings is a reason to show more, never less.
    // ========================================================================
    var NAV_KIND = {
        'function': 'function', 'method': 'function', 'local function': 'function',
        'class': 'class', 'var': 'value', 'let': 'value', 'const': 'value',
        'property': 'value', 'alias': null, 'interface': null, 'type': null,
        'enum': 'value', 'module': null, 'script': null
    };

    function refineWithWorker(g, sources, done) {
        var monaco = ctx && ctx.monaco && ctx.monaco();
        if (!monaco || !monaco.languages || !monaco.languages.typescript) { done(false); return; }
        var getWorker = monaco.languages.typescript.getTypeScriptWorker;
        if (!getWorker) { done(false); return; }
        var fin = false;
        // The worker can simply never answer on a page with no worker at all.
        // A timer is the only honest way to give up, because there is no error
        // to catch when the promise is never settled.
        var bail = setTimeout(function () { if (!fin) { fin = true; done(false); } }, 4000);

        getWorker().then(function (getClient) {
            var models = [];
            g.files.forEach(function (rel) {
                var f = ctx.files()[rel];
                if (f && f.model) models.push({ rel: rel, uri: f.model.uri });
            });
            if (!models.length) { clearTimeout(bail); if (!fin) { fin = true; done(false); } return; }
            return getClient.apply(null, models.map(function (m) { return m.uri; }))
                .then(function (client) {
                    var jobs = models.map(function (m) {
                        return client.getNavigationTree(m.uri.toString()).then(function (tree) {
                            return { rel: m.rel, tree: tree };
                        }, function () { return null; });
                    });
                    return Promise.all(jobs).then(function (results) {
                        var added = 0;
                        results.forEach(function (r) {
                            if (!r || !r.tree) return;
                            added += foldNav(g, sources, r.rel, r.tree);
                        });
                        if (added) layerAndFlag(g.nodes, g.byId);
                        g.source = 'language service';
                        g.counts = countKinds(g.nodes);
                        clearTimeout(bail);
                        if (!fin) { fin = true; done(true); }
                    });
                });
        }).catch(function () {
            clearTimeout(bail);
            if (!fin) { fin = true; done(false); }
        });
    }

    function foldNav(g, sources, rel, tree) {
        var kids = (tree && tree.childItems) || [];
        var text = sources[rel] || '';
        var blank = blankOut(text);
        var starts = lineIndex(blank);
        var added = 0;
        kids.forEach(function (item) {
            var kind = NAV_KIND[item.kind];
            if (!kind) return;
            var name = String(item.text || '').replace(/^["'`]|["'`]$/g, '');
            if (!/^[A-Za-z_$][\w$]*$/.test(name)) return;
            var id = rel + '#' + name;
            if (g.byId[id]) { g.byId[id].from = 'language service'; return; }
            if (g.nodes.length >= MAX_NODES) { g.capped = true; return; }
            var span = (item.spans && item.spans[0]) || null;
            var off = span ? span.start : 0;
            var node = {
                id: id, rel: rel, name: name, kind: kind,
                exported: /export/.test(item.kindModifiers || ''), event: null, wiring: null,
                line: lineAt(starts, off), start: off, end: span ? off + span.length : off,
                calls: [], calledBy: [], reads: [], writes: [], readBy: [], writtenBy: [],
                mod: [], subscribes: [], layer: 0, dead: false, from: 'language service'
            };
            g.nodes.push(node);
            g.byId[id] = node;
            added++;
        });
        return added;
    }

    // ========================================================================
    // THE READING
    //
    // One node in, plain sentences out. Every sentence is built from the graph
    // or lifted from guide.js. Where there is nothing to say, it says that
    // rather than filling the space.
    // ========================================================================
    function describe(node, g) {
        var G = (ctx && ctx.guide) || { mod: {}, boilerplate: {} };
        var out = [];
        var nm = function (id) { var t = g.byId[id]; return t ? t.name : null; };
        var names = function (ids) { return ids.map(nm).filter(Boolean); };

        // ---- what starts it ------------------------------------------------
        if (node.kind === 'handler') {
            var human = humanName(node.event || node.name);
            var lead = 'The game runs this when ' + (node.event || node.name) + ' happens'
                + (human ? ' (' + human + ')' : '') + '.';
            if (node.wiring === 'exported') {
                lead += ' Portal starts it by name because it is exported, so nothing in your code has to call it.';
            } else if (node.wiring === 'subscribe') {
                lead += ' It is wired up by the Events.' + node.event + '.subscribe line'
                    + (node.subLine ? ' on line ' + node.subLine : '') + '.';
            } else if (node.wiring === 'inline') {
                lead += ' The code that runs is written inside the subscribe call itself.';
            } else if (node.wiring === 'expression') {
                lead += ' The subscribe call passes an expression rather than a plain name, so this map cannot say which function it runs.';
            }
            out.push({ label: 'Starts here', text: lead });

            var bp = G.boilerplate && G.boilerplate['Events.' + node.event + '.subscribe'];
            if (bp) out.push({ label: 'About this event', text: bp });

            var ps = ctx && ctx.eventParams && ctx.eventParams(node.event || node.name);
            if (ps && ps.length) {
                out.push({
                    label: 'What the game hands it',
                    text: ps.map(function (p) { return p.name + (p.type ? ' (' + p.type + ')' : ''); }).join(', ') + '.'
                });
            }
        } else if (node.kind === 'value') {
            out.push({
                label: 'A value kept between events',
                text: 'It sits at the top of ' + node.rel + ', outside every function, so whatever is put in it stays there after an event finishes.'
            });
        } else if (node.dead) {
            out.push({
                label: 'Nothing runs this',
                text: 'No other function in the files you have open calls ' + node.name
                    + ', and no event points at it. As things stand it never runs. Either call it from something that does, or subscribe an event to it.'
            });
        } else {
            var by = names(node.calledBy);
            out.push({
                label: 'What runs it',
                text: 'Called by ' + joinList(by) + '.'
            });
        }

        // ---- what it reaches ------------------------------------------------
        if (node.kind !== 'value') {
            var calls = names(node.calls);
            out.push({
                label: 'What it calls',
                text: calls.length
                    ? 'It calls ' + joinList(calls) + '.'
                    : 'It does not call any of the other functions in your files.'
            });

            var reads = names(node.reads), writes = names(node.writes);
            if (reads.length || writes.length) {
                var t = [];
                if (writes.length) t.push('It changes ' + joinList(writes) + '.');
                if (reads.length) t.push('It reads ' + joinList(reads) + '.');
                out.push({ label: 'What it uses', text: t.join(' ') });
            }

            if (node.subscribes.length) {
                out.push({
                    label: 'What it wires up',
                    text: 'It subscribes to ' + joinList(node.subscribes) + '.'
                });
            }

            if (node.mod.length) {
                var known = node.mod.filter(function (x) { return G.mod && G.mod[x] && G.mod[x].say; });
                out.push({
                    label: 'Game commands it uses',
                    text: joinList(node.mod.slice(0, 12).map(function (x) { return 'mod.' + x; }))
                        + (node.mod.length > 12 ? ', and others.' : '.')
                });
                known.slice(0, 4).forEach(function (x) {
                    out.push({ label: 'mod.' + x, text: G.mod[x].say, quiet: true });
                });
            }
        } else {
            var wb = names(node.writtenBy), rb = names(node.readBy);
            out.push({
                label: 'What changes it',
                text: wb.length ? joinList(wb) + '.' : 'Nothing in your files changes it after the line that creates it.'
            });
            out.push({
                label: 'What reads it',
                text: rb.length ? joinList(rb) + '.' : 'Nothing in your files reads it, so it is not being used yet.'
            });
        }

        out.push({ label: 'Where it lives', text: node.rel + ', line ' + node.line + '.', quiet: true });
        return out;
    }

    // ========================================================================
    // DRAWING
    // ========================================================================
    function sources() {
        var files = ctx.files();
        var out = {};
        Object.keys(files).forEach(function (rel) {
            var f = files[rel];
            if (f && f.model) { try { out[rel] = f.model.getValue(); } catch (e) { } }
        });
        return out;
    }

    // A cheap fingerprint of everything open. Comparing it on a timer is what
    // keeps a pinned panel from quietly showing yesterday's structure while
    // somebody edits beside it, without hooking this file into the editor's
    // own change handler.
    function signature() {
        var files = ctx.files();
        var s = '';
        Object.keys(files).sort().forEach(function (rel) {
            var f = files[rel];
            var v = 0;
            try { v = f.model.getVersionId(); } catch (e) { }
            s += rel + ':' + v + ';';
        });
        return s;
    }

    function build() {
        var src = sources();
        graph = buildGraph(src);
        lastSig = signature();
        if (ctx.mode() === 'worker') {
            // The worker answers a moment later. By then the user may have
            // typed and we may be on a newer graph already, so the refinement
            // of a graph that has been replaced is dropped rather than drawn
            // over the top of the current one.
            var mine = graph;
            refineWithWorker(mine, src, function (ok) {
                if (ok && graph === mine) render();
            });
        }
        return graph;
    }

    function draw() {
        if (!ctx) return;
        if (!graph) build();
        render();
        startPolling();
    }

    function invalidate() { graph = null; }

    function startPolling() {
        if (pollTimer) return;
        pollTimer = setInterval(function () {
            var pane = $('pane-map');
            if (!pane || !pane.classList.contains('on')) { clearInterval(pollTimer); pollTimer = null; return; }
            if (signature() === lastSig) return;
            build();
            render();
        }, 1500);
    }

    // ---- the graph itself ---------------------------------------------------
    function render() {
        var host = $('mapGraph');
        if (!host) return;
        var where = $('mapWhere');

        if (!graph || !graph.nodes.length) {
            host.innerHTML = '';
            host.appendChild(emptyCard());
            if (where) where.textContent = 'Nothing to map yet.';
            $('mapDetail').innerHTML = '';
            renderMissing();
            return;
        }

        var c = graph.counts;
        if (where) {
            where.innerHTML =
                '<b>' + c.entry + '</b> starting point' + (c.entry === 1 ? '' : 's') + ' &nbsp; ' +
                '<b>' + c.fn + '</b> function' + (c.fn === 1 ? '' : 's') + ' &nbsp; ' +
                '<b>' + c.value + '</b> value' + (c.value === 1 ? '' : 's') + ' &nbsp; ' +
                '<b>' + c.dead + '</b> never run' + (c.dead === 1 ? 's' : '') +
                (graph.capped
                    ? '<br><b>This map is incomplete.</b> The project has more than ' +
                      MAX_NODES + ' declarations, so some are not shown and the counts ' +
                      'above are lower than the real ones.'
                    : '') +
                '<br>' + (graph.source === 'language service'
                    ? 'Read with the language service.'
                    : 'Read by matching patterns in the text, which is what index mode has. It can miss an unusual declaration.');
        }

        renderLegend();
        host.innerHTML = svgFor(graph);
        host.onclick = function (ev) {
            var g = ev.target.closest ? ev.target.closest('g[data-id]') : null;
            if (!g) return;
            select(g.getAttribute('data-id'), true);
        };
        renderMissing();
        if (selected && graph.byId[selected]) renderDetail(graph.byId[selected]);
        else $('mapDetail').innerHTML = '';
    }

    function emptyCard() {
        var card = el('div', 'card');
        var open = ctx ? Object.keys(ctx.files()).length : 0;
        card.innerHTML = open
            ? '<div class="t">Nothing to draw</div><div>The files you have open declare no functions, no event handlers and no values kept between events.</div>'
            : '<div class="t">No files open</div><div>Open a project and a file, then come back. This panel maps whatever is open.</div>';
        return card;
    }

    function renderLegend() {
        var host = $('mapLegend');
        if (!host) return;
        host.innerHTML =
            '<span class="pill acc">Starts here</span>' +
            '<span class="pill">Your function</span>' +
            '<span class="pill warn">Never runs</span>' +
            '<span class="pill">Value kept</span>';
    }

    // Only what is open can be read, and a map that silently leaves out half
    // the project is exactly the wrong thing to hand somebody who is already
    // lost. So the files it did not read are named, with the button that fixes
    // it, one file at a time so nothing jumps under the user.
    function renderMissing() {
        var host = $('mapMissing');
        if (!host) return;
        host.innerHTML = '';
        if (!ctx.projectFiles) return;
        var open = ctx.files();
        var missing = ctx.projectFiles().filter(function (rel) {
            if (open[rel]) return false;
            if (/^dist\//.test(rel) || /node_modules/.test(rel) || /\.d\.ts$/.test(rel)) return false;
            return /\.(ts|tsx|js|mjs)$/i.test(rel);
        });
        if (!missing.length) return;
        host.appendChild(headEl('Not on the map yet',
            missing.length + ' file' + (missing.length === 1 ? '' : 's') + ' in this project '
            + (missing.length === 1 ? 'is' : 'are') + ' not open, so nothing in '
            + (missing.length === 1 ? 'it' : 'them') + ' could be read. Open one to add it.'));
        missing.slice(0, 20).forEach(function (rel) {
            var b = el('button', 'row');
            b.innerHTML = '<span class="t">' + esc(rel.replace(/^.*\//, '')) + '</span>' +
                '<span class="d">' + esc(rel) + '</span>';
            b.onclick = function () { ctx.readFile(rel); invalidate(); };
            host.appendChild(b);
        });
    }

    function headEl(title, sub) {
        var w = el('div');
        w.appendChild(el('div', 'h', title));
        if (sub) w.appendChild(el('div', 'sub', sub));
        return w;
    }

    // ---- layout -------------------------------------------------------------
    // One labelled container per file, stacked. Inside a container the column
    // is the node's distance from a starting point, which is why a function
    // called from another file still lines up to the right of its caller: the
    // columns are worked out across the whole project, not per file.
    function layout(g) {
        var maxLayer = 0;
        g.nodes.forEach(function (n) { if (n.layer > maxLayer) maxLayer = n.layer; });

        var boxes = [];
        var y = PAD;
        g.files.forEach(function (rel) {
            var mine = g.nodes.filter(function (n) { return n.rel === rel; });
            if (!mine.length) return;
            var rows = {};        // layer -> next free row
            var maxRow = 0;
            mine.sort(function (a, b) {
                return a.layer - b.layer || a.line - b.line;
            }).forEach(function (n) {
                var r = rows[n.layer] || 0;
                rows[n.layer] = r + 1;
                if (r + 1 > maxRow) maxRow = r + 1;
                n.x = PAD + n.layer * COL;
                n.y = y + HEAD + r * ROW;
            });
            var h = HEAD + Math.max(1, maxRow) * ROW + 4;
            boxes.push({ rel: rel, y: y, h: h });
            y += h + GAP;
        });

        return {
            boxes: boxes,
            width: PAD * 2 + (maxLayer + 1) * COL,
            height: y
        };
    }

    function svgFor(g) {
        var L = layout(g);
        var parts = [];
        parts.push('<svg width="' + L.width + '" height="' + L.height + '" viewBox="0 0 ' + L.width + ' ' + L.height + '">');

        L.boxes.forEach(function (b) {
            parts.push('<g class="mfile">' +
                '<rect x="2" y="' + b.y + '" width="' + (L.width - 4) + '" height="' + b.h + '"></rect>' +
                '<text x="8" y="' + (b.y + 12) + '">' + esc(short(b.rel, 46)) + '</text>' +
                '</g>');
        });

        // Edges first, so a box is never drawn under a line.
        g.nodes.forEach(function (n) {
            if (n.x == null) return;
            n.calls.forEach(function (id) {
                var t = g.byId[id];
                if (!t || t.x == null) return;
                parts.push(edgePath(n, t, 'medge' + (t.rel !== n.rel ? ' far' : '')));
            });
            n.writes.forEach(function (id) {
                var t = g.byId[id];
                if (!t || t.x == null) return;
                parts.push(edgePath(n, t, 'medge rw'));
            });
            n.reads.forEach(function (id) {
                if (n.writes.indexOf(id) !== -1) return;
                var t = g.byId[id];
                if (!t || t.x == null) return;
                parts.push(edgePath(n, t, 'medge rw'));
            });
        });

        g.nodes.forEach(function (n) {
            if (n.x == null) return;
            var cls = 'mnode ' +
                (n.kind === 'handler' ? 'entry' : n.kind === 'value' ? 'val' : 'fn') +
                (n.dead ? ' dead' : '') +
                (n.id === selected ? ' sel' : '');
            var sub = n.kind === 'handler'
                ? (n.wiring === 'inline' ? 'written here' : 'starts here')
                : n.kind === 'value' ? (n.readBy.length ? 'value kept' : 'nothing reads it')
                    : n.dead ? 'never runs'
                        : n.kind === 'class' ? 'class' : 'function';
            parts.push('<g class="' + cls + '" data-id="' + esc(n.id) + '">' +
                '<title>' + esc(n.name + '  (' + n.rel + ' line ' + n.line + ')') + '</title>' +
                '<rect x="' + n.x + '" y="' + n.y + '" width="' + NW + '" height="' + NH + '"></rect>' +
                '<text x="' + (n.x + 7) + '" y="' + (n.y + 12) + '">' + esc(short(n.name, 22)) + '</text>' +
                '<text class="msub" x="' + (n.x + 7) + '" y="' + (n.y + 21) + '">' + esc(sub) + '</text>' +
                '</g>');
        });

        parts.push('</svg>');
        return parts.join('');
    }

    function edgePath(a, b, cls) {
        var x1 = a.x + NW, y1 = a.y + NH / 2;
        var x2 = b.x, y2 = b.y + NH / 2;
        // A target to the left of its source is a call back up the chain. It
        // still has to be drawn, so it leaves the left side instead and bows
        // out, rather than crossing straight through the box it came from.
        if (x2 < x1) { x1 = a.x; x2 = b.x + NW; }
        var dx = Math.max(18, Math.abs(x2 - x1) * 0.5);
        var c1 = x1 + (x2 >= x1 ? dx : -dx);
        var c2 = x2 - (x2 >= x1 ? dx : -dx);
        return '<path class="' + cls + '" d="M' + x1 + ' ' + y1 + ' C' + c1 + ' ' + y1 + ' ' + c2 + ' ' + y2 + ' ' + x2 + ' ' + y2 + '"></path>';
    }

    // ---- selection and the reading -----------------------------------------
    function select(id, reveal) {
        selected = id;
        var node = graph && graph.byId[id];
        if (!node) return;
        var host = $('mapGraph');
        if (host) {
            var all = host.querySelectorAll('g[data-id]');
            for (var i = 0; i < all.length; i++) {
                all[i].classList.toggle('sel', all[i].getAttribute('data-id') === id);
            }
        }
        renderDetail(node);
        if (reveal) revealNode(node);
    }

    // The same reveal the EXPLAIN panel uses: open the file if it is not the
    // one on screen, put the cursor on the line, and mark the line so the eye
    // lands on it after the panel took the attention.
    function revealNode(node) {
        var editor = ctx.editor();
        var monaco = ctx.monaco();
        if (!editor || !monaco) return;
        if (ctx.activeRel() !== node.rel) {
            if (!ctx.files()[node.rel]) return;
            ctx.openFile(node.rel);
        }
        var line = Math.max(1, node.line);
        try {
            editor.revealLineInCenter(line);
            editor.setPosition({ lineNumber: line, column: 1 });
            decorations = editor.deltaDecorations(decorations, [{
                range: new monaco.Range(line, 1, line, 1),
                options: { isWholeLine: true, className: 'bf6-explain-line' }
            }]);
        } catch (e) { }
    }

    function renderDetail(node) {
        var host = $('mapDetail');
        if (!host) return;
        host.innerHTML = '';

        var card = el('div', 'card' + (node.dead ? ' warn' : node.kind === 'handler' ? ' ok' : ''));
        var pill = node.kind === 'handler' ? '<span class="pill acc">Starts here</span>'
            : node.dead ? '<span class="pill warn">Never runs</span>'
                : node.kind === 'value' ? '<span class="pill">Value kept</span>'
                    : '<span class="pill">Function</span>';
        card.innerHTML = '<div class="t">' + esc(node.name) + '</div>' + pill +
            '<span class="pill">' + esc(short(node.rel, 30)) + '</span>';

        describe(node, graph).forEach(function (p) {
            var d = el('div');
            d.style.marginTop = '5px';
            if (p.quiet) d.style.color = 'var(--dim)';
            d.innerHTML = '<b>' + esc(p.label) + '.</b> ' + esc(p.text);
            card.appendChild(d);
        });

        var go = el('button', null, 'Show me this line');
        go.style.marginTop = '7px';
        go.onclick = function () { revealNode(node); };
        card.appendChild(go);
        host.appendChild(card);

        linkRow(host, 'What runs this', node.calledBy);
        linkRow(host, 'What this calls', node.calls);
        linkRow(host, 'Values it changes', node.writes);
        linkRow(host, 'Values it reads', node.reads);
        linkRow(host, 'Changed by', node.writtenBy);
        linkRow(host, 'Read by', node.readBy);
    }

    function linkRow(host, title, ids) {
        if (!ids || !ids.length) return;
        host.appendChild(el('div', 'h', title));
        ids.forEach(function (id) {
            var t = graph.byId[id];
            if (!t) return;
            var b = el('button', 'row');
            b.innerHTML = '<span class="t">' + esc(t.name) + '</span>' +
                '<span class="d">' + esc(t.rel) + ', line ' + t.line + '</span>';
            b.onclick = function () { select(id, true); };
            host.appendChild(b);
        });
    }

    // ========================================================================
    // ATTACH
    //
    // editor.js keeps the editor, the open files and the mode inside its own
    // closure. Rather than exporting six things from there, it hands over one
    // object of accessors, which is the whole of the seam between the two
    // files and the reason nothing in editor.js had to be restructured.
    // ========================================================================
    function attach(handles) {
        ctx = handles;
        var b = $('btnMapRedraw');
        if (b) b.onclick = function () { invalidate(); draw(); };
    }

    return {
        version: 1,
        attach: attach,
        draw: draw,
        invalidate: invalidate,
        buildGraph: buildGraph,
        describe: describe
    };
})();
