/*
 * BF6Convert - two-way converter between Battlefield Portal block workspaces
 * (Blockly 10.3.0 serialization JSON) and Portal TypeScript.
 *
 * Loads in Node (module.exports) and in a browser (window.BF6Convert) with no
 * bundler. ES5 syntax only, so it also runs inside embedded script engines.
 *
 * Data sidecars (optional but strongly recommended):
 *   catalog.json  block type -> kind / input names / parameter types
 *   events.json   Portal event handler name -> typed parameter list
 * Node loads both automatically. A browser host calls setCatalog / setEvents.
 *
 * TypeScript parsing (tsToBlocks) needs the TypeScript compiler API. Node
 * resolves it through require('typescript'); any host can inject it with
 * setTypeScript(ts).
 */
;(function (root, factory) {
    'use strict';
    if (typeof module === 'object' && module && module.exports) {
        module.exports = factory();
    } else {
        root.BF6Convert = factory();
    }
})(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    var VERSION = '1.1.0-readable-names';

    /* ------------------------------------------------------------------ *
     * Sidecar data
     * ------------------------------------------------------------------ */

    var CATALOG = {};
    var EVENTS = {};
    var TS = null;

    function setCatalog(c) { CATALOG = c || {}; }
    function setEvents(e) { EVENTS = e || {}; }
    function setTypeScript(ts) { TS = ts || null; }

    if (typeof require === 'function') {
        try { CATALOG = require('./catalog.json'); } catch (e0) { /* browser */ }
        try { EVENTS = require('./events.json'); } catch (e1) { /* browser */ }
        try { TS = require('typescript'); } catch (e2) { /* injected later */ }
    }

    /* ------------------------------------------------------------------ *
     * Constants
     * ------------------------------------------------------------------ */

    /* Blocks whose second value input is an expression evaluated once per
     * array element. In TypeScript they become an arrow function. */
    var LAMBDA_ARG = {
        FilteredArray: 1,
        SortedArray: 1,
        IsTrueForAll: 1,
        IsTrueForAny: 1,
        IndexOfFirstTrue: 1,
        MappedArray: 1
    };

    /* Blocks that exist in Blockly but not as a mod.* function. They are
     * emitted as helpers from the generated runtime.ts. */
    var RUNTIME_ONLY = {
        ArrayContains: 1,
        IndexOfArrayValue: 1,
        RemoveFromArray: 1
    };

    /* Structural block types handled by name, never as a mod.* call. */
    /* Statement blocks that suspend the script; they need await in TypeScript. */
    var AWAITING = { Wait: 1, WaitUntil: 1 };

    /* Loop control blocks with a native TypeScript form. */
    var LOOP_CONTROL = { Break: 'break', Continue: 'continue' };

    var STRUCTURAL = {
        modBlock: 1, ruleBlock: 1, conditionBlock: 1, subroutineBlock: 1,
        subroutineInstanceBlock: 1, subroutineArgumentBlock: 1,
        variableReferenceBlock: 1, SetVariable: 1, GetVariable: 1,
        SetVariableAtIndex: 1, If: 1, ForVariable: 1, While: 1, Wait: 1,
        Number: 1, Text: 1, Boolean: 1, CurrentArrayElement: 1
    };

    /* Rule group ordering. First match wins, so the specific patterns lead. */
    var FAMILIES = [
        ['ui', /UIButton|UIWidget/, 'UI'],
        ['ai', /^(OnAI|OnAutoPlayer|OngoingSpawner|OnSpawnerSpawned)/, 'AI and spawners'],
        ['vehicles', /Vehicle/, 'Vehicles'],
        ['objectives', /(CapturePoint|Sector|HQ|MCOM|InteractPoint|AreaTrigger|RingOfFire|WaypointPath|LootSpawner|WorldIcon|SpawnPoint)/, 'Objectives and world'],
        ['round-flow', /^(OnGameMode|OnTimeLimit|OngoingGlobal)/, 'Round flow'],
        ['players', /^(OnPlayer|OngoingPlayer|OngoingTeam|OnMandown|OnRevived|OnPortalGadget|OnRayCast)/, 'Players'],
        ['other', /./, 'Other']
    ];

    var TYPE_MAP = {
        Boolean: 'boolean', boolean: 'boolean',
        Number: 'number', number: 'number',
        String: 'string', string: 'string',
        Text: 'string',
        Any: 'mod.Any'
    };

    /* ------------------------------------------------------------------ *
     * Small utilities (ES5)
     * ------------------------------------------------------------------ */

    function has(o, k) { return Object.prototype.hasOwnProperty.call(o, k); }

    function keysOf(o) {
        var r = [], k;
        for (k in o) { if (has(o, k)) { r.push(k); } }
        return r;
    }

    function repeat(s, n) {
        var r = '', i;
        for (i = 0; i < n; i++) { r += s; }
        return r;
    }

    function pad(n) { return repeat('    ', n); }

    function sanitizeIdent(name) {
        var s = String(name === undefined || name === null ? '' : name);
        s = s.replace(/[^A-Za-z0-9_]/g, '_');
        if (s === '') { s = 'Unnamed'; }
        if (/^[0-9]/.test(s)) { s = '_' + s; }
        return s;
    }

    function jsonStr(s) { return JSON.stringify(String(s)); }

    function numLit(n) {
        var v = Number(n);
        if (!isFinite(v)) { return '0'; }
        return String(v);
    }

    function tsTypeOf(portalType) {
        var t = String(portalType || 'Any');
        if (has(TYPE_MAP, t)) { return TYPE_MAP[t]; }
        return 'mod.' + t;
    }

    function familyOf(eventKey) {
        var i;
        for (i = 0; i < FAMILIES.length; i++) {
            if (FAMILIES[i][1].test(eventKey)) { return FAMILIES[i][0]; }
        }
        return 'other';
    }

    function familyTitle(id) {
        var i;
        for (i = 0; i < FAMILIES.length; i++) {
            if (FAMILIES[i][0] === id) { return FAMILIES[i][2]; }
        }
        return id;
    }

    /* THE CONNECTION CHECKS BLOCKLY ITSELF ENFORCES.
     *
     * catalog.json carries one signature per block, so Add reads as taking a
     * Number when it really takes a Number or a Vector. Using it to decide what
     * fits where condemned correct code. The block definitions carry the actual
     * check arrays - Add's input says ["Number","Vector"] - and those are what
     * the editor tests a connection against.
     *
     * Supplied from outside because the definitions live with the editor, not
     * with this file. Without them nothing below runs and the converter behaves
     * exactly as it did before. */
    var CHECKS = {};
    function setBlockChecks(defs) {
        CHECKS = {};
        if (!defs) { return; }
        var names = keysOf(defs), i, j;
        for (i = 0; i < names.length; i++) {
            var d = defs[names[i]];
            if (!d) { continue; }
            var rec = { inputs: {}, output: null };
            if (d.output && d.output.length !== undefined) { rec.output = d.output; }
            var ins = d.inputs || [];
            for (j = 0; j < ins.length; j++) {
                if (ins[j] && ins[j].name && ins[j].check && ins[j].check.length !== undefined) {
                    rec.inputs[ins[j].name] = ins[j].check;
                }
            }
            CHECKS[names[i]] = rec;
        }
    }

    function blockKind(type) {
        var e = CATALOG[type];
        if (e && e.kind) { return e.kind; }
        return 'value';
    }

    /* Chain walker for Blockly "next" links. */
    function chain(block) {
        var out = [];
        var b = block;
        while (b) {
            out.push(b);
            b = b.next && b.next.block ? b.next.block : null;
        }
        return out;
    }

    function inputBlock(block, name) {
        if (!block || !block.inputs || !block.inputs[name]) { return null; }
        var slot = block.inputs[name];
        return slot.block || slot.shadow || null;
    }

    function valueInputs(block) {
        /* Ordered VALUE-0, VALUE-1, ... present on this block instance. */
        var out = [], i = 0, b;
        if (!block || !block.inputs) { return out; }
        for (i = 0; i < 32; i++) {
            b = inputBlock(block, 'VALUE-' + i);
            if (b === null && !has(block.inputs, 'VALUE-' + i)) {
                /* Allow a single gap so a missing optional socket does not
                 * truncate the argument list. */
                if (!has(block.inputs, 'VALUE-' + (i + 1))) { break; }
            }
            out.push(b);
        }
        while (out.length && out[out.length - 1] === null) { out.pop(); }
        return out;
    }

    /* ------------------------------------------------------------------ *
     * Report
     * ------------------------------------------------------------------ */

    function newReport(kind) {
        return {
            kind: kind,
            unconvertible: [],
            warnings: [],
            counts: {}
        };
    }

    /* MEASURED, NOT GUESSED. From the Portal editor bundle itself:
     *
     *     { maxDepth: 64, maxGlobalVariables: ue ?? 128, maxObjectVariables: ce ?? 128 }
     *
     * so 128 variables per scope, and separately a maximum nesting DEPTH of
     * 64 - which is a limit on how deep one expression may be, and is easy to
     * blow by building a long value out of nested calls.
     */
    var VARIABLE_CEILING = 128;
    var DEPTH_CEILING = 64;

    function reportUnconvertible(report, file, line, construct, suggestion) {
        report.unconvertible.push({
            file: file, line: line, construct: construct, suggestion: suggestion
        });
    }

    function warn(report, text) { report.warnings.push(text); }

    /* ================================================================== *
     * 1. BLOCKS -> INTERMEDIATE MODEL
     * ================================================================== */

    function blocksToIr(workspace, report) {
        var ws = workspace && workspace.mod ? workspace.mod : workspace;
        if (!ws || !ws.blocks || !ws.blocks.blocks) {
            throw new Error('Not a Portal block workspace: missing mod.blocks.blocks');
        }

        var program = {
            variables: [], varById: {}, rules: [], subroutines: [], orphans: [],
            layout: { mod: null }
        };

        /* --- variables ------------------------------------------------ */
        var scopeCount = { Global: 0, Player: 0, Team: 0 };
        var vars = ws.variables || [];
        var i, v, scope, rec, used = {};
        for (i = 0; i < vars.length; i++) {
            v = vars[i];
            scope = v.type || 'Global';
            if (!has(scopeCount, scope)) { scopeCount[scope] = 0; }
            var ident = sanitizeIdent(v.name) + scope + 'Var';
            if (used[ident]) { ident = ident + '_' + i; }
            used[ident] = 1;
            rec = {
                id: v.id, name: v.name, scope: scope,
                slot: scopeCount[scope]++, ident: ident
            };
            program.variables.push(rec);
            program.varById[v.id] = rec;
        }

        /* --- top level blocks ----------------------------------------- */
        var tops = ws.blocks.blocks;
        var modBlock = null, subBlocks = [];
        for (i = 0; i < tops.length; i++) {
            if (tops[i].type === 'modBlock') { modBlock = tops[i]; }
            else if (tops[i].type === 'subroutineBlock') { subBlocks.push(tops[i]); }
            else {
                /* Detritus left on the workspace canvas: not part of any rule,
                 * but kept so a round trip does not silently delete it. */
                program.orphans.push(tops[i]);
            }
        }
        if (modBlock) {
            program.layout.mod = { x: modBlock.x || 0, y: modBlock.y || 0, id: modBlock.id };
        }

        /* --- subroutines (needed before rules for argument names) ----- */
        var subMeta = {};
        for (i = 0; i < subBlocks.length; i++) {
            var sb = subBlocks[i];
            var sname = (sb.fields && sb.fields.SUBROUTINE_NAME) ||
                (sb.extraState && sb.extraState.subroutineName) || ('Subroutine' + i);
            var sparams = (sb.extraState && sb.extraState.parameters) || [];
            subMeta[sname] = { name: sname, params: sparams };
        }

        for (i = 0; i < subBlocks.length; i++) {
            program.subroutines.push(
                subroutineToIr(subBlocks[i], program, subMeta, report));
        }
        resolveEventInfoNeed(program);

        /* --- rules ---------------------------------------------------- */
        if (modBlock) {
            var ruleBlocks = chain(inputBlock(modBlock, 'RULES'));
            var nameSeen = {};
            for (i = 0; i < ruleBlocks.length; i++) {
                var rb = ruleBlocks[i];
                if (rb.type !== 'ruleBlock') {
                    warn(report, 'Skipped non rule block "' + rb.type + '" in the rule stack');
                    continue;
                }
                program.rules.push(ruleToIr(rb, program, subMeta, report, nameSeen, program.rules.length));
            }
        }

        report.counts.rules = program.rules.length;
        report.counts.subroutines = program.subroutines.length;
        report.counts.variables = program.variables.length;
        if (program.orphans.length) {
            report.counts.orphanBlocks = program.orphans.length;
            warn(report, program.orphans.length + " free floating block(s) on the workspace canvas are not part of any rule. They carry no behaviour and are preserved verbatim on the way back.");
        }
        return program;
    }

    /* A subroutine that reads an event value (EventPlayer and friends) inherits
     * the calling rule's event context. Blocks pass that implicitly; TypeScript
     * cannot, so such subroutines take a trailing eventInfo parameter, and the
     * need propagates to every caller. */
    function resolveEventInfoNeed(program) {
        var i, changed = true, pass = 0;
        var byName = {};
        for (i = 0; i < program.subroutines.length; i++) {
            var s = program.subroutines[i];
            s.needsEventInfo = usesEventValue(s.conditions, s.actions);
            byName[s.name] = s;
            byName[s.fnName] = s;
        }
        while (changed && pass < 64) {
            changed = false;
            pass++;
            for (i = 0; i < program.subroutines.length; i++) {
                var sub = program.subroutines[i];
                if (sub.needsEventInfo) { continue; }
                if (callsNeedy(sub.actions, byName) || callsNeedyExpr(sub.conditions, byName)) {
                    sub.needsEventInfo = true;
                    changed = true;
                }
            }
        }
    }

    function usesEventValue(conds, stmts) {
        var found = false;
        function ex(e) {
            if (!e || found) { return; }
            if (e.k === 'event') { found = true; return; }
            var i;
            if (e.args) { for (i = 0; i < e.args.length; i++) { ex(e.args[i]); } }
            if (e.body) { ex(e.body); }
            if (e.ref) { ex(e.ref); }
            if (e.object) { ex(e.object); }
        }
        function st(list) {
            var i, s, j;
            if (!list) { return; }
            for (i = 0; i < list.length; i++) {
                s = list[i];
                if (!s || found) { continue; }
                if (s.args) { for (j = 0; j < s.args.length; j++) { ex(s.args[j]); } }
                ex(s.seconds); ex(s.ref); ex(s.value); ex(s.index); ex(s.cond);
                ex(s.from); ex(s.to); ex(s.step);
                if (s.branches) { for (j = 0; j < s.branches.length; j++) { ex(s.branches[j].cond); st(s.branches[j].body); } }
                st(s.elseBody); st(s.body);
            }
        }
        var i;
        if (conds) { for (i = 0; i < conds.length; i++) { ex(conds[i]); } }
        st(stmts);
        return found;
    }

    function callsNeedyExpr(conds, byName) {
        var found = false;
        function ex(e) {
            if (!e || found) { return; }
            if (e.k === 'subCall' && byName[e.name] && byName[e.name].needsEventInfo) { found = true; return; }
            var i;
            if (e.args) { for (i = 0; i < e.args.length; i++) { ex(e.args[i]); } }
            if (e.body) { ex(e.body); }
            if (e.ref) { ex(e.ref); }
            if (e.object) { ex(e.object); }
        }
        var i;
        if (conds) { for (i = 0; i < conds.length; i++) { ex(conds[i]); } }
        return found;
    }

    function callsNeedy(stmts, byName) {
        var found = false;
        function ex(e) {
            if (!e || found) { return; }
            if (e.k === 'subCall' && byName[e.name] && byName[e.name].needsEventInfo) { found = true; return; }
            var i;
            if (e.args) { for (i = 0; i < e.args.length; i++) { ex(e.args[i]); } }
            if (e.body) { ex(e.body); }
            if (e.ref) { ex(e.ref); }
            if (e.object) { ex(e.object); }
        }
        function st(list) {
            var i, s, j;
            if (!list) { return; }
            for (i = 0; i < list.length; i++) {
                s = list[i];
                if (!s || found) { continue; }
                if (s.k === 'sub' && byName[s.name] && byName[s.name].needsEventInfo) { found = true; return; }
                if (s.args) { for (j = 0; j < s.args.length; j++) { ex(s.args[j]); } }
                ex(s.seconds); ex(s.ref); ex(s.value); ex(s.index); ex(s.cond);
                ex(s.from); ex(s.to); ex(s.step);
                if (s.branches) { for (j = 0; j < s.branches.length; j++) { ex(s.branches[j].cond); st(s.branches[j].body); } }
                st(s.elseBody); st(s.body);
            }
        }
        st(stmts);
        return found;
    }

    function eventKeyOf(eventType, objectType) {
        if (eventType === 'Ongoing') { return 'Ongoing' + (objectType || 'Global'); }
        return eventType;
    }

    function ruleToIr(rb, program, subMeta, report, nameSeen, index) {
        var f = rb.fields || {};
        var eventType = f.EVENTTYPE || 'Ongoing';
        var objectType = f.OBJECTTYPE || 'Global';
        var key = eventKeyOf(eventType, objectType);
        var base = key + '_' + sanitizeIdent(f.NAME || ('Rule' + index));
        var fn = base;
        if (nameSeen[base]) { fn = base + nameSeen[base]; }
        nameSeen[base] = (nameSeen[base] || 0) + 1;

        var ctx = { program: program, subMeta: subMeta, report: report, params: [], where: 'rule ' + (f.NAME || index) };
        return {
            kind: 'rule',
            id: rb.id,
            index: index,
            name: f.NAME === undefined ? '' : f.NAME,
            event: eventType,
            objectType: objectType,
            eventKey: key,
            fnName: fn,
            isOngoing: !!(rb.extraState && rb.extraState.isOngoingEvent) || eventType === 'Ongoing',
            comment: commentOf(rb),
            /* A WHOLE RULE CAN BE SWITCHED OFF TOO, AND THAT IS THE WORST ONE
             * TO IGNORE. Disabling a rule is how you try a match without it;
             * exporting it as live code means the thing you turned off is the
             * thing that ships. Carried into the IR so every writer can honour
             * it rather than each rediscovering the flag. */
            disabled: isDisabledBlock(rb),
            conditions: conditionsToIr(rb, ctx),
            actions: stmtsToIr(inputBlock(rb, 'ACTIONS'), ctx)
        };
    }

    function subroutineToIr(sb, program, subMeta, report) {
        var name = (sb.fields && sb.fields.SUBROUTINE_NAME) ||
            (sb.extraState && sb.extraState.subroutineName) || 'Subroutine';
        var params = (sb.extraState && sb.extraState.parameters) || [];
        var norm = [], i;
        for (i = 0; i < params.length; i++) {
            norm.push({
                name: sanitizeIdent(params[i].name || ('arg' + i)),
                type: params[i].types || 'Any'
            });
        }
        var ctx = { program: program, subMeta: subMeta, report: report, params: norm, where: 'subroutine ' + name };
        return {
            kind: 'subroutine',
            id: sb.id,
            name: name,
            fnName: sanitizeIdent(name),
            params: norm,
            x: sb.x || 0,
            y: sb.y || 0,
            comment: commentOf(sb),
            disabled: isDisabledBlock(sb),
            conditions: conditionsToIr(sb, ctx),
            actions: stmtsToIr(inputBlock(sb, 'ACTIONS'), ctx)
        };
    }

    function commentOf(b) {
        if (b && b.icons && b.icons.comment && b.icons.comment.text) {
            return String(b.icons.comment.text);
        }
        return null;
    }

    function conditionsToIr(holder, ctx) {
        var out = [];
        var cbs = chain(inputBlock(holder, 'CONDITIONS'));
        var i, e;
        for (i = 0; i < cbs.length; i++) {
            if (cbs[i].type !== 'conditionBlock') { continue; }
            e = exprToIr(inputBlock(cbs[i], 'CONDITION'), ctx);
            if (e) { out.push(e); }
        }
        return out;
    }

    /* A DISABLED BLOCK IS NOT CODE.
     *
     * Blockly serialises a block the author switched off as enabled:false, and
     * that used to be ignored completely: the block was lowered like any other
     * and the exported TypeScript RAN it. Somebody who disables a block to try
     * a match without it, exports, and finds it still happening has been lied
     * to by the tool.
     *
     * It is not silently dropped either, because the author kept it on the
     * canvas on purpose. It goes out commented, which both stops it running
     * and preserves the thing they were keeping.
     */
    function isDisabledBlock(b) {
        if (!b) { return false; }
        return b.enabled === false || b.disabled === true;
    }

    /* Comments on the value blocks inside a statement.
     *
     * A comment on an expression - on the Add inside SetScore, say - had
     * nowhere to go and was dropped. Expression lowering returns a value, not
     * a place to hang a line of text, so they are gathered from the statement's
     * input tree and written above the statement, each naming the block it was
     * attached to so it is still obvious what it referred to. */
    function valueCommentsOf(b, into) {
        if (!b || typeof b !== 'object') { return into; }
        var k;
        if (b.inputs) {
            for (k in b.inputs) {
                if (!has(b.inputs, k) || !b.inputs[k] || !b.inputs[k].block) { continue; }
                var inner = b.inputs[k].block;
                var c = commentOf(inner);
                if (c) { into.push({ on: inner.type || 'a block', text: c }); }
                valueCommentsOf(inner, into);
            }
        }
        return into;
    }

    function stmtsToIr(first, ctx) {
        var out = [];
        var blocks = chain(first);
        var i, s;
        for (i = 0; i < blocks.length; i++) {
            var b = blocks[i];

            /* A COMMENT SITS ABOVE THE THING IT IS ABOUT.
             *
             * This ran after the statement was pushed, so every comment landed
             * BELOW its block and the last one in a rule appeared to belong to
             * the closing brace. */
            var c = commentOf(b);
            if (c) { out.push({ k: 'comment', text: c }); }

            var vc = valueCommentsOf(b, []), vi;
            for (vi = 0; vi < vc.length; vi++) {
                out.push({ k: 'comment', text: 'on ' + vc[vi].on + ': ' + vc[vi].text });
            }

            if (isDisabledBlock(b)) {
                out.push({ k: 'comment',
                           text: 'disabled in the block editor: ' + (b.type || 'block') });
                continue;
            }

            s = stmtToIr(b, ctx);
            if (s) { out.push(s); }
        }
        return out;
    }

    function stmtToIr(b, ctx) {
        if (!b) { return null; }
        var t = b.type;
        var args;

        if (t === 'Wait') {
            return { k: 'wait', id: b.id, seconds: exprToIr(inputBlock(b, 'VALUE-0'), ctx) };
        }
        if (has(LOOP_CONTROL, t)) {
            return { k: 'control', id: b.id, word: LOOP_CONTROL[t], blockType: t };
        }
        if (t === 'SetVariable') {
            return {
                k: 'setVar', id: b.id,
                ref: exprToIr(inputBlock(b, 'VALUE-0'), ctx),
                value: exprToIr(inputBlock(b, 'VALUE-1'), ctx)
            };
        }
        if (t === 'SetVariableAtIndex') {
            return {
                k: 'setVarAt', id: b.id,
                ref: exprToIr(inputBlock(b, 'VALUE-0'), ctx),
                index: exprToIr(inputBlock(b, 'VALUE-1'), ctx),
                value: exprToIr(inputBlock(b, 'VALUE-2'), ctx)
            };
        }
        if (t === 'If') {
            return ifToIr(b, ctx);
        }
        if (t === 'ForVariable') {
            return {
                k: 'for', id: b.id,
                ref: exprToIr(inputBlock(b, 'VALUE-0'), ctx),
                from: exprToIr(inputBlock(b, 'VALUE-1'), ctx),
                to: exprToIr(inputBlock(b, 'VALUE-2'), ctx),
                step: exprToIr(inputBlock(b, 'VALUE-3'), ctx),
                body: stmtsToIr(inputBlock(b, 'DO'), ctx)
            };
        }
        if (t === 'While') {
            return {
                k: 'while', id: b.id,
                cond: exprToIr(inputBlock(b, 'VALUE-0'), ctx),
                body: stmtsToIr(inputBlock(b, 'DO'), ctx)
            };
        }
        if (t === 'subroutineInstanceBlock') {
            return { k: 'sub', id: b.id, name: subNameOf(b), args: subArgsToIr(b, ctx) };
        }
        /* Plain mod API statement. */
        args = argsToIr(b, ctx);
        return { k: 'call', id: b.id, fn: t, args: args };
    }

    function ifToIr(b, ctx) {
        var extra = b.extraState || {};
        var elseifCount = extra.elseif || 0;
        var branches = [];
        branches.push({
            cond: exprToIr(inputBlock(b, 'VALUE-0'), ctx),
            body: stmtsToIr(inputBlock(b, 'DO'), ctx)
        });
        var i;
        for (i = 1; i <= elseifCount; i++) {
            branches.push({
                cond: exprToIr(inputBlock(b, 'IF' + i), ctx),
                body: stmtsToIr(inputBlock(b, 'DO' + i), ctx)
            });
        }
        var elseBody = null;
        if (extra['else']) {
            elseBody = stmtsToIr(inputBlock(b, 'ELSE'), ctx);
        }
        return { k: 'if', id: b.id, branches: branches, elseBody: elseBody };
    }

    function subNameOf(b) {
        return (b.fields && b.fields.SUBROUTINE_NAME) ||
            (b.extraState && b.extraState.subroutineName) || 'Subroutine';
    }

    function subArgsToIr(b, ctx) {
        var out = [], i, e;
        for (i = 0; i < 16; i++) {
            if (!b.inputs || !has(b.inputs, 'PARAM-' + i)) { break; }
            e = exprToIr(inputBlock(b, 'PARAM-' + i), ctx);
            out.push(e);
        }
        return out;
    }

    function argsToIr(b, ctx) {
        var raw = valueInputs(b);
        var out = [], i;
        var lam = has(LAMBDA_ARG, b.type) ? LAMBDA_ARG[b.type] : -1;
        for (i = 0; i < raw.length; i++) {
            var e = exprToIr(raw[i], ctx);
            if (i === lam && e) { e = { k: 'lambda', body: e }; }
            out.push(e);
        }
        return out;
    }

    function exprToIr(b, ctx) {
        if (!b) { return null; }
        var t = b.type;
        var f = b.fields || {};

        if (t === 'Number') { return { k: 'num', v: Number(f.NUM || 0) }; }
        if (t === 'Text') { return { k: 'str', v: f.TEXT === undefined ? '' : String(f.TEXT) }; }
        if (t === 'Boolean') { return { k: 'bool', v: String(f.BOOL).toUpperCase() === 'TRUE' }; }
        if (t === 'CurrentArrayElement') { return { k: 'element' }; }

        if (/Item$/.test(t) && f['VALUE-0'] !== undefined && f['VALUE-1'] !== undefined) {
            return { k: 'enum', enumName: String(f['VALUE-0']), member: String(f['VALUE-1']), blockType: t };
        }

        if (t === 'variableReferenceBlock') {
            var vid = f.VAR && f.VAR.id ? f.VAR.id : null;
            var rec = vid && ctx.program.varById[vid] ? ctx.program.varById[vid] : null;
            if (!rec) {
                reportUnconvertible(ctx.report, 'workspace', 0,
                    'variableReferenceBlock pointing at an undeclared variable id ' + vid,
                    'Declare the variable in the workspace variable list.');
                rec = { ident: 'UnknownGlobalVar', name: 'Unknown', scope: f.OBJECTTYPE || 'Global', slot: 0 };
            }
            var isObj = !!(b.extraState && b.extraState.isObjectVar);
            return {
                k: 'varRef', name: rec.name, scope: rec.scope, ident: rec.ident, slot: rec.slot,
                object: isObj ? exprToIr(inputBlock(b, 'OBJECT'), ctx) : null
            };
        }

        if (t === 'GetVariable') {
            return { k: 'getVar', ref: exprToIr(inputBlock(b, 'VALUE-0'), ctx) };
        }

        if (t === 'subroutineArgumentBlock') {
            var idx = parseInt((f.ARGUMENT_INDEX === undefined ? '0' : f.ARGUMENT_INDEX), 10) || 0;
            var p = ctx.params[idx];
            return { k: 'arg', index: idx, name: p ? p.name : ('arg' + idx) };
        }

        if (t === 'subroutineInstanceBlock') {
            return { k: 'subCall', name: subNameOf(b), args: subArgsToIr(b, ctx) };
        }

        if (/^Event[A-Z]/.test(t) && (!b.inputs || keysOf(b.inputs).length === 0)) {
            return { k: 'event', name: 'event' + t.slice(5), blockType: t };
        }

        return { k: 'call', fn: t, args: argsToIr(b, ctx) };
    }

    /* ================================================================== *
     * 2. INTERMEDIATE MODEL -> TYPESCRIPT
     * ================================================================== */

    function containsWait(stmts) {
        var i, s;
        if (!stmts) { return false; }
        for (i = 0; i < stmts.length; i++) {
            s = stmts[i];
            if (!s) { continue; }
            if (s.k === 'wait') { return true; }
            if (s.k === 'call' && has(AWAITING, s.fn)) { return true; }
            if (s.k === 'if') {
                var j;
                for (j = 0; j < s.branches.length; j++) {
                    if (containsWait(s.branches[j].body)) { return true; }
                }
                if (containsWait(s.elseBody)) { return true; }
            }
            if ((s.k === 'for' || s.k === 'while') && containsWait(s.body)) { return true; }
        }
        return false;
    }

    /* --- expression emitter ------------------------------------------- */

    function emitExpr(e, st) {
        if (!e) { return 'undefined'; }
        switch (e.k) {
            case 'num': return numLit(e.v);
            case 'str': return jsonStr(e.v);
            case 'bool': return e.v ? 'true' : 'false';
            case 'enum': return 'mod.' + e.enumName + '.' + e.member;
            case 'element': return st.elementVar;
            case 'event': return 'eventInfo.' + e.name;
            case 'arg': return e.name;
            case 'raw': return e.text;
            case 'varRef':
                if (e.object) {
                    return 'mod.ObjectVariable(' + emitExpr(e.object, st) + ', ' + e.ident + ')';
                }
                return e.ident;
            case 'getVar':
                return 'mod.GetVariable(' + emitExpr(e.ref, st) + ')';
            case 'lambda':
                return '(' + st.elementVar + ': mod.Any) => ' + emitExpr(e.body, st);
            case 'gap':
                /* An empty socket on the way back out. It has to be something
                 * that compiles, and something nobody mistakes for a value. */
                return 'undefined /* left empty: the converter could not read this */'
            case 'subCall':
                return e.name === undefined ? 'undefined'
                    : sanitizeIdent(e.name) + '(' + subArgList(e, st) + ')';
            case 'call':
                return emitCall(e, st);
            default:
                return 'undefined';
        }
    }

    function subArgList(s, st) {
        var base = emitArgs(s.args, st);
        if (st.needySubs[sanitizeIdent(s.name)]) {
            return base ? base + ', eventInfo' : 'eventInfo';
        }
        return base;
    }

    function emitArgs(args, st) {
        var out = [], i;
        if (!args) { return ''; }
        for (i = 0; i < args.length; i++) { out.push(emitExpr(args[i], st)); }
        return out.join(', ');
    }

    function emitCall(e, st) {
        var fn = e.fn;
        if (fn === 'WaitUntil') {
            st.usedRuntime[fn] = 1;
            var condition = e.args[1];
            if (condition && condition.k === 'lambda') condition = condition.body;
            return 'rt.WaitUntil(' + emitExpr(e.args[0], st) + ', () => ' + emitExpr(condition, st) + ')';
        }
        /* Portal's And / Or take exactly two operands. Fold longer chains so
         * the generated TypeScript type checks. */
        if ((fn === 'And' || fn === 'Or') && e.args && e.args.length > 2) {
            var acc = e.args[0], i;
            for (i = 1; i < e.args.length; i++) {
                acc = { k: 'call', fn: fn, args: [acc, e.args[i]] };
            }
            return emitExpr(acc, st);
        }
        if (has(RUNTIME_ONLY, fn) || has(LAMBDA_ARG, fn)) {
            st.usedRuntime[fn] = 1;
            return 'rt.' + fn + '(' + emitArgs(e.args, st) + ')';
        }
        return 'mod.' + fn + '(' + emitArgs(e.args, st) + ')';
    }

    function loopVarName(ref, st) {
        if (ref && ref.k === 'varRef' && ref.name) {
            return sanitizeIdent(ref.name) + 'Var';
        }
        return 'iteratorVar' + (st.loopDepth ? String(st.loopDepth) : '');
    }

    function emitConditions(conds, st) {
        if (!conds || conds.length === 0) { return 'true'; }
        if (conds.length === 1) { return emitExpr(conds[0], st); }
        var acc = conds[0], i;
        for (i = 1; i < conds.length; i++) {
            acc = { k: 'call', fn: 'And', args: [acc, conds[i]] };
        }
        return emitExpr(acc, st);
    }

    /* --- statement emitter --------------------------------------------- */

    function emitStmts(stmts, st, depth, lines) {
        var i;
        if (!stmts || stmts.length === 0) { return; }
        for (i = 0; i < stmts.length; i++) {
            emitStmt(stmts[i], st, depth, lines);
        }
    }

    function emitStmt(s, st, depth, lines) {
        if (!s) { return; }
        var ind = pad(depth);
        var j, br;
        switch (s.k) {
            case 'comment':
                lines.push(ind + '// ' + String(s.text).replace(/\r?\n/g, ' '));
                return;
            case 'raw':
                lines.push(ind + s.text);
                return;
            case 'wait':
                lines.push(ind + 'await mod.Wait(' + emitExpr(s.seconds, st) + ');');
                return;
            case 'setVar':
                lines.push(ind + 'mod.SetVariable(' + emitExpr(s.ref, st) + ', ' + emitExpr(s.value, st) + ');');
                return;
            case 'setVarAt':
                lines.push(ind + 'mod.SetVariableAtIndex(' + emitExpr(s.ref, st) + ', ' +
                    emitExpr(s.index, st) + ', ' + emitExpr(s.value, st) + ');');
                return;
            case 'sub':
                lines.push(ind + (st.asyncSubs[sanitizeIdent(s.name)] ? 'void ' : '') +
                    sanitizeIdent(s.name) + '(' + subArgList(s, st) + ');');
                return;
            case 'control':
                lines.push(ind + s.word + ';');
                return;
            case 'call':
                lines.push(ind + (has(AWAITING, s.fn) ? 'await ' : '') + emitCall(s, st) + ';');
                return;
            case 'if':
                for (j = 0; j < s.branches.length; j++) {
                    br = s.branches[j];
                    lines.push(ind + (j === 0 ? 'if (' : '} else if (') + emitExpr(br.cond, st) + ') {');
                    emitStmts(br.body, st, depth + 1, lines);
                }
                if (s.elseBody) {
                    lines.push(ind + '} else {');
                    emitStmts(s.elseBody, st, depth + 1, lines);
                }
                lines.push(ind + '}');
                return;
            case 'for':
                var lv = loopVarName(s.ref, st);
                st.loopDepth++;
                lines.push(ind + 'for (let ' + lv + ' = ' + emitExpr(s.from, st) + '; ' +
                    lv + ' < ' + emitExpr(s.to, st) + '; ' + lv + ' += ' + emitExpr(s.step, st) + ') {');
                lines.push(pad(depth + 1) + 'mod.SetVariable(' + emitExpr(s.ref, st) + ', ' + lv + ');');
                emitStmts(s.body, st, depth + 1, lines);
                lines.push(ind + '}');
                st.loopDepth--;
                return;
            case 'while':
                lines.push(ind + 'while (' + emitExpr(s.cond, st) + ') {');
                emitStmts(s.body, st, depth + 1, lines);
                lines.push(ind + '}');
                return;
            default:
                lines.push(ind + '// unsupported statement kind: ' + s.k);
                return;
        }
    }

    function newEmitState(program) {
        var asyncSubs = {}, needySubs = {}, i;
        for (i = 0; i < program.subroutines.length; i++) {
            if (containsWait(program.subroutines[i].actions)) {
                asyncSubs[program.subroutines[i].fnName] = 1;
            }
            if (program.subroutines[i].needsEventInfo) {
                needySubs[program.subroutines[i].fnName] = 1;
            }
        }
        return {
            elementVar: 'currentArrayElement',
            loopDepth: 0,
            usedRuntime: {},
            asyncSubs: asyncSubs,
            needySubs: needySubs
        };
    }

    /* --- file writers --------------------------------------------------- */

    function metaComment(tag, obj) {
        return '// portal:' + tag + ' ' + JSON.stringify(obj);
    }

    function writeVariablesFile(program) {
        var L = [];
        L.push('// Variables converted from the Portal block workspace.');
        L.push('// Global variables address mod.GlobalVariable slots.');
        if (!program.variables.length) L.push('export {}; // Keep an empty variable table importable.');
        L.push('// Player and Team variables address mod.ObjectVariable slots on the owning object,');
        L.push('// so the exported constant is the slot index, used as mod.ObjectVariable(object, slot).');
        L.push('');
        var scopes = ['Global', 'Player', 'Team'], si, i, v;
        for (si = 0; si < scopes.length; si++) {
            var scope = scopes[si];
            var group = [];
            for (i = 0; i < program.variables.length; i++) {
                if (program.variables[i].scope === scope) { group.push(program.variables[i]); }
            }
            if (!group.length) { continue; }
            L.push('// ' + scope + ' scope (' + group.length + ')');
            for (i = 0; i < group.length; i++) {
                v = group[i];
                L.push(metaComment('var', { name: v.name, scope: v.scope, slot: v.slot }));
                if (scope === 'Global') {
                    L.push('export const ' + v.ident + ' = mod.GlobalVariable(' + v.slot + ');');
                } else {
                    L.push('export const ' + v.ident + ' = ' + v.slot + ';');
                }
            }
            L.push('');
        }
        /* Scopes the workspace invented beyond Global / Player / Team. */
        for (i = 0; i < program.variables.length; i++) {
            v = program.variables[i];
            if (scopes.join(',').indexOf(v.scope) === -1) {
                L.push('export const ' + v.ident + ' = ' + v.slot + '; // scope ' + v.scope);
            }
        }
        return L.join('\n') + '\n';
    }

    var RUNTIME_SOURCE = [
        '// Runtime helpers for a converted Portal block workspace.',
        '// Everything here mirrors behaviour the block editor provides implicitly.',
        '',
        '// Like the SDK modlib helper, re-evaluate the condition between waits.',
        '// The last interval is shortened so a fractional timeout is preserved.',
        'export async function WaitUntil(delay: number, condition: () => boolean): Promise<void> {',
        '    if (!Number.isFinite(delay) || delay < 0) throw new Error("WaitUntil needs a finite, nonnegative timeout");',
        '    const checks = Math.ceil(delay / 0.2);',
        '    for (let check = 0; check < checks && !condition(); check++) {',
        '        const interval = Math.min(0.2, delay - check * 0.2);',
        '        await mod.Wait(interval);',
        '    }',
        '}',
        '',
        '/**',
        ' * Edge triggered rule state. A Portal rule fires its actions on the tick its',
        ' * conditions become true, not on every tick they stay true.',
        ' */',
        'export class ConditionState {',
        '    private previous = false;',
        '',
        '    update(newState: boolean): boolean {',
        '        const fired = newState && !this.previous;',
        '        this.previous = newState;',
        '        return fired;',
        '    }',
        '}',
        '',
        'const ongoingStates = new Map<string, ConditionState>();',
        '',
        'function stateFor(key: string): ConditionState {',
        '    let state = ongoingStates.get(key);',
        '    if (state === undefined) {',
        '        state = new ConditionState();',
        '        ongoingStates.set(key, state);',
        '    }',
        '    return state;',
        '}',
        '',
        '/** Persistent state for an Ongoing Global rule. */',
        'export function getGlobalCondition(ruleIndex: number): ConditionState {',
        '    return stateFor(\'g:\' + ruleIndex);',
        '}',
        '',
        '/** Persistent state for an Ongoing rule scoped to one object. */',
        'export function getObjectCondition(object: mod.Any, ruleIndex: number): ConditionState {',
        '    return stateFor(ruleIndex + \':\' + mod.GetObjId(object));',
        '}',
        '',
        '/**',
        ' * State for an event rule. Event rule conditions are a plain guard evaluated',
        ' * once per event, so the state is fresh on every call and update() returns the',
        ' * condition itself.',
        ' */',
        'export function getEventCondition(): ConditionState {',
        '    return new ConditionState();',
        '}',
        '',
        '/** Number of elements in a Portal array. */',
        'function count(array: mod.Array): number {',
        '    return mod.CountOf(array);',
        '}',
        '',
        'export function FilteredArray(array: mod.Array, predicate: (element: mod.Any) => boolean): mod.Array {',
        '    let out = mod.EmptyArray();',
        '    const n = count(array);',
        '    for (let i = 0; i < n; i++) {',
        '        const element = mod.ValueInArray(array, i);',
        '        if (predicate(element)) {',
        '            out = mod.AppendToArray(out, element);',
        '        }',
        '    }',
        '    return out;',
        '}',
        '',
        'export function MappedArray(array: mod.Array, project: (element: mod.Any) => mod.Any): mod.Array {',
        '    let out = mod.EmptyArray();',
        '    const n = count(array);',
        '    for (let i = 0; i < n; i++) {',
        '        out = mod.AppendToArray(out, project(mod.ValueInArray(array, i)));',
        '    }',
        '    return out;',
        '}',
        '',
        'export function SortedArray(array: mod.Array, key: (element: mod.Any) => number): mod.Array {',
        '    const n = count(array);',
        '    const items: { element: mod.Any; key: number }[] = [];',
        '    for (let i = 0; i < n; i++) {',
        '        const element = mod.ValueInArray(array, i);',
        '        items.push({ element: element, key: key(element) });',
        '    }',
        '    items.sort((a, b) => a.key - b.key);',
        '    let out = mod.EmptyArray();',
        '    for (const item of items) {',
        '        out = mod.AppendToArray(out, item.element);',
        '    }',
        '    return out;',
        '}',
        '',
        'export function IsTrueForAll(array: mod.Array, predicate: (element: mod.Any) => boolean): boolean {',
        '    const n = count(array);',
        '    for (let i = 0; i < n; i++) {',
        '        if (!predicate(mod.ValueInArray(array, i))) {',
        '            return false;',
        '        }',
        '    }',
        '    return true;',
        '}',
        '',
        'export function IsTrueForAny(array: mod.Array, predicate: (element: mod.Any) => boolean): boolean {',
        '    const n = count(array);',
        '    for (let i = 0; i < n; i++) {',
        '        if (predicate(mod.ValueInArray(array, i))) {',
        '            return true;',
        '        }',
        '    }',
        '    return false;',
        '}',
        '',
        'export function IndexOfFirstTrue(array: mod.Array, predicate: (element: mod.Any) => boolean): number {',
        '    const n = count(array);',
        '    for (let i = 0; i < n; i++) {',
        '        if (predicate(mod.ValueInArray(array, i))) {',
        '            return i;',
        '        }',
        '    }',
        '    return -1;',
        '}',
        '',
        'export function ArrayContains(array: mod.Array, value: mod.Any): boolean {',
        '    return IsTrueForAny(array, (element) => mod.Equals(element, value));',
        '}',
        '',
        'export function IndexOfArrayValue(array: mod.Array, value: mod.Any): number {',
        '    return IndexOfFirstTrue(array, (element) => mod.Equals(element, value));',
        '}',
        '',
        'export function RemoveFromArray(array: mod.Array, value: mod.Any): mod.Array {',
        '    return FilteredArray(array, (element) => !mod.Equals(element, value));',
        '}',
        ''
    ].join('\n');

    function writeSubroutinesFile(program, st) {
        var L = [];
        var i, j, s;
        L.push('// Subroutines converted from the Portal block workspace (' +
            program.subroutines.length + ').');
        if (program.subroutines.length) {
            L.push('//');
            for (i = 0; i < program.subroutines.length; i++) {
                s = program.subroutines[i];
                L.push('//   ' + s.name + '(' + s.params.length + ' parameter' +
                    (s.params.length === 1 ? '' : 's') + ')');
            }
        }
        L.push('');
        L.push('import * as rt from \'./runtime.ts\';');
        L.push('import * as vars from \'./variables.ts\';');
        L.push('');
        for (i = 0; i < program.subroutines.length; i++) {
            s = program.subroutines[i];
            var isAsync = !!st.asyncSubs[s.fnName];
            L.push(metaComment('subroutine', {
                name: s.name, x: s.x, y: s.y,
                params: s.params
            }));
            if (s.comment) { L.push('// ' + s.comment); }
            /* A disabled SUBROUTINE is ambiguous in a way a disabled rule is
             * not: its callers still call it, so silently making it do nothing
             * would break them. The flag is surfaced and the behaviour left
             * alone, which is the honest reading of "switched off but still
             * called". */
            if (s.disabled) {
                L.push('// NOTE: this subroutine is disabled in the block editor, but it still ' +
                       'has callers, so it is exported as it stands.');
            }
            var ps = [];
            for (j = 0; j < s.params.length; j++) {
                ps.push(s.params[j].name + ': ' + tsTypeOf(s.params[j].type));
            }
            if (s.needsEventInfo) {
                /* Reads an event value, so it needs the calling rule's context. */
                ps.push('eventInfo: any');
            }
            L.push('export ' + (isAsync ? 'async ' : '') + 'function ' + s.fnName + '(' +
                ps.join(', ') + '): ' + (isAsync ? 'Promise<void>' : 'void') + ' {');
            if (s.conditions.length) {
                L.push(pad(1) + 'if (!(' + emitConditions(s.conditions, st) + ')) {');
                L.push(pad(2) + 'return;');
                L.push(pad(1) + '}');
                L.push('');
            }
            var body = [];
            emitStmts(s.actions, st, 1, body);
            L = L.concat(body);
            L.push('}');
            L.push('');
        }
        return prefixVarRefs(L.join('\n') + '\n', program);
    }

    function writeRuleGroupFile(familyId, rules, program, st) {
        var L = [], i, j, r;
        L.push('// Portal rules, group: ' + familyTitle(familyId) + ' (' + rules.length + ').');
        L.push('//');
        for (i = 0; i < rules.length; i++) {
            L.push('//   ' + rules[i].eventKey + '  ' + (rules[i].name || '(unnamed)'));
        }
        L.push('');
        L.push('import * as rt from \'./runtime.ts\';');
        L.push('import * as vars from \'./variables.ts\';');
        L.push('import * as subs from \'./subroutines.ts\';');
        L.push('');

        for (i = 0; i < rules.length; i++) {
            r = rules[i];
            var actionAsync = containsWait(r.actions);
            L.push('// Rule "' + (r.name || '(unnamed)') + '"  event ' + r.eventKey +
                (r.isOngoing ? '  (ongoing)' : ''));
            L.push(metaComment('rule', {
                name: r.name, event: r.event, objectType: r.objectType, index: r.index
            }));
            if (r.comment) { L.push('// ' + r.comment); }

            /* A rule switched off in the editor keeps its condition, which is
             * where the author's intent lives, and is simply never true. That
             * leaves the whole rule readable and editable in TypeScript while
             * matching what the workspace actually does. */
            if (r.disabled) {
                L.push('// DISABLED in the block editor. Delete this guard to turn it back on.');
            }

            /* condition */
            L.push('function ' + r.fnName + '_Condition(eventInfo: any): boolean {');
            if (r.disabled) {
                L.push(pad(1) + 'return false;   // disabled in the block editor');
            } else {
                L.push(pad(1) + 'return ' + emitConditions(r.conditions, st) + ';');
            }
            L.push('}');
            L.push('');

            /* action */
            L.push((actionAsync ? 'async ' : '') + 'function ' + r.fnName + '_Action(eventInfo: any): ' +
                (actionAsync ? 'Promise<void>' : 'void') + ' {');
            var body = [];
            emitStmts(r.actions, st, 1, body);
            L = L.concat(body);
            L.push('}');
            L.push('');

            /* dispatch glue */
            L.push('export function ' + r.fnName + '(conditionState: rt.ConditionState, eventInfo: any): void {');
            L.push(pad(1) + 'const newState = ' + r.fnName + '_Condition(eventInfo);');
            L.push(pad(1) + 'if (!conditionState.update(newState)) {');
            L.push(pad(2) + 'return;');
            L.push(pad(1) + '}');
            L.push(pad(1) + (actionAsync ? 'void ' : '') + r.fnName + '_Action(eventInfo);');
            L.push('}');
            L.push('');
        }
        var src = L.join('\n') + '\n';
        src = prefixSubRefs(src, program);
        return prefixVarRefs(src, program);
    }

    /* The emitters produce bare identifiers for variables and subroutines.
     * Namespace them so each generated file stays self contained. */
    function prefixVarRefs(src, program) {
        var i, v;
        for (i = 0; i < program.variables.length; i++) {
            v = program.variables[i];
            src = src.replace(new RegExp('(^|[^A-Za-z0-9_.])' + v.ident + '\\b', 'g'), '$1vars.' + v.ident);
        }
        return src;
    }

    function prefixSubRefs(src, program) {
        var i, s;
        for (i = 0; i < program.subroutines.length; i++) {
            s = program.subroutines[i];
            src = src.replace(new RegExp('(^|[^A-Za-z0-9_.])' + s.fnName + '\\(', 'g'), '$1subs.' + s.fnName + '(');
        }
        return src;
    }

    function writeIndexFile(program, groups, st, options) {
        var L = [], i, j;
        var totalRules = program.rules.length;
        L.push('// Battlefield Portal mod converted from a block workspace.');
        L.push('// Rules: ' + totalRules + '   Subroutines: ' + program.subroutines.length +
            '   Variables: ' + program.variables.length);
        L.push('//');
        L.push('// This file owns every Portal event handler export. Rule bodies live in the');
        L.push('// rules-*.ts files, one per event family, in the same order the block');
        L.push('// workspace lists them.');
        L.push('');
        L.push('import * as rt from \'./runtime.ts\';');
        var gids = keysOf(groups);
        gids.sort();
        for (i = 0; i < gids.length; i++) {
            L.push('import * as g_' + gids[i].replace(/-/g, '_') + ' from \'./rules-' + gids[i] + '.ts\';');
        }
        L.push('');

        /* group rules by event key, preserving workspace order */
        var byEvent = {}, order = [];
        for (i = 0; i < program.rules.length; i++) {
            var r = program.rules[i];
            if (!has(byEvent, r.eventKey)) { byEvent[r.eventKey] = []; order.push(r.eventKey); }
            byEvent[r.eventKey].push(r);
        }

        var base = 0;
        for (i = 0; i < order.length; i++) {
            var key = order[i];
            var list = byEvent[key];
            var sig = EVENTS[key] ? EVENTS[key].params : null;
            var params = [], names = [];
            if (sig) {
                for (j = 0; j < sig.length; j++) {
                    params.push(sig[j].name + ': ' + sig[j].type);
                    names.push(sig[j].name);
                }
            } else if (/^Ongoing/.test(key) && key !== 'OngoingGlobal') {
                var objType = key.slice(7);
                params.push('event' + objType + ': mod.' + objType);
                names.push('event' + objType);
            }

            L.push('/** ' + list.length + ' rule' + (list.length === 1 ? '' : 's') + ': ' +
                ruleNameList(list) + ' */');
            L.push('export function ' + key + '(' + params.join(', ') + '): void {');
            L.push(pad(1) + 'const eventInfo: any = ' +
                (names.length ? '{ ' + eventInfoLiteral(names) + ' }' : '{}') + ';');
            var ongoing = /^Ongoing/.test(key);
            for (j = 0; j < list.length; j++) {
                var g = 'g_' + familyOf(key).replace(/-/g, '_');
                var stateExpr;
                if (ongoing && key === 'OngoingGlobal') {
                    stateExpr = 'rt.getGlobalCondition(' + (base + j) + ')';
                } else if (ongoing) {
                    stateExpr = 'rt.getObjectCondition(' + names[0] + ', ' + (base + j) + ')';
                } else {
                    stateExpr = 'rt.getEventCondition()';
                }
                L.push(pad(1) + g + '.' + list[j].fnName + '(' + stateExpr + ', eventInfo);');
            }
            L.push('}');
            L.push('');
            base += list.length;
        }
        return L.join('\n') + '\n';
    }

    function eventInfoLiteral(names) {
        var out = [], i;
        for (i = 0; i < names.length; i++) { out.push(names[i] + ': ' + names[i]); }
        return out.join(', ');
    }

    function ruleNameList(list) {
        var out = [], i;
        for (i = 0; i < list.length; i++) { out.push(list[i].name || '(unnamed)'); }
        return out.join(', ');
    }

    function buildStringsTable(program) {
        /* The site interns every string token the script mentions, in order of
         * first appearance, as s0..sN. Reproduced here for parity. */
        var table = {}, seen = {}, n = 0;
        function add(s) {
            if (typeof s !== 'string') { return; }
            if (has(seen, s)) { return; }
            seen[s] = 1;
            table['s' + n] = s;
            n++;
        }
        function walkExpr(e) {
            if (!e) { return; }
            var i;
            if (e.k === 'str') { add(e.v); }
            else if (e.k === 'enum') { add(e.enumName); add(e.member); }
            else if (e.k === 'subCall') { add(e.name); if (e.args) { for (i = 0; i < e.args.length; i++) { walkExpr(e.args[i]); } } }
            else if (e.k === 'lambda') { walkExpr(e.body); }
            else if (e.k === 'getVar') { walkExpr(e.ref); }
            else if (e.k === 'varRef') { walkExpr(e.object); }
            else if (e.k === 'call' && e.args) { for (i = 0; i < e.args.length; i++) { walkExpr(e.args[i]); } }
        }
        function walkStmts(list) {
            var i, s, j;
            if (!list) { return; }
            for (i = 0; i < list.length; i++) {
                s = list[i];
                if (!s) { continue; }
                if (s.k === 'sub') { add(s.name); }
                if (s.k === 'call' || s.k === 'sub') { if (s.args) { for (j = 0; j < s.args.length; j++) { walkExpr(s.args[j]); } } }
                if (s.k === 'wait') { walkExpr(s.seconds); }
                if (s.k === 'setVar') { walkExpr(s.ref); walkExpr(s.value); }
                if (s.k === 'setVarAt') { walkExpr(s.ref); walkExpr(s.index); walkExpr(s.value); }
                if (s.k === 'if') {
                    for (j = 0; j < s.branches.length; j++) { walkExpr(s.branches[j].cond); walkStmts(s.branches[j].body); }
                    walkStmts(s.elseBody);
                }
                if (s.k === 'for') { walkExpr(s.ref); walkExpr(s.from); walkExpr(s.to); walkExpr(s.step); walkStmts(s.body); }
                if (s.k === 'while') { walkExpr(s.cond); walkStmts(s.body); }
            }
        }
        var i, j;
        for (i = 0; i < program.rules.length; i++) {
            for (j = 0; j < program.rules[i].conditions.length; j++) { walkExpr(program.rules[i].conditions[j]); }
            walkStmts(program.rules[i].actions);
        }
        for (i = 0; i < program.subroutines.length; i++) {
            for (j = 0; j < program.subroutines[i].conditions.length; j++) { walkExpr(program.subroutines[i].conditions[j]); }
            walkStmts(program.subroutines[i].actions);
        }
        return table;
    }

    /* --- public: blocksToTs -------------------------------------------- */

    function blocksToTs(workspace, options) {
        options = options || {};
        var report = newReport('blocksToTs');
        var program = blocksToIr(workspace, report);
        var st = newEmitState(program);

        /* group rules by family, keeping workspace order inside a group */
        var groups = {}, i, fam;
        for (i = 0; i < program.rules.length; i++) {
            fam = familyOf(program.rules[i].eventKey);
            if (!has(groups, fam)) { groups[fam] = []; }
            groups[fam].push(program.rules[i]);
        }

        var files = {};
        files['variables.ts'] = writeVariablesFile(program);
        files['runtime.ts'] = RUNTIME_SOURCE;
        files['subroutines.ts'] = writeSubroutinesFile(program, st);
        var gids = keysOf(groups);
        gids.sort();
        for (i = 0; i < gids.length; i++) {
            files['rules-' + gids[i] + '.ts'] = writeRuleGroupFile(gids[i], groups[gids[i]], program, st);
        }
        files['index.ts'] = writeIndexFile(program, groups, st, options);

        if (options.strings !== false) {
            files['strings.json'] = JSON.stringify(buildStringsTable(program), null, 2) + '\n';
        }

        report.counts.files = keysOf(files).length;
        report.counts.groups = gids.length;
        report.groups = {};
        for (i = 0; i < gids.length; i++) { report.groups[gids[i]] = groups[gids[i]].length; }

        return { files: files, report: report, program: program };
    }

    /* ================================================================== *
     * 3. TYPESCRIPT -> INTERMEDIATE MODEL
     * ================================================================== */

    function requireTs() {
        if (!TS) {
            throw new Error('The TypeScript compiler API is not available. ' +
                'Call BF6Convert.setTypeScript(require("typescript")) first.');
        }
        return TS;
    }

    function tsToIr(sources, options) {
        options = options || {};
        var ts = requireTs();
        TS_HANDLE = ts;
        var report = newReport('tsToBlocks');

        var program = {
            variables: [], varByIdent: {}, rules: [], subroutines: [], orphans: [],
            layout: { mod: null }
        };

        var files = normaliseSources(sources);

        /* A FOLDER IS NOT A PROJECT. CONVERT WHAT THE MOD ACTUALLY SHIPS.
         *
         * Handed a source folder this converted every .ts file in it, which is
         * not what the build does: the build starts at the entry point and
         * follows imports. Real projects accumulate files that nothing imports
         * - undead-ai.backup.ts, a _backup_targeting_v3.7 folder holding older
         * copies of three systems - and converting those produced duplicate
         * rules, duplicate subroutines and, worse, duplicate global variables
         * competing for a budget of 128.
         *
         * On one project that was 7 files, 2,176 blocks and 22 Global
         * variables spent on code that has not run since it was superseded.
         * The dropped files are reported by name, because a file being
         * unreachable is sometimes the bug rather than the intent.
         */
        var shipOrder = null;
        (function keepOnlyWhatShips() {
            var names = keysOf(files), entry = null, n;
            if (names.length < 2) { return; }
            var prefer = ['src/index.ts', 'index.ts', 'src/main.ts', 'main.ts'];
            for (n = 0; n < prefer.length; n++) {
                if (has(files, prefer[n])) { entry = prefer[n]; break; }
            }
            if (!entry) { return; }

            /* ASK THE PARSER, NOT A REGULAR EXPRESSION.
             *
             * This first shipped as a regex anchored to a line start, which
             * quietly dropped the second of two imports written on one line
             * and would have mistaken a matching string inside a comment or a
             * template literal for a dependency. Dropping a dependency here is
             * silent and severe: the file is excluded from the conversion
             * entirely, so its rules and variables simply are not there.
             *
             * TypeScript is already loaded and every file is about to be
             * parsed anyway, so the import list comes from the AST, which is
             * the only thing that actually knows.
             */
            function specifiersOf(text, name) {
                var out = [];
                var sf;
                try {
                    sf = ts.createSourceFile(name, text, ts.ScriptTarget.ES2022, true);
                } catch (e) { return out; }
                (function visit(node) {
                    if (!node) { return; }
                    var isImport = (node.kind === ts.SyntaxKind.ImportDeclaration ||
                                    node.kind === ts.SyntaxKind.ExportDeclaration);
                    if (isImport && node.moduleSpecifier &&
                        typeof node.moduleSpecifier.text === 'string') {
                        out.push(node.moduleSpecifier.text);
                    }
                    /* import('./x.ts') resolves the same file and is a real
                     * dependency even though it is an expression. */
                    if (node.kind === ts.SyntaxKind.CallExpression && node.expression &&
                        node.expression.kind === ts.SyntaxKind.ImportKeyword &&
                        node.arguments && node.arguments.length &&
                        typeof node.arguments[0].text === 'string') {
                        out.push(node.arguments[0].text);
                    }
                    ts.forEachChild(node, visit);
                }(sf));
                return out;
            }

            function dirOf(p) {
                var at = p.lastIndexOf('/');
                return at < 0 ? '' : p.slice(0, at);
            }
            function joinPath(dir, rel) {
                var parts = (dir ? dir.split('/') : []).concat(rel.split('/')), out = [], i2;
                for (i2 = 0; i2 < parts.length; i2++) {
                    var seg = parts[i2];
                    if (seg === '' || seg === '.') { continue; }
                    if (seg === '..') { out.pop(); continue; }
                    out.push(seg);
                }
                return out.join('/');
            }

            function resolve(from, spec) {
                /* A bare specifier is a library, and libraries are not in this
                 * file map. */
                if (spec.charAt(0) !== '.') { return null; }
                var base = joinPath(dirOf(from), spec);
                var cands = [base, base.replace(/\.js$/, '.ts'), base + '.ts',
                             base + '/index.ts'], c;
                for (c = 0; c < cands.length; c++) {
                    if (has(files, cands[c])) { return cands[c]; }
                }
                return null;
            }

            /* DEPTH FIRST, DEPENDENCIES FIRST.
             *
             * This walked breadth first and the result was then thrown away:
             * the file list was sorted ALPHABETICALLY before anything read it,
             * so module initialisers ran in filename order. A file defining
             * `base = 7` and a file computing `result = base + 1` gave 8 when
             * run and 1 when converted, purely because a sorts before z.
             *
             * Post-order from the entry point is the order the modules
             * actually evaluate in: everything a file imports is finished
             * before the file itself. A cycle stops at the file already on the
             * stack, which is the same thing the module system does.
             */
            var live = {}, order = [], onStack = {};
            (function visit(f) {
                if (has(live, f) || has(onStack, f) || !has(files, f)) { return; }
                onStack[f] = true;
                var specs = specifiersOf(files[f], f), si;
                for (si = 0; si < specs.length; si++) {
                    var dep = resolve(f, specs[si]);
                    if (dep) { visit(dep); }
                }
                delete onStack[f];
                live[f] = true;
                order.push(f);
            }(entry));
            shipOrder = order;

            var dropped = [];
            for (n = 0; n < names.length; n++) {
                if (!has(live, names[n])) { dropped.push(names[n]); delete files[names[n]]; }
            }
            if (!dropped.length) { return; }
            report.counts.filesNotImported = dropped.length;
            report.warnings.push('left out ' + dropped.length + ' file(s) that ' + entry +
                ' never imports, directly or indirectly: ' + dropped.join(', ') +
                '. They would have added duplicate rules and variables. Import them ' +
                'from the entry point if they are meant to run.');
        }());

        /* OUR OWN RUNTIME IS NOT SOMEBODY'S RULES.
         *
         * blocksToTs writes runtime.ts from the fixed RUNTIME_SOURCE below: a
         * class and a handful of helpers that mirror what the block editor
         * does implicitly. Handed a whole exported folder, this used to parse
         * that file like any other and report 57 things the blocks cannot say
         * - loops, returns, local variables - none of which came from the
         * user, plus 14 helper functions counted as subroutines.
         *
         * It is skipped by CONTENT, not by name, so a project that happens to
         * keep its own runtime.ts full of real rules is still read.
         */
        (function dropGeneratedRuntime() {
            /* Built from a char code: a backslash written into this file by
             * tooling has been eaten six times in one session, and a broken
             * character class here is a comparison that quietly stops
             * matching. */
            var FLAT = new RegExp('[' + String.fromCharCode(92) + 's]+', 'g');
            var flatten = function (s) { return String(s || '').replace(FLAT, ' ').trim(); };
            var ours = flatten(RUNTIME_SOURCE);
            var all = keysOf(files), i, body;
            for (i = 0; i < all.length; i++) {
                body = flatten(files[all[i]]);
                if (body && body === ours) { delete files[all[i]]; }
            }
        })();

        /* Dependency order when we could work it out, filename order when we
         * could not. Sorting was never meaningful; it was just deterministic,
         * and it silently reordered module initialisation. */
        var names;
        if (shipOrder && shipOrder.length) {
            /* The order was worked out before our own generated runtime.ts was
             * dropped just above, so a name in it may no longer be a file.
             * Reading one gives the compiler undefined source and it throws
             * inside createSourceFile, a long way from the cause. */
            names = [];
            for (i = 0; i < shipOrder.length; i++) {
                if (has(files, shipOrder[i])) { names.push(shipOrder[i]); }
            }
            /* Anything that arrived after the order was taken still belongs. */
            var known = {}, an;
            for (i = 0; i < names.length; i++) { known[names[i]] = 1; }
            var allNow = keysOf(files);
            for (an = 0; an < allNow.length; an++) {
                if (!has(known, allNow[an])) { names.push(allNow[an]); }
            }
        } else {
            names = keysOf(files);
            names.sort();
        }

        /* Pass 1: collect declarations across every file. */
        var fnBodies = {};      /* fnName -> {node, file, params} */
        var dispatchers = [];   /* {event, calls:[fnName], file} */
        var metaByFn = {};      /* fnName -> portal:rule metadata */
        var subMeta = {};       /* subName -> portal:subroutine metadata */
        var varMeta = {};       /* ident -> portal:var metadata */
        var i, fi;

        var asts = {};
        for (fi = 0; fi < names.length; fi++) {
            var fname = names[fi];
            var text = files[fname];
            var sf = ts.createSourceFile(fname, text, ts.ScriptTarget.ES2020, true, ts.ScriptKind.TS);
            asts[fname] = sf;
            collectMeta(ts, sf, text, metaByFn, subMeta, varMeta, report);
        }

        /* Which module constants are pure data has to be settled before the
         * scan, because the scan turns every other module binding into a
         * workspace variable and these must not become one. */
        collectConstRecords(ts, names, asts, program);
        collectSourceMarkers(files, names, program);
        collectModAliases(ts, names, asts, program);
        collectCollections(ts, names, asts, program);
        collectRecordTables(ts, names, asts, program);
        collectObjectTables(ts, names, asts, program);
        collectFieldSlots(ts, names, asts, program);

        /* Component classes are read before the scan too, for the same reason:
         * the scan reports a `namespace UI { class Text ... }` as something
         * with no block form, and it only is when its constructors could not
         * be read. */
        collectUiComponents(ts, names, asts, program);

        ctx_varMeta = varMeta;
        for (fi = 0; fi < names.length; fi++) {
            scanTopLevel(ts, asts[names[fi]], names[fi], files[names[fi]],
                program, fnBodies, dispatchers, report, options);
        }


        /* Pass 2: build rules from dispatchers, or from the *_Action naming
         * convention when a file has no dispatchers (hand written source). */
        buildRules(ts, program, fnBodies, dispatchers, metaByFn, report);

        /* Pass 3: everything left that looks like a subroutine. */
        buildSubroutines(ts, program, fnBodies, subMeta, report);

        /* SLOTS AND PACKING RUN LAST, AFTER EVERY BODY EXISTS.
         *
         * Both rewrite program.variables and varByIdent wholesale. Run before
         * buildSubroutines they were, and every subroutine body parsed after
         * them looked up identifiers in a table those passes had already
         * emptied - so each one minted a fresh variable and reported the name
         * it could no longer find as unconvertible. 545 invented failures, and
         * the locals of 438 subroutines left unpooled and unpacked.
         *
         * Anything that rewrites the variable table has to be the last thing
         * that runs.
         */
        /* MODULE INITIALISERS BEFORE ALLOCATION, NOT AFTER.
         *
         * They are statements like any other and they can contain a subroutine
         * call used as a value, so the sweep below has to see them. Running
         * after allocation also meant any variable they touched was looked up
         * in a table the allocator had already rewritten. */
        emitModuleInit(program, report);

        /* Now that every body exists, guarantee the one thing that makes a
         * workspace unloadable is gone from all of them. */
        ensureNoValueSubCalls(program, report);
        ensureBooleanSockets(program, report);
        /* Every socket checked against what the editor will actually accept, so a
         * type clash becomes a visible hole instead of a file that will not open. */
        ensureSocketTypes(program, report);

        allocateSlots(program, report);

        /* The hoisted UI colour table, for the same reason and in the same
         * place: it is a module constant with no module load phase to build it
         * in, and packing has to see the reads. */
        emitUiInit(program, report);

        /* PACKING IS FOR PORTAL, NOT FOR READING.
         *
         * Portal allows about 128 variables per scope, so a large project has
         * to fold many of them into shared arrays: zombieHealth stops being a
         * name and becomes ValueInArray(pack1, 7). That is necessary to deploy
         * and ruinous to read, and it was being done unconditionally.
         *
         * The editor is not Portal. Blockly holds 250 variables without
         * complaint, and someone opening a converted project is trying to
         * understand it, not upload it. So packing is off unless it is asked
         * for, the budget warning still tells the truth, and the workspace can
         * be packed on the way out when it actually matters.
         */
        if (options && options.packVariables) {
            packGlobals(program, report, VARIABLE_CEILING);
        }
        /* The object scope has its own budget of the same size, shared by Player
         * and Team, and moving per object state into it can fill it. Packed the
         * same way, and after the Global pass so it sees the final variable
         * table. */
        if (options && options.packVariables) {
            packObjectVars(program, report, VARIABLE_CEILING);
        }

        /* COUNTED LAST, OR IT REPORTS A PROBLEM THAT HAS ALREADY BEEN FIXED.
         *
         * Portal refuses a workspace past a few dozen variables per scope: its
         * own manager renders GLOBAL (n / max) and says so. This check used to
         * run before pooling and packing and reported 531 globals on a project
         * that finishes with far fewer - a warning about work already done is
         * worse than none, because it teaches you to ignore it.
         */
        (function checkVariableBudget() {
            var byScope = {}, i, v, worst = 0;
            for (i = 0; i < program.variables.length; i++) {
                v = program.variables[i];
                byScope[v.scope] = (byScope[v.scope] || 0) + 1;
            }
            var parts = [], scopes = keysOf(byScope);
            for (i = 0; i < scopes.length; i++) {
                parts.push(scopes[i] + ' ' + byScope[scopes[i]]);
                if (byScope[scopes[i]] > worst) { worst = byScope[scopes[i]]; }
            }
            report.counts.variablesByScope = parts.join(', ');
            if (worst > VARIABLE_CEILING) {
                warn(report, 'VARIABLE BUDGET: ' + parts.join(', ') + ' after pooling and ' +
                    'packing. Portal allows about ' + VARIABLE_CEILING + ' per scope, so this ' +
                    'workspace will still be refused.');
            }
        })();

        report.counts.rules = program.rules.length;
        report.counts.subroutines = program.subroutines.length;
        report.counts.variables = program.variables.length;
        return { program: program, report: report };
    }

    function normaliseSources(sources) {
        var files = {};
        if (typeof sources === 'string') { files['input.ts'] = sources; return files; }
        var k;
        for (k in sources) { if (has(sources, k)) { files[k] = sources[k]; } }
        return files;
    }

    function collectMeta(ts, sf, text, metaByFn, subMeta, varMeta, report) {
        /* portal:rule / portal:subroutine / portal:var comments attach to the
         * next declaration. Scan the raw text: cheaper and order preserving. */
        var lines = text.split(/\r?\n/);
        var pending = null, i, m;
        for (i = 0; i < lines.length; i++) {
            var line = lines[i];
            m = /^\s*\/\/\s*portal:(rule|subroutine|var)\s+(\{.*\})\s*$/.exec(line);
            if (m) {
                try { pending = { tag: m[1], data: JSON.parse(m[2]) }; } catch (e) { pending = null; }
                continue;
            }
            if (!pending) { continue; }
            var d = /function\s+([A-Za-z0-9_]+)\s*\(/.exec(line);
            if (d) {
                if (pending.tag === 'rule') { metaByFn[d[1]] = pending.data; }
                else if (pending.tag === 'subroutine') { subMeta[d[1]] = pending.data; }
                pending = null;
                continue;
            }
            var c = /const\s+([A-Za-z0-9_]+)\s*=/.exec(line);
            if (c) {
                if (pending.tag === 'var') { varMeta[c[1]] = pending.data; }
                pending = null;
                continue;
            }
            if (!/^\s*(\/\/|$)/.test(line)) { pending = null; }
        }
        var k;
        for (k in varMeta) {
            if (has(varMeta, k)) { metaByFn['$var$' + k] = varMeta[k]; }
        }
    }

    function lineOf(ts, sf, node) {
        try { return sf.getLineAndCharacterOfPosition(node.getStart(sf)).line + 1; }
        catch (e) { return 0; }
    }

    /* Data written out in full: numbers, strings, booleans, and objects or
     * arrays built only from those. No calls, no names, nothing computed. */
    function isLiteralData(ts, e) {
        if (!e) { return false; }
        if (ts.isNumericLiteral(e) || ts.isStringLiteral(e) ||
            ts.isNoSubstitutionTemplateLiteral(e) ||
            e.kind === ts.SyntaxKind.TrueKeyword ||
            e.kind === ts.SyntaxKind.FalseKeyword) {
            return true;
        }
        if (ts.isPrefixUnaryExpression(e) && e.operator === ts.SyntaxKind.MinusToken) {
            return isLiteralData(ts, e.operand);
        }
        if (ts.isObjectLiteralExpression(e)) {
            for (var i = 0; i < e.properties.length; i++) {
                var p = e.properties[i];
                if (!ts.isPropertyAssignment(p) || !p.name || !ts.isIdentifier(p.name) ||
                    !isLiteralData(ts, p.initializer)) {
                    return false;
                }
            }
            return true;
        }
        if (ts.isArrayLiteralExpression(e)) {
            for (var j = 0; j < e.elements.length; j++) {
                if (!isLiteralData(ts, e.elements[j])) { return false; }
            }
            return true;
        }
        return false;
    }

    /* A CONFIG OBJECT IS NOT STATE, IT IS A LIST OF NUMBERS.
     *
     * `const GAMEMODE_CONFIG = { score: 75, team1ID: 1, ... }` was becoming a
     * workspace variable holding an object the blocks cannot represent, and then
     * every `GAMEMODE_CONFIG.team1ID` was reported as an unconvertible property
     * access - about sixty of them across three objects in one project, none of
     * which is real work. The value is known while converting, so the access is
     * replaced by what it reads: `GAMEMODE_CONFIG.team1ID` becomes the number 1.
     * That also gives back the variable slots the objects were holding.
     *
     * Only for objects nothing ever writes to. A candidate is dropped the moment
     * the name is assigned through, or escapes anywhere the converter cannot
     * see - passed to a function, aliased to another name - because then this
     * would be inlining a value something else may have changed.
     */
    function collectConstRecords(ts, names, asts, program) {
        var cand = {}, fi, i, j;

        for (fi = 0; fi < names.length; fi++) {
            var sf = asts[names[fi]];
            for (i = 0; i < sf.statements.length; i++) {
                var st = sf.statements[i];
                if (!ts.isVariableStatement(st)) { continue; }
                var isConst = !!(st.declarationList.flags & ts.NodeFlags.Const);
                if (!isConst) { continue; }
                var ds = st.declarationList.declarations;
                for (j = 0; j < ds.length; j++) {
                    var d = ds[j];
                    if (!ts.isIdentifier(d.name) || !d.initializer) { continue; }
                    if (!ts.isObjectLiteralExpression(d.initializer)) { continue; }
                    if (!isLiteralData(ts, d.initializer)) { continue; }
                    cand[d.name.text] = { node: d.initializer, file: names[fi], sf: sf };
                }
            }
        }

        /* Now disqualify. Every mention of the name anywhere, in any file. */
        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }
                if (ts.isIdentifier(n) && has(cand, n.text)) {
                    var p = n.parent;
                    var reading = p && (
                        (ts.isPropertyAccessExpression(p) && p.expression === n) ||
                        (ts.isElementAccessExpression(p) && p.expression === n) ||
                        (ts.isVariableDeclaration(p) && p.name === n));
                    if (!reading) { delete cand[n.text]; }
                }
                /* CONFIG.field = x, and CONFIG.field++ */
                if (ts.isBinaryExpression(n) && n.operatorToken &&
                    n.operatorToken.kind >= ts.SyntaxKind.FirstAssignment &&
                    n.operatorToken.kind <= ts.SyntaxKind.LastAssignment) {
                    var root = n.left;
                    while (root && (ts.isPropertyAccessExpression(root) || ts.isElementAccessExpression(root))) {
                        root = root.expression;
                    }
                    if (root && ts.isIdentifier(root) && has(cand, root.text)) { delete cand[root.text]; }
                }
                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }

        program.constRecords = cand;
    }

    /* AN OBJECT IS A HANDFUL OF NAMED SLOTS. SO IS AN ARRAY.
     *
     * Portal has arrays and no objects, so `{ k: 0, d: 0, a: 0, hs: 0 }` was
     * dropped and every later `stats.k` reported. In TDM that one gap emptied
     * two whole phases: the mod stored nothing on join, then threw reading the
     * slot it never wrote.
     *
     * A record becomes an array and a field name becomes a fixed index into it,
     * so `stats.k` is `ValueInArray(stats, 0)`. The index has to mean the same
     * thing everywhere, because a function taking a record does not know which
     * literal built it.
     *
     * Giving every field name in the program its own index would work and would
     * make each record as wide as the program has field names. Only fields that
     * appear in the SAME object need to differ, so this is the interference
     * problem the variable allocator already solves: fields sharing a literal
     * are neighbours, and the colouring hands back indices no wider than the
     * biggest record.
     */
    function collectFieldSlots(ts, names, asts, program) {
        var shapes = [], seen = {}, fi;

        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }
                if (ts.isObjectLiteralExpression(n)) {
                    var keys = [], ok = true, i;
                    for (i = 0; i < n.properties.length; i++) {
                        var p = n.properties[i];
                        if (ts.isShorthandPropertyAssignment(p) && p.name && ts.isIdentifier(p.name)) {
                            keys.push(p.name.text);
                        } else if (ts.isPropertyAssignment(p) && p.name && ts.isIdentifier(p.name)) {
                            keys.push(p.name.text);
                        } else {
                            /* A spread or a computed key has no fixed shape, so
                             * this literal is not treated as a record at all. */
                            ok = false;
                        }
                    }
                    if (ok && keys.length) {
                        shapes.push(keys);
                        for (i = 0; i < keys.length; i++) { seen[keys[i]] = true; }
                    }
                }
                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }

        /* Who cannot share an index with whom. */
        var conflict = {}, k;
        for (k in seen) { if (has(seen, k)) { conflict[k] = {}; } }
        for (fi = 0; fi < shapes.length; fi++) {
            var s = shapes[fi];
            for (var a = 0; a < s.length; a++) {
                for (var b = 0; b < s.length; b++) {
                    if (s[a] !== s[b]) { conflict[s[a]][s[b]] = true; }
                }
            }
        }

        /* Most crowded field first, so the common ones take the low indices and
         * the records that hold them stay short. */
        var order = Object.keys(conflict).sort(function (x, y) {
            var d = Object.keys(conflict[y]).length - Object.keys(conflict[x]).length;
            return d !== 0 ? d : (x < y ? -1 : 1);
        });

        var slots = {};
        for (fi = 0; fi < order.length; fi++) {
            var name = order[fi], taken = {}, nb;
            for (nb in conflict[name]) {
                if (has(conflict[name], nb) && has(slots, nb)) { taken[slots[nb]] = true; }
            }
            var idx = 0;
            while (taken[idx]) { idx++; }
            slots[name] = idx;
        }
        program.fieldSlots = slots;
    }

    /* A TABLE ADDRESSED ONLY BY A PLAYER IS PER PLAYER STORAGE.
     *
     * `playerNearDebris[playerId] = true`, `repairTorchUIWidgets[playerId]`,
     * `playerPrimaryWeapon[playerId]` - a plain array used as a dictionary from
     * player to one value. Each one was a Global array variable, and one real
     * mod has about forty of them, which is a third of the entire Global budget
     * spent on state Portal already knows how to keep per player.
     *
     * These become one object slot each, so they leave the Global scope
     * entirely and land in the object scope, which packs.
     *
     * The decision has to be made for the WHOLE table, not per site. If one
     * place reads `x[i]` with a loop counter, the table is a real array and
     * moving it would turn that read into a lookup on the wrong object. So a
     * table qualifies only when every single index across the program looks
     * like an object id, and it is never iterated, measured, appended to, or
     * used as a value in its own right.
     */
    function collectObjectTables(ts, names, asts, program) {
        var cand = {}, banned = {}, fi;

        function looksLikeObjectKey(a) {
            if (!a) { return false; }
            if (ts.isCallExpression(a) && ts.isPropertyAccessExpression(a.expression) &&
                a.expression.name.text === 'GetObjId') { return true; }
            return ts.isIdentifier(a) && /id$/i.test(a.text);
        }

        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }

                if (ts.isElementAccessExpression(n) && ts.isIdentifier(n.expression)) {
                    if (looksLikeObjectKey(n.argumentExpression)) { cand[n.expression.text] = 1; }
                    else { banned[n.expression.text] = 1; }
                }

                /* Anything that treats the name as an array in its own right. */
                if (ts.isPropertyAccessExpression(n) && ts.isIdentifier(n.expression)) {
                    banned[n.expression.text] = 1;
                }
                if (ts.isForOfStatement(n) && ts.isIdentifier(n.expression)) {
                    banned[n.expression.text] = 1;
                }

                /* Passed somewhere whole, or assigned to another name: the value
                 * would have to be an array and there is no array any more. */
                if (ts.isCallExpression(n)) {
                    for (var ai = 0; ai < (n.arguments || []).length; ai++) {
                        var a = n.arguments[ai];
                        if (ts.isIdentifier(a)) { banned[a.text] = 1; }
                    }
                }
                if (ts.isBinaryExpression(n) && ts.isIdentifier(n.right)) { banned[n.right.text] = 1; }
                if (ts.isVariableDeclaration(n) && n.initializer && ts.isIdentifier(n.initializer)) {
                    banned[n.initializer.text] = 1;
                }

                /* A NAME BUILT FROM AN ARRAY LITERAL IS AN ARRAY.
                 *
                 * The test above is "indexed by something ending in id", which
                 * is true of an ordinary lookup table just as much as a per
                 * player record:
                 *
                 *     const BONUS = [0, 10, 20];
                 *     const cash  = START_CASH + BONUS[id];
                 *
                 * BONUS became a Player scoped variable, its initialiser was
                 * dropped because a per object slot has no module init, and
                 * every read returned nothing. The declaration settles it: this
                 * is a list of values, whatever the subscript happens to be
                 * called. */
                if (ts.isVariableDeclaration(n) && n.name && ts.isIdentifier(n.name) &&
                    n.initializer && (ts.isArrayLiteralExpression(n.initializer) ||
                        (ts.isNewExpression(n.initializer) && n.initializer.expression &&
                         ts.isIdentifier(n.initializer.expression) &&
                         n.initializer.expression.text === 'Array'))) {
                    banned[n.name.text] = 1;
                }

                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }

        var out = {};
        for (var k in cand) {
            if (has(cand, k) && !has(banned, k) &&
                !(program.recTables && has(program.recTables, k)) &&
                !(program.collections && has(program.collections, k))) {
                out[k] = 1;
            }
        }
        program.objTables = out;
    }

    /* The one object slot this table keeps per object. */
    function objTableRef(ctx, name, objectIr) {
        return varRefFromIdent(ctx, name + 'PlayerVar', objectIr);
    }

    /* `X[key]` on such a table, as a read. Null when it is not that shape. */
    function objTableAccess(ts, e, ctx) {
        if (!ctx.program.objTables || !ts.isElementAccessExpression(e)) { return null; }
        if (!ts.isIdentifier(e.expression)) { return null; }
        if (!has(ctx.program.objTables, e.expression.text)) { return null; }
        var objNode = objectForKey(ts, e.argumentExpression, ctx);
        if (!objNode) { return null; }
        var objIr = exprFromTs(ts, objNode, ctx);
        if (!objIr) { return null; }
        return objTableRef(ctx, e.expression.text, objIr);
    }

    /* A RECORD KEPT PER PLAYER IS A SET OF PLAYER SLOTS.
     *
     * `playersStats[playerId] = { k: 0, d: 0, a: 0, hs: 0 }` was lowered as an
     * array of four values stored into a slot of another array. That is the one
     * encoding Portal cannot hold. From EA's own note on AppendToArray:
     * "It is not possible for an array to contain arrays. Attempting to append
     * an array to an array will concatenate them instead." The blocks accept it
     * and the game flattens it, so the mod silently loses the record.
     *
     * modsim does not model that either - its AppendToArray pushes - so the
     * behavioural test went green on a shape that cannot work.
     *
     * The table is keyed by a player, and Portal already stores things per
     * player, so each FIELD becomes one slot on the object:
     *
     *     playersStats[playerId].d      ->  GetVariable(ObjectVariable(player, slot_d))
     *     playersStats[playerId] = {..} ->  one SetVariable per field
     *
     * and `playersStats[playerId]` on its own is simply the player, which is why
     * passing it to a subroutine still works: the subroutine receives the object
     * and reads the same slots.
     */
    function collectRecordTables(ts, names, asts, program) {
        var fields = {}, tables = {}, fi;

        /* An index that looks like an object id. Deliberately loose here,
         * because this only decides which FIELD NAMES are record fields; the
         * lowering re-checks each site with objectForKey and reports the ones it
         * cannot resolve. */
        function looksLikeObjectKey(a) {
            if (!a) { return false; }
            if (ts.isCallExpression(a) && ts.isPropertyAccessExpression(a.expression) &&
                a.expression.name.text === 'GetObjId') { return true; }
            return ts.isIdentifier(a) && /id$/i.test(a.text);
        }

        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }
                if (ts.isElementAccessExpression(n) && ts.isIdentifier(n.expression) &&
                    looksLikeObjectKey(n.argumentExpression)) {
                    var p = n.parent, tableName = n.expression.text;
                    /* X[id] = { a: .., b: .. } */
                    if (p && ts.isBinaryExpression(p) && p.left === n &&
                        p.operatorToken.kind === ts.SyntaxKind.EqualsToken &&
                        ts.isObjectLiteralExpression(p.right)) {
                        tables[tableName] = 1;
                        for (var q = 0; q < p.right.properties.length; q++) {
                            var pr = p.right.properties[q];
                            if (pr.name && ts.isIdentifier(pr.name)) { fields[pr.name.text] = 1; }
                        }
                    }
                    /* X[id].field */
                    if (p && ts.isPropertyAccessExpression(p) && p.expression === n) {
                        tables[tableName] = 1;
                        fields[p.name.text] = 1;
                    }
                }
                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }
        program.recFields = fields;
        /* WHICH TABLES, NOT JUST WHICH FIELDS.
         *
         * Without this, every `X[playerId]` was treated as a record and gave
         * back the player. That is right for playersStats, whose fields live on
         * the player, and badly wrong for playerPrimaryWeapon, where the table
         * holds one value per player and `playerPrimaryWeapon[id]` is a weapon.
         * A table only counts once something is seen reading a field of it or
         * writing a whole record into it. */
        program.recTables = tables;
    }

    /* The player slot holding one field of a per object record. */
    function recFieldRef(ctx, field, objectIr) {
        return varRefFromIdent(ctx, 'rec_' + field + 'PlayerVar', objectIr);
    }

    /* `X[key]` where the key is an object id IS that object, once the record is
     * spread across the object's own slots. Null when this is not that shape. */
    function recordTableObject(ts, e, ctx) {
        if (!ctx.program.recTables || !ts.isElementAccessExpression(e)) { return null; }
        if (!ts.isIdentifier(e.expression)) { return null; }
        if (!has(ctx.program.recTables, e.expression.text)) { return null; }
        var objNode = objectForKey(ts, e.argumentExpression, ctx);
        if (!objNode) { return null; }
        return exprFromTs(ts, objNode, ctx);
    }

    /* A MAP KEYED BY A PLAYER IS A PLAYER VARIABLE.
     *
     * Portal has no Map and no Set, so every `.get`, `.set`, `.has`, `.add` and
     * `.delete` was reported: 1,104 of them across the corpus, the single
     * largest family left.
     *
     * The obvious encoding is a pair of arrays, and it is the wrong one. There
     * are 329 collections in one project set; two Global array slots each is 658
     * against a ceiling of 128, and they cannot be packed because an array
     * cannot live inside an array. It would also turn `.has` from a lookup into
     * a linear scan, inside per tick rules.
     *
     * But 81 percent of call sites key on an object id, and Portal already has
     * per object storage: mod.ObjectVariable(obj, slot) is one slot in the
     * Player or Team scope, which has its own budget of 128, and it is O(1).
     * So a Set becomes one boolean slot on the object, and a Map becomes a value
     * slot plus a present slot, because a Map has to tell a stored 0 from an
     * absent key.
     *
     * This only works when the OBJECT is recoverable at the call site, and the
     * source almost never passes one: it passes an id. See objectForKey. When
     * the object cannot be recovered the site is reported exactly as before,
     * rather than guessed at.
     */
    function collectCollections(ts, names, asts, program) {
        var found = {}, fi;
        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }
                if (ts.isVariableDeclaration(n) && ts.isIdentifier(n.name)) {
                    var kind = collectionKind(ts, n);
                    if (kind) { found[n.name.text] = { kind: kind, valueSlot: null, presentSlot: null }; }
                }
                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }
        program.collections = found;
    }

    /* 'Map' or 'Set' when this declaration is one, from either the annotation
     * or the constructor. Both spellings appear in the corpus. */
    function collectionKind(ts, d) {
        if (d.type && ts.isTypeReferenceNode(d.type) && ts.isIdentifier(d.type.typeName)) {
            var tn = d.type.typeName.text;
            if (tn === 'Map' || tn === 'Set') { return tn; }
        }
        if (d.initializer && ts.isNewExpression(d.initializer) &&
            ts.isIdentifier(d.initializer.expression)) {
            var cn = d.initializer.expression.text;
            if (cn === 'Map' || cn === 'Set') { return cn; }
        }
        return null;
    }

    /* THE SOURCE PASSES AN ID, THE BLOCKS NEED THE OBJECT.
     *
     * Only 59 call sites in the corpus write mod.GetObjId(x) in the argument.
     * 775 pass an identifier that was assigned from one earlier in the function:
     *
     *     const playerId = mod.GetObjId(eventPlayer);
     *     playersStats.set(playerId, ...)
     *
     * so the id is traced back to the object it came from. The scan is per
     * function and literal: an id reassigned from something else is not matched,
     * because being wrong here would silently write another player's slot. */
    function objectForKey(ts, keyExpr, ctx) {
        if (!keyExpr) { return null; }
        if (ts.isCallExpression(keyExpr) && ts.isPropertyAccessExpression(keyExpr.expression) &&
            ts.isIdentifier(keyExpr.expression.expression) &&
            aliasedNamespace(ctx, keyExpr.expression.expression.text) === 'mod' &&
            keyExpr.expression.name.text === 'GetObjId' && keyExpr.arguments.length) {
            return keyExpr.arguments[0];
        }
        if (ts.isIdentifier(keyExpr) && ctx.objOfId && has(ctx.objOfId, keyExpr.text)) {
            return ctx.objOfId[keyExpr.text];
        }
        return null;
    }

    /* Which identifiers in this function hold an object id, and what object it
     * came from. An id assigned more than once is dropped: one of the writes
     * would be wrong and there is no way to tell which. */
    function scanObjIds(ts, fnNode, ctx) {
        var map = {}, banned = {};
        if (!fnNode) { return map; }
        (function walk(n) {
            if (!n) { return; }
            if (ts.isVariableDeclaration(n) && ts.isIdentifier(n.name) && n.initializer) {
                var src = objectForKey(ts, n.initializer, { objOfId: null, program: ctx.program });
                if (src) {
                    if (has(map, n.name.text)) { banned[n.name.text] = true; }
                    map[n.name.text] = src;
                } else {
                    banned[n.name.text] = true;
                }
            }
            if (ts.isBinaryExpression(n) && n.operatorToken &&
                n.operatorToken.kind === ts.SyntaxKind.EqualsToken &&
                ts.isIdentifier(n.left)) {
                banned[n.left.text] = true;
            }
            ts.forEachChild(n, walk);
        })(fnNode);
        for (var k in banned) { if (has(banned, k)) { delete map[k]; } }
        return map;
    }

    /* The object scope slot(s) this collection owns, allocated on first use so
     * a collection we never manage to convert costs nothing. */
    function collectionRef(ctx, name, which, objectIr) {
        var col = ctx.program.collections[name];
        var ident = which === 'present' ? (name + '_hasPlayerVar') : (name + 'PlayerVar');
        var ref = varRefFromIdent(ctx, ident, objectIr);
        if (which === 'present') { col.presentSlot = ref.slot; } else { col.valueSlot = ref.slot; }
        return ref;
    }

    /* `.set`, `.add`, `.delete` and `.clear` change something, so they are
     * statements. Returns an array when one call needs several statements, which
     * stmtFromTs already splices. Null means this is not a collection call, or
     * the object could not be recovered, and the caller reports it as before. */
    function collectionStmt(ts, e, ctx, line) {
        if (!ctx.program.collections) { return null; }
        var callee = e.expression;
        if (!ts.isPropertyAccessExpression(callee) || !ts.isIdentifier(callee.expression)) { return null; }
        var name = callee.expression.text, m = callee.name.text;
        if (!has(ctx.program.collections, name)) { return null; }
        var col = ctx.program.collections[name];
        var args = e.arguments || [];

        if (m === 'clear') {
            /* Clearing per object storage means visiting every object, and the
             * only domain the blocks can enumerate is the players. A collection
             * keyed by anything else cannot be cleared this way, so it is
             * reported rather than half done. */
            reportUnconvertible(ctx.report, ctx.file, line,
                'collection clear on ' + name,
                'A Map or Set held per object has no single place to empty. Unset the ' +
                'entry for each object as it leaves, or loop over All Players and unset ' +
                'each one.');
            return null;
        }

        var objNode = objectForKey(ts, args[0], ctx);
        if (!objNode) { return null; }
        var objIr = exprFromTs(ts, objNode, ctx);
        if (!objIr) { return null; }

        if (m === 'add') {
            return { k: 'setVar', ref: collectionRef(ctx, name, 'value', objIr),
                     value: { k: 'bool', v: true } };
        }
        if (m === 'delete') {
            /* A Set forgets by clearing its one flag. A Map clears the present
             * flag and leaves the value, which nothing can read without it. */
            var which = col.kind === 'Set' ? 'value' : 'present';
            return { k: 'setVar', ref: collectionRef(ctx, name, which, objIr),
                     value: { k: 'bool', v: false } };
        }
        if (m === 'set') {
            var val = exprFromTs(ts, args[1], ctx);
            if (!val) { return null; }
            return [
                { k: 'setVar', ref: collectionRef(ctx, name, 'value', objIr), value: val },
                { k: 'setVar', ref: collectionRef(ctx, name, 'present', objIr),
                  value: { k: 'bool', v: true } }
            ];
        }
        return null;
    }

    /* `.get` and `.has` read, so they are expressions. */
    function collectionExpr(ts, e, ctx) {
        if (!ctx.program.collections) { return null; }
        var callee = e.expression;
        if (!ts.isPropertyAccessExpression(callee) || !ts.isIdentifier(callee.expression)) { return null; }
        var name = callee.expression.text, m = callee.name.text;
        if (!has(ctx.program.collections, name)) { return null; }
        var col = ctx.program.collections[name];
        var args = e.arguments || [];
        if (m !== 'get' && m !== 'has') { return null; }

        var objNode = objectForKey(ts, args[0], ctx);
        if (!objNode) { return null; }
        var objIr = exprFromTs(ts, objNode, ctx);
        if (!objIr) { return null; }

        if (m === 'get') {
            return { k: 'getVar', ref: collectionRef(ctx, name, 'value', objIr) };
        }
        /* A Set stores membership in its one slot; a Map needs the present flag,
         * because a stored 0 or false is a real entry and must not read as
         * absent. */
        var which = col.kind === 'Set' ? 'value' : 'present';
        return { k: 'call', fn: 'Equals', args: [
            { k: 'getVar', ref: collectionRef(ctx, name, which, objIr) },
            { k: 'bool', v: true }
        ] };
    }

    /* A SHORT NAME FOR A LONG mod PATH IS STILL THAT PATH.
     *
     * Mods that touch a lot of one namespace give it a short name first:
     *
     *     const A = mod.WeaponAttachments;
     *     const SK = mod.stringkeys.gunfight.loadout;
     *
     * and then write `A.Muzzle_Lightened_Suppressor` or `SK.weapons.m433`. The
     * converter saw a property of some unknown object and reported it, so one
     * loadout table produced hundreds of entries: about 700 across the projects
     * measured, all of them a name the converter already knew, spelled shorter.
     * Worse, the alias itself became a workspace variable holding a namespace,
     * spending a slot out of the 128 Portal allows on nothing.
     *
     * The alias is followed instead, and the use is converted as though the
     * whole path had been written out.
     */
    /* A BUNDLE IS ONE FILE THAT REMEMBERS WHERE IT CAME FROM.
     *
     * Every project here ships a bundler, and converting `dist/bundle.ts`
     * instead of the source tree is the better read: it is flat, with no
     * imports left to resolve, so calls between modules simply work. On one
     * project it finds 83 event handlers where the source tree finds 18.
     *
     * The cost is that everything lands in one file, and the workspace is laid
     * out one column per file, so a bundle would arrive as a single column of
     * 427 subroutines. The bundler leaves a trail:
     *
     *     // --- SOURCE: src\systems\barriers.ts ---
     *
     * so the original file is recovered from the nearest marker above a line.
     * That restores the columns AND makes every NOT CONVERTED note name the
     * file the author actually wrote, rather than a line number in a bundle
     * they have never opened.
     */
    function collectSourceMarkers(files, names, program) {
        var map = {}, fi;
        for (fi = 0; fi < names.length; fi++) {
            var text = files[names[fi]] || '';
            if (text.indexOf('--- SOURCE:') < 0) { continue; }
            var lines = text.split(/\r?\n/), marks = [], i;
            for (i = 0; i < lines.length; i++) {
                var m = /^\s*\/\/\s*---\s*SOURCE:\s*(.+?)\s*---\s*$/.exec(lines[i]);
                if (m) { marks.push({ line: i + 1, path: m[1].replace(/\\/g, '/') }); }
            }
            if (marks.length) { map[names[fi]] = marks; }
        }
        program.sourceMarks = map;
    }

    /* The file the author wrote, for a line of a converted file. */
    function originalFileOf(program, file, line) {
        var marks = program.sourceMarks && program.sourceMarks[file];
        if (!marks || !marks.length || !line) { return file; }
        var best = null, i;
        for (i = 0; i < marks.length; i++) {
            if (marks[i].line <= line) { best = marks[i]; } else { break; }
        }
        return best ? best.path : file;
    }

    function collectModAliases(ts, names, asts, program) {
        var alias = {}, banned = {}, fi, i, j;
        for (fi = 0; fi < names.length; fi++) {
            var sf = asts[names[fi]];
            for (i = 0; i < sf.statements.length; i++) {
                var st = sf.statements[i];
                if (!ts.isVariableStatement(st)) { continue; }
                if (!(st.declarationList.flags & ts.NodeFlags.Const)) { continue; }
                var ds = st.declarationList.declarations;
                for (j = 0; j < ds.length; j++) {
                    var d = ds[j];
                    if (!ts.isIdentifier(d.name) || !d.initializer) { continue; }
                    var segs = rawModPath(ts, d.initializer);
                    if (segs) { alias[d.name.text] = segs; continue; }
                    /* `const M = mod;` - the namespace itself, no path after it. */
                    if (ts.isIdentifier(d.initializer) && d.initializer.text === 'mod') {
                        alias[d.name.text] = [];
                    }
                }
            }
        }

        /* THE NAMESPACE HANDED IN AS AN ARGUMENT.
         *
         * One project passes the whole namespace into its functions rather than
         * importing it, across 37 files:
         *
         *     export function initDoors(M: typeof mod): number {
         *         const p = M.GetObjectPosition(obj);
         *
         * `M` is a parameter, so no declaration anywhere says what it is except
         * its type. `typeof mod` says it exactly, and it is not a type anyone
         * writes by accident.
         *
         * Parameters are function scoped and this table is not, so a name is
         * only taken to mean the namespace when NOTHING else in the program
         * declares that name. One `M: typeof mod` and one unrelated `let M`
         * elsewhere, and the name is left alone rather than guessed at. */
        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }
                if (ts.isParameter(n) && ts.isIdentifier(n.name)) {
                    if (isTypeofMod(ts, n.type)) { alias[n.name.text] = alias[n.name.text] || []; }
                    else { banned[n.name.text] = true; }
                }
                if (ts.isVariableDeclaration(n) && ts.isIdentifier(n.name) &&
                    !(n.initializer && (rawModPath(ts, n.initializer) ||
                      (ts.isIdentifier(n.initializer) && n.initializer.text === 'mod')))) {
                    banned[n.name.text] = true;
                }
                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }
        for (var k in banned) {
            if (has(banned, k) && has(alias, k) && !alias[k].length) { delete alias[k]; }
        }
        program.modAliases = alias;
    }

    function isTypeofMod(ts, t) {
        return !!(t && ts.isTypeQueryNode && ts.isTypeQueryNode(t) &&
            t.exprName && ts.isIdentifier(t.exprName) && t.exprName.text === 'mod');
    }

    /* The segments of a property chain rooted at the identifier `mod`, without
     * consulting aliases. `mod.stringkeys.a.b` -> ['stringkeys','a','b']. */
    /* A name standing in for the whole namespace dispatches as `mod` does.
     * Only an alias with nothing after it, because `mod.stringkeys.loadout` is
     * a table of keys and calling through it would be nonsense. */
    function aliasedNamespace(ctx, name) {
        var al = ctx.program && ctx.program.modAliases;
        if (al && has(al, name) && al[name].length === 0) { return 'mod'; }
        return name;
    }

    function rawModPath(ts, e) {
        var segs = [];
        while (ts.isPropertyAccessExpression(e)) {
            segs.unshift(e.name.text);
            e = e.expression;
        }
        if (ts.isIdentifier(e) && e.text === 'mod' && segs.length) { return segs; }
        return null;
    }

    /* The same, but a chain may start at an alias instead of at `mod`. */
    function modPath(ts, e, ctx) {
        var segs = [];
        while (ts.isPropertyAccessExpression(e)) {
            segs.unshift(e.name.text);
            e = e.expression;
        }
        if (!ts.isIdentifier(e)) { return null; }
        if (e.text === 'mod') { return segs.length ? segs : null; }
        var al = ctx.program && ctx.program.modAliases;
        if (al && has(al, e.text)) { return al[e.text].concat(segs); }
        return null;
    }

    /* Follow `CONFIG.a.b` down the literal and hand back the node it names. */
    function constRecordField(ts, node, name) {
        if (!node || !ts.isObjectLiteralExpression(node)) { return null; }
        for (var i = 0; i < node.properties.length; i++) {
            var p = node.properties[i];
            if (ts.isPropertyAssignment(p) && p.name && ts.isIdentifier(p.name) &&
                p.name.text === name) {
                return p.initializer;
            }
        }
        return null;
    }

    /* The object node a property access chain is rooted in, or null. */
    function constRecordNodeFor(ts, e, ctx) {
        if (!ctx.program || !ctx.program.constRecords) { return null; }
        if (ts.isIdentifier(e)) {
            return has(ctx.program.constRecords, e.text)
                ? ctx.program.constRecords[e.text].node : null;
        }
        if (ts.isPropertyAccessExpression(e)) {
            var owner = constRecordNodeFor(ts, e.expression, ctx);
            return owner ? constRecordField(ts, owner, e.name.text) : null;
        }
        return null;
    }

    /* ================================================================== *
     * 3b. UI COMPONENT LIBRARIES
     *
     * A workspace has no classes and no `new`, so every `new UI.Text({...})`
     * was reported unconvertible - 23 sites in one project - and the
     * converted mod drew no HUD at all.
     *
     * A UI component is not a class in any way the blocks care about,
     * though. Its constructor fills in defaults, makes ONE mod.AddUI* call
     * with positional arguments, and looks the new widget back up by name.
     * That is a shape blocks can say exactly, so it is lowered rather than
     * reported:
     *
     *     new UI.Text({ parent: c, x: 0, y: 8, message: m })
     *
     *       AddUIText("ui_3", CreateVector(0,8,0), CreateVector(100,20,0),
     *                 UIAnchor.Center, <c>, true, 0, <bgColor>, 0,
     *                 UIBgFill.None, <m>, 14, <textColor>, 1, UIAnchor.Center)
     *       <target> = FindUIWidgetWithName("ui_3")
     *
     * A component instance IS its widget here. Everything a caller can do
     * with one - pass it as a parent, call setMessage on it, delete it -
     * needs only the widget, and that keeps a component in a single slot
     * instead of a record.
     *
     * TWO SOURCES OF TRUTH, IN THIS ORDER.
     *
     *  1. A `namespace UI { class Container { ... } }` written in the project
     *     itself: the constructor is READ and reproduced argument for
     *     argument. A vendored copy of a component library is usually
     *     trimmed - the one in TDM drops the trailing UIDepth argument and
     *     defaults a text background to black where the published library
     *     uses white - so answering from the package would emit calls this
     *     project never makes.
     *
     *  2. Failing that, the published bf6-portal-utils components, encoded
     *     below from their own constructors.
     * ================================================================== */

    function uiLit(ir) { return { t: 'lit', ir: ir }; }
    function uiNum(v) { return uiLit({ k: 'num', v: v }); }
    function uiBool(v) { return uiLit({ k: 'bool', v: v }); }
    function uiEnum(enumName, member) { return uiLit({ k: 'enum', enumName: enumName, member: member }); }
    function uiOpt(name, def) { return { t: 'opt', name: name, def: def || null }; }
    function uiCall(fn, args) { return { t: 'call', fn: fn, args: args }; }
    function uiColor(ns, key) { return { t: 'color', ns: ns, key: key }; }
    /* `params.x ?? params.position?.x ?? 0` - the library lets a caller give
     * either loose x/y or a position record, so both are looked at in order. */
    function uiFirst(list, def) { return { t: 'first', list: list, def: def }; }
    function uiOptField(name, field) { return { t: 'optField', name: name, field: field }; }

    /* The published library's COLORS table, which is built once at module
     * load. Every entry is hoisted to one workspace variable set at round
     * start rather than inlined at each use: inlining would run a CreateVector
     * per widget that the original never performs. */
    var UI_LIB_COLORS = {
        BLACK: [0, 0, 0], GREY_25: [0.25, 0.25, 0.25], GREY_50: [0.5, 0.5, 0.5],
        GREY_75: [0.75, 0.75, 0.75], WHITE: [1, 1, 1], RED: [1, 0, 0],
        GREEN: [0, 1, 0], BLUE: [0, 0, 1], YELLOW: [1, 1, 0], PURPLE: [1, 0, 1],
        CYAN: [0, 1, 1], MAGENTA: [1, 0, 1],
        BF_GREY_1: [0.8353, 0.9216, 0.9765], BF_GREY_2: [0.3294, 0.3686, 0.3882],
        BF_GREY_3: [0.2118, 0.2235, 0.2353], BF_GREY_4: [0.0314, 0.0431, 0.0431],
        BF_BLUE_BRIGHT: [0.4392, 0.9216, 1.0], BF_BLUE_DARK: [0.0745, 0.1843, 0.2471],
        BF_RED_BRIGHT: [1.0, 0.5137, 0.3804], BF_RED_DARK: [0.251, 0.0941, 0.0667],
        BF_GREEN_BRIGHT: [0.6784, 0.9922, 0.5255], BF_GREEN_DARK: [0.2784, 0.4471, 0.2118],
        BF_YELLOW_BRIGHT: [1.0, 0.9882, 0.6118], BF_YELLOW_DARK: [0.4431, 0.3765, 0.0]
    };

    /* The leading arguments every bf6-portal-utils element shares, in the
     * order its constructors assemble them. LIB is the namespace key their
     * COLORS entries are hoisted under. */
    function uiLibHead() {
        return [
            { t: 'name' },
            uiCall('CreateVector', [
                uiFirst([uiOpt('x'), uiOptField('position', 'x')], uiNum(0)),
                uiFirst([uiOpt('y'), uiOptField('position', 'y')], uiNum(0)),
                uiNum(0)
            ]),
            uiCall('CreateVector', [
                uiFirst([uiOpt('width'), uiOptField('size', 'width')], uiNum(0)),
                uiFirst([uiOpt('height'), uiOptField('size', 'height')], uiNum(0)),
                uiNum(0)
            ]),
            uiOpt('anchor', uiEnum('UIAnchor', 'Center')),
            { t: 'parentWidget' },
            uiOpt('visible', uiBool(true))
        ];
    }

    var UI_SETTER_METHODS = {
        setMessage: 'SetUITextLabel', setTextSize: 'SetUITextSize',
        setTextColor: 'SetUITextColor', setTextAlpha: 'SetUITextAlpha',
        setTextAnchor: 'SetUITextAnchor', setPadding: 'SetUIWidgetPadding',
        setVisible: 'SetUIWidgetVisible', setBgColor: 'SetUIWidgetBgColor',
        setBgAlpha: 'SetUIWidgetBgAlpha', setBgFill: 'SetUIWidgetBgFill',
        setImageType: 'SetUIImageType', setImageColor: 'SetUIImageColor',
        setImageAlpha: 'SetUIImageAlpha'
    };

    /* One method entry: the mod.* call it makes, on the widget, with the
     * method's own arguments. Only setters that touch nothing but the widget
     * are listed - `set x` also needs the element's remembered y, which no
     * block carries. */
    function uiLibMethods(names) {
        var m = { 'delete': { calls: [{ fn: 'DeleteUIWidget', args: [{ t: 'recv' }] }] } }, i;
        for (i = 0; i < names.length; i++) {
            m[names[i]] = { calls: [{ fn: UI_SETTER_METHODS[names[i]],
                                      args: [{ t: 'recv' }, { t: 'marg', i: 0 }] }] };
        }
        return m;
    }

    var UI_LIB_SPECS = null;
    function uiLibSpecs() {
        if (UI_LIB_SPECS) { return UI_LIB_SPECS; }
        var common = ['setVisible', 'setBgColor', 'setBgAlpha', 'setBgFill'];
        var tail = function (extra) {
            return uiLibHead().concat([
                uiNum(0),                                   /* padding */
                uiOpt('bgColor', uiColor('LIB', 'WHITE')),
                uiOpt('bgAlpha', uiNum(0)),
                uiOpt('bgFill', uiEnum('UIBgFill', 'None'))
            ]).concat(extra);
        };
        UI_LIB_SPECS = {
            UIContainer: {
                label: 'UIContainer', fn: 'AddUIContainer', namePrefix: 'root_',
                args: tail([uiOpt('depth', uiEnum('UIDepth', 'AboveGameUI'))]),
                /* delete() on a container also deletes the children the
                 * library tracks in a Set the blocks do not have, so it is
                 * left out rather than half done. */
                methods: uiLibMethods(common), required: []
            },
            UIText: {
                label: 'UIText', fn: 'AddUIText', namePrefix: 'root_',
                args: uiLibHead().concat([
                    uiOpt('padding', uiNum(0)),
                    uiOpt('bgColor', uiColor('LIB', 'WHITE')),
                    uiOpt('bgAlpha', uiNum(0)),
                    uiOpt('bgFill', uiEnum('UIBgFill', 'None')),
                    uiOpt('message'),
                    uiOpt('textSize', uiNum(36)),
                    uiOpt('textColor', uiColor('LIB', 'BLACK')),
                    uiOpt('textAlpha', uiNum(1)),
                    uiOpt('textAnchor', uiEnum('UIAnchor', 'Center')),
                    uiOpt('depth', uiEnum('UIDepth', 'AboveGameUI'))
                ]),
                methods: uiLibMethods(common.concat(['setMessage', 'setTextSize',
                    'setTextColor', 'setTextAlpha', 'setTextAnchor', 'setPadding'])),
                required: ['message']
            },
            UIImage: {
                label: 'UIImage', fn: 'AddUIImage', namePrefix: 'root_',
                args: tail([
                    uiOpt('imageType'),
                    uiOpt('imageColor', uiColor('LIB', 'WHITE')),
                    uiOpt('imageAlpha', uiNum(1)),
                    uiOpt('depth', uiEnum('UIDepth', 'AboveGameUI'))
                ]),
                methods: uiLibMethods(common.concat(['setImageType', 'setImageColor', 'setImageAlpha'])),
                required: ['imageType']
            },
            /* The weapon and gadget images fix their own background and depth
             * rather than taking them from the caller, and their AddUI* takes
             * the subject BEFORE the parent. */
            UIWeaponImage: {
                label: 'UIWeaponImage', fn: 'AddUIWeaponImage', namePrefix: 'root_',
                args: uiLibHead().slice(0, 4).concat([
                    uiOpt('weapon'),
                    { t: 'parentWidget' },
                    uiOpt('weaponPackage', uiCall('CreateNewWeaponPackage', []))
                ]),
                methods: uiLibMethods(common), required: ['weapon']
            },
            UIGadgetImage: {
                label: 'UIGadgetImage', fn: 'AddUIGadgetImage', namePrefix: 'root_',
                args: uiLibHead().slice(0, 4).concat([
                    uiOpt('gadget'),
                    { t: 'parentWidget' }
                ]),
                methods: uiLibMethods(common), required: ['gadget']
            }
        };
        return UI_LIB_SPECS;
    }

    /* ---- reading a component the project wrote itself ------------------ */

    /* The name a widget is created under. `ui_${_counter++}` is a runtime
     * counter, which blocks cannot compute, so the prefix is taken from the
     * template and the number is handed out per construction SITE instead.
     * See uiAssignSiteNames for what that costs. */
    function uiNamePrefixOf(ts, fn) {
        var body = fn && fn.body, i, st, e;
        if (!body || !body.statements) { return null; }
        for (i = 0; i < body.statements.length; i++) {
            st = body.statements[i];
            if (!ts.isReturnStatement(st) || !st.expression) { continue; }
            e = st.expression;
            if (ts.isTemplateExpression(e) && e.head && typeof e.head.text === 'string') {
                return e.head.text;
            }
            if (ts.isStringLiteral(e)) { return e.text; }
        }
        return null;
    }

    /* Walk the constructor once, remembering what each name and each `this`
     * field was set from, and find the single mod.AddUI* call. */
    function uiCtorScan(ts, ctor) {
        var out = { locals: {}, fields: {}, add: null, widgetField: null, ok: true };
        var body = ctor && ctor.body;
        if (!body || !body.statements) { return null; }
        var i, j, st, e;
        for (i = 0; i < body.statements.length; i++) {
            st = body.statements[i];
            if (ts.isVariableStatement(st)) {
                var ds = st.declarationList.declarations;
                for (j = 0; j < ds.length; j++) {
                    if (ds[j].name && ts.isIdentifier(ds[j].name) && ds[j].initializer) {
                        out.locals[ds[j].name.text] = ds[j].initializer;
                    } else { out.ok = false; }
                }
                continue;
            }
            if (!ts.isExpressionStatement(st)) { out.ok = false; continue; }
            e = st.expression;
            if (ts.isBinaryExpression(e) && e.operatorToken.kind === ts.SyntaxKind.EqualsToken &&
                ts.isPropertyAccessExpression(e.left) &&
                e.left.expression.kind === ts.SyntaxKind.ThisKeyword) {
                var fname = e.left.name.text;
                var rhs = e.right;
                while (ts.isAsExpression(rhs) || ts.isParenthesizedExpression(rhs)) { rhs = rhs.expression; }
                if (ts.isCallExpression(rhs) && isModCall(ts, rhs, 'FindUIWidgetWithName')) {
                    out.widgetField = fname;
                    continue;
                }
                out.fields[fname] = rhs;
                continue;
            }
            if (ts.isCallExpression(e) && ts.isPropertyAccessExpression(e.expression) &&
                ts.isIdentifier(e.expression.expression) && e.expression.expression.text === 'mod' &&
                /^AddUI/.test(e.expression.name.text)) {
                if (out.add) { out.ok = false; }
                out.add = e;
                continue;
            }
            /* Anything else the constructor does is real work with no place to
             * go, so the class is left unread rather than half reproduced. */
            out.ok = false;
        }
        return out;
    }

    /* One constructor expression -> one descriptor, or null when the shape is
     * outside what a block can restate. */
    function uiDeriveDesc(ts, node, scan, env, prm, ns, depth, inMethod) {
        if (!node || depth > 12) { return null; }
        while (ts.isParenthesizedExpression(node) || ts.isAsExpression(node) ||
            (ts.isNonNullExpression && ts.isNonNullExpression(node))) { node = node.expression; }

        if (ts.isNumericLiteral(node)) { return uiNum(Number(node.text)); }
        if (ts.isPrefixUnaryExpression(node) && node.operator === ts.SyntaxKind.MinusToken &&
            ts.isNumericLiteral(node.operand)) { return uiNum(-Number(node.operand.text)); }
        if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) {
            return uiLit({ k: 'str', v: node.text });
        }
        if (node.kind === ts.SyntaxKind.TrueKeyword) { return uiBool(true); }
        if (node.kind === ts.SyntaxKind.FalseKeyword) { return uiBool(false); }

        /* `params.x ?? 0` and `params.parent ? A : B` are the only two ways
         * these constructors ask whether an option was given, and both fold
         * away here: the call site either has the property or it does not. */
        if (ts.isBinaryExpression(node) &&
            node.operatorToken.kind === ts.SyntaxKind.QuestionQuestionToken) {
            var left = uiDeriveDesc(ts, node.left, scan, env, prm, ns, depth + 1, inMethod);
            var right = uiDeriveDesc(ts, node.right, scan, env, prm, ns, depth + 1, inMethod);
            if (!left || !right) { return null; }
            if (left.t === 'opt' && !left.def) { return uiOpt(left.name, right); }
            if (left.t === 'first' && !left.def) { return uiFirst(left.list, right); }
            return null;
        }
        if (ts.isConditionalExpression(node)) {
            var test = uiDeriveDesc(ts, node.condition, scan, env, prm, ns, depth + 1, inMethod);
            if (!test || test.t !== 'opt' || test.def) { return null; }
            var a = uiDeriveDesc(ts, node.whenTrue, scan, env, prm, ns, depth + 1, inMethod);
            var b = uiDeriveDesc(ts, node.whenFalse, scan, env, prm, ns, depth + 1, inMethod);
            if (!a || !b) { return null; }
            return { t: 'present', name: test.name, then: a, otherwise: b };
        }

        if (ts.isPropertyAccessExpression(node)) {
            var owner = node.expression, key = node.name.text;
            if (ts.isIdentifier(owner) && owner.text === 'mod') { return null; }
            if (ts.isPropertyAccessExpression(owner) && ts.isIdentifier(owner.expression) &&
                owner.expression.text === 'mod') { return uiEnum(owner.name.text, key); }
            if (owner.kind === ts.SyntaxKind.ThisKeyword) {
                if (key === scan.widgetField) { return { t: 'recv' }; }
                if (has(env, key)) { return env[key]; }
                if (has(scan.fields, key)) {
                    /* Inside a method the field is whatever the CONSTRUCTOR put
                     * there, which is per instance and so not a value a block
                     * can read. It is left as a reference and answered later,
                     * and only if every construction agreed on it. */
                    if (inMethod) { return { t: 'field', name: key }; }
                    return uiDeriveDesc(ts, scan.fields[key], scan, env, prm, ns, depth + 1, inMethod);
                }
                return null;
            }
            /* `params.parent._widget` - a component option used for its widget,
             * and a component IS its widget here. */
            if (ts.isPropertyAccessExpression(owner) && ts.isIdentifier(owner.expression) &&
                owner.expression.text === prm) {
                if (/^(_?widget|uiWidget)$/.test(key)) { return uiOpt(owner.name.text); }
                return null;
            }
            if (ts.isIdentifier(owner) && owner.text === prm) { return uiOpt(key); }
            if (ts.isIdentifier(owner) && owner.text === 'COLORS') { return uiColor(ns, key); }
            if (ts.isPropertyAccessExpression(owner) && owner.name.text === 'COLORS') {
                return uiColor(ns, key);
            }
            return null;
        }

        if (ts.isIdentifier(node)) {
            if (has(env, node.text)) { return env[node.text]; }
            if (has(scan.locals, node.text)) {
                return uiDeriveDesc(ts, scan.locals[node.text], scan, env, prm, ns, depth + 1, inMethod);
            }
            return null;
        }

        if (ts.isCallExpression(node)) {
            var c = node.expression;
            if (ts.isIdentifier(c) && scan.nameFn && c.text === scan.nameFn) { return { t: 'name' }; }
            if (ts.isPropertyAccessExpression(c) && ts.isIdentifier(c.expression) &&
                c.expression.text === 'mod') {
                var args = [], ai;
                for (ai = 0; ai < node.arguments.length; ai++) {
                    var d = uiDeriveDesc(ts, node.arguments[ai], scan, env, prm, ns, depth + 1, inMethod);
                    if (!d) { return null; }
                    args.push(d);
                }
                return uiCall(c.name.text, args);
            }
            return null;
        }
        return null;
    }

    /* A method that only calls mod.* on the widget with its own arguments. */
    function uiDeriveMethod(ts, m, scan, prm, ns) {
        var body = m.body, out = { calls: [] }, env = {}, i, st, e;
        if (!body || !body.statements) { return null; }
        var margs = {};
        for (i = 0; i < (m.parameters || []).length; i++) {
            if (m.parameters[i].name && ts.isIdentifier(m.parameters[i].name)) {
                margs[m.parameters[i].name.text] = { t: 'marg', i: i };
            }
        }
        for (i = 0; i < body.statements.length; i++) {
            st = body.statements[i];
            if (ts.isReturnStatement(st) &&
                (!st.expression || st.expression.kind === ts.SyntaxKind.ThisKeyword)) { continue; }
            if (!ts.isExpressionStatement(st)) { return null; }
            e = st.expression;
            /* `this._y = y` before the call that reads it: the field is that
             * argument from here on, so nothing has to remember it. */
            if (ts.isBinaryExpression(e) && e.operatorToken.kind === ts.SyntaxKind.EqualsToken &&
                ts.isPropertyAccessExpression(e.left) &&
                e.left.expression.kind === ts.SyntaxKind.ThisKeyword) {
                var d = uiDeriveDesc(ts, e.right, scan, mergeEnv(env, margs), prm, ns, 0, true);
                if (!d) { return null; }
                env[e.left.name.text] = d;
                continue;
            }
            if (!ts.isCallExpression(e) || !ts.isPropertyAccessExpression(e.expression) ||
                !ts.isIdentifier(e.expression.expression) ||
                e.expression.expression.text !== 'mod') { return null; }
            var cargs = [], ai;
            for (ai = 0; ai < e.arguments.length; ai++) {
                var cd = uiDeriveDesc(ts, e.arguments[ai], scan, mergeEnv(env, margs), prm, ns, 0, true);
                if (!cd) { return null; }
                cargs.push(cd);
            }
            out.calls.push({ fn: e.expression.name.text, args: cargs });
        }
        return out.calls.length ? out : null;
    }

    function mergeEnv(a, b) {
        var out = {}, k;
        for (k in b) { if (has(b, k)) { out[k] = b[k]; } }
        for (k in a) { if (has(a, k)) { out[k] = a[k]; } }
        return out;
    }

    /* A `this` field a method needs but no block carries. It is only usable
     * when the field means the same thing for every instance in the program,
     * which uiFieldConstants decides. */
    function uiFieldDescs(ts, scan, prm, ns) {
        var out = {}, k;
        for (k in scan.fields) {
            if (!has(scan.fields, k)) { continue; }
            var d = uiDeriveDesc(ts, scan.fields[k], scan, {}, prm, ns, 0, false);
            if (d) { out[k] = d; }
        }
        return out;
    }

    function uiDeriveSpec(ts, cls, ns, nameFn, namePrefix) {
        var i, ctor = null, methodNodes = [];
        for (i = 0; i < (cls.members || []).length; i++) {
            var mem = cls.members[i];
            if (ts.isConstructorDeclaration(mem)) { ctor = mem; }
            else if (ts.isMethodDeclaration(mem) && mem.name && ts.isIdentifier(mem.name)) {
                methodNodes.push(mem);
            }
        }
        if (!ctor || !ctor.parameters.length || !ts.isIdentifier(ctor.parameters[0].name)) { return null; }
        var prm = ctor.parameters[0].name.text;
        var scan = uiCtorScan(ts, ctor);
        if (!scan || !scan.ok || !scan.add) { return null; }
        scan.nameFn = nameFn;

        var args = [];
        for (i = 0; i < scan.add.arguments.length; i++) {
            var d = uiDeriveDesc(ts, scan.add.arguments[i], scan, {}, prm, ns, 0, false);
            if (!d) { return null; }
            args.push(d);
        }
        var spec = {
            label: ns + '.' + cls.name.text,
            fn: scan.add.expression.name.text,
            namePrefix: namePrefix || 'ui_',
            args: args, methods: {}, required: [],
            fieldDescs: uiFieldDescs(ts, scan, prm, ns), derived: true
        };
        for (i = 0; i < args.length; i++) {
            if (args[i].t === 'opt' && !args[i].def) { spec.required.push(args[i].name); }
        }
        for (i = 0; i < methodNodes.length; i++) {
            var mm = uiDeriveMethod(ts, methodNodes[i], scan, prm, ns);
            if (mm) { spec.methods[methodNodes[i].name.text] = mm; }
            else { spec.unreadable = spec.unreadable || {};
                   spec.unreadable[methodNodes[i].name.text] = true; }
        }
        return spec;
    }

    /* ---- the collection pass ------------------------------------------- */

    function uiTypeName(ts, tn) {
        if (!tn || !ts.isTypeReferenceNode(tn) || !tn.typeName) { return null; }
        if (ts.isIdentifier(tn.typeName)) { return tn.typeName.text; }
        if (tn.typeName.left && tn.typeName.right && ts.isIdentifier(tn.typeName.left)) {
            return tn.typeName.left.text + '.' + tn.typeName.right.text;
        }
        return null;
    }

    function uiCalleeName(ts, e) {
        var c = e.expression;
        if (!c) { return null; }
        if (ts.isIdentifier(c)) { return c.text; }
        if (ts.isPropertyAccessExpression(c) && ts.isIdentifier(c.expression)) {
            return c.expression.text + '.' + c.name.text;
        }
        return null;
    }

    function collectUiComponents(ts, names, asts, program) {
        var specs = {}, colors = {}, fi, i;
        program.uiSpecs = specs;
        program.uiColorTables = colors;   /* ns -> { KEY: [r,g,b] } */
        program.uiKinds = {};
        program.uiSiteNames = {};
        program.uiSites = {};             /* label -> [option maps] */
        program.uiColorInit = {};
        program.uiNamespaces = {};

        /* 1. namespaces the project wrote itself. */
        for (fi = 0; fi < names.length; fi++) {
            var sf = asts[names[fi]];
            for (i = 0; i < sf.statements.length; i++) {
                var st = sf.statements[i];
                if (!ts.isModuleDeclaration(st) || !st.name || !ts.isIdentifier(st.name)) { continue; }
                var bodyNode = st.body;
                if (!bodyNode || !bodyNode.statements) { continue; }
                var ns = st.name.text, members = bodyNode.statements;
                var nameFn = null, prefix = null, table = null, classes = [], j, unread = 0;
                for (j = 0; j < members.length; j++) {
                    var m = members[j];
                    if (ts.isFunctionDeclaration(m) && m.name) {
                        var p = uiNamePrefixOf(ts, m);
                        if (p !== null) { nameFn = m.name.text; prefix = p; }
                    } else if (ts.isVariableStatement(m)) {
                        var ds = m.declarationList.declarations, di;
                        for (di = 0; di < ds.length; di++) {
                            if (ds[di].name && ts.isIdentifier(ds[di].name) &&
                                ds[di].name.text === 'COLORS' && ds[di].initializer &&
                                ts.isObjectLiteralExpression(ds[di].initializer)) {
                                table = ds[di].initializer;
                            }
                        }
                    } else if (ts.isClassDeclaration(m) && m.name) {
                        classes.push(m);
                    }
                }
                if (table) { colors[ns] = { node: table }; }
                for (j = 0; j < classes.length; j++) {
                    var spec = uiDeriveSpec(ts, classes[j], ns, nameFn, prefix);
                    if (spec) { specs[ns + '.' + classes[j].name.text] = spec; }
                    else { unread++; }
                }
                /* Only claim the namespace when every class in it was read.
                 * Half of one understood is still a namespace the author has
                 * to be told about. */
                if (classes.length && !unread) { program.uiNamespaces[ns] = { file: names[fi], node: st }; }
            }
        }

        /* 2. the published components, for names imported from the package. */
        var lib = uiLibSpecs();
        for (fi = 0; fi < names.length; fi++) {
            var sfi = asts[names[fi]];
            for (i = 0; i < sfi.statements.length; i++) {
                var imp = sfi.statements[i];
                if (!ts.isImportDeclaration(imp) || !imp.moduleSpecifier) { continue; }
                var from = imp.moduleSpecifier.text || '';
                if (from.indexOf('bf6-portal-utils/ui') !== 0) { continue; }
                var clause = imp.importClause && imp.importClause.namedBindings;
                if (!clause || !clause.elements) { continue; }
                for (var ei = 0; ei < clause.elements.length; ei++) {
                    var local = clause.elements[ei].name.text;
                    var orig = clause.elements[ei].propertyName ?
                        clause.elements[ei].propertyName.text : local;
                    if (has(lib, orig) && !has(specs, local)) { specs[local] = lib[orig]; }
                    /* A project that writes its own `namespace UI` and also
                     * imports the package one under the same name has two
                     * tables. The project's wins, with the package as the
                     * fallback for keys it did not copy over - overwriting the
                     * entry here made the shim read the published colours. */
                    if (orig === 'UI') {
                        colors[local] = colors[local] || {};
                        colors[local].lib = true;
                    }
                }
            }
        }

        ctx_uiSpecNames = specs;
        uiAssignSiteNames(ts, names, asts, program);
        uiCollectKinds(ts, names, asts, program);
        uiFieldConstants(ts, program);
    }

    /* WIDGET NAMES ARE HANDED OUT PER SITE, NOT PER CONSTRUCTION.
     *
     * The libraries name a widget from a counter bumped on every construction,
     * and no block builds a string at runtime: Portal takes a widget name as a
     * literal. So each `new` in the source gets its own literal name, in file
     * order. Two consequences worth knowing, and neither is silent - the
     * warning in uiNewToIr says so:
     *
     *   - the numbering is the source's, not the run's, so a widget built by
     *     the second function to run is still named after where it was
     *     written;
     *   - a construction inside a function called twice reuses its name, where
     *     the original would have made a second widget.
     */
    function uiAssignSiteNames(ts, names, asts, program) {
        var counters = {}, fi;
        program.uiSitesByKey = {};
        for (fi = 0; fi < names.length; fi++) {
            (function walk(n) {
                if (!n) { return; }
                if (ts.isNewExpression(n)) {
                    var key = uiCalleeName(ts, n);
                    if (key && has(program.uiSpecs, key)) {
                        var pre = program.uiSpecs[key].namePrefix || 'ui_';
                        if (!has(counters, pre)) { counters[pre] = 0; }
                        program.uiSiteNames[names[fi] + ':' + n.pos] = pre + (counters[pre]++);
                        var opts = uiOptionsOf(ts, n);
                        if (opts) {
                            if (!has(program.uiSitesByKey, key)) { program.uiSitesByKey[key] = []; }
                            program.uiSitesByKey[key].push(opts);
                        }
                    }
                }
                ts.forEachChild(n, walk);
            })(asts[names[fi]]);
        }
    }

    /* Which names hold a component, so a later `x.setMessage(...)` knows what
     * x is. Keyed by the source text of the path, which is what a call site
     * gives us: a block workspace has no types to ask. */
    function uiCollectKinds(ts, names, asts, program) {
        var fi;
        function note(text, kind) {
            if (!text) { return; }
            if (has(program.uiKinds, text) && program.uiKinds[text] !== kind) {
                program.uiKinds[text] = '?';
                return;
            }
            program.uiKinds[text] = kind;
        }
        function textOf(node, sf) {
            try { return node.getText(sf).replace(/\s+/g, ''); } catch (e) { return null; }
        }
        for (fi = 0; fi < names.length; fi++) {
            var sf = asts[names[fi]];
            (function walk(n) {
                if (!n) { return; }
                if (ts.isNewExpression(n)) {
                    var kind = uiCalleeName(ts, n);
                    if (kind && has(program.uiSpecs, kind)) {
                        var p = n.parent;
                        if (p && ts.isVariableDeclaration(p) && p.name && ts.isIdentifier(p.name)) {
                            note(p.name.text, kind);
                        } else if (p && ts.isBinaryExpression(p) && p.right === n &&
                            p.operatorToken.kind === ts.SyntaxKind.EqualsToken) {
                            note(textOf(p.left, sf), kind);
                        }
                    }
                }
                if ((ts.isParameter(n) || ts.isVariableDeclaration(n) ||
                     ts.isPropertyAssignment(n)) && n.type) {
                    var tn = uiTypeName(ts, n.type);
                    if (tn && has(program.uiSpecs, tn) && n.name && ts.isIdentifier(n.name)) {
                        note(n.name.text, tn);
                    }
                }
                ts.forEachChild(n, walk);
            })(sf);
        }
    }

    /* A METHOD MAY NEED A FIELD ONLY THE CONSTRUCTOR KNOWS.
     *
     *     setY(y) { mod.SetUIWidgetPosition(w, mod.CreateVector(this._x, y, 0)) }
     *
     * `this._x` is per instance state, and blocks carry none. It is still
     * answerable when every construction of that component in the program
     * passes the same literal - TDM builds all three of its containers at
     * x: 0 - because then the value does not depend on which instance this is.
     * When the sites disagree the field stays unknown and the method that
     * needs it is reported, rather than being emitted with one site's number.
     */
    function uiFieldConstants(ts, program) {
        var byLabel = {}, fi;
        function litOf(node) {
            if (!node) { return null; }
            if (ts.isNumericLiteral(node)) { return { k: 'num', v: Number(node.text) }; }
            if (ts.isPrefixUnaryExpression(node) && node.operator === ts.SyntaxKind.MinusToken &&
                ts.isNumericLiteral(node.operand)) { return { k: 'num', v: -Number(node.operand.text) }; }
            if (ts.isStringLiteral(node)) { return { k: 'str', v: node.text }; }
            if (node.kind === ts.SyntaxKind.TrueKeyword) { return { k: 'bool', v: true }; }
            if (node.kind === ts.SyntaxKind.FalseKeyword) { return { k: 'bool', v: false }; }
            return null;
        }
        for (var key in program.uiSpecs) {
            if (!has(program.uiSpecs, key)) { continue; }
            var spec = program.uiSpecs[key];
            if (!spec.fieldDescs) { continue; }
            byLabel[key] = spec;
            spec.fieldConst = {};
        }
        var sitesByKey = program.uiSitesByKey || {};
        for (var k2 in byLabel) {
            if (!has(byLabel, k2)) { continue; }
            var sp = byLabel[k2], sites = sitesByKey[k2] || [];
            for (var f in sp.fieldDescs) {
                if (!has(sp.fieldDescs, f)) { continue; }
                var d = sp.fieldDescs[f];
                if (d.t === 'lit') { sp.fieldConst[f] = d.ir; continue; }
                if (d.t !== 'opt' || !d.def || d.def.t !== 'lit') { continue; }
                var agreed = d.def.ir, ok = true;
                for (fi = 0; fi < sites.length; fi++) {
                    var node = has(sites[fi], d.name) ? sites[fi][d.name] : null;
                    if (!node) { continue; }
                    var lit = litOf(node);
                    if (!lit || lit.k !== agreed.k || lit.v !== agreed.v) { ok = false; break; }
                }
                if (ok) { sp.fieldConst[f] = agreed; }
            }
        }
    }

    /* ---- lowering a construction --------------------------------------- */

    function uiOptionsOf(ts, e) {
        var opts = {}, arg = e.arguments && e.arguments[0], i;
        if (!e.arguments || !e.arguments.length) { return opts; }
        if (!ts.isObjectLiteralExpression(arg)) { return null; }
        for (i = 0; i < arg.properties.length; i++) {
            var p = arg.properties[i];
            if (ts.isPropertyAssignment(p) && p.name && ts.isIdentifier(p.name)) {
                opts[p.name.text] = p.initializer;
            } else if (ts.isShorthandPropertyAssignment(p) && p.name && ts.isIdentifier(p.name)) {
                opts[p.name.text] = p.name;
            } else { return null; }
        }
        return opts;
    }

    function uiSpecFor(ts, e, ctx) {
        if (!ctx.program || !ctx.program.uiSpecs) { return null; }
        var key = uiCalleeName(ts, e);
        return key && has(ctx.program.uiSpecs, key) ? ctx.program.uiSpecs[key] : null;
    }

    /* A colour table entry, hoisted to one workspace variable that round start
     * fills in. The tables are module constants built once, so inlining the
     * CreateVector at each use would add calls the original never makes. */
    function uiColorIr(ns, key, ctx) {
        var tables = ctx.program.uiColorTables || {};
        var tbl = has(tables, ns) ? tables[ns] : (ns === 'LIB' ? { lib: true } : null);
        if (!tbl) { return null; }
        var ir = null;
        if (tbl.node) {
            var node = constRecordField(getTs(), tbl.node, key);
            if (node) { ir = exprFromTs(getTs(), node, ctx); }
        }
        if (!ir && (tbl.lib || ns === 'LIB') && has(UI_LIB_COLORS, key)) {
            var v = UI_LIB_COLORS[key];
            ir = { k: 'call', fn: 'CreateVector', args: [
                { k: 'num', v: v[0] }, { k: 'num', v: v[1] }, { k: 'num', v: v[2] }] };
        }
        if (!ir) { return null; }
        var ident = 'uiColor_' + ns + '_' + key;
        ctx.program.uiColorInit = ctx.program.uiColorInit || {};
        ctx.program.uiColorInit[ident] = ir;
        return { k: 'getVar', ref: varRefFromIdent(ctx, ident, null) };
    }

    function uiDescToIr(ts, d, site, ctx) {
        if (!d) { return null; }
        var i, args;
        if (d.t === 'lit') { return d.ir; }
        if (d.t === 'name') { return { k: 'str', v: site.name }; }
        if (d.t === 'color') { return uiColorIr(d.ns, d.key, ctx); }
        if (d.t === 'call') {
            args = [];
            for (i = 0; i < d.args.length; i++) {
                var a = uiDescToIr(ts, d.args[i], site, ctx);
                if (!a) { return null; }
                args.push(a);
            }
            return { k: 'call', fn: d.fn, args: args };
        }
        if (d.t === 'opt') {
            if (has(site.optIr, d.name)) { return site.optIr[d.name]; }
            if (has(site.opts, d.name)) { return uiOptIr(ts, site.opts[d.name], ctx); }
            return d.def ? uiDescToIr(ts, d.def, site, ctx) : null;
        }
        if (d.t === 'optField') {
            if (!has(site.opts, d.name)) { return null; }
            var rec = site.opts[d.name];
            if (!ts.isObjectLiteralExpression(rec)) { return null; }
            var node = constRecordField(ts, rec, d.field);
            return node ? uiOptIr(ts, node, ctx) : null;
        }
        if (d.t === 'first') {
            for (i = 0; i < d.list.length; i++) {
                var got = uiDescToIr(ts, d.list[i], site, ctx);
                if (got) { return got; }
            }
            return d.def ? uiDescToIr(ts, d.def, site, ctx) : null;
        }
        if (d.t === 'present') {
            return uiDescToIr(ts, has(site.opts, d.name) ? d.then : d.otherwise, site, ctx);
        }
        if (d.t === 'parentWidget') {
            if (has(site.optIr, 'parent')) { return site.optIr.parent; }
            if (has(site.opts, 'parent')) { return uiOptIr(ts, site.opts.parent, ctx); }
            return { k: 'call', fn: 'GetUIRoot', args: [] };
        }
        return null;
    }

    /* Does evaluating this touch anything the game can see? Reads of a variable
     * or a slot of one do not, so an option built out of those needs no
     * temporary and no slot: nothing can tell when it happened. */
    var UI_PURE_CALLS = {
        ValueInArray: 1, CountOf: 1, EmptyArray: 1, AppendToArray: 1, FirstOf: 1,
        LastOf: 1, IndexOfArrayValue: 1, Not: 1, And: 1, Or: 1, Equals: 1,
        NotEqualTo: 1, GreaterThan: 1, GreaterThanEqualTo: 1, LessThan: 1,
        LessThanEqualTo: 1, Add: 1, Subtract: 1, Multiply: 1, Divide: 1,
        Modulo: 1, IfThenElse: 1, AbsoluteValue: 1, Floor: 1, Ceiling: 1,
        RoundToInteger: 1, Max: 1, SquareRoot: 1, RaiseToPower: 1
    };

    function uiIrIsObservable(ir) {
        if (!ir || typeof ir !== 'object') { return false; }
        if (ir.k === 'call' && !has(UI_PURE_CALLS, ir.fn)) { return true; }
        var args = ir.args || [], i;
        for (i = 0; i < args.length; i++) {
            if (uiIrIsObservable(args[i])) { return true; }
        }
        return false;
    }

    /* Temporaries for hoisted option values. They die at the AddUI* call, so
     * one small pool per function serves every construction in it. */
    /* CAN THIS BE EVALUATED WHEN IT WAS NOT CHOSEN.
     *
     * Portal's IfThenElse, And and Or are VALUE blocks: they evaluate every
     * operand and then pick. The local SDK says so directly above IfThenElse in
     * index.d.ts. For a value that is just a value, that is harmless and much
     * cheaper than building control flow.
     *
     * It stops being harmless the moment an operand DOES something. Written
     *
     *     let x = enabled ? mod.RandomReal(1, 10) : 5;
     *
     * the original makes no random call when enabled is false and the converted
     * version makes one, which shifts every later random number by one and is
     * invisible in the report. The same applies to the right hand side of &&
     * and ||, which JavaScript promises not to evaluate at all.
     *
     * So purity decides: pure operands keep the cheap value block, impure ones
     * become real control flow.
     */
    function isPureExpr(ts, e) {
        if (!e) { return true; }
        if (ts.isCallExpression(e) || ts.isNewExpression(e) ||
            ts.isAwaitExpression(e) || ts.isYieldExpression(e) ||
            ts.isTaggedTemplateExpression(e)) {
            return false;
        }
        if (ts.isBinaryExpression(e)) {
            /* An assignment is an effect wherever it appears. */
            if (e.operatorToken.kind >= ts.SyntaxKind.FirstAssignment &&
                e.operatorToken.kind <= ts.SyntaxKind.LastAssignment) {
                return false;
            }
            return isPureExpr(ts, e.left) && isPureExpr(ts, e.right);
        }
        if (ts.isPrefixUnaryExpression(e) || ts.isPostfixUnaryExpression(e)) {
            /* ++ and -- write. */
            if (e.operator === ts.SyntaxKind.PlusPlusToken ||
                e.operator === ts.SyntaxKind.MinusMinusToken) {
                return false;
            }
            return isPureExpr(ts, e.operand);
        }
        if (ts.isConditionalExpression(e)) {
            return isPureExpr(ts, e.condition) && isPureExpr(ts, e.whenTrue) &&
                   isPureExpr(ts, e.whenFalse);
        }
        if (ts.isParenthesizedExpression(e)) { return isPureExpr(ts, e.expression); }
        if (ts.isPropertyAccessExpression(e)) { return isPureExpr(ts, e.expression); }
        if (ts.isElementAccessExpression(e)) {
            return isPureExpr(ts, e.expression) && isPureExpr(ts, e.argumentExpression);
        }
        if (ts.isArrayLiteralExpression(e)) {
            for (var i = 0; i < e.elements.length; i++) {
                if (!isPureExpr(ts, e.elements[i])) { return false; }
            }
            return true;
        }
        if (ts.isObjectLiteralExpression(e)) {
            for (var j = 0; j < e.properties.length; j++) {
                var p = e.properties[j];
                if (p.initializer && !isPureExpr(ts, p.initializer)) { return false; }
            }
            return true;
        }
        return true;
    }

    /* Build "decide, then use" out of a choice whose arms cannot both run.
     * Returns the expression that reads the result, having pushed the decision
     * into the statement prelude. */
    function chooseIntoTemp(ts, ctx, condIr, whenTrueNode, whenFalseNode, label) {
        var tmp = tempRef(ctx, label || 'choice');
        var thenBody = [{ k: 'setVar', ref: tmp, value: exprFromTs(ts, whenTrueNode, ctx) }];
        var elseBody = [{ k: 'setVar', ref: tmp, value: exprFromTs(ts, whenFalseNode, ctx) }];
        ctx.pre.push({ k: 'if', branches: [{ cond: condIr, body: thenBody }],
                       elseBody: elseBody });
        return { k: 'getVar', ref: tmp };
    }

    /* A LOOP CONDITION CANNOT USE THE PRELUDE.
     *
     * The prelude is spliced in front of the statement that needed it, which is
     * correct everywhere except a loop test: hoisting it there would evaluate
     * the operand once before the loop instead of on every pass. Rather than
     * lower it wrongly and say nothing, those report. */
    function canUsePrelude(ctx) { return !!ctx.pre && !ctx.inLoopTest; }

    function uiArgTemp(ctx, i) {
        if (!ctx.uiArgPool) { ctx.uiArgPool = []; }
        if (!ctx.uiArgPool[i]) { ctx.uiArgPool[i] = tempRef(ctx, 'uiarg'); }
        return ctx.uiArgPool[i];
    }

    /* An option value written at the call site. UI.ROOT_NODE is the one name
     * that is not an ordinary expression: it stands for the widget the whole
     * tree hangs from. */
    function uiImportIsResolved(ts, st, program) {
        if (!program.uiSpecs || !st.moduleSpecifier) { return false; }
        var from = st.moduleSpecifier.text || '';
        if (from.indexOf('bf6-portal-utils/ui') !== 0) { return false; }
        var named = st.importClause && st.importClause.namedBindings;
        if (!named || !named.elements || !named.elements.length) { return false; }
        for (var i = 0; i < named.elements.length; i++) {
            var local = named.elements[i].name.text;
            if (!has(program.uiSpecs, local) &&
                !(program.uiColorTables && has(program.uiColorTables, local))) { return false; }
        }
        return true;
    }

    /* `UI.COLORS.<NAME>` and `UI.ROOT_NODE` written at a call site. */
    function uiNamespaceValue(ts, e, ctx) {
        if (!ctx.program || !ctx.program.uiColorTables) { return null; }
        var tables = ctx.program.uiColorTables;
        if (ts.isIdentifier(e.expression) && e.name.text === 'ROOT_NODE' &&
            has(tables, e.expression.text)) {
            return { k: 'call', fn: 'GetUIRoot', args: [] };
        }
        var owner = e.expression;
        if (!ts.isPropertyAccessExpression(owner) || owner.name.text !== 'COLORS') { return null; }
        if (!ts.isIdentifier(owner.expression)) { return null; }
        var ns = owner.expression.text;
        if (!has(tables, ns)) { return null; }
        return uiColorIr(ns, e.name.text, ctx);
    }

    function uiOptIr(ts, node, ctx) {
        if (ts.isPropertyAccessExpression(node) && node.name.text === 'ROOT_NODE') {
            return { k: 'call', fn: 'GetUIRoot', args: [] };
        }
        return exprFromTs(ts, node, ctx);
    }

    function uiNewToIr(ts, e, ctx, line) {
        var spec = uiSpecFor(ts, e, ctx);
        if (!spec) { return null; }
        if (!ctx.pre) {
            reportUnconvertible(ctx.report, ctx.file, line,
                'new ' + spec.label + ' outside a statement',
                'Building a widget takes several block statements, and a condition holds ' +
                'one value. Construct it in the rule body and test the result.');
            return { k: 'gap' };
        }
        var opts = uiOptionsOf(ts, e);
        if (!opts) {
            reportUnconvertible(ctx.report, ctx.file, line,
                'new ' + spec.label + ' with options that are not a plain object',
                'The converter reads the option names to fill in the AddUI* arguments, so ' +
                'the argument has to be an object literal without spreads or computed keys.');
            return { k: 'gap' };
        }
        /* A per receiver widget is named from a receiver id the library mints
         * at runtime, and mod.AddUI* takes the receiver as an optional trailing
         * argument that a block cannot leave off conditionally. Neither half
         * has a block form, so this is not attempted. */
        if (has(opts, 'receiver') || has(opts, 'player') || has(opts, 'team')) {
            reportUnconvertible(ctx.report, ctx.file, line,
                'new ' + spec.label + ' with a per receiver option',
                'Blocks cannot build the per receiver widget name the library generates. ' +
                'Create the widget for everyone, or add one widget per player with its own ' +
                'literal name.');
            return { k: 'gap' };
        }
        var i;
        for (i = 0; i < spec.required.length; i++) {
            if (!has(opts, spec.required[i])) {
                reportUnconvertible(ctx.report, ctx.file, line,
                    'new ' + spec.label + ' without ' + spec.required[i],
                    'That option has no default, so there is no argument to pass. Give it a value.');
                return { k: 'gap' };
            }
        }
        var name = ctx.program.uiSiteNames[ctx.file + ':' + e.pos];
        if (!name) {
            ctx.program.uiSpillNames = (ctx.program.uiSpillNames || 0) + 1;
            name = (spec.namePrefix || 'ui_') + 'x' + ctx.program.uiSpillNames;
        }
        /* THE OPTIONS ARE EVALUATED WHERE THEY ARE WRITTEN, NOT WHERE THE
         * CONSTRUCTOR USES THEM.
         *
         * `new UI.Text({ x: 0, message: mod.Message(k) })` builds the object
         * first, so the Message is made BEFORE the constructor's own
         * CreateVector calls - and the constructor puts the message in
         * argument eleven. Lowering each option in place would move those
         * calls behind the vectors, which is visible whenever an option does
         * real work. So anything with an effect is computed here, in the order
         * it was written, and the AddUI* call reads the result.
         */
        var optIr = {}, optNames = keysOf(opts), pool = 0;
        for (i = 0; i < optNames.length; i++) {
            var onode = opts[optNames[i]];
            /* A position/size record is read field by field further down, so
             * it is left as source rather than built into an array first. */
            if (ts.isObjectLiteralExpression(onode)) { continue; }
            var oir = uiOptIr(ts, onode, ctx);
            if (!oir) { continue; }
            if (uiIrIsObservable(oir)) {
                var tmp = uiArgTemp(ctx, pool++);
                ctx.pre.push({ k: 'setVar', ref: tmp, value: oir });
                oir = { k: 'getVar', ref: tmp };
            }
            optIr[optNames[i]] = oir;
        }

        var site = { opts: opts, optIr: optIr, name: name, spec: spec, fixed: {} };

        /* The parent widget is a constructor LOCAL - `const parent = params
         * .parent ? params.parent.widget : mod.GetUIRoot()` - so it is worked
         * out before the AddUI* call, not in the middle of its argument list.
         * Left inline, the GetUIRoot of a root parented widget landed after
         * the two CreateVector calls the original makes after it. */
        for (i = 0; i < spec.args.length; i++) {
            var pd = spec.args[i];
            if (pd.t !== 'parentWidget' && pd.t !== 'present') { continue; }
            var pir = uiDescToIr(ts, pd, site, ctx);
            if (!pir) { continue; }
            if (uiIrIsObservable(pir)) {
                var ptmp = uiArgTemp(ctx, pool++);
                ctx.pre.push({ k: 'setVar', ref: ptmp, value: pir });
                pir = { k: 'getVar', ref: ptmp };
            }
            site.fixed[i] = pir;
        }

        var args = [];
        for (i = 0; i < spec.args.length; i++) {
            var a = has(site.fixed, i) ? site.fixed[i] : uiDescToIr(ts, spec.args[i], site, ctx);
            if (!a) {
                reportUnconvertible(ctx.report, ctx.file, line,
                    'new ' + spec.label + ': argument ' + i + ' of ' + spec.fn,
                    'The constructor computes that argument from something with no block ' +
                    'form. Pass the value the option takes directly.');
                return { k: 'gap' };
            }
            args.push(a);
        }
        if (!ctx.program.uiNamedWarned) {
            ctx.program.uiNamedWarned = true;
            warn(ctx.report, 'UI widgets are named once per construction in the source, ' +
                'because Portal takes a widget name as a literal and the component ' +
                'libraries build theirs from a counter. A construction that runs twice ' +
                'reuses its name instead of making a second widget.');
        }
        ctx.pre.push({ k: 'call', fn: spec.fn, args: args });
        return { k: 'call', fn: 'FindUIWidgetWithName', args: [{ k: 'str', v: name }] };
    }

    /* ---- lowering a call on a component -------------------------------- */

    var UI_NOT_A_COMPONENT = { mod: 1, rt: 1, modlib: 1, subs: 1, Math: 1, Object: 1,
                               JSON: 1, Events: 1, Timers: 1, console: 1, vars: 1 };

    function uiKindOf(ts, node, ctx, method) {
        if (!ctx.program || !ctx.program.uiSpecs) { return null; }
        /* A call into a namespace the converter already dispatches on is never
         * a component method, whatever it is called. Without this, a spec with
         * a delete() claims Timers.delete(id) and the real call is lost. */
        if (ts.isIdentifier(node) && has(UI_NOT_A_COMPONENT, node.text)) { return null; }
        var text = null;
        try { text = node.getText(ctx.sf).replace(/\s+/g, ''); } catch (e) { text = null; }
        if (text && has(ctx.program.uiKinds, text) && ctx.program.uiKinds[text] !== '?') {
            return ctx.program.uiKinds[text];
        }
        /* Nothing named the variable's type, so fall back to the method: it
         * only answers when exactly one known component has that method, which
         * is not a guess between two candidates.
         *
         * Only for a setter, because those names belong to the components.
         * delete/get/add/has are what a Map is asked for too, and a project
         * that imports one component would otherwise have every
         * `someMap.delete(id)` turned into DeleteUIWidget. Those need the
         * receiver to be a name something declared as a component. */
        if (!/^set[A-Z]/.test(method)) { return null; }
        var found = null, k;
        for (k in ctx.program.uiSpecs) {
            if (!has(ctx.program.uiSpecs, k)) { continue; }
            var sp = ctx.program.uiSpecs[k];
            if (has(sp.methods, method) || (sp.unreadable && has(sp.unreadable, method))) {
                if (found && found !== k) { return null; }
                found = k;
            }
        }
        return found;
    }

    function uiMethodToIr(ts, e, ctx, line) {
        if (!ctx.program || !ctx.program.uiSpecs) { return null; }
        var access = e.expression;
        if (!ts.isPropertyAccessExpression(access) || !access.name) { return null; }
        var method = access.name.text;
        var kind = uiKindOf(ts, access.expression, ctx, method);
        if (!kind || !has(ctx.program.uiSpecs, kind)) { return null; }
        var spec = ctx.program.uiSpecs[kind];
        if (!has(spec.methods, method)) {
            if (spec.unreadable && has(spec.unreadable, method)) {
                reportUnconvertible(ctx.report, ctx.file, line,
                    kind + '.' + method + '()',
                    'That method does more than call mod.* on its own widget, and blocks ' +
                    'have no object to keep the rest in. Inline what it does at the call site.');
                return [];
            }
            return null;
        }
        var recv = exprFromTs(ts, access.expression, ctx);
        if (!recv) { return null; }
        var m = spec.methods[method], calls = [], i, j;
        for (i = 0; i < m.calls.length; i++) {
            var args = [], bad = false;
            for (j = 0; j < m.calls[i].args.length; j++) {
                var d = m.calls[i].args[j], ir = null;
                ir = uiMethodArgIr(ts, d, e, recv, spec, ctx);
                if (!ir) { bad = true; break; }
                args.push(ir);
            }
            if (bad) {
                reportUnconvertible(ctx.report, ctx.file, line,
                    kind + '.' + method + '() needs remembered state',
                    'The method reads a value the widget was built with, and the ' +
                    'constructions in this program do not all pass the same one, so there ' +
                    'is no single number to use. Pass it as an argument instead.');
                return [];
            }
            calls.push({ k: 'call', fn: m.calls[i].fn, args: args });
        }
        /* `x?.setMessage(m)` does nothing when x is unset, and does not even
         * evaluate m. Emitting the call bare would run it on a widget that was
         * never created, so the guard is kept. */
        if (access.questionDotToken) {
            return { k: 'if', branches: [{ cond: recv, body: calls }], elseBody: null };
        }
        return calls.length === 1 ? calls[0] : calls;
    }

    function uiMethodArgIr(ts, d, e, recv, spec, ctx) {
        if (!d) { return null; }
        if (d.t === 'lit') { return d.ir; }
        if (d.t === 'recv') { return recv; }
        if (d.t === 'marg') { return exprFromTs(ts, e.arguments[d.i], ctx); }
        if (d.t === 'color') { return uiColorIr(d.ns, d.key, ctx); }
        if (d.t === 'field') {
            /* A constructor field the method reads. Only answerable when every
             * construction in the program passed the same literal. */
            var fc = spec.fieldConst || {};
            return has(fc, d.name) ? fc[d.name] : null;
        }
        if (d.t === 'call') {
            var args = [], i;
            for (i = 0; i < d.args.length; i++) {
                var a = uiMethodArgIr(ts, d.args[i], e, recv, spec, ctx);
                if (!a) { return null; }
                args.push(a);
            }
            return { k: 'call', fn: d.fn, args: args };
        }
        return null;
    }

    /* Round start fills the hoisted colour and root variables in. Called from
     * tsToIr straight after the module initialisers and before packing, so
     * these reads are packed with everything else. */
    function emitUiInit(program, report) {
        var init = program.uiColorInit || {}, stmts = [], ident;
        for (ident in init) {
            if (!has(init, ident)) { continue; }
            var rec = program.varByIdent[ident];
            if (!rec) { continue; }
            stmts.push({ k: 'setVar',
                ref: { k: 'varRef', name: rec.name, scope: rec.scope, ident: rec.ident,
                       slot: rec.slot, object: null },
                value: init[ident] });
        }
        if (stmts.length) { attachRoundStart(program, stmts, report); }
        report.counts.uiColors = stmts.length;
    }

    function scanTopLevel(ts, sf, fname, text, program, fnBodies, dispatchers, report, options) {
        var i;
        for (i = 0; i < sf.statements.length; i++) {
            var st = sf.statements[i];

            if (ts.isImportDeclaration(st)) {
                var spec = st.moduleSpecifier && st.moduleSpecifier.text ? st.moduleSpecifier.text : '';
                if (/^\.\/(runtime|variables|subroutines|rules-)/.test(spec) || spec === 'modlib') { continue; }
                /* A component import every name of which the converter resolves
                 * by itself. The module system is still not there; there is
                 * simply nothing left in this line to carry over, and a use of
                 * something it brings that is NOT resolved still reports at the
                 * point of use. */
                if (uiImportIsResolved(ts, st, program)) { continue; }
                reportUnconvertible(report, fname, lineOf(ts, sf, st),
                    'import from ' + JSON.stringify(spec),
                    'Block workspaces have no module system. Inline the helper as a subroutine, ' +
                    'or keep this file as TypeScript only code.');
                continue;
            }

            if (ts.isClassDeclaration(st)) {
                reportUnconvertible(report, fname, lineOf(ts, sf, st),
                    'class ' + (st.name ? st.name.text : '(anonymous)'),
                    'Blocks have no classes. Flatten the class into subroutines plus ' +
                    'Global or Player scoped variables.');
                continue;
            }

            if (ts.isInterfaceDeclaration(st) || ts.isTypeAliasDeclaration(st) || ts.isEnumDeclaration(st)) {
                reportUnconvertible(report, fname, lineOf(ts, sf, st),
                    'type level declaration ' + (st.name ? st.name.text : ''),
                    'Type declarations have no block form and are dropped. The runtime ' +
                    'behaviour is unaffected.');
                continue;
            }

            if (ts.isVariableStatement(st)) {
                scanVariableStatement(ts, sf, fname, st, program, report);
                continue;
            }

            if (ts.isFunctionDeclaration(st) && st.name) {
                var name = st.name.text;
                if (st.typeParameters && st.typeParameters.length) {
                    reportUnconvertible(report, fname, lineOf(ts, sf, st),
                        'generic function ' + name,
                        'Blocks have no generics. Give the subroutine concrete parameter types.');
                }
                fnBodies[name] = { node: st, file: fname, sf: sf, name: name };
                if (isDispatcher(ts, st, name)) {
                    dispatchers.push({ event: name, node: st, file: fname, sf: sf });
                }
                continue;
            }

            if (ts.isExpressionStatement(st)) {
                var ex = st.expression;
                /* Events.OnX.subscribe(handler) - a utils module subscription. */
                var sub = matchEventsSubscribe(ts, ex);
                if (sub) {
                    /* AN INLINE HANDLER IS STILL A HANDLER.
                     *
                     * A registration whose argument is an arrow with a real
                     * body used to arrive here with handler === null, get
                     * pushed as a dispatcher pointing at nothing, and then
                     * vanish: buildRuleFrom found no named function and
                     * returned null WITHOUT a diagnostic. Undead Ground Zero
                     * lost OnPlayerUIButtonEvent, OnRayCastHit and
                     * OnRayCastMissed that way - all of its class selection
                     * buttons and every raycast result - and nothing in the
                     * report mentioned them. Silent loss is worse than a
                     * refusal, because a refusal can be read.
                     *
                     * An arrow already carries what a rule needs: parameters
                     * and a body. So it is registered under a synthesised name
                     * and built exactly like a named handler.
                     */
                    if (!sub.handler && sub.arg &&
                        (ts.isArrowFunction(sub.arg) || ts.isFunctionExpression(sub.arg)) &&
                        sub.arg.body && ts.isBlock(sub.arg.body)) {
                        var inlineName = sub.event + '_inline';
                        var bump = 2;
                        while (has(fnBodies, inlineName)) {
                            inlineName = sub.event + '_inline' + bump; bump++;
                        }
                        fnBodies[inlineName] = { node: sub.arg, file: fname, sf: sf,
                                                 name: inlineName, inlineFor: sub.event };
                        sub.handler = inlineName;
                    }

                    if (!sub.handler) {
                        /* Still nothing to build from. Say so, by event name,
                         * so the author knows which registration was lost. */
                        reportUnconvertible(report, fname, lineOf(ts, sf, st),
                            'event registration ' + sub.event + ' with a handler that is ' +
                            'neither a named function nor an inline function body',
                            'Give ' + sub.event + ' a named handler function, or an inline ' +
                            'arrow with a { } body. This registration was NOT converted.');
                        continue;
                    }
                    dispatchers.push({ event: sub.event, subscribeHandler: sub.handler, file: fname, sf: sf, node: st });
                    continue;
                }
                reportUnconvertible(report, fname, lineOf(ts, sf, st),
                    'top level statement ' + shortText(ts, sf, st),
                    'Move the work into an Ongoing Global rule; block workspaces have no ' +
                    'module initialisation phase.');
                continue;
            }

            /* A component namespace whose constructors were all read is not a
             * gap: every `new` from it is lowered into the AddUI* call the
             * constructor makes, so the declaration itself has nothing left
             * to carry over. */
            if (ts.isModuleDeclaration(st) && st.name && ts.isIdentifier(st.name) &&
                program.uiNamespaces && has(program.uiNamespaces, st.name.text)) {
                continue;
            }

            reportUnconvertible(report, fname, lineOf(ts, sf, st),
                'top level ' + ts.SyntaxKind[st.kind],
                'Not part of the block model. Rewrite as a rule or a subroutine.');
        }
    }

    function shortText(ts, sf, node) {
        var t;
        try { t = node.getText(sf); } catch (e) { t = ''; }
        t = t.replace(/\s+/g, ' ');
        return t.length > 70 ? t.slice(0, 67) + '...' : t;
    }

    function matchEventsSubscribe(ts, ex) {
        if (!ts.isCallExpression(ex)) { return null; }
        var c = ex.expression;
        if (!ts.isPropertyAccessExpression(c) || c.name.text !== 'subscribe') { return null; }
        var inner = c.expression;
        if (!ts.isPropertyAccessExpression(inner)) { return null; }
        if (!ts.isIdentifier(inner.expression) || inner.expression.text !== 'Events') { return null; }
        /* THE HANDLER IS NOT ALWAYS A BARE NAME.
         *
         * This accepted only `Events.OnX.subscribe(handlerName)`. Real projects
         * write the wrapper form:
         *
         *     Events.OnPlayerJoinGame.subscribe((player) => {
         *         _OnPlayerJoinGame(player);
         *     });
         *
         * and with an arrow there the handler came back null, no rule was built,
         * and a 39 file project reported ZERO rules while happily lowering 456
         * functions that nothing could ever call. An arrow whose whole body is
         * one call to a named function is that function, so it is unwrapped.
         */
        var arg = ex.arguments && ex.arguments.length ? ex.arguments[0] : null;
        var handler = null;
        if (arg && ts.isIdentifier(arg)) {
            handler = arg.text;
        } else if (arg && (ts.isArrowFunction(arg) || ts.isFunctionExpression(arg))) {
            handler = soleCallName(ts, arg.body);
        }
        return { event: inner.name.text, handler: handler, arg: arg };
    }

    /* The name called by a body that does nothing but call one function.
     * Accepts both `() => f(x)` and `() => { f(x); }`. Returns null for a body
     * that does anything else, so a real inline handler is not mistaken for a
     * one line forwarder. */
    function soleCallName(ts, body) {
        var e = body;
        if (ts.isBlock(body)) {
            var real = [], i;
            for (i = 0; i < body.statements.length; i++) {
                if (!ts.isEmptyStatement(body.statements[i])) { real.push(body.statements[i]); }
            }
            if (real.length !== 1 || !ts.isExpressionStatement(real[0])) { return null; }
            e = real[0].expression;
        }
        if (!e || !ts.isCallExpression(e)) { return null; }
        return ts.isIdentifier(e.expression) ? e.expression.text : null;
    }

    function isDispatcher(ts, fn, name) {
        if (!EVENTS || !has(EVENTS, name)) { return /^(On[A-Z]|Ongoing[A-Z])/.test(name) && !/_/.test(name); }
        return true;
    }

    var ctx_varMeta = null;
    /* Which type names name a UI component, for the one place that reads a
     * type annotation without a program to hand. Set by collectUiComponents. */
    var ctx_uiSpecNames = null;

    function scanVariableStatement(ts, sf, fname, st, program, report) {
        var decls = st.declarationList.declarations, i;
        for (i = 0; i < decls.length; i++) {
            var d = decls[i];
            if (!ts.isIdentifier(d.name)) {
                reportUnconvertible(report, fname, lineOf(ts, sf, st),
                    'destructuring declaration', 'Declare one Portal variable per name.');
                continue;
            }
            var ident = d.name.text;
            var m = /^(.*?)(Global|Player|Team)Var$/.exec(ident);
            if (!m) {
                /* A MODULE BINDING IS A GLOBAL NOBODY NAMED CORRECTLY.
                 *
                 * This demanded the <Name>GlobalVar convention and rejected
                 * everything else, so a plain `let gameStarted = false` was
                 * dropped AND every read of it failed separately afterwards.
                 * It is a workspace variable; the convention only tells the
                 * reader which SCOPE was wanted, and Global is the right
                 * default.
                 *
                 * The initial value is a separate problem: blocks have no
                 * module load phase, so it is collected here and run at the
                 * start of the round rather than quietly lost.
                 */
                /* Pure data that nothing writes to is inlined at every use, so
                 * it needs neither a variable nor a round start initialiser. */
                if (program.constRecords && has(program.constRecords, ident)) { continue; }
                /* A short name for a mod path is followed at every use, so it
                 * needs no slot and no round start initialiser either. */
                if (program.modAliases && has(program.modAliases, ident)) { continue; }
                /* Lives in the object scope, one slot per key object, so it takes
                 * no Global slot and has no round start value to set. */
                if (program.collections && has(program.collections, ident)) { continue; }
                /* One slot per object rather than one Global array. */
                if (program.objTables && has(program.objTables, ident)) { continue; }
                addVariable(program, ident, 'Global', nextSlot(program, 'Global'), ident);
                if (d.initializer) {
                    program.moduleInit = program.moduleInit || [];
                    program.moduleInit.push({ ident: ident, node: d.initializer,
                                              file: fname, sf: sf });
                }
                continue;
            }
            var slot = 0;
            if (d.initializer) {
                if (ts.isNumericLiteral(d.initializer)) { slot = Number(d.initializer.text); }
                else if (ts.isCallExpression(d.initializer) && d.initializer.arguments.length &&
                    ts.isNumericLiteral(d.initializer.arguments[0])) {
                    slot = Number(d.initializer.arguments[0].text);
                }
            }
            var vm = ctx_varMeta && ctx_varMeta[ident] ? ctx_varMeta[ident] : null;
            addVariable(program, vm && vm.name !== undefined ? vm.name : m[1],
                vm && vm.scope ? vm.scope : m[2], slot, ident);
        }
    }

    function addVariable(program, name, scope, slot, ident) {
        if (has(program.varByIdent, ident)) { return program.varByIdent[ident]; }
        var rec = { name: name, scope: scope, slot: slot, ident: ident, id: null };
        program.variables.push(rec);
        program.varByIdent[ident] = rec;
        return rec;
    }

    /* ================================================================== *
     * SLOT ALLOCATION
     *
     * Portal has room for a few dozen variables per scope. Giving every
     * TypeScript local a slot of its own minted 531 globals on one real
     * project, which the site refuses outright.
     *
     * A frame slot only has to hold its value while its function is running,
     * so two functions can SHARE a slot when neither can be on the stack
     * while the other is. That is the whole rule, with two exceptions that
     * matter:
     *
     *  - CALLERS AND CALLEES INTERFERE. When f calls g, the locals of f are
     *    still live while g runs. Interference is the transitive call
     *    closure, not just direct calls.
     *
     *  - A FUNCTION THAT YIELDS INTERFERES WITH ITSELF. Wait suspends a rule,
     *    While is documented as REQUIRING a Wait, and Ongoing rules run one
     *    activation per player or team, so several activations of one
     *    yielding function are live at once and must not share. Those keep
     *    private slots.
     *
     * Recursion would break the scheme, so a cycle in the call graph is
     * reported and every function in it kept private rather than quietly
     * miscompiled.
     * ================================================================== */
    /* NO SUBROUTINE CALL MAY SURVIVE IN A VALUE SOCKET. ANYWHERE.
     *
     * subroutineInstanceBlock has no output, so one of these left in a value
     * input makes the whole workspace unloadable - not the one rule, the whole
     * file. The statement lowering already turns a call used as a value into a
     * call followed by a read of its return slot, but three paths never went
     * through it: module initialisers, rule conditions, and subroutine
     * conditions. 336 survived in one project and emptied the canvas.
     *
     * This is the guarantee rather than another place that remembers. Every
     * statement list gets the call hoisted in front of it. A CONDITION cannot:
     * there is nowhere to put a statement before it, so the call is reported
     * and the socket is left open, which is at least visible and loads.
     */
    /* EVERY SOCKET THAT WANTS A BOOL MUST BE GIVEN ONE.
     *
     * JavaScript tests anything: `if (count)`, `while (queue.length)`,
     * `secondsLeft || 60`. Portal tests only Bool, and one Number in a Bool
     * socket makes the editor refuse the WHOLE workspace:
     *
     *   GetMatchTimeElapsed expected Number, found Boolean
     *
     * Fixing that at each operator as it turned up was losing: the same clash
     * arrives from if statements, while loops, ternaries, Not, and rule
     * conditions. The catalogue already says which sockets take a Bool, so
     * every call is checked once, here, against what it actually declares.
     */
    /* WHICH SOCKETS REALLY TAKE A BOOL.
     *
     * Not read from the catalogue. An overloaded block has all of its
     * signatures concatenated into one params string there, so GetSoldierState
     * looks as though it takes a Bool and its perfectly good argument gets
     * rewritten - which is exactly what broke two round trip tests. These four
     * are the logical blocks, they have one signature each, and they are where
     * a JavaScript truthiness test actually lands. */
    var BOOL_SOCKETS = {
        IfThenElse: [1, 0, 0],
        Not: [1],
        And: [1, 1],
        Or: [1, 1]
    };

    /* A WRONG TYPE IN A SOCKET COSTS THE WHOLE FILE, SO NONE MAY LEAVE HERE.
     *
     * Blockly refuses an entire workspace on the first connection it cannot
     * make. One `'Round ' + n` in twenty thousand lines and nothing opens at
     * all - no rules, no subroutines, an empty canvas and a message with no
     * detail in it. Six separate clashes were found this way, one editor round
     * trip each, and each fix only revealed the next.
     *
     * This is the guarantee instead. Every socket whose type is known, given a
     * value whose type is known and different, is emptied and reported. The
     * mod then opens with a visible hole exactly where the converter could not
     * express something, which is the honest outcome and an enormously better
     * one than a blank canvas.
     *
     * Only where BOTH types are certain. An overloaded block has all of its
     * signatures concatenated in the catalogue, so cleanRet and cleanParam
     * refuse to answer for those rather than guess - a wrong guess here would
     * empty a socket that was perfectly good.
     */
    function ensureSocketTypes(program, report) {
        var i, removed = 0;

        function ctxFor(where) {
            return { program: program, report: report, file: where || '', sf: null,
                     params: [], locals: {}, fnName: '', temps: 0, objOfId: {} };
        }

        /* The accepted types of one value socket, or null when the catalogue
         * cannot say plainly. A union is a real answer; a run together set of
         * overload signatures is not. */
        /* What this socket will accept, straight from the block definition. */
        function socketCheck(fn, index) {
            var r = CHECKS[fn];
            if (!r) { return null; }
            return r.inputs['VALUE-' + index] || null;
        }
        /* What this value offers. */
        function outputCheck(ir) {
            if (!ir) { return null; }
            if (ir.k === 'num') { return ['Number']; }
            if (ir.k === 'str') { return ['String']; }
            if (ir.k === 'bool') { return ['Boolean']; }
            if (ir.k === 'call') {
                var r = CHECKS[ir.fn];
                return (r && r.output) || null;
            }
            /* An enum member draws as its own block, named after the enum. It
             * offers that enum type and nothing else, so `slot || fallback`
             * puts one straight into a Boolean socket. */
            if (ir.k === 'enum') {
                var er = CHECKS[ir.blockType || (ir.enumName + 'Item')];
                return (er && er.output) || null;
            }
            return null;
        }

        /* What a value definitely is, or null. */
        function typeOf(ir) {
            if (!ir) { return null; }
            if (ir.k === 'num') { return 'Number'; }
            if (ir.k === 'str') { return 'String'; }
            if (ir.k === 'bool') { return 'Bool'; }
            if (ir.k === 'call') { return cleanRet(ir.fn); }
            return null;
        }

        function walk(node, ctx) {
            if (!node || typeof node !== 'object') { return; }
            if (node.length !== undefined && node.k === undefined) {
                for (var a = 0; a < node.length; a++) { walk(node[a], ctx); }
                return;
            }
            if (node.k === 'call' && node.args) {
                for (var q = 0; q < node.args.length; q++) {
                    var want = socketCheck(node.fn, q);
                    var got = outputCheck(node.args[q]);
                    if (!want || !got) { continue; }
                    var fits = false, w;
                    for (w = 0; w < got.length; w++) {
                        if (want.indexOf(got[w]) >= 0) { fits = true; break; }
                    }
                    if (!fits) {
                        reportUnconvertible(report, ctx.file, 0,
                            node.fn + ' given a ' + got.join(' or ') + ' where it takes ' +
                            want.join(' or '),
                            'The value and the socket are different types, and Portal will ' +
                            'not connect them. The socket is left empty so the rest of the ' +
                            'mod still opens.');
                        node.args[q] = { k: 'gap' };
                        removed++;
                    }
                }
            }
            var ks = keysOf(node), n;
            for (n = 0; n < ks.length; n++) {
                var v = node[ks[n]];
                if (v && typeof v === 'object') { walk(v, ctx); }
            }
        }

        for (i = 0; i < program.rules.length; i++) {
            var r = program.rules[i], rc = ctxFor(r.file || 'a rule');
            walk(r.actions, rc); walk(r.conditions, rc);
        }
        for (i = 0; i < program.subroutines.length; i++) {
            var s = program.subroutines[i], sc = ctxFor(s.file || 'a subroutine');
            walk(s.actions, sc); walk(s.conditions, sc);
        }
        for (i = 0; i < (program.orphans || []).length; i++) {
            walk(program.orphans[i], ctxFor('an orphan'));
        }
        report.counts.socketsEmptied = removed;
    }

    function ensureBooleanSockets(program, report) {
        var i;

        function ctxFor(where) {
            return { program: program, report: report, file: where || '', sf: null,
                     params: [], locals: {}, fnName: '', temps: 0, objOfId: {} };
        }

        function walk(node, ctx) {
            if (!node || typeof node !== 'object') { return; }
            if (node.length !== undefined && node.k === undefined) {
                for (var a = 0; a < node.length; a++) { walk(node[a], ctx); }
                return;
            }
            /* An if or a while is its own IR node, not a call, so the check
             * below never reached the one socket that matters on it. `If` is
             * also absent from the catalogue, so nothing else typed it either,
             * and `if (eventPlayer)` reached the editor as a Player in a Bool
             * socket and refused the file. */
            if (node.k === 'if' && node.branches) {
                for (var bi = 0; bi < node.branches.length; bi++) {
                    var br = node.branches[bi];
                    if (br) { br.cond = asBoolean(br.cond, ctx, 0); }
                }
            }
            if (node.k === 'while' && node.cond) {
                node.cond = asBoolean(node.cond, ctx, 0);
            }
            if (node.k === 'call' && node.args && has(BOOL_SOCKETS, node.fn)) {
                var want = BOOL_SOCKETS[node.fn];
                for (var q = 0; q < node.args.length; q++) {
                    if (want[q]) { node.args[q] = asBoolean(node.args[q], ctx, 0); }
                }
            }
            var ks = keysOf(node), n;
            for (n = 0; n < ks.length; n++) {
                var v = node[ks[n]];
                if (v && typeof v === 'object') { walk(v, ctx); }
            }
        }

        /* A CONDITION IS ITSELF A BOOL SOCKET.
         *
         * conditionBlock takes one, and a rule condition is written straight
         * into it. Coercing only the ARGUMENTS of And, Or and the rest missed
         * the outermost expression, so `GetMatchTimeElapsed()` used as a rule
         * condition still arrived as a Number in a Bool socket and refused the
         * whole workspace. */
        function fixConditions(list, ctx) {
            if (!list) { return; }
            for (var q = 0; q < list.length; q++) { list[q] = asBoolean(list[q], ctx, 0); }
        }

        for (i = 0; i < program.rules.length; i++) {
            var r = program.rules[i], rc = ctxFor(r.file || 'a rule');
            walk(r.actions, rc);
            walk(r.conditions, rc);
            fixConditions(r.conditions, rc);
        }
        for (i = 0; i < program.subroutines.length; i++) {
            var s = program.subroutines[i], sc = ctxFor(s.file || 'a subroutine');
            walk(s.actions, sc);
            walk(s.conditions, sc);
            fixConditions(s.conditions, sc);
        }
        for (i = 0; i < (program.orphans || []).length; i++) {
            walk(program.orphans[i], ctxFor('an orphan'));
        }
    }

    function ensureNoValueSubCalls(program, report) {
        var i;

        function ctxFor(where) {
            return { program: program, report: report, file: where || '', sf: null,
                     params: [], locals: {}, fnName: '', temps: 0, objOfId: {} };
        }

        /* Hoist out of a statement list, innermost call first. */
        function sweepStmts(list, ctx) {
            if (!list || !list.length) { return list; }
            var out = [], q;
            for (q = 0; q < list.length; q++) {
                var pre = [];
                spillSubCalls(list[q], ctx, pre);
                if (pre.length) { out = out.concat(pre); }
                out.push(list[q]);
                /* A nested body is a statement list of its own. */
                var s = list[q], b;
                if (s && s.k === 'if') {
                    for (b = 0; b < (s.branches || []).length; b++) {
                        s.branches[b].body = sweepStmts(s.branches[b].body, ctx);
                    }
                    s.elseBody = sweepStmts(s.elseBody, ctx);
                } else if (s && (s.k === 'for' || s.k === 'while')) {
                    s.body = sweepStmts(s.body, ctx);
                }
            }
            return out;
        }

        /* A condition has no statement in front of it to hoist into. */
        function sweepConds(list, ctx, what) {
            var found = 0;
            function walk(node) {
                if (!node || typeof node !== 'object') { return node; }
                if (node.length !== undefined && node.k === undefined) {
                    for (var a = 0; a < node.length; a++) { node[a] = walk(node[a]); }
                    return node;
                }
                var ks = keysOf(node), n;
                for (n = 0; n < ks.length; n++) {
                    var v = node[ks[n]];
                    if (v && typeof v === 'object') { node[ks[n]] = walk(v); }
                }
                if (node.k === 'subCall') {
                    found++;
                    reportUnconvertible(report, what, 0,
                        'subroutine ' + node.name + '() called inside a condition',
                        'A subroutine call is a statement in blocks and cannot be part of a ' +
                        'test. Call it in the actions above and keep the answer in a ' +
                        'variable, then test that variable here.');
                    return { k: 'gap' };
                }
                return node;
            }
            walk(list);
            return found;
        }

        for (i = 0; i < program.rules.length; i++) {
            var r = program.rules[i];
            var rc = ctxFor(r.file || 'a rule');
            r.actions = sweepStmts(r.actions, rc);
            sweepConds(r.conditions, rc, r.file || 'a rule');
        }
        for (i = 0; i < program.subroutines.length; i++) {
            var s2 = program.subroutines[i];
            var sc = ctxFor(s2.file || 'a subroutine');
            s2.actions = sweepStmts(s2.actions, sc);
            sweepConds(s2.conditions, sc, s2.file || 'a subroutine');
        }
        for (i = 0; i < (program.orphans || []).length; i++) {
            var o = program.orphans[i];
            if (o && o.actions) { o.actions = sweepStmts(o.actions, ctxFor('an orphan')); }
        }
    }

    function allocateSlots(program, report) {
        if (!program.frameVars || !program.frameVars.length) { return; }

        var calls = {}, yields = {}, fns = {}, waitFns = {}, whileFns = {};
        function noteFn(n) { if (!has(fns, n)) { fns[n] = 1; calls[n] = {}; } }
        function scan(fnName, stmts) {
            var i, s, b;
            for (i = 0; i < (stmts || []).length; i++) {
                s = stmts[i];
                if (!s || typeof s !== 'object') { continue; }
                if (s.k === 'sub' && s.name) { calls[fnName][sanitizeIdent(s.name)] = 1; }
                if (s.k === 'wait') { yields[fnName] = 1; waitFns[fnName] = 1; }
                if (s.k === 'while') { yields[fnName] = 1; whileFns[fnName] = 1; }
                if (s.k === 'if') {
                    for (b = 0; b < (s.branches || []).length; b++) { scan(fnName, s.branches[b].body); }
                    scan(fnName, s.elseBody);
                } else if (s.k === 'for' || s.k === 'while') {
                    scan(fnName, s.body);
                }
            }
        }
        var i, j;
        for (i = 0; i < program.subroutines.length; i++) { noteFn(sanitizeIdent(program.subroutines[i].name)); }
        for (i = 0; i < program.rules.length; i++) { noteFn(sanitizeIdent(program.rules[i].fnName || ('rule' + i))); }
        for (i = 0; i < program.frameVars.length; i++) { noteFn(program.frameVars[i].fn); }
        for (i = 0; i < program.subroutines.length; i++) {
            scan(sanitizeIdent(program.subroutines[i].name), program.subroutines[i].actions);
        }
        for (i = 0; i < program.rules.length; i++) {
            var rn = sanitizeIdent(program.rules[i].fnName || ('rule' + i));
            scan(rn, program.rules[i].actions);
            scan(rn, program.rules[i].conditions);
        }

        var names = keysOf(fns);
        var reach = {}, cyclic = {};
        for (i = 0; i < names.length; i++) {
            var seen = {}, stack = keysOf(calls[names[i]] || {});
            while (stack.length) {
                var t = stack.pop();
                if (has(seen, t)) { continue; }
                seen[t] = 1;
                var more = keysOf(calls[t] || {});
                for (j = 0; j < more.length; j++) { stack.push(more[j]); }
            }
            reach[names[i]] = seen;
            if (has(seen, names[i])) { cyclic[names[i]] = 1; }
        }
        var cyc = keysOf(cyclic);
        if (cyc.length) {
            warn(report, 'Recursive call cycle through ' + cyc.slice(0, 6).join(', ') +
                (cyc.length > 6 ? ' and ' + (cyc.length - 6) + ' more' : '') +
                '. Those keep private slots: a shared slot cannot survive re-entry.');
        }

        /* WHICH LOCALS ACTUALLY SURVIVE A WAIT.
         *
         * A function containing a Wait was treated as conflicting with every
         * other function, so all of its locals got private slots. That is far
         * stronger than the truth. Other rules run during a Wait and can reuse a
         * slot, so a local is only in danger if it is still needed AFTER the
         * wait. A function that fills an array, uses it, and then waits is not
         * holding anything across the pause and can share like any other.
         *
         * So the question moves from the function to the variable: referenced
         * before a wait AND referenced again after one. Anything referenced only
         * on one side of every wait is free.
         *
         * A loop containing a wait is the case that looks safe and is not: the
         * next iteration reads what the previous one wrote, with a pause in
         * between. Everything touched inside such a loop is treated as
         * surviving.
         */
        var crossesWait = {};
        (function findCrossing() {
            function identsIn(node, out) {
                if (!node || typeof node !== 'object') { return; }
                if (node.ident) { out[node.ident] = 1; }
                var ks = keysOf(node), n, q;
                for (n = 0; n < ks.length; n++) {
                    var v = node[ks[n]];
                    if (v && typeof v === 'object') {
                        if (v.length !== undefined && v.k === undefined) {
                            for (q = 0; q < v.length; q++) { identsIn(v[q], out); }
                        } else { identsIn(v, out); }
                    }
                }
            }
            function hasWait(list) {
                var i, s, b;
                for (i = 0; i < (list || []).length; i++) {
                    s = list[i];
                    if (!s || typeof s !== 'object') { continue; }
                    if (s.k === 'wait') { return true; }
                    if (s.k === 'if') {
                        for (b = 0; b < (s.branches || []).length; b++) {
                            if (hasWait(s.branches[b].body)) { return true; }
                        }
                        if (hasWait(s.elseBody)) { return true; }
                    } else if (s.k === 'for' || s.k === 'while') {
                        if (hasWait(s.body)) { return true; }
                    }
                }
                return false;
            }
            /* A WRITE AFTER THE WAIT KILLS WHATEVER WAS THERE BEFORE IT.
             *
             * Counting any mention on both sides of a wait marked 682 of 725
             * locals as surviving one, which is nearly all of them and plainly
             * wrong. `x = 1; use(x); wait; x = 2; use(x)` does not hold x across
             * anything: the value read afterwards was written afterwards.
             *
             * So reads and writes are separated. `fresh` marks a name whose
             * current value was written since the last wait; reading one of those
             * is reading something the pause cannot have touched. Passing a wait
             * clears every fresh mark, because from then on those values are old.
             */
            function classify(s, reads, writes) {
                if (s.k === 'setVar' && s.ref && s.ref.ident) {
                    writes[s.ref.ident] = 1;
                    identsIn(s.value, reads);
                    if (s.ref.object) { identsIn(s.ref.object, reads); }
                    return;
                }
                if (s.k === 'setVarAt' && s.ref && s.ref.ident) {
                    /* Changes one slot and keeps the rest, so the old array is
                     * still needed: this reads as well as writes. */
                    writes[s.ref.ident] = 1; reads[s.ref.ident] = 1;
                    identsIn(s.index, reads); identsIn(s.value, reads);
                    if (s.ref.object) { identsIn(s.ref.object, reads); }
                    return;
                }
                identsIn(s, reads);
            }

            function walk(list, state) {
                var i, s, b, k;
                for (i = 0; i < (list || []).length; i++) {
                    s = list[i];
                    if (!s || typeof s !== 'object') { continue; }
                    if (s.k === 'wait') { state.past = true; state.fresh = {}; continue; }

                    if (s.k === 'if') {
                        for (b = 0; b < (s.branches || []).length; b++) { walk(s.branches[b].body, state); }
                        walk(s.elseBody, state);
                        continue;
                    }
                    if (s.k === 'for' || s.k === 'while') {
                        /* Twice, so a value written on one pass and read on the
                         * next with a wait in between is caught. */
                        walk(s.body, state);
                        if (hasWait(s.body)) { walk(s.body, state); }
                        continue;
                    }

                    var reads = {}, writes = {};
                    classify(s, reads, writes);
                    for (k in reads) {
                        if (!has(reads, k)) { continue; }
                        if (state.past && state.seen[k] && !state.fresh[k]) { crossesWait[k] = 1; }
                        state.seen[k] = 1;
                    }
                    for (k in writes) {
                        if (!has(writes, k)) { continue; }
                        state.seen[k] = 1;
                        state.fresh[k] = 1;
                    }
                }
            }
            var a;
            for (a = 0; a < program.subroutines.length; a++) {
                walk(program.subroutines[a].actions, { past: false, seen: {}, fresh: {} });
            }
            for (a = 0; a < program.rules.length; a++) {
                walk(program.rules[a].actions, { past: false, seen: {}, fresh: {} });
            }
        })();

        /* TWO POOLS, BECAUSE ONLY ONE KIND OF SLOT IS SCARCE.
         *
         * Every local of a function used to take one slot out of a single
         * contiguous block, and functions that can be live together get disjoint
         * blocks. That is correct, and it costs far more than it needs to.
         *
         * Scalar slots are not scarce: packGlobals folds hundreds of them into
         * one variable holding an array. Array valued slots ARE scarce, because
         * an array cannot be packed into another array, so each one costs a real
         * variable out of the 128.
         *
         * Mixed together, the array locals inherit the width of the scalar
         * demand: one project needed 110 array slots when the most it ever had
         * live at once was far fewer. Split, the array pool is coloured against
         * array demand alone and the scalar pool can be as wide as it likes.
         */
        var arrayishIdents = markArrayishIdents(program);

        var byFn = {}, byFnArr = {}, byFnHold = {}, byFnArrHold = {}, order = [];
        for (i = 0; i < program.frameVars.length; i++) {
            var fv = program.frameVars[i];
            if (!has(byFn, fv.fn)) {
                byFn[fv.fn] = []; byFnArr[fv.fn] = [];
                byFnHold[fv.fn] = []; byFnArrHold[fv.fn] = [];
                order.push(fv.fn);
            }
            var isArr = has(arrayishIdents, fv.ident);
            var holds = has(crossesWait, fv.ident);
            if (isArr && holds) { byFnArrHold[fv.fn].push(fv.ident); }
            else if (isArr) { byFnArr[fv.fn].push(fv.ident); }
            else if (holds) { byFnHold[fv.fn].push(fv.ident); }
            else { byFn[fv.fn].push(fv.ident); }
        }
        order.sort(function (a, b) { return byFn[b].length - byFn[a].length; });

        /* ONE SLOT CANNOT HOLD TWO LIVE INVOCATIONS.
         *
         * A local that is still needed after a Wait gets a slot of its own, and
         * that slot belongs to the FUNCTION. Two invocations of the same
         * function are two different things needing the same slot:
         *
         *     export async function OnPlayerDeployed(eventPlayer) {
         *         const id = mod.GetObjId(eventPlayer);
         *         await mod.Wait(1);
         *         mod.SetGameModeTargetScore(id);      // both see the LAST id
         *     }
         *
         * Deploy two players before the first wait finishes and the original
         * reports 1 then 2 while the conversion reports 2 twice. Recursion has
         * the same shape.
         *
         * This is NOT fixed here, and it is worth saying why rather than
         * quietly shipping a guess. The fix is per-invocation storage, and the
         * only candidates Portal offers are subroutine parameters carried
         * through a continuation split. Whether a Portal subroutine gives each
         * live invocation its own parameter storage is UNPROVEN: no captured
         * export contains a recursive or re-entrant subroutine, and the editor
         * permits things the runtime does not honour (appending an array to an
         * array being the standing example). Building a continuation transform
         * on an unverified guarantee would replace a visible wrong answer with
         * a hidden one.
         *
         * So it is reported, by variable, with the function it belongs to.
         */
        (function warnAboutSharedFrames() {
            var perFn = {}, k, anyN = 0;
            for (k = 0; k < program.frameVars.length; k++) {
                var v = program.frameVars[k];
                if (!has(crossesWait, v.ident)) { continue; }
                /* Only a function that can be entered again while paused can
                 * collide: an event handler, or anything a cycle reaches. */
                if (!has(yields, v.fn)) { continue; }
                if (!perFn[v.fn]) { perFn[v.fn] = []; }
                /* local_<fn>_<name> back to just <name>, which is what the
                 * author wrote and the only part they can act on. */
                perFn[v.fn].push(String(v.ident).replace(/^local_[^_]*_/, ''));
                anyN++;
            }
            var fns = keysOf(perFn);
            if (!fns.length) { return; }
            report.counts.localsSharedAcrossInvocations = anyN;
            for (k = 0; k < fns.length; k++) {
                report.warnings.push(
                    fns[k] + ' keeps ' + perFn[fns[k]].length + ' value(s) across a Wait (' +
                    perFn[fns[k]].slice(0, 6).join(', ') +
                    '). If it runs again before the first pass finishes, both passes share ' +
                    'one slot and the earlier one sees the later one\'s value. Portal has no ' +
                    'per-invocation storage this converter can rely on, so guard the handler ' +
                    'or move the value into a Player scoped variable keyed on the player.');
            }
        }());

        /* `strong` is for locals that are still needed after a Wait. Those must
         * not share with anything that could run during the pause, which is
         * anything at all. Locals that do not survive a wait only have to avoid
         * the functions that can be on the stack with them. */
        function interferes(a, b, strong) {
            if (a === b) { return true; }
            if (has(cyclic, a) || has(cyclic, b)) { return true; }
            if (strong && (has(yields, a) || has(yields, b))) { return true; }
            return (reach[a] && has(reach[a], b)) || (reach[b] && has(reach[b], a));
        }

        /* Lay one pool out: each function gets a run of consecutive slots, and
         * two functions that can be live at the same time never overlap. */
        function layOut(sizes, strong) {
            var b = {}, placed = [], a, q;
            var ord = order.slice().sort(function (x, y) { return sizes[y].length - sizes[x].length; });
            for (a = 0; a < ord.length; a++) {
                var fn = ord[a], want = sizes[fn].length, at = 0, ok = false;
                if (!want) { b[fn] = 0; continue; }
                while (!ok) {
                    ok = true;
                    for (q = 0; q < placed.length; q++) {
                        var o = placed[q];
                        if (!interferes(fn, o.fn, strong)) { continue; }
                        if (at < o.base + o.size && o.base < at + want) {
                            ok = false; at = o.base + o.size; break;
                        }
                    }
                }
                b[fn] = at;
                placed.push({ fn: fn, base: at, size: want });
            }
            var w = 0;
            for (a = 0; a < placed.length; a++) { w = Math.max(w, placed[a].base + placed[a].size); }
            return { base: b, width: w };
        }

        report.counts.frameFns = order.length;
        report.counts.frameYielding = keysOf(yields).length;

        /* WHAT NATIVE BLOCKS CANNOT DO, said plainly.
         *
         * A Portal block workspace has no per-invocation storage. A handler
         * that WAITS is suspended, and an Ongoing rule runs one activation per
         * player, so two players inside the same handler share one set of
         * variable slots and overwrite each other. The behaviour suite proves
         * it: two players through a wait give [[1,510,2],[2,520,2]] as
         * TypeScript and [[2,520,2],[2,520,2]] as blocks - the first player's
         * state simply gone.
         *
         * The converter cannot fix that; it is what the block language is. What
         * it can do is refuse to let the result look trustworthy, so anything
         * that yields is named here and reported as unsafe rather than left for
         * somebody to discover in a live match.
         */
        if (keysOf(yields).length > 0) {
            var names = keysOf(yields).sort();
            report.warnings.push(
                'PER-PLAYER STATE IS NOT PRESERVED in ' + names.length + ' handler(s) that wait: '
                + names.slice(0, 6).join(', ') + (names.length > 6 ? ', and others' : '')
                + '. Native blocks have no per-invocation variables, so two players inside one of '
                + 'these at once share the same slots and overwrite each other. Keep these as '
                + 'TypeScript - script mode has real locals - rather than converting them to blocks.');
            report.counts.frameUnsafeForBlocks = names.length;
        }
        report.counts.frameWait = keysOf(waitFns).length;
        report.counts.frameWhile = keysOf(whileFns).length;
        report.counts.frameCyclic = keysOf(cyclic).length;

        var scal = layOut(byFn, false);
        var scalHold = layOut(byFnHold, true);
        var arrs = layOut(byFnArr, false);
        var arrsHold = layOut(byFnArrHold, true);
        var base = scal.base, width = scal.width;
        report.counts.frameScalarWidth = scal.width + scalHold.width;
        report.counts.frameArrayWidth = arrs.width + arrsHold.width;
        report.counts.frameHeldAcrossWait = keysOf(crossesWait).length;

        var pooled = [], k;
        for (k = 0; k < width; k++) {
            pooled.push({ name: 'frame' + k, scope: 'Global', slot: 0, ident: '__frame' + k, id: null });
        }
        /* The array pool is separate and deliberately named so, because these
         * are the slots that actually cost something. */
        function makePool(prefix, ident, n) {
            var out = [], q;
            for (q = 0; q < n; q++) {
                out.push({ name: prefix + q, scope: 'Global', slot: 0, ident: ident + q, id: null });
            }
            return out;
        }
        var pooledHold = makePool('frameHeld', '__frameheld', scalHold.width);
        var pooledArr = makePool('list', '__list', arrs.width);
        var pooledArrHold = makePool('listHeld', '__listheld', arrsHold.width);

        var remap = {};
        for (i = 0; i < order.length; i++) {
            var fnq = order[i], list, q2;
            list = byFn[fnq];
            for (j = 0; j < list.length; j++) { remap[list[j]] = pooled[scal.base[fnq] + j]; }
            list = byFnHold[fnq];
            for (j = 0; j < list.length; j++) { remap[list[j]] = pooledHold[scalHold.base[fnq] + j]; }
            list = byFnArr[fnq];
            for (j = 0; j < list.length; j++) { remap[list[j]] = pooledArr[arrs.base[fnq] + j]; }
            list = byFnArrHold[fnq];
            for (j = 0; j < list.length; j++) { remap[list[j]] = pooledArrHold[arrsHold.base[fnq] + j]; }
        }
        pooled = pooled.concat(pooledHold).concat(pooledArr).concat(pooledArrHold);

        var kept = [];
        for (i = 0; i < program.variables.length; i++) {
            if (!has(program.frameByIdent, program.variables[i].ident)) { kept.push(program.variables[i]); }
        }
        /* A SLOT WITH ONE OCCUPANT SHOULD WEAR ITS NAME.
         *
         * Portal has no local variables, so every local is given a slot in a
         * shared global pool. That part is necessary. Calling the slots list0,
         * list1, frame0 is not: most of them are only ever used by a single
         * variable from the source, and that variable had a perfectly good
         * name which was thrown away.
         *
         * Where a slot is shared by several, the generic name is honest and
         * stays. Where it is not, the reader gets undeadList back instead of
         * list22.
         */
        (function nameThePools() {
            var occupants = {}, k2;
            for (k2 in remap) {
                if (!has(remap, k2) || !remap[k2]) { continue; }
                var slot = remap[k2];
                var was = program.varByIdent && program.varByIdent[k2];
                var nm = (was && was.name) || String(k2).replace(/^__/, '');
                if (!occupants[slot.ident]) { occupants[slot.ident] = {}; }
                occupants[slot.ident][nm] = true;
            }
            var used = {};
            for (var p = 0; p < pooled.length; p++) {
                var names = keysOf(occupants[pooled[p].ident] || {});
                if (names.length !== 1) { continue; }
                var want = names[0];
                /* Two functions can each have their own i, and they may land in
                 * different slots; both cannot be called i. */
                if (has(used, want)) { continue; }
                used[want] = true;
                pooled[p].name = want;
            }
        }());

        program.variables = kept.concat(pooled);
        program.varByIdent = {};
        var slots = {};
        for (i = 0; i < program.variables.length; i++) {
            var v = program.variables[i];
            slots[v.scope] = (slots[v.scope] === undefined) ? 0 : slots[v.scope] + 1;
            v.slot = slots[v.scope];
            program.varByIdent[v.ident] = v;
        }

        /* A varRef carries its own copy of the name and slot, so the whole IR
         * has to be walked. Missing one leaves a block pointing at a variable
         * that no longer exists. */
        var moved = 0;
        function fix(node) {
            if (!node || typeof node !== 'object') { return; }
            if (node.k === 'varRef' && node.ident && has(remap, node.ident)) {
                var t = remap[node.ident];
                node.name = t.name; node.ident = t.ident; node.slot = t.slot; node.scope = t.scope;
                moved++;
            }
            var ks = keysOf(node), n, q;
            for (n = 0; n < ks.length; n++) {
                var val = node[ks[n]];
                if (val && typeof val === 'object') {
                    if (val.length !== undefined && val.k === undefined) {
                        for (q = 0; q < val.length; q++) { fix(val[q]); }
                    } else { fix(val); }
                }
            }
        }
        for (i = 0; i < program.rules.length; i++) { fix(program.rules[i]); }
        for (i = 0; i < program.subroutines.length; i++) { fix(program.subroutines[i]); }
        for (i = 0; i < (program.orphans || []).length; i++) { fix(program.orphans[i]); }

        report.counts.frameSlots = width;
        report.counts.frameLocals = program.frameVars.length;
        report.counts.framesMoved = moved;
    }

    /* ================================================================== *
     * ARRAY PACKING
     *
     * Slot reuse fixed the wrong half. Of 531 globals on one real project only
     * 79 were function locals; the rest are module level bindings that live
     * for the whole round and genuinely cannot share a slot with anything.
     *
     * They can share an ARRAY. Portal addresses array elements directly, so a
     * hundred separate globals become one array variable and a hundred
     * indices:
     *
     *     read   x        ->  ValueInArray(GetVariable(pack0), 7)
     *     write  x = v    ->  SetVariableAtIndex(pack0, 7, v)
     *
     * Two things this must not get wrong:
     *
     *  - ARRAYS DO NOT NEST. A variable that itself holds an array cannot go
     *    in a pack, so anything ever assigned an array, or ever used as the
     *    array argument of an array operation, is left alone. The test is
     *    deliberately generous: a wrongly packed array is silent corruption,
     *    a wrongly skipped one only costs a slot.
     *
     *  - THE PACK HAS TO EXIST BEFORE IT IS WRITTEN. SetVariableAtIndex on an
     *    array that is too short has no defined behaviour, so each pack is
     *    built to full length at the start of the round. That same rule is
     *    where module initialisers finally run - blocks have no module load
     *    phase, and until now those initial values were collected and never
     *    emitted.
     * ================================================================== */
    /* Wider than the variable ceiling on purpose: every extra pack costs a
     * variable, and with a flat initialiser the width costs only statements. */
    var PACK_WIDTH = 256;
    var ARRAY_MAKERS = {
        EmptyArray: 1, AppendToArray: 1, ArraySlice: 1, SortedArray: 1,
        FilteredArray: 1, MappedArray: 1, RandomizedArray: 1, RemoveFromArray: 1
    };
    var ARRAY_TAKERS = {
        AppendToArray: 1, RemoveFromArray: 1, ArraySlice: 1, SortedArray: 1,
        FilteredArray: 1, MappedArray: 1, RandomizedArray: 1, CountOf: 1,
        ValueInArray: 1, ArrayContains: 1, IndexOfArrayValue: 1, FirstOf: 1,
        LastOf: 1, RandomValueInArray: 1, IndexOfFirstTrue: 1
    };

    /* The compiler handle, so a pass that runs after parsing can still lower
     * an expression it kept a node for. */
    var TS_HANDLE = null;
    function getTs() { return TS_HANDLE; }

    /* MODULE INITIALISERS.
     *
     * `const WALL_WEAPONS = [...]` at the top of a file has no block form: a
     * workspace has no module load phase. The values were being recorded in
     * program.moduleInit and then quietly dropped, so every one of those
     * variables started the round empty and anything reading one got nothing.
     * The differential harness found it on the first run - a for-of over one
     * of them called CountOf(false).
     *
     * This runs BEFORE packing even though the values must end up AFTER the
     * pack construction, because attachRoundStart prepends: packing then puts
     * its array building in front of these, which is the order needed - a
     * packed variable is written with SetVariableAtIndex and the array has to
     * exist first.
     */
    function emitModuleInit(program, report) {
        if (!program.moduleInit || !program.moduleInit.length) { return; }
        var ictx = newTsCtx(program, report, 'module init', null, []);
        ictx.fnName = 'moduleInit';
        var stmts = [], i, skipped = 0;
        for (i = 0; i < program.moduleInit.length; i++) {
            var mi = program.moduleInit[i];
            var rec = program.varByIdent[mi.ident];
            if (!rec) { skipped++; continue; }
            ictx.file = mi.file;
            ictx.sf = mi.sf;
            var value = null;
            try { value = exprFromTs(getTs(), mi.node, ictx); } catch (e) { value = null; }
            if (!value) { skipped++; continue; }
            stmts.push({
                k: 'setVar',
                ref: { k: 'varRef', name: rec.name, scope: rec.scope,
                       ident: rec.ident, slot: rec.slot, object: null },
                value: value
            });
        }
        if (stmts.length) { attachRoundStart(program, stmts, report); }
        report.counts.moduleInit = stmts.length;
        if (skipped) {
            warn(report, skipped + ' module level initial value(s) had no block form and ' +
                'were not set at the start of the round.');
        }
    }

    /* Every variable that has ever held an array. Such a variable cannot be
     * folded into a pack, because a pack IS an array and Portal flattens an
     * array placed inside another one. Both packing passes ask the same
     * question, so they ask it in one place. */
    function markArrayishIdents(program) {
        var arrayish = {}, i;
        function mark(node) {
            if (!node || typeof node !== 'object') { return; }
            if (node.k === 'setVar' && node.ref && node.ref.ident) {
                if (node.value && node.value.k === 'call' && has(ARRAY_MAKERS, node.value.fn)) {
                    arrayish[node.ref.ident] = 1;
                }
            }
            if (node.k === 'setVarAt' && node.ref && node.ref.ident) {
                arrayish[node.ref.ident] = 1;
            }
            if (node.k === 'call' && has(ARRAY_TAKERS, node.fn)) {
                var a0 = (node.args || [])[0];
                if (a0 && a0.k === 'getVar' && a0.ref && a0.ref.ident) { arrayish[a0.ref.ident] = 1; }
            }
            var ks = keysOf(node), n, q;
            for (n = 0; n < ks.length; n++) {
                var v = node[ks[n]];
                if (v && typeof v === 'object') {
                    if (v.length !== undefined && v.k === undefined) {
                        for (q = 0; q < v.length; q++) { mark(v[q]); }
                    } else { mark(v); }
                }
            }
        }
        for (i = 0; i < program.rules.length; i++) { mark(program.rules[i]); }
        for (i = 0; i < program.subroutines.length; i++) { mark(program.subroutines[i]); }
        for (i = 0; i < (program.orphans || []).length; i++) { mark(program.orphans[i]); }
        return arrayish;
    }

    /* THE OBJECT SCOPE FILLS UP TOO, AND IT PACKS THE SAME WAY.
     *
     * Moving per player state where it belongs - collections keyed by a player,
     * records held per player - takes it out of the Global budget and puts it in
     * the object one, which is 128 shared by Player and Team. One real mod
     * reached 129 there, so the move alone is not enough.
     *
     * An object variable can hold an array, and SetVariableAtIndex takes a
     * Variable, which mod.ObjectVariable(obj, slot) is. So many per player
     * scalars fold into ONE slot holding a flat array:
     *
     *     GetVariable(ObjectVariable(p, 7))       -> ValueInArray(GetVariable(ObjectVariable(p, 0)), 7)
     *     SetVariable(ObjectVariable(p, 7), v)    -> SetVariableAtIndex(ObjectVariable(p, 0), 7, v)
     *
     * No array is ever placed inside another array here, which is the thing
     * Portal cannot do: the pack is one flat array of scalars held in a
     * variable, exactly like the Global packs.
     *
     * Object variables that already hold an array are left alone for that same
     * reason.
     */
    function packObjectVars(program, report, ceiling) {
        var i;

        var arrayish = markArrayishIdents(program);


        var objs = [];
        for (i = 0; i < program.variables.length; i++) {
            var v = program.variables[i];
            if (v.scope === 'Global') { continue; }
            if (has(arrayish, v.ident)) { continue; }
            objs.push(v);
        }
        var scopeTotal = 0;
        for (i = 0; i < program.variables.length; i++) {
            if (program.variables[i].scope !== 'Global') { scopeTotal++; }
        }
        if (scopeTotal <= ceiling) { return; }
        if (objs.length < 2) { return; }

        var where = {};
        for (i = 0; i < objs.length; i++) { where[objs[i].ident] = i; }

        /* One slot per object, whatever the object turns out to be. The scope
         * stays whatever the packed variables used, which is Player in every
         * case we have seen; a Team scoped one would need its own pack. */
        var packIdent = '__objpack';
        var packName = 'perObject';
        var packScope = objs[0].scope;
        function packRefFor(objectIr) {
            return { k: 'varRef', name: packName, scope: packScope,
                     ident: packIdent, slot: 0, object: objectIr || null };
        }

        var reads = 0, writes = 0, other = 0;
        function rewriteOne(node) {
            if (!node || typeof node !== 'object') { return node; }
            if (node.k === 'getVar' && node.ref && node.ref.ident && has(where, node.ref.ident)) {
                reads++;
                return { k: 'call', fn: 'ValueInArray', args: [
                    { k: 'getVar', ref: packRefFor(node.ref.object) },
                    { k: 'num', v: where[node.ref.ident] }
                ] };
            }
            if (node.k === 'setVar' && node.ref && node.ref.ident && has(where, node.ref.ident)) {
                writes++;
                return { k: 'setVarAt', ref: packRefFor(node.ref.object),
                         index: { k: 'num', v: where[node.ref.ident] }, value: node.value };
            }
            if (node.k === 'varRef' && node.ident && has(where, node.ident)) { other++; }
            return node;
        }
        function rewrite(node) {
            if (!node || typeof node !== 'object') { return; }
            var ks = keysOf(node), n, q;
            for (n = 0; n < ks.length; n++) {
                var v2 = node[ks[n]];
                if (!v2 || typeof v2 !== 'object') { continue; }
                if (v2.length !== undefined && v2.k === undefined) {
                    for (q = 0; q < v2.length; q++) { v2[q] = rewriteOne(v2[q]); rewrite(v2[q]); }
                } else {
                    node[ks[n]] = rewriteOne(v2);
                    rewrite(node[ks[n]]);
                }
            }
        }
        for (i = 0; i < program.rules.length; i++) { rewrite(program.rules[i]); }
        for (i = 0; i < program.subroutines.length; i++) { rewrite(program.subroutines[i]); }
        for (i = 0; i < (program.orphans || []).length; i++) { rewrite(program.orphans[i]); }

        var kept = [];
        for (i = 0; i < program.variables.length; i++) {
            if (!has(where, program.variables[i].ident)) { kept.push(program.variables[i]); }
        }
        kept.push({ name: packName, scope: packScope, slot: 0, ident: packIdent, id: null });
        program.variables = kept;
        program.varByIdent = {};
        var slots = {};
        for (i = 0; i < program.variables.length; i++) {
            var vv = program.variables[i];
            slots[vv.scope] = (slots[vv.scope] === undefined) ? 0 : slots[vv.scope] + 1;
            vv.slot = slots[vv.scope];
            program.varByIdent[vv.ident] = vv;
        }

        /* No round start fill. SetVariableAtIndex initialises the array on first
         * write by its own documented behaviour, and unlike the Global packs
         * there is no single moment at which every object exists to fill. */
        report.counts.objectPacked = objs.length;
        if (other) {
            warn(report, other + ' reference(s) to a packed object variable were not a ' +
                'plain read or write and were left pointing at a variable that no longer exists.');
        }
    }

    function packGlobals(program, report, ceiling) {
        var i, j;

        var arrayish = markArrayishIdents(program);

        /* A LOOP COUNTER CANNOT LIVE INSIDE A PACK.
         *
         * Packing turns a variable into an index into a shared array, and every
         * read and write becomes ValueInArray / SetVariableAtIndex. A for loop
         * does not read or write its counter: the loop block OWNS it, and it
         * has to be a real variable.
         *
         * So packing rewrote the reads and writes it understood and left the
         * loop counters pointing at variables it had just deleted. It counted
         * them and warned - 278 references on Undead Ground Zero - and the
         * warning was true and nobody acted on it. Blockly then invented a
         * variable per dangling id and named it with a single letter, which is
         * where "Global Variable a" came from.
         *
         * They are excluded here for the same reason arrays are: not everything
         * can be an index. */
        var loopVars = {};
        (function markLoops(node) {
            if (!node || typeof node !== 'object') { return; }
            if (node.length !== undefined && node.k === undefined) {
                for (var a = 0; a < node.length; a++) { markLoops(node[a]); }
                return;
            }
            if (node.k === 'for' && node.ref && node.ref.ident) {
                loopVars[node.ref.ident] = true;
            }
            var ks = keysOf(node), n;
            for (n = 0; n < ks.length; n++) {
                var val = node[ks[n]];
                if (val && typeof val === 'object') { markLoops(val); }
            }
        }({ r: program.rules, s: program.subroutines, o: program.orphans || [] }));

        /* Only Global scope is packed. Player and Team variables are already
         * one slot serving every player, and turning them into array indices
         * would change who owns what. */
        /* HOW OFTEN EACH VARIABLE IS ACTUALLY READ.
         *
         * Packing has to lose some names, but it does not have to lose them at
         * random. A variable read six thousand times is one the reader will meet
         * constantly; one read twice is one they will probably never look at.
         * Counting first means the budget can be met by folding away the quiet
         * ones and leaving the loud ones legible.
         */
        var readCount = {};
        (function countReads(node) {
            if (!node || typeof node !== 'object') { return; }
            if (node.length !== undefined && node.k === undefined) {
                for (var a = 0; a < node.length; a++) { countReads(node[a]); }
                return;
            }
            if (node.k === 'getVar' && node.ref && node.ref.ident) {
                readCount[node.ref.ident] = (readCount[node.ref.ident] || 0) + 1;
            }
            var ks = keysOf(node), n;
            for (n = 0; n < ks.length; n++) {
                var v3 = node[ks[n]];
                if (v3 && typeof v3 === 'object') { countReads(v3); }
            }
        }({ r: program.rules, s: program.subroutines, o: program.orphans || [] }));

        var globals = [], arrays = [];
        for (i = 0; i < program.variables.length; i++) {
            var v = program.variables[i];
            if (v.scope !== 'Global') { continue; }
            if (has(arrayish, v.ident)) { arrays.push(v); continue; }
            if (has(loopVars, v.ident)) { arrays.push(v); continue; }
            globals.push(v);
        }
        /* THE CEILING COUNTS THE WHOLE SCOPE, NOT JUST WHAT WE CAN PACK.
         *
         * This asked whether the PACKABLE variables were over the limit. They
         * usually are not, because arrays keep their own slot and get filtered
         * out above. One project reached 250 Global variables, of which about
         * 122 held arrays: the remaining 128 looked fine, so nothing was packed
         * and the workspace stayed at 250 and would be refused outright by
         * Portal. The check has to be against the scope the limit applies to.
         *
         * Packing is worth doing whenever the SCOPE is over, even if the result
         * is still over, because getting closer is strictly better and the
         * budget warning downstream still reports the truth. In the case above
         * the 128 scalars fold into a single 256 wide pack and the scope lands
         * near 123, under the limit, with no other change.
         */
        var scopeTotal = 0;
        for (i = 0; i < program.variables.length; i++) {
            if (program.variables[i].scope === 'Global') { scopeTotal++; }
        }
        if (scopeTotal <= ceiling) { return []; }

        /* AN ARRAY CANNOT GO IN A PACK. DO NOT TRY AGAIN.
         *
         * This is tempting, because arrays are what is left over: one project
         * has 1,219 module values, 973 of which pack away cleanly, and the 246
         * that remain all hold arrays, so the scope sits at 250 against a limit
         * of 128 and Portal refuses the workspace.
         *
         * It does not work, and it fails silently, which is worse. From EA's own
         * documentation of the block, bf6-portal-mod-types/index.d.ts:2201:
         *
         *   Returns a copy of an array with the provided value appended to the
         *   end. Note: It is not possible for an array to contain arrays.
         *   Attempting to append an array to an array will concatenate them
         *   instead.
         *
         * The block EDITOR permits the connection - AppendToArray's value input
         * is declared anyType, which resolves to every type including Array, and
         * there is no connection check - so this looks legal right up until the
         * game flattens two arrays into one and the data is gone. modsim does
         * not model it either: its AppendToArray pushes, so a differential run
         * passes and tells you nothing.
         *
         * The real answer for the leftover arrays is object scoped variables:
         * a collection keyed by a player or object id belongs in the Player or
         * Team scope, which has its own budget. See LOWERING.md.
         */

        /* Folding one variable into a one element array is not a saving, and
         * folding none is a no-op that would still emit an empty pack. */
        if (globals.length < 2) { return []; }

        /* QUIETEST FIRST.
         *
         * Packing everything packable was why a converted project came back
         * reading ValueInArray(pack1, 7) everywhere: it folded away hundreds of
         * variables to solve a shortfall of a few dozen. Sorted by read count,
         * the ones that go are the ones nobody was going to look at, and the
         * heavily used names survive untouched.
         */
        globals.sort(function (x, y) {
            return (readCount[x.ident] || 0) - (readCount[y.ident] || 0);
        });

        /* ONLY AS MANY AS THE BUDGET DEMANDS.
         *
         * This used to fold every packable variable, so a project over the
         * limit by forty came back with every name in the file replaced by an
         * array index. Folding N of them removes N names and adds ceil(N/width)
         * packs, so the smallest N that fits is worth finding: on a project
         * needing to shed forty, forty go and the other six hundred keep their
         * names.
         *
         * globals is already sorted quietest first, so the ones that go are the
         * ones nobody reads.
         */
        var unpackable = scopeTotal - globals.length;
        var need = 0;
        for (need = 0; need <= globals.length; need++) {
            var after = unpackable + (globals.length - need) +
                        Math.ceil(need / PACK_WIDTH);
            if (after <= ceiling) { break; }
        }
        /* Nothing to gain: either it already fits, or folding cannot make it
         * fit and mangling every name for a workspace Portal will refuse anyway
         * helps nobody. */
        if (need <= 1) { return []; }
        if (need > globals.length) { need = globals.length; }
        globals = globals.slice(0, need);

        var packCount = Math.ceil(globals.length / PACK_WIDTH);
        var packs = [];
        for (i = 0; i < packCount; i++) {
            packs.push({ name: 'pack' + i, scope: 'Global', slot: 0,
                         ident: '__pack' + i, id: null, fill: 0 });
        }
        var where = {};
        for (i = 0; i < globals.length; i++) {
            var p = Math.floor(i / PACK_WIDTH);
            where[globals[i].ident] = { pack: packs[p], index: i % PACK_WIDTH };
            packs[p].fill = (i % PACK_WIDTH) + 1;
        }
        report.counts.packedOfPackable = need + " of " + (scopeTotal - unpackable) + " packable";

        /* Every read and every write of a packed variable is rewritten in
         * place. A node carrying a varRef is either a read (getVar) or a write
         * (setVar); anything else pointing at one would be a reference we did
         * not know about, so it is counted and reported. */
        var reads = 0, writes = 0, other = 0;
        function rewrite(node) {
            if (!node || typeof node !== 'object') { return; }
            var ks = keysOf(node), n, q;
            for (n = 0; n < ks.length; n++) {
                var v = node[ks[n]];
                if (!v || typeof v !== 'object') { continue; }
                if (v.length !== undefined && v.k === undefined) {
                    for (q = 0; q < v.length; q++) {
                        var rw = rewriteOne(v[q]);
                        /* Writing into a packed array takes three statements
                         * where there was one, so a statement list can grow
                         * under us. Splice, then step over what was inserted. */
                        if (rw && rw.length !== undefined && rw.k === undefined) {
                            var args = [q, 1].concat(rw), z;
                            Array.prototype.splice.apply(v, args);
                            for (z = 0; z < rw.length; z++) { rewrite(v[q + z]); }
                            q += rw.length - 1;
                        } else {
                            v[q] = rw;
                            rewrite(v[q]);
                        }
                    }
                } else {
                    node[ks[n]] = rewriteOne(v);
                    rewrite(node[ks[n]]);
                }
            }
        }
        function rewriteOne(node) {
            if (!node || typeof node !== 'object') { return node; }
            if (node.k === 'getVar' && node.ref && node.ref.ident && has(where, node.ref.ident)) {
                var w = where[node.ref.ident];
                reads++;
                return { k: 'call', fn: 'ValueInArray', args: [
                    { k: 'getVar', ref: packRef(w.pack) },
                    { k: 'num', v: w.index }
                ] };
            }
            if (node.k === 'setVar' && node.ref && node.ref.ident && has(where, node.ref.ident)) {
                var w2 = where[node.ref.ident];
                writes++;
                return { k: 'setVarAt', ref: packRef(w2.pack),
                         index: { k: 'num', v: w2.index }, value: node.value };
            }
            /* `arr[i] = v` where arr now lives in a pack slot. SetVariableAtIndex
             * needs a Variable, and a slot is not one, so the inner array is
             * lifted into a real variable, changed there, and put back. One
             * shared temporary serves every such write: it is live for exactly
             * these three statements and never across a call. */
            if (node.k === 'setVarAt' && node.ref && node.ref.ident && has(where, node.ref.ident)) {
                var w3 = where[node.ref.ident];
                writes++;
                return [
                    { k: 'setVar', ref: packTmpRef(),
                      value: { k: 'call', fn: 'ValueInArray', args: [
                          { k: 'getVar', ref: packRef(w3.pack) }, { k: 'num', v: w3.index }] } },
                    { k: 'setVarAt', ref: packTmpRef(), index: node.index, value: node.value },
                    { k: 'setVarAt', ref: packRef(w3.pack), index: { k: 'num', v: w3.index },
                      value: { k: 'getVar', ref: packTmpRef() } }
                ];
            }
            if (node.k === 'varRef' && node.ident && has(where, node.ident)) { other++; }
            return node;
        }
        function packTmpRef() {
            return { k: 'varRef', name: 'packSlot', scope: 'Global',
                     ident: '__packtmp', slot: 0, object: null };
        }
        /* Declared only when a packed array was written to, so the ordinary
         * case does not gain a variable it never reads. */
        function usesPackTmp(prog) {
            var found = false;
            (function look(n) {
                if (found || !n || typeof n !== 'object') { return; }
                if (n.ident === '__packtmp') { found = true; return; }
                var kk = keysOf(n), a;
                for (a = 0; a < kk.length; a++) {
                    var vv = n[kk[a]];
                    if (vv && typeof vv === 'object') { look(vv); }
                }
            })({ r: prog.rules, s: prog.subroutines, o: prog.orphans || [] });
            return found;
        }

        function packRef(p) {
            return { k: 'varRef', name: p.name, scope: p.scope, ident: p.ident,
                     slot: p.slot, object: null };
        }
        for (i = 0; i < program.rules.length; i++) { rewrite(program.rules[i]); }
        for (i = 0; i < program.subroutines.length; i++) { rewrite(program.subroutines[i]); }
        for (i = 0; i < (program.orphans || []).length; i++) { rewrite(program.orphans[i]); }

        /* Drop the packed variables, keep the rest, add the packs, renumber. */
        var kept = [];
        for (i = 0; i < program.variables.length; i++) {
            if (!has(where, program.variables[i].ident)) { kept.push(program.variables[i]); }
        }
        var extra = packs.slice();
        if (usesPackTmp(program)) {
            extra.push({ name: 'packSlot', scope: 'Global', slot: 0,
                         ident: '__packtmp', id: null, fill: 0 });
        }
        program.variables = kept.concat(extra);
        program.varByIdent = {};
        var slots = {};
        for (i = 0; i < program.variables.length; i++) {
            var vv = program.variables[i];
            slots[vv.scope] = (slots[vv.scope] === undefined) ? 0 : slots[vv.scope] + 1;
            vv.slot = slots[vv.scope];
            program.varByIdent[vv.ident] = vv;
        }

        /* Build each pack to full length at round start, or the first write to
         * a high index has nowhere to land. */
        /* ONE STATEMENT PER ELEMENT, NOT ONE EXPRESSION PER PACK.
         *
         * Building a pack as AppendToArray(AppendToArray(...)) nests once per
         * element, so a 128 wide pack is an expression 128 deep - and the
         * editor caps nesting at 64. The same array built by assigning to
         * itself repeatedly is a flat list of statements, each two deep, and
         * has no depth limit at all.
         */
        var init = [];
        for (i = 0; i < packs.length; i++) {
            init.push({ k: 'setVar', ref: packRef(packs[i]),
                        value: { k: 'call', fn: 'EmptyArray', args: [] } });
            for (j = 0; j < packs[i].fill; j++) {
                init.push({
                    k: 'setVar', ref: packRef(packs[i]),
                    value: { k: 'call', fn: 'AppendToArray',
                             args: [{ k: 'getVar', ref: packRef(packs[i]) }, { k: 'num', v: 0 }] }
                });
            }
        }
        if (init.length) { attachRoundStart(program, init, report); }

        report.counts.packed = globals.length;
        report.counts.packs = packs.length;
        if (other) {
            warn(report, other + ' reference(s) to a packed variable were not a plain read ' +
                'or write and were left pointing at a variable that no longer exists.');
        }
    }

    /* Statements that must run before anything else. Prepended to an existing
     * round start rule when there is one, so ordering against the mod own set
     * up is preserved, and a new rule only when there is not. */
    function attachRoundStart(program, stmts, report) {
        var i;
        for (i = 0; i < program.rules.length; i++) {
            if (program.rules[i].event === 'OnGameModeStarted') {
                program.rules[i].actions = stmts.concat(program.rules[i].actions || []);
                return;
            }
        }
        program.rules.push({
            kind: 'rule', id: null, index: program.rules.length,
            name: 'Set up packed variables', event: 'OnGameModeStarted',
            eventKey: 'OnGameModeStarted', fnName: '__packInit',
            isOngoing: false, conditions: [], actions: stmts, comment: null
        });
    }

    function buildRules(ts, program, fnBodies, dispatchers, metaByFn, report) {
        var i, j;
        var claimed = {};
        var nameSeen = {};

        for (i = 0; i < dispatchers.length; i++) {
            var d = dispatchers[i];
            var event = d.event;
            var calls = [];
            if (d.subscribeHandler) {
                calls.push({ fn: d.subscribeHandler });
            } else if (isPureGlue(ts, d.node)) {
                calls = dispatcherCalls(ts, d.node);
            } else {
                /* The handler does the work itself, so IT is the rule. */
                calls = [{ fn: d.event, qualifier: null }];
            }
            for (j = 0; j < calls.length; j++) {
                var glueName = calls[j].fn;
                var r = buildRuleFrom(ts, program, fnBodies, glueName, event, metaByFn, claimed, nameSeen, report, calls[j].qualifier);
                if (r) { program.rules.push(r); }
            }
        }

        /* Hand written source with no dispatcher: any *_Action function whose
         * prefix names a known event still forms a rule. */
        var fnames = keysOf(fnBodies);
        for (i = 0; i < fnames.length; i++) {
            var n = fnames[i];
            if (claimed[n]) { continue; }
            var m = /^(Ongoing[A-Za-z0-9]+|On[A-Za-z0-9]+)_(.+)_Action$/.exec(n);
            if (!m) { continue; }
            if (!has(EVENTS, m[1])) { continue; }
            var glue = m[1] + '_' + m[2];
            var rr = buildRuleFrom(ts, program, fnBodies, glue, m[1], metaByFn, claimed, nameSeen, report, null);
            if (rr) { program.rules.push(rr); }
        }
    }

    /* IS THIS HANDLER ONLY A LIST OF CALLS?
     *
     * The rule builder assumed an event handler is glue: a body of nothing but
     * calls to other functions, one rule each. Plenty of handlers are written
     * that way. TDM is not:
     *
     *     export function OnPlayerJoinGame(eventPlayer) {
     *         const playerId = mod.GetObjId(eventPlayer);
     *         mod.SetRedeployTime(eventPlayer, 3);
     *         mod.SkipManDown(eventPlayer, true);
     *         playersStats[playerId] = { k: 0, d: 0, a: 0, hs: 0 };
     *         updateScoreboard(eventPlayer, playersStats[playerId]);
     *     }
     *
     * Every statement but the last was DROPPED, because only the call was
     * looked at. The converted mod reported a rule for that event and then did
     * nothing when it fired - which is exactly what the differential harness
     * showed: 8 mod calls on the original, 0 on the round trip.
     *
     * So a handler counts as glue only when its whole body is calls to named
     * functions. Anything else and the handler itself becomes the rule, and
     * the functions it calls become subroutines - which is the shape a block
     * workspace has anyway.
     */
    /* A value that is written out in full rather than computed. Object literals
     * count when every member is itself inert, so `{ eventPlayer: eventPlayer }`
     * passes and `{ id: mod.GetObjId(p) }` does not. */
    function isInertInitializer(ts, e) {
        if (!e) { return false; }
        if (ts.isNumericLiteral(e) || ts.isStringLiteral(e) ||
            ts.isNoSubstitutionTemplateLiteral(e) ||
            e.kind === ts.SyntaxKind.TrueKeyword ||
            e.kind === ts.SyntaxKind.FalseKeyword ||
            e.kind === ts.SyntaxKind.NullKeyword ||
            ts.isIdentifier(e)) {
            return true;
        }
        if (ts.isObjectLiteralExpression(e)) {
            for (var i = 0; i < e.properties.length; i++) {
                var p = e.properties[i];
                if (ts.isShorthandPropertyAssignment(p)) { continue; }
                if (!ts.isPropertyAssignment(p) || !isInertInitializer(ts, p.initializer)) {
                    return false;
                }
            }
            return true;
        }
        if (ts.isArrayLiteralExpression(e)) {
            for (var j = 0; j < e.elements.length; j++) {
                if (!isInertInitializer(ts, e.elements[j])) { return false; }
            }
            return true;
        }
        return false;
    }

    function isPureGlue(ts, fn) {
        if (!fn || !fn.body || !fn.body.statements) { return false; }
        var st = fn.body.statements, i, seen = 0;
        for (i = 0; i < st.length; i++) {
            var s = st[i];
            if (ts.isEmptyStatement(s)) { continue; }
            /* Declarations are allowed only when they are INERT: initialised by
             * a literal and nothing else. Both dispatcher writers open this way
             *
             *     const eventInfo = { eventPlayer: eventPlayer };   // ours
             *     let eventNum = 8;                                 // the site's
             *
             * and rejecting them made the tool fail to read its own output, and
             * the site's, as glue.
             *
             * A declaration initialised by a CALL is real work:
             * `const playerId = mod.GetObjId(eventPlayer)` is the first line of
             * a handler that does its own job, and must not be mistaken for
             * preamble. Anything else - an index, an operator, a name - is left
             * out too, because a body holding one is not a plain call list and
             * dropping it would silently lose whatever it computed. */
            if (ts.isVariableStatement(s)) {
                var ds = (s.declarationList && s.declarationList.declarations) || [];
                var inert = ds.length > 0;
                for (var q = 0; q < ds.length; q++) {
                    if (!isInertInitializer(ts, ds[q].initializer)) { inert = false; }
                }
                if (inert) { continue; }
                return false;
            }
            if (!ts.isExpressionStatement(s)) { return false; }
            var e = s.expression;
            if (ts.isAwaitExpression(e) || ts.isVoidExpression(e)) { e = e.expression; }
            if (!ts.isCallExpression(e)) { return false; }
            var callee = e.expression;
            var named = ts.isIdentifier(callee) ||
                (ts.isPropertyAccessExpression(callee) && ts.isIdentifier(callee.expression) &&
                 callee.expression.text !== 'mod');
            if (!named) { return false; }

            /* GLUE FORWARDS. IT DOES NOT CARRY.
             *
             * A glue call is replaced by the body of the function it names, and
             * its arguments are thrown away, because forwarding an event's own
             * values loses nothing: the body reads the same event. An argument
             * that is anything else DOES carry something.
             *
             *     export function OnGameModeStarted() { visit(2); }
             *
             * was read as glue, so the rule became the body of visit with the 2
             * discarded and its parameter bound to nothing. Every read of n
             * became a gap and the rule ran on null. A call with a real
             * argument is work, so the handler itself becomes the rule and the
             * callee becomes an ordinary subroutine that receives it.
             *
             * A NAME is fine, and so is a call: a parameter, an event value, a
             * module level binding or our own rt.getEventCondition() plumbing
             * is all reachable from inside the callee, so forwarding it and
             * then not passing it loses nothing. Our generated dispatchers are
             * written exactly that way, and rejecting them made every event
             * produce two rules.
             *
             * A LITERAL or a COMPUTED value is the opposite: `2` and `n - 1`
             * exist only at the call site, and dropping them is the bug.
             */
            var args = e.arguments || [], pi;
            for (pi = 0; pi < args.length; pi++) {
                var a = args[pi];
                var forwardable = ts.isIdentifier(a) || ts.isCallExpression(a) ||
                    ts.isPropertyAccessExpression(a) ||
                    a.kind === ts.SyntaxKind.ThisKeyword;
                if (!forwardable) { return false; }
            }
            seen++;
        }
        return seen > 0;
    }

    function dispatcherCalls(ts, fn) {
        var out = [];
        if (!fn || !fn.body || !fn.body.statements) { return out; }
        var i;
        for (i = 0; i < fn.body.statements.length; i++) {
            var s = fn.body.statements[i];
            if (!ts.isExpressionStatement(s)) { continue; }
            var e = s.expression;
            if (ts.isVoidExpression(e)) { e = e.expression; }
            if (!ts.isCallExpression(e)) { continue; }
            var callee = e.expression;
            if (ts.isIdentifier(callee)) { out.push({ fn: callee.text, qualifier: null }); }
            else if (ts.isPropertyAccessExpression(callee) && ts.isIdentifier(callee.expression)) {
                out.push({ fn: callee.name.text, qualifier: callee.expression.text });
            }
        }
        return out;
    }

    function buildRuleFrom(ts, program, fnBodies, glueName, event, metaByFn, claimed, nameSeen, report, qualifier) {
        var glue = fnBodies[glueName];
        var condFn = fnBodies[glueName + '_Condition'];
        var actFn = fnBodies[glueName + '_Action'];
        if (!glue && !actFn) { return null; }
        claimed[glueName] = 1;
        if (condFn) { claimed[glueName + '_Condition'] = 1; }
        if (actFn) { claimed[glueName + '_Action'] = 1; }

        var meta = metaByFn[glueName] || metaByFn[glueName + '_Action'] || null;
        var eventType, objectType;
        if (meta) {
            eventType = meta.event; objectType = meta.objectType;
        } else if (/^Ongoing/.test(event)) {
            eventType = 'Ongoing'; objectType = event.slice(7);
        } else {
            eventType = event; objectType = 'Global';
        }

        var displayName;
        if (meta && meta.name !== undefined) {
            displayName = meta.name;
        } else {
            displayName = deriveRuleName(glueName, event, nameSeen);
        }

        var srcFn = glue || actFn || condFn;
        var ruleLine = srcFn ? lineOf(ts, srcFn.sf, srcFn.node) : 0;
        var ruleFile = originalFileOf(program, srcFn ? srcFn.file : '', ruleLine);
        var ctx = newTsCtx(program, report, ruleFile, glue ? glue.sf : actFn.sf, []);

        /* BIND THE HANDLER'S PARAMETERS TO THE EVENT'S VALUES, BY POSITION.
         *
         * events.json carries each event's values in order with their canonical
         * names, which is the only thing that actually knows what the second
         * argument of OnRayCastHit is. Mapping the handler's Nth parameter onto
         * the event's Nth value means an author may call them anything.
         *
         * Extra parameters are reported rather than ignored: a handler that
         * declares four values for a three value event is reading something
         * that will never arrive, and silently compiling it to a gap is how
         * this class of bug stayed invisible. */
        (function bindEventParams() {
            var src = srcFn && srcFn.node;
            var declared = (src && src.parameters) || [];
            if (!declared.length) { return; }
            var table = (EVENTS && EVENTS[eventType]) || null;
            /* NO TABLE MEANS NO OPINION.
             *
             * Ongoing rules carry eventType "Ongoing", which is not an entry in
             * events.json and never will be, and a handler read back out of our
             * own generated TypeScript is declared (conditionState, eventInfo)
             * by the runtime rather than by the author. Reporting either as a
             * parameter with nothing to bind to turned a clean round trip into
             * five false losses. Bind what the table knows; say nothing about
             * what it does not. */
            if (!table) { return; }
            var supplied = table.params || [];
            var pi;
            for (pi = 0; pi < declared.length; pi++) {
                var pnode = declared[pi];
                if (!pnode.name || !ts.isIdentifier(pnode.name)) { continue; }
                var pname = pnode.name.text;
                /* Our own runtime's signature, not the author's. */
                if (pname === 'conditionState' || pname === 'eventInfo') { continue; }
                if (pi < supplied.length) {
                    ctx.eventAlias[pname] = supplied[pi].name;
                } else {
                    reportUnconvertible(report, ruleFile, ruleLine,
                        'parameter ' + pname + ' of ' + eventType +
                        ' (position ' + (pi + 1) + ')',
                        eventType + ' supplies ' + supplied.length + ' value(s). This ' +
                        'parameter has nothing to bind to and every read of it is a gap.');
                }
            }
        }());
        /* Which locals in this handler hold an object id, so a collection keyed
         * on one can find the object it belongs to. */
        withObjIds(ts, ctx, (glue || actFn || condFn).node);
        /* Names the slots this rule's locals land in, so two rules that both
         * use `i` get two slots rather than quietly sharing one. */
        ctx.fnName = sanitizeIdent(glueName || displayName || 'rule');
        var conditions = [];
        if (condFn) { conditions = conditionsFromReturn(ts, condFn.node, ctx); }
        else if (glue) { conditions = conditionsFromGuard(ts, glue.node, ctx); }

        var actions = [];
        if (actFn) { actions = applyReturnGuards(stmtsFromBlock(ts, actFn.node.body, ctx), ctx); }
        else if (glue) { actions = applyReturnGuards(stmtsFromBlock(ts, glue.node.body, ctx, true), ctx); }

        return {
            kind: 'rule',
            name: displayName,
            event: eventType,
            objectType: objectType,
            eventKey: eventKeyOf(eventType, objectType),
            fnName: glueName,
            isOngoing: eventType === 'Ongoing',
            index: meta && meta.index !== undefined ? meta.index : program.rules.length,
            conditions: conditions,
            actions: actions,
            file: ruleFile || '',
            line: ruleLine,
            comment: null
        };
    }

    function deriveRuleName(glueName, event, nameSeen) {
        var raw = glueName;
        if (raw.indexOf(event + '_') === 0) { raw = raw.slice(event.length + 1); }
        /* strip a trailing de-duplication digit added when two rules share a name */
        var m = /^(.*?)(\d+)$/.exec(raw);
        if (m && nameSeen[m[1]]) { raw = m[1]; }
        nameSeen[raw] = (nameSeen[raw] || 0) + 1;
        return raw.replace(/_/g, ' ');
    }

    function buildSubroutines(ts, program, fnBodies, subMeta, report) {
        var names = keysOf(fnBodies), i, j;
        var ruleFns = {};
        for (i = 0; i < program.rules.length; i++) {
            ruleFns[program.rules[i].fnName] = 1;
            ruleFns[program.rules[i].fnName + '_Condition'] = 1;
            ruleFns[program.rules[i].fnName + '_Action'] = 1;
        }
        for (i = 0; i < names.length; i++) {
            var n = names[i];
            if (ruleFns[n]) { continue; }
            if (has(EVENTS, n)) { continue; }
            if (/_(Condition|Action)$/.test(n)) { continue; }
            var f = fnBodies[n];
            var meta = subMeta[n] || null;
            var params = [];
            var ps = f.node.parameters || [];
            var pcount = ps.length;
            if (pcount && ts.isIdentifier(ps[pcount - 1].name) &&
                ps[pcount - 1].name.text === 'eventInfo') {
                pcount--;   /* implicit event context, not a block parameter */
            }
            for (j = 0; j < pcount; j++) {
                var pname = ts.isIdentifier(ps[j].name) ? ps[j].name.text : ('arg' + j);
                params.push({
                    name: pname,
                    type: meta && meta.params && meta.params[j] ? meta.params[j].type
                        : portalTypeOfTsType(ts, ps[j].type)
                });
            }
            var subLine = lineOf(ts, f.sf, f.node);
            var subFile = originalFileOf(program, f.file, subLine);
            var ctx = newTsCtx(program, report, subFile, f.sf, params);
            withObjIds(ts, ctx, f.node);
            ctx.fnName = sanitizeIdent(n);
            program.subroutines.push({
                kind: 'subroutine',
                name: meta && meta.name !== undefined ? meta.name : n,
                fnName: n,
                params: params,
                x: meta && meta.x !== undefined ? meta.x : 0,
                y: meta && meta.y !== undefined ? meta.y : 0,
                conditions: conditionsFromGuard(ts, f.node, ctx),
                actions: applyReturnGuards(stmtsFromBlock(ts, f.node.body, ctx, true), ctx),
                /* WHERE THIS CAME FROM.
                 *
                 * The file was already in hand here and was being dropped. It is
                 * what lets the workspace be laid out one column per source file
                 * instead of one undifferentiated wall, and it is the only way a
                 * reader who cannot read TypeScript can ask where a block came
                 * from. */
                file: subFile || '',
                line: subLine,
                comment: null
            });
        }
    }

    function portalTypeOfTsType(ts, tn) {
        if (!tn) { return 'Any'; }
        /* A component parameter carries the widget, so the Portal type is
         * UIWidget. Taking the last segment of `UI.Container` would name a
         * mod.Container that does not exist. */
        var uiName = uiTypeName(ts, tn);
        if (uiName && ctx_uiSpecNames && has(ctx_uiSpecNames, uiName)) { return 'UIWidget'; }
        if (tn.kind === ts.SyntaxKind.BooleanKeyword) { return 'Boolean'; }
        if (tn.kind === ts.SyntaxKind.NumberKeyword) { return 'Number'; }
        if (tn.kind === ts.SyntaxKind.StringKeyword) { return 'String'; }
        if (ts.isTypeReferenceNode(tn)) {
            var t = tn.typeName;
            if (ts.isQualifiedName(t)) { return t.right.text; }
            if (ts.isIdentifier(t)) { return t.text; }
        }
        return 'Any';
    }

    /* LOWERING, NOT TRANSLITERATION.
     *
     * The reader below was written to recognise TypeScript that had been
     * GENERATED from blocks, so it only accepted what a block can already say:
     * a loop counter had to be a workspace variable the author declared by
     * hand, an identifier had to be named xGlobalVar. Fed a real program it
     * gave up 4,583 times on one project, 834 of them a plain `let`.
     *
     * A local is not unrepresentable - it is a workspace variable nobody
     * bothered to mint. ctx.locals is the scope map that mints them: TS name
     * -> the varRef it was lowered to, so a later read of that name resolves
     * instead of failing. It is per function, because two functions may both
     * use `i` and they must not become the same slot.
     */
    function newTsCtx(program, report, file, sf, params) {
        return {
            program: program, report: report, file: file, sf: sf,
            params: params || [],
            locals: {},
            /* WHAT THIS HANDLER CALLED THE EVENT'S VALUES.
             *
             * An event supplies its values positionally; the author names them
             * whatever reads best. UGZ writes (player, point, normal) where the
             * canonical names are eventPlayer, eventVector, eventOtherVector.
             * Event values used to be recognised by NAME alone, so every one of
             * those became "identifier player: not a workspace variable" and
             * compiled to a gap. Filled by buildRuleFrom from events.json. */
            eventAlias: {},
            fnName: '',
            temps: 0,
            /* ident -> the expression the object id came from. Filled by
             * withObjIds once the function body is known. */
            objOfId: {}
        };
    }

    function withObjIds(ts, ctx, fnNode) {
        ctx.objOfId = scanObjIds(ts, fnNode, ctx);
        return ctx;
    }

    /* A workspace variable standing in for a TypeScript local. The name is
     * mangled with the function it came from so two functions' `i` are two
     * slots, and kept readable so somebody opening the blocks can see where it
     * came from. varRefFromIdent registers it in program.variables for us. */
    function localRef(ctx, name) {
        if (ctx.locals && has(ctx.locals, name)) { return ctx.locals[name]; }
        var ident = 'local_' + (ctx.fnName || 'rule') + '_' + name;
        var ref = varRefFromIdent(ctx, ident, null);
        notePoolable(ctx, ident);
        if (!ctx.locals) { ctx.locals = {}; }
        ctx.locals[name] = ref;
        return ref;
    }

    /* WHICH SLOTS ARE FRAME SLOTS.
     *
     * A module level binding lives for the whole round and keeps its own slot.
     * A function local exists only while that function is running, so it can
     * share a slot with a local of any function that cannot be on the stack at
     * the same time. Only these are poolable, and only the allocator decides
     * which of them actually share.
     */
    function notePoolable(ctx, ident) {
        var p = ctx.program;
        if (!p.frameVars) { p.frameVars = []; p.frameByIdent = {}; }
        if (has(p.frameByIdent, ident)) { return; }
        var rec = { ident: ident, fn: ctx.fnName || 'rule' };
        p.frameVars.push(rec);
        p.frameByIdent[ident] = rec;
    }

    /* A slot nothing in the source is named after: loop counters and the like. */
    function tempRef(ctx, what) {
        ctx.temps = (ctx.temps || 0) + 1;
        var tident = 'tmp_' + (ctx.fnName || 'rule') + '_' + what + '_' + ctx.temps;
        var tref = varRefFromIdent(ctx, tident, null);
        notePoolable(ctx, tident);
        return tref;
    }

    function conditionsFromReturn(ts, fn, ctx) {
        var ts_ = ts;
        var out = [];
        if (!fn.body || !fn.body.statements) { return out; }
        var i;
        for (i = 0; i < fn.body.statements.length; i++) {
            var s = fn.body.statements[i];
            var e = null;
            if (ts_.isReturnStatement(s) && s.expression) { e = s.expression; }
            else if (ts_.isVariableStatement(s) && s.declarationList.declarations.length &&
                s.declarationList.declarations[0].initializer) {
                e = s.declarationList.declarations[0].initializer;
            }
            if (!e) { continue; }
            if (ts_.isIdentifier(e)) { continue; }
            if (e.kind === ts_.SyntaxKind.TrueKeyword) { return []; }
            return splitAnd(ts_, e, ctx);
        }
        return out;
    }

    function newStateGuard(ts, stmts) {
        /* const newState = <cond>; return newState;  which the site's exporter emits
         * at the top of a subroutine that has conditions. */
        if (!stmts || stmts.length < 2) { return null; }
        var d = stmts[0], r = stmts[1];
        if (!ts.isVariableStatement(d) || !d.declarationList.declarations.length) { return null; }
        var decl = d.declarationList.declarations[0];
        if (!ts.isIdentifier(decl.name) || decl.name.text !== 'newState' || !decl.initializer) { return null; }
        if (!ts.isReturnStatement(r) || !r.expression || !ts.isIdentifier(r.expression) ||
            r.expression.text !== 'newState') { return null; }
        return decl.initializer;
    }

    function conditionsFromGuard(ts, fn, ctx) {
        /* if (!(<cond>)) { return; } as the first statement */
        if (!fn.body || !fn.body.statements || !fn.body.statements.length) { return []; }
        var ns = newStateGuard(ts, fn.body.statements);
        if (ns) { return splitAnd(ts, ns, ctx); }
        var s = fn.body.statements[0];
        if (!ts.isIfStatement(s)) { return []; }
        if (!isBareReturn(ts, s.thenStatement)) { return []; }
        var e = s.expression;
        if (ts.isPrefixUnaryExpression(e) && e.operator === ts.SyntaxKind.ExclamationToken) {
            var inner = e.operand;
            while (ts.isParenthesizedExpression(inner)) { inner = inner.expression; }
            if (ts.isCallExpression(inner) && ts.isPropertyAccessExpression(inner.expression) &&
                inner.expression.name.text === 'update') { return []; }
            return splitAnd(ts, inner, ctx);
        }
        return [];
    }

    function isBareReturn(ts, node) {
        if (!node) { return false; }
        if (ts.isReturnStatement(node)) { return !node.expression; }
        if (ts.isBlock(node)) {
            return node.statements.length === 1 && ts.isReturnStatement(node.statements[0]) &&
                !node.statements[0].expression;
        }
        return false;
    }

    function splitAnd(ts, e, ctx) {
        while (ts.isParenthesizedExpression(e)) { e = e.expression; }
        if (ts.isCallExpression(e) && isModCall(ts, e, 'And') && e.arguments.length >= 2) {
            var out = [], i;
            for (i = 0; i < e.arguments.length; i++) {
                out = out.concat(splitAnd(ts, e.arguments[i], ctx));
            }
            return out;
        }
        var x = exprFromTs(ts, e, ctx);
        return x ? [x] : [];
    }

    function isModCall(ts, e, name) {
        var c = e.expression;
        return ts.isPropertyAccessExpression(c) && ts.isIdentifier(c.expression) &&
            c.expression.text === 'mod' && c.name.text === name;
    }

    /* A SUBROUTINE CALL IS A STATEMENT, EVEN WHEN THE SOURCE USES ITS RESULT.
     *
     * `const brain = getBrain(player)` reads as a value, and there is no block
     * that can be one: subroutineInstanceBlock has no output and cannot sit in a
     * value socket. Emitting it into one produced a workspace the editor
     * refused to load at all, with
     *
     *   The block "subroutineInstanceBlock" (id=...) is missing a(n) output
     *
     * and, because the page is served from file://, that arrived in the log as
     * the words "Script error." and nothing else. One bad block anywhere threw
     * the whole load away, which is why a converted project opened blank.
     *
     * Portal authors write this as two steps, and so does the converter now:
     * call the subroutine, then read the variable it left its answer in. That
     * variable already exists - applyReturnGuards writes every `return` into
     * ret_<fn> - so this only has to put the call in front and read the slot.
     *
     * Innermost first, so `f(g(x))` calls g, then f, in the order the source
     * would have.
     */
    function spillSubCalls(stmt, ctx, out) {
        function walk(node) {
            if (!node || typeof node !== 'object') { return node; }
            if (node.length !== undefined && node.k === undefined) {
                for (var a = 0; a < node.length; a++) { node[a] = walk(node[a]); }
                return node;
            }
            var ks = keysOf(node), n;
            for (n = 0; n < ks.length; n++) {
                var v = node[ks[n]];
                if (v && typeof v === 'object') { node[ks[n]] = walk(v); }
            }
            if (node.k === 'subCall') {
                out.push({ k: 'sub', name: node.name, args: node.args || [] });
                return { k: 'getVar', ref: retRefFor(ctx, node.name) };
            }
            return node;
        }
        /* The statement itself is never a subCall - only what is inside it. */
        walk(stmt);
        return stmt;
    }

    /* The slot a subroutine leaves its answer in. */
    function retRefFor(ctx, name) {
        return varRefFromIdent(ctx, 'ret_' + sanitizeIdent(name), null);
    }

    function stmtsFromBlock(ts, body, ctx, skipGuard) {
        var out = [];
        if (!body) { return out; }
        if (!body.statements) { return out; }
        var i = 0;
        if (skipGuard && body.statements.length && ts.isIfStatement(body.statements[0]) &&
            isBareReturn(ts, body.statements[0].thenStatement)) {
            i = 1;
        } else if (skipGuard && newStateGuard(ts, body.statements)) {
            i = 2;
        }
        for (; i < body.statements.length; i++) {
            var s = stmtFromTs(ts, body.statements[i], ctx);
            if (!s) { continue; }
            var list = (s.length !== undefined && s.k === undefined) ? s : [s];
            for (var q = 0; q < list.length; q++) {
                /* Any subroutine call used as a value becomes a call in front
                 * and a read of its return slot in place. */
                var pre = [];
                spillSubCalls(list[q], ctx, pre);
                if (pre.length) { out = out.concat(pre); }
                out.push(list[q]);
            }
        }
        return out;
    }

    /* SOME EXPRESSIONS NEED A STATEMENT OF THEIR OWN FIRST.
     *
     * Building a UI widget is one source expression that becomes two block
     * statements: mod.AddUI*(...) and then the lookup that gives the widget
     * back. Expression lowering has nowhere to put the first of those, so it
     * pushes it here and this splices it in front of whatever needed it.
     *
     * Every nested statement lowering gets its own buffer, so a construction
     * inside an if body stays inside that body.
     */
    function stmtFromTs(ts, s, ctx) {
        var outer = ctx.pre;
        ctx.pre = [];
        var made = stmtFromTsInner(ts, s, ctx);
        var pre = ctx.pre;
        ctx.pre = outer;
        if (!pre.length) { return made; }
        if (!made) { return pre.length === 1 ? pre[0] : pre; }
        var isList = made.length !== undefined && made.k === undefined;
        return pre.concat(isList ? made : [made]);
    }

    function stmtFromTsInner(ts, s, ctx) {
        var line = lineOf(ts, ctx.sf, s);

        if (ts.isExpressionStatement(s)) {
            var e = s.expression;
            if (ts.isAwaitExpression(e)) { e = e.expression; }
            if (ts.isVoidExpression(e)) { e = e.expression; }

            /* i++ and i-- are += 1 and -= 1 written shorter, and they are
             * allowed on anything assignable - `playersStats[id].k++` included.
             *
             * This has to sit INSIDE this branch. It was written just below the
             * one that handles break, where it could never run, because the
             * branch we are in returns for every expression statement there is.
             * That is the second time a statement rule has been added to that
             * dead spot: if it applies to an expression statement, it belongs
             * here. */
            if ((ts.isPostfixUnaryExpression(e) || ts.isPrefixUnaryExpression(e)) &&
                (e.operator === ts.SyntaxKind.PlusPlusToken ||
                 e.operator === ts.SyntaxKind.MinusMinusToken)) {
                var step = e.operator === ts.SyntaxKind.PlusPlusToken ? 'Add' : 'Subtract';
                var cur = exprFromTs(ts, e.operand, ctx);
                if (cur) {
                    var inc = writeThroughPath(ts, e.operand,
                        { k: 'call', fn: step, args: [cur, { k: 'num', v: 1 }] }, ctx);
                    if (inc) { return inc.length === 1 ? inc[0] : inc; }
                }
            }
            /* ASSIGNING TO SOMETHING, WHICH IS MOST OF WHAT A PROGRAM DOES.
             *
             * `weight -= penalty`, `total += n`, `x = y` were being dropped
             * here - 45 of them in one 3 file project, which is most of why
             * the converted mod ran its handlers and did almost nothing.
             *
             * A compound assignment is the plain one with the operator
             * applied:  x -= v  ->  SetVariable(x, Subtract(GetVariable(x), v))
             * and Portal has Add, Subtract, Multiply and Divide as value
             * blocks. This has to sit INSIDE this branch: the test above
             * returns for every expression statement there is, so a check
             * placed after it can never run.
             */
            if (ts.isBinaryExpression(e) && assignOp(ts, e.operatorToken.kind) !== null) {
                var asg = assignToIr(ts, e, ctx, line);
                if (asg) { return asg; }
            }
            /* A WIDGET BUILT AND NOT KEPT.
             *
             * `new UI.Text({...});` on its own line is most of a HUD, and the
             * constructor still looks the widget up after adding it. A value
             * block cannot stand alone as a statement, so the lookup is parked
             * in one scratch slot that every discarded widget in this function
             * shares - one slot, not one per widget.
             *
             * This has to sit INSIDE this branch: the tests below return for
             * every expression statement there is. */
            if (ts.isNewExpression(e) && uiSpecFor(ts, e, ctx)) {
                var built = exprFromTs(ts, e, ctx);
                if (!built || built.k !== 'call') { return null; }
                if (!ctx.uiScratch) { ctx.uiScratch = tempRef(ctx, 'uiwidget'); }
                return { k: 'setVar', ref: ctx.uiScratch, value: built };
            }

            if (!ts.isCallExpression(e)) {
                return unconvertibleStmt(ts, s, ctx, line, 'expression statement');
            }

            /* `text.setMessage(m)`, `container.delete()`. A component method
             * that only calls mod.* on its own widget is that call. */
            if (ts.isPropertyAccessExpression(e.expression)) {
                var um = uiMethodToIr(ts, e, ctx, line);
                if (um) { return um.length === 0 ? null : um; }
            }

            var callee = e.expression;
            if (ts.isPropertyAccessExpression(callee) && ts.isIdentifier(callee.expression)) {
                var ns = aliasedNamespace(ctx, callee.expression.text), fn = callee.name.text;
                if (ns === 'mod') {
                    if (fn === 'Wait') { return { k: 'wait', seconds: exprFromTs(ts, e.arguments[0], ctx) }; }
                    if (fn === 'SetVariable') {
                        return {
                            k: 'setVar',
                            ref: exprFromTs(ts, e.arguments[0], ctx),
                            value: exprFromTs(ts, e.arguments[1], ctx)
                        };
                    }
                    if (fn === 'SetVariableAtIndex') {
                        return {
                            k: 'setVarAt',
                            ref: exprFromTs(ts, e.arguments[0], ctx),
                            index: exprFromTs(ts, e.arguments[1], ctx),
                            value: exprFromTs(ts, e.arguments[2], ctx)
                        };
                    }
                    return { k: 'call', fn: fn, args: argsFromTs(ts, fn, e.arguments, ctx) };
                }
                if (ns === 'rt' || ns === 'modlib') {
                    return { k: 'call', fn: fn, args: argsFromTs(ts, fn, e.arguments, ctx) };
                }
                if (ns === 'subs') {
                    return { k: 'sub', name: fn, args: subCallArgs(ts, e.arguments, ctx) };
                }
                var colS = collectionStmt(ts, e, ctx, line);
                if (colS) { return colS; }
                return unconvertibleStmt(ts, s, ctx, line, 'call through ' + ns);
            }
            if (ts.isIdentifier(callee)) {
                return { k: 'sub', name: callee.text, args: subCallArgs(ts, e.arguments, ctx) };
            }
            return unconvertibleStmt(ts, s, ctx, line, 'call expression');
        }

        /* TRY / CATCH, WHERE THERE IS NOTHING TO CATCH.
         *
         * Blocks have no exceptions, so the whole statement was being dropped -
         * body included. In TDM that threw away 25 sites, and because the kill
         * handler wraps its spawn bookkeeping in one, the guarded work simply
         * never ran.
         *
         * Dropping the body is the one clearly wrong answer. What the common
         * shape means is plain enough:
         *
         *     try { recordDeath(...); } catch (error) {}
         *
         * an empty catch says run this and carry on if it fails. Blocks carry on
         * regardless, so the body is emitted on its own and that is faithful.
         *
         * A catch that DOES something is a different promise - it runs only on
         * failure, and there is no failure to test for here. The body and the
         * finally still run, and the handler is reported so the author knows
         * what was not carried over.
         */
        if (ts.isTryStatement(s)) {
            var out = stmtsFromBlock(ts, s.tryBlock, ctx, false);
            var handler = s.catchClause && s.catchClause.block;
            var handled = handler ? stmtsFromBlock(ts, handler, ctx, false) : [];
            if (handled.length) {
                reportUnconvertible(ctx.report, ctx.file, line,
                    'catch clause with a body',
                    'Blocks have no exceptions, so there is no failure to react to. The ' +
                    'guarded statements and any finally block are kept and the recovery ' +
                    'is dropped. Move the recovery into a condition the rule can test.');
            }
            if (s.finallyBlock) {
                out = out.concat(stmtsFromBlock(ts, s.finallyBlock, ctx, false));
            }
            return out;
        }

        if (ts.isIfStatement(s)) { return ifFromTs(ts, s, ctx); }

        if (ts.isForOfStatement(s)) { return forOfToIr(ts, s, ctx); }
        if (ts.isForStatement(s)) { return forFromTs(ts, s, ctx); }

        if (ts.isWhileStatement(s)) {
            return {
                k: 'while',
                cond: exprFromTs(ts, s.expression, ctx),
                body: stmtsFromBlock(ts, blockOf(ts, s.statement), ctx)
            };
        }

        if (ts.isBreakStatement(s)) { return { k: 'control', word: 'break', blockType: 'Break' }; }
        if (ts.isContinueStatement(s)) { return { k: 'control', word: 'continue', blockType: 'Continue' }; }

        /* A RETURN IS A FLAG, NOT AN ABORT.
         *
         * Portal has no return value at all: subroutineInstanceBlock has no
         * output and can never sit in a value socket, so a result has to be
         * parked in a variable and read after the call - which is what Portal
         * authors do by hand.
         *
         * Early exit is the harder half. Abort's own help text says it 'stops
         * the execution of a list of ACTIONS in a RULE', and the one piece of
         * testimony about calling it inside a SUBROUTINE says it kills the
         * CALLING RULE rather than returning to it. That is not proven, so
         * this does not depend on it: a guard flag behaves the same whichever
         * way Abort turns out to work.
         *
         *     done_<fn> = false            at entry
         *     return v   ->  ret_<fn> = v; done_<fn> = true
         *     everything after a possible return is wrapped in If (Not done_<fn>)
         *
         * Marked here and rewritten by guardReturns once the whole body is
         * built, because guarding needs to see what FOLLOWS a return.
         */
        if (ts.isReturnStatement(s)) {
            var rv = s.expression ? exprFromTs(ts, s.expression, ctx) : null;
            return { k: 'ret', value: rv };
        }

        if (ts.isVariableStatement(s)) {
            /* `let a = 1, b = 2` is two SetVariables. stmtsFromBlock already
             * splices an array, so several statements may come back from one. */
            var decls = (s.declarationList && s.declarationList.declarations) || [];
            var made = [], di, d, nm;
            for (di = 0; di < decls.length; di++) {
                d = decls[di];
                if (!d.name || !ts.isIdentifier(d.name)) {
                    /* Destructuring has no block form: one slot, one name. */
                    made.push(unconvertibleStmt(ts, s, ctx, line, 'destructuring declaration'));
                    continue;
                }
                nm = d.name.text;
                if (!d.initializer) {
                    /* Declared with no value. The slot still has to exist, or a
                     * later read of it resolves to nothing. */
                    localRef(ctx, nm);
                    continue;
                }
                made.push({ k: 'setVar', ref: localRef(ctx, nm),
                            value: exprFromTs(ts, d.initializer, ctx) });
            }
            if (!made.length) { return null; }
            return made.length === 1 ? made[0] : made;
        }

        return unconvertibleStmt(ts, s, ctx, line, ts.SyntaxKind[s.kind]);
    }

    function blockOf(ts, node) {
        if (!node) { return null; }
        if (ts.isBlock(node)) { return node; }
        return { statements: [node], kind: -1, _synthetic: true };
    }

    function unconvertibleStmt(ts, s, ctx, line, what) {
        var txt = shortText(ts, ctx.sf, s);
        reportUnconvertible(ctx.report, ctx.file, line, what + ': ' + txt,
            'No block equivalent. Rewrite it with mod.* calls, a subroutine call, or a ' +
            'Portal variable, or keep this logic in a TypeScript only module.');
        return { k: 'comment', text: 'NOT CONVERTED (' + what + ' at ' + ctx.file + ':' + line + '): ' + txt };
    }

    function ifFromTs(ts, s, ctx) {
        var branches = [];
        var elseBody = null;
        var cur = s;
        while (cur) {
            branches.push({
                cond: exprFromTs(ts, cur.expression, ctx),
                body: stmtsFromBlock(ts, blockOf(ts, cur.thenStatement), ctx)
            });
            if (cur.elseStatement && ts.isIfStatement(cur.elseStatement)) {
                cur = cur.elseStatement;
            } else {
                if (cur.elseStatement) {
                    elseBody = stmtsFromBlock(ts, blockOf(ts, cur.elseStatement), ctx);
                }
                cur = null;
            }
        }
        return { k: 'if', branches: branches, elseBody: elseBody };
    }

    /* for (const x of EXPR) BODY
     *
     *   __tmp_arr = EXPR                      (hoisted: evaluated once, and a
     *                                          side effecting EXPR must not run
     *                                          twice just because the lowering
     *                                          needs it for both the bound and
     *                                          the element)
     *   ForVariable __tmp_idx from 0 to CountOf(__tmp_arr) step 1 {
     *       x = ValueInArray(__tmp_arr, __tmp_idx)
     *       BODY
     *   }
     *
     * The bound is EXCLUSIVE, which is the convention this file already uses:
     * the emitter writes `for (let i = from; i < to; ...)`.
     */
    /* Does this statement, or anything inside it, hand control back early? */
    function mayReturn(s) {
        if (!s || typeof s !== 'object') { return false; }
        if (s.k === 'ret') { return true; }
        var i, j;
        if (s.k === 'if') {
            for (i = 0; i < (s.branches || []).length; i++) {
                var body = s.branches[i].body || [];
                for (j = 0; j < body.length; j++) { if (mayReturn(body[j])) { return true; } }
            }
            for (j = 0; j < (s.elseBody || []).length; j++) {
                if (mayReturn(s.elseBody[j])) { return true; }
            }
            return false;
        }
        if (s.k === 'for' || s.k === 'while') {
            for (j = 0; j < (s.body || []).length; j++) { if (mayReturn(s.body[j])) { return true; } }
        }
        return false;
    }

    /* Rewrites a built body so early returns work without Abort. Right to
     * left: the first statement that may return keeps its place, and
     * everything after it moves inside If (Not done). Recursing into the tail
     * handles several returns; recursing into branches and loop bodies handles
     * a return nested inside them. */
    function guardReturns(stmts, ctx, refs) {
        var out = [], i, s;
        for (i = 0; i < stmts.length; i++) {
            s = stmts[i];
            if (s && s.k === 'ret') {
                if (s.value !== null && s.value !== undefined) {
                    out.push({ k: 'setVar', ref: refs.ret, value: s.value });
                }
                out.push({ k: 'setVar', ref: refs.done, value: { k: 'bool', v: true } });
            } else {
                if (s && s.k === 'if') {
                    for (var b = 0; b < (s.branches || []).length; b++) {
                        s.branches[b].body = guardReturns(s.branches[b].body || [], ctx, refs);
                    }
                    if (s.elseBody) { s.elseBody = guardReturns(s.elseBody, ctx, refs); }
                } else if (s && (s.k === 'for' || s.k === 'while')) {
                    s.body = guardReturns(s.body || [], ctx, refs);
                    /* A return inside a loop has to leave the loop as well, or
                     * the remaining iterations still run. */
                    if (mayReturn(s)) {
                        s.body.push({ k: 'if',
                            branches: [{ cond: { k: 'getVar', ref: refs.done },
                                         body: [{ k: 'control', word: 'break', blockType: 'Break' }] }],
                            elseBody: null });
                    }
                }
                out.push(s);
            }
            if (mayReturn(s)) {
                var rest = guardReturns(stmts.slice(i + 1), ctx, refs);
                if (rest.length) {
                    out.push({ k: 'if',
                        branches: [{ cond: { k: 'call', fn: 'Not',
                                             args: [{ k: 'getVar', ref: refs.done }] },
                                     body: rest }],
                        elseBody: null });
                }
                return out;
            }
        }
        return out;
    }

    /* Applied to a whole function body. No return in it means no flag, no
     * wrapper and no cost - which is most rules. */
    function applyReturnGuards(stmts, ctx) {
        var i, any = false;
        for (i = 0; i < stmts.length; i++) { if (mayReturn(stmts[i])) { any = true; break; } }
        if (!any) { return stmts; }
        /* THE RETURN FLAGS ARE FRAME STATE, NOT MOD STATE.
         *
         * Blocks have no early return, so a function that returns gets two
         * variables: one saying it has finished, one holding the value. Those
         * were being declared as ordinary module bindings, which meant every
         * subroutine permanently owned two of the 128 slots Portal allows. On
         * TDM that was 48 slots - a third of the budget - spent on plumbing, and
         * it pushed the workspace over the limit so Portal would refuse it.
         *
         * They live exactly as long as the call does, same as any local, so they
         * are registered as poolable and the slot allocator gives two functions
         * the same slot whenever neither can be running inside the other. */
        var doneIdent = 'done_' + (ctx.fnName || 'rule');
        var retIdent = 'ret_' + (ctx.fnName || 'rule');
        notePoolable(ctx, doneIdent);
        notePoolable(ctx, retIdent);
        var refs = {
            done: varRefFromIdent(ctx, doneIdent, null),
            ret: varRefFromIdent(ctx, retIdent, null)
        };
        var body = guardReturns(stmts, ctx, refs);
        body.unshift({ k: 'setVar', ref: refs.done, value: { k: 'bool', v: false } });
        return body;
    }

    /* null when this is not an assignment; '' for a plain one; otherwise the
     * name of the Portal block that combines the old value with the new. */
    function assignOp(ts, kind) {
        if (kind === ts.SyntaxKind.EqualsToken) { return ''; }
        if (kind === ts.SyntaxKind.PlusEqualsToken) { return 'Add'; }
        if (kind === ts.SyntaxKind.MinusEqualsToken) { return 'Subtract'; }
        if (kind === ts.SyntaxKind.AsteriskEqualsToken) { return 'Multiply'; }
        if (kind === ts.SyntaxKind.SlashEqualsToken) { return 'Divide'; }
        return null;
    }

    /* The variable a bare name assigns to, or null if it names nothing. */
    function assignTargetRef(ts, lhs, ctx) {
        if (!ts.isIdentifier(lhs)) { return null; }
        var name = lhs.text;
        if (ctx.locals && has(ctx.locals, name)) { return ctx.locals[name]; }
        if (ctx.program.varByIdent && has(ctx.program.varByIdent, name)) {
            return varRefFromIdent(ctx, name, null);
        }
        if (/(Global|Player|Team)Var$/.test(name)) { return varRefFromIdent(ctx, name, null); }
        /* Assigning to something that was never declared anywhere this converter
         * saw. Minting a variable would invent state, so it is reported. */
        return null;
    }

    /* The index a step of a path reads, or null when it is not a step at all. */
    function pathIndex(ts, lhs, ctx) {
        if (ts.isElementAccessExpression(lhs)) {
            return exprFromTs(ts, lhs.argumentExpression, ctx);
        }
        if (ts.isPropertyAccessExpression(lhs) && lhs.name && ctx.program &&
            ctx.program.fieldSlots && has(ctx.program.fieldSlots, lhs.name.text)) {
            return { k: 'num', v: ctx.program.fieldSlots[lhs.name.text] };
        }
        return null;
    }

    /* WRITING THROUGH A PATH, WHEN PORTAL CAN ONLY WRITE TO A VARIABLE.
     *
     * SetVariableAtIndex is the only way a workspace changes one slot, and its
     * first input is a VARIABLE, not an expression. So `stats[id] = x` writes
     * directly, and `stats[id].d = x` cannot: the thing being written into is a
     * value pulled out of stats, and storing to it changes nothing.
     *
     * TDM counts a death with `playersStats[eventPlayerId].d++`, so this was not
     * an exotic case - it was the whole kill phase, silently dropped.
     *
     * The deeper write is done the only way the blocks allow: lift the inner
     * array into a temporary variable, change the slot there, then store the
     * temporary back where it came from. That last step is itself a write
     * through a path, one level shorter, so this recurses and bottoms out at a
     * plain variable.
     */
    function writeThroughPath(ts, lhs, valueIr, ctx) {
        var ref = assignTargetRef(ts, lhs, ctx);
        if (ref) { return [{ k: 'setVar', ref: ref, value: valueIr }]; }

        /* A table kept per object: one slot on that object. */
        var objT = objTableAccess(ts, lhs, ctx);
        if (objT) { return [{ k: 'setVar', ref: objT, value: valueIr }]; }

        /* A field of a per object record: one slot on the object, written
         * directly. No read, change and put back, because there is no array. */
        if (ts.isPropertyAccessExpression(lhs) && lhs.name && ctx.program.recFields &&
            has(ctx.program.recFields, lhs.name.text)) {
            var robj = exprFromTs(ts, lhs.expression, ctx);
            if (robj) {
                return [{ k: 'setVar', ref: recFieldRef(ctx, lhs.name.text, robj), value: valueIr }];
            }
        }

        var idx = pathIndex(ts, lhs, ctx);
        if (!idx) { return null; }
        var base = lhs.expression;

        var baseRef = assignTargetRef(ts, base, ctx);
        if (baseRef) { return [{ k: 'setVarAt', ref: baseRef, index: idx, value: valueIr }]; }

        var baseIr = exprFromTs(ts, base, ctx);
        if (!baseIr) { return null; }
        var tmp = tempRef(ctx, 'slot');
        var back = writeThroughPath(ts, base, { k: 'getVar', ref: tmp }, ctx);
        if (!back) { return null; }
        return [
            { k: 'setVar', ref: tmp, value: baseIr },
            { k: 'setVarAt', ref: tmp, index: idx, value: valueIr }
        ].concat(back);
    }

    /* `x op= v` is `x = x op v`, so the read side is just the expression. */
    function combineAssigned(ts, lhs, op, rhs, ctx) {
        if (!op) { return rhs; }
        var cur = exprFromTs(ts, lhs, ctx);
        if (!cur) { return null; }
        /* `+=` ON TEXT IS THE SAME TRAP AS `+` ON TEXT, AND THIS PATH MISSED IT.
         *
         * The binary form has checked for a string join for a long time. The
         * compound form went straight to Add, so `s += ' more'` produced a
         * Number block holding text, and Blockly refused the whole workspace on
         * it with "expected Number,Vector, found String". One of these stopped
         * a 52,000 block project from opening at all.
         *
         * Same answer as the binary form: Concat where both sides really are
         * text, and an honest report where they are not, since Portal has no
         * block that turns a Number into a String. */
        if (op === 'Add' && (isStringy(cur) || isStringy(rhs))) {
            if (isStringy(cur) && isStringy(rhs)) {
                return { k: 'call', fn: 'Concat', args: [cur, rhs] };
            }
            reportUnconvertible(ctx.report, ctx.file, 0,
                'adding text to a variable with +=',
                'One side is text and the other is not. Blocks cannot turn a number into ' +
                'text, so put the sentence in strings.json with a placeholder and use ' +
                'Message with that key.');
            return null;
        }
        return { k: 'call', fn: op, args: [cur, rhs] };
    }

    function assignToIr(ts, e, ctx, line) {
        var op = assignOp(ts, e.operatorToken.kind);

        /* Storing a whole record against an object id. The literal never becomes
         * a value; each field is written to its own slot on the object. */
        if (!op && ts.isObjectLiteralExpression(e.right)) {
            var target = recordTableObject(ts, e.left, ctx);
            if (target) {
                var out = [], i;
                for (i = 0; i < e.right.properties.length; i++) {
                    var pr = e.right.properties[i];
                    var fname = pr.name && ts.isIdentifier(pr.name) ? pr.name.text : null;
                    if (!fname) { return null; }
                    var v = ts.isShorthandPropertyAssignment(pr)
                        ? exprFromTs(ts, pr.name, ctx)
                        : exprFromTs(ts, pr.initializer, ctx);
                    if (!v) { return null; }
                    out.push({ k: 'setVar', ref: recFieldRef(ctx, fname, target), value: v });
                }
                if (out.length) { return out.length === 1 ? out[0] : out; }
            }
        }

        var rhs = exprFromTs(ts, e.right, ctx);
        if (!rhs) { return null; }
        var value = combineAssigned(ts, e.left, op, rhs, ctx);
        if (!value) { return null; }
        var out = writeThroughPath(ts, e.left, value, ctx);
        if (!out) { return null; }
        return out.length === 1 ? out[0] : out;
    }

    function forOfToIr(ts, s, ctx) {
        /* DO NOT HOIST WHAT IS ALREADY A VARIABLE.
         *
         * The hoist exists so a side effecting expression runs once even
         * though the lowering needs it twice, for the bound and for the
         * element. Reading a variable has no side effects, so hoisting one
         * buys nothing - and it costs a slot that can never be packed, because
         * it holds an array and arrays do not nest.
         *
         * On one real project that was 178 array temps, tainting 97 of the 491
         * pooled slots and keeping the whole workspace over the variable
         * ceiling. Reading the variable each time also matches what the source
         * says more closely than freezing a copy of it.
         */
        var arrExpr = exprFromTs(ts, s.expression, ctx);
        var out = [];
        var arrRead;
        if (arrExpr && arrExpr.k === 'getVar') {
            arrRead = function () { return { k: 'getVar', ref: arrExpr.ref }; };
        } else {
            var arrTmp = tempRef(ctx, 'arr');
            out.push({ k: 'setVar', ref: arrTmp, value: arrExpr });
            arrRead = function () { return { k: 'getVar', ref: arrTmp }; };
        }
        var idxTmp = tempRef(ctx, 'idx');

        var body = [];
        var decls = s.initializer && s.initializer.declarations;
        if (decls && decls.length && ts.isIdentifier(decls[0].name)) {
            body.push({
                k: 'setVar',
                ref: localRef(ctx, decls[0].name.text),
                value: { k: 'call', fn: 'ValueInArray',
                         args: [arrRead(), { k: 'getVar', ref: idxTmp }] }
            });
        } else {
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, s),
                'for-of over a destructured element',
                'Give the loop a single named element, so it can be one variable.');
        }
        body = body.concat(stmtsFromBlock(ts, blockOf(ts, s.statement), ctx));

        out.push({
            k: 'for', ref: idxTmp,
            from: { k: 'num', v: 0 },
            to: { k: 'call', fn: 'CountOf', args: [arrRead()] },
            step: { k: 'num', v: 1 },
            body: body
        });
        return out;
    }

    function forFromTs(ts, s, ctx) {
        /* for (let iteratorVar = A; iteratorVar < B; iteratorVar += C) {
         *     mod.SetVariable(<ref>, iteratorVar); ... } */
        var init = s.initializer, loopVar = null, from = null;
        if (init && init.declarations && init.declarations.length &&
            ts.isIdentifier(init.declarations[0].name)) {
            loopVar = init.declarations[0].name.text;
            from = exprFromTs(ts, init.declarations[0].initializer, ctx);
        }
        var to = s.condition && s.condition.right ? exprFromTs(ts, s.condition.right, ctx) : null;
        var step = s.incrementor && s.incrementor.right ? exprFromTs(ts, s.incrementor.right, ctx) : { k: 'num', v: 1 };
        var body = blockOf(ts, s.statement);
        var ref = null;
        var stmts = [];
        if (body && body.statements && body.statements.length) {
            var first = body.statements[0];
            if (ts.isExpressionStatement(first) && ts.isCallExpression(first.expression) &&
                isModCall(ts, first.expression, 'SetVariable')) {
                ref = exprFromTs(ts, first.expression.arguments[0], ctx);
                stmts = stmtsFromBlock(ts, { statements: sliceStatements(body.statements, 1) }, ctx);
            }
        }
        if (!ref) {
            /* MINT THE COUNTER INSTEAD OF DEMANDING ONE.
             *
             * This used to insist the author had already written
             * mod.SetVariable(iGlobalVar, i) as the first statement - which is
             * true of TypeScript generated FROM blocks and of nothing else. A
             * counter is just a variable; the compiler can mint it. The name is
             * registered as the loop variable's local so reads of `i` in the
             * body resolve to it.
             */
            ref = tempRef(ctx, 'i');
            if (loopVar) {
                if (!ctx.locals) { ctx.locals = {}; }
                ctx.locals[loopVar] = ref;
            }
            stmts = stmtsFromBlock(ts, body, ctx);
        }
        return { k: 'for', ref: ref, from: from, to: to, step: step, body: stmts };
    }

    function sliceStatements(list, n) {
        var out = [], i;
        for (i = n; i < list.length; i++) { out.push(list[i]); }
        return out;
    }

    /* Subroutine call arguments, minus the trailing eventInfo the TypeScript
     * side threads through for the implicit block event context. */
    function subCallArgs(ts, args, ctx) {
        var list = [], i;
        if (!args) { return list; }
        for (i = 0; i < args.length; i++) { list.push(args[i]); }
        if (list.length && ts.isIdentifier(list[list.length - 1]) &&
            list[list.length - 1].text === 'eventInfo') {
            list.pop();
        }
        return argsListFromTs(ts, list, ctx);
    }

    function argsListFromTs(ts, args, ctx) {
        var out = [], i;
        if (!args) { return out; }
        for (i = 0; i < args.length; i++) { out.push(exprFromTs(ts, args[i], ctx)); }
        return out;
    }

    function argsFromTs(ts, fn, args, ctx) {
        var out = argsListFromTs(ts, args, ctx);
        return out;
    }

    /* JS SPELLS ARITHMETIC `Math.x`. PORTAL SPELLS IT AS BLOCKS.
     *
     * Every one of these exists in the block catalogue, so `Math.floor` being
     * reported as an unconvertible call was a naming gap and nothing deeper.
     * Min is the exception: there is a Max block and no Min, so it is written
     * as the comparison it stands for. */
    function mathCallToIr(ts, e, ctx) {
        var callee = e.expression;
        if (!ts.isPropertyAccessExpression(callee) || !ts.isIdentifier(callee.expression) ||
            callee.expression.text !== 'Math') {
            return null;
        }
        var name = callee.name.text;
        var a = e.arguments || [];
        function arg(i) { return exprFromTs(ts, a[i], ctx); }

        if (name === 'random' && a.length === 0) {
            return { k: 'call', fn: 'RandomReal', args: [{ k: 'num', v: 0 }, { k: 'num', v: 1 }] };
        }
        var ONE = { floor: 'Floor', ceil: 'Ceiling', round: 'RoundToInteger',
                    abs: 'AbsoluteValue', sqrt: 'SquareRoot' };
        if (has(ONE, name) && a.length === 1) {
            return { k: 'call', fn: ONE[name], args: [arg(0)] };
        }
        if (name === 'pow' && a.length === 2) {
            return { k: 'call', fn: 'RaiseToPower', args: [arg(0), arg(1)] };
        }
        /* Max and Min take any number of arguments in JS and exactly two in
         * Portal, so they fold left: max(a,b,c) is max(max(a,b),c). */
        if ((name === 'max' || name === 'min') && a.length >= 1) {
            var acc = arg(0), i;
            if (!acc) { return null; }
            for (i = 1; i < a.length; i++) {
                var nxt = arg(i);
                if (!nxt) { return null; }
                acc = name === 'max'
                    ? { k: 'call', fn: 'Max', args: [acc, nxt] }
                    /* No Min block exists, so it is spelled out. */
                    : { k: 'call', fn: 'IfThenElse', args: [
                        { k: 'call', fn: 'LessThan', args: [acc, nxt] }, acc, nxt] };
            }
            return acc;
        }
        return null;
    }

    /* The array a record literal stands for. Fields land on their own index and
     * the gaps are filled with 0, because Portal builds an array by appending
     * and cannot leave a hole in the middle. */
    function recordToIr(ts, e, ctx) {
        if (!ctx.program || !ctx.program.fieldSlots) { return null; }
        var slots = ctx.program.fieldSlots, byIndex = {}, top = -1, i;
        for (i = 0; i < e.properties.length; i++) {
            var p = e.properties[i], key = null, val = null;
            if (ts.isShorthandPropertyAssignment(p) && p.name && ts.isIdentifier(p.name)) {
                key = p.name.text; val = p.name;
            } else if (ts.isPropertyAssignment(p) && p.name && ts.isIdentifier(p.name)) {
                key = p.name.text; val = p.initializer;
            } else {
                return null;
            }
            if (!has(slots, key)) { return null; }
            byIndex[slots[key]] = exprFromTs(ts, val, ctx);
            if (slots[key] > top) { top = slots[key]; }
        }
        var built = { k: 'call', fn: 'EmptyArray', args: [] };
        for (i = 0; i <= top; i++) {
            built = { k: 'call', fn: 'AppendToArray',
                args: [built, has(byIndex, i) ? byIndex[i] : { k: 'num', v: 0 }] };
        }
        return built;
    }

    /* Reading a field is reading the index it was given. Only for names some
     * object literal in this program actually declares - anything else is a
     * property of something that is not a record, and stays reported. */
    function fieldReadToIr(ts, e, ctx) {
        if (!ctx.program) { return null; }
        var name = e.name && e.name.text;
        if (!name) { return null; }

        /* A field of a per object record lives in a slot ON that object. This is
         * checked first because the array form below cannot hold it: a record
         * inside an array is the one thing Portal flattens. */
        if (ctx.program.recFields && has(ctx.program.recFields, name)) {
            var obj = exprFromTs(ts, e.expression, ctx);
            if (obj) { return { k: 'getVar', ref: recFieldRef(ctx, name, obj) }; }
        }

        if (!ctx.program.fieldSlots || !has(ctx.program.fieldSlots, name)) { return null; }
        var owner = exprFromTs(ts, e.expression, ctx);
        if (!owner) { return null; }
        return { k: 'call', fn: 'ValueInArray',
            args: [owner, { k: 'num', v: ctx.program.fieldSlots[name] }] };
    }

    /* Does this expression produce a Bool? Answered from the block it became,
     * because that is what Portal will type check. Anything the catalogue says
     * returns something else, or that we could not read at all, is not one. */
    var BOOL_BLOCKS = {
        Not: 1, And: 1, Or: 1, Equals: 1, NotEqualTo: 1,
        GreaterThan: 1, GreaterThanEqualTo: 1, LessThan: 1, LessThanEqualTo: 1,
        ArrayContains: 1, IsTrueForAny: 1, IsTrueForAll: 1
    };
    /* PORTAL HAS NO TRUTHINESS.
     *
     * JavaScript will test anything: `if (count)`, `secondsLeft || 60`. Every
     * Portal socket that takes a condition takes a Bool and nothing else, and
     * putting a Number in one makes the editor refuse the entire workspace:
     *
     *   GetMatchTimeElapsed expected Number, found Boolean
     *
     * A number has an exact test and it is worth writing out, because `count`
     * meaning `count is not zero` is the whole intent. Anything else - a
     * variable, an event value, a subroutine result - carries no type we can
     * see, and guessing at one would quietly change what the mod does. Those
     * are reported instead, and the socket is left open.
     */
    function asBoolean(ir, ctx, where) {
        if (looksBoolean(ir)) { return ir; }
        if (ir && ir.k === 'num') {
            return { k: 'bool', v: ir.v !== 0 };
        }
        if (ir && ir.k === 'call') {
            var rt = cleanRet(ir.fn);
            if (rt === 'Bool') { return ir; }
            if (rt === 'Number') {
                return { k: 'call', fn: 'NotEqualTo', args: [ir, { k: 'num', v: 0 }] };
            }
            if (rt === 'String') {
                return { k: 'call', fn: 'NotEqualTo', args: [ir, { k: 'str', v: '' }] };
            }
            if (rt === 'Array') {
                /* An array is truthy when it has something in it. */
                return { k: 'call', fn: 'NotEqualTo', args: [
                    { k: 'call', fn: 'CountOf', args: [ir] }, { k: 'num', v: 0 }] };
            }
            if (rt) {
                /* A Player, a Team, a Vehicle: JavaScript tests whether the
                 * handle is there at all, and IsValid is exactly that test. */
                return { k: 'call', fn: 'IsValid', args: [ir] };
            }
            /* Anything else is left exactly as it was. An overloaded block
             * carries all of its signatures concatenated in ret, so a test for
             * 'not Bool' matches GetSoldierState and every other overload, and
             * reporting those emptied sockets that had always been correct. */
        }
        /* An event value is a handle of whatever the event carries, so the same
         * question applies: is it there. */
        if (ir && ir.k === 'event') {
            return { k: 'call', fn: 'IsValid', args: [ir] };
        }

        /* ONLY CHANGE WHAT IS KNOWN TO BE WRONG.
         *
         * A variable read, a subroutine parameter, an event value or a
         * subroutine result carries no type here. Most of them genuinely hold a
         * boolean, and an earlier version of this reported every one of them and
         * emptied the socket, which broke two round trip tests that had been
         * green all along. Left alone, they behave exactly as they did before.
         */
        return ir;
    }

    /* Certain this is NOT a boolean. Only literals and blocks whose single
     * declared return type says so; an unknown stays unknown. */
    /* WHAT A BLOCK RETURNS, OR NOTHING AT ALL.
     *
     * An overloaded block has every one of its signatures concatenated into
     * this field, so GetSoldierState's ret reads
     *   NumberGetSoldierState(Player, PlayerStateBoolItem): Bool...
     * Treating that as a type name matched the wrong branch three separate
     * times today and rewrote arguments that had always been right.
     *
     * A real type is a bare word. Anything carrying a bracket or a colon is
     * several signatures run together, and the only safe answer is that we do
     * not know. */
    /* Is this value definitely text? A literal, or a block whose one clean
     * return type is String. Anything uncertain answers no, because guessing
     * wrong here turns arithmetic into a text join. */
    function isStringy(ir) {
        if (!ir) { return false; }
        if (ir.k === 'str') { return true; }
        if (ir.k === 'call') { return cleanRet(ir.fn) === 'String'; }
        return false;
    }

    /* The same value as a String, or null when blocks cannot express it.
     *
     * Portal has no number-to-text block: the definitions carry Concat and the
     * text_* helpers, and nothing that turns a Number into a String. So
     * `'Round ' + n` genuinely cannot be built out of blocks, and saying so is
     * the honest answer. What CAN be built is text joined to text, which is
     * what this allows through. */
    function asString(ir, ctx, line) {
        return isStringy(ir) ? ir : null;
    }

    function cleanRet(fn) {
        var ce = CATALOG[fn];
        if (!ce || !ce.ret) { return null; }
        return /^[A-Za-z_][A-Za-z0-9_]*$/.test(ce.ret) ? ce.ret : null;
    }

    function notBoolean(ir) {
        if (!ir) { return false; }
        if (ir.k === 'num' || ir.k === 'str') { return true; }
        if (ir.k === 'call') {
            if (has(BOOL_BLOCKS, ir.fn)) { return false; }
            if (ir.fn === 'EmptyArray' || ir.fn === 'AppendToArray') { return true; }
            var r = cleanRet(ir.fn);
            if (r && r !== 'Bool') { return true; }
        }
        return false;
    }

    function looksBoolean(ir) {
        if (!ir) { return false; }
        if (ir.k === 'bool') { return true; }
        if (ir.k === 'gap') { return false; }
        if (ir.k === 'call') {
            if (has(BOOL_BLOCKS, ir.fn)) { return true; }
            var ce = CATALOG[ir.fn];
            return !!(ce && ce.ret === 'Bool');
        }
        /* A variable read, an event value or a subroutine result carries no
         * type we can see, so it is left to the safe form. */
        return false;
    }

    function exprFromTs(ts, e, ctx) {
        if (!e) { return null; }
        while (ts.isParenthesizedExpression(e) || ts.isAsExpression(e) ||
            ts.isAwaitExpression(e) ||
            (ts.isNonNullExpression && ts.isNonNullExpression(e))) {
            e = e.expression;
        }

        if (ts.isNumericLiteral(e)) { return { k: 'num', v: Number(e.text) }; }
        if (ts.isPrefixUnaryExpression(e) && e.operator === ts.SyntaxKind.MinusToken &&
            ts.isNumericLiteral(e.operand)) {
            return { k: 'num', v: -Number(e.operand.text) };
        }
        if (ts.isStringLiteral(e) || ts.isNoSubstitutionTemplateLiteral(e)) {
            return { k: 'str', v: e.text };
        }
        if (e.kind === ts.SyntaxKind.TrueKeyword) { return { k: 'bool', v: true }; }
        if (e.kind === ts.SyntaxKind.FalseKeyword) { return { k: 'bool', v: false }; }

        if (ts.isPrefixUnaryExpression(e) && e.operator === ts.SyntaxKind.ExclamationToken) {
            return { k: 'call', fn: 'Not', args: [exprFromTs(ts, e.operand, ctx)] };
        }

        if (ts.isArrowFunction(e) || ts.isFunctionExpression(e)) {
            var b = e.body;
            if (b && !ts.isBlock(b)) { return { k: 'lambda', body: exprFromTs(ts, b, ctx) }; }
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'closure with a statement body',
                'Array helper blocks take a single expression. Reduce the closure to one ' +
                'expression, or move the work into a subroutine.');
            return { k: 'gap' };
        }

        if (ts.isPropertyAccessExpression(e)) {
            var owner = e.expression;
            if (ts.isIdentifier(owner) && owner.text === 'mod') {
                /* mod.<Enum>.<Member> arrives as a nested access, handled below. */
                return { k: 'call', fn: e.name.text, args: [] };
            }
            if (ts.isPropertyAccessExpression(owner) && ts.isIdentifier(owner.expression) &&
                owner.expression.text === 'mod') {
                return { k: 'enum', enumName: owner.name.text, member: e.name.text };
            }

            /* Anything else that turns out to be a mod path: an alias standing
             * in for one, or a namespace nested deeper than one level, which
             * `mod.stringkeys.gunfight.loadout.weapons.m433` is. The first
             * segment names the namespace and everything after it is the member,
             * dots included, so writing it back out reproduces the original
             * exactly. */
            var mp = modPath(ts, e, ctx);
            if (mp && mp.length >= 2) {
                return { k: 'enum', enumName: mp[0], member: mp.slice(1).join('.') };
            }
            if (ts.isIdentifier(owner) && owner.text === 'eventInfo') {
                return { k: 'event', name: e.name.text, blockType: 'Event' + capitalise(e.name.text.slice(5)) };
            }
            if (ts.isIdentifier(owner) && owner.text === 'vars') {
                return varRefFromIdent(ctx, e.name.text, null);
            }
            /* A field of a module constant is known now, so it is replaced by
             * the value it reads rather than reported. */
            var constNode = constRecordNodeFor(ts, e, ctx);
            if (constNode) { return exprFromTs(ts, constNode, ctx); }

            /* UI.COLORS.WHITE and UI.ROOT_NODE. A colour table is built once at
             * module load, so it reads a variable round start filled in rather
             * than running a CreateVector at every widget. */
            var uiv = uiNamespaceValue(ts, e, ctx);
            if (uiv) { return uiv; }

            /* An array knows how long it is, and Portal asks with CountOf. This
             * rule existed further down the function, where it was unreachable:
             * every property access is decided here and this branch returns, so
             * `spawners.length` was reported as unconvertible by a converter
             * that had the answer written twenty lines below. */
            if (e.name && e.name.text === 'length') {
                return { k: 'call', fn: 'CountOf', args: [exprFromTs(ts, e.expression, ctx)] };
            }

            var fld = fieldReadToIr(ts, e, ctx);
            if (fld) { return fld; }

            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'property access ' + shortText(ts, ctx.sf, e),
                'Blocks address values through mod.* calls and workspace variables only.');
            return { k: 'gap' };
        }

        if (ts.isIdentifier(e)) {
            var name = e.text;
            /* A NAME THIS FUNCTION DECLARED IS THAT DECLARATION, WHATEVER IT
             * IS CALLED.
             *
             * Event values are recognised by their name, and that guess used to
             * run first. TDM opens its death handler with
             *   const eventPlayerId = mod.GetObjId(eventPlayer);
             * which is a local, but matched the pattern - so every read of it
             * compiled to eventInfo.eventPlayerId, a field no event carries. The
             * scoreboard update for the whole kill phase then ran on undefined.
             * Parameters and locals are checked first now; the naming rule only
             * applies to a name nothing in scope declares. */
            var declaredIdx = paramIndex(ctx, name);
            if (declaredIdx >= 0) { return { k: 'arg', index: declaredIdx, name: name }; }
            if (ctx.locals && has(ctx.locals, name)) {
                return { k: 'getVar', ref: ctx.locals[name] };
            }
            /* The handler's own name for one of this event's values. Checked
             * after locals so an inner declaration still shadows, and before
             * the naming rule below so a handler that uses the canonical names
             * behaves identically. */
            if (ctx.eventAlias && has(ctx.eventAlias, name)) {
                var canon = ctx.eventAlias[name];
                return { k: 'event', name: canon, blockType: 'Event' + capitalise(canon.slice(5)) };
            }
            if (/^event[A-Z]/.test(name)) {
                return { k: 'event', name: name, blockType: 'Event' + capitalise(name.slice(5)) };
            }
            if (name === 'currentArrayElement') { return { k: 'element' }; }
            if (/(Global|Player|Team)Var$/.test(name)) { return varRefFromIdent(ctx, name, null); }
            var pi = paramIndex(ctx, name);
            if (pi >= 0) { return { k: 'arg', index: pi, name: name }; }
            /* A local this function already declared: read its slot. Checked
             * after parameters, because a parameter of the same name shadows. */
            if (ctx.locals && has(ctx.locals, name)) {
                return { k: 'getVar', ref: ctx.locals[name] };
            }
            /* A module level binding registered while scanning the file. */
            if (ctx.program && ctx.program.varByIdent && has(ctx.program.varByIdent, name)) {
                return { k: 'getVar', ref: varRefFromIdent(ctx, name, null) };
            }
            if (/^iteratorVar\d*$/.test(name)) { return { k: 'element' }; }
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'identifier ' + name,
                'Not a workspace variable, subroutine parameter or event value.');
            return { k: 'gap' };
        }

        if (ts.isCallExpression(e)) {
            /* Checked before the mod.* dispatch below, which would otherwise
             * report Math.floor as a call into a namespace it does not know. */
            var m = mathCallToIr(ts, e, ctx);
            if (m) { return m; }
            var colE = collectionExpr(ts, e, ctx);
            if (colE) { return colE; }
            var callee = e.expression;
            if (ts.isPropertyAccessExpression(callee) && ts.isIdentifier(callee.expression)) {
                var ns = aliasedNamespace(ctx, callee.expression.text), fn = callee.name.text;
                if (ns === 'mod') {
                    if (fn === 'GetVariable') { return { k: 'getVar', ref: exprFromTs(ts, e.arguments[0], ctx) }; }
                    if (fn === 'ObjectVariable') {
                        var slotExpr = e.arguments[1];
                        var ident = null;
                        if (ts.isIdentifier(slotExpr)) { ident = slotExpr.text; }
                        else if (ts.isPropertyAccessExpression(slotExpr)) { ident = slotExpr.name.text; }
                        else if (ts.isNumericLiteral(slotExpr)) {
                            ident = 'ObjectSlot' + slotExpr.text + 'PlayerVar';
                            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                                'mod.ObjectVariable with a raw slot number ' + slotExpr.text,
                                'The variable name is not in the source. Replace the number with the ' +
                                'matching <Name>PlayerVar or <Name>TeamVar constant so the workspace ' +
                                'keeps the original variable.');
                        }
                        return varRefFromIdent(ctx, ident || 'UnknownTeamVar', exprFromTs(ts, e.arguments[0], ctx));
                    }
                    if (fn === 'CurrentArrayElement') { return { k: 'element' }; }
                    return { k: 'call', fn: fn, args: lambdaAwareArgs(ts, fn, e.arguments, ctx) };
                }
                if (ns === 'rt' || ns === 'modlib') {
                    return { k: 'call', fn: fn, args: lambdaAwareArgs(ts, fn, e.arguments, ctx) };
                }
                if (ns === 'subs') {
                    return { k: 'subCall', name: fn, args: subCallArgs(ts, e.arguments, ctx) };
                }
            }
            if (ts.isIdentifier(callee)) {
                return { k: 'subCall', name: callee.text, args: subCallArgs(ts, e.arguments, ctx) };
            }
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'call ' + shortText(ts, ctx.sf, e),
                'Only mod.*, runtime helpers and subroutine calls have block forms.');
            return { k: 'gap' };
        }

        if (ts.isBinaryExpression(e)) {
            var op = e.operatorToken.kind;
            var map = {};
            map[ts.SyntaxKind.EqualsEqualsToken] = 'Equals';
            map[ts.SyntaxKind.EqualsEqualsEqualsToken] = 'Equals';
            map[ts.SyntaxKind.ExclamationEqualsToken] = 'NotEqualTo';
            map[ts.SyntaxKind.ExclamationEqualsEqualsToken] = 'NotEqualTo';
            map[ts.SyntaxKind.LessThanToken] = 'LessThan';
            map[ts.SyntaxKind.LessThanEqualsToken] = 'LessThanEqualTo';
            map[ts.SyntaxKind.GreaterThanToken] = 'GreaterThan';
            map[ts.SyntaxKind.GreaterThanEqualsToken] = 'GreaterThanEqualTo';
            map[ts.SyntaxKind.PlusToken] = 'Add';
            map[ts.SyntaxKind.MinusToken] = 'Subtract';
            map[ts.SyntaxKind.AsteriskToken] = 'Multiply';
            map[ts.SyntaxKind.SlashToken] = 'Divide';
            map[ts.SyntaxKind.PercentToken] = 'Modulo';
            map[ts.SyntaxKind.AmpersandAmpersandToken] = 'And';
            map[ts.SyntaxKind.BarBarToken] = 'Or';

            /* `a || b` IS NOT ALWAYS A BOOLEAN TEST.
             *
             * JavaScript uses it two ways. As a test it means one or the other,
             * and Or is right. As a default it means `a` unless `a` is missing,
             * in which case `b` - and `b` is whatever type the thing is:
             *
             *     const spawners = cached || [1, 2, 3];
             *
             * Lowered to Or that puts an array in a socket that wants a Bool,
             * Portal refuses the connection, and the WHOLE workspace fails to
             * load. One line like this kept a 52,000 block project off the
             * canvas entirely.
             *
             * The two readings are told apart by what the operands are. If
             * either side is plainly not a boolean, this is the default form,
             * and it becomes the choice it actually expresses. IfThenElse
             * evaluates both arms, which costs nothing here because a default
             * value is a value, not an action.
             */
            if (op === ts.SyntaxKind.BarBarToken || op === ts.SyntaxKind.AmpersandAmpersandToken) {
                /* THE RIGHT HAND SIDE MAY NOT RUN AT ALL.
                 *
                 * `on && mod.RandomReal(1,10) > 0` calls RandomReal exactly
                 * never when on is false. And is a value block and calls it
                 * always. Where the right side does something, the operator
                 * becomes control flow so the promise JavaScript makes is kept.
                 */
                if (!isPureExpr(ts, e.right)) {
                    if (canUsePrelude(ctx)) {
                        var lHead = exprFromTs(ts, e.left, ctx);
                        var test = asBoolean(lHead, ctx, lineOf(ts, ctx.sf, e));
                        /* a && b keeps a when a is false; a || b keeps a when
                         * a is true. Either way the arm that is not taken is
                         * the untouched left value. */
                        return op === ts.SyntaxKind.AmpersandAmpersandToken
                            ? chooseIntoTemp(ts, ctx, test, e.right, e.left, 'andthen')
                            : chooseIntoTemp(ts, ctx, test, e.left, e.right, 'orelse');
                    }
                    reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                        'short circuit with a side effect in a loop test: ' +
                        shortText(ts, ctx.sf, e),
                        'And and Or evaluate both sides, so the right hand side would run ' +
                        'even when the left decides the answer. Lift it out of the loop test.');
                    return { k: 'gap' };
                }
                var lv = exprFromTs(ts, e.left, ctx);
                var rv = exprFromTs(ts, e.right, ctx);
                if (!lv || !rv) { return { k: 'gap' }; }
                /* Or and And stay the answer unless an operand is DEFINITELY
                 * not a boolean. Asking the opposite - are both definitely
                 * booleans - turns every `a || b` on two variables into a
                 * choice, because a variable carries no type here, and that
                 * rewrote workspaces that had been round tripping correctly. */
                if (!notBoolean(lv) && !notBoolean(rv)) {
                    return { k: 'call', fn: map[op], args: [lv, rv] };
                }
                /* a || b  ->  if a then a else b        a && b  ->  if a then b else a
                 * The TEST has to be a Bool even though the arms need not be. */
                var cond = asBoolean(lv, ctx, lineOf(ts, ctx.sf, e));
                return op === ts.SyntaxKind.BarBarToken
                    ? { k: 'call', fn: 'IfThenElse', args: [cond, lv, rv] }
                    : { k: 'call', fn: 'IfThenElse', args: [cond, rv, lv] };
            }

            if (map[op]) {
                var la = exprFromTs(ts, e.left, ctx);
                var ra = exprFromTs(ts, e.right, ctx);

                /* `+` ON TEXT IS NOT ADDITION.
                 *
                 * JavaScript joins strings with the same operator it adds
                 * numbers with, and Add is a Number block, so `'Round ' + n`
                 * arrived as a String in a Number socket and the editor refused
                 * the whole workspace.
                 *
                 * Portal DOES join text: Concat(String, String) -> String. This
                 * used to report the whole expression as unconvertible on the
                 * belief that it did not, which threw away every `'Round ' + n`
                 * in a project rather than lowering it.
                 *
                 * Concat takes two Strings, so a number on either side has to
                 * be made into one first. ToString is the block for that, and
                 * where there is no such block the operand is left as it is and
                 * the socket pass will empty it rather than refuse the file.
                 */
                if (op === ts.SyntaxKind.PlusToken && (isStringy(la) || isStringy(ra))) {
                    var ls = asString(la, ctx, lineOf(ts, ctx.sf, e));
                    var rs = asString(ra, ctx, lineOf(ts, ctx.sf, e));
                    if (ls && rs) { return { k: 'call', fn: 'Concat', args: [ls, rs] }; }
                    reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                        'joining text with +',
                        'One side of the join could not be turned into text. Put the whole ' +
                        'sentence in strings.json with a placeholder and use Message, so the ' +
                        'value is substituted into it.');
                    return { k: 'gap' };
                }

                return { k: 'call', fn: map[op], args: [la, ra] };
            }
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'operator ' + ts.SyntaxKind[op],
                'Use the matching mod.* function, or split the expression.');
            return { k: 'gap' };
        }

        /* THREE EXPRESSION FORMS THAT DO HAVE BLOCKS, once you stop looking
         * for a one to one match. Each of these was in the give-up list purely
         * because nothing had been written to lower it.
         *
         *   arr[i]      -> ValueInArray(arr, i)
         *   [a, b, c]   -> AppendToArray(AppendToArray(EmptyArray(), a), b)...
         *   a ? b : c   -> IfThenElse(a, b, c)      (an expression, not the If block)
         *   arr.length  -> CountOf(arr)
         */
        if (ts.isElementAccessExpression(e)) {
            var objT = objTableAccess(ts, e, ctx);
            if (objT) { return { k: 'getVar', ref: objT }; }

            /* A per object record table addressed by id gives back the OBJECT,
             * so `updateScoreboard(p, playersStats[id])` passes the player and
             * the subroutine reads the same slots off it. Checked before the
             * array read below, which would otherwise index an array that no
             * longer exists. */
            var recObj = recordTableObject(ts, e, ctx);
            if (recObj) { return recObj; }
            return { k: 'call', fn: 'ValueInArray', args: [
                exprFromTs(ts, e.expression, ctx),
                exprFromTs(ts, e.argumentExpression, ctx)
            ] };
        }

        if (ts.isObjectLiteralExpression(e)) {
            var rec = recordToIr(ts, e, ctx);
            if (rec) { return rec; }
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'ObjectLiteralExpression: ' + shortText(ts, ctx.sf, e),
                'A record becomes an array of its fields. This one has a spread or a ' +
                'computed key, so it has no fixed set of fields to lay out.');
            return { k: 'gap' };
        }

        if (ts.isArrayLiteralExpression(e)) {
            var built = { k: 'call', fn: 'EmptyArray', args: [] };
            var els = e.elements || [];
            for (var ai = 0; ai < els.length; ai++) {
                built = { k: 'call', fn: 'AppendToArray',
                          args: [built, exprFromTs(ts, els[ai], ctx)] };
            }
            return built;
        }

        if (ts.isConditionalExpression(e)) {
            var armsPure = isPureExpr(ts, e.whenTrue) && isPureExpr(ts, e.whenFalse);
            var condIr = exprFromTs(ts, e.condition, ctx);
            if (armsPure) {
                return { k: 'call', fn: 'IfThenElse', args: [
                    condIr,
                    exprFromTs(ts, e.whenTrue, ctx),
                    exprFromTs(ts, e.whenFalse, ctx)
                ] };
            }
            if (canUsePrelude(ctx)) {
                return chooseIntoTemp(ts, ctx,
                    asBoolean(condIr, ctx, lineOf(ts, ctx.sf, e)),
                    e.whenTrue, e.whenFalse, 'ternary');
            }
            reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
                'conditional with a side effect in a loop test: ' + shortText(ts, ctx.sf, e),
                'IfThenElse evaluates both arms, so the arm that was not chosen would ' +
                'still run. Lift it into a variable set before the loop.');
            return { k: 'gap' };
        }

        /* `new UI.Text({...})` is the AddUI* call its constructor makes plus
         * the name lookup that hands the widget back. The call goes into the
         * statement prelude and this is the widget. */
        if (ts.isNewExpression(e) && uiSpecFor(ts, e, ctx)) {
            var made = uiNewToIr(ts, e, ctx, lineOf(ts, ctx.sf, e));
            if (made) { return made; }
        }

        reportUnconvertible(ctx.report, ctx.file, lineOf(ts, ctx.sf, e),
            ts.SyntaxKind[e.kind] + ': ' + shortText(ts, ctx.sf, e),
            'No block equivalent for this expression form.');
        return { k: 'gap' };
    }

    function lambdaAwareArgs(ts, fn, args, ctx) {
        var out = [], i;
        var lam = has(LAMBDA_ARG, fn) ? LAMBDA_ARG[fn] : -1;
        for (i = 0; i < args.length; i++) {
            var x = exprFromTs(ts, args[i], ctx);
            if (i === lam && x && x.k !== 'lambda') { x = { k: 'lambda', body: x }; }
            out.push(x);
        }
        return out;
    }

    function capitalise(s) { return s ? s.charAt(0).toUpperCase() + s.slice(1) : s; }

    function paramIndex(ctx, name) {
        var i;
        for (i = 0; i < ctx.params.length; i++) {
            if (ctx.params[i].name === name) { return i; }
        }
        return -1;
    }

    function varRefFromIdent(ctx, ident, objectExpr) {
        var rec = ctx.program.varByIdent[ident];
        if (!rec) {
            var m = /^(.*?)(Global|Player|Team)Var$/.exec(ident);
            rec = addVariable(ctx.program, m ? m[1] : ident, m ? m[2] : 'Global',
                nextSlot(ctx.program, m ? m[2] : 'Global'), ident);
        }
        return {
            k: 'varRef', name: rec.name, scope: rec.scope, ident: rec.ident,
            slot: rec.slot, object: objectExpr || null
        };
    }

    function nextSlot(program, scope) {
        var n = 0, i;
        for (i = 0; i < program.variables.length; i++) {
            if (program.variables[i].scope === scope) { n++; }
        }
        return n;
    }

    /* ================================================================== *
     * 4. INTERMEDIATE MODEL -> BLOCKS
     * ================================================================== */

    function IdGen() {
        this.n = 0;
        this.next = function () {
            this.n++;
            var s = this.n.toString(36);
            while (s.length < 4) { s = '0' + s; }
            return 'c' + s;
        };
    }

    function irToBlocks(program, options) {
        options = options || {};
        var ids = new IdGen();
        var varIds = {}, i;

        var variables = [];
        for (i = 0; i < program.variables.length; i++) {
            var v = program.variables[i];
            var vid = 'v' + i.toString(36);
            varIds[v.ident] = vid;
            v.id = vid;
            variables.push({ name: v.name, id: vid, type: v.scope });
        }

        /* The block builder can still need a variable that no earlier pass knew
         * about, so it is handed the live lists rather than a copy. See
         * discardRef. */
        var ctx = { ids: ids, varIds: varIds, program: program, params: [],
                    variables: variables };

        /* rule chain under the mod block */
        /* THIRTY THOUSAND BLOCKS IS CORRECT AND UNREADABLE.
         *
         * A large mod converts to about 30,000 blocks. Opened flat that is a
         * wall, and the person this is for cannot read the TypeScript it came
         * from either, so they have nowhere to start.
         *
         * Collapsed, a rule or subroutine is one line carrying its name. The
         * first rule is left open so the workspace opens on something rather
         * than on a list of closed drawers. `collapsed` is a real Blockly save
         * attribute, so this survives an export into Portal rather than being a
         * trick of our own editor.
         */
        var first = null, prev = null;
        for (i = 0; i < program.rules.length; i++) {
            var rb = ruleToBlock(program.rules[i], ctx);
            /* Rules live chained inside the mod block rather than in the file
             * columns, so position cannot say where one came from. The file
             * goes on the block itself, the same as the subroutines below, so
             * the editor can group and scope them by source too. */
            rb.data = JSON.stringify({
                file: program.rules[i].file || '',
                line: program.rules[i].line || 0
            });
            if (!first) { first = rb; } else { prev.next = { block: rb }; rb.collapsed = true; }
            prev = rb;
        }

        var modBlock = {
            type: 'modBlock',
            id: ids.next(),
            x: program.layout && program.layout.mod ? program.layout.mod.x : 100,
            y: program.layout && program.layout.mod ? program.layout.mod.y : 100,
            deletable: false,
            inputs: {}
        };
        if (first) { modBlock.inputs.RULES = { block: first }; }

        var tops = [modBlock];
        if (program.orphans) {
            for (i = 0; i < program.orphans.length; i++) { tops.push(program.orphans[i]); }
        }

        /* ONE COLUMN PER SOURCE FILE.
         *
         * Subroutines used to wrap into fixed width columns at whatever point
         * they ran out of vertical room, so neighbours in the workspace had
         * nothing to do with each other. Every project in the corpus is written
         * one file per feature, so the file IS the grouping the author already
         * chose, and following it costs nothing.
         *
         * Collapsed blocks are one line each, so a column holds a whole file's
         * worth without wrapping in the common case.
         */
        var byFile = {}, order = [];
        for (i = 0; i < program.subroutines.length; i++) {
            var key = program.subroutines[i].file || '';
            if (!has(byFile, key)) { byFile[key] = []; order.push(key); }
            byFile[key].push(program.subroutines[i]);
        }
        /* An index file is the way in, so it leads. The rest keep a stable
         * order rather than the order the scan happened to reach them in. */
        order.sort(function (a, b) {
            var ai = /(^|\/)index\.ts$/.test(a) ? 0 : 1;
            var bi = /(^|\/)index\.ts$/.test(b) ? 0 : 1;
            if (ai !== bi) { return ai - bi; }
            return a < b ? -1 : (a > b ? 1 : 0);
        });

        var COL_W = 420, ROW_H = 44, COL_TOP = (modBlock.y || 100);
        var colX = (modBlock.x || 100) + 1400;
        for (var fi = 0; fi < order.length; fi++) {
            var list = byFile[order[fi]];
            var colY = COL_TOP;
            for (var si = 0; si < list.length; si++) {
                var s = list[si];
                var sb = subroutineToBlock(s, ctx);
                if (options.keepLayout !== false && s.x) {
                    sb.x = s.x; sb.y = s.y;
                } else {
                    sb.x = colX; sb.y = colY;
                    colY += ROW_H;
                }
                /* WHICH FILE THIS CAME FROM, WRITTEN DOWN.
                 *
                 * The layout already puts one file per column, but a column is
                 * a position and nothing more: the editor could see 29 columns
                 * and not one file name, so it could not group, tab, colour or
                 * scope a search by anything meaningful. `data` is a standard
                 * Blockly block field and the serializer round trips it, so the
                 * file survives a save and comes back on load.
                 *
                 * Kept as JSON rather than a bare string so a line number and
                 * anything else can join it later without breaking readers. */
                sb.data = JSON.stringify({ file: order[fi] || '', line: s.line || 0 });
                tops.push(sb);
            }
            colX += COL_W;
        }

        return {
            mod: {
                blocks: { languageVersion: 0, blocks: tops },
                variables: variables
            }
        };
    }

    function estimateHeight(items) {
        var n = 0, i;
        for (i = 0; i < items.length; i++) { n += countBlocksIn(items[i]); }
        return 40 + n * 30;
    }

    function countBlocksIn(item) {
        var n = 1;
        function stmts(list) {
            var i, s;
            if (!list) { return; }
            for (i = 0; i < list.length; i++) {
                s = list[i];
                if (!s) { continue; }
                n++;
                if (s.k === 'if') {
                    var j;
                    for (j = 0; j < s.branches.length; j++) { stmts(s.branches[j].body); }
                    stmts(s.elseBody);
                } else if (s.k === 'for' || s.k === 'while') { stmts(s.body); }
            }
        }
        stmts(item.actions);
        return n;
    }

    function ruleToBlock(r, ctx) {
        ctx.params = [];
        var b = {
            type: 'ruleBlock',
            id: ctx.ids.next(),
            extraState: { isOngoingEvent: !!r.isOngoing },
            fields: {
                NAME: r.name === undefined || r.name === null ? '' : r.name,
                EVENTTYPE: r.event,
                OBJECTTYPE: r.objectType
            },
            inputs: {}
        };
        var c = conditionsToBlocks(r.conditions, ctx);
        if (c) { b.inputs.CONDITIONS = { block: c }; }
        var a = stmtsToBlocks(r.actions, ctx);
        if (a) { b.inputs.ACTIONS = { block: a }; }
        return b;
    }

    function subroutineToBlock(s, ctx) {
        ctx.params = s.params || [];
        var params = [], i;
        for (i = 0; i < ctx.params.length; i++) {
            params.push({ types: ctx.params[i].type || 'Any', name: ctx.params[i].name });
        }
        var b = {
            type: 'subroutineBlock',
            id: ctx.ids.next(),
            x: 0, y: 0,
            extraState: { subroutineName: s.name, parameters: params },
            fields: { SUBROUTINE_NAME: s.name },
            /* One line showing its name. See the note on the rule chain. */
            collapsed: true,
            inputs: {}
        };
        var c = conditionsToBlocks(s.conditions, ctx);
        if (c) { b.inputs.CONDITIONS = { block: c }; }
        var a = stmtsToBlocks(s.actions, ctx);
        if (a) { b.inputs.ACTIONS = { block: a }; }
        return b;
    }

    /* The slot a thrown away result is parked in. Never read, so one is enough
     * for the whole mod. */
    function discardRef(ctx) {
        /* DECLARES ITSELF THE FIRST TIME IT IS ASKED FOR.
         *
         * This slot is invented while blocks are being built, which is after
         * every pass that collects variables has run. So it was referenced and
         * never declared, and Blockly answered an unknown id by inventing a
         * variable with a generated single letter name.
         *
         * Declared on demand rather than always, because a Global costs one of
         * the 128 slots a mod is allowed and a project that never throws a
         * result away should not pay for it.
         */
        if (ctx && ctx.varIds && !has(ctx.varIds, '__discard')) {
            var vid = 'v' + ctx.variables.length.toString(36) + 'd';
            ctx.varIds['__discard'] = vid;
            ctx.variables.push({ name: 'unused', id: vid, type: 'Global' });
        }
        return { k: 'varRef', name: 'unused', scope: 'Global',
                 ident: '__discard', slot: 0, object: null };
    }

    function conditionsToBlocks(conds, ctx) {
        if (!conds || !conds.length) { return null; }
        var first = null, prev = null, i;
        for (i = 0; i < conds.length; i++) {
            var cb = { type: 'conditionBlock', id: ctx.ids.next(), inputs: {} };
            var e = exprToBlock(conds[i], ctx);
            if (e) { cb.inputs.CONDITION = { block: e }; }
            if (!first) { first = cb; } else { prev.next = { block: cb }; }
            prev = cb;
        }
        return first;
    }

    function stmtsToBlocks(stmts, ctx) {
        var first = null, prev = null, i;
        if (!stmts) { return null; }
        for (i = 0; i < stmts.length; i++) {
            var s = stmts[i];
            if (!s) { continue; }
            if (s.k === 'comment') {
                /* Nothing was dropped silently: the text rides on the previous
                 * block as a Blockly comment, or on the parent when there is
                 * no previous block. */
                if (prev) { attachComment(prev, s.text); }
                else { ctx.pendingComment = (ctx.pendingComment ? ctx.pendingComment + '\n' : '') + s.text; }
                continue;
            }
            var b = stmtToBlock(s, ctx);
            if (!b) { continue; }
            if (ctx.pendingComment) { attachComment(b, ctx.pendingComment); ctx.pendingComment = null; }
            if (!first) { first = b; } else { prev.next = { block: b }; }
            prev = b;
        }
        return first;
    }

    function attachComment(block, text) {
        if (!block.icons) { block.icons = {}; }
        var existing = block.icons.comment ? block.icons.comment.text + '\n' : '';
        block.icons.comment = {
            text: existing + text, pinned: false, height: 80, width: 320
        };
    }

    function stmtToBlock(s, ctx) {
        var b, i;
        switch (s.k) {
            case 'wait':
                b = { type: 'Wait', id: ctx.ids.next(), inputs: {} };
                setInput(b, 'VALUE-0', s.seconds, ctx);
                return b;
            case 'setVar':
                b = { type: 'SetVariable', id: ctx.ids.next(), inputs: {} };
                setInput(b, 'VALUE-0', s.ref, ctx);
                setInput(b, 'VALUE-1', s.value, ctx);
                return b;
            case 'setVarAt':
                b = { type: 'SetVariableAtIndex', id: ctx.ids.next(), inputs: {} };
                setInput(b, 'VALUE-0', s.ref, ctx);
                setInput(b, 'VALUE-1', s.index, ctx);
                setInput(b, 'VALUE-2', s.value, ctx);
                return b;
            case 'control':
                return { type: s.blockType || (s.word === 'continue' ? 'Continue' : 'Break'), id: ctx.ids.next() };
            case 'sub':
                return subInstanceBlock(s.name, s.args, ctx);
            case 'call':
                b = { type: s.fn, id: ctx.ids.next(), inputs: {} };
                for (i = 0; i < (s.args || []).length; i++) {
                    setInput(b, 'VALUE-' + i, s.args[i], ctx);
                }
                /* A VALUE BLOCK CANNOT STAND IN A LIST OF ACTIONS.
                 *
                 * `mod.SpawnObject(...)` on its own line throws its result away,
                 * which JavaScript allows and blocks do not: SpawnObject is a
                 * value block, it has no previous connection, and nothing can
                 * chain to it. The editor then refuses the whole workspace with
                 *   The block "SpawnObject" is missing a(n) previous
                 *
                 * Portal authors park the result in a variable and ignore it,
                 * and that is exactly what this does. One shared slot serves
                 * every discarded result, because nothing ever reads it.
                 */
                if (CATALOG[s.fn] && CATALOG[s.fn].kind === 'value') {
                    var keep = {
                        type: 'SetVariable', id: ctx.ids.next(), inputs: {}
                    };
                    setInput(keep, 'VALUE-0', discardRef(ctx), ctx);
                    keep.inputs['VALUE-1'] = { block: b };
                    return keep;
                }
                return b;
            case 'if':
                b = {
                    type: 'If', id: ctx.ids.next(),
                    extraState: {}, inputs: {}
                };
                if (s.branches.length > 1) { b.extraState.elseif = s.branches.length - 1; }
                if (s.elseBody) { b.extraState['else'] = 1; }
                for (i = 0; i < s.branches.length; i++) {
                    var condName = i === 0 ? 'VALUE-0' : ('IF' + i);
                    var doName = i === 0 ? 'DO' : ('DO' + i);
                    setInput(b, condName, s.branches[i].cond, ctx);
                    var body = stmtsToBlocks(s.branches[i].body, ctx);
                    if (body) { b.inputs[doName] = { block: body }; }
                }
                if (s.elseBody) {
                    var eb = stmtsToBlocks(s.elseBody, ctx);
                    if (eb) { b.inputs.ELSE = { block: eb }; }
                }
                return b;
            case 'for':
                b = { type: 'ForVariable', id: ctx.ids.next(), inputs: {} };
                setInput(b, 'VALUE-0', s.ref, ctx);
                setInput(b, 'VALUE-1', s.from, ctx);
                setInput(b, 'VALUE-2', s.to, ctx);
                setInput(b, 'VALUE-3', s.step, ctx);
                var fb = stmtsToBlocks(s.body, ctx);
                if (fb) { b.inputs.DO = { block: fb }; }
                return b;
            case 'while':
                b = { type: 'While', id: ctx.ids.next(), inputs: {} };
                setInput(b, 'VALUE-0', s.cond, ctx);
                var wb = stmtsToBlocks(s.body, ctx);
                if (wb) { b.inputs.DO = { block: wb }; }
                return b;
            default:
                return null;
        }
    }

    function subInstanceBlock(name, args, ctx) {
        var params = [], i;
        var target = null;
        for (i = 0; i < ctx.program.subroutines.length; i++) {
            if (ctx.program.subroutines[i].name === name ||
                ctx.program.subroutines[i].fnName === name) {
                target = ctx.program.subroutines[i];
                break;
            }
        }
        /* THE SOCKETS HAVE TO EXIST BEFORE ANYTHING IS PUT IN THEM.
         *
         * This block builds its PARAM sockets from extraState.parameters, and
         * that list was only filled when the called subroutine was found among
         * the ones we built. A call to anything else - a helper that was never
         * promoted to a subroutine, a name from a library, a function filtered
         * out earlier - declared ZERO parameters and then had its arguments
         * attached anyway, as PARAM-0 and up.
         *
         * Blockly refuses that, and it refuses the whole file with it:
         *
         *   The block "subroutineInstanceBlock" (id=...) is missing a(n) PARAM-0
         *
         * so one call to an unknown name emptied the entire canvas. The count
         * that matters is how many arguments are actually being passed, so the
         * sockets are declared from that, and the target's names and types are
         * used for the ones it does know about.
         */
        var argc = (args && args.length) || 0;
        var declared = target ? target.params.length : 0;
        for (i = 0; i < Math.max(argc, declared); i++) {
            var tp = target && target.params[i];
            params.push({
                types: (tp && tp.type) || 'Any',
                name: (tp && tp.name) || ('arg' + i)
            });
        }
        if (!target && argc && ctx.report) {
            /* Worth saying: the call is kept, but nothing checked the argument
             * types because there is no subroutine here to check them against. */
            warn(ctx.report, 'calls ' + name + '() with ' + argc + ' argument(s), but no ' +
                'subroutine of that name was built. The call is kept and its arguments ' +
                'are passed through unchecked.');
        }
        var b = {
            type: 'subroutineInstanceBlock', id: ctx.ids.next(),
            extraState: { subroutineName: target ? target.name : name, parameters: params },
            fields: { SUBROUTINE_NAME: target ? target.name : name }
        };
        if (argc) {
            b.inputs = {};
            for (i = 0; i < args.length; i++) { setInput(b, 'PARAM-' + i, args[i], ctx); }
        }
        return b;
    }

    function setInput(block, name, expr, ctx) {
        if (!expr) { return; }
        var b = exprToBlock(expr, ctx);
        if (!b) { return; }
        if (!block.inputs) { block.inputs = {}; }
        block.inputs[name] = { block: b };
    }

    /* AN UNREADABLE EXPRESSION IS AN EMPTY SOCKET, NOT A FALSE.
     *
     * Anything the converter could not read used to become the boolean false,
     * which is a real block of a real type, and Portal type checks its sockets.
     * One unreadable term inside a sum produced Subtract(x, false), the editor
     * refused the connection, and the whole workspace failed to load - so a
     * single unconvertible line anywhere emptied the entire canvas.
     *
     * Left empty, the socket is honest: it is a visible hole exactly where the
     * converter gave up, the NOT CONVERTED comment beside it says why, and
     * nothing else in the file is put at risk.
     */
    function exprToBlock(e, ctx) {
        if (!e) { return null; }
        if (e.k === 'gap') { return null; }
        var b, i;
        switch (e.k) {
            case 'num':
                return { type: 'Number', id: ctx.ids.next(), fields: { NUM: e.v } };
            case 'str':
                return { type: 'Text', id: ctx.ids.next(), fields: { TEXT: e.v } };
            case 'bool':
                return { type: 'Boolean', id: ctx.ids.next(), fields: { BOOL: e.v ? 'TRUE' : 'FALSE' } };
            case 'enum':
                return {
                    type: e.blockType || (e.enumName + 'Item'), id: ctx.ids.next(),
                    fields: { 'VALUE-0': e.enumName, 'VALUE-1': e.member }
                };
            case 'element':
                return { type: 'CurrentArrayElement', id: ctx.ids.next() };
            case 'event':
                return { type: e.blockType || ('Event' + capitalise(e.name.slice(5))), id: ctx.ids.next() };
            case 'arg':
                return {
                    type: 'subroutineArgumentBlock', id: ctx.ids.next(),
                    fields: { ARGUMENT_INDEX: String(e.index) }
                };
            case 'varRef':
                b = {
                    type: 'variableReferenceBlock', id: ctx.ids.next(),
                    extraState: { isObjectVar: !!e.object },
                    fields: { OBJECTTYPE: e.scope, VAR: { id: ctx.varIds[e.ident] || e.ident } }
                };
                if (e.object) {
                    b.inputs = {};
                    var ob = exprToBlock(e.object, ctx);
                    if (ob) { b.inputs.OBJECT = { block: ob }; }
                }
                return b;
            case 'getVar':
                b = { type: 'GetVariable', id: ctx.ids.next(), inputs: {} };
                setInput(b, 'VALUE-0', e.ref, ctx);
                return b;
            case 'lambda':
                return exprToBlock(e.body, ctx);
            case 'subCall':
                return subInstanceBlock(e.name, e.args, ctx);
            case 'call':
                b = { type: e.fn, id: ctx.ids.next(), inputs: {} };
                for (i = 0; i < (e.args || []).length; i++) {
                    setInput(b, 'VALUE-' + i, e.args[i], ctx);
                }
                if (!keysOf(b.inputs).length) { delete b.inputs; }
                return b;
            default:
                return null;
        }
    }

    /* --- public: tsToBlocks --------------------------------------------- */

    function tsToBlocks(sources, options) {
        options = options || {};
        var r = tsToIr(sources, options);
        var workspace = irToBlocks(r.program, options);
        r.report.counts.blocks = countWorkspaceBlocks(workspace);
        return { workspace: workspace, report: r.report, program: r.program };
    }

    function countWorkspaceBlocks(ws) {
        var n = 0;
        function walk(o) {
            var k;
            if (!o || typeof o !== 'object') { return; }
            if (o.type) { n++; }
            for (k in o) {
                if (!has(o, k) || k === 'type') { continue; }
                if (o[k] && typeof o[k] === 'object') { walk(o[k]); }
            }
        }
        walk(ws.mod.blocks.blocks);
        return n;
    }

    /* ================================================================== *
     * 5. Semantic summary and diffing
     * ================================================================== */

    /* Desugarings the site's own exporter applies. Normalised away so two
     * workspaces that mean the same thing compare equal. */
    function normaliseExpr(e) {
        if (!e) { return null; }
        var i, args;
        if (e.k === 'call') {
            args = [];
            for (i = 0; i < (e.args || []).length; i++) { args.push(normaliseExpr(e.args[i])); }

            if (e.fn === 'Not' && args.length === 1 && args[0] && args[0].k === 'call' &&
                args[0].fn === 'Equals') {
                return { k: 'call', fn: 'NotEqualTo', args: args[0].args };
            }
            if (e.fn === 'ArrayContains') {
                return normaliseExpr({
                    k: 'call', fn: 'IsTrueForAny',
                    args: [e.args[0], { k: 'lambda', body: { k: 'call', fn: 'Equals', args: [{ k: 'element' }, e.args[1]] } }]
                });
            }
            if (e.fn === 'IndexOfArrayValue') {
                return normaliseExpr({
                    k: 'call', fn: 'IndexOfFirstTrue',
                    args: [e.args[0], { k: 'lambda', body: { k: 'call', fn: 'Equals', args: [{ k: 'element' }, e.args[1]] } }]
                });
            }
            if (e.fn === 'RemoveFromArray') {
                return normaliseExpr({
                    k: 'call', fn: 'FilteredArray',
                    args: [e.args[0], { k: 'lambda', body: { k: 'call', fn: 'Not', args: [{ k: 'call', fn: 'Equals', args: [{ k: 'element' }, e.args[1]] }] } }]
                });
            }
            /* And / Or fold to a right leaning binary tree either way */
            if ((e.fn === 'And' || e.fn === 'Or') && args.length > 2) {
                var acc = args[0];
                for (i = 1; i < args.length; i++) { acc = { k: 'call', fn: e.fn, args: [acc, args[i]] }; }
                return acc;
            }
            return { k: 'call', fn: e.fn, args: args };
        }
        if (e.k === 'lambda') { return { k: 'lambda', body: normaliseExpr(e.body) }; }
        if (e.k === 'getVar') { return { k: 'getVar', ref: normaliseExpr(e.ref) }; }
        if (e.k === 'varRef') {
            return { k: 'varRef', name: sanitizeIdent(e.name), scope: e.scope, object: normaliseExpr(e.object) };
        }
        if (e.k === 'subCall') {
            args = [];
            for (i = 0; i < (e.args || []).length; i++) { args.push(normaliseExpr(e.args[i])); }
            return { k: 'subCall', name: e.name, args: args };
        }
        if (e.k === 'arg') { return { k: 'arg', index: e.index }; }
        if (e.k === 'event') { return { k: 'event', name: e.name }; }
        return { k: e.k, v: e.v, enumName: e.enumName, member: e.member };
    }

    function normaliseStmts(list) {
        var out = [], i, s, j, br;
        if (!list) { return out; }
        for (i = 0; i < list.length; i++) {
            s = list[i];
            if (!s || s.k === 'comment') { continue; }
            if (s.k === 'call') {
                var n = normaliseExpr({ k: 'call', fn: s.fn, args: s.args });
                out.push({ k: 'call', fn: n.fn, args: n.args });
            } else if (s.k === 'sub') {
                var a = [];
                for (j = 0; j < (s.args || []).length; j++) { a.push(normaliseExpr(s.args[j])); }
                out.push({ k: 'sub', name: s.name, args: a });
            } else if (s.k === 'control') {
                out.push({ k: 'control', word: s.word });
            } else if (s.k === 'wait') {
                out.push({ k: 'wait', seconds: normaliseExpr(s.seconds) });
            } else if (s.k === 'setVar') {
                out.push({ k: 'setVar', ref: normaliseExpr(s.ref), value: normaliseExpr(s.value) });
            } else if (s.k === 'setVarAt') {
                out.push({ k: 'setVarAt', ref: normaliseExpr(s.ref), index: normaliseExpr(s.index), value: normaliseExpr(s.value) });
            } else if (s.k === 'if') {
                var branches = [];
                for (j = 0; j < s.branches.length; j++) {
                    br = s.branches[j];
                    branches.push({ cond: normaliseExpr(br.cond), body: normaliseStmts(br.body) });
                }
                out.push({ k: 'if', branches: branches, elseBody: s.elseBody ? normaliseStmts(s.elseBody) : null });
            } else if (s.k === 'for') {
                out.push({
                    k: 'for', ref: normaliseExpr(s.ref), from: normaliseExpr(s.from),
                    to: normaliseExpr(s.to), step: normaliseExpr(s.step), body: normaliseStmts(s.body)
                });
            } else if (s.k === 'while') {
                out.push({ k: 'while', cond: normaliseExpr(s.cond), body: normaliseStmts(s.body) });
            }
        }
        return out;
    }

    /* A compact, comparable description of a program. */
    function summarize(input, opts) {
        opts = opts || {};
        var program;
        if (input && input.rules && input.variables && !input.mod) { program = input; }
        else { program = blocksToIr(input, newReport('summarize')); }

        var out = { rules: [], subroutines: [], variables: [] };
        var i, j;
        for (i = 0; i < program.rules.length; i++) {
            var r = program.rules[i];
            var conds = [];
            for (j = 0; j < r.conditions.length; j++) { conds.push(normaliseExpr(r.conditions[j])); }
            out.rules.push({
                name: String(r.name || ''),
                event: r.event,
                objectType: r.objectType,
                eventKey: r.eventKey,
                conditions: conds,
                actions: normaliseStmts(r.actions)
            });
        }
        for (i = 0; i < program.subroutines.length; i++) {
            var s = program.subroutines[i];
            var params = [];
            for (j = 0; j < s.params.length; j++) {
                params.push(opts.ignoreParamTypes ? { name: s.params[j].name } : s.params[j]);
            }
            out.subroutines.push({
                name: s.name,
                paramCount: s.params.length,
                params: params,
                actions: normaliseStmts(s.actions)
            });
        }
        for (i = 0; i < program.variables.length; i++) {
            out.variables.push({ name: program.variables[i].name, scope: program.variables[i].scope });
        }
        out.rules.sort(byRuleKey);
        out.subroutines.sort(byName);
        out.variables.sort(byName);
        return out;
    }

    function byName(a, b) { return a.name < b.name ? -1 : (a.name > b.name ? 1 : 0); }
    function byRuleKey(a, b) {
        var ka = a.eventKey + '|' + a.name, kb = b.eventKey + '|' + b.name;
        return ka < kb ? -1 : (ka > kb ? 1 : 0);
    }

    function diffSummaries(a, b, labelA, labelB) {
        var diffs = [];
        function cmpList(nameKey, la, lb, kind) {
            var mapA = {}, mapB = {}, i, k;
            for (i = 0; i < la.length; i++) { mapA[keyOf(la[i], kind)] = la[i]; }
            for (i = 0; i < lb.length; i++) { mapB[keyOf(lb[i], kind)] = lb[i]; }
            for (k in mapA) {
                if (!has(mapA, k)) { continue; }
                if (!has(mapB, k)) { diffs.push(kind + ' only in ' + labelA + ': ' + k); }
            }
            for (k in mapB) {
                if (!has(mapB, k)) { continue; }
                if (!has(mapA, k)) { diffs.push(kind + ' only in ' + labelB + ': ' + k); }
            }
            for (k in mapA) {
                if (!has(mapA, k) || !has(mapB, k)) { continue; }
                var sa = JSON.stringify(mapA[k]), sb = JSON.stringify(mapB[k]);
                if (sa !== sb) {
                    diffs.push(kind + ' body differs: ' + k + ' (' + firstDiff(sa, sb) + ')');
                }
            }
        }
        function keyOf(x, kind) {
            if (kind === 'rule') { return x.eventKey + ' / ' + x.name; }
            return x.name + (x.scope ? ' [' + x.scope + ']' : '');
        }
        function firstDiff(sa, sb) {
            var i, n = Math.min(sa.length, sb.length);
            for (i = 0; i < n; i++) { if (sa.charAt(i) !== sb.charAt(i)) { break; } }
            return 'at char ' + i + ': ' + labelA + '=' + sa.slice(i, i + 60) + ' | ' +
                labelB + '=' + sb.slice(i, i + 60);
        }
        cmpList('name', a.rules, b.rules, 'rule');
        cmpList('name', a.subroutines, b.subroutines, 'subroutine');
        cmpList('name', a.variables, b.variables, 'variable');
        return diffs;
    }

    /* PACK ON THE WAY OUT, NOT ON THE WAY IN.
     *
     * Import leaves every name alone, because someone opening a converted
     * project is reading it. Portal is not reading it: it counts variables,
     * allows 128 Global and 128 shared between Player and Team, and refuses
     * the upload past that. Those are two different jobs and they were being
     * done by one switch, so the project either read well or uploaded, never
     * both.
     *
     * This is the second job. The workspace on the canvas keeps its names; the
     * copy that goes to the site is folded down, quietest variables first, and
     * only as far as the budget actually forces. Nothing here touches the open
     * workspace: the packing runs on a program parsed out of it, and the
     * caller sends the returned copy.
     */
    function packForPortal(workspace, options) {
        options = options || {};
        var ceiling = options.ceiling || VARIABLE_CEILING;
        var report = newReport('packForPortal');
        var program = blocksToIr(workspace, report);

        function tally() {
            var by = { Global: 0, Player: 0, Team: 0 }, i;
            for (i = 0; i < program.variables.length; i++) {
                var sc = program.variables[i].scope || 'Global';
                by[sc] = (by[sc] || 0) + 1;
            }
            /* Player and Team are one budget, not two. Measured from the site
             * bundle: getObjectVariableCount filters type !== Global as a
             * single aggregate. */
            by.Object = by.Player + by.Team;
            return by;
        }

        var before = tally();
        packGlobals(program, report, ceiling);
        packObjectVars(program, report, ceiling);
        var after = tally();

        /* WHAT IS STILL IN THE WAY, BY NAME.
         *
         * A workspace that will not upload is worth more as a list of the
         * variables responsible than as a number. These are the ones packing
         * cannot fold: arrays, because appending an array to an array
         * concatenates it rather than nesting, and loop counters, because the
         * For block binds a variable and not an array element.
         */
        var blockers = [];
        if (after.Global > ceiling) {
            var k;
            for (k = 0; k < program.variables.length; k++) {
                var v = program.variables[k];
                if (v.scope !== 'Global') { continue; }
                if (/^(pack|frame|list|frameHeld|listHeld)[0-9]+$/.test(v.name)) { continue; }
                blockers.push(v.name);
            }
        }

        return {
            json: irToBlocks(program, options),
            report: report,
            before: before,
            after: after,
            ceiling: ceiling,
            fits: after.Global <= ceiling && after.Object <= ceiling,
            blockers: blockers
        };
    }

    /* ================================================================== *
     * Public surface
     * ================================================================== */

    function nativeImportProblems(report) {
        if (!report) return ['The converter returned no report.'];
        var problems = [], counts = report.counts || {};
        if ((report.unconvertible || []).length) problems.push('Some script constructs cannot be represented by native blocks.');
        if (counts.frameUnsafeForBlocks || counts.localsSharedAcrossInvocations)
            problems.push('Overlapping invocations can overwrite local variables while a handler waits.');
        if (counts.frameCyclic)
            problems.push('Recursive functions require per-invocation storage that this native conversion cannot preserve.');
        return problems;
    }

    return {
        VERSION: VERSION,
        setBlockChecks: setBlockChecks,
        setCatalog: setCatalog,
        setEvents: setEvents,
        setTypeScript: setTypeScript,

        blocksToTs: blocksToTs,
        tsToBlocks: tsToBlocks,
        nativeImportProblems: nativeImportProblems,
        packForPortal: packForPortal,

        blocksToIr: function (ws) { return blocksToIr(ws, newReport('ir')); },
        tsToIr: tsToIr,
        irToBlocks: irToBlocks,

        summarize: summarize,
        diffSummaries: diffSummaries,

        familyOf: familyOf,
        familyTitle: familyTitle,
        families: FAMILIES
    };
});
