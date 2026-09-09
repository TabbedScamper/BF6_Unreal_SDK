/*
 * BF6 assist: everything an AI needs about this project, in one go.
 *
 *   Node:    const P = require('./contextpack.js'); P.build({...})
 *   Browser: BF6ContextPack.build({...})
 *
 * WHY THIS IS PROSE AND NOT A PILE OF JSON
 *
 * The obvious version of this attaches catalog.json, events.json and the .d.ts
 * files and calls it context. That is 500 KB of mostly punctuation, it buries
 * the six facts that actually decide whether generated code works, and every
 * model that reads it spends its attention on syntax rather than on the rules.
 *
 * So this writes a briefing. The block vocabulary becomes one signature per
 * line, taken from the catalog's own tip field. The events become their real
 * parameter lists. And the traps go in explicitly, because they are the things
 * no amount of reading the type definitions would reveal: an array cannot go
 * inside an array, a For block binds a variable rather than an array slot,
 * IfThenElse evaluates both arms. Every one of those was learned by being
 * bitten, and each one silently produces a mod that loads and misbehaves.
 *
 * The pack is deliberately usable with NO provider attached. It is text. Copy
 * it into whatever you already talk to.
 */
(function (root, factory) {
    if (typeof module === 'object' && module && module.exports) { module.exports = factory(); }
    else { root.BF6ContextPack = factory(); }
}(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var VERSION = '1.0.0';

    function has(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }
    function lines(a) { return a.join('\n'); }

    /* ---------------------------------------------------------------- rules */

    /* THE PART A MODEL CANNOT DERIVE.
     *
     * Written once, here, because all three editors need the same answer and
     * because these are the facts that decide whether generated code is right.
     * Each is stated with its consequence, since "arrays cannot nest" without
     * "and the editor will let you try" is only half a warning.
     */
    function rulesFor(target) {
        var common = [
            '## Rules that decide whether this works',
            '',
            'These are not style preferences. Each one silently produces a mod that',
            'loads and then behaves wrongly.',
            '',
            '- **An array cannot contain an array.** `AppendToArray(a, b)` where b is an',
            '  array CONCATENATES them; it does not nest. The block editor permits the',
            '  connection, so this looks legal right up until the data is gone.',
            '  (EA, bf6-portal-mod-types/index.d.ts, on AppendToArray.)',
            '- **`IfThenElse` evaluates both arms** before choosing, and so do `And` and',
            '  `Or`. A discarded branch still runs. If either side has an effect, use',
            '  real control flow instead.',
            '- **Player and Team share one variable budget.** Capacity is 128 Global plus',
            '  128 Object, where Object is Player and Team counted together, not 128 each.',
            '- **`maxDepth` is enforced nowhere** in the editor, and there is no cap on',
            '  blocks, rules or subroutines. Variables are the only hard editor limit.',
            '- **Recursion has no check anywhere.** The flyout will offer a subroutine to',
            '  itself. That is not evidence it works at runtime.',
            '- **`compile()` keeps only the FIRST top-level mod block** and silently',
            '  discards every other orphan. Subroutine blocks must be top level.',
            '- **An unknown block type is replaced on load with a placeholder AND its',
            '  whole input subtree is dropped.** A workspace can lose work quietly.'
        ];

        if (target === 'typescript') {
            return common.concat([
                '',
                '### This project targets Portal TypeScript',
                '',
                '- Portal script mode runs real TypeScript, so **`let` inside a handler is a',
                '  genuine per-invocation local**. Two players through the same handler each',
                '  get their own copy, including across an `await mod.Wait(...)`.',
                '- Script state does not consume Portal variable slots, so the 128 budget is',
                '  not the ceiling here.',
                '- Prefer ordinary locals, `Map`, and arrays. Do not hand-roll global slots.'
            ]);
        }
        return common.concat([
            '',
            '### This project targets native Portal blocks',
            '',
            '- **There are no locals.** Every "local" becomes a shared global slot owned by',
            '  the function. Two live invocations of one handler SHARE it, so a value held',
            '  across a `Wait` is overwritten by the next player through that handler.',
            '  Recursion has the same problem. Avoid holding state across a wait.',
            '- **A `For` block binds a variable**, not an array element, so a loop counter',
            '  cannot live inside a packed array.',
            '- Keep the variable count low: 128 Global, 128 shared Player and Team.'
        ]);
    }

    /* ------------------------------------------------------------ vocabulary */

    /* One line per block, from the catalog's own tip. A model completing against
     * this cannot invent a block that does not exist, which is the single most
     * common failure when generating for a closed API. */
    function vocabulary(catalog, limit) {
        if (!catalog) { return ['(no block catalog was available)']; }
        var names = Object.keys(catalog).sort(), out = [], i, n = 0;
        for (i = 0; i < names.length; i++) {
            var b = catalog[names[i]];
            if (!b) { continue; }
            var tip = b.tip || '';
            if (!tip) {
                var ps = (b.params || []).join(', ');
                tip = names[i] + '(' + ps + ')' + (b.ret ? ': ' + b.ret : '');
            }
            out.push('  ' + tip);
            n++;
            if (limit && n >= limit) { out.push('  ... and ' + (names.length - n) + ' more'); break; }
        }
        return out;
    }

    function eventList(events) {
        if (!events) { return ['(no event table was available)']; }
        var names = Object.keys(events).sort(), out = [], i;
        for (i = 0; i < names.length; i++) {
            var ps = ((events[names[i]] || {}).params || [])
                .map(function (p) { return p.name + ': ' + p.type; }).join(', ');
            out.push('  ' + names[i] + '(' + ps + ')');
        }
        return out;
    }

    /* ------------------------------------------------------------- the build */

    /*
     * opts:
     *   where       'blocks' | 'script' | 'ui'      which editor is asking
     *   target      'blocks' | 'typescript'         the project's output target
     *   catalog     catalog.json
     *   events      events.json
     *   project     { name, files: [..], target }
     *   openFile    { path, text }                  what the user is looking at
     *   workspace   { blocks, variables, rules }    counts, not the whole tree
     *   diagnostics [ { message, file, line, blockId } ]
     *   selection   free text: the block or lines selected
     *   question    the user's sentence
     *   vocabLimit  cap the block list (0 = all)
     */
    function build(opts) {
        opts = opts || {};
        var target = opts.target === 'typescript' ? 'typescript' : 'blocks';
        var L = [];

        L.push('# Battlefield 6 Portal project briefing');
        L.push('');
        L.push('Generated by the BF6 Unreal SDK, context pack ' + VERSION + '.');
        L.push('You are helping inside the ' + (opts.where || 'blocks') + ' editor.');
        L.push('');

        L.push('## What you are being asked to produce');
        L.push('');
        if (target === 'typescript') {
            L.push('TypeScript for a Portal script project. It is compiled with strict');
            L.push('TypeScript against the real Portal typings and then bundled, so it must');
            L.push('type-check. Use only `mod.*` calls from the vocabulary below.');
        } else {
            L.push('TypeScript that will be CONVERTED INTO PORTAL BLOCKS by this tool. Write');
            L.push('plain, flat TypeScript: event handler functions plus small helper');
            L.push('functions. Use only `mod.*` calls from the vocabulary below. Avoid');
            L.push('classes, closures, generics, try/catch and Promises: they have no block');
            L.push('form and will be reported as unconvertible.');
        }
        L.push('');
        L.push('Answer with a short explanation, then one fenced ```ts block. Do not invent');
        L.push('block or function names. If something cannot be expressed, say so plainly');
        L.push('rather than approximating it.');
        L.push('');

        L.push(lines(rulesFor(target)));
        L.push('');

        if (opts.project) {
            L.push('## This project');
            L.push('');
            L.push('- name: ' + (opts.project.name || '(unnamed)'));
            L.push('- output target: ' + target);
            if (opts.project.files && opts.project.files.length) {
                L.push('- files: ' + opts.project.files.join(', '));
            }
            if (opts.workspace) {
                L.push('- workspace: ' + (opts.workspace.blocks || 0) + ' blocks, ' +
                       (opts.workspace.rules || 0) + ' rules, ' +
                       (opts.workspace.variables || 0) + ' variables');
            }
            L.push('');
        }

        if (opts.diagnostics && opts.diagnostics.length) {
            L.push('## Problems the tool is reporting right now');
            L.push('');
            opts.diagnostics.slice(0, 40).forEach(function (d) {
                L.push('- ' + (d.file ? d.file + ':' + (d.line || '?') + '  ' : '') +
                       (d.message || String(d)));
            });
            if (opts.diagnostics.length > 40) {
                L.push('- ... and ' + (opts.diagnostics.length - 40) + ' more');
            }
            L.push('');
        }

        L.push('## Events you can handle');
        L.push('');
        L.push('```');
        L.push(lines(eventList(opts.events)));
        L.push('```');
        L.push('');

        // The same list, named for what it is on this side: a script project
        // calls commands, and telling a model it is choosing "blocks" while it
        // writes TypeScript is a word it has to work around on every answer.
        L.push(target === 'typescript'
            ? '## Every mod.* command available, as a signature'
            : '## Every block available, as a signature');
        L.push('');
        L.push('```');
        L.push(lines(vocabulary(opts.catalog, opts.vocabLimit || 0)));
        L.push('```');
        L.push('');

        if (opts.selection) {
            L.push('## What the user has selected');
            L.push('');
            L.push('```');
            L.push(String(opts.selection).slice(0, 4000));
            L.push('```');
            L.push('');
        }

        if (opts.openFile && opts.openFile.text) {
            L.push('## The file they are looking at: ' + (opts.openFile.path || 'source.ts'));
            L.push('');
            L.push('```ts');
            L.push(String(opts.openFile.text).slice(0, 60000));
            L.push('```');
            L.push('');
        }

        if (opts.question) {
            L.push('## What they asked');
            L.push('');
            L.push(String(opts.question));
            L.push('');
        }

        return L.join('\n');
    }

    /* A rough size, so the caller can warn before sending something enormous. */
    function measure(text) {
        var chars = String(text || '').length;
        return { chars: chars, kb: Math.round(chars / 1024), approxTokens: Math.round(chars / 4) };
    }

    return { VERSION: VERSION, build: build, measure: measure,
             vocabulary: vocabulary, eventList: eventList, rulesFor: rulesFor };
}));
