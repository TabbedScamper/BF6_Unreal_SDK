// ============================================================================
// BF6 Script: the editor page.
//
// One Monaco editor, the TypeScript language service, and the beginner layer
// that is the whole point of the feature: an explanation beside every line, a
// shelf of annotated recipes, the community's answered questions, and a
// diagnose panel that reads the game's own log.
//
// WHAT TALKS TO WHAT
//   page  -> tool   window.ue.bf6script.call('<json>')  one JSON string, always
//   tool  -> page   window.BF6ScriptEditor.reply/push/event(<object>)
//
// The tool is the only way out of this page. There is no fetch and no XHR:
// loaded from file://, both are refused. Anything on disk arrives either as a
// <script> tag whose src the tool hands us, or in chunks across the bridge.
//
// NOTHING HERE SIGNS IN, and nothing here writes to the Portal site on its
// own. PUSH TO PORTAL puts text in the site's editor because the user pressed
// PUSH TO PORTAL; the Save on the site stays the user's press.
// ============================================================================

(function () {
    'use strict';

    var G = window.BF6ScriptGuide || { mod: {}, utils: {}, ts: {}, errors: {}, pitfalls: [], recipes: [], boilerplate: {}, loglines: [] };

    // ---- small helpers ------------------------------------------------------
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

    // ========================================================================
    // OUTPUT
    // ========================================================================
    // The log is closed at rest. Every line still lands in it; the newest one
    // also lands in the status strip, so nothing the tool says is ever silent,
    // and the MESSAGES button colours itself when a warning or an error went by
    // while the log was shut.
    var outEl = $('out');
    // One line into the host's log. Fire and forget: a diagnostic that can
    // fail loudly is worse than the thing it is diagnosing.
    function note(text) {
        try { call('note', { text: String(text) }); } catch (e) { /* never matters */ }
    }

    function say(text, cls) {
        var line = el('div', 'l' + (cls ? ' ' + cls : ''), text);
        outEl.appendChild(line);
        while (outEl.childNodes.length > 600) outEl.removeChild(outEl.firstChild);
        outEl.scrollTop = outEl.scrollHeight;
        var strip = $('lastMsg');
        if (strip) {
            strip.textContent = text;
            strip.style.color = cls === 'e' ? 'var(--bad)' : cls === 'w' ? 'var(--warn)' : 'var(--dim)';
        }
        var btn = $('btnMsgs');
        if (btn && !outEl.classList.contains('on') && (cls === 'e' || cls === 'w')) {
            btn.classList.add('unread');
            if (cls === 'e') btn.classList.add('bad');
        }
    }
    window.onerror = function (m, src, ln) { say('page error: ' + m + ' (' + src + ':' + ln + ')', 'e'); };

    // ========================================================================
    // THE BRIDGE
    // ========================================================================
    var seq = 1;
    // id -> { resolve, reject, timer } for calls the tool has not answered yet.
    // Nothing else may use this name: see buildPending, further down, for why.
    var pending = {};

    // A reply that never comes used to leave its promise unsettled forever, and
    // with it whatever button was disabled while it waited. Builds and pushes
    // can legitimately take minutes, so this is deliberately generous: it is
    // here to end an impossible wait, not to police a slow one.
    var CALL_TIMEOUT_MS = 5 * 60 * 1000;

    function settle(id, how, value) {
        var w = pending[id];
        if (!w) { return false; }      // already settled: a reply can only land once
        delete pending[id];
        if (w.timer) { clearTimeout(w.timer); }
        if (how === 'reject') { w.reject(value); } else { w.resolve(value); }
        return true;
    }

    function bridge() {
        return (window.ue && window.ue.bf6script) ? window.ue.bf6script : null;
    }

    // WHICH PROJECT A REQUEST IS ABOUT, SAID OUT LOUD.
    //
    // A write used to carry a rel path and nothing else, and the host resolved
    // that path against whatever project it had open at the moment it arrived.
    // Those are two different facts whenever the host has switched project and
    // the page does not know it yet, and the page can be left not knowing: an
    // open that the host CARRIED OUT, whose acknowledgment was lost, times out
    // as an unknown outcome. The page was still showing project A, so the next
    // save sent A's text, and the host wrote it into B under the same rel path.
    //
    // So every request that means "in the project I have open" now names that
    // project, by path and by the generation counter the host stamped on it
    // when it opened it. The host compares both and refuses a request that
    // names a project it no longer has open, which turns a silent write into
    // the wrong project into a refusal the user can read. `open`, `new`,
    // `projects` and `status` are deliberately not on the list: they are how a
    // project is chosen, so they cannot be about one already open.
    function stampProject(op, msg) {
        var scoped = {
            read: 1, write: 1, newfile: 1, files: 1, types: 1, pending: 1,
            install: 1, build: 1, run: 1, push: 1, pull: 1, sitesave: 1
        };
        if (!scoped[op] || !project || !project.path) return msg;
        msg.proj = project.path;
        if (project.gen !== undefined && project.gen !== null) msg.projGen = project.gen;
        return msg;
    }

    // Every call returns a promise. A call made before the binding exists is
    // held and replayed, because the page can finish loading before the tool
    // has bound its object to the window.
    var heldCalls = [];
    function call(op, args) {
        var id = seq++;
        var msg = { id: id, src: 'editor', op: op };
        if (args) for (var k in args) msg[k] = args[k];
        stampProject(op, msg);
        var p = new Promise(function (resolve, reject) {
            pending[id] = {
                resolve: resolve,
                reject: reject,
                sent: false,
                timer: setTimeout(function () {
                    /* WHAT A TIMEOUT IS ALLOWED TO CLAIM.
                     *
                     * Two very different situations end up here, and telling
                     * them apart is the whole point:
                     *
                     *   never sent  the binding was not up, so the message is
                     *               still sitting in heldCalls. Nothing reached
                     *               the tool, and it must be dropped from the
                     *               queue as well, or drainHeld would cheerfully
                     *               deliver an expired write minutes later,
                     *               after the user was told nothing had changed.
                     *
                     *   sent        the tool has it and did not answer. We do
                     *               NOT know whether it took effect, and saying
                     *               "nothing was changed" would be a guess
                     *               presented as a fact. */
                    var entry = pending[id];
                    var wasSent = entry && entry.sent;
                    if (!wasSent) {
                        for (var i = heldCalls.length - 1; i >= 0; i--) {
                            if (heldCalls[i].id === id) { heldCalls.splice(i, 1); }
                        }
                    }
                    settle(id, 'reject', {
                        ok: false, id: id, op: op, timedOut: true, sent: !!wasSent,
                        error: wasSent
                            ? 'the tool did not answer "' + op + '", so whether it took effect is '
                              + 'unknown. Check before trying it again.'
                            : 'the tool was never reached for "' + op + '". Nothing was changed.'
                    });
                }, CALL_TIMEOUT_MS)
            };
        });
        var b = bridge();
        if (b) { b.call(JSON.stringify(msg)); if (pending[id]) { pending[id].sent = true; } }
        else { heldCalls.push(msg); }
        return p;
    }
    function drainHeld() {
        var b = bridge();
        if (!b || !heldCalls.length) return;
        var list = heldCalls; heldCalls = [];
        for (var i = 0; i < list.length; i++) {
            /* Only messages still waiting for an answer. One that timed out was
             * removed from pending, and replaying it here is exactly the bug
             * this guard exists for: a write the user was told had not happened,
             * arriving later anyway. */
            var entry = pending[list[i].id];
            if (!entry) { continue; }
            b.call(JSON.stringify(list[i]));
            entry.sent = true;
        }
    }

    // ---- what the tool calls back into --------------------------------------
    var chunks = {};    // kind -> array of strings being assembled

    window.BF6ScriptEditor = {
        reply: function (r) {
            // settle() drops the entry and cancels the timer first, so a
            // duplicate or late reply after a timeout is ignored rather than
            // resolving a promise that already rejected.
            settle(r.id, r.ok === false ? 'reject' : 'resolve', r);
        },

        // A large payload, in order. { kind, seq, of, data } with data a plain
        // string; the last chunk completes it.
        push: function (p) {
            var buf = chunks[p.kind] || (chunks[p.kind] = []);
            buf[p.seq] = p.data;
            if (p.seq + 1 < p.of) return;
            var whole = buf.join('');
            delete chunks[p.kind];
            onPayload(p.kind, whole, p);
        },

        // Something happened in the tool that the page did not ask for.
        event: function (e) {
            if (e.kind === 'progress') { say(e.text, e.level); return; }
            if (e.kind === 'sitestate') { onSiteState(e); return; }
            if (e.kind === 'portallog') { onPortalLog(e.lines || []); return; }
            if (e.kind === 'selection') { setSelectedObject(e); return; }
            if (e.kind === 'built') { onBuilt(e); return; }
            if (e.kind === 'status') { applyStatus(e); return; }
            if (e.kind === 'pending') { onPending(e); return; }
            if (e.kind === 'session') { onSession(e); return; }
            if (e.kind === 'portalverdict') { onPortalVerdict(e); return; }
            say('event: ' + JSON.stringify(e), 'd');
        }
    };

    function onPayload(kind, whole, meta) {
        if (kind === 'workersrc') { onWorkerSrc(whole); return; }
        if (kind === 'lib') { addExtraLib(meta.path, whole); return; }
        if (kind === 'faq') { onFaq(whole); return; }
        if (kind === 'answers') { onAnswers(meta.path, whole); return; }
        if (kind === 'snippets') { onSnippets(whole); return; }
        if (kind === 'api') { onApi(whole); return; }
        // meta.PATH, not meta.rel. The host sends every chunk as
        // {kind, path, seq, of, data} - the same field every other payload here
        // reads - and this one line asked for a field that has never existed.
        // So every file read arrived with an undefined name: it was not
        // recognised as a quiet read (which closed the Text and Values panes on
        // top of the user), and then openFile threw on rel.replace and opened
        // nothing at all. That is why an imported script never appeared, why
        // clicking a file in the list did nothing, and why the Text tab bounced
        // straight back to whatever was already on screen.
        if (kind === 'file') { onFileText(meta.path, whole); return; }
        say('unknown payload: ' + kind, 'w');
    }

    // ========================================================================
    // MONACO
    //
    // The language service runs in a worker. A worker cannot be constructed
    // from a file:// URL, so the tool reads Monaco's worker sources off disk
    // and hands them here as one string; we wrap that in a Blob and construct
    // the worker from the blob URL instead. If THAT is refused as well, the
    // page falls back to index mode: completion, hover and signature help
    // built from the type definitions we already have, and no live
    // diagnostics. The status bar always says which of the two is running,
    // because "no red squiggles" must never be mistaken for "no errors".
    // ========================================================================
    var monacoReady = false;
    var editor = null;
    var mode = 'starting';           // starting | worker | index
    var workerSrcUrl = null;
    var pendingWorkerSrc = null;

    function setMode(m, why) {
        mode = m;
        var pill = $('mode');
        pill.textContent = m === 'worker' ? 'full intellisense'
            : m === 'index' ? 'index mode, no live errors' : m;
        pill.className = 'pill' + (m === 'worker' ? ' acc' : '');
        $('stMode').textContent = pill.textContent;
        if (why) say(why, m === 'worker' ? 'g' : 'w');
    }

    function onWorkerSrc(src) {
        try {
            var blob = new Blob([src], { type: 'text/javascript' });
            workerSrcUrl = URL.createObjectURL(blob);
        } catch (e) {
            say('could not build the worker blob: ' + e.message, 'w');
            workerSrcUrl = null;
        }
        bootMonaco();
    }

    // The probe. Monaco asks for a worker through this; if construction throws
    // we remember it and never ask again, which is what puts us in index mode.
    var workerBroken = false;
    self.MonacoEnvironment = {
        getWorker: function () {
            if (workerBroken || !workerSrcUrl) throw new Error('no worker');
            try {
                return new Worker(workerSrcUrl);
            } catch (e) {
                workerBroken = true;
                setMode('index', 'The language service worker could not start from a local page: ' + e.message + '. Completion and hover still work from the type definitions; live error checking does not, so use BUILD to find errors.');
                throw e;
            }
        }
    };

    function bootMonaco() {
        require.config({ paths: { vs: 'vendor/monaco/min/vs' } });
        require(['vs/editor/editor.main'], function () {
            monacoReady = true;
            defineTheme();
            configureTypeScript();
            makeEditor();
            registerProviders();
            setMode(workerSrcUrl ? 'worker' : 'index',
                workerSrcUrl ? 'Language service ready.'
                    : 'Running without the language service worker. Completion and hover come from the type definitions; BUILD reports the real errors.');
            afterEditorReady();
        });
    }

    function defineTheme() {
        monaco.editor.defineTheme('bf6', {
            base: 'vs-dark', inherit: true,
            rules: [
                { token: 'comment', foreground: '5d6a72', fontStyle: 'italic' },
                { token: 'keyword', foreground: 'ff581a' },
                { token: 'string', foreground: '9ecb7a' },
                { token: 'number', foreground: 'd8a657' },
                { token: 'type', foreground: 'aec0cc' }
            ],
            colors: {
                'editor.background': '#0d0f10',
                'editor.foreground': '#bfcad1',
                'editorLineNumber.foreground': '#3a4247',
                'editorLineNumber.activeForeground': '#ff581a',
                'editorCursor.foreground': '#ff581a',
                'editor.selectionBackground': '#28323a',
                'editor.lineHighlightBackground': '#14171a',
                'editorIndentGuide.background1': '#1e2225'
            }
        });
    }

    function configureTypeScript() {
        var ts = monaco.languages.typescript;
        ts.typescriptDefaults.setCompilerOptions({
            target: ts.ScriptTarget.ES2020,
            module: ts.ModuleKind.ES2020,
            moduleResolution: ts.ModuleResolutionKind.NodeJs,
            lib: ['es2020'],
            allowNonTsExtensions: true,
            allowImportingTsExtensions: true,
            noEmit: true,
            strict: true,
            noImplicitAny: true,
            strictNullChecks: true,
            resolveJsonModule: true,
            esModuleInterop: true,
            skipLibCheck: true
        });
        ts.typescriptDefaults.setDiagnosticsOptions({
            noSemanticValidation: false,
            noSyntaxValidation: false,
            // 2792 is "cannot find module, did you mean to set moduleResolution":
            // the project resolves through node_modules on disk, which the
            // in-page service cannot see, so the message is noise here.
            diagnosticCodesToIgnore: [2792]
        });
        ts.typescriptDefaults.setEagerModelSync(true);
    }

    var libCount = 0;
    var libDisposables = {};
    function addExtraLib(path, text) {
        if (!monacoReady) { (pendingLibs = pendingLibs || []).push([path, text]); return; }
        var uri = 'file:///node_modules/' + path.replace(/\\/g, '/');
        if (libDisposables[uri]) { libDisposables[uri].dispose(); }
        try {
            libDisposables[uri] = monaco.languages.typescript.typescriptDefaults.addExtraLib(text, uri);
            libCount++;
            $('stTypes').textContent = String(libCount);
            indexSymbols(path, text);
            // Timers, UI, Logger and the rest: their members come out of their
            // own declaration file, because nothing else on this page knows
            // them and inventing a member list would be worse than none.
            indexUtils(path, text);
            // The engine's own event handler shapes. TypeScript never checks an
            // exported handler against them, so CHECK MY SCRIPT does.
            if (/event-handler-signatures\.d\.ts$/.test(path)) parseEventSigs(text);
        } catch (e) {
            say('type file refused: ' + path + ' ' + e.message, 'w');
        }
    }
    var pendingLibs = null;

    // ========================================================================
    // THE SYMBOL INDEX
    //
    // Built from the same .d.ts text Monaco gets. Two jobs:
    //   - it is what the explanation gutter reads a symbol's JSDoc out of;
    //   - in index mode it is ALSO what completion, hover and signature help
    //     are served from, so a page with no worker is still usable.
    // ========================================================================
    var SYM = {};   // name -> { kind, sig, doc, file }

    var DECL = /(?:\/\*\*([\s\S]*?)\*\/\s*)?(?:export\s+)?(?:declare\s+)?(function|const|let|class|interface|enum|type)\s+([A-Za-z_$][\w$]*)\s*([^\n{;=]*)/g;

    function cleanDoc(block) {
        if (!block) return '';
        return block.split('\n')
            .map(function (l) { return l.replace(/^\s*\*ic?\s?/, '').replace(/^\s*\*\s?/, '').trim(); })
            .filter(function (l) { return l && l.charAt(0) !== '@'; })
            .join(' ')
            .trim();
    }

    function indexSymbols(path, text) {
        DECL.lastIndex = 0;
        var m, n = 0;
        while ((m = DECL.exec(text)) !== null && n < 6000) {
            n++;
            var name = m[3];
            if (SYM[name] && SYM[name].doc) continue;
            SYM[name] = {
                kind: m[2],
                sig: (m[3] + ' ' + (m[4] || '')).trim().replace(/\s+/g, ' ').slice(0, 300),
                doc: cleanDoc(m[1]),
                file: path
            };
        }
    }

    // ========================================================================
    // THE PARSED API
    //
    // api.json is the SDK's typings run through the real TypeScript compiler:
    // 415 commands, 74 events, 51 enums and their 1039 members, 40 opaque
    // types and the two constants, each with its true parameter names, types,
    // optionality, overloads and the doc comment the SDK shipped. 414 of the
    // 415 commands carry a real description.
    //
    // SYM, above, is a one-line regex over the same .d.ts text, capped at 6000
    // declarations, first-doc-wins. It cannot tell an optional parameter from
    // a required one, cannot see a per-parameter doc, cannot see an overload
    // and cannot see an enum's members at all. So everything a user types
    // against is served from here, and SYM stays only as the fallback for a
    // page where the api payload never arrived.
    //
    // It arrives over the bridge like the snippets do: there is no fetch on a
    // file:// page. See the 'api' op in BF6Script.cpp.
    // ========================================================================
    var API = null;
    var API_FN = {}, API_EV = {}, API_ENUM = {}, API_CONST = {}, API_TYPE = {};
    var apiAsked = false;

    function onApi(json) {
        var a;
        try { a = JSON.parse(json); }
        catch (e) {
            say('api.json is not valid JSON: ' + e.message + '. Completion falls back to the symbol index, which has no parameter names.', 'e');
            return;
        }
        API = a;
        (a.functions || []).forEach(function (f) { API_FN[f.name] = f; });
        (a.events || []).forEach(function (e) { API_EV[e.name] = e; });
        (a.enums || []).forEach(function (e) { API_ENUM[e.name] = e; });
        (a.constants || []).forEach(function (c) { API_CONST[c.name] = c; });
        (a.types || []).forEach(function (t) { API_TYPE[t.name] = t; });
        var members = (a.enums || []).reduce(function (n, e) { return n + (e.members || []).length; }, 0);
        say('Portal API ' + (a.version || '') + ' loaded: ' + (a.functions || []).length + ' commands, ' +
            (a.events || []).length + ' events, ' + (a.enums || []).length + ' lists of choices with ' +
            members + ' entries.', 'g');
        // FIND A COMMAND reads the same source, so it has to be redrawn.
        drawApiSearch();
    }

    function askApi() {
        if (apiAsked) return;
        apiAsked = true;
        call('api').catch(function (e) {
            say('The command descriptions did not load' + (e && e.why ? ': ' + e.why : '') +
                '. Completion still works from the type definitions, without parameter names.', 'w');
        });
    }

    // ------------------------------------------------------------------------
    // WHAT A BEGINNER NEEDS FIRST
    //
    // 415 names in one alphabetical list is not help, it is a wall: the first
    // screen of it is AbsoluteValue, Add, AddAttachmentToWeaponPackage. Two
    // pieces of evidence put the useful ones on top, and neither is taste.
    //
    //   1. guide.js writes a plain-English sentence for 186 of these commands.
    //      Somebody chose those on purpose, so they are the beginner-facing
    //      set, and a command that can explain itself in one sentence is worth
    //      more to a beginner than one that cannot.
    //
    //   2. USAGE_FN is every mod.* name that appears in the 231 TypeScript
    //      files of the shipped SDK projects, most used first. Message and
    //      GetObjId turn up on nearly a thousand lines each; the tail of the
    //      list turns up once. 226 of the 415 commands are never used at all,
    //      and they sort below everything that is.
    //
    // A third piece is live and beats both: a command the OPEN PROJECT already
    // calls is a command this user is working with today.
    // ------------------------------------------------------------------------
    var USAGE_FN = [
        'Message','GetObjId','GetSoldierState','CreateVector','IsPlayerValid','Wait','GetTeam',
        'YComponentOf','XComponentOf','ZComponentOf','DistanceBetween','SpawnObject',
        'GetObjectPosition','GetMatchTimeElapsed','GetSpatialObject','ValueInArray','CountOf',
        'HasEquipment','AllPlayers','FindUIWidgetWithName','RemoveEquipment','EnableVFX',
        'DisplayNotificationMessage','AIMoveToBehavior','Teleport','AddEquipment',
        'DeleteUIWidget','UnspawnObject','SetUIWidgetVisible','SetUITextLabel','AISetMoveSpeed',
        'AISetStance','PlaySound','EnableUIInputMode','AIEnableShooting',
        'EnableInputRestriction','Add','EnableInteractPoint','SetUITextColor','IsType',
        'AIValidatedMoveToBehavior','AIEnableTargeting','AddUIText','ForceSwitchInventory',
        'AISetTarget','GetUIRoot','GetVehicleState','GetObjectRotation',
        'AddAttachmentToWeaponPackage','SetPlayerMaxHealth','GetSpawner','SetTeam',
        'SetPlayerMovementSpeedMultiplier','MoveObject','IsUndefined','SpawnAIFromAISpawner',
        'IsInventorySlotActive','SetSpawnMode','Kill','DealDamage','SetUIWidgetPosition','Heal',
        'Equals','MoveObjectOverTime','SetAiInput','SetInventoryAmmo','SetObjectTransform',
        'SetUITextAlpha','IsValid','ForcePlayerToSeat','AddUIContainer','DeployPlayer',
        'UndeployPlayer','SetUITextSize','EnableAllInputRestrictions','SetCameraTypeForPlayer',
        'EnablePlayerDeploy','CreateTransform','PlayVO','AIBattlefieldBehavior','StopSound',
        'PlayMusic','ForcePlayerExitVehicle','SetInventoryMagazineAmmo','GetLootSpawner',
        'CreateNewWeaponPackage','EndGameMode','SpawnLoot','SetUIButtonColorBase',
        'AIDefendPositionBehavior','DirectionTowards','AIIdleBehavior','GetInventoryAmmo',
        'SetUIWidgetBgAlpha','GetVehicleSpawner','DeployAllPlayers','SetRedeployTime',
        'SetPlayerIncomingDamageFactor','RayCast','DotProduct','SetUIWidgetBgColor',
        'AddUIWeaponImage','Multiply','SetVehicleSpawnerAutoSpawn','AIForceFire','SpotTarget',
        'GetInventoryMagazineAmmo','GetAreaTrigger','Normalize','SetMusicParam',
        'GetVehicleFromPlayer','SetUIWidgetSize','AISetUnspawnOnDead','SkipManDown','GetSFX',
        'GetPlayer','SetWorldIconColor','AddUIGadgetImage','SetUnspawnDelayInSeconds',
        'SetSpectatingFiltersForAll','SetScoreboardPlayerValues','SendPortalLogToAdmin',
        'ForceRevive','SetVFXColor','UnspawnAllLoot','SetSoldierEffect','EnableAllPlayerDeploy',
        'EnableAreaTrigger','GetHQ','RemoveUIIcon','EnableGameModeObjective','GetInteractPoint',
        'GetWorldIcon','SetVehicleSpawnerVehicleType','ForceVehicleSpawnerSpawn',
        'CompareVehicleName','SetWorldIconImage','SetScoreboardColumnNames',
        'SetScoreboardColumnWidths','SetScoreboardSorting','EventDamageTypeCompare',
        'GetAllPlayersInVehicle','LoadMusic','GetUIWidgetName','SetWorldIconText',
        'UnspawnAllAIsFromAISpawner','AISetFocusPoint','EventDeathTypeCompare','AddUIIcon',
        'SetVariable','GlobalVariable','UndeployAllPlayers','GetPortalAverageFrameTime',
        'GetServerAverageFrameTime','EnableWorldIconText','SetWorldIconPosition','AddUIImage',
        'SetVehicleSpawnerRespawnTime','SetGameModeTimeLimit',
        'DisplayHighlightedWorldLogMessage','SetScoreboardType','IsCurrentMap',
        'GetMatchTimeRemaining','EnableHQ','GetMCOM','GetCapturePoint','GetVFX',
        'GetFixedCamera','SetVehicleSpawnerApplyDamageToAbandonVehicle',
        'SetVehicleSpawnerAbandonVehiclesOutOfCombatArea','GetVehicleSeatCount',
        'StopActiveMovementForObject','EnableWorldIconImage','SetUIImageAlpha','GetSquad',
        'IsSquadLeader','GetPlayerVehicleSeat','AllVehicles','AddUIButton',
        'SetCameraTypeForAll','SetFriendlyFire','GetUIWidgetPosition','GetVL7Cloud',
        'SetVL7CloudEffects','SetScoreboardHeader','SetGameModeTargetScore','SetGameModeScore',
        'SetUIButtonEnabled','SetVehicleMaxHealthMultiplier'
    ];
    var USAGE_ENUM = [
        'WeaponAttachments','UIAnchor','Weapons','InventorySlots','SoldierStateBool',
        'SoldierStateVector','UIBgFill','Gadgets','RestrictedInputs','UIDepth','MoveSpeed',
        'VoiceOverEvents2D','Stance','SoldierClass','Types','VehicleList','MusicEvents',
        'VehicleStateVector','SpawnModes','AiInput','Cameras','SoldierStateNumber',
        'MusicParams','VoiceOverFlags','SpotStatus','WorldIconImages','MusicPackages',
        'PlayerDeathTypes','SpectatingGroup','SoldierEffects','UIImageType','PlayerDamageTypes',
        'UIButtonEvent','ScreenEffects','ScoreboardType'
    ];
    var USAGE_EV = [
        'OnPlayerDeployed','OnGameModeStarted','OnPlayerDamaged','OnPlayerLeaveGame',
        'OnPlayerJoinGame','OnPlayerDied','OnVehicleSpawned','OnRayCastHit','OngoingPlayer',
        'OnVehicleDestroyed','OnPlayerInteract','OnSpawnerSpawned','OnRayCastMissed',
        'OnPlayerEnterAreaTrigger','OnMandown','OngoingGlobal','OnPlayerEarnedKill',
        'OnPlayerUndeploy','OnPlayerSwitchTeam','OnPlayerUIButtonEvent',
        'OnPlayerExitAreaTrigger','OnPlayerEnterVehicleSeat','OnPlayerExitVehicle','OnRevived',
        'OnTimeLimitReached','OnGameModeEnding','OnAIMoveToSucceeded','OnAIMoveToFailed',
        'OnPortalGadgetFireStart','OngoingVehicle','OngoingLootSpawner','OnPlayerEnterVehicle',
        'OnPortalGadgetLaserToggle','OnPortalGadgetAimStart','OnPlayerExitVehicleSeat',
        'OnPlayerEarnedKillAssist','OnAIMoveToRunning'
    ];

    var USAGE_AT = {};
    (function () {
        var i;
        for (i = 0; i < USAGE_FN.length; i++) USAGE_AT[USAGE_FN[i]] = i;
        for (i = 0; i < USAGE_ENUM.length; i++) USAGE_AT[USAGE_ENUM[i]] = i;
        for (i = 0; i < USAGE_EV.length; i++) USAGE_AT[USAGE_EV[i]] = i;
    })();

    // What the open project itself calls. Rebuilt at most every four seconds
    // because it walks every open model, and a completion list is built on
    // every keystroke.
    var ownNames = {}, ownNamesAt = 0;
    function projectNames() {
        var now = Date.now();
        if (now - ownNamesAt < 4000) return ownNames;
        ownNamesAt = now;
        ownNames = {};
        Object.keys(files).forEach(function (rel) {
            var text;
            try { text = files[rel].model.getValue(); } catch (e) { return; }
            var re = /\b(?:mod\.)?([A-Z][\w$]*)/g, m;
            while ((m = re.exec(text)) !== null) ownNames[m[1]] = 1;
        });
        return ownNames;
    }

    // Lower sorts first. Monaco compares sortText as plain strings, so the
    // rank is zero padded and the name is appended to break ties predictably.
    function rankOf(name, extra) {
        var r;
        if (USAGE_AT[name] !== undefined) r = 100 + USAGE_AT[name];
        else if (G.mod[name]) r = 400;
        else r = 700;
        if (G.mod[name]) r -= 50;                       // a name somebody wrote a sentence for
        if (projectNames()[name]) r -= 80;              // a name this project already uses
        if (extra) r += extra;
        if (r < 0) r = 0;
        if (r > 998) r = 998;
        return r;
    }
    function sortKey(rank, name) {
        return ('000' + rank).slice(-3) + String(name).toLowerCase();
    }
    function orderKey(i) { return ('0000' + i).slice(-4); }

    // ------------------------------------------------------------------------
    // ONE DESCRIPTION, BUILT THE SAME WAY EVERYWHERE
    //
    // The order is deliberate and it is the order a beginner reads in: the
    // plain sentence first if this tool has one, then the signature so the
    // shape is visible, then the SDK's own words. Nothing here is written
    // here: every line comes out of guide.js or out of api.json.
    // ------------------------------------------------------------------------
    // The same signature, laid out so it can be read. Nothing is dropped: the
    // truthful text for SpawnObject is fifteen hundred characters because its
    // first parameter is a union of twenty five map lists, and one line of that
    // is a horizontal scrollbar with the answer somewhere off the right edge.
    function prettySignature(entry, prefix) {
        var whole = (prefix || '') + entry.signature;
        if (whole.length <= 90 || !entry.params || !entry.params.length) return whole;
        var body = entry.params.map(function (p) {
            var type = String(p.type || '');
            if (type.split('|').length > 4) {
                type = type.split('|').map(function (t, i) {
                    return (i ? '\n          | ' : '') + t.trim();
                }).join('');
            }
            return '    ' + p.name + (p.optional ? '?' : '') + ': ' + type;
        }).join(',\n');
        return (prefix || '') + entry.name + '(\n' + body + '\n): ' + (entry.returns || 'void');
    }

    // One line for the list: the names of the parts and what comes back. The
    // types are in the panel beside it, and a completion row that is mostly a
    // union type tells a beginner nothing at all.
    function shortSignature(entry, prefix) {
        return (prefix || '') + entry.name + '(' +
            (entry.params || []).map(function (p) { return p.name + (p.optional ? '?' : ''); }).join(', ') +
            ')' + (entry.returns && entry.returns !== 'void' ? ': ' + entry.returns : '');
    }

    function describe(entry, prefix) {
        // Built once per command and kept on the entry. The bare-name list is
        // 473 items and Monaco rebuilds it on every keystroke; SpawnObject's
        // description alone is fifteen hundred characters, so building all of
        // them again for each letter typed is felt as typing lag.
        var key = '_md' + (prefix || '');
        if (entry[key]) return entry[key];

        var md = [];
        var g = G.mod[entry.name];
        if (g && g.say) md.push(g.say);
        if (entry.deprecated) md.push('**Do not use this.** ' + entry.deprecated);
        if (g && g.warn) md.push('**Careful.** ' + g.warn);
        if (entry.signature) md.push('```ts\n' + prettySignature(entry, prefix) + '\n```');
        if (entry.doc && (!g || g.say !== entry.doc)) md.push(entry.doc);

        var pd = (entry.params || []).filter(function (p) { return p.doc; });
        if (pd.length) {
            md.push('**What each part is**\n' + pd.map(function (p) {
                return '- `' + p.name + '` ' + p.doc;
            }).join('\n'));
        }
        if (entry.returnsDoc) md.push('**What it gives back.** ' + entry.returnsDoc);
        if (entry.overloads && entry.overloads.length) {
            md.push('**It can also be called as**\n' + entry.overloads.map(function (o) {
                return '- `' + (prefix || '') + o.signature + '`';
            }).join('\n'));
        }
        entry[key] = md.join('\n\n');
        return entry[key];
    }

    // The call itself, with the real parameter names left in as tab stops so
    // what lands in the file says what each slot is for. Optional parameters
    // are left off: the engine fills them in, and tabbing through six slots
    // you do not need is worse than typing the one you do.
    function callSnippet(entry) {
        var slots = [];
        var ps = entry.params || [];
        for (var i = 0; i < ps.length; i++) {
            if (ps[i].optional) break;
            slots.push('${' + (i + 1) + ':' + ps[i].name + '}');
        }
        return entry.name + '(' + slots.join(', ') + ')';
    }

    // ========================================================================
    // THE UTILS MODULES
    //
    // Timers, Logger, UI and the eighteen others are Mike De Luca's, not the
    // engine's, so api.json says nothing about them. Their own index.d.ts does,
    // and the tool already pushes every one of those in as a type library. So
    // the members are read out of that text as it arrives rather than being
    // listed here, which is the only way this can be right about a version of
    // the utils package we have never seen.
    //
    // Events is the exception and is handled beside it: its channels are not
    // declared one by one, they are a mapped type over the engine's event
    // list, so the names come from the `readonly X: typeof X` table the same
    // file carries, and from api.json's events if that table ever moves.
    // ========================================================================
    var UTILS = {};        // exported name -> { file, members: [{ name, sig, doc }] }
    var EVENT_CHANNELS = null;

    var UTILS_KEYWORD = /\b(?:class|interface|namespace|enum|type|function|const|let|var)\s+([A-Za-z_$][\w$]*)/;
    var UTILS_MEMBER = /^(?:(?:export|declare|public|static|readonly|abstract|async)\s+)*([A-Za-z_$][\w$]*)\s*[(<:?]/;

    function utilsMembers(text, open) {
        // Depth-1 members only. A class inside a namespace is reached as
        // UI.Root, and its own members are one dot further than this completes.
        //
        // Two things this has to get right or it lists nonsense. A declaration
        // written across lines (the utils package formats a long signature that
        // way) must be joined before it is read, or the parameter names on the
        // middle lines come out looking like members of their own. And a
        // declaration that opens a brace on the same line, which is every class
        // and every object type, has to be read at the brace, or it is lost.
        var out = [], i = open, doc = '', line = '';
        var depth = 0;      // braces: how deep inside the class or namespace
        var pdepth = 0;     // brackets: whether we are mid-signature

        function take(t, atBrace) {
            if (depth !== 1) return;
            if (!t || t.indexOf('private') !== -1) return;
            var m = UTILS_KEYWORD.exec(t) || (atBrace ? null : UTILS_MEMBER.exec(t));
            if (!m || m[1].charAt(0) === '_') return;
            out.push({ name: m[1], sig: t.replace(/\s+/g, ' ').slice(0, 240), doc: doc });
        }

        for (; i < text.length; i++) {
            var c = text[i];
            if (c === "'" || c === '"' || c === '`') {
                var q = c; i++;
                while (i < text.length && text[i] !== q) { if (text[i] === '\\') i++; i++; }
                continue;
            }
            if (c === '/' && text[i + 1] === '*') {
                var end = text.indexOf('*/', i);
                if (end === -1) break;
                if (depth === 1 && pdepth === 0) { doc = cleanDoc(text.slice(i + 2, end)); line = ''; }
                i = end + 1;
                continue;
            }
            if (c === '/' && text[i + 1] === '/') { while (i < text.length && text[i] !== '\n') i++; continue; }
            if (c === '(' || c === '[') { pdepth++; line += c; continue; }
            if (c === ')' || c === ']') { pdepth--; line += c; continue; }
            if (c === '{') {
                if (pdepth === 0) { take(line.trim(), true); doc = ''; line = ''; }
                else { line += c; }
                depth++;
                continue;
            }
            if (c === '}') {
                depth--;
                if (depth === 0) break;
                line = '';
                continue;
            }
            if (c === ';' || (c === '\n' && pdepth === 0)) {
                take(line.trim(), false);
                if (pdepth === 0) { doc = ''; line = ''; }
                continue;
            }
            if (c === '\n') { line += ' '; continue; }   // still inside a signature
            line += c;
        }

        var seen = {}, keep = [];
        out.forEach(function (x) { if (!seen[x.name]) { seen[x.name] = 1; keep.push(x); } });
        return keep;
    }

    function indexUtils(path, text) {
        if (!/^bf6-portal-utils\//.test(path)) return;
        if (!/\.d\.ts$/.test(path)) return;             // the .ts source repeats the same names

        var re = /(?:export\s+)?declare\s+(?:class|namespace)\s+([A-Za-z_$][\w$]*)\s*\{/g, m;
        while ((m = re.exec(text)) !== null) {
            var name = m[1];
            var members = utilsMembers(text, m.index + m[0].length - 1);
            if (!members.length) continue;
            var have = UTILS[name];
            // A class and a namespace of the same name are the shipped pattern
            // (Timers the class, Timers the namespace of its enums), so the two
            // member lists are joined rather than one replacing the other.
            if (have) {
                var seen = {};
                have.members.forEach(function (x) { seen[x.name] = 1; });
                members.forEach(function (x) { if (!seen[x.name]) have.members.push(x); });
            } else {
                UTILS[name] = { file: path, members: members };
            }
        }

        if (/bf6-portal-utils\/events\//.test(path)) {
            var chans = [], cre = /readonly\s+([A-Za-z_$][\w$]*)\s*:\s*typeof\s+\1\s*;/g, cm;
            while ((cm = cre.exec(text)) !== null) chans.push(cm[1]);
            if (chans.length) EVENT_CHANNELS = chans;
        }
    }

    // ========================================================================
    // THE PROJECT'S OWN STRING KEYS
    //
    // mod.stringkeys.<key> is the only way text reaches a player, and a key
    // that is not in strings.json is the community's most common save refusal.
    // The keys are the shape of the project's own JSON, so they are read off
    // disk rather than guessed: src/strings.json and one per feature folder.
    // ========================================================================
    var STRINGKEYS = [];    // [{ key, text, rel }]

    function flattenStrings(obj, prefix, rel, out) {
        Object.keys(obj || {}).forEach(function (k) {
            var v = obj[k];
            var key = prefix ? prefix + '.' + k : k;
            if (v && typeof v === 'object' && !Array.isArray(v)) flattenStrings(v, key, rel, out);
            else if (typeof v === 'string') out.push({ key: key, text: v, rel: rel });
        });
    }

    function refreshStringKeys() {
        var rels = Object.keys(lastFiles || {}).filter(function (rel) {
            return /(^|\/)strings\.json$/.test(rel) && rel.indexOf('dist/') !== 0;
        });
        if (!rels.length) { STRINGKEYS = []; return; }
        var out = [], left = rels.length;
        rels.forEach(function (rel) {
            readText(rel, true).then(function (text) {
                try { flattenStrings(JSON.parse(text), '', rel, out); }
                catch (e) { say(rel + ' is not valid JSON, so its keys are not offered: ' + e.message, 'w'); }
            }, function () { }).then(function () {
                if (--left === 0) {
                    out.sort(function (a, b) { return a.key < b.key ? -1 : a.key > b.key ? 1 : 0; });
                    STRINGKEYS = out;
                }
            });
        });
    }

    // ========================================================================
    // THE EDITOR AND ITS FILES
    // ========================================================================
    var files = {};        // rel -> { model, dirty }
    var activeRel = null;
    var project = null;    // { name, path, experience }

    // True while a project change is part way through. It is declared here,
    // beside the table it protects, because everything that writes a file or
    // disposes a model has to know: a rel path in `files` names a file in the
    // project being LEFT, and once the host has switched, that same path names
    // a different file in the new one. See openProject for the whole story.
    var switching = false;
    // Text that was still unsaved when its project was closed, kept per project
    // path. Nothing here is ever discarded; it goes back into the editor when
    // that project is opened again.
    var stranded = {};

    // WE ASKED THE HOST TO OPEN SOMETHING AND WE DO NOT KNOW WHAT IT DID.
    //
    // Set to { path, from } from the moment the open request leaves until the
    // host's answer, or a reconciliation, says which project it actually has
    // open. It is NOT the same thing as `switching`: `switching` was already
    // true for the failure cases too, and the failure case that matters here
    // is the one that used to look like a failure and was not. An open the
    // host CARRIED OUT whose reply was lost rejects as "outcome unknown", and
    // treating unknown as "it did not happen" is what let the next save write
    // the outgoing project's text into the incoming project.
    //
    // While this is set, nothing is written. See writesAreAllowed.
    var unresolvedOpen = null;

    // The single question every write path asks. A write sent while the host's
    // project is unknown can land in a project the user is not looking at, and
    // no message afterwards can put that back.
    function writesAreAllowed() { return !unresolvedOpen; }

    function makeEditor() {
        editor = monaco.editor.create($('editor'), {
            value: '',
            language: 'typescript',
            theme: 'bf6',
            automaticLayout: true,
            fontSize: 13,
            lineHeight: 20,
            minimap: { enabled: false },
            scrollBeyondLastLine: false,
            renderLineHighlight: 'line',
            tabSize: 4,
            insertSpaces: true,
            wordWrap: 'off',
            fixedOverflowWidgets: true,
            inlayHints: { enabled: 'on' },
            quickSuggestions: { other: true, comments: false, strings: false },
            suggestOnTriggerCharacters: true,
            parameterHints: { enabled: true }
        });

        // FIND, ON ITS OWN BUTTON AND ITS OWN KEY.
        //
        // Monaco has a find widget and Ctrl+F normally opens it, but the editor
        // is inside a browser inside Slate, and a host that takes the key first
        // leaves the page looking as though it has no search at all. Binding it
        // here means the page asks for the action itself, and the FIND button
        // beside it works even if the key never arrives.
        editor.addAction({
            id: 'bf6.find',
            label: 'Find in this file',
            keybindings: [monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyF],
            contextMenuGroupId: 'navigation',
            contextMenuOrder: 1.4,
            run: function (ed) { ed.getAction('actions.find').run(); }
        });

        // TIDY UP. A bundle comes back from Portal with every line flattened to
        // column zero - that is what the bundler emits, not something the tool
        // did - and 8,000 unindented lines are unreadable. This is Monaco's own
        // formatter, run on demand rather than automatically: it rewrites the
        // whole file, and doing that to somebody's code without being asked is
        // not the tool's decision to make.
        editor.addAction({
            id: 'bf6.tidy',
            label: 'Tidy up: put the indentation back',
            contextMenuGroupId: 'navigation',
            contextMenuOrder: 1.5,
            run: function (ed) {
                // The same re-indenter the open path uses. Monaco's own
                // formatter needs its TypeScript worker to answer for a file
                // marked @ts-nocheck, and on a bundle it does not.
                var m = ed.getModel();
                if (!m) { return; }
                var t0 = Date.now();
                m.setValue(reindentFlat(m.getValue()));
                say('indentation put back in ' + (Date.now() - t0) + 'ms', 'o');
            }
        });

        // RIGHT-CLICK A VALUE TO PUT IT ON THE QUICK LIST. In the editor's own
        // context menu, because that is where somebody looking at the number
        // already has their cursor.
        editor.addAction({
            id: 'bf6.quickValue.add',
            label: 'Add to Values (quick edit list)',
            contextMenuGroupId: 'navigation',
            contextMenuOrder: 1.6,
            run: function () { quickAddFromCursor(); }
        });

        // Right-click, then type the sentence. First in the group because it is
        // the one thing here that can answer a question nobody wrote a menu
        // item for.
        editor.addAction({
            id: 'bf6.assist.ask',
            label: 'Ask the AI about this...',
            contextMenuGroupId: 'navigation',
            contextMenuOrder: 1.1,
            keybindings: [monaco.KeyMod.CtrlCmd | monaco.KeyMod.Shift | monaco.KeyCode.KeyA],
            run: function () { openAsk(''); }
        });

        editor.onDidChangeModelContent(function () {
            if (activeRel && files[activeRel]) { files[activeRel].dirty = true; drawTabs(); }
            scheduleGutter();
            scheduleAutosave();
        });
        // The panel follows the caret, and a selection is a caret movement with
        // an edge on it: selecting a name is how you ask about that name.
        editor.onDidChangeCursorPosition(scheduleGutter);
        editor.onDidChangeCursorSelection(scheduleGutter);

        editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, saveActive);

        monaco.editor.onDidChangeMarkers(function () { renderDiagnostics(); });
    }

    // OPENING A FILE MUST NOT SILENTLY REPLACE WHAT IS IN IT.
    //
    // `text` is a copy from somewhere else: off disk, or off the Portal page.
    // If the model is already open with unsaved edits, writing that copy into
    // it destroys them. setValue is a single undo step, the dirty star clears,
    // and nothing on screen says what happened - so the loss is silent and
    // there is nothing on disk to go back to, because the edits were never
    // written. That is exactly how NEW FILE over an existing name used to lose
    // work: the read came back, its payload landed here, and the older bytes on
    // disk replaced what was on the screen.
    //
    // A dirty model is therefore only ever FOCUSED. The single exception is a
    // caller that has just rewritten the file itself and knows the disk copy is
    // the newer one; it says so with `replace`, and nothing else may.
    // ---- the template's own code, out of the way -------------------------
    //
    // A bundle is every module concatenated into one file, so an imported
    // experience opens on 8,000 lines of which a good slice is the template's
    // library - events, ui, raycast, sounds, timers, logging - that the author
    // never wrote and has no reason to read. Measured on a real 300 KB mod:
    // 1,304 of 7,938 lines, 16%, in six blocks scattered through the file.
    //
    // The bundler leaves "// --- SOURCE: <path> ---" before each module, so
    // which lines belong to whom is recorded exactly rather than guessed. Those
    // regions are HIDDEN, not deleted: the model is untouched, so line numbers,
    // search, and anything the file is later saved as are all unaffected. It is
    // the same bargain as the Text tab's checkbox - the template's own material
    // is out of the way until somebody says otherwise.
    var showTemplateCode = false;

    function bundleTemplateRanges(model) {
        var out = [];
        var lines = model.getLineCount();
        var text = model.getValue();
        if (text.indexOf('// --- BUNDLED TYPESCRIPT OUTPUT ---') !== 0) { return out; }
        var start = -1;
        for (var i = 1; i <= lines; i++) {
            var l = model.getLineContent(i);
            var m = /^\/\/ --- SOURCE:\s*(.+?)\s*---\s*$/.exec(l);
            if (!m) { continue; }
            // A new marker ends whatever was open.
            if (start > 0) { out.push({ startLineNumber: start, endLineNumber: i - 1 }); start = -1; }
            if (m[1].indexOf('node_modules') >= 0) { start = i; }
        }
        if (start > 0) { out.push({ startLineNumber: start, endLineNumber: lines }); }
        return out;
    }

    function applyTemplateHiding() {
        if (!editor || !editor.getModel()) { return; }
        var box = $('bundleHide');
        var ranges = showTemplateCode ? [] : bundleTemplateRanges(editor.getModel());
        try {
            editor.setHiddenAreas(ranges.map(function (r) {
                return new monaco.Range(r.startLineNumber, 1, r.endLineNumber, 1);
            }));
        } catch (e) { /* an editor build without hidden areas simply shows it all */ }

        // The control only appears on a file that HAS template code in it, so
        // it never sits there meaninglessly on a hand-written project.
        var any = bundleTemplateRanges(editor.getModel()).length;
        if (box) {
            box.hidden = any === 0;
            var lab = $('bundleHideCount');
            if (lab && any) {
                var n = 0;
                bundleTemplateRanges(editor.getModel()).forEach(function (r) {
                    n += (r.endLineNumber - r.startLineNumber + 1);
                });
                lab.textContent = n + ' lines of the template\'s own code';
            }
        }
    }

    // ---- search the whole project -----------------------------------------
    //
    // Ctrl+F searches the file you are looking at. This searches everything,
    // which is the question somebody actually has: where is this name used?
    // A block workspace has no such thing, and a bundle is one enormous file,
    // so following a name across a project is the one navigation aid that
    // matters most and was missing entirely.
    //
    // Reads each file once and remembers it for a few seconds, so typing does
    // not re-read nineteen files per keystroke.
    // Keyed by PROJECT as well as time, and every search carries a sequence
    // number. Without the first, searching B within the cache window showed A's
    // same-named file; without the second, a slow earlier query could land
    // after a newer one and overwrite the results being read.
    var findCache = { at: 0, project: '', files: {} };
    var findSeq = 0;

    function findInvalidate() { findCache = { at: 0, project: '', files: {} }; }

    function findAllRun(term) {
        var myseq = ++findSeq;
        var myProject = project && project.path;
        var panel = $('findPanel');
        if (!panel) { return; }
        var q = String(term || '').trim();
        if (q.length < 2) { panel.hidden = true; return; }

        var now = Date.now();
        // A cache from another project is not a cache, it is the bug.
        var fresh = (now - findCache.at) < 8000 && findCache.project === myProject;
        if (!fresh) { findCache = { at: now, project: myProject, files: {} }; }
        var rels = Object.keys(lastFiles || {}).filter(function (rel) {
            return /\.(ts|json)$/i.test(rel);
        });

        var reads = rels.map(function (rel) {
            // An open file's UNSAVED text is what the author is looking at, so
            // it is what gets searched. The cache is only for files on disk.
            if (files[rel]) { return Promise.resolve({ rel: rel, text: files[rel].model.getValue() }); }
            if (fresh && findCache.files[rel] !== undefined) {
                return Promise.resolve({ rel: rel, text: findCache.files[rel] });
            }
            return readText(rel, true).then(function (t) { return { rel: rel, text: t }; },
                                            function () { return { rel: rel, text: '' }; });
        });

        Promise.all(reads).then(function (all) {
            // An answer to a question nobody is asking any more.
            if (myseq !== findSeq) { return; }
            if (myProject !== (project && project.path)) { return; }
            all.forEach(function (f) { findCache.files[f.rel] = f.text; });
            findCache.at = findCache.at || now;

            var want = q.toLowerCase();
            var hits = [];
            all.forEach(function (f) {
                var lines = String(f.text).split('\n');
                for (var i = 0; i < lines.length && hits.length < 400; i++) {
                    var at = lines[i].toLowerCase().indexOf(want);
                    if (at < 0) { continue; }
                    hits.push({ rel: f.rel, line: i + 1, text: lines[i], at: at });
                }
            });
            drawFindResults(q, hits);
        });
    }

    function drawFindResults(q, hits) {
        var panel = $('findPanel'), host = $('findList');
        if (!panel || !host) { return; }
        panel.hidden = false;
        host.innerHTML = '';
        $('findCount').textContent = hits.length
            ? (hits.length + (hits.length === 400 ? '+' : '') + ' match'
               + (hits.length === 1 ? '' : 'es') + ' for "' + q + '"')
            : 'Nothing matches "' + q + '"';

        hits.forEach(function (h) {
            var b = el('button', 'findRow');
            var where = el('span', 'findWhere', h.rel + ':' + h.line);
            var line = document.createElement('span');
            line.className = 'findText';
            // The match itself picked out, so a long line is still readable.
            var pre = h.text.slice(Math.max(0, h.at - 40), h.at);
            var mid = h.text.substr(h.at, q.length);
            var post = h.text.substr(h.at + q.length, 60);
            line.appendChild(document.createTextNode((h.at > 40 ? '...' : '') + pre));
            var strong = document.createElement('b');
            strong.textContent = mid;
            line.appendChild(strong);
            line.appendChild(document.createTextNode(post));
            b.appendChild(where);
            b.appendChild(line);
            b.onclick = function () {
                showStrings(false);
                showQuick(false);
                var go = function () {
                    if (!editor || !editor.getModel()) { return; }
                    editor.revealLineInCenter(h.line);
                    editor.setPosition({ lineNumber: h.line, column: h.at + 1 });
                    editor.setSelection({
                        startLineNumber: h.line, startColumn: h.at + 1,
                        endLineNumber: h.line, endColumn: h.at + 1 + q.length
                    });
                    editor.focus();
                };
                if (activeRel === h.rel) { go(); return; }
                // The file has to arrive before the line can be shown.
                readText(h.rel).then(function (t) { openFile(h.rel, t); setTimeout(go, 60); });
            };
            host.appendChild(b);
        });
    }

    // PUTTING THE INDENTATION BACK.
    //
    // The bundler emits every line at column zero. Eight thousand lines of that
    // is unreadable, and Monaco's own formatter needs its TypeScript worker to
    // answer for a file it has been told not to check - which on a bundle it
    // does not. So the shape is restored here, from the only thing that
    // survives bundling: the brackets.
    //
    // It only ever changes LEADING WHITESPACE. Strings, template literals and
    // comments are tracked so a brace inside one cannot move anything, and no
    // character inside a line is touched - so this cannot alter what the file
    // means, only where it sits.
    function reindentFlat(text, unit) {
        var lines = String(text).split('\n');
        var pad = unit || '    ';
        var out = [];
        var depth = 0;
        var inBlock = false;      // a /* */ that spans lines
        var inTemplate = false;   // a ` ` that spans lines
        // A 'quoted string' continued onto the next line with a trailing
        // backslash. Its continuation is string CONTENT, and re-indenting it
        // rewrites what the program says: 'hello \' + newline + 'soldier' went
        // from "hello soldier" to "hello         soldier" just by opening the
        // file, with the model marked clean so nothing on screen said so.
        var inSq = false, inDq = false;
        var contIndent = 0;       // extra indent for a wrapped statement

        for (var i = 0; i < lines.length; i++) {
            var raw = lines[i];
            var line = raw.replace(/^[ \t]+/, '');

            // Inside a multi-line string or comment nothing is re-indented:
            // those characters are the file's content, not its layout.
            if (inTemplate || inBlock || inSq || inDq) {
                out.push(raw);
                var scanCarry = scanLine(raw, inBlock, inTemplate, inSq, inDq);
                inBlock = scanCarry.inBlock;
                inTemplate = scanCarry.inTemplate;
                inSq = scanCarry.inSq;
                inDq = scanCarry.inDq;
                continue;
            }
            if (line === '') { out.push(''); continue; }

            // A line that starts by closing something belongs one level out.
            var lead = /^[)\]}]/.test(line) ? 1 : 0;
            // case/default sit one level in from their switch.
            var isCase = /^(case\b|default\b)/.test(line);
            var here = Math.max(0, depth - lead - (isCase ? 1 : 0));
            out.push(repeat(pad, here + contIndent) + line);

            var r = scanLine(line, false, false, false, false);
            depth = Math.max(0, depth + r.delta);
            inBlock = r.inBlock;
            inTemplate = r.inTemplate;
            inSq = r.inSq;
            inDq = r.inDq;
            // A statement continued onto the next line is nudged in once, so a
            // wrapped argument list does not read as a new statement.
            contIndent = (r.open || /[,+\-*/&|?:=]$/.test(line.replace(/\s+$/, ''))) && !r.delta ? 1 : 0;
        }
        return out.join('\n');
    }

    function repeat(s, n) { var o = ''; for (var i = 0; i < n; i++) { o += s; } return o; }

    // One line's effect on nesting, ignoring anything inside a string or a
    // comment. Returns the depth change and whether a block comment or template
    // literal is still open at the end of it.
    // `inSq`/`inDq` say a quoted string was still open when the last line
    // ended, which in JavaScript means it was continued with a trailing
    // backslash and the next line's leading spaces are STRING CONTENT.
    function scanLine(line, inBlock, inTemplate, inSq, inDq) {
        var delta = 0, open = false;
        var i = 0, n = line.length;
        var sq = !!inSq, dq = !!inDq;
        while (i < n) {
            var c = line[i], c2 = line[i + 1];
            if (inBlock) {
                if (c === '*' && c2 === '/') { inBlock = false; i += 2; continue; }
                i++; continue;
            }
            if (inTemplate) {
                if (c === '\\') { i += 2; continue; }
                if (c === '`') { inTemplate = false; }
                i++; continue;
            }
            if (sq || dq) {
                if (c === '\\') { i += 2; continue; }
                if ((sq && c === "'") || (dq && c === '"')) { sq = false; dq = false; }
                i++; continue;
            }
            if (c === '/' && c2 === '/') { break; }            // rest is a comment
            if (c === '/' && c2 === '*') { inBlock = true; i += 2; continue; }
            if (c === "'") { sq = true; i++; continue; }
            if (c === '"') { dq = true; i++; continue; }
            if (c === '`') { inTemplate = true; i++; continue; }
            if (c === '{' || c === '[' || c === '(') { delta++; open = true; i++; continue; }
            if (c === '}' || c === ']' || c === ')') { delta--; i++; continue; }
            i++;
        }
        return { delta: delta, inBlock: inBlock, inTemplate: inTemplate, open: open,
                 inSq: sq, inDq: dq };
    }

    // Is this file flat enough to be worth re-indenting? A bundle is; anything
    // somebody has written themselves is left exactly as they wrote it.
    function looksFlat(text) {
        var lines = String(text).split('\n');
        if (lines.length < 40) { return false; }
        var indented = 0, code = 0;
        for (var i = 0; i < lines.length && i < 800; i++) {
            var l = lines[i];
            if (!l.trim()) { continue; }
            code++;
            if (/^[ \t]/.test(l)) { indented++; }
        }
        // Real source indents most of its body; a bundle indents almost none.
        return code > 30 && (indented / code) < 0.10;
    }

    // CLOSING A TAB IS NOT THROWING WORK AWAY.
    //
    // A file with unsaved edits is written before it goes, because the tab is
    // the only place those edits existed. bQuiet is for the bulk actions, which
    // redraw once at the end rather than after every file.
    // Close a list of tabs in order, waiting for each save. Returns how many
    // would not close, and says so once rather than once per file.
    function closeMany(rels) {
        var kept = [];
        return rels.reduce(function (chain, rel) {
            return chain.then(function () {
                return closeFile(rel, true).then(function (bClosed) {
                    if (!bClosed) { kept.push(rel); }
                });
            });
        }, Promise.resolve()).then(function () {
            if (kept.length) {
                say(kept.length + ' file(s) could not be saved and are still open: '
                    + kept.join(', '), 'e');
            }
            return kept;
        });
    }

    // CLOSING IS A SAVE FIRST AND A CLOSE SECOND.
    //
    // This used to fire the write and dispose the model on the next line, so
    // the only copy of the text was destroyed while the save was still in
    // flight. A refused write - a locked file, a project that had moved on, a
    // reply that never came - produced an error message about text that no
    // longer existed anywhere. Close all did it to every tab at once.
    //
    // Now the tab goes only after the write has been acknowledged. If it fails
    // the tab stays, still dirty, with everything in it.
    function closeFile(rel, bQuiet) {
        var f = files[rel];
        if (!f) { return Promise.resolve(false); }

        var finish = function () {
            var still = files[rel];
            if (!still) { return true; }
            try { still.model.dispose(); } catch (e) { /* already gone */ }
            delete files[rel];
            if (activeRel === rel) {
                activeRel = '';
                var next = Object.keys(files)[0];
                if (next) { openFile(next); }
                else if (editor) { editor.setModel(null); }
            }
            if (!bQuiet) { drawTabs(); }
            return true;
        };

        if (!f.dirty) { return Promise.resolve(finish()); }

        var text = f.model.getValue();
        return call('write', { rel: rel, text: text }).then(function () {
            // Anything typed between the send and the acknowledgment is newer
            // than what was saved, so the tab stays rather than taking those
            // keystrokes with it.
            var still = files[rel];
            if (still && still.model.getValue() !== text) {
                still.dirty = true;
                if (!bQuiet) { drawTabs(); }
                say(rel + ' changed while it was being saved, so it is still open. Close it again.', 'w');
                return false;
            }
            return finish();
        }, function (e) {
            say('could not save ' + rel + ', so it has been left open with your changes in it: '
                + (e && (e.why || e.error)), 'e');
            if (!bQuiet) { drawTabs(); }
            return false;
        });
    }

    function openFile(rel, text, replace) {
        // A BUNDLE ARRIVES FLAT. Every line at column zero is how the bundler
        // emits it, and 8,000 of those is not something anybody can read. The
        // shape is put back as it opens rather than left for somebody to find a
        // menu item for - and only leading whitespace moves, so the file still
        // means exactly what it did.
        if (text != null && looksFlat(text)) {
            var t0 = Date.now();
            text = reindentFlat(text);
            note('reindented ' + rel + ' in ' + (Date.now() - t0) + 'ms');
        }
        var f = files[rel];
        if (!f) {
            var lang = /\.json$/i.test(rel) ? 'json' : /\.(js|mjs)$/i.test(rel) ? 'javascript' : 'typescript';
            var uri = monaco.Uri.parse('file:///project/' + rel.replace(/\\/g, '/'));
            var model = monaco.editor.getModel(uri) || monaco.editor.createModel(text || '', lang, uri);
            f = files[rel] = { model: model, dirty: false };
        } else if (text != null && (replace || !f.dirty || f.model.getValue() === text)) {
            f.model.setValue(text);
            f.dirty = false;
        } else if (text != null) {
            say(rel + ' has unsaved changes here, so the copy on disk was not loaded over them. What you can see is your own text.', 'w');
        }
        note('openFile ' + rel);
        activeRel = rel;
        editor.setModel(f.model);
        // Done after the model is in, because hidden areas belong to the
        // editor's view of a model rather than to the model.
        applyTemplateHiding();
        drawTabs();
        scheduleGutter();
    }

    function onFileText(rel, text) {
        note('file arrived: ' + rel + ' (' + (text ? text.length : 0) + ' chars)');
        if (!rel) {
            // Loud on purpose: a nameless payload used to throw inside openFile
            // and leave the screen unchanged, which looks exactly like nothing
            // was sent at all.
            say('a file arrived with no name, so it could not be opened.', 'e');
            return;
        }
        // A file arriving to be SHOWN has to be shown: the same hidden-editor
        // trap as the list, for reads the tool starts itself.
        if (!quietReads[rel] && (isStringsOpen() || isQuickOpen())) {
            showStrings(false);
            showQuick(false);
        }
        // Somebody asked for this file's text rather than for it to be opened.
        var waiting = window.__bf6PendingReads && window.__bf6PendingReads[rel];
        // A QUIET read is one nothing on screen asked for: the string keys are
        // collected from every strings.json in the project, and opening a tab
        // per feature folder because completion needed a key list would be the
        // tool rearranging the window behind the user's back.
        var quiet = quietReads[rel];
        delete quietReads[rel];
        // A RELOAD read is one the tool asked for because the tool itself had
        // just rewritten the file, so the disk copy really is newer than the
        // open model and is allowed past the guard in openFile. It is consumed
        // here: only the read that was asked for gets the permission.
        var replace = !!reloadReads[rel];
        delete reloadReads[rel];
        if (!quiet) openFile(rel, text, replace);
        if (waiting) { delete window.__bf6PendingReads[rel]; waiting(text); }
    }
    var quietReads = {};
    var reloadReads = {};

    // ---- the text a player sees ------------------------------------------
    //
    // src/strings.json edited as a list of lines rather than as JSON. The file
    // is a TREE (mod.stringkeys.a.b.c), so it is flattened to one row per leaf
    // and folded back on save; nothing else in the project has to know.
    //
    // WHAT IS HIDDEN AND WHY. The template ships its own strings - the debug
    // tool's buttons, an example notification, eighteen map names - and none of
    // them are the user's. A list that opens on those is a list people scroll
    // past, so they are behind a checkbox rather than in the way. The debug
    // tool's own strings live in src/debug-tool/strings.json and are not read
    // here at all.
    var STRINGS_REL = 'src/strings.json';
    var strTree = null;        // the parsed file, or null before it is read
    var strShowTemplate = false;
    var strDirty = false;

    // The template's own branches, by top-level key.
    var STR_TEMPLATE_KEYS = { template: 1, debugTool: 1 };

    function strFlatten(node, path, out) {
        Object.keys(node || {}).forEach(function (k) {
            var v = node[k];
            var next = path ? path + '.' + k : k;
            if (v && typeof v === 'object' && !Array.isArray(v)) { strFlatten(v, next, out); }
            else { out.push({ path: next, text: String(v == null ? '' : v) }); }
        });
        return out;
    }

    function strSetAt(tree, path, value) {
        var parts = path.split('.');
        var node = tree;
        for (var i = 0; i < parts.length - 1; i++) {
            if (!node[parts[i]] || typeof node[parts[i]] !== 'object') { node[parts[i]] = {}; }
            node = node[parts[i]];
        }
        node[parts[parts.length - 1]] = value;
    }

    function strDeleteAt(tree, path) {
        var parts = path.split('.');
        var node = tree, chain = [];
        for (var i = 0; i < parts.length - 1; i++) {
            if (!node[parts[i]]) return;
            chain.push([node, parts[i]]);
            node = node[parts[i]];
        }
        delete node[parts[parts.length - 1]];
        // Prune the branches that only existed to hold it, so deleting the last
        // line under a heading does not leave an empty heading behind forever.
        for (var j = chain.length - 1; j >= 0; j--) {
            var parent = chain[j][0], key = chain[j][1];
            if (parent[key] && Object.keys(parent[key]).length === 0) { delete parent[key]; }
        }
    }

    function strIsTemplate(path) {
        return !!STR_TEMPLATE_KEYS[path.split('.')[0]];
    }

    // The one line of code that puts this text on screen. Copied whole, because
    // half of it is no use to somebody who does not write code.
    function strCodeFor(path) {
        return 'mod.Message(mod.stringkeys.' + path + ')';
    }

    // WHICH PROJECT THIS TEXT BELONGS TO.
    //
    // The list is loaded asynchronously and saved on a timer, so both can be in
    // flight while somebody switches project - and a timer that fired afterwards
    // would write project A's strings into project B, under B's name. Every load
    // and every save carries the project it was for and is dropped when that is
    // no longer the project open.
    var strProject = '';

    function strSaveSoon() {
        strDirty = true;
        if (strSaveSoon.t) clearTimeout(strSaveSoon.t);
        var forProject = strProject;
        strSaveSoon.t = setTimeout(function () {
            if (!strTree) return;
            if (forProject !== (project && project.path)) {
                say('the project changed, so that text was not saved into the new one.', 'w');
                return;
            }
            call('write', { rel: STRINGS_REL, text: JSON.stringify(strTree, null, 4) + '\n' })
                .then(function () {
                    if (forProject !== (project && project.path)) return;
                    strDirty = false; say('text saved', 'o');
                },
                      function (e) { say('could not save the text: ' + (e && (e.why || e.error)), 'e'); });
        }, 600);
    }

    // Anything unsaved, written now. Called before a build or a project switch,
    // so an edit made a moment ago is not left behind by a 600 ms timer.
    function strFlush() {
        if (!strDirty || !strTree) return Promise.resolve();
        if (strSaveSoon.t) { clearTimeout(strSaveSoon.t); strSaveSoon.t = null; }
        if (strProject !== (project && project.path)) return Promise.resolve();
        return call('write', { rel: STRINGS_REL, text: JSON.stringify(strTree, null, 4) + '\n' })
            .then(function () { strDirty = false; }, function () {});
    }

    function drawStrings() {
        var host = $('strList');
        if (!host) return;
        host.innerHTML = '';
        var rows = strTree ? strFlatten(strTree, '', []) : [];
        var shown = rows.filter(function (r) { return strShowTemplate || !strIsTemplate(r.path); });

        var empty = $('strEmpty');
        if (empty) empty.hidden = shown.length > 0;

        shown.forEach(function (r) {
            var row = el('div', 'strRow' + (strIsTemplate(r.path) ? ' tmpl' : ''));

            var inp = document.createElement('input');
            inp.type = 'text';
            inp.className = 'strText';
            inp.value = r.text;
            inp.oninput = function () { strSetAt(strTree, r.path, inp.value); strSaveSoon(); };
            row.appendChild(inp);

            var key = el('div', 'strKey');
            var last = r.path.split('.').pop();
            key.innerHTML = '';
            var b = document.createElement('b'); b.textContent = last;
            key.appendChild(b);
            // A placeholder is the one part of a line that behaves like code.
            if (r.text.indexOf('{}') >= 0) {
                var w = el('span', 'strHasArg', '{} takes a value');
                key.appendChild(w);
            }
            key.title = 'mod.stringkeys.' + r.path;
            row.appendChild(key);

            var btns = el('div', 'strBtns');
            var use = el('button', null, 'Use');
            use.title = strCodeFor(r.path);
            use.onclick = function () {
                var code = strCodeFor(r.path);
                try { navigator.clipboard.writeText(code); say('copied: ' + code, 'o'); }
                catch (e) { say(code, 'd'); }
            };
            btns.appendChild(use);

            var del = el('button', 'strDel', 'Delete');
            del.onclick = function () {
                strDeleteAt(strTree, r.path);
                strSaveSoon();
                drawStrings();
            };
            btns.appendChild(del);
            row.appendChild(btns);

            host.appendChild(row);
        });
    }

    function strAdd() {
        // A null tree means the file could not be read. Creating one here would
        // quietly replace a malformed file with a new one holding a single
        // line, which is the loss this refuses to take part in.
        if (strTree === null) {
            say('the text file could not be read, so nothing can be added to it yet.', 'e');
            return;
        }
        if (!strTree) strTree = {};
        // A name of its own so two additions never collide, and short enough to
        // read in code. The user renames nothing: the text is what they edit.
        var n = 1;
        while (strTree['text' + n] !== undefined) n++;
        var path = 'text' + n;
        strSetAt(strTree, path, 'New text');
        strSaveSoon();
        drawStrings();
        // Straight into the new row, because adding one is always followed by
        // typing what it says.
        var inputs = $('strList').querySelectorAll('input.strText');
        if (inputs.length) {
            var last = inputs[inputs.length - 1];
            last.focus();
            last.select();
        }
    }

    function loadStrings() {
        if (!project) { strTree = null; strProject = ''; drawStrings(); return Promise.resolve(); }
        var forProject = project.path;
        strProject = forProject;
        // Nothing is editable until this project's own read lands, so a stale
        // tree from the last project cannot be shown or written.
        strTree = null;
        return readText(STRINGS_REL, true).then(function (text) {
            if (forProject !== (project && project.path)) return;
            try {
                strTree = JSON.parse(text || '{}');
            }
            catch (e) {
                // AN UNREADABLE FILE IS NOT AN EMPTY ONE. Turning a parse
                // failure into {} made the list editable, and the first edit
                // then saved that empty object over a file that was merely
                // malformed: somebody's entire text gone because of a stray
                // comma. Refusing to edit it is the only safe answer.
                strTree = null;
                say('src/strings.json could not be read as JSON, so it is not being edited here. '
                    + 'Open it in the editor and fix it; nothing is written until you do.', 'e');
            }
            drawStrings();
        }, function () {
            if (forProject !== (project && project.path)) return;
            // No strings file yet is a normal state for a new project, and an
            // empty tree is the honest answer to that.
            strTree = {};
            drawStrings();
        });
    }

    function showStrings(on) {
        var pane = $('stringsPane'), split = $('split');
        note('showStrings(' + on + ') pane=' + (pane ? 'yes' : 'MISSING'));
        if (!pane) return;
        pane.hidden = !on;
        // Only the editor is swapped out. The tab strip stays: it is what you
        // click to come back, and hiding it stranded you in the text list.
        if (split) split.style.display = (on || isQuickOpen()) ? 'none' : '';
        if (on) loadStrings();
        drawTabs();
    }

    // A right-click menu for one row. Same look as the toolbar's menu, built
    // from whatever the caller offers rather than from fixed markup.
    function openCtx(ev, items) {
        ev.preventDefault();
        ev.stopPropagation();
        var m = $('ctxMenu');
        if (!m) return;
        m.innerHTML = '';
        items.forEach(function (it) {
            var b = el('button', null, it.label);
            b.onclick = function () { closeCtx(); it.run(); };
            m.appendChild(b);
        });
        m.hidden = false;
        // Positioned after it is shown, so its real size is known and a menu
        // opened near an edge is nudged back on screen instead of clipped.
        var w = m.offsetWidth, h = m.offsetHeight;
        var x = Math.min(ev.clientX, window.innerWidth - w - 6);
        var y = Math.min(ev.clientY, window.innerHeight - h - 6);
        m.style.left = Math.max(4, Math.round(x)) + 'px';
        m.style.top = Math.max(4, Math.round(y)) + 'px';
    }
    function closeCtx() { var m = $('ctxMenu'); if (m) { m.hidden = true; } }
    document.addEventListener('mousedown', function (ev) {
        var m = $('ctxMenu');
        if (!m || m.hidden) return;
        if (m.contains(ev.target)) return;
        closeCtx();
    }, true);
    document.addEventListener('keydown', function (ev) {
        if (ev.key === 'Escape') closeCtx();
    }, true);

    // ---- quick values ------------------------------------------------------
    //
    // A short list of the settings somebody actually changes - the starting
    // cash, whether friendly fire is on, how loud the music is - pulled out of
    // wherever they happen to live in the script so they can be found in one
    // place instead of hunted for.
    //
    // THE VALUE STAYS IN THE CODE. Nothing is moved into a settings file and
    // nothing is generated: the script is exactly what the author wrote, and
    // this list holds a REFERENCE to a declaration in it plus a friendly name.
    //
    // ANCHORED BY NAME, NOT BY LINE. A line number is wrong the moment anything
    // above it changes, and a quick-value that silently starts editing the
    // wrong line is worse than no feature. So a value is only added when it has
    // a name to anchor to - const/let/var, or an object property - and the
    // whole feature is that name plus the file it lives in.
    //
    // The friendly label is stored HERE and never in the script, so renaming
    // one is free and cannot break a build.
    var QUICK_REL = 'bf6-quick-values.json';
    var quickList = null;      // [{file, name, label, kind}]
    var quickValues = {};      // "file|name" -> current literal text

    // The occurrence is part of the identity: two entries can share a name in
    // the same file and be different values.
    function quickKey(q) { return q.file + '|' + q.name + '|' + (q.nth || 0); }

    // FINDING A DECLARATION WITHOUT A REGULAR EXPRESSION.
    //
    // The first version matched const/let/var NAME = (everything up to ; or a
    // newline). Two things break that, and both were reproduced against real
    // source rather than imagined:
    //
    //   const TITLE = 'Hello; soldier';
    //     the semicolon INSIDE the string ended the match, so writing a new
    //     value produced  const TITLE = 'Changed'; soldier';  - two parser
    //     errors and somebody's file broken by a settings panel.
    //
    //   // the old value was const SCORE = 10
    //   const SCORE = 250;
    //     the comment matched first, so the comment was edited and the actual
    //     declaration was left alone. Silently doing nothing while reporting
    //     success is the worse half of that.
    //
    // So the text is walked once with the state a source file actually has -
    // line comments, block comments, single, double and template strings, and
    // bracket depth - and the declaration is only recognised in code. It is not
    // a TypeScript parser and does not try to be; it is enough to find one
    // declaration's initializer exactly, and it refuses rather than guesses
    // when it cannot.
    // `want` is which occurrence of the name to return, counting from 0.
    //
    // It matters because real scripts repeat a name deliberately: this one
    // holds killsPerWeapon three times, once in each difficulty preset. Editing
    // by name alone always wrote the first, so changing the hard preset quietly
    // changed the easy one instead. Every value carries the occurrence it was
    // found at, and the edit goes back to that same one.
    // A negative `want` collects every occurrence in one pass and returns the
    // list. Asking for them one at a time re-scanned the whole file per
    // occurrence, which on a 300KB bundle with thirty repeated names took
    // thirty-four seconds.
    function scanDeclaration(text, name, want) {
        var s = String(text);
        var n = s.length;
        var i = 0;
        var seen = 0;
        want = want || 0;
        var collecting = want < 0;
        var all = collecting ? [] : null;
        var state = 'code';       // code | line | block | sq | dq | tpl
        var depth = 0;            // () [] {} nesting, so an object literal survives
        var word = '';            // the identifier being accumulated in code
        // WHICH bracket we are inside, not just how deep. A property lives in
        // an object literal; x: number inside parentheses is a parameter type
        // and must never be mistaken for one.
        var stack = [];
        var pendingKeyword = '';  // const/let/var seen most recently
        var awaiting = null;      // {nameEnd} once NAME follows the keyword

        function isIdent(c) { return /[A-Za-z0-9_$]/.test(c); }

        // A REGULAR EXPRESSION IS NOT CODE EITHER.
        //
        // Comments and strings were already skipped, and a regex was not, so
        //   const RE = /const SCORE = 1;/;
        //   const SCORE = 9;
        // found the declaration INSIDE the pattern: setting SCORE to 42 rewrote
        // the regex and left the real SCORE alone. Same class of bug as the
        // comment case, one syntax later.
        var lastSig = '', lastWord = '';

        while (i < n) {
            var c = s[i], c2 = s[i + 1];

            if (state === 'code') {
                if (c === '/' && c2 === '/') { state = 'line'; i += 2; continue; }
                if (c === '/' && c2 === '*') { state = 'block'; i += 2; continue; }
                if (c === '/' && regexCanStart(lastSig, lastWord)) {
                    var re = skipRegex(s, i);
                    if (re > i) { i = re; lastSig = 'a'; lastWord = ''; continue; }
                }
                if (c === "'") { state = 'sq'; i++; lastSig = 'a'; lastWord = ''; continue; }
                if (c === '"') { state = 'dq'; i++; lastSig = 'a'; lastWord = ''; continue; }
                if (c === '`') { state = 'tpl'; i++; lastSig = 'a'; lastWord = ''; continue; }
                if (c === '(' || c === '[' || c === '{') { depth++; stack.push(c); i++; lastSig = c; lastWord = ''; continue; }
                if (c === ')' || c === ']' || c === '}') { depth--; stack.pop(); i++; lastSig = c; lastWord = ''; continue; }
                if (c !== ' ' && c !== '\t' && c !== '\n' && c !== '\r' && !isIdent(c)) {
                    // Remembered before the character is acted on below, so the
                    // next '/' knows whether a value is expected here.
                    lastSig = c; lastWord = '';
                }

                if (isIdent(c)) {
                    var start = i;
                    while (i < n && isIdent(s[i])) { i++; }
                    word = s.slice(start, i);
                    lastSig = 'a'; lastWord = word;
                    if (word === 'const' || word === 'let' || word === 'var') {
                        pendingKeyword = word;
                        awaiting = null;
                        continue;
                    }
                    if (pendingKeyword && word === name) {
                        awaiting = { nameEnd: i };
                        pendingKeyword = '';
                        continue;
                    }
                    // A PROPERTY INSIDE AN OBJECT IS A SETTING TOO.
                    //
                    //   export const GUNMASTER_CONFIG = {
                    //       KILLS_PER_WEAPON: 5,
                    //
                    // Most tuning in a real mod is written like this, grouped
                    // under one config object, so only accepting a top-level
                    // const meant the commonest case was refused with "give it
                    // a name first" - when it already has one.
                    //
                    // Only inside braces, so a type annotation (x: number) and
                    // a label cannot be mistaken for one.
                    if (word === name && stack.length > 0 && stack[stack.length - 1] === '{') {
                        var j = i;
                        while (j < n && (s[j] === ' ' || s[j] === '	')) { j++; }
                        if (s[j] === ':' && s[j + 1] !== ':') {
                            var pv = j + 1;
                            while (pv < n && (s[pv] === ' ' || s[pv] === '	')) { pv++; }
                            var pe = scanPropertyEnd(s, pv);
                            if (pe > pv) {
                                if (collecting) { all.push({ valueStart: pv, valueEnd: pe, nth: seen }); }
                                else if (seen === want) { return { valueStart: pv, valueEnd: pe, nth: want }; }
                                seen++;
                                // Past this one, and past its value, so a name
                                // repeated inside it cannot be counted twice.
                                i = pe;
                                pendingKeyword = ''; awaiting = null;
                                continue;
                            }
                        }
                    }
                    // Any other identifier ends the keyword's reach: "const x"
                    // followed by something else is not our declaration.
                    pendingKeyword = '';
                    continue;
                }

                if (awaiting) {
                    // Between the name and its "=" only a type annotation may
                    // sit. Anything else means this was not an initialiser.
                    if (c === '=' && c2 !== '=') {
                        var vs = i + 1;
                        while (vs < n && (s[vs] === ' ' || s[vs] === '\t')) { vs++; }
                        var ve = scanInitializerEnd(s, vs);
                        if (ve > vs) {
                            if (collecting) { all.push({ valueStart: vs, valueEnd: ve, nth: seen }); }
                            else if (seen === want) { return { valueStart: vs, valueEnd: ve, nth: want }; }
                            seen++;
                            i = ve;
                            pendingKeyword = ''; awaiting = null;
                            continue;
                        }
                        if (collecting) { awaiting = null; i++; continue; }
                        return null;
                    }
                    if (c === ':' || c === ' ' || c === '\t' || c === '<' || c === '>'
                        || c === '|' || c === '[' || c === ']' || c === '.' || c === ',') {
                        i++; continue;
                    }
                    if (c === '\n' || c === ';') { awaiting = null; i++; continue; }
                }
                i++;
                continue;
            }

            if (state === 'line') { if (c === '\n') { state = 'code'; } i++; continue; }
            if (state === 'block') { if (c === '*' && c2 === '/') { state = 'code'; i += 2; continue; } i++; continue; }
            if (state === 'sq' || state === 'dq' || state === 'tpl') {
                if (c === '\\') { i += 2; continue; }
                if ((state === 'sq' && c === "'") || (state === 'dq' && c === '"') || (state === 'tpl' && c === '`')) {
                    state = 'code';
                }
                i++;
                continue;
            }
            i++;
        }
        return collecting ? all : null;
    }

    // Every place this name is set, in one pass.
    function scanDeclarationAll(text, name) { return scanDeclaration(text, name, -1); }

    // ---- telling a regular expression from a division ----------------------
    //
    // The only difference is what came before the slash. After a value - a
    // name, a number, a closing bracket - it divides; where a value is expected
    // it opens a pattern. These few tokens cover every case a mod actually
    // writes, and getting it wrong the safe way (treating a divide as a regex)
    // is caught by the newline check below rather than swallowing the file.
    var REGEX_AFTER_WORD = {
        'return': 1, 'typeof': 1, 'case': 1, 'in': 1, 'of': 1, 'new': 1, 'delete': 1,
        'void': 1, 'instanceof': 1, 'do': 1, 'else': 1, 'yield': 1, 'await': 1
    };

    function regexCanStart(lastSig, lastWord) {
        if (!lastSig) { return true; }                       // the very start of the file
        if (lastWord && REGEX_AFTER_WORD[lastWord]) { return true; }
        if (lastSig === 'a') { return false; }                // after a name, string or number
        return '=(,[:!&|?{};+-*%~^<>'.indexOf(lastSig) >= 0;
    }

    // From the opening slash to just past the flags, or -1 if this was not a
    // regex after all. A pattern cannot contain a raw newline, so hitting one
    // means the slash was a divide.
    function skipRegex(s, i) {
        var n = s.length, inClass = false;
        i++;
        while (i < n) {
            var c = s[i];
            if (c === '\\') { i += 2; continue; }
            if (c === '\n') { return -1; }
            if (c === '[') { inClass = true; i++; continue; }
            if (c === ']') { inClass = false; i++; continue; }
            if (c === '/' && !inClass) { i++; break; }
            i++;
        }
        if (i > n) { return -1; }
        while (i < n && /[a-z]/.test(s[i])) { i++; }         // gimsuy
        return i;
    }

    // Where a PROPERTY's value ends: the comma or closing brace that belongs to
    // the object holding it, not one inside a nested value.
    function scanPropertyEnd(s, from) {
        var n = s.length, i = from, depth = 0, state = 'code';
        // A value is expected at the start, so a slash there opens a pattern.
        var lastSig = '=', lastWord = '';
        while (i < n) {
            var c = s[i], c2 = s[i + 1];
            if (state === 'code') {
                if (c === '/' && c2 === '/') { break; }
                if (c === '/' && c2 === '*') { state = 'block'; i += 2; continue; }
                // A regex can hold a ; or a , or a } - every character this
                // scanner stops on - so it has to be stepped over whole.
                if (c === '/' && regexCanStart(lastSig, lastWord)) {
                    var reAt = skipRegex(s, i);
                    if (reAt > i) { i = reAt; lastSig = 'a'; lastWord = ''; continue; }
                }
                if (c === "'") { state = 'sq'; i++; continue; }
                if (c === '"') { state = 'dq'; i++; continue; }
                if (c === '`') { state = 'tpl'; i++; continue; }
                if (c === '(' || c === '[' || c === '{') { depth++; i++; continue; }
                if (c === ')' || c === ']' || c === '}') {
                    if (depth === 0) { break; }
                    depth--; i++; continue;
                }
                if (depth === 0 && (c === ',' || c === '\n' || c === ';')) { break; }
                if (c !== ' ' && c !== '\t' && c !== '\r') {
                    lastSig = /[A-Za-z0-9_$]/.test(c) ? 'a' : c;
                    lastWord = '';
                }
                i++;
                continue;
            }
            if (state === 'block') { if (c === '*' && c2 === '/') { state = 'code'; i += 2; continue; } i++; continue; }
            if (c === '\\') { i += 2; continue; }
            if ((state === 'sq' && c === "'") || (state === 'dq' && c === '"') || (state === 'tpl' && c === '`')) {
                state = 'code';
            }
            i++;
        }
        while (i > from && /\s/.test(s[i - 1])) { i--; }
        return i;
    }

    // Where an initialiser ends: the first ; or line end that is genuinely in
    // code, at the same bracket depth it started.
    function scanInitializerEnd(s, from) {
        var n = s.length, i = from, depth = 0, state = 'code';
        // A value is expected at the start, so a slash there opens a pattern.
        var lastSig = '=', lastWord = '';
        while (i < n) {
            var c = s[i], c2 = s[i + 1];
            if (state === 'code') {
                if (c === '/' && c2 === '/') { break; }
                if (c === '/' && c2 === '*') { state = 'block'; i += 2; continue; }
                // A regex can hold a ; or a , or a } - every character this
                // scanner stops on - so it has to be stepped over whole.
                if (c === '/' && regexCanStart(lastSig, lastWord)) {
                    var reAt = skipRegex(s, i);
                    if (reAt > i) { i = reAt; lastSig = 'a'; lastWord = ''; continue; }
                }
                if (c === "'") { state = 'sq'; i++; continue; }
                if (c === '"') { state = 'dq'; i++; continue; }
                if (c === '`') { state = 'tpl'; i++; continue; }
                if (c === '(' || c === '[' || c === '{') { depth++; i++; continue; }
                if (c === ')' || c === ']' || c === '}') {
                    if (depth === 0) { break; }
                    depth--; i++; continue;
                }
                if (depth === 0 && (c === ';' || c === '\n')) { break; }
                if (c !== ' ' && c !== '\t' && c !== '\r') {
                    lastSig = /[A-Za-z0-9_$]/.test(c) ? 'a' : c;
                    lastWord = '';
                }
                i++;
                continue;
            }
            if (state === 'block') { if (c === '*' && c2 === '/') { state = 'code'; i += 2; continue; } i++; continue; }
            if (c === '\\') { i += 2; continue; }
            if ((state === 'sq' && c === "'") || (state === 'dq' && c === '"') || (state === 'tpl' && c === '`')) {
                state = 'code';
            }
            i++;
        }
        // Trailing spaces belong to the layout, not to the value.
        while (i > from && /\s/.test(s[i - 1])) { i--; }
        return i;
    }

    function quickReadFrom(text, name, nth) {
        var at = scanDeclaration(text, name, nth);
        if (!at) { return null; }
        return { literal: String(text).slice(at.valueStart, at.valueEnd).trim(),
                 index: 0, nth: at.nth || 0 };
    }

    function quickWriteInto(text, name, literal, nth) {
        var at = scanDeclaration(text, name, nth);
        if (!at) { return null; }
        // A minimal edit: only the initialiser's own characters move, so every
        // byte of the rest of the file is untouched.
        return String(text).slice(0, at.valueStart) + literal + String(text).slice(at.valueEnd);
    }

    function saveQuickList() {
        return call('write', {
            rel: QUICK_REL,
            text: JSON.stringify({ values: quickList || [] }, null, 4) + '\n'
        });
    }

    function loadQuick() {
        if (!project) { quickList = null; drawQuick(); return Promise.resolve(); }
        return readText(QUICK_REL, true).then(function (text) {
            try { quickList = (JSON.parse(text || '{}').values) || []; }
            catch (e) { quickList = []; }
            return refreshQuickValues();
        }, function () { quickList = []; drawQuick(); });
    }

    // The CURRENT value of each entry, read from the source it lives in. Done
    // per file so a list of twenty values in one file is one read.
    function refreshQuickValues() {
        quickValues = {};
        var byFile = {};
        (quickList || []).forEach(function (q) { (byFile[q.file] = byFile[q.file] || []).push(q); });
        var files = Object.keys(byFile);
        if (!files.length) { drawQuick(); return Promise.resolve(); }
        return Promise.all(files.map(function (f) {
            return readText(f, true).then(function (text) {
                byFile[f].forEach(function (q) {
                    var found = quickReadFrom(text, q.name, q.nth || 0);
                    quickValues[quickKey(q)] = found ? found.literal : null;
                });
            }, function () {
                byFile[f].forEach(function (q) { quickValues[quickKey(q)] = null; });
            });
        })).then(drawQuick);
    }

    function quickSet(q, literal) {
        // WHICH PROJECT THIS EDIT BELONGS TO IS DECIDED NOW, NOT WHEN THE READ
        // COMES BACK.
        //
        // The read is a round trip. Start one in project A, switch to B while
        // it is in flight, and the reply used to be written into B: same
        // relative filename, A's text, B's file. The project was read from the
        // live variable inside the callback, by which time it was B.
        var forProject = project && project.path;
        var forGen = project && project.gen;
        return readText(q.file).then(function (text) {
            if ((project && project.path) !== forProject || (project && project.gen) !== forGen) {
                say('you changed project while that value was being read, so nothing was written.', 'w');
                return;
            }
            var next = quickWriteInto(text, q.name, literal, q.nth || 0);
            if (next === null) {
                say('could not find ' + q.name + ' in ' + q.file + ' any more. It may have been renamed.', 'e');
                return;
            }
            // Through the same path an edit in the editor takes, so an open tab
            // shows the new value rather than going stale behind it.
            if (files[q.file]) { files[q.file].model.setValue(next); }

            // WHAT WAS WRITTEN IS NOT NECESSARILY WHAT IS ON THE SCREEN.
            //
            // A write is a round trip. Typing does not stop while it is in
            // flight, so by the time the acknowledgment arrives the model can
            // hold something newer than the text that reached the disk. Marking
            // it clean regardless marks the NEWER version saved: autosave then
            // skips it, and those edits exist only on screen. Close the tab and
            // they are gone.
            //
            // This is the same trap saveOne already guards, and the Quick Value
            // path went round it. Only the exact text that was sent may clear
            // the flag; anything newer stays dirty and gets saved on its own.
            var sentProject = project && project.path;
            var sent = next;
            return call('write', { rel: q.file, text: sent }).then(function () {
                // A reply that arrives after the project changed says nothing
                // about the project now open.
                if ((project && project.path) !== sentProject) { return; }
                quickValues[quickKey(q)] = literal;
                var f = files[q.file];
                if (f && f.model.getValue() === sent) { f.dirty = false; drawTabs(); }
            });
        });
    }

    // ---- swapping a sound or an effect -------------------------------------
    //
    // A sound in a Portal script is a RuntimeSpawn enum member, so swapping one
    // is swapping an identifier - which makes it a quick value like any other,
    // except that nobody can hold 938 names in their head. Hence a search box
    // and, where the tool can manage it, something to listen to.
    var sfxAll = null;          // [{name, enum, kind, crash, silent, hasClip}]
    var sfxPreviewSource = '';  // 'addon', 'soundboard', or empty
    var sfxAudio = null;
    var sfxFor = null;          // the quick value being changed

    function loadSfx() {
        if (sfxAll) return Promise.resolve(sfxAll);
        return call('sfxlist').then(function (r) {
            sfxAll = r.items || [];
            sfxPreviewSource = r.preview || '';
            return sfxAll;
        }, function () { sfxAll = []; return sfxAll; });
    }

    function sfxStop() {
        if (sfxAudio) { try { sfxAudio.pause(); } catch (e) {} sfxAudio = null; }
        if (sfxPreviewSource === 'addon') { call('sfxstop').catch(function () {}); }
    }

    // TWO WAYS TO HEAR ONE SOUND.
    //
    //   addon      the game's own asset, decoded and played by the editor. No
    //              audio crosses the page bridge, and it is the real thing at
    //              its real quality.
    //   soundboard a recorded clip, handed over as data and played here.
    //
    // The first is tried whenever it is available, and a failure falls through
    // to the second rather than leaving the button dead.
    function sfxPlay(name) {
        sfxStop();
        if (!sfxPreviewSource) { say('no sound library is set up to play from', 'w'); return; }
        if (sfxPreviewSource === 'addon') {
            call('sfxplay', { name: name }).then(function () { /* the editor is playing it */ },
                function (e) {
                    // Not every name in the enum has an asset in every install.
                    say((e && e.why) || ('could not play ' + name), 'w');
                });
            return;
        }
        call('sfxclip', { name: name }).then(function (r) {
            if (!r || !r.audio) { say('nothing to play for ' + name, 'w'); return; }
            sfxAudio = new Audio('data:audio/ogg;base64,' + r.audio);
            sfxAudio.play().catch(function () { say('could not play ' + name, 'w'); });
        }, function (e) { say((e && e.why) || 'could not fetch that sound', 'w'); });
    }

    // The picker itself: one search box over every sound and effect the SDK
    // declares, filtered as you type.
    function openSfxPicker(q) {
        sfxFor = q;
        var pane = $('sfxPane');
        if (!pane) return;
        pane.hidden = false;
        $('sfxTitle').textContent = 'Choose a ' + (q.kind === 'fx' ? 'effect' : 'sound') + ' for ' + (q.label || q.name);
        var box = $('sfxSearch');
        box.value = '';
        loadSfx().then(function () {
            var note = $('sfxSource');
            if (note) {
                note.textContent = sfxPreviewSource === 'addon'
                    ? 'Playing from your game install, at full quality.'
                    : sfxPreviewSource === 'soundboard'
                        ? 'Previews come from your recorded sound library.'
                        : 'No preview available: install the High Poly add-on, or set a sound library folder.';
            }
            drawSfxList('');
            box.focus();
        });
    }

    function closeSfxPicker() {
        sfxStop();
        var pane = $('sfxPane');
        if (pane) pane.hidden = true;
        sfxFor = null;
    }

    function drawSfxList(term) {
        var host = $('sfxList');
        if (!host) return;
        host.innerHTML = '';
        var want = String(term || '').toLowerCase();
        var kind = sfxFor && sfxFor.kind === 'fx' ? 'fx' : 'sfx';
        var rows = (sfxAll || []).filter(function (x) {
            if (x.kind !== kind) return false;
            return !want || x.name.toLowerCase().indexOf(want) >= 0;
        });
        $('sfxCount').textContent = rows.length + ' of '
            + (sfxAll || []).filter(function (x) { return x.kind === kind; }).length;

        rows.slice(0, 300).forEach(function (x) {
            var row = el('div', 'sfxRow');
            var nm = el('span', 'sfxName', x.name);
            row.appendChild(nm);

            // The warnings are the point of showing the manifest at all.
            if (x.crash) { row.appendChild(el('span', 'sfxBad', 'crashes the game')); }
            else if (x.silent) { row.appendChild(el('span', 'sfxWarn', 'silent')); }
            else if (x.unreliable) { row.appendChild(el('span', 'sfxWarn', 'unreliable')); }
            else if (x.dur) { row.appendChild(el('span', 'sfxDur', x.dur.toFixed(1) + 's')); }
            else { row.appendChild(el('span', 'sfxDur', '')); }

            var btns = el('div', 'sfxBtns');
            if (sfxPreviewSource && !x.crash && (x.hasClip || sfxPreviewSource === 'addon')) {
                var p = el('button', null, 'Play');
                p.onclick = function (ev) { ev.stopPropagation(); sfxPlay(x.name); };
                btns.appendChild(p);
            }
            var use = el('button', 'primary', 'Use this');
            use.onclick = function () { applySfx(x); };
            btns.appendChild(use);
            row.appendChild(btns);
            host.appendChild(row);
        });
        if (rows.length > 300) {
            host.appendChild(el('div', 'strEmptyD', 'and ' + (rows.length - 300)
                + ' more. Type a few more letters to narrow it down.'));
        }
        if (!rows.length) {
            host.appendChild(el('div', 'strEmptyD', 'Nothing matches "' + term + '".'));
        }
    }

    // The new reference, written the way the old one was: everything in front
    // of the enum is kept, the enum and the member are replaced.
    //
    //   mod.RuntimeSpawn_Common.SFX_Old  ->  mod.RuntimeSpawn_Other.SFX_New
    //   RuntimeSpawn_Common.SFX_Old      ->  RuntimeSpawn_Other.SFX_New
    function sfxLiteral(current, x) {
        var parts = String(current == null ? '' : current).trim().split('.');
        var prefix = parts.length > 2 ? parts.slice(0, parts.length - 2).join('.') + '.' : '';
        return prefix + x['enum'] + '.' + x.name;
    }

    function applySfx(x) {
        if (!sfxFor) return;
        var q = sfxFor;
        // The reference has to name its enum, or it will not compile - and it
        // has to reach that enum the same way the rest of the file does. This
        // project writes mod.RuntimeSpawn_Common.SFX_x; another may have the
        // enum imported and write it bare. Whatever stands in front of the enum
        // now is what the file can resolve, so it is kept.
        var literal = sfxLiteral(quickValues[quickKey(q)], x);
        quickSet(q, literal).then(function () {
            say('set ' + (q.label || q.name) + ' to ' + x.name, 'o');
            closeSfxPicker();
            refreshQuickValues();
        }, function (e) {
            say('could not change it: ' + (e && (e.why || e.error)), 'e');
        });
    }

    // WHERE THE VALUES ACTUALLY ARE.
    //
    // src/ first, because a value edited there survives a build. But an
    // imported experience is scaffolded from the template, so src/ is never
    // EMPTY - it holds index.ts and helpers/ that nobody wrote - and keying the
    // fallback on "no source files" meant it never fired and the list stayed
    // blank on exactly the projects that needed it.
    //
    // So the fallback is on the RESULT: if the source yields no settings, the
    // built file is read instead. That is the mod, and a change to it goes
    // straight into what Portal is given.
    function valueSourceFiles(all) {
        var src = (all || []).filter(function (f) {
            return /^src\/.+\.ts$/.test(f) && f.indexOf('/debug-tool/') < 0;
        });
        return { src: src, bundle: (all || []).indexOf('dist/bundle.ts') >= 0 ? 'dist/bundle.ts' : '' };
    }

    function scanForValues(all) {
        var where = valueSourceFiles(all);
        return Promise.all(where.src.map(function (rel) {
            return readText(rel, true).then(function (t) { return scanFileForValues(rel, t); },
                                            function () { return []; });
        })).then(function (lists) {
            var found = [];
            lists.forEach(function (l) { found = found.concat(l); });
            if (found.length > 0 || !where.bundle) { return { rows: found, fromBundle: false }; }
            return readText(where.bundle, true).then(function (t) {
                return { rows: scanFileForValues(where.bundle, t), fromBundle: true };
            }, function () { return { rows: [], fromBundle: false }; });
        });
    }

    function drawQuick() {
        var host = $('quickList');
        if (!host) return;
        host.innerHTML = '';
        var list = quickList || [];
        var empty = $('quickEmpty');
        if (empty) empty.hidden = list.length > 0;

        list.forEach(function (q, i) {
            var row = el('div', 'strRow qRow');

            var lab = document.createElement('input');
            lab.type = 'text';
            lab.className = 'strText qLabel';
            lab.value = q.label || q.name;
            lab.title = 'What you call it. Renaming this never touches your script.';
            lab.oninput = function () { q.label = lab.value; saveQuickList(); };
            row.appendChild(lab);

            var ctrl = el('div', 'qCtrl');
            var cur = quickValues[quickKey(q)];
            if (cur === null || cur === undefined) {
                ctrl.appendChild(el('span', 'qGone', 'not found in ' + q.file));
            } else if (q.kind === 'boolean') {
                var cb = document.createElement('input');
                cb.type = 'checkbox';
                cb.checked = String(cur).trim() === 'true';
                cb.onchange = function () { quickSet(q, cb.checked ? 'true' : 'false'); };
                ctrl.appendChild(cb);
                ctrl.appendChild(el('span', 'qHint', cb.checked ? 'on' : 'off'));
            } else if (q.kind === 'number') {
                var num = document.createElement('input');
                num.type = 'number';
                num.className = 'qNum';
                num.value = cur;
                num.onchange = function () {
                    if (num.value === '' || isNaN(Number(num.value))) { num.value = cur; return; }
                    quickSet(q, String(Number(num.value)));
                };
                ctrl.appendChild(num);
            } else if (q.kind === 'sfx' || q.kind === 'fx') {
                // A picker, not a text box: 938 sounds, and a typo is a build
                // failure rather than a wrong note.
                var cur2 = String(cur).trim();
                var shown = cur2.indexOf('.') >= 0 ? cur2.split('.').pop() : cur2;
                ctrl.appendChild(el('span', 'qSfxName', shown));
                var ch = el('button', null, 'Change');
                ch.onclick = function () { openSfxPicker(q); };
                ctrl.appendChild(ch);
            } else {
                var txt = document.createElement('input');
                txt.type = 'text';
                txt.className = 'qStr';
                txt.value = quickUnquote(cur);
                txt.onchange = function () { quickSet(q, quickQuote(txt.value)); };
                ctrl.appendChild(txt);
            }
            row.appendChild(ctrl);

            var btns = el('div', 'strBtns');
            var go = el('button', null, 'Show me');
            go.title = q.name + ' in ' + q.file;
            go.onclick = function () {
                showQuick(false);
                openFile(q.file);
                setTimeout(function () { quickReveal(q.name, q.line); }, 250);
            };
            btns.appendChild(go);

            var rm = el('button', 'strDel', 'Remove');
            rm.title = 'Take it off this list. Your script is not changed.';
            rm.onclick = function () {
                quickList.splice(i, 1);
                saveQuickList();
                drawQuick();
            };
            btns.appendChild(rm);
            row.appendChild(btns);

            // The same two things on right-click, because that is where a hand
            // already is when it wants to get rid of one.
            row.oncontextmenu = function (ev) {
                openCtx(ev, [
                    { label: 'Show it in the code', run: function () {
                        showQuick(false);
                        openFile(q.file);
                        setTimeout(function () { quickReveal(q.name, q.line); }, 250);
                    } },
                    { label: 'Remove from this list', run: function () {
                        quickList.splice(i, 1);
                        saveQuickList();
                        drawQuick();
                    } }
                ]);
            };
            host.appendChild(row);
        });
    }

    // Put the cursor on the declaration, so "Show me" lands on the line rather
    // than merely opening the file.
    function quickReveal(name, line) {
        if (!editor || !editor.getModel()) return;
        // The line it was found on, when there is one: with the same name in
        // three difficulty presets, searching for it lands on the wrong one.
        if (line && line <= editor.getModel().getLineCount()
            && editor.getModel().getLineContent(line).indexOf(name) >= 0) {
            editor.revealLineInCenter(line);
            editor.setPosition({ lineNumber: line, column: 1 });
            editor.focus();
            return;
        }
        var matches = editor.getModel().findMatches(
            '\\b(?:const|let|var)\\s+' + name + '\\b', false, true, false, null, false, 1);
        if (!matches.length) {
            matches = editor.getModel().findMatches('\\b' + name + '\\b', false, true, false, null, false, 1);
        }
        if (!matches.length) return;
        var r = matches[0].range;
        editor.revealLineInCenter(r.startLineNumber);
        editor.setPosition({ lineNumber: r.startLineNumber, column: r.startColumn });
        editor.focus();
    }

    function showQuick(on) {
        var pane = $('quickPane'), split = $('split');
        note('showQuick(' + on + ') pane=' + (pane ? 'yes' : 'MISSING'));
        if (!pane) return;
        if (on) { showStrings(false); }
        pane.hidden = !on;
        if (split) split.style.display = (on || isStringsOpen()) ? 'none' : '';
        if (on) { loadQuick().then(offerValues); }
        drawTabs();
    }

    // OFFERED, NEVER ADDED. Nothing goes on this list that the author did not
    // put there: the whole point is that it is short, and a list filled in for
    // somebody is a list they now have to prune. So an empty list says how many
    // it can see and waits to be asked.
    //
    // Once only, per project per session - an offer that reappears every time
    // the tab is opened is not an offer, it is nagging.
    var offeredFor = {};
    function offerValues() {
        if (!project || (quickList && quickList.length)) return;
        if (offeredFor[project.path]) return;
        var box = $('quickOffer');
        if (!box) return;
        offeredFor[project.path] = 1;
        // Counted quietly first, so a project with nothing to offer says nothing.
        call('files').then(function (r) {
            return scanForValues(r.files || []);
        }).then(function (res) {
            var n = res.rows.length;
            var txt = $('quickOfferText');
            if (!n) { return; }
            box.hidden = false;
            if (txt) {
                txt.textContent = 'There are ' + n + ' value' + (n === 1 ? '' : 's')
                    + ' in your script that could go on this list.'
                    + (res.fromBundle
                        ? ' They are in the built file, so changes go straight into what Portal '
                          + 'gets - and rebuilding from source would replace them.'
                        : '');
            }
        });
    }

    function isQuickOpen() {
        var p = $('quickPane');
        return !!(p && !p.hidden);
    }

    // Which control a value can be edited with, or '' for one that has no
    // simple control - an object, an array, a call, an expression. Anything
    // else would need a code editor to change, and a code editor is what the
    // Values tab exists to avoid.
    //
    // A sound or an effect is not a literal, it is an enum member, so it looks
    // like an expression to anything reading for literals - which is exactly
    // why the one kind of value people most want to swap was the one kind the
    // list refused to take. It gets a picker rather than a text box: there are
    // 938 sounds and a typo is a build failure, not a wrong note.
    function quickKindOf(literal) {
        var t = String(literal == null ? '' : literal).trim();
        if (t === 'true' || t === 'false') { return 'boolean'; }
        if (/^-?\d+(\.\d+)?$/.test(t)) { return 'number'; }
        if (/^(['"`])(?:\\.|(?!\1)[\s\S])*\1$/.test(t)) { return 'string'; }
        var s = spawnKindOf(t);
        if (s) { return s; }
        return '';
    }

    // mod.RuntimeSpawn_Common.SFX_Alarm, RuntimeSpawn_Common.SFX_Alarm, or the
    // bare member. Real scripts write all three, and the one measured here
    // writes the longest: the whole path in front of the member is optional and
    // may be any depth.
    //
    // The prefix on the MEMBER says which it is, because the enum it lives in
    // does not: RuntimeSpawn_Common holds both sounds and effects.
    function spawnKindOf(t) {
        var m = String(t).trim().match(/^(?:[A-Za-z_$][\w$]*\s*\.\s*)*((?:S|V)?FX_[\w$]+)$/);
        if (!m) { return ''; }
        return /^SFX_/.test(m[1]) ? 'sfx' : 'fx';
    }

    // What the right-click acts on: the name under the cursor, and the value it
    // is declared with.
    function quickAddFromCursor() {
        if (!editor || !activeRel) { say('open a script file first', 'w'); return; }
        var model = editor.getModel();
        var pos = editor.getPosition();
        var word = model.getWordAtPosition(pos);
        if (!word) { say('put the cursor on the name of the value first.', 'w'); return; }
        var name = word.word;
        var text = model.getValue();
        // WHICH ONE THE CURSOR IS ON, when the name is used more than once.
        // Right-clicking killsPerWeapon in the hard preset must add the hard
        // one, not the first one in the file that happens to share its name.
        var nth = 0;
        for (var k = 0; k < 64; k++) {
            var cand = scanDeclaration(text, name, k);
            if (!cand) { break; }
            if (model.getPositionAt(cand.valueStart).lineNumber === pos.lineNumber) { nth = k; break; }
        }
        var found = quickReadFrom(text, name, nth);
        if (!found) {
            // A bare literal has nothing to anchor to, and anchoring to a line
            // would start editing the wrong one the first time the file changed.
            say('"' + name + '" is not a named value. Give it a name first - '
                + 'const ' + name + ' = ... - then add it.', 'w');
            return;
        }
        var kind = quickKindOf(found.literal);
        if (!kind) {
            say('"' + name + '" is ' + found.literal.slice(0, 30)
                + ', which has no simple control. Numbers, on/off and text can be added.', 'w');
            return;
        }
        quickList = quickList || [];
        var already = quickList.some(function (q) {
            return q.file === activeRel && q.name === name && (q.nth || 0) === nth;
        });
        if (already) { say('"' + name + '" is already on the quick list.', 'w'); showQuick(true); return; }
        quickList.push({ file: activeRel, name: name, label: name, kind: kind, nth: nth });
        saveQuickList().then(function () {
            say('added "' + name + '" to Values', 'o');
            showQuick(true);
        });
    }

    // ---- finding the settings for them ------------------------------------
    //
    // MEASURED BEFORE IT WAS BUILT, across eight real Portal projects by
    // different authors: every one declares its settings as named literal
    // constants (10 to 197 of them), so they are always findable. But the
    // NAMING is not shared - the share written UPPER_SNAKE runs from 16% to
    // 73% - so shoutiness is a good way to rank candidates and a bad way to
    // filter them. Filtering on it would hide most of one project's settings
    // and half of another's.
    //
    // And the counts are why nothing is added automatically: a list of 197 is
    // the haystack this feature exists to get out of. So the script is scanned,
    // the likely ones are floated to the top, and the author ticks the handful
    // they actually want.
    var suggestRows = null;
    var suggestFromBundle = false;

    function suggestRank(s) {
        var r = 0;
        if (/^[A-Z][A-Z0-9_]*$/.test(s.name)) r += 100;   // the loudest signal
        // A sound or an effect is named to be swapped. Nobody keeps one in a
        // script for any other reason.
        if (s.kind === 'sfx' || s.kind === 'fx') r += 60;
        if (s.kind === 'boolean') r += 20;                // an on/off is nearly always a setting
        if (s.exported) r += 15;
        // Something declared near the top of a file is usually configuration;
        // something declared on line 900 is usually working state.
        r += Math.max(0, 30 - Math.floor(s.line / 20));
        return r;
    }

    // WHAT COUNTS AS A VALUE SOMEBODY MIGHT WANT TO CHANGE.
    //
    // Two shapes, both measured in real Portal scripts:
    //
    //   const START_CASH = 500                       a named declaration
    //   const CFG = { KILLS_PER_WEAPON: 5, ... }     a property of a settings object
    //
    // The second was missed entirely, and it is how most authors group their
    // settings - the file this was measured against keeps seven of its eight
    // sounds that way. Both shapes are editable by name through the same
    // quickReadFrom/quickWriteInto path, so both belong here.
    //
    // A value is a literal, or an enum member such as
    // mod.RuntimeSpawn_Common.SFX_Alarm. The enum member is a sound or an
    // effect: not a literal, so a scan that read only literals could never
    // find the values people most want to swap.
    var VALUE_RE = '(-?\\d+(?:\\.\\d+)?|true|false|\'[^\']*\'|"[^"]*"'
                 + '|(?:[A-Za-z_$][\\w$]*\\s*\\.\\s*)*(?:S|V)?FX_[\\w$]+)';

    function scanFileForValues(rel, text) {
        var out = [];
        var lines = String(text).split('\n');
        var reDecl = new RegExp('^\\s*(export\\s+)?(?:const|let|var)\\s+([A-Za-z_$][\\w$]*)\\s*'
            + '(?::\\s*[^=]+?)?\\s*=\\s*' + VALUE_RE + '\\s*;?\\s*(?:\\/\\/.*)?$');
        // A property line. The name may be quoted; the value may end in a comma
        // rather than a semicolon. `case 'x':` and `default:` are not properties.
        var reProp = new RegExp('^\\s*(?:[\'"])?([A-Za-z_$][\\w$]*)[\'"]?\\s*:\\s*'
            + VALUE_RE + '\\s*,?\\s*(?:\\/\\/.*)?$');
        lines.forEach(function (l, i) {
            if (/^\s*(case|default|return|import|export\s+default)\b/.test(l)) { return; }
            var m = reDecl.exec(l);
            var isProp = false;
            if (!m) { m = reProp.exec(l); isProp = !!m; }
            if (!m) return;
            var kind = quickKindOf(m[isProp ? 2 : 3]);
            if (!kind) return;
            out.push({ file: rel, name: m[isProp ? 1 : 2], value: m[isProp ? 2 : 3], kind: kind,
                       line: i + 1, exported: !isProp && !!m[1], property: isProp });
        });
        return withOccurrences(String(text), out);
    }

    // A NAME IS NOT ALWAYS UNIQUE.
    //
    // Difficulty presets are the ordinary case: three objects, each with a
    // killsPerWeapon in it. The editor works by name, so all three read and
    // wrote the first one - pick the hard preset off the list, change it, and
    // the easy preset changes instead, with nothing on screen to say so.
    //
    // Each row is therefore told WHICH occurrence it is, by matching the line
    // it was found on against the occurrences the editor itself can reach. A
    // row the editor cannot reach is dropped rather than offered: an entry that
    // cannot be edited is worse than one that is not there.
    function withOccurrences(text, rows) {
        var count = {};
        rows.forEach(function (r) { count[r.name] = (count[r.name] || 0) + 1; });
        var lineOf = null;
        var places = {};      // name -> line number of each occurrence, in order
        var kept = [];
        rows.forEach(function (r) {
            if ((count[r.name] || 0) < 2) { r.nth = 0; kept.push(r); return; }
            if (!lineOf) {
                // Character offset to line number, built once per file.
                lineOf = [];
                var at = 0, ln = 1;
                while (at < text.length) {
                    lineOf[at] = ln;
                    if (text.charCodeAt(at) === 10) { ln++; }
                    at++;
                }
            }
            // One pass per repeated NAME, not one per row: the same thirty
            // names were otherwise re-scanned once per occurrence.
            if (!places[r.name]) {
                places[r.name] = (scanDeclarationAll(text, r.name) || []).map(function (f) {
                    return lineOf[f.valueStart];
                });
            }
            var k = places[r.name].indexOf(r.line);
            // Not reachable by name: leave it off rather than offer an edit
            // that would land somewhere else.
            if (k < 0) { return; }
            r.nth = k;
            kept.push(r);
        });
        return kept;
    }

    function findValues() {
        if (!project) { say('open a project first', 'w'); return Promise.resolve(); }
        var host = $('suggestList');
        if (host) host.innerHTML = '<div class="strEmptyD">Reading your script...</div>';
        return call('files').then(function (r) {
            return scanForValues(r.files || []);
        }).then(function (res) {
            var all = res.rows;
            // Anything already on the quick list is not a suggestion.
            var have = {};
            (quickList || []).forEach(function (q) { have[quickKey(q)] = 1; });
            all = all.filter(function (s2) { return !have[quickKey(s2)]; });
            all.sort(function (a, b) { return suggestRank(b) - suggestRank(a); });
            suggestRows = all;
            suggestFromBundle = res.fromBundle;
            // A filter left over from the last scan would hide most of a fresh
            // one and look like it found nothing.
            suggestTerm = '';
            suggestKind = '';
            drawSuggest();
        });
    }

    // EVERY VALUE, NOT THE FIRST SIXTY.
    //
    // The list used to stop at sixty and tell you the rest were "less likely to
    // be settings", which is a guess dressed up as a fact: the ranking is a
    // hint, and a real bundle holds 126. Cutting it meant the value somebody
    // came here for could simply not be on the list, with no way to ask for it
    // except right-clicking it in seven thousand lines of code.
    //
    // So all of them are shown, and a box narrows them. The box is built once
    // and the rows below it are redrawn, so typing in it never loses focus.
    //
    // The kind buttons beside it are the other half of that: "the sound I want
    // to swap" and "the number I want to turn up" are the two things people
    // come to this list for, and each is one click away from a hundred names
    // that are neither.
    var suggestTerm = '';
    var suggestKind = '';       // '', 'sfx', 'fx', 'boolean', 'number', 'string'

    function drawSuggest() {
        var host = $('suggestList');
        if (!host) return;
        host.innerHTML = '';
        var rows = suggestRows || [];
        if (!rows.length) {
            host.innerHTML = '<div class="strEmptyD">'
                + ((lastFiles && lastFiles['dist/bundle.ts'])
                    ? 'Nothing to read. This experience is one built file (dist/bundle.ts), and '
                      + 'values are only offered from source you can edit - a value changed in a '
                      + 'built file is wiped by the next build. Run BF6.Script.UseMySource to bring '
                      + 'in the project it was built from.'
                    : 'Nothing found that is not already on the list.')
                + '</div>';
            return;
        }
        var head = el('div', 'sgHead', rows.length + ' value(s) found. The ones most likely to be settings are first. Tick what you want.'
            + (suggestFromBundle
                ? ' These are in the built file (dist/bundle.ts) because this project has no source of its own yet.'
                : ''));
        host.appendChild(head);

        var tools = el('div', 'sgTools');
        var box = document.createElement('input');
        box.type = 'text';
        box.className = 'sgFind';
        box.placeholder = 'Filter by name, value or file';
        box.value = suggestTerm;
        tools.appendChild(box);
        var count = el('span', 'sgCount', '');
        tools.appendChild(count);
        var all = el('button', null, 'Tick all shown');
        tools.appendChild(all);
        var none = el('button', null, 'Untick all');
        tools.appendChild(none);
        host.appendChild(tools);

        // Only kinds this project actually has: a Sounds button on a script
        // with no sounds in it is a button that can only disappoint.
        var have = {};
        rows.forEach(function (s) { have[s.kind] = (have[s.kind] || 0) + 1; });
        var kinds = [
            { k: '', label: 'All', n: rows.length },
            { k: 'sfx', label: 'Sounds', n: have.sfx || 0 },
            { k: 'fx', label: 'Effects', n: have.fx || 0 },
            { k: 'boolean', label: 'On/off', n: have.boolean || 0 },
            { k: 'number', label: 'Numbers', n: have.number || 0 },
            { k: 'string', label: 'Text', n: have.string || 0 }
        ].filter(function (x) { return x.k === '' || x.n > 0; });
        var chips = el('div', 'sgKinds');
        var chipBtns = [];
        kinds.forEach(function (x) {
            var b = el('button', 'sgChip' + (suggestKind === x.k ? ' on' : ''),
                       x.label + ' ' + x.n);
            b.onclick = function () {
                suggestKind = x.k;
                chipBtns.forEach(function (o) { o.b.classList.toggle('on', o.k === x.k); });
                paint();
            };
            chipBtns.push({ b: b, k: x.k });
            chips.appendChild(b);
        });
        if (kinds.length > 1) { host.appendChild(chips); }

        var body = el('div', 'sgBody');
        host.appendChild(body);

        var add = el('button', 'primary', 'Add the ticked ones');
        add.onclick = function () {
            var picked = rows.filter(function (s) { return s.pick; });
            if (!picked.length) { say('tick one first', 'w'); return; }
            quickList = quickList || [];
            picked.forEach(function (s) {
                quickList.push({ file: s.file, name: s.name, label: s.name, kind: s.kind,
                                 nth: s.nth || 0, line: s.line });
            });
            saveQuickList().then(function () {
                say('added ' + picked.length + ' value(s)', 'o');
                suggestRows = null;
                suggestTerm = '';
                suggestKind = '';
                var sp = $('suggestPane'); if (sp) sp.hidden = true;
                refreshQuickValues();
            });
        };
        host.appendChild(add);

        function shown() {
            var want = suggestTerm.toLowerCase();
            return rows.filter(function (s) {
                if (suggestKind && s.kind !== suggestKind) { return false; }
                if (!want) { return true; }
                return (s.name + ' ' + s.value + ' ' + s.file).toLowerCase().indexOf(want) >= 0;
            });
        }

        function paint() {
            var list = shown();
            body.innerHTML = '';
            count.textContent = list.length === rows.length
                ? rows.length + ' shown'
                : list.length + ' of ' + rows.length;
            if (!list.length) {
                body.appendChild(el('div', 'strEmptyD', suggestTerm
                    ? 'Nothing matches "' + suggestTerm + '"'
                      + (suggestKind ? ' in that kind.' : '.')
                    : 'None of that kind here.'));
                return;
            }
            var frag = document.createDocumentFragment();
            list.forEach(function (s) {
                var row = el('div', 'sgRow');
                var cb = document.createElement('input');
                cb.type = 'checkbox';
                cb.checked = !!s.pick;
                cb.onchange = function () { s.pick = cb.checked; };
                row.appendChild(cb);
                row.appendChild(el('span', 'sgName', s.name));
                // A sound reads as its member alone: the enum in front of it is
                // the same on every row and pushes the name off the end.
                var v = (s.kind === 'sfx' || s.kind === 'fx') && s.value.indexOf('.') >= 0
                    ? s.value.split('.').pop() : s.value;
                row.appendChild(el('span', 'sgVal', v));
                // Always a cell, even when it is empty, so every row keeps the
                // same columns.
                row.appendChild(el('span', 'sgKind',
                    s.kind === 'fx' ? 'effect' : s.kind === 'sfx' ? 'sound' : ''));
                row.appendChild(el('span', 'sgWhere', s.file.replace(/^src\//, '') + ':' + s.line));
                frag.appendChild(row);
            });
            body.appendChild(frag);
        }

        box.oninput = function () { suggestTerm = box.value; paint(); };
        all.onclick = function () { shown().forEach(function (s) { s.pick = true; }); paint(); };
        none.onclick = function () { rows.forEach(function (s) { s.pick = false; }); paint(); };
        paint();
    }

    function drawTabs() {
        var host = $('tabs');
        host.innerHTML = '';

        // THE WAY IN, first in the strip. Editing the text a player sees is a
        // different job from editing code and should not require finding a JSON
        // file first, so it gets a tab of its own rather than a menu item.
        var st = el('div', 'ftab strTab' + (isStringsOpen() ? ' on' : ''), 'Text');
        st.id = 'tabStrings';
        st.title = 'Everything your mod can say to a player';
        st.onclick = function () { showQuick(false); showStrings(!isStringsOpen()); };
        host.appendChild(st);

        var qt = el('div', 'ftab strTab' + (isQuickOpen() ? ' on' : ''), 'Values');
        qt.id = 'tabQuick';
        qt.title = 'The settings you pulled out of your script to change quickly';
        qt.onclick = function () { showQuick(!isQuickOpen()); };
        host.appendChild(qt);

        Object.keys(files).forEach(function (rel) {
            var t = el('div', 'ftab closable' + (rel === activeRel && !isStringsOpen() && !isQuickOpen() ? ' on' : '')
                              + (files[rel].dirty ? ' dirty' : ''));
            var label = el('span', 'ftabName', rel);
            t.appendChild(label);

            // A SMALL X, AND A RIGHT-CLICK. Text and Values have neither: they
            // are views of the project rather than open documents, and closing
            // one would leave nothing to reopen it with.
            var x = el('span', 'ftabX', '×');
            x.title = 'Close ' + rel;
            x.onclick = function (ev) { ev.stopPropagation(); closeFile(rel); };
            t.appendChild(x);

            t.onclick = function () { showStrings(false); showQuick(false); openFile(rel); };
            t.oncontextmenu = function (ev) {
                openCtx(ev, [
                    { label: 'Close', run: function () { closeFile(rel); } },
                    // ONE AT A TIME, EACH WAITING FOR THE ONE BEFORE. Firing
                    // twenty writes at once and tidying up afterwards is how a
                    // single refusal gets lost in the middle of a batch; and a
                    // tab that would not save has to still be there at the end.
                    { label: 'Close the others', run: function () {
                        closeMany(Object.keys(files).filter(function (o) { return o !== rel; }))
                            .then(function () { openFile(rel); drawTabs(); });
                    } },
                    { label: 'Close all', run: function () {
                        closeMany(Object.keys(files)).then(function () { drawTabs(); });
                    } }
                ]);
            };
            host.appendChild(t);
        });
    }

    function isStringsOpen() {
        var p = $('stringsPane');
        return !!(p && !p.hidden);
    }

    // WHAT WAS WRITTEN IS NOT NECESSARILY WHAT IS ON THE SCREEN.
    //
    // A write is a round trip to the host. Typing does not stop while it is in
    // flight, so by the time the acknowledgment comes back the model can hold
    // something newer than the text that actually reached the disk. Both save
    // paths used to clear the dirty flag on the reply regardless, which marks
    // the NEWER version clean: autosave then skips it, and the edits made
    // during that round trip are on screen and nowhere else. Close the tab and
    // they are gone.
    //
    // So each write remembers the exact text it sent and the project it
    // belonged to. The flag is cleared only if the model still holds that text.
    // If it does not, the file stays dirty and another save is scheduled. If
    // the project changed underneath, the reply is ignored entirely rather than
    // allowed to mark a different project's file clean.
    function saveOne(rel) {
        var f = files[rel];
        if (!f) { return Promise.resolve({ rel: rel, gone: true }); }
        // THE ONE PLACE A WRITE LEAVES, SO THE ONE PLACE THIS CAN BE ASKED.
        // While the host's project is unknown, `rel` names a file in a project
        // we can only guess at, and the host would resolve it against whatever
        // it has open. Refusing costs a message; guessing costs the contents of
        // somebody else's file.
        if (!writesAreAllowed()) {
            return Promise.reject({
                rel: rel, held: true,
                why: 'the tool has not said which project it has open, so ' + rel +
                     ' was not written anywhere. Your text is still here in the editor.'
            });
        }
        var sent = f.model.getValue();
        var forProject = project && project.path;
        return call('write', { rel: rel, text: sent }).then(function () {
            if ((project && project.path) !== forProject) { return { rel: rel, stale: true }; }
            var now = files[rel];
            if (!now) { return { rel: rel, gone: true }; }
            if (now.model.getValue() === sent) {
                now.dirty = false;
                return { rel: rel, saved: true };
            }
            // Newer text arrived while this was in flight: still dirty, save again.
            scheduleAutosave();
            return { rel: rel, superseded: true };
        });
    }

    function saveActive() {
        if (!activeRel || !project) return Promise.resolve();
        // Ctrl+S is a keyboard command, and Monaco runs those on a read only
        // editor. During a project change the host may already have moved on,
        // so this write could land in the wrong project. The transition saves
        // everything itself, so there is nothing here that needs doing anyway.
        if (switching) { say('changing project. Everything open has already been saved.', 'd'); return Promise.resolve(); }
        var rel = activeRel;
        return saveOne(rel).then(function (r) {
            drawTabs();
            if (r.saved) { say('saved ' + rel, 'g'); }
            else if (r.superseded) { say('saved ' + rel + ', and you have since typed more. Saving again.', 'd'); }
        }, function (r) {
            say('save failed: ' + (r && r.why) + '. Your text is still here in the editor.', 'e');
            throw r;
        });
    }

    // ---- AUTOSAVE ----------------------------------------------------------
    //
    // The site signs people out, sessions expire, editors crash. None of that
    // may cost anybody their work, so nothing here relies on the user pressing
    // save: two seconds after the last keystroke the file goes to disk. The
    // tool writes it to a temporary and renames it into place, and keeps the
    // last ten versions under <project>/.history, so an autosave can never
    // leave a half written file and never destroys the previous one.
    //
    // The dirty star stays until the write comes back, so it is always
    // possible to tell what has landed and what has not.
    var autosaveTimer = null;
    function scheduleAutosave() {
        if (autosaveTimer) clearTimeout(autosaveTimer);
        autosaveTimer = setTimeout(function () {
            autosaveTimer = null;
            if (!project) return;
            // A project change flushes everything itself and then freezes the
            // editor until the new project is open. An autosave firing inside
            // that window would be writing rel paths belonging to the outgoing
            // project into whichever project the host has by then.
            if (switching) return;
            // Already reported by saveAllDirty; absorbed so a failed autosave
            // does not surface as an unhandled rejection with no context.
            saveAllDirty(true).catch(function () {});
        }, 2000);
    }

    // A FAILED SAVE MUST NOT LOOK LIKE A SUCCESSFUL ONE.
    //
    // This used to hand Promise.all a rejection handler, which turns a failed
    // write into a FULFILLED promise. Every caller then carried on: BUILD and
    // PUSH both begin `saveAllDirty().then(...)`, so a file that could not be
    // written - locked, read only, out of disk - produced a build of the older
    // contents on disk and a push of that build, reported as success, while the
    // editor still showed the text nobody had saved.
    //
    // Now it rejects. The message still says the text is safe in the editor,
    // because that is the reassuring and true part, and the callers below stop.
    function saveAllDirty(quiet) {
        // Same refusal as saveOne, made once and up front so the user gets one
        // sentence about the project rather than one failure per open file.
        // saveOne keeps its own copy: this is not the only way in.
        if (!writesAreAllowed()) {
            var held = new Error('nothing was written');
            held.held = true;
            held.why = 'the tool has not said which project it has open, so nothing was written. ' +
                       'Your text is still here in the editor.';
            if (!quiet) say(held.why, 'e');
            return Promise.reject(held);
        }
        var work = [], names = [];
        Object.keys(files).forEach(function (rel) {
            if (!files[rel].dirty) return;
            names.push(rel);
            work.push(saveOne(rel));
        });
        return Promise.all(work).then(function (results) {
            drawTabs();
            // A key added to strings.json has to be offered by completion
            // before the next line is typed, not after the next project open.
            if (names.some(function (rel) { return /strings\.json$/.test(rel); })) refreshStringKeys();
            if (names.length && !quiet) say('saved ' + names.join(', '), 'g');
            if (names.length && quiet) $('stSaved').textContent = new Date().toLocaleTimeString();

            // Anything superseded mid-flight is still unsaved. Dependent work
            // must not treat this as "everything is on disk".
            var pending = results.filter(function (r) { return r && r.superseded; });
            if (pending.length) {
                var e = new Error('still unsaved: ' + pending.map(function (r) { return r.rel; }).join(', '));
                e.why = 'you typed while saving, so the newest text is not on disk yet';
                e.superseded = true;
                throw e;
            }
            return results;
        }, function (e) {
            drawTabs();
            say('a file could not be saved: ' + (e && (e.why || e.message)) +
                '. Your text is still here in the editor, and nothing was built or sent.', 'e');
            if (e && typeof e === 'object') { e.saveFailed = true; }
            throw e;
        });
    }

    // ========================================================================
    // THE USER'S OWN AI, ATTACHED TO WHAT THEY RIGHT-CLICKED
    //
    // The tool ships no model and picks none. BF6Assist in the host holds the
    // provider (BF6_AI_ENDPOINT / BF6_AI_MODEL / BF6_AI_KEY) and posts the
    // request; this page builds the briefing and shows the conversation. The
    // key never comes to the page: this document is a file:// URL, and a key
    // that reached it would effectively be published.
    //
    // NOTHING IS APPLIED WITHOUT BEING ASKED FOR. An answer is a proposal. It
    // arrives as text with its code in a block, and the code goes into the file
    // only when somebody presses the button, as one undoable edit. A model that
    // silently rewrites a rule in an editor where a disabled block ran for
    // months is a bad trade.
    // ========================================================================
    var aiMode = 'explain';          // which half of the right column is showing
    var aiTurns = [];                // [{ q, what, a, ok, at, rel, range, waiting }]
    var aiProv = null;               // last assistStatus reply
    var askAnchor = null;            // what the question was about, captured when asked

    function showSide(mode) {
        aiMode = (mode === 'ai') ? 'ai' : 'explain';
        var g = $('gutterInner'), a = $('aiInner');
        if (g) { g.hidden = aiMode === 'ai'; }
        if (a) { a.hidden = aiMode !== 'ai'; }
        var be = $('sideExplain'), ba = $('sideAi');
        if (be) { be.classList.toggle('on', aiMode === 'explain'); }
        if (ba) { ba.classList.toggle('on', aiMode === 'ai'); }
        if (!guided) {
            // The column is hidden entirely; asking for either half means
            // wanting the column back.
            guided = true;
            $('btnGuided').classList.toggle('on', true);
            setPref('guided', true);
        }
        if (aiMode === 'ai') { drawAi(); } else { buildGutter(); }
    }

    // WHERE IT IS GOING, said plainly and never guessed at. The host decides
    // whether an endpoint is loopback; a page that decided for itself would be
    // promising something it cannot check.
    function assistStatus() {
        return call('assistStatus').then(function (r) {
            aiProv = r || {};
            var who = $('aiWho');
            if (who) {
                who.className = r && r.linked ? (r.local ? 'local' : '') : 'off';
                who.textContent = !r || !r.linked ? 'nothing linked'
                    : (r.local ? 'on this machine' : (r.model || 'linked'));
                who.title = !r || !r.linked
                    ? 'Set BF6_AI_ENDPOINT, BF6_AI_MODEL and BF6_AI_KEY, then restart the editor.'
                    : (r.local
                        ? 'A loopback endpoint: nothing you send leaves this machine.'
                        : 'This sends your briefing OFF this machine, to ' + (r.model || 'the linked model') + '.');
            }
            return aiProv;
        }, function () { aiProv = { linked: false }; return aiProv; });
    }

    // ---- what the AI is told ----------------------------------------------
    //
    // contextpack.js writes the spine of it: the rules that decide whether
    // Portal code works, the vocabulary, the project, the open file and the
    // selection. It is prose on purpose. Attaching catalog.json and the .d.ts
    // files would be half a megabyte of punctuation and would bury the six
    // facts that actually matter.
    //
    // Everything after that call is what this editor knows and the pack cannot:
    // the other files, the text the mod says, the settings on the Values list,
    // what the scene has selected, what Portal said about the last upload, the
    // game log, and the conversation so far. An answer written without those
    // is confident about the wrong project.
    //
    // Bounded on purpose. Each section has a cap and the whole thing is checked
    // before it is sent, because a briefing that does not fit is a request that
    // fails after the user has typed their question.
    var BRIEF_OTHER_FILE_KB = 12;      // per other file
    var BRIEF_OTHER_TOTAL_KB = 48;     // across all of them
    var BRIEF_HISTORY_TURNS = 6;

    function firstLine(s, n) {
        var t = String(s || '').replace(/\s+/g, ' ').trim();
        return t.length > n ? t.slice(0, n - 3) + '...' : t;
    }

    // The command list, from the best source loaded. api.json carries parameter
    // NAMES and a sentence of documentation for 415 commands; the symbol index
    // is the fallback and has neither.
    function briefCatalog() {
        var out = {};
        var names = Object.keys(API_FN || {});
        if (names.length) {
            names.forEach(function (n) {
                var f = API_FN[n];
                out[n] = { tip: (f.signature || n) + (f.doc ? '   // ' + firstLine(f.doc, 110) : '') };
            });
            return out;
        }
        Object.keys(SYM || {}).forEach(function (n) {
            var s = SYM[n];
            out[n] = { tip: n + (s && s.sig ? s.sig : '') };
        });
        return out;
    }

    // The events, in the shape contextpack prints them. Without this the
    // briefing said, in as many words, "(no event table was available)" - to a
    // model being asked to write event handlers.
    function briefEvents() {
        var names = Object.keys(API_EV || {});
        if (!names.length) { return null; }
        var out = {};
        names.forEach(function (n) {
            out[n] = { params: (API_EV[n].params || []).map(function (p) {
                return { name: p.name, type: p.type };
            }) };
        });
        return out;
    }

    function briefingFor(question, anchor) {
        if (typeof BF6ContextPack === 'undefined') { return question; }
        var problems = [];
        try {
            (monaco.editor.getModelMarkers({}) || []).slice(0, 40).forEach(function (m) {
                problems.push({ message: m.message, line: m.startLineNumber,
                                file: m.resource ? String(m.resource.path).split('/').pop() : '' });
            });
        } catch (e) { /* markers are a nicety, not a requirement */ }

        var brief = BF6ContextPack.build({
            where: 'script',
            target: 'typescript',
            catalog: briefCatalog(),
            events: briefEvents(),
            project: project ? {
                name: project.name || project.path,
                files: Object.keys(files || {})
            } : null,
            openFile: anchor && anchor.fileText
                ? { path: anchor.rel, text: anchor.fileText } : null,
            selection: anchor && anchor.code ? anchor.code : '',
            diagnostics: problems,
            question: question
        });

        var L = [];

        // ---- what kind of project this is -------------------------------
        //
        // The single most important fact about an imported experience is that
        // dist/bundle.ts is generated. Advice to edit it is advice to lose the
        // change at the next build, and a model has no way to know that from
        // the code in front of it.
        var all = Object.keys(lastFiles || {});
        if (!all.length) { all = Object.keys(files || {}); }
        var hasBundle = all.indexOf('dist/bundle.ts') >= 0;
        // THE SCAFFOLD IS NOT SOURCE. A template project always leaves
        // src/index.ts and src/helpers/index.ts behind, so "are there files
        // under src" is true for an experience that arrived as one built file
        // and has nothing of the author's in src at all. Counting those two as
        // the author's work is the same mistake that kept Quick Values empty on
        // exactly the projects that needed it.
        var SCAFFOLD = { 'src/index.ts': 1, 'src/helpers/index.ts': 1 };
        var ownSrc = all.filter(function (f) {
            return /^src\/.+\.ts$/.test(f) && !SCAFFOLD[f] && f.indexOf('/debug-tool/') < 0;
        });
        L.push('## How this project is put together');
        L.push('');
        L.push('- It is a Portal TypeScript project built from the DeLuca template and bundled to');
        L.push('  dist/bundle.ts, which is what Portal is given.');
        // A FILE LIST IS EVIDENCE, NOT A VERDICT.
        //
        // "No files in src except the two a scaffold leaves" usually means an
        // imported bundle. It can also mean a small mod written entirely in
        // src/index.ts, and telling that author their source is a scaffold
        // would send them to edit generated output. So where the index is open
        // and has real content in it, that settles it; where it is not, the
        // evidence is stated and the conclusion is not.
        var indexText = files['src/index.ts'] ? files['src/index.ts'].model.getValue() : null;
        var indexIsScaffold = indexText === null ? null : indexText.length < 2048;
        if (hasBundle && ownSrc.length === 0 && indexIsScaffold !== false) {
            if (indexIsScaffold === true) {
                L.push('- IT WAS IMPORTED AS A BUILT FILE. src/index.ts is the template scaffold and');
                L.push('  nothing else of the author\'s is in src/, so dist/bundle.ts IS the mod here');
                L.push('  and editing it is the right thing to do. Do not suggest editing src/: a');
                L.push('  rebuild from that scaffold would replace the mod with an empty template.');
            } else {
                L.push('- src/ holds only the two files a template scaffold leaves behind, and there is');
                L.push('  a dist/bundle.ts. That usually means the experience was imported as one');
                L.push('  built file, in which case dist/bundle.ts IS the mod and is the right thing');
                L.push('  to edit. If src/index.ts turns out to hold the author\'s own code, say so');
                L.push('  and change that instead. Do not assume; the file list cannot tell you.');
            }
        } else if (hasBundle) {
            L.push('- There is source in src/ AND a built dist/bundle.ts. Change the source: the');
            L.push('  built file is regenerated by the next build and any edit to it is lost.');
        }
        var dirty = Object.keys(files || {}).filter(function (r) { return files[r].dirty; });
        if (dirty.length) {
            L.push('- Unsaved right now: ' + dirty.join(', ') + '. What you are shown is the text in');
            L.push('  the editor, which is ahead of what is on disk.');
        }
        if (typeof showTemplateCode !== 'undefined' && !showTemplateCode) {
            L.push('- The template\'s own debug code is folded out of sight in the editor. It is');
            L.push('  still in the file you are shown, and it is not the author\'s code.');
        }
        L.push('');

        // ---- the rest of the mod ----------------------------------------
        var budget = BRIEF_OTHER_TOTAL_KB * 1024;
        var others = Object.keys(files || {}).filter(function (r) {
            return r !== (anchor && anchor.rel);
        });
        var shown = [];
        others.forEach(function (rel) {
            if (budget <= 0) { return; }
            var text = '';
            try { text = files[rel].model.getValue(); } catch (e) { return; }
            var cap = Math.min(BRIEF_OTHER_FILE_KB * 1024, budget);
            var clipped = text.length > cap;
            text = text.slice(0, cap);
            budget -= text.length;
            shown.push({ rel: rel, text: text, clipped: clipped });
        });
        if (shown.length) {
            L.push('## The other files open in this project');
            L.push('');
            shown.forEach(function (f) {
                L.push('### ' + f.rel + (f.clipped ? '  (first part only)' : ''));
                L.push('');
                L.push('```ts');
                L.push(f.text);
                L.push('```');
                L.push('');
            });
        }
        var unopened = all.filter(function (r) { return !files[r]; });
        if (unopened.length) {
            L.push('Other files in the project, not open and not shown: ' + unopened.join(', '));
            L.push('');
        }

        // ---- what the mod says to a player ------------------------------
        if (strTree) {
            var rows = strFlatten(strTree, '', []).slice(0, 60);
            if (rows.length) {
                L.push('## The text this mod shows players (src/strings.json)');
                L.push('');
                rows.forEach(function (r) { L.push('- ' + r.path + ': ' + firstLine(r.text, 120)); });
                L.push('');
            }
        }

        // ---- the settings the author pulled out --------------------------
        if (quickList && quickList.length) {
            L.push('## Settings the author put on the Values list');
            L.push('');
            L.push('These are the numbers, switches and sounds they change often. Changing one of');
            L.push('these is usually a better answer than changing the code around it.');
            L.push('');
            quickList.slice(0, 40).forEach(function (q) {
                var cur = quickValues ? quickValues[quickKey(q)] : null;
                L.push('- ' + q.name + ' (' + q.kind + ') in ' + q.file
                       + (cur == null ? '' : ' = ' + firstLine(cur, 60)));
            });
            L.push('');
        }

        // ---- the scene next door ----------------------------------------
        //
        // The editor and the map are the same session. An object selected in
        // the viewport is very often what the question is about, and its id is
        // the thing the script needs to name it.
        if (selectedObj) {
            L.push('## Selected in the Unreal scene right now');
            L.push('');
            L.push('- ' + (selectedObj.label || 'an object') + ', object id ' + selectedObj.objid
                   + (selectedObj.kind ? ', kind ' + selectedObj.kind : ''));
            var getter = (typeof GETTER_FOR !== 'undefined' && GETTER_FOR[selectedObj.kind])
                ? GETTER_FOR[selectedObj.kind] : 'GetSpatialObject';
            L.push('- Reached from script as: mod.' + getter + '(' + selectedObj.objid + ')');
            L.push('');
        }

        // ---- what Portal itself said -------------------------------------
        //
        // The site type-checks every upload, so its refusal is a second opinion
        // from the only compiler that counts.
        if (portalVerdict && portalVerdict.ok === false) {
            L.push('## What Portal said when this was last uploaded');
            L.push('');
            (portalVerdict.mapped || []).slice(0, 20).forEach(function (m) {
                L.push('- ' + (m.rel || '') + ':' + (m.line || '?') + '  ' + m.message);
            });
            (portalVerdict.unmapped || []).slice(0, 10).forEach(function (m) {
                L.push('- ' + (m.message || String(m)));
            });
            L.push('');
        }

        // ---- what the game itself said -----------------------------------
        //
        // The type checker's problems are above; this is the other half, and it
        // is the half that explains a mod which compiles and then misbehaves in
        // a match.
        if (typeof logLines !== 'undefined' && logLines && logLines.length) {
            var tail = logLines.slice(-40).map(function (e) { return e.raw; }).join('\n');
            L.push('## The game log (PortalLog), oldest first');
            L.push('');
            L.push('```');
            L.push(tail.slice(0, 8000));
            L.push('```');
            L.push('');
        }

        // ---- what has already been said ----------------------------------
        //
        // Without this every question was the first question. "Now make it
        // faster" had nothing to make faster, and the same ground was covered
        // again with slightly different words each time.
        // Only this project's conversation. The turns live in one array so the
        // history survives switching away and back, but a briefing that carried
        // another experience's questions into this one would be describing a
        // mod that is not open.
        var here = project && project.path;
        var past = (aiTurns || []).filter(function (t) {
            return !t.waiting && t.a && (!t.proj || t.proj === here);
        });
        if (past.length) {
            L.push('## This conversation so far, oldest first');
            L.push('');
            past.slice(-BRIEF_HISTORY_TURNS).forEach(function (t) {
                L.push('THEY ASKED: ' + firstLine(t.q, 400));
                L.push('YOU ANSWERED: ' + String(t.a).slice(0, 1500)
                       + (String(t.a).length > 1500 ? '\n(answer clipped)' : ''));
                L.push('');
            });
            L.push('The question below is the newest one. Do not repeat an answer already given;');
            L.push('build on it.');
            L.push('');
        }

        return brief + '\n' + L.join('\n');
    }


    // What the question carries with it, decided when it is asked rather than
    // when it is sent, so the answer is about the code they right-clicked.
    function captureAnchor() {
        // WHICH PROJECT ASKED. Two projects can hold the same relative path with
        // the same lines in it - a template scaffold guarantees they do - so
        // without this an answer about A applied cleanly to B, and A's
        // conversation went along with B's next question.
        var a = { rel: activeRel || '', code: '', range: null, fileText: '', lines: 0,
                  proj: project && project.path, gen: project && project.gen };
        if (!editor || !editor.getModel()) { return a; }
        var model = editor.getModel();
        var sel = editor.getSelection();
        a.fileText = model.getValue();
        if (sel && !sel.isEmpty()) {
            a.code = model.getValueInRange(sel);
            a.range = {
                startLineNumber: sel.startLineNumber, startColumn: sel.startColumn,
                endLineNumber: sel.endLineNumber, endColumn: sel.endColumn
            };
            a.lines = sel.endLineNumber - sel.startLineNumber + 1;
        }
        return a;
    }

    function openAsk(seed) {
        var box = $('askBox');
        if (!box) { return; }
        askAnchor = captureAnchor();
        box.hidden = false;

        var what = $('askWhat');
        if (what) {
            // MEASURED, NOT DESCRIBED. The briefing is built here anyway to be
            // sized, so the number on screen is the real one rather than an
            // estimate of the open file that ignored everything else going
            // with it.
            var bits = [];
            if (askAnchor.rel) { bits.push(askAnchor.rel); }
            bits.push(askAnchor.lines
                ? askAnchor.lines + ' selected line' + (askAnchor.lines === 1 ? '' : 's')
                : 'the whole open file');
            var openCount = Object.keys(files || {}).length;
            if (openCount > 1) { bits.push(openCount - 1 + ' other open file'
                + (openCount === 2 ? '' : 's')); }
            if (strTree) { bits.push('the text your mod shows'); }
            if (quickList && quickList.length) { bits.push('your Values list'); }
            if (selectedObj) { bits.push('what is selected in the scene'); }
            if (typeof logLines !== 'undefined' && logLines.length) { bits.push('the game log'); }
            var kb = 0;
            try { kb = Math.round(briefingFor('', askAnchor).length / 1024); } catch (e) { kb = 0; }
            what.textContent = 'Going with your question: ' + bits.join(', ')
                + ', the Portal rules, every command and event. About ' + kb + ' KB in all.';
        }

        var note = $('askNote');
        if (note) {
            if (!aiProv || !aiProv.linked) {
                note.className = 'askNote warn';
                note.textContent = 'Nothing is linked yet. Copy the briefing works anyway.';
            } else if (aiProv.local) {
                note.className = 'askNote';
                note.textContent = 'Staying on this machine.';
            } else {
                note.className = 'askNote warn';
                note.textContent = 'This leaves your machine.';
            }
        }

        var chips = $('askChips');
        if (chips) {
            chips.innerHTML = '';
            var chipText = [
                'What does this do, and what calls it?',
                'Is there a bug in this?',
                'Rewrite this to be simpler, same behaviour.',
                'Why is the tool reporting these problems?'
            ];
            // Only offered when there is a log to read, so it never points at
            // something that is not there.
            if (typeof logLines !== 'undefined' && logLines && logLines.length) {
                chipText.push('What went wrong in the game log?');
            }
            chipText.forEach(function (t) {
                var b = el('button', null, t.length > 34 ? t.slice(0, 32) + '...' : t);
                b.title = t;
                b.onclick = function () { $('askText').value = t; $('askText').focus(); };
                chips.appendChild(b);
            });
        }

        var ta = $('askText');
        if (ta) { ta.value = seed || ''; ta.focus(); ta.select(); }
        assistStatus();
    }

    function closeAsk() {
        var box = $('askBox');
        if (box) { box.hidden = true; }
    }

    function askSend() {
        var ta = $('askText');
        var q = ta ? ta.value.trim() : '';
        if (!q) { say('type what you want it to do first', 'w'); return; }
        var anchor = askAnchor || captureAnchor();
        closeAsk();
        showSide('ai');

        var turn = { q: q, rel: anchor.rel, lines: anchor.lines, range: anchor.range,
                     code: anchor.code, waiting: true, a: '', ok: false,
                     proj: anchor.proj, gen: anchor.gen };
        aiTurns.push(turn);
        drawAi();

        call('assistAsk', { briefing: briefingFor(q, anchor), question: q }).then(function (r) {
            turn.waiting = false;
            turn.ok = true;
            turn.a = (r && r.text) || '';
            drawAi();
        }, function (e) {
            turn.waiting = false;
            turn.ok = false;
            turn.a = (e && (e.why || e.text)) || 'the request did not come back';
            drawAi();
        });
    }

    // Fenced blocks are the part that can be applied; everything else is the
    // explanation and stays text.
    function splitAnswer(text) {
        var parts = [];
        var s = String(text || '');
        var re = /```[a-zA-Z]*\n([\s\S]*?)```/g;
        var at = 0, m;
        while ((m = re.exec(s)) !== null) {
            if (m.index > at) { parts.push({ code: false, text: s.slice(at, m.index) }); }
            parts.push({ code: true, text: m[1] });
            at = m.index + m[0].length;
        }
        if (at < s.length) { parts.push({ code: false, text: s.slice(at) }); }
        return parts;
    }

    // PUTTING AN ANSWER IN THE FILE.
    //
    // Into the exact range the question was asked about, but only while that
    // range still holds what was sent. Files move under you - a save, a
    // re-indent, another edit - and an offset applied to changed text lands in
    // the middle of something else.
    function applyCode(turn, code) {
        if (!editor || !editor.getModel()) { say('open a file first', 'w'); return; }
        // THE PROJECT IS PART OF THE ADDRESS. A relative path is not an
        // identity: src/index.ts exists in every project made from the
        // template, with the same lines in it, so the file check alone let an
        // answer about one experience be applied to another.
        if (turn.proj && turn.proj !== (project && project.path)) {
            say('that answer was about a different project. Open it again to apply this.', 'w');
            return;
        }
        if (turn.gen !== undefined && turn.gen !== (project && project.gen)) {
            say('that project has been reopened since you asked, so this answer was not applied. '
                + 'Ask again to be sure it still fits.', 'w');
            return;
        }
        if (activeRel !== turn.rel) {
            say('that answer was about ' + turn.rel + '. Open it first, then apply.', 'w');
            return;
        }
        var model = editor.getModel();
        var r = turn.range;
        if (r) {
            var now = model.getValueInRange(new monaco.Range(
                r.startLineNumber, r.startColumn, r.endLineNumber, r.endColumn));
            if (now !== turn.code) {
                say('those lines have changed since you asked, so nothing was replaced. '
                    + 'Select where it should go and press "Put it at the cursor".', 'w');
                return;
            }
            editor.executeEdits('bf6-ai', [{
                range: new monaco.Range(r.startLineNumber, r.startColumn, r.endLineNumber, r.endColumn),
                text: code,
                forceMoveMarkers: true
            }]);
        } else {
            var p = editor.getPosition();
            editor.executeEdits('bf6-ai', [{
                range: new monaco.Range(p.lineNumber, p.column, p.lineNumber, p.column),
                text: code,
                forceMoveMarkers: true
            }]);
        }
        editor.focus();
        say('put it in ' + turn.rel + '. Ctrl+Z takes it straight back out.', 'o');
    }

    // This project's conversation only. Switching project shows that
    // project's turns; switching back brings these ones with it.
    function turnsHere() {
        var here = project && project.path;
        return (aiTurns || []).filter(function (t) { return !t.proj || t.proj === here; });
    }

    function drawAi() {
        var host = $('aiInner');
        if (!host) { return; }
        host.innerHTML = '';
        var mine = turnsHere();

        if (!mine.length) {
            if (aiProv && aiProv.linked) {
                host.appendChild(el('div', 'aiWait',
                    'Select some code, right-click, and choose "Ask the AI about this". '
                    + 'What you ask and what comes back both stay here.'));
                var ask1 = el('button', 'exLink', 'Ask something now');
                ask1.onclick = function () { openAsk(''); };
                host.appendChild(ask1);
                return;
            }
            // HOW TO ATTACH ONE, in the panel rather than in a manual nobody
            // has open. The words come from the host, so they cannot drift out
            // of step with what the commands are actually called.
            host.appendChild(el('div', 'aiWait',
                'No AI is attached yet. It is your own: this tool ships no model and picks none, '
                + 'and your key is never stored by it, never logged, and never given to this page.'));
            var steps = el('pre', 'aiHelp',
                (aiProv && aiProv.help) ? aiProv.help
                : 'Type BF6.Assist.Setup in the Unreal console for the steps.');
            host.appendChild(steps);
            var copyCmd = el('button', 'exLink', 'Copy these steps');
            copyCmd.onclick = function () {
                copyText(steps.textContent);
                say('copied. Paste them into the Unreal console one at a time.', 'o');
            };
            host.appendChild(copyCmd);
            host.appendChild(el('div', 'aiWait',
                'Everything here works without one: "Copy the briefing" puts the whole question, '
                + 'with your project, on the clipboard for whatever you already talk to.'));
            var ask2 = el('button', 'exLink', 'Write a question and copy the briefing');
            ask2.onclick = function () { openAsk(''); };
            host.appendChild(ask2);
            return;
        }

        mine.forEach(function (t) {
            var box = el('div', 'aiTurn');
            box.appendChild(el('div', 'aiMeta',
                (t.rel || 'no file') + (t.lines ? ', ' + t.lines + ' line'
                    + (t.lines === 1 ? '' : 's') + ' selected' : '')));
            box.appendChild(el('div', 'aiQ', t.q));

            if (t.waiting) {
                box.appendChild(el('div', 'aiWait', 'asking...'));
                host.appendChild(box);
                return;
            }
            if (!t.ok) {
                box.appendChild(el('div', 'aiA bad', t.a));
                host.appendChild(box);
                return;
            }
            splitAnswer(t.a).forEach(function (part) {
                if (!part.code) {
                    if (part.text.trim()) { box.appendChild(el('div', 'aiA', part.text.trim())); }
                    return;
                }
                box.appendChild(el('div', 'aiCode', part.text));
                var acts = el('div', 'aiActs');
                if (t.range) {
                    var rep = el('button', null, 'Replace what I selected');
                    rep.onclick = function () { applyCode(t, part.text.replace(/\n$/, '')); };
                    acts.appendChild(rep);
                }
                var ins = el('button', null, 'Put it at the cursor');
                ins.onclick = function () {
                    var keep = t.range; t.range = null;
                    applyCode(t, part.text.replace(/\n$/, ''));
                    t.range = keep;
                };
                acts.appendChild(ins);
                var cp = el('button', null, 'Copy');
                cp.onclick = function () { copyText(part.text); say('copied', 'o'); };
                acts.appendChild(cp);
                box.appendChild(acts);
            });
            host.appendChild(box);
        });

        var again = el('button', 'exLink', 'Ask something else');
        again.onclick = function () { openAsk(''); };
        host.appendChild(again);
    }

    function copyText(t) {
        try { navigator.clipboard.writeText(t); }
        catch (e) { say('could not reach the clipboard', 'w'); }
    }


    // ========================================================================
    // THE EXPLANATION PANEL
    //
    // Plain language about the one place the caret is: what this line does,
    // what it is part of, and what the name under the caret is - where it is
    // set, what it holds, and where else it is used. It follows the caret
    // rather than the lines, so nothing in it can fall out of line with the
    // code, however the code is folded, hidden or re-indented.
    // ========================================================================
    var guided = true;
    var gutterTimer = null;

    function scheduleGutter() {
        if (gutterTimer) clearTimeout(gutterTimer);
        gutterTimer = setTimeout(buildGutter, 120);
    }

    // What a line is ABOUT, in one sentence, or null if it needs no comment.
    // Order matters: the most specific reading wins.
    function explainLine(text, rel) {
        var t = text.trim();
        if (!t || t === '}' || t === '};' || t === ')' || t === ');' || t === '{') return null;
        if (t.charAt(0) === '/' || t.charAt(0) === '*') return null;   // a comment explains itself

        // 1. a recipe or snippet put a note on this exact line
        // (handled by the caller through NOTE_OVERRIDES)

        // 2. an import of a utils module
        var im = t.match(/from\s+['"]bf6-portal-utils\/([\w-]+)/);
        if (im && G.utils[im[1]]) {
            var u = G.utils[im[1]];
            return { text: u.say, link: u.link, label: 'Read the module docs' };
        }
        if (/from\s+['"]bf6-portal-mod-types/.test(t)) {
            return { text: 'Brings in the type descriptions for the mod namespace. They are what makes autocomplete know every Portal call.' };
        }
        if (/^import\s/.test(t)) {
            var rel2 = t.match(/from\s+['"]\.{1,2}\/([^'"]+)/);
            if (rel2) return { text: 'Brings in your own file ' + rel2[1] + '. The path is relative to this file and keeps the .ts on the end.' };
            return { text: G.ts['import'].say };
        }

        // 3. an event subscription
        var sub = t.match(/Events\.(\w+)\.subscribe/);
        if (sub) return { text: eventSentence(sub[1]) };
        var exp = t.match(/^export\s+(?:async\s+)?function\s+(On\w+|Ongoing\w+)/);
        if (exp) return { text: eventSentence(exp[1]) + ' Portal calls this by name because it is exported, so the name has to be exactly right.' };

        // 4. a mod.* call
        var mods = t.match(/mod\.([A-Za-z_$][\w$]*)/g);
        if (mods && mods.length) {
            // the outermost call is the one the line is really doing
            var name = pickPrimaryMod(t, mods);
            if (name) {
                var g = G.mod[name];
                var sym = SYM[name];
                var sentence = g ? g.say : (sym && sym.doc ? firstSentence(sym.doc) : null);
                if (sentence) {
                    return { text: sentence, warn: g && g.warn, name: name };
                }
            }
        }

        // 5. utils usage
        var uu = t.match(/\b(Timers|Events|UI|Logger|MapDetector|MultiClickDetector|Vectors|Sounds|Logging)\b/);
        if (uu) {
            var key = { Timers: 'timers', Events: 'events', UI: 'ui', Logger: 'logger', MapDetector: 'map-detector', MultiClickDetector: 'multi-click-detector', Vectors: 'vectors', Sounds: 'sounds', Logging: 'logging' }[uu[1]];
            if (G.utils[key]) return { text: G.utils[key].say, link: G.utils[key].link, label: 'Read the module docs' };
        }

        // 6. the template's own boilerplate
        for (var frag in G.boilerplate) {
            if (t.indexOf(frag) !== -1) return { text: G.boilerplate[frag] };
        }

        // 7. plain TypeScript
        return tsSentence(t);
    }

    function pickPrimaryMod(line, mods) {
        // The call whose open bracket is furthest LEFT is the one doing the work;
        // the rest are arguments to it.
        var best = null, bestAt = 1e9;
        for (var i = 0; i < mods.length; i++) {
            var nm = mods[i].slice(4);
            var at = line.indexOf(mods[i]);
            if (at < bestAt && (G.mod[nm] || SYM[nm])) { best = nm; bestAt = at; }
        }
        return best;
    }

    function firstSentence(s) {
        var m = s.match(/^(.{10,220}?[.!?])(\s|$)/);
        return m ? m[1] : s.slice(0, 200);
    }

    function eventSentence(name) {
        var known = {
            OnPlayerDeployed: 'Runs each time a player spawns into the world.',
            OnPlayerJoinGame: 'Runs once as each player arrives in the match.',
            OnPlayerLeaveGame: 'Runs as each player leaves. This is where per player cleanup belongs.',
            OnPlayerDied: 'Runs when a player dies, and carries who killed them and how.',
            OnPlayerDamaged: 'Runs on every hit. Keep the handler synchronous and do almost nothing in it.',
            OnGameModeStarted: 'Runs once when the round starts.',
            OnGameModeEnding: 'Runs as the round is ending.',
            OnPlayerEnterArea: 'Runs when a player crosses into an area trigger you placed on the map.',
            OnPlayerExitArea: 'Runs when a player leaves an area trigger.',
            OnCapturePointCaptured: 'Runs when a capture point changes hands.',
            OnCapturePointCapturing: 'Runs while a capture point is being taken.',
            OnVehicleSpawned: 'Runs when any vehicle appears.',
            OnVehicleDestroyed: 'Runs when any vehicle is destroyed.',
            OnSpawnerSpawned: 'Runs when a spawner produces something. The thing it made is not ready to command until the next tick.',
            OnPlayerInteract: 'Runs when a player holds the interact key on an interact point.',
            OnPlayerUIButtonEvent: 'Runs when a player presses one of your UI buttons.',
            OngoingPlayer: 'Runs about thirty times a second FOR EVERY PLAYER. Keep it tiny.',
            OngoingGlobal: 'Runs about thirty times a second. Setup does not belong here; use OnGameModeStarted.'
        };
        if (known[name]) return known[name];
        if (/^Ongoing/.test(name)) return 'Runs about thirty times a second. Keep whatever is inside it very small.';
        return 'Registers what happens on the ' + name.replace(/^On/, '') + ' event.';
    }

    function tsSentence(t) {
        if (/^(export\s+)?(async\s+)?function\s/.test(t)) {
            var isAsync = /\basync\b/.test(t);
            return { text: 'Declares a function' + (isAsync ? ' that can pause partway through. async means it hands back a promise, and only an async function may use await.' : '. It runs when something calls it, not where it is written.') };
        }
        if (/^\s*(const|let)\s+\w+\s*=\s*\[/.test(t)) return { text: 'A list of values. Items are numbered from zero.' };
        if (/^\s*(const|let)\s/.test(t)) {
            return { text: /^const/.test(t) ? 'Names a value that will not be pointed at anything else. Prefer const over let.' : 'Names a value you intend to change later.' };
        }
        if (/^\s*(if|else if)\s*\(/.test(t)) {
            if (/return\s*;?\s*$/.test(t)) return { text: 'An early exit: when this is true, the rest of the function is skipped. Guards like this are how you keep the real work unindented.' };
            return { text: G.ts['if'].say };
        }
        if (/^\s*for\s*\(/.test(t)) return { text: G.ts['for'].say };
        if (/^\s*while\s*\(/.test(t)) return { text: G.ts['while'].say };
        if (/^\s*return\b/.test(t)) return { text: G.ts['return'].say };
        if (/\bawait\b/.test(t)) return { text: G.ts['await'].say };
        if (/=>/.test(t)) return { text: G.ts['=>'].say };
        if (/^\s*console\.log/.test(t)) return { text: 'Writes a line to PortalLog on disk while you host locally. The DIAGNOSE panel reads that file.' };
        if (/^\s*(export\s+)?(interface|type)\s/.test(t)) return { text: G.ts['interface'].say };
        if (/^\s*(export\s+)?class\s/.test(t)) return { text: G.ts['class'].say };
        return null;
    }

    var NOTE_OVERRIDES = {};   // rel -> { lineNumber -> note }

    // WHAT AM I LOOKING AT.
    //
    // This panel used to print one note per line, pinned to that line's Y.
    // On a file somebody wrote by hand that reads well. On an imported bundle
    // of eight thousand lines it is thousands of notes about machinery nobody
    // wrote, and the moment a region is folded or hidden every note is beside
    // the wrong line.
    //
    // So it answers one question instead, about the one place the caret is:
    // what this line is doing, what it belongs to, and what the name under the
    // caret actually is - where it is set, what it holds now, and everywhere
    // else it is used. Nothing is positioned against a line, so nothing can
    // drift out of line.

    // What encloses this line: the handler, function or class it is inside.
    // Depth is counted with the same string- and comment-aware scanner the
    // re-indenter uses, so a brace inside a string does not invent a scope.
    function ownerAt(lines, upto) {
        var stack = [];
        var depth = 0, inBlock = false, inTemplate = false;
        for (var i = 0; i < upto && i < lines.length; i++) {
            var line = lines[i];
            var here = null;
            if (!inBlock && !inTemplate) {
                var m = line.match(/(?:export\s+)?(?:async\s+)?function\s+([A-Za-z_$][\w$]*)/);
                if (m) { here = { name: m[1], kind: /^(On|Ongoing)/.test(m[1]) ? 'handler' : 'function' }; }
                if (!here) {
                    var s = line.match(/Events\.(\w+)\.subscribe/);
                    if (s) { here = { name: s[1], kind: 'handler' }; }
                }
                if (!here) {
                    var c = line.match(/(?:export\s+)?class\s+([A-Za-z_$][\w$]*)/);
                    if (c) { here = { name: c[1], kind: 'class' }; }
                }
            }
            var r = scanLine(line, inBlock, inTemplate);
            inBlock = r.inBlock; inTemplate = r.inTemplate;
            var before = depth;
            depth = Math.max(0, depth + r.delta);
            if (here && depth > before) { stack.push({ at: before, own: here }); }
            while (stack.length && depth <= stack[stack.length - 1].at) { stack.pop(); }
        }
        return stack.length ? stack[stack.length - 1].own : null;
    }

    // Where a name is set, if it is set in this file at all.
    function declarationOf(lines, name) {
        var esc2 = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        var pats = [
            { re: new RegExp('\\b(?:const|let|var)\\s+' + esc2 + '\\b'), kind: 'value' },
            { re: new RegExp('\\bfunction\\s+' + esc2 + '\\b'), kind: 'function' },
            { re: new RegExp('\\bclass\\s+' + esc2 + '\\b'), kind: 'class' },
            { re: new RegExp('^\\s*' + esc2 + '\\s*:(?!:)'), kind: 'setting' }
        ];
        for (var i = 0; i < lines.length; i++) {
            for (var p = 0; p < pats.length; p++) {
                if (pats[p].re.test(lines[i])) { return { line: i + 1, kind: pats[p].kind }; }
            }
        }
        return null;
    }

    // Every line that mentions the name, the declaration excluded.
    function usesOf(lines, name, skipLine, cap) {
        var re = new RegExp('\\b' + name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\b');
        var rows = [], total = 0;
        for (var i = 0; i < lines.length; i++) {
            if (i + 1 === skipLine) { continue; }
            if (!re.test(lines[i])) { continue; }
            total++;
            if (rows.length < (cap || 6)) { rows.push({ line: i + 1, text: lines[i].trim() }); }
        }
        return { rows: rows, total: total };
    }

    // The word the caret is in, or the selection when it is one plain name.
    function caretName(model, sel, pos) {
        if (sel && !sel.isEmpty()) {
            var picked = model.getValueInRange(sel).trim();
            return /^[A-Za-z_$][\w$]*$/.test(picked) ? picked : '';
        }
        var w = pos ? model.getWordAtPosition(pos) : null;
        return w ? w.word : '';
    }

    function buildGutter() {
        var inner = $('gutterInner');
        if (!inner) return;
        inner.innerHTML = '';
        $('gutter').className = guided ? 'on' : '';
        if (!guided || !editor || !editor.getModel()) return;

        var model = editor.getModel();
        var pos = editor.getPosition();
        var sel = editor.getSelection();
        if (!pos) { return; }

        var ln = pos.lineNumber;
        var lines = model.getValue().split('\n');
        var raw = model.getLineContent(ln);

        // ---- the line itself -------------------------------------------
        var over = (NOTE_OVERRIDES[activeRel] || {})[ln];
        var note = over ? { text: over } : explainLine(raw, activeRel);

        var head = el('div', 'exHead');
        head.appendChild(el('span', 'exWhere', 'line ' + ln + (activeRel ? ' of ' + activeRel : '')));
        inner.appendChild(head);

        var codeRow = el('div', 'exCodeLine', raw.trim().slice(0, 160) || '(a blank line)');
        inner.appendChild(codeRow);

        var body = el('div', 'exCard');
        if (note && note.text) {
            var p = el('div', 'exSay');
            var html = esc(note.warn ? note.warn : note.text);
            if (note.name) { html = '<b>' + esc(note.name) + '</b> ' + html; }
            if (note.warn) { html = '<b>Careful.</b> ' + html; }
            if (note.link) { html += ' <a href="#" data-url="' + esc(note.link) + '">' + esc(note.label || 'More') + '</a>'; }
            p.innerHTML = html;
            if (note.warn) { p.className = 'exSay warn'; }
            body.appendChild(p);
        } else {
            body.appendChild(el('div', 'exSay dim',
                'Nothing special happens on this line. Move the caret onto a name, a call or a statement.'));
        }
        inner.appendChild(body);

        // ---- what it belongs to ----------------------------------------
        var own = ownerAt(lines, ln - 1);
        if (own) {
            var ob = el('div', 'exPart');
            ob.appendChild(el('div', 'exLabel', 'Part of'));
            var line1 = el('div', 'exSay');
            line1.innerHTML = '<b>' + esc(own.name) + '</b> ' +
                esc(own.kind === 'handler'
                    ? eventSentence(own.name)
                    : own.kind === 'class'
                        ? 'a class: the code and the values that belong to one thing.'
                        : 'a function of yours. It runs when something calls it, not where it is written.');
            ob.appendChild(line1);
            var od = declarationOf(lines, own.name);
            if (od) { ob.appendChild(jumpButton('Go to it', od.line)); }
            inner.appendChild(ob);
        }

        // ---- the name under the caret ----------------------------------
        var name = caretName(model, sel, pos);
        if (name && !/^(const|let|var|function|class|return|if|else|for|while|await|async|import|export|new|this|true|false|null|undefined)$/.test(name)) {
            var np = el('div', 'exPart');
            np.appendChild(el('div', 'exLabel', 'The name here'));
            np.appendChild(el('div', 'exName', name));

            var whole = lines.join('\n');
            var decl = declarationOf(lines, name);
            var val = null;
            try { val = quickReadFrom(whole, name); } catch (e) { val = null; }

            var says = '';
            if (name.indexOf('mod.') === 0 || (G.mod && G.mod[name])) {
                var g = G.mod[name.replace(/^mod\./, '')] || G.mod[name];
                says = g ? g.say : 'Something the game itself provides. Portal calls live on the mod namespace.';
            } else if (/^(SFX_|VFX_|FX_)/.test(name)) {
                says = 'The name of a sound or effect the game ships. The Values tab can swap it for another one and preview it first.';
            } else if (decl && decl.kind === 'function') {
                says = 'A function declared in this file.';
            } else if (decl && decl.kind === 'class') {
                says = 'A class declared in this file.';
            } else if (decl && val) {
                says = 'A ' + (quickKindOf(val.literal) || 'value') + ' this mod sets, currently ' + clip(val.literal, 60) + '.';
            } else if (decl) {
                says = decl.kind === 'setting'
                    ? 'A setting inside an object of settings.'
                    : 'A value this file names.';
            } else {
                says = 'Not declared in this file, so it comes in from somewhere else: an import, a parameter, or the game.';
            }
            np.appendChild(el('div', 'exSay', says));

            if (decl) {
                var dr = el('div', 'exSay dim');
                dr.textContent = 'Set on line ' + decl.line + '.';
                dr.appendChild(jumpButton('Show me', decl.line));
                np.appendChild(dr);
            }

            var u = usesOf(lines, name, decl ? decl.line : 0, 6);
            np.appendChild(el('div', 'exSay dim', u.total === 0
                ? 'Used nowhere else in this file.'
                : 'Used on ' + u.total + ' other line' + (u.total === 1 ? '' : 's') + ' here:'));
            u.rows.forEach(function (x) {
                var b = el('button', 'exUse');
                b.innerHTML = '<span class="exLn">' + x.line + '</span><span class="exSnip">' +
                    esc(clip(x.text, 70)) + '</span>';
                b.onclick = function () { goToLine(x.line); };
                np.appendChild(b);
            });

            var all = el('button', 'exLink', 'Find it everywhere in this project');
            all.onclick = function () {
                var box = $('findAll');
                if (box) { box.value = name; }
                findAllRun(name);
            };
            np.appendChild(all);
            inner.appendChild(np);
        }

        // ---- a real selection: what the whole of it does ----------------
        if (sel && !sel.isEmpty() && sel.endLineNumber > sel.startLineNumber) {
            var sp = el('div', 'exPart');
            var n = sel.endLineNumber - sel.startLineNumber + 1;
            sp.appendChild(el('div', 'exLabel', 'The ' + n + ' lines you selected'));
            var said = {}, shown = 0;
            for (var i = sel.startLineNumber; i <= sel.endLineNumber && shown < 8; i++) {
                var sn = explainLine(model.getLineContent(i), activeRel);
                if (!sn || !sn.text || said[sn.text]) { continue; }
                said[sn.text] = true; shown++;
                var row = el('div', 'exSay');
                row.innerHTML = '<span class="exLn">' + i + '</span> ' + esc(sn.text);
                sp.appendChild(row);
            }
            if (!shown) { sp.appendChild(el('div', 'exSay dim', 'Nothing in the selection needs explaining.')); }
            inner.appendChild(sp);
        }
    }

    function jumpButton(label, line) {
        var b = el('button', 'exLink', label);
        b.onclick = function () { goToLine(line); };
        return b;
    }

    function goToLine(line) {
        if (!editor) return;
        editor.revealLineInCenter(line);
        editor.setPosition({ lineNumber: line, column: 1 });
        editor.focus();
    }

    $('gutterInner').addEventListener('click', function (ev) {
        var a = ev.target.closest ? ev.target.closest('a[data-url]') : null;
        if (!a) return;
        ev.preventDefault();
        call('openurl', { url: a.dataset.url });
    });

    // ========================================================================
    // PROVIDERS: code lenses, inlay hints, hovers, and index-mode completion
    // ========================================================================
    function registerProviders() {

        // ---- code lenses above every handler and exported function ----------
        monaco.languages.registerCodeLensProvider('typescript', {
            provideCodeLenses: function (model) {
                var lenses = [];
                var text = model.getValue();
                var re = /^(?:export\s+)?(?:async\s+)?function\s+(\w+)|^\s*Events\.(\w+)\.subscribe/gm;
                var m;
                while ((m = re.exec(text)) !== null) {
                    var pos = model.getPositionAt(m.index);
                    var name = m[1] || m[2];
                    var range = { startLineNumber: pos.lineNumber, startColumn: 1, endLineNumber: pos.lineNumber, endColumn: 1 };
                    lenses.push({ range: range, id: 'what' + pos.lineNumber, command: { id: CMD_WHAT, title: 'What this does', arguments: [name, pos.lineNumber] } });
                    lenses.push({ range: range, id: 'ex' + pos.lineNumber, command: { id: CMD_EXAMPLE, title: 'Insert example', arguments: [name, pos.lineNumber] } });
                    lenses.push({ range: range, id: 'bl' + pos.lineNumber, command: { id: CMD_BLOCKS, title: 'Show as blocks', arguments: [name, pos.lineNumber] } });
                }
                return { lenses: lenses, dispose: function () { } };
            },
            resolveCodeLens: function (m, lens) { return lens; }
        });

        // ---- inlay hints: the parameter name beside each argument -----------
        // The worker gives real ones; this fills in for index mode, and it is
        // better than the worker's for a Portal call because the names come out
        // of api.json rather than out of a signature string.
        monaco.languages.registerInlayHintsProvider('typescript', {
            provideInlayHints: function (model, range) {
                var hints = [];
                var from = range.startLineNumber, to = Math.min(range.endLineNumber, from + 200);
                var local = localDecls(model);
                for (var ln = from; ln <= to; ln++) {
                    var line = model.getLineContent(ln);
                    if (line.indexOf('(') === -1) continue;
                    var frames = walkCalls(line).all;
                    for (var fi = 0; fi < frames.length; fi++) {
                        var f = frames[fi];
                        // A name the file declares itself is not the engine's,
                        // whatever it is called, so it never gets our hints.
                        if (!f.qualified && local[f.name]) continue;
                        var ps = paramsFor(f.name);
                        if (!ps.length) continue;
                        for (var a = 0; a < f.argAt.length && a < ps.length; a++) {
                            var at = f.argAt[a];
                            var rest = line.slice(at);
                            if (!/^[^\s,)\]}]/.test(rest)) continue;         // an empty slot says nothing
                            var word = (/^[A-Za-z_$][\w$]*/.exec(rest) || [''])[0];
                            if (word && word.toLowerCase() === ps[a].name.toLowerCase()) continue;
                            hints.push({
                                position: { lineNumber: ln, column: at + 1 },
                                label: ps[a].name + ':',
                                kind: monaco.languages.InlayHintKind.Parameter,
                                paddingRight: true
                            });
                        }
                    }
                }
                return { hints: hints, dispose: function () { } };
            }
        });

        // ---- hover: our plain sentence, then the JSDoc, then the object label
        monaco.languages.registerHoverProvider('typescript', {
            provideHover: function (model, position) {
                var w = model.getWordAtPosition(position);
                if (!w) return null;
                var line = model.getLineContent(position.lineNumber);
                var parts = [];

                // a number sitting in a getter is an ObjId: name it from the scene
                var num = numberUnder(line, position.column);
                if (num !== null && /Get(SpatialObject|CapturePoint|AreaTrigger|SFX|VO|HQ|Spawner|VehicleSpawner|InteractPoint|WorldIcon|SpawnPoint|Sector|MCOM)\s*\(\s*\d/.test(line)) {
                    var label = objLabels[num];
                    parts.push({ value: '**Object ' + num + '**: ' + (label ? label : 'not found in the open scene') });
                    if (!label) askObjLabel(num);
                }

                // api.json first: it is the only source here that knows what a
                // parameter is for, which overloads exist and what is
                // deprecated. SYM is the fallback for a page that never got it.
                var entry = API_FN[w.word] || API_EV[w.word] || null;
                if (entry) {
                    parts.push({ value: describe(entry, API_FN[w.word] ? 'mod.' : '') });
                } else if (API_ENUM[w.word]) {
                    parts.push({ value: enumDescribe(API_ENUM[w.word]) });
                } else if (API_TYPE[w.word] && API_TYPE[w.word].doc) {
                    parts.push({ value: '**mod.' + w.word + '**\n\n' + API_TYPE[w.word].doc });
                } else {
                    var g = G.mod[w.word];
                    if (g) {
                        parts.push({ value: '**mod.' + w.word + '**\n\n' + g.say });
                        if (g.warn) parts.push({ value: 'Careful. ' + g.warn });
                    }
                    var sym = SYM[w.word];
                    if (sym) {
                        parts.push({ value: '```ts\n' + sym.sig + '\n```' });
                        if (sym.doc) parts.push({ value: sym.doc });
                    }
                    var tsi = G.ts[w.word];
                    if (tsi && !g && !sym) parts.push({ value: '**' + w.word + '**\n\n' + tsi.say });
                }

                // An enum member reads as a bare word, so the enum it belongs
                // to has to come off the line rather than out of the word.
                var em = new RegExp('mod\\.([A-Za-z_$][\\w$]*)\\.' + w.word + '\\b').exec(line);
                if (em && API_ENUM[em[1]]) {
                    var mem = API_ENUM[em[1]].members.filter(function (x) { return x.name === w.word; })[0];
                    if (mem) {
                        parts.push({
                            value: '**mod.' + em[1] + '.' + mem.name + '**\n\nOne of the ' +
                                API_ENUM[em[1]].members.length + ' choices in ' + em[1] + '.' +
                                (mem.doc ? '\n\n' + mem.doc : '')
                        });
                    }
                }

                if (!parts.length) return null;
                return {
                    range: new monaco.Range(position.lineNumber, w.startColumn, position.lineNumber, w.endColumn),
                    contents: parts
                };
            }
        });

        // ---- completion -----------------------------------------------------
        //
        // Registered always, and it is the whole of completion in index mode.
        // When the worker is alive Monaco merges both sets and dedupes on the
        // label, so this costs nothing there and adds the ranking, the plain
        // sentences and the snippets, none of which the worker has.
        //
        // It used to return nothing unless the line ended in "mod.", which
        // meant a beginner had to already know that every Portal command lives
        // under mod before completion would say a single word to them. Every
        // place below is a place somebody types a name and needs the list.
        monaco.languages.registerCompletionItemProvider('typescript', {
            triggerCharacters: ['.'],
            provideCompletionItems: function (model, position, context) {
                var upto = model.getValueInRange({
                    startLineNumber: position.lineNumber, startColumn: 1,
                    endLineNumber: position.lineNumber, endColumn: position.column
                });
                var after = model.getLineContent(position.lineNumber).slice(position.column - 1);
                var explicit = !!(context && monaco.languages.CompletionTriggerKind &&
                    context.triggerKind === monaco.languages.CompletionTriggerKind.Invoke);
                var ctx = completionContext(upto, model, explicit);
                if (!ctx) return { suggestions: [] };

                // The replaced range is worked out from the prefix we matched,
                // not from Monaco's word, because a string key has dots in it
                // and Monaco's idea of a word stops at the first one.
                var range = {
                    startLineNumber: position.lineNumber, endLineNumber: position.lineNumber,
                    startColumn: position.column - ctx.word.length, endColumn: position.column
                };

                var out = [];
                if (ctx.what === 'enum') out = enumItems(ctx.enumOf, range);
                else if (ctx.what === 'strings') out = stringKeyItems(ctx, range);
                else if (ctx.what === 'utils') out = utilsItems(ctx.owner, range, upto, after);
                else if (ctx.what === 'mod') out = modItems(range);
                else if (ctx.what === 'bare') out = bareItems(range, upto, after);
                return { suggestions: out, incomplete: false };
            }
        });

        // ---- signature help -------------------------------------------------
        // From api.json's parameters, so an optional one reads as optional and
        // an overload is offered as a second signature. The old version split
        // the line on commas to find the active parameter, which counted the
        // commas of any nested call as its own.
        monaco.languages.registerSignatureHelpProvider('typescript', {
            signatureHelpTriggerCharacters: ['(', ','],
            retriggerCharacters: [','],
            provideSignatureHelp: function (model, position) {
                var f = openCallAt(model, position);
                if (!f) return null;
                var fn = API_FN[f.name] || API_EV[f.name];
                var prefix = API_FN[f.name] ? 'mod.' : '';
                var sigs = [];
                if (fn) {
                    var g = G.mod[fn.name];
                    sigs.push({
                        label: prefix + fn.signature,
                        documentation: { value: describe(fn, prefix) },
                        parameters: (fn.params || []).map(function (p) {
                            return {
                                label: p.name + (p.optional ? '?' : '') + ': ' + p.type,
                                documentation: { value: p.doc || (p.optional ? 'You may leave this one out.' : '') }
                            };
                        })
                    });
                    (fn.overloads || []).forEach(function (o) {
                        sigs.push({
                            label: prefix + o.signature,
                            documentation: { value: 'The same command, given a different kind of value.' },
                            parameters: (o.params || []).map(function (p) {
                                return { label: p.name + (p.optional ? '?' : '') + ': ' + p.type, documentation: { value: p.doc || '' } };
                            })
                        });
                    });
                } else {
                    // No api.json on this page: the regex index still knows the
                    // shape, just not what any of it is for.
                    var sym = SYM[f.name];
                    if (!sym) return null;
                    var gg = G.mod[f.name];
                    sigs.push({
                        label: 'mod.' + sym.sig,
                        documentation: { value: (gg ? gg.say : sym.doc) || '' },
                        parameters: paramNames(sym.sig).map(function (p) { return { label: p }; })
                    });
                }

                // The signature whose parameters actually reach the argument
                // being typed is the one to show.
                var active = 0;
                for (var i = 0; i < sigs.length; i++) {
                    if (sigs[i].parameters.length > f.arg) { active = i; break; }
                }
                return {
                    value: { signatures: sigs, activeSignature: active, activeParameter: f.arg },
                    dispose: function () { }
                };
            }
        });
    }

    // ========================================================================
    // WHAT THE CURSOR IS IN THE MIDDLE OF
    //
    // Everything below reads the text to the LEFT of the cursor with a regex.
    // That is not a shortcut: in index mode there is no language service to
    // ask, and index mode is the state a user with a locked-down machine sits
    // in permanently, so nothing here may depend on the worker being alive.
    // ========================================================================
    function completionContext(upto, model, explicit) {
        var m;

        // mod.stringkeys.a.b   the project's own text keys, dots and all.
        // Each of the three anchors mod on a non-word character, or a variable
        // somebody called weaponmod would be answered as if it were the
        // namespace and offer them 415 commands that do not exist on it.
        m = /(?:^|[^\w$.])mod\.(stringkeys|strings)\.((?:[\w$]+\.)*[\w$]*)$/.exec(upto);
        if (m) return { what: 'strings', which: m[1], word: m[2] };

        // mod.SoldierStateBool.   one enum's own members, not all 1039 of them
        m = /(?:^|[^\w$.])mod\.([A-Za-z_$][\w$]*)\.([\w$]*)$/.exec(upto);
        if (m) {
            if (!API_ENUM[m[1]]) return null;
            return { what: 'enum', enumOf: API_ENUM[m[1]], word: m[2] };
        }

        // mod.
        m = /(?:^|[^\w$.])mod\.([\w$]*)$/.exec(upto);
        if (m) return { what: 'mod', word: m[1] };

        // Events. Timers. UI. Logger. and the rest of the utils modules
        m = /(?:^|[^\w$.])([A-Z][\w$]*)\.([\w$]*)$/.exec(upto);
        if (m && (m[1] === 'Events' || UTILS[m[1]]) && importsModule(model, m[1])) {
            return { what: 'utils', owner: m[1], word: m[2] };
        }
        if (m) return null;                     // some other object: not ours to answer for

        // A bare name. Only where a statement can start, or where a value can
        // go, so this does not fire in the middle of an identifier or a string.
        m = /(?:^|[;{}()[\],=<>+\-*/%!&|?:]|\b(?:await|return|new|typeof|of|in)\s)\s*([A-Za-z_$][\w$]*)?$/.exec(upto);
        if (!m) return null;
        var word = m[1] || '';
        // Nothing at all under the cursor would mean offering 415 names to
        // somebody who pressed Enter. That is a wall again, so it only happens
        // when they asked for the list themselves with Ctrl+Space.
        if (!word && !explicit) return null;
        return { what: 'bare', word: word };
    }

    // Whether this file actually brought the module in. Offering Timers to a
    // file with no import would put a red line under the completion it just
    // accepted, which is the worst thing a suggestion can do.
    function importsModule(model, name) {
        var text;
        try { text = model.getValue(); } catch (e) { return false; }
        return new RegExp('import\\s*\\{[^}]*\\b' + name + '\\b[^}]*\\}\\s*from').test(text);
    }

    // ---- the items themselves ----------------------------------------------
    var K = null;
    function kinds() {
        if (!K) K = monaco.languages.CompletionItemKind;
        return K;
    }
    function asSnippet() {
        return monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet;
    }

    // Everything under mod.: the commands first, then the lists of choices,
    // then the two constants, then the opaque type names that only ever appear
    // in an annotation.
    function modItems(range) {
        var out = [];
        if (!API) return symFallbackItems(range);

        API.functions.forEach(function (f) {
            out.push({
                label: f.name,
                kind: kinds().Function,
                detail: shortSignature(f, ''),
                documentation: { value: describe(f, 'mod.') },
                insertText: callSnippet(f),
                insertTextRules: asSnippet(),
                sortText: sortKey(rankOf(f.name, f.deprecated ? 600 : 0), f.name),
                tags: f.deprecated ? [monaco.languages.CompletionItemTag.Deprecated] : undefined,
                range: range
            });
        });
        API.enums.forEach(function (e) {
            out.push({
                label: e.name,
                kind: kinds().Enum,
                detail: e.members.length + ' choices',
                documentation: { value: enumDescribe(e) },
                // The dot is typed for them, because an enum name on its own is
                // never what anybody wanted: it is always a member of one.
                insertText: e.name + '.',
                command: { id: 'editor.action.triggerSuggest', title: 'choices' },
                sortText: sortKey(rankOf(e.name, 20), e.name),
                range: range
            });
        });
        API.constants.forEach(function (c) {
            out.push({
                label: c.name,
                kind: kinds().Constant,
                detail: c.name === 'stringkeys' ? 'the keys in your strings.json' : 'the text in your strings.json',
                documentation: { value: c.doc || '' },
                insertText: c.name + '.',
                command: { id: 'editor.action.triggerSuggest', title: 'keys' },
                sortText: sortKey(c.name === 'stringkeys' ? 60 : 150, c.name),
                range: range
            });
        });
        API.types.forEach(function (t) {
            if (!/^[A-Z]/.test(t.name)) return;
            out.push({
                label: t.name,
                kind: kinds().Interface,
                detail: 'a kind of thing, for writing down what a value is',
                documentation: { value: t.doc || '' },
                insertText: t.name,
                sortText: sortKey(750, t.name),
                range: range
            });
        });
        return out;
    }

    // The old path, kept whole. A page that never received api.json still
    // completes, it just has no parameter names and no ranking evidence.
    function symFallbackItems(range) {
        var out = [];
        for (var name in SYM) {
            var s = SYM[name];
            if (s.kind !== 'function' && s.kind !== 'const' && s.kind !== 'enum') continue;
            var g = G.mod[name];
            out.push({
                label: name,
                kind: s.kind === 'function' ? kinds().Function : kinds().Variable,
                detail: s.sig,
                documentation: { value: (g ? g.say + '\n\n' : '') + (s.doc || '') },
                insertText: name,
                sortText: sortKey(rankOf(name, 0), name),
                range: range
            });
        }
        return out;
    }

    function enumDescribe(e) {
        var some = e.members.slice(0, 6).map(function (m) { return m.name; }).join(', ');
        return '**mod.' + e.name + '**\n\nOne of ' + e.members.length + ' choices: ' + some +
            (e.members.length > 6 ? ', and ' + (e.members.length - 6) + ' more.' : '.');
    }

    // One enum's members, in the order the SDK declares them rather than
    // alphabetically: an ordered list like Stance or MoveSpeed means something
    // in its own order, and sorting it by name throws that away.
    function enumItems(e, range) {
        return e.members.map(function (m, i) {
            return {
                label: m.name,
                kind: kinds().EnumMember,
                detail: 'mod.' + e.name + ' = ' + m.value,
                documentation: { value: m.doc || ('One of the ' + e.members.length + ' choices in ' + e.name + '.') },
                insertText: m.name,
                sortText: orderKey(i),
                range: range
            };
        });
    }

    function stringKeyItems(ctx, range) {
        if (!STRINGKEYS.length) return [];
        var pre = ctx.which === 'stringkeys'
            ? 'A key from your own strings.json. mod.Message turns it into text a player can see.'
            : 'The text itself, for comparing or logging. It cannot be shown to a player: mod.Message needs a key from stringkeys.';
        return STRINGKEYS.map(function (s, i) {
            return {
                label: s.key,
                kind: kinds().Value,
                detail: s.text.length > 60 ? s.text.slice(0, 60) + '...' : s.text,
                documentation: { value: pre + '\n\n```\n' + s.text + '\n```\n\nFrom ' + s.rel + '.' },
                insertText: s.key,
                filterText: s.key + ' ' + s.text,
                sortText: orderKey(i),
                range: range
            };
        });
    }

    // Events, and the utils classes. The Events channels are the engine's own
    // event list; the rest are whatever that module's index.d.ts declares.
    function utilsItems(owner, range, upto, after) {
        var out = [];
        if (owner === 'Events') {
            var names = EVENT_CHANNELS || (API ? API.events.map(function (e) { return e.name; }) : []);
            // A whole subscribe block only makes sense where a statement can
            // stand on its own. Anywhere else, the name alone is the answer.
            var alone = /^\s*Events\.[\w$]*$/.test(upto) && !after.trim();
            names.forEach(function (n) {
                var ev = API_EV[n];
                out.push({
                    label: n,
                    kind: kinds().Event,
                    detail: ev ? shortSignature(ev, '') : 'an event you can subscribe to',
                    documentation: { value: ev ? describe(ev, '') : '' },
                    insertText: alone && ev ? subscribeSnippet(ev) : n,
                    insertTextRules: (alone && ev) ? asSnippet() : undefined,
                    sortText: sortKey(rankOf(n, 0), n),
                    range: range
                });
            });
            return out;
        }
        var mod = UTILS[owner];
        if (!mod) return out;
        return mod.members.map(function (m, i) {
            return {
                label: m.name,
                kind: /\(/.test(m.sig) ? kinds().Method : kinds().Property,
                detail: m.sig,
                documentation: { value: m.doc || '' },
                insertText: m.name,
                sortText: orderKey(i),
                range: range
            };
        });
    }

    // The handler written out with the engine's own parameter names and no
    // types on them: subscribe types the callback itself, so writing the types
    // here would only be another thing to get wrong.
    function subscribeSnippet(ev) {
        var ps = (ev.params || []).map(function (p, i) { return '${' + (i + 1) + ':' + p.name + '}'; });
        return ev.name + '.subscribe((' + ps.join(', ') + ') => {\n    $0\n});';
    }

    // A bare word, at the start of a statement or where a value goes. This is
    // where somebody who does not yet know that everything lives under mod
    // types "spawn" and needs to be told about mod.SpawnObject, and it is where
    // typing "deployed" should offer the whole worked shape of a deploy handler.
    function bareItems(range, upto, after) {
        var out = [];
        var statement = !upto.slice(0, range.startColumn - 1).trim() && !after.trim();

        // the pieces of code, first, because a whole shape beats one call
        if (statement) {
            snippets.forEach(function (s, i) {
                out.push({
                    label: s.label,
                    kind: kinds().Snippet,
                    detail: 'a piece of code to fill in',
                    documentation: { value: (s.note || '') + (s.warn ? '\n\n**Careful.** ' + s.warn : '') },
                    insertText: snippetToMonaco(s.body),
                    insertTextRules: asSnippet(),
                    // The label alone would not find it: somebody types
                    // "deployed" or "area", which live in the id and the note.
                    filterText: s.label + ' ' + String(s.id || '').replace(/-/g, ' ') + ' ' + (s.theme || ''),
                    additionalTextEdits: importEdits(s.imports || []),
                    sortText: '00' + orderKey(i),
                    range: range
                });
            });
        }

        if (!API) return out.concat(symFallbackItems(range).map(function (it) {
            it.label = 'mod.' + it.label;
            it.insertText = 'mod.' + it.insertText;
            it.filterText = it.label.slice(4);
            return it;
        }));

        API.functions.forEach(function (f) {
            out.push({
                label: 'mod.' + f.name,
                kind: kinds().Function,
                detail: shortSignature(f, 'mod.'),
                documentation: { value: describe(f, 'mod.') },
                insertText: 'mod.' + callSnippet(f),
                insertTextRules: asSnippet(),
                filterText: f.name,
                sortText: sortKey(rankOf(f.name, f.deprecated ? 600 : 10), f.name),
                tags: f.deprecated ? [monaco.languages.CompletionItemTag.Deprecated] : undefined,
                range: range
            });
        });
        API.enums.forEach(function (e) {
            out.push({
                label: 'mod.' + e.name,
                kind: kinds().Enum,
                detail: e.members.length + ' choices',
                documentation: { value: enumDescribe(e) },
                insertText: 'mod.' + e.name + '.',
                command: { id: 'editor.action.triggerSuggest', title: 'choices' },
                filterText: e.name,
                sortText: sortKey(rankOf(e.name, 30), e.name),
                range: range
            });
        });
        return out;
    }

    // The import a snippet needs, added in the same edit rather than announced
    // in the log afterwards. Line 1 is where ensureImports already puts them.
    function importEdits(list) {
        if (!list.length || !editor) return undefined;
        var text;
        try { text = editor.getModel().getValue(); } catch (e) { return undefined; }
        var add = list.filter(function (l) { return text.indexOf(l) === -1; });
        if (!add.length) return undefined;
        return [{
            range: new monaco.Range(1, 1, 1, 1),
            text: add.join('\n') + '\n'
        }];
    }

    // ------------------------------------------------------------------------
    // WHERE THE CURSOR IS INSIDE A CALL
    //
    // The parameter hints and the inlay hints need the same answer: which call
    // is this, and which argument am I on. One walker gives it, and it tracks
    // brackets, strings and comments, because the cheap version (split the line
    // on commas) counts a nested call's commas as its own, and every second
    // line of Portal script nests one call inside another.
    // ------------------------------------------------------------------------
    function walkCalls(text) {
        var stack = [], all = [];
        var i = 0, n = text.length;
        while (i < n) {
            var c = text[i];
            if (c === '"' || c === "'" || c === '`') {
                var q = c; i++;
                while (i < n && text[i] !== q) { if (text[i] === '\\') i++; i++; }
                i++;
                continue;
            }
            if (c === '/' && text[i + 1] === '/') { while (i < n && text[i] !== '\n') i++; continue; }
            if (c === '/' && text[i + 1] === '*') { var e = text.indexOf('*/', i); i = (e === -1) ? n : e + 2; continue; }
            if (c === '(') {
                var m = /(?:(mod)\s*\.\s*)?([A-Za-z_$][\w$]*)\s*$/.exec(text.slice(0, i));
                var j = i + 1;
                while (j < n && (text[j] === ' ' || text[j] === '\t')) j++;
                var f = { name: m ? m[2] : null, qualified: !!(m && m[1]), arg: 0, argAt: [j], closed: false };
                stack.push(f);
                if (f.name) all.push(f);
                i++;
                continue;
            }
            if (c === '[' || c === '{') { stack.push({ name: null, arg: 0, argAt: [], closed: false }); i++; continue; }
            if (c === ')' || c === ']' || c === '}') {
                var done = stack.pop();
                if (done) done.closed = true;
                i++;
                continue;
            }
            if (c === ',' && stack.length) {
                var top = stack[stack.length - 1];
                top.arg++;
                var k = i + 1;
                while (k < n && (text[k] === ' ' || text[k] === '\t')) k++;
                top.argAt.push(k);
            }
            i++;
        }
        return { open: stack, all: all };
    }

    // The innermost call the cursor is still inside, looked for over the line
    // it is on and the twenty above, so a call written across several lines
    // still gets its hints.
    function openCallAt(model, position) {
        var from = Math.max(1, position.lineNumber - 20);
        var text = model.getValueInRange({
            startLineNumber: from, startColumn: 1,
            endLineNumber: position.lineNumber, endColumn: position.column
        });
        var stack = walkCalls(text).open;
        for (var i = stack.length - 1; i >= 0; i--) {
            if (stack[i].name && (API_FN[stack[i].name] || API_EV[stack[i].name] || SYM[stack[i].name])) return stack[i];
        }
        return null;
    }

    // The parameters of a Portal command, from api.json where we have it and
    // from the regex index where we do not.
    function paramsFor(name) {
        var fn = API_FN[name] || API_EV[name];
        if (fn) return fn.params || [];
        var sym = SYM[name];
        if (!sym) return [];
        return paramNames(sym.sig).map(function (p) { return { name: p, type: '', optional: false }; });
    }

    // Names the open file declares for itself. A user's own Add() must not be
    // annotated with the engine's Add parameters. Cached on the model version
    // because it walks the whole file and the hints are asked for on scroll.
    var declCache = { uri: null, version: -1, names: {} };
    function localDecls(model) {
        var uri = model.uri.toString(), v = model.getVersionId();
        if (declCache.uri === uri && declCache.version === v) return declCache.names;
        var names = {}, text = '', m;
        try { text = model.getValue(); } catch (e) { return names; }
        var re = /\b(?:function|const|let|var|class)\s+([A-Za-z_$][\w$]*)/g;
        while ((m = re.exec(text)) !== null) names[m[1]] = 1;
        declCache = { uri: uri, version: v, names: names };
        return names;
    }

    function paramNames(sig) {
        var open = sig.indexOf('(');
        if (open < 0) return [];
        var close = sig.lastIndexOf(')');
        if (close <= open) return [];
        var inner = sig.slice(open + 1, close);
        if (!inner.trim()) return [];
        var out = [], depth = 0, cur = '';
        for (var i = 0; i < inner.length; i++) {
            var c = inner[i];
            if (c === '<' || c === '(' || c === '[') depth++;
            if (c === '>' || c === ')' || c === ']') depth--;
            if (c === ',' && depth === 0) { out.push(cur); cur = ''; continue; }
            cur += c;
        }
        out.push(cur);
        return out.map(function (p) { return p.split(':')[0].replace(/[?\s]/g, ''); }).filter(Boolean);
    }

    function numberUnder(line, col) {
        var i = col - 1;
        if (i >= line.length) i = line.length - 1;
        if (i < 0 || !/\d/.test(line[i])) return null;
        var a = i, b = i;
        while (a > 0 && /\d/.test(line[a - 1])) a--;
        while (b < line.length - 1 && /\d/.test(line[b + 1])) b++;
        return parseInt(line.slice(a, b + 1), 10);
    }

    // ---- code lens commands --------------------------------------------------
    var CMD_WHAT = null, CMD_EXAMPLE = null, CMD_BLOCKS = null;
    function registerCommands() {
        CMD_WHAT = editor.addCommand(0, function (_ctx, name, line) {
            showPane('explain');
            explainRange(line, line + 40, 'What ' + name + ' does');
        });
        CMD_EXAMPLE = editor.addCommand(0, function (_ctx, name) {
            var rec = bestRecipeFor(name);
            if (!rec) { say('no example for ' + name + ' yet', 'w'); return; }
            insertRecipe(rec, true);
        });
        /* The seam was always finished at both ends: the host opens the blocks
         * panel and hands it a payload, and that panel pastes without
         * disturbing the canvas. Only the middle was missing, and the middle is
         * the conversion, which lives on the blocks page. So the file goes
         * there. */
        CMD_BLOCKS = editor.addCommand(0, function (_ctx, name) {
            var text = '';
            try { text = editor.getModel().getValue(); } catch (e) {}
            if (!text) { say('there is nothing open to show as blocks', 'w'); return; }
            say('opening ' + (name || 'this file') + ' in the block editor', 'd');
            call('toblocksSource', { source: text, name: name || '', file: activeRel || 'source.ts' })
                .then(function (r) {
                    if (r && r.ok === false) { say(r.error || 'the block editor could not take it', 'w'); }
                    else { say('the block editor has it. Look for the blocks it just pasted.', 'g'); }
                })
                .catch(function (e) { say('could not reach the block editor: ' + (e && e.message || e), 'w'); });
        });
    }

    function bestRecipeFor(name) {
        var lower = String(name || '').toLowerCase();
        for (var i = 0; i < G.recipes.length; i++) {
            var r = G.recipes[i];
            var blob = (r.id + ' ' + r.title + ' ' + JSON.stringify(r.lines)).toLowerCase();
            if (lower && blob.indexOf(lower) !== -1) return r;
        }
        return G.recipes[0];
    }

    // ========================================================================
    // DIAGNOSTICS, IN PLAIN WORDS
    // ========================================================================
    var lastDiags = [];

    function renderDiagnostics() {
        if (!editor || !editor.getModel()) return;
        var markers = monaco.editor.getModelMarkers({ resource: editor.getModel().uri });
        lastDiags = markers;
        if ($('pane-diagnose').classList.contains('on')) drawDiagnose();
    }

    function explainDiag(m) {
        var e = G.errors[m.code];
        if (e) return e;
        // A message we have no table entry for still gets a shape.
        return {
            cause: m.message,
            fix: 'Hover the underlined part for the compiler\'s own detail. If it names a type, hover both sides of the assignment to see what each one really is.'
        };
    }

    // ========================================================================
    // CHECK MY SCRIPT
    //
    // A text scan, deliberately. It is meant to find the shapes that compile
    // cleanly and then fail in the game, which is exactly the class the
    // compiler cannot see.
    // ========================================================================
    function checkScript() {
        var found = [];
        Object.keys(files).forEach(function (rel) {
            var lines = files[rel].model.getValue().split('\n');
            scanFile(rel, lines, found);
        });
        return found;
    }

    function hit(found, id, rel, ln, extra) {
        for (var i = 0; i < G.pitfalls.length; i++) {
            if (G.pitfalls[i].id !== id) continue;
            var p = G.pitfalls[i];
            found.push({ p: p, rel: rel, line: ln, extra: extra || '' });
            return;
        }
    }

    function scanFile(rel, lines, found) {
        var i, j, t;
        var subs = 0, unsubs = 0;

        for (i = 0; i < lines.length; i++) {
            t = lines[i];

            // spawn then configure on the same tick
            if (/mod\.SpawnObject\s*\(/.test(t)) {
                var waited = false;
                for (j = i; j < Math.min(i + 14, lines.length); j++) {
                    if (/await\s+mod\.Wait\s*\(/.test(lines[j])) { waited = true; break; }
                    if (/mod\.Set\w+\s*\(/.test(lines[j]) && j > i) break;
                }
                if (!waited) {
                    var configures = false;
                    for (j = i; j < Math.min(i + 14, lines.length); j++) {
                        if (/mod\.(Set|Enable|Force)\w+\s*\(/.test(lines[j])) { configures = true; break; }
                    }
                    if (configures) hit(found, 'spawn-then-configure', rel, i + 1);
                }
            }

            // VO on a carrier spawned this tick
            if (/mod\.PlayVO\s*\(/.test(t)) {
                var recent = false;
                for (j = Math.max(0, i - 10); j < i; j++) {
                    if (/mod\.SpawnObject\s*\(/.test(lines[j])) recent = true;
                    if (/await\s+mod\.Wait\s*\(/.test(lines[j])) recent = false;
                }
                if (recent) hit(found, 'vo-same-tick', rel, i + 1);
            }

            // a scaled object moved by a transform write
            if (/mod\.(SetObjectTransform|MoveObject|RotateObject)\w*\s*\(/.test(t)) {
                var scaled = false;
                var whole = lines.join('\n');
                if (/mod\.SpawnObject\s*\([^)]*,[^)]*,[^)]*,[^)]*\)/.test(whole) ||
                    /scale/i.test(whole)) scaled = true;
                if (scaled) hit(found, 'scaled-move', rel, i + 1,
                    'This file also creates or mentions a scaled object. If the object on this line is the scaled one, the move corrupts it.');
            }

            // MoveObject given something that reads as an absolute position
            if (/mod\.MoveObject\s*\(/.test(t) &&
                /(GetObjectPosition|GetSoldierState|position|Position)\s*[,)]/.test(t) &&
                !/Vector\s*\(\s*0/.test(t)) {
                hit(found, 'move-absolute', rel, i + 1);
            }

            // a pooled emitter moved
            if (/mod\.MoveObject\s*\(/.test(t) && /(sfx|SFX|emitter|voice)/.test(t)) {
                hit(found, 'pooled-sfx', rel, i + 1);
            }

            // async on a high frequency event
            if (/(OnPlayerDamaged|OngoingPlayer|OngoingGlobal)/.test(t) &&
                /(async|=>\s*\{?\s*$)/.test(t) && /async/.test(t)) {
                hit(found, 'damaged-async', rel, i + 1);
            }

            // a polygon written by hand
            if (/points\s*[:=]\s*\[/.test(t) || /PolygonVolume/.test(t)) {
                hit(found, 'polygon-winding', rel, i + 1,
                    'Only the map\'s spatial JSON can define a combat area, so check the winding there rather than in script.');
            }

            // negative UI offsets
            if (/(x|y)\s*:\s*-\d/.test(t) && /(anchor|UIAnchor|UI\.|AddUI)/.test(lines.slice(Math.max(0, i - 6), i + 7).join(' '))) {
                hit(found, 'ui-anchor-outward', rel, i + 1);
            }

            // SetTeam without the guards
            if (/mod\.SetTeam\s*\(/.test(t)) {
                var guarded = lines.slice(Math.max(0, i - 8), i).join(' ');
                if (!/Undeploy/.test(guarded) || !/GetTeam/.test(guarded)) {
                    hit(found, 'setteam-rules', rel, i + 1);
                }
            }

            // setup in an Ongoing handler
            if (/function\s+Ongoing\w+|Ongoing\w+\.subscribe/.test(t)) {
                var body = lines.slice(i, Math.min(i + 30, lines.length)).join('\n');
                if (/mod\.(SetCapturePoint\w+|EnableHQ|SetHQTeam|SetGameMode(TimeLimit|TargetScore|Criteria))/.test(body)) {
                    hit(found, 'setup-in-ongoing', rel, i + 1);
                }
            }

            // GetMatchTimeElapsed inside a handler that fires per event
            if (/mod\.GetMatchTimeElapsed\s*\(/.test(t)) {
                var ctx = lines.slice(Math.max(0, i - 25), i).join('\n');
                if (/(OnPlayerDamaged|OnPlayerDied|Ongoing\w+)/.test(ctx)) hit(found, 'matchtime-hot-path', rel, i + 1);
            }

            // console.log on a tick path
            if (/console\.log\s*\(/.test(t)) {
                var ctx2 = lines.slice(Math.max(0, i - 25), i).join('\n');
                if (/(Ongoing\w+|OnPlayerDamaged)/.test(ctx2)) hit(found, 'console-log-hot', rel, i + 1);
            }

            // native calls on a stored handle with no guard
            if (/mod\.(EnableInputRestriction|SetRedeployTime|UndeployPlayer|EnablePlayerDeploy|Teleport|Kill|Heal)\s*\(/.test(t)) {
                var g2 = lines.slice(Math.max(0, i - 6), i).join(' ');
                if (!/IsPlayerValid|IsValid/.test(g2)) hit(found, 'unguarded-native', rel, i + 1);
            }

            if (/\.subscribe\s*\(/.test(t)) subs++;
            if (/unsubscribe|\(\)\s*;\s*$/.test(t) && /unsub/i.test(t)) unsubs++;
        }

        // a per player subscription with no cleanup anywhere in the file
        var all = lines.join('\n');
        if (subs > 0 && !/OnPlayerLeaveGame/.test(all) && /player/i.test(all) && unsubs === 0) {
            hit(found, 'subscribe-no-unsubscribe', rel, 1,
                'This file subscribes ' + subs + ' time' + (subs === 1 ? '' : 's') + ' and never unsubscribes, and it has no OnPlayerLeaveGame handler.');
        }

        // text shown that is not a strings key
        if (/mod\.Message\s*\(\s*[`'"]/.test(all)) {
            hit(found, 'raw-string-message', rel, lineOf(lines, /mod\.Message\s*\(\s*[`'"]/));
        }

        // an exported handler whose parameters are not the ones the engine sends
        scanEventHandlers(rel, lines, found);
    }

    function lineOf(lines, re) {
        for (var i = 0; i < lines.length; i++) if (re.test(lines[i])) return i + 1;
        return 1;
    }

    // ------------------------------------------------------------------------
    // THE SIXTEENTH TRAP: an event handler with the wrong parameters.
    //
    // An exported handler is just an exported function. TypeScript has no idea
    // it is a handler, so the wrong parameter type compiles perfectly and then
    // the SITE rejects the upload, which is the community's most reported
    // "Action Needed" save failure and the least obvious.
    //
    // The live case that found it: OnPlayerLeaveGame written as
    // (eventPlayer: mod.Player), where the engine declares (eventNumber: number).
    // The player has already gone by then, so all you get is their id.
    //
    // The truth is node_modules/bf6-portal-mod-types/event-handler-signatures.d.ts,
    // which the tool already pushes in as a type library, so nothing extra has
    // to be read off disk.
    // ------------------------------------------------------------------------
    var EVENT_SIGS = null;      // name -> [{ name, type }]

    function splitParams(inner) {
        var out = [], depth = 0, cur = '';
        for (var i = 0; i < inner.length; i++) {
            var c = inner[i];
            if (c === '<' || c === '(' || c === '[' || c === '{') depth++;
            if (c === '>' || c === ')' || c === ']' || c === '}') depth--;
            if (c === ',' && depth === 0) { out.push(cur); cur = ''; continue; }
            cur += c;
        }
        if (cur.trim()) out.push(cur);
        return out.map(function (p) {
            var t = p.trim();
            if (!t) return null;
            var at = t.indexOf(':');
            return {
                name: (at === -1 ? t : t.slice(0, at)).replace(/[?\s]/g, ''),
                type: (at === -1 ? '' : t.slice(at + 1)).trim().replace(/\s+/g, ' ')
            };
        }).filter(Boolean);
    }

    // A type written two defensible ways is the same type: mod.Player and
    // Player, number and Number. Reporting those as a mismatch would train
    // people to ignore the check.
    function sameType(a, b) {
        var norm = function (t) {
            return String(t || '').replace(/\bmod\./g, '').replace(/\s+/g, '').toLowerCase();
        };
        return norm(a) === norm(b);
    }

    function parseEventSigs(text) {
        var sigs = {};
        var re = /export\s+function\s+(On\w+)\s*\(([\s\S]*?)\)\s*:/g;
        var m;
        while ((m = re.exec(text)) !== null) sigs[m[1]] = splitParams(m[2]);
        // Ongoing* handlers take a subject too and are worth the same check.
        var re2 = /export\s+function\s+(Ongoing\w+)\s*\(([\s\S]*?)\)\s*:/g;
        while ((m = re2.exec(text)) !== null) sigs[m[1]] = splitParams(m[2]);
        EVENT_SIGS = sigs;
        say('event handler shapes loaded: ' + Object.keys(sigs).length, 'd');
    }

    function describeParams(ps) {
        if (!ps.length) return 'no parameters';
        return ps.map(function (p) { return p.name + ': ' + (p.type || '?'); }).join(', ');
    }

    // Every exported On*/Ongoing* function in a file, checked against the
    // declared shape. Multi-line parameter lists are joined first.
    function scanEventHandlers(rel, lines, found) {
        if (!EVENT_SIGS) return;
        var text = lines.join('\n');
        var re = /export\s+(?:async\s+)?function\s+((?:On|Ongoing)\w+)\s*\(([\s\S]*?)\)\s*(?::|\{)/g;
        var m;
        while ((m = re.exec(text)) !== null) {
            var name = m[1];
            var want = EVENT_SIGS[name];
            if (!want) continue;                       // not one the engine declares
            var got = splitParams(m[2]);
            var line = text.slice(0, m.index).split('\n').length;

            // TAKING FEWER THAN THE ENGINE SENDS IS NOT A MISTAKE. Ignoring the
            // trailing arguments is ordinary JavaScript and five of this tool's
            // own checked answers do it on purpose. Flagging that would train
            // people to ignore the check, which would cost more than it saved.
            // What is wrong is a parameter the engine will never fill, and a
            // parameter whose type is not the one it sends.
            var problem = null;
            if (got.length > want.length) {
                problem = 'It takes ' + got.length + ' parameter' + (got.length === 1 ? '' : 's') +
                    ' and the engine only ever sends ' + want.length +
                    ', so the last ' + (got.length - want.length) + ' would always be undefined.';
            } else {
                for (var i = 0; i < got.length; i++) {
                    if (want[i].type && got[i].type && !sameType(want[i].type, got[i].type)) {
                        problem = 'Parameter ' + (i + 1) + ' is written as ' +
                            (got[i].type) + ' and the engine sends ' + want[i].type + '.';
                        break;
                    }
                }
            }
            if (!problem) continue;

            hit(found, 'event-handler-signature', rel, line,
                problem + ' The engine declares ' + name + '(' + describeParams(want) + '). ' +
                'You wrote ' + name + '(' + describeParams(got) + '). ' +
                'Change your parameters to match, name for name and type for type.');
        }
    }

    // ========================================================================
    // THE SLIDE-OUT SHELF
    //
    // The code is the canvas and it fills the window. Everything that helps you
    // write it sits behind one small labelled handle on the left strip and
    // slides out over the code, the same way the scene tree's symbol legend
    // sits on the viewport: closed by default, one panel at a time, Escape
    // closes it, and the tool comes back the way you left it.
    //
    // KEEP OPEN is the escape hatch for the two panels you work beside rather
    // than read once (EXPLAIN THIS and WHAT WENT WRONG). Pinned, a click in the
    // code no longer closes it.
    // ========================================================================
    var prefs = {};                 // what the tool remembered from last time
    var shelfOpen = false;
    var shelfPane = 'walk';         // which handle, open or not
    var shelfPinned = false;

    function setPref(name, value) {
        prefs[name] = value;
        call('pref', { name: name, value: String(value) }).catch(function () { });
    }

    // The one sentence at the top of each panel lives in the markup; the handle
    // name lives here, so the strip and the panel header can never disagree.
    var PANE_NAME = {
        walk: 'Start here', files: 'Your files', project: 'Project', learn: 'Learn',
        map: 'How it fits together',
        api: 'Find a command', snippets: 'Examples', explain: 'Explain this',
        diagnose: 'What went wrong', faq: 'Ask a question'
    };

    // ---- HOW IT FITS TOGETHER ----------------------------------------------
    // The whole panel lives in map.js. All this end does is hand it the things
    // it cannot reach from outside this closure, once, the first time the panel
    // is opened. Accessors rather than values, because every one of them
    // changes while the tool is running: the open files, which file is on
    // screen, and whether the language service worker actually started.
    var mapAttached = false;
    function drawMap() {
        var M = window.BF6ScriptMap;
        if (!M) { $('mapWhere').textContent = 'The structure map did not load.'; return; }
        if (!mapAttached) {
            mapAttached = true;
            M.attach({
                monaco: function () { return monaco; },
                editor: function () { return editor; },
                files: function () { return files; },
                activeRel: function () { return activeRel; },
                mode: function () { return mode; },
                openFile: function (rel) { openFile(rel); },
                readFile: function (rel) { call('read', { rel: rel }); },
                // lastFiles is filled in when a project opens, so it can still
                // be undefined the first time this panel is opened.
                projectFiles: function () { return Object.keys(lastFiles || {}); },
                eventParams: function (name) { return EVENT_SIGS ? EVENT_SIGS[name] : null; },
                guide: G
            });
        }
        M.draw();
    }

    function showPane(name) {
        shelfPane = name;
        shelfOpen = true;
        var panes = document.querySelectorAll('.pane');
        for (var i = 0; i < panes.length; i++) panes[i].classList.remove('on');
        var p = $('pane-' + name);
        if (p) p.classList.add('on');
        var btns = document.querySelectorAll('.railbtn');
        for (i = 0; i < btns.length; i++) btns[i].classList.toggle('on', btns[i].dataset.pane === name);
        $('side').classList.add('open');
        $('sideTitle').textContent = PANE_NAME[name] || name;
        // The checked answers feed three panels, so any of the three brings
        // them in. They are 400 KB across the three sets, which is why they
        // arrive on first use rather than at boot.
        if (name === 'faq' || name === 'snippets' || name === 'diagnose') ensureAnswers();
        if (name === 'snippets') drawSnippets();
        if (name === 'diagnose') drawDiagnose();
        if (name === 'project') drawProject();
        if (name === 'map') drawMap();
        if (name === 'api') { drawApiSearch(); drawTypeSources(); setTimeout(function () { $('apiQ').focus(); }, 0); }
        if (name === 'faq') { drawFaq(); if (!faqLoaded) { faqLoaded = true; call('faq'); } }
        setPref('shelf', name);
        setPref('shelfOpen', true);
    }

    // Closing leaves nothing marked ON, so the checks elsewhere that ask
    // "is the diagnose panel showing?" stay true to what is on screen.
    function closeShelf(remember) {
        shelfOpen = false;
        var panes = document.querySelectorAll('.pane');
        for (var i = 0; i < panes.length; i++) panes[i].classList.remove('on');
        var btns = document.querySelectorAll('.railbtn');
        for (i = 0; i < btns.length; i++) btns[i].classList.remove('on');
        $('side').classList.remove('open');
        if (remember !== false) setPref('shelfOpen', false);
    }

    function toggleShelf(name) {
        if (shelfOpen && shelfPane === name) { closeShelf(); return; }
        // EXPLAIN THIS is the one handle that does its job the moment you press
        // it: pick some code, press it, read the answer. Two steps was one too
        // many for somebody who has never scripted.
        if (name === 'explain' && editor && editor.getModel()) { $('btnExplain').click(); return; }
        showPane(name);
    }

    function setPinned(on) {
        shelfPinned = !!on;
        $('sidePin').classList.toggle('on', shelfPinned);
        $('sidePin').textContent = shelfPinned ? 'Stays open' : 'Keep open';
        setPref('shelfPinned', shelfPinned);
    }

    var rail = document.querySelectorAll('.railbtn');
    for (var ri = 0; ri < rail.length; ri++) {
        rail[ri].onclick = (function (b) { return function () { toggleShelf(b.dataset.pane); }; })(rail[ri]);
    }
    $('sideClose').onclick = function () { closeShelf(); };
    $('sidePin').onclick = function () { setPinned(!shelfPinned); };

    // Click away closes it, unless it is pinned. The strip and the panel are
    // not "away", and neither is anything the panel put on the page.
    document.addEventListener('mousedown', function (ev) {
        if (!shelfOpen || shelfPinned) return;
        if ($('side').contains(ev.target) || $('rail').contains(ev.target)) return;
        if ($('more').contains(ev.target)) return;
        closeShelf();
    }, true);

    // ---- the overflow --------------------------------------------------------
    function closeMore() { $('more').hidden = true; }
    $('btnMore').onclick = function (ev) {
        ev.stopPropagation();
        var m = $('more');
        if (!m.hidden) { closeMore(); return; }
        var r = $('btnMore').getBoundingClientRect();
        m.style.left = Math.round(r.left) + 'px';
        m.style.top = Math.round(r.bottom + 2) + 'px';
        m.hidden = false;
    };
    $('more').addEventListener('click', function (ev) {
        if (ev.target.tagName === 'BUTTON') closeMore();
    });
    document.addEventListener('mousedown', function (ev) {
        if ($('more').hidden) return;
        if ($('more').contains(ev.target) || ev.target === $('btnMore')) return;
        closeMore();
    }, true);

    // ---- the message log -----------------------------------------------------
    // It used to sit open under the editor at all times. It is a log, and a log
    // is not a thing a beginner reads: the last line goes in the status strip,
    // the rest is one press away, and the button colours itself when something
    // arrived that nobody has looked at.
    function toggleConsole(on) {
        var want = (on === undefined) ? !outEl.classList.contains('on') : !!on;
        outEl.classList.toggle('on', want);
        $('btnMsgs').classList.toggle('on', want);
        if (want) { $('btnMsgs').classList.remove('unread', 'bad'); outEl.scrollTop = outEl.scrollHeight; }
        setPref('console', want);
    }
    $('btnMsgs').onclick = function () { toggleConsole(); };

    // ---- first run -----------------------------------------------------------
    // A fresh install used to open on an empty editor with twenty-two things to
    // press. It now opens on one.
    function refreshFirstRun() {
        $('firstrun').hidden = !!project;
    }
    $('btnFirstMake').onclick = function () { $('btnNew').click(); };

    // ---- the text list's own controls ------------------------------------
    if ($('btnStrAdd')) { $('btnStrAdd').onclick = function () { strAdd(); }; }
    if ($('chkBundleHide')) {
        $('chkBundleHide').onchange = function () {
            showTemplateCode = !!$('chkBundleHide').checked;
            applyTemplateHiding();
        };
    }
    if ($('sfxClose')) { $('sfxClose').onclick = function () { closeSfxPicker(); }; }
    if ($('sfxSearch')) {
        $('sfxSearch').oninput = function () { drawSfxList($('sfxSearch').value); };
        $('sfxSearch').onkeydown = function (ev) { if (ev.key === 'Escape') closeSfxPicker(); };
    }
    if ($('findAll')) {
        var findTimer = null;
        $('findAll').oninput = function () {
            if (findTimer) { clearTimeout(findTimer); }
            var v = $('findAll').value;
            // Debounced: every keystroke would otherwise search the project.
            findTimer = setTimeout(function () { findAllRun(v); }, 220);
        };
        $('findAll').onkeydown = function (ev) {
            if (ev.key === 'Escape') { $('findPanel').hidden = true; $('findAll').blur(); }
            if (ev.key === 'Enter') { findAllRun($('findAll').value); }
        };
    }
    if ($('findClose')) {
        $('findClose').onclick = function () { $('findPanel').hidden = true; };
    }
    if ($('btnOfferYes')) {
        $('btnOfferYes').onclick = function () {
            $('quickOffer').hidden = true;
            var sp = $('suggestPane'); if (sp) sp.hidden = false;
            findValues();
        };
    }
    if ($('btnOfferNo')) {
        $('btnOfferNo').onclick = function () { $('quickOffer').hidden = true; };
    }
    if ($('btnFindValues')) {
        $('btnFindValues').onclick = function () {
            var sp = $('suggestPane');
            if (sp && !sp.hidden) { sp.hidden = true; return; }
            if (sp) sp.hidden = false;
            findValues();
        };
    }
    if ($('chkStrAll')) {
        $('chkStrAll').onchange = function () {
            strShowTemplate = !!$('chkStrAll').checked;
            drawStrings();
        };
    }
    $('btnFirstTour').onclick = function () { showPane('walk'); };

    // ---- Escape --------------------------------------------------------------
    // Not on capture, and not when something already answered it: Monaco uses
    // Escape to dismiss its own suggestion box, and taking that away would be a
    // worse trade than leaving the panel open one more press.
    document.addEventListener('keydown', function (ev) {
        if (ev.key !== 'Escape' || ev.defaultPrevented) return;
        if (!$('more').hidden) { closeMore(); ev.preventDefault(); return; }
        if ($('askBox') && !$('askBox').hidden) { closeAsk(); ev.preventDefault(); return; }
        if (shelfOpen) { closeShelf(); ev.preventDefault(); }
    });

    // ========================================================================
    // THE PROJECT PANEL: the template's own tools, and the feature split
    //
    // Mike De Luca's template ships eight npm scripts. The tool ran two of them
    // and never mentioned the rest, so unless you opened package.json you never
    // learned that the template could tidy your code, check it, make the
    // thumbnail the site asks for or update its own scripts.
    //
    // DEPLOY IS NOT HERE AND MUST NOT BE. It signs in to EA with a session id
    // this tool does not hold. PUT ON PORTAL is the path that ships a script.
    // ========================================================================
    var TOOLS = [
        {
            id: 'build', t: 'Build',
            d: 'Turns everything under src into the one file the game runs. Do this before you put anything on Portal.',
            cmd: 'npm run build'
        },
        {
            id: 'install', t: 'Install packages',
            d: 'Fetches the code your project leans on. Once per project, and again after Update the template scripts.',
            cmd: 'npm install'
        },
        {
            id: 'lint', t: 'Check style',
            d: 'Reads your code for sloppy shapes and the mistakes that usually turn out to be bugs.',
            cmd: 'npm run lint',
            care: 'In a brand new project this fails until you have installed the packages: the template\'s own style rules ask for a package its package.json does not list, and the install brings it in.'
        },
        {
            id: 'prettier', t: 'Tidy the code',
            d: 'Lays every file out the same way. It moves spacing and punctuation only, never what the code does.',
            cmd: 'npm run prettier'
        },
        {
            id: 'refresh-ai', t: 'Refresh the helper notes',
            d: 'Rebuilds the notes the template keeps for an AI assistant, so they describe the code you have now.',
            cmd: 'npm run refresh-ai'
        },
        {
            id: 'update', t: 'Update the template scripts',
            d: 'Brings the template\'s newest scripts and settings into this project.',
            cmd: 'npm run update',
            care: 'This changes files in your project. Save your work first, and install the packages again afterwards.'
        },
        {
            id: 'export-thumbnail', t: 'Make a thumbnail',
            d: 'Turns src/thumbnail.png into the picture the Portal site asks for: 352 by 248, under 78 KB.',
            cmd: 'npm run export-thumbnail'
        },
        {
            id: 'minify-spatials', t: 'Shrink map files',
            d: 'Squeezes the spatial map files so they take less room and upload faster.',
            cmd: 'npm run minify-spatials'
        }
    ];

    // What a failure MEANS. npm's own output is a wall, and the first line of it
    // is almost never the reason.
    function plainFailure(tool, text, why) {
        var blob = ((text || '') + ' ' + (why || ''));
        var low = blob.toLowerCase();
        if (low.indexOf('cannot find package') !== -1 || low.indexOf('cannot find module') !== -1 ||
            low.indexOf('err_module_not_found') !== -1) {
            return 'Something this needs is not installed in the project yet. Run INSTALL PACKAGES, then try again.';
        }
        if (low.indexOf('missing script') !== -1) {
            return 'This project\'s package.json has no ' + tool.id + ' script. It was made from an older template: run UPDATE THE TEMPLATE SCRIPTS.';
        }
        if (low.indexOf('enoent') !== -1 && tool.id === 'export-thumbnail') {
            return 'There is no src/thumbnail.png to make a thumbnail from. Put a picture there first.';
        }
        if (low.indexOf('enoent') !== -1) {
            return 'A file this needs is not where it expected. The message above names it.';
        }
        if (tool.id === 'lint') {
            return 'Style problems were found, and the lines above say where. Nothing is broken: TIDY THE CODE fixes most of them on its own.';
        }
        if (low.indexOf('eacces') !== -1 || low.indexOf('eperm') !== -1) {
            return 'Windows would not let it write. Close anything that has the project folder open and try again.';
        }
        return 'It stopped before it finished. The lines above are its own words for why.';
    }

    var toolBusy = null;

    function runTool(tool) {
        if (!project) { say('open or create a project first', 'w'); return; }
        if (toolBusy) { say(toolBusy.t + ' is still running. One at a time.', 'w'); return; }
        if (tool.id === 'update' &&
            !window.confirm('Update the template scripts?\n\nThis rewrites the template\'s own files inside your project. Your code under src is left alone, but save anything you have open first.')) {
            return;
        }
        toolBusy = tool;
        drawProject();
        setToolOut(tool.t + ' is running.', '');
        var p = (tool.id === 'install') ? call('install') : call('run', { script: tool.id });
        p.then(function (r) {
            toolBusy = null;
            drawProject();
            say(r.text || (tool.t + ' finished'), 'g');
            setToolOut(tool.t + ' finished.', '', 'ok');
            if (tool.id === 'build') { builtOnce = true; drawWalk(); }
            if (tool.id === 'install') refreshTypes();
            if (tool.id === 'update' || tool.id === 'prettier') refreshFiles();
        }, function (e) {
            toolBusy = null;
            drawProject();
            var text = (e && e.text) || '';
            var why = (e && e.why) || '';
            say(tool.t + ' failed: ' + (why || text), 'e');
            setToolOut(plainFailure(tool, text, why), text || why, 'bad');
        });
    }

    function setToolOut(headline, detail, kind) {
        var host = $('toolOut');
        if (!host) return;
        host.innerHTML = '';
        var c = el('div', 'card' + (kind ? ' ' + kind : ''));
        c.innerHTML = '<div class="t">' + esc(headline) + '</div>';
        if (detail) {
            var d = el('div', 's');
            d.style.whiteSpace = 'pre-wrap';
            d.style.fontStyle = 'normal';
            d.textContent = String(detail).slice(-1200);
            c.appendChild(d);
        }
        var more = el('button', 'link', 'See every line in Messages');
        more.onclick = function () { toggleConsole(true); };
        c.appendChild(more);
        host.appendChild(c);
    }

    // ---- features ------------------------------------------------------------
    // A feature is a folder under src. The word is deliberate: a beginner does
    // not have "modules", they have things their experience does.
    var FEATURE_SKIP = { helpers: 1 };

    function featureFolders() {
        var seen = {}, out = [];
        Object.keys(lastFiles || {}).forEach(function (rel) {
            var m = /^src\/([^\/]+)\//.exec(rel);
            if (!m) return;
            if (seen[m[1]]) return;
            seen[m[1]] = 1;
            out.push(m[1]);
        });
        return out.sort();
    }

    function kebab(s) {
        return String(s).trim().toLowerCase()
            .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
    }
    function pascal(s) {
        return kebab(s).split('-').filter(Boolean)
            .map(function (w) { return w.charAt(0).toUpperCase() + w.slice(1); }).join('');
    }
    function camel(s) {
        var p = pascal(s);
        return p.charAt(0).toLowerCase() + p.slice(1);
    }

    // Modelled on src/debug-tool and src/helpers, so what comes out looks like
    // the rest of the template rather than like something a tool bolted on.
    function featureIndexTs(name) {
        var P = pascal(name), C = camel(name);
        return '' +
            '// ' + name + '\n' +
            '//\n' +
            '// One feature of your experience, in its own folder. Everything under src\n' +
            '// is built into the one file that goes to Portal, so a feature is a way to\n' +
            '// keep one idea in one place, not a separate thing to upload.\n' +
            '//\n' +
            '// The words this shows on screen live in strings.json beside this file.\n' +
            '\n' +
            'import { Events } from \'bf6-portal-utils/events/index.ts\';\n' +
            '\n' +
            'export function start' + P + '(): void {\n' +
            '    Events.OnPlayerDeployed.subscribe((player: mod.Player): void => {\n' +
            '        mod.DisplayNotificationMessage(mod.Message(mod.stringkeys.' + C + '.hello), player);\n' +
            '    });\n' +
            '}\n';
    }

    function featureStringsJson(name) {
        var o = {};
        o[camel(name)] = { hello: name + ' is running' };
        return JSON.stringify(o, null, 4) + '\n';
    }

    // Where the import goes: under the last local import if there is one, under
    // the last import of any kind otherwise, and at the very top if the file has
    // none. Where the call goes: the end, which is where both shipped variants
    // of index.ts already put their event subscriptions.
    function wireFeatureInto(src, name) {
        var P = pascal(name), K = kebab(name);
        var imp = 'import { start' + P + ' } from \'./' + K + '/index.ts\';';
        if (src.indexOf(imp) === -1) {
            var lines = src.split('\n');
            var at = -1;
            for (var i = 0; i < lines.length; i++) {
                if (/^import\s.*from\s+'\.\//.test(lines[i])) at = i;
            }
            if (at === -1) for (i = 0; i < lines.length; i++) if (/^import\s/.test(lines[i])) at = i;
            if (at === -1) { lines.unshift(imp, ''); }
            else lines.splice(at + 1, 0, imp);
            src = lines.join('\n');
        }
        var call = 'start' + P + '();';
        if (src.indexOf(call) === -1) {
            if (!/\n$/.test(src)) src += '\n';
            src += '\n// The ' + name + ' feature.\n' + call + '\n';
        }
        return src;
    }

    function unwireFeatureFrom(src, name) {
        var P = pascal(name), K = kebab(name);
        var out = [];
        var lines = src.split('\n');
        for (var i = 0; i < lines.length; i++) {
            var L = lines[i];
            if (L.indexOf('\'./' + K + '/') !== -1 && /^import\s/.test(L.trim())) continue;
            if (L.trim() === 'start' + P + '();') continue;
            if (L.trim() === '// The ' + name + ' feature.') continue;
            out.push(L);
        }
        return out.join('\n').replace(/\n{3,}$/, '\n');
    }

    function addFeature() {
        if (!project) { say('open or create a project first', 'w'); return; }
        var raw = window.prompt('What should the feature be called?\n\nSomething plain, like "scoring" or "night mode".');
        if (!raw) return;
        var K = kebab(raw);
        if (!K) { say('that name has nothing in it a folder can be called', 'w'); return; }
        if (featureFolders().indexOf(K) !== -1) { say('there is already a feature called ' + K, 'w'); return; }

        var rel = 'src/' + K + '/index.ts';
        call('write', { rel: rel, text: featureIndexTs(raw) })
            .then(function () { return call('write', { rel: 'src/' + K + '/strings.json', text: featureStringsJson(raw) }); })
            .then(function () { return readText('src/index.ts'); })
            .then(function (src) { return call('write', { rel: 'src/index.ts', text: wireFeatureInto(src, raw) }); })
            .then(function () {
                say('added the ' + K + ' feature and wired it into src/index.ts', 'g');
                // The open copy of index.ts is now behind the file on disk: the
                // wiring above was written FROM that copy, so the disk version
                // contains it and is the newer one. This is the one case
                // openFile lets past its unsaved-work guard.
                if (files['src/index.ts']) { reloadReads['src/index.ts'] = 1; call('read', { rel: 'src/index.ts' }); }
                refreshFiles();
                openFile(rel, featureIndexTs(raw));
                drawProject();
            }, function (e) { say('could not add the feature: ' + (e && (e.why || e.text)), 'e'); });
    }

    function removeFeature() {
        if (!project) { say('open or create a project first', 'w'); return; }
        var have = featureFolders().filter(function (f) { return !FEATURE_SKIP[f]; });
        if (!have.length) { say('there are no features to remove', 'w'); return; }
        var raw = window.prompt('Which feature should stop running?\n\n' + have.join(', ') +
            '\n\nThe folder and everything in it stays on disk. Only the wiring in src/index.ts goes.');
        if (!raw) return;
        var K = kebab(raw);
        if (have.indexOf(K) === -1) { say('there is no feature called ' + K, 'w'); return; }
        readText('src/index.ts').then(function (src) {
            return call('write', { rel: 'src/index.ts', text: unwireFeatureFrom(src, K) });
        }).then(function () {
            say('the ' + K + ' feature is no longer wired in. Its folder is still there.', 'g');
            // Same as addFeature: the unwired index.ts on disk was written from
            // the open copy, so it supersedes it rather than competing with it.
            if (files['src/index.ts']) { reloadReads['src/index.ts'] = 1; call('read', { rel: 'src/index.ts' }); }
            drawProject();
        }, function (e) { say('could not remove the feature: ' + (e && (e.why || e.text)), 'e'); });
    }

    // A file's text, from the open copy if there is one, off disk otherwise.
    // The disk read arrives as a payload rather than as a reply, so the waiter
    // is parked where onFileText can find it.
    function readText(rel, quiet) {
        if (files[rel]) return Promise.resolve(files[rel].model.getValue());
        return new Promise(function (resolve, reject) {
            var park = window.__bf6PendingReads || (window.__bf6PendingReads = {});
            park[rel] = resolve;
            if (quiet) quietReads[rel] = 1;
            call('read', { rel: rel }).catch(function (e) { delete quietReads[rel]; reject(e); });
            setTimeout(function () {
                if (park[rel]) { delete park[rel]; delete quietReads[rel]; reject({ why: 'could not read ' + rel }); }
            }, 8000);
        });
    }

    function drawProject() {
        var fhost = $('featureList');
        if (fhost) {
            fhost.innerHTML = '';
            var have = featureFolders();
            if (!have.length) {
                fhost.appendChild(el('div', 'sub', 'None yet. Everything is in src/index.ts.'));
            }
            have.forEach(function (f) {
                var b = el('button', 'row');
                b.innerHTML = '<span class="t">' + esc(f) + '</span>' +
                    '<span class="d">src/' + esc(f) + '/index.ts</span>';
                b.onclick = function () {
                    var rel = 'src/' + f + '/index.ts';
                    if (files[rel]) openFile(rel); else call('read', { rel: rel });
                };
                fhost.appendChild(b);
            });
        }

        var host = $('toolList');
        if (!host) return;
        host.innerHTML = '';
        TOOLS.forEach(function (t) {
            var b = el('button', 'row' + (toolBusy === t ? ' busy' : ''));
            b.innerHTML = '<span class="t">' + esc(t.t) + (toolBusy === t ? ' (running)' : '') + '</span>' +
                '<span class="d">' + esc(t.d) + '</span>' +
                (t.care ? '<span class="care">Careful. ' + esc(t.care) + '</span>' : '') +
                '<span class="cmd">' + esc(t.cmd) + '</span>';
            b.onclick = function () { runTool(t); };
            host.appendChild(b);
        });
    }

    $('btnAddFeature').onclick = addFeature;
    $('btnRemoveFeature').onclick = removeFeature;

    // ========================================================================
    // FIND A COMMAND
    //
    // The symbol index is already built from every .d.ts the tool loads, so the
    // whole API is sitting in memory with its documentation attached. Nothing
    // offered it: you could only find a command if you already knew its name,
    // which is exactly what a beginner does not have.
    //
    // It matches on the NAME and on the TEXT OF THE DOCUMENTATION, so "spawn"
    // finds SpawnObject and also everything whose description mentions
    // spawning. Clicking one puts a correctly shaped call at the cursor.
    // ========================================================================
    // api.json when it arrived, the regex index otherwise. The two are not
    // interchangeable: 414 of the 415 commands carry a real description here,
    // and the search matches on the description, so this is most of what makes
    // "spawn a car" find anything at all.
    function apiEntries() {
        var out = [];
        if (API) {
            API.functions.forEach(function (f) {
                // The short shape in the row, because the row is one line and
                // SpawnObject's true signature is longer than this panel.
                out.push({ name: f.name, kind: 'function', sig: shortSignature(f, ''), doc: f.doc || '', entry: f });
            });
            API.enums.forEach(function (e) {
                out.push({
                    name: e.name, kind: 'enum',
                    sig: e.name + ': one of ' + e.members.length + ' choices',
                    doc: e.members.map(function (m) { return m.name; }).join(', '),
                    entry: e
                });
            });
            API.events.forEach(function (e) {
                out.push({ name: e.name, kind: 'event', sig: shortSignature(e, ''), doc: e.doc || '', entry: e });
            });
            return out;
        }
        Object.keys(SYM).forEach(function (name) {
            var s = SYM[name];
            if (!s) return;
            if (s.kind !== 'function' && s.kind !== 'enum' && s.kind !== 'const') return;
            out.push({ name: name, kind: s.kind, sig: s.sig, doc: s.doc || '', file: s.file || '' });
        });
        return out;
    }

    function scoreApi(e, words) {
        var name = e.name.toLowerCase();
        var doc = e.doc.toLowerCase();
        var total = 0;
        for (var i = 0; i < words.length; i++) {
            var w = words[i];
            var n = name.indexOf(w), d = doc.indexOf(w);
            if (n === -1 && d === -1) return -1;          // every word has to land somewhere
            if (name === w) total += 100;
            else if (n === 0) total += 40;
            else if (n > 0) total += 20;
            if (d !== -1) total += 6;
        }
        if (e.kind === 'function') total += 5;            // a command beats a name
        if (e.doc) total += 3;                            // one that explains itself beats one that does not
        if (G.mod[e.name]) total += 8;                    // one this tool can explain in plain words beats both
        // and the ones the shipped projects actually call beat the rest again
        if (USAGE_AT[e.name] !== undefined) total += Math.max(2, 40 - USAGE_AT[e.name] / 5);
        return total;
    }

    function drawApiSearch() {
        var host = $('apiList');
        if (!host) return;
        var q = ($('apiQ') && $('apiQ').value || '').trim().toLowerCase();
        var all = apiEntries();
        host.innerHTML = '';

        if (!all.length) {
            $('apiCount').textContent = 'Nothing loaded yet. The command descriptions arrive a moment after this window opens.';
            return;
        }
        if (!q) {
            $('apiCount').textContent = all.length + ' commands, events and lists of choices are loaded' +
                (API ? ' (Portal API ' + API.version + ')' : '') + '. Type a few letters.';
            return;
        }

        var words = q.split(/\s+/).filter(Boolean);
        var hits = [];
        all.forEach(function (e) {
            var s = scoreApi(e, words);
            if (s >= 0) hits.push({ e: e, s: s });
        });
        hits.sort(function (a, b) { return b.s - a.s || (a.e.name < b.e.name ? -1 : 1); });

        $('apiCount').textContent = hits.length + ' match' + (hits.length === 1 ? '' : 'es') +
            (hits.length > 60 ? ', showing the closest 60' : '');

        hits.slice(0, 60).forEach(function (h) {
            var e = h.e;
            var b = el('button', 'row');
            // An event is not reached through mod: it is a name you export, or
            // a channel on the Events module. Labelling it mod.OnPlayerDied
            // would be teaching the one thing that will not compile.
            var shown = (e.kind === 'event' ? 'Events.' : 'mod.') + e.name;
            var g = G.mod[e.name];
            b.innerHTML = '<span class="t">' + esc(shown) + '</span>' +
                '<span class="cmd">' + esc(e.sig) + '</span>' +
                (g ? '<span class="d">' + esc(g.say) + '</span>' : '') +
                (e.doc ? '<span class="d">' + esc(e.doc.slice(0, 240)) + '</span>'
                       : (g ? '' : '<span class="d">No description came with this one.</span>')) +
                (e.entry && e.entry.deprecated ? '<span class="care">Do not use this. ' + esc(e.entry.deprecated) + '</span>' : '');
            b.onclick = function () { insertApiCall(e); };
            host.appendChild(b);
        });
    }

    // A call with its parameter names left in as placeholders, so what lands in
    // the file says what each slot is for rather than being an empty pair of
    // brackets somebody has to guess at.
    function insertApiCall(e) {
        if (!editor) { say('the editor is not ready yet', 'w'); return; }
        var text;
        if (e.kind === 'event') {
            // The exported handler shape, because that is the form that works
            // with no import: Portal calls an exported On* function by name.
            var ps = (e.entry && e.entry.params) || [];
            text = 'export function ' + e.name + '(' +
                // number and boolean are TypeScript's own; everything else in
                // an event's parameters is an engine type and lives under mod.
                ps.map(function (p) {
                    return p.name + ': ' + (/^[A-Z]/.test(p.type) ? 'mod.' : '') + p.type;
                }).join(', ') + '): void {\n\n}';
        } else if (e.kind === 'function') {
            // Real parameter names where api.json gave us some; the regex
            // index's guess at them otherwise.
            var params = (e.entry && e.entry.params)
                ? e.entry.params.filter(function (p) { return !p.optional; }).map(function (p) { return p.name; })
                : (paramNames(e.sig) || []);
            text = 'mod.' + e.name + '(' + params.join(', ') + ')';
        } else {
            text = 'mod.' + e.name;
        }
        var sel = editor.getSelection();
        editor.executeEdits('apisearch', [{ range: sel, text: text, forceMoveMarkers: true }]);
        var ln = sel.startLineNumber;
        if (e.doc) {
            (NOTE_OVERRIDES[activeRel] || (NOTE_OVERRIDES[activeRel] = {}))[ln] = e.doc;
            scheduleGutter();
        }
        editor.focus();
        say('inserted ' + text, 'g');
    }

    (function wireApiSearch() {
        var box = $('apiQ');
        if (!box) return;
        var t = null;
        box.oninput = function () { clearTimeout(t); t = setTimeout(drawApiSearch, 120); };
        box.onkeydown = function (ev) {
            if (ev.key !== 'Enter') return;
            var first = $('apiList').querySelector('.row');
            if (first) first.click();
        };
    })();

    // ---- LEARN ---------------------------------------------------------------
    function drawRecipes() {
        var host = $('recipeList');
        host.innerHTML = '';
        G.recipes.forEach(function (r) {
            var b = el('button', 'row');
            b.innerHTML = '<span class="t">' + esc(r.title) + '</span><span class="d">' + esc(r.when) + '</span>';
            b.onclick = function () { openRecipe(r); };
            host.appendChild(b);
        });
    }

    function openRecipe(r) {
        var host = $('recipeList');
        host.innerHTML = '';
        var back = el('button', 'row');
        back.innerHTML = '<span class="t">Back to all recipes</span>';
        back.onclick = drawRecipes;
        host.appendChild(back);

        var c = el('div', 'card');
        c.innerHTML = '<div class="t">' + esc(r.title) + '</div><div>' + esc(r.when) + '</div>' +
            (r.warn ? '<div class="w">Careful. ' + esc(r.warn) + '</div>' : '') +
            '<div class="s">Where this comes from: ' + esc(r.source) + '</div>';
        host.appendChild(c);

        var mk = el('button', 'row');
        mk.innerHTML = '<span class="t">Try it as a new project</span><span class="d">Makes a project from the template and writes this in</span>';
        mk.onclick = function () { newProjectFromRecipe(r); };
        host.appendChild(mk);

        var ins = el('button', 'row');
        ins.innerHTML = '<span class="t">Insert into the open file</span><span class="d">Drops the code at the cursor with its notes</span>';
        ins.onclick = function () { insertRecipe(r, true); };
        host.appendChild(ins);

        (r.links || []).forEach(function (lk) {
            var a = el('button', 'row');
            a.innerHTML = '<span class="t">' + esc(lk.label) + '</span><span class="d">Opens in your own browser</span>';
            a.onclick = function () { call('openurl', { url: lk.url }); };
            host.appendChild(a);
        });
    }

    function recipeText(r) {
        return r.lines.map(function (l) { return l[0]; }).join('\n');
    }

    function insertRecipe(r, focus) {
        if (!editor) return;
        var pos = editor.getPosition();
        var text = recipeText(r) + '\n';
        editor.executeEdits('recipe', [{
            range: new monaco.Range(pos.lineNumber, 1, pos.lineNumber, 1),
            text: text,
            forceMoveMarkers: true
        }]);
        // Carry the recipe's own notes onto the lines they landed on.
        var over = NOTE_OVERRIDES[activeRel] || (NOTE_OVERRIDES[activeRel] = {});
        r.lines.forEach(function (l, i) { if (l[1]) over[pos.lineNumber + i] = l[1]; });
        scheduleGutter();
        if (focus) editor.focus();
        say('inserted recipe: ' + r.title, 'g');
        if (r.strings && Object.keys(r.strings).length) {
            say('this recipe needs keys in src/strings.json: ' + JSON.stringify(r.strings), 'w');
        }
    }

    function newProjectFromRecipe(r) {
        var name = (r.id + '-example');
        say('creating project ' + name + ' from the template...');
        call('new', {
            name: name,
            description: r.title,
            boilerplate: 'minimal',
            seedFile: 'src/index.ts',
            seedText: recipeText(r) + '\n',
            seedStrings: JSON.stringify(r.strings || {})
        }).then(function (res) {
            say('project created at ' + res.path, 'g');
            refreshProjects(res.path);
        }, function (e) { say('could not create the project: ' + (e && e.why), 'e'); });
    }

    // ---- SNIPPETS ------------------------------------------------------------
    var snippets = [];
    function onSnippets(json) {
        try {
            var packs = JSON.parse(json);
            snippets = [];
            (packs || []).forEach(function (p) {
                (p.snippets || []).forEach(function (s) { snippets.push(s); });
            });
            drawSnippets();
            say('loaded ' + snippets.length + ' snippets', 'd');
        } catch (e) { say('snippets file is not valid JSON: ' + e.message, 'e'); }
    }

    // Two kinds of example, kept apart on purpose: a WORKED ANSWER was written
    // and compiled against a real project, a PIECE OF CODE is a shape to fill
    // in. Running them together would let a beginner take an unchecked
    // fragment believing it had been proved.
    function drawSnippets() {
        var host = $('snipList');
        if (!host) return;
        host.innerHTML = '';

        var q = ($('snipQ') && $('snipQ').value || '').trim();
        var worked = searchAnswers(q, '', '');

        var h1 = el('div', 'grp');
        h1.innerHTML = 'Worked answers<span class="why">Written out and compiled against a real project. Read one, then take the code or open it as blocks.</span>';
        host.appendChild(h1);
        if (!answers.length) {
            host.appendChild(el('div', 'sub', 'Loading...'));
        } else if (!worked.length) {
            host.appendChild(el('div', 'sub', 'None of the checked answers match those words.'));
        } else {
            worked.slice(0, 12).forEach(function (e) { host.appendChild(answerCard(e, false)); });
            if (worked.length > 12) {
                host.appendChild(el('div', 'sub', worked.length + ' match. Showing the first 12; ASK A QUESTION searches all of them.'));
            }
        }

        var h2 = el('div', 'grp');
        h2.innerHTML = 'Pieces of code<span class="why">Small shapes to drop in and fill out. Placeholders become tab stops.</span>';
        host.appendChild(h2);
        var ql = q.toLowerCase();
        var shown = 0;
        snippets.forEach(function (s) {
            if (ql && (s.label + ' ' + s.note).toLowerCase().indexOf(ql) === -1) return;
            shown++;
            var b = el('button', 'row');
            b.innerHTML = '<span class="t">' + esc(s.label) + '</span><span class="d">' + esc(s.note) + '</span>';
            b.onclick = function () { insertSnippet(s); };
            host.appendChild(b);
        });
        if (!shown) host.appendChild(el('div', 'sub', 'No piece of code matches those words.'));
    }

    // The placeholder format is the one the block editor uses, so a snippet is
    // the same object on both sides:
    //   {{OBJID:kind}}         an object from the scene
    //   {{VAR:name:type}}      a value the user names
    //   {{TEXT:label}}         a key in strings.json
    function snippetToMonaco(body) {
        var n = 0;
        return body.join('\n').replace(/\{\{(OBJID|VAR|TEXT):([^}]*)\}\}/g, function (_all, kind, rest) {
            n++;
            var parts = rest.split(':');
            var hint = kind === 'OBJID' ? (selectedObj ? String(selectedObj.objid) : parts[0])
                : kind === 'TEXT' ? parts[0]
                    : parts[0];
            return '${' + n + ':' + hint + '}';
        });
    }

    function insertSnippet(s) {
        if (!editor) return;
        ensureImports(s.imports || []);
        var start = editor.getPosition().lineNumber;
        var contrib = editor.getContribution('snippetController2');
        if (contrib) contrib.insert(snippetToMonaco(s.body));
        else editor.trigger('bf6', 'type', { text: s.body.join('\n') });

        var over = NOTE_OVERRIDES[activeRel] || (NOTE_OVERRIDES[activeRel] = {});
        (s.explain || []).forEach(function (note, i) { if (note) over[start + i] = note; });
        scheduleGutter();
        say('inserted snippet: ' + s.label, 'g');
        if (s.warn) say('careful: ' + s.warn, 'w');
        if (s.strings && Object.keys(s.strings).length) {
            say('add these to src/strings.json: ' + JSON.stringify(s.strings), 'w');
        }
    }

    function ensureImports(list) {
        if (!list.length || !editor) return;
        var model = editor.getModel();
        var text = model.getValue();
        var add = list.filter(function (l) { return text.indexOf(l) === -1; });
        if (!add.length) return;
        editor.executeEdits('imports', [{
            range: new monaco.Range(1, 1, 1, 1),
            text: add.join('\n') + '\n'
        }]);
        say('added ' + add.length + ' import line' + (add.length === 1 ? '' : 's'), 'd');
    }

    // ========================================================================
    // THE CHECKED ANSWERS
    //
    // Three curated sets on disk (players, systems, presentation), fifteen
    // questions each, every one carrying a short answer, a long one, the
    // gotchas, a TypeScript example that was actually compiled, and a Blockly
    // workspace that actually loads.
    //
    // THEY ARE NOT THE SAME THING AS faq.json AND MUST NEVER READ AS IF THEY
    // WERE. faq.json is mined community threads: wider, unchecked, and useful
    // exactly because nobody edited it. These were written and verified. Both
    // panels keep them in separate groups with separate headings, and a curated
    // card carries the word CHECKED where a thread carries its channel name.
    // ========================================================================
    var answers = [], answersAsked = false;

    function ensureAnswers() {
        if (answersAsked) return;
        answersAsked = true;
        call('answers').then(function (r) {
            if (!r.sets) say('no checked answers are installed with this build', 'd');
        }, function () { });
    }

    function onAnswers(theme, json) {
        try {
            var pack = JSON.parse(json);
            (pack.entries || []).forEach(function (e) {
                e._set = pack.theme || theme;
                answers.push(e);
            });
            say('checked answers: ' + (pack.entries || []).length + ' for ' + (pack.theme || theme), 'g');
            drawSnippets();
            if ($('pane-faq').classList.contains('on')) drawFaq();
            if ($('pane-diagnose').classList.contains('on')) drawDiagnose();
        } catch (e) {
            say('the ' + theme + ' answers file is not valid JSON: ' + e.message, 'e');
        }
    }

    function answerBlob(e) {
        if (!e._blob) {
            e._blob = (e.question + ' ' + (e.short || '') + ' ' + (e.answer || '') + ' ' +
                (e.gotchas || []).join(' ') + ' ' + (e.themes || []).join(' ')).toLowerCase();
        }
        return e._blob;
    }

    // Search across the question, the answer and the gotchas, which is where
    // the words a beginner actually types tend to live.
    function searchAnswers(q, ed, th) {
        var words = String(q || '').trim().toLowerCase().split(/\s+/).filter(Boolean);
        return answers.filter(function (e) {
            if (ed && (e.editors || []).indexOf(ed) === -1 && ed !== 'both') return false;
            if (th && !words.length) {
                var t = th.toLowerCase();
                var any = (e.themes || []).some(function (x) {
                    return t.indexOf(x.toLowerCase()) !== -1 || x.toLowerCase().indexOf(t) !== -1;
                });
                if (!any) return false;
            }
            var blob = answerBlob(e);
            for (var i = 0; i < words.length; i++) if (blob.indexOf(words[i]) === -1) return false;
            return true;
        });
    }

    // The example, put where the cursor is. The three sets were written against
    // plain mod.* calls on purpose: the template's node_modules does not
    // resolve modlib, so an example that used it would look right and then fail
    // to build. If one ever does arrive needing an import, say so out loud
    // rather than letting the build be the one to mention it.
    function insertAnswerTs(e) {
        if (!editor) { say('the editor is not ready yet', 'w'); return; }
        var code = (e.ts && e.ts.code) || '';
        if (!code) { say('that answer has no TypeScript example', 'w'); return; }
        if (/modlib/.test(code)) {
            say('this example uses modlib, which the template does not install. Add it to package.json before you build.', 'w');
        }
        var imports = code.match(/^\s*import\s.*$/gm);
        if (imports) say('this example needs ' + imports.length + ' import line' + (imports.length === 1 ? '' : 's') + '. They came with it.', 'd');

        var sel = editor.getSelection();
        editor.executeEdits('answer', [{ range: sel, text: code, forceMoveMarkers: true }]);
        if (e.ts.note) {
            (NOTE_OVERRIDES[activeRel] || (NOTE_OVERRIDES[activeRel] = {}))[sel.startLineNumber] = e.ts.note;
            scheduleGutter();
        }
        editor.focus();
        say('put the example from "' + clip(e.question, 60) + '" in ' + (activeRel || 'the editor'), 'g');
    }

    // The block example, handed to the block editor exactly the way the UI
    // builder hands its own exports over: the tool writes the snippet where the
    // block editor already looks and asks it to load that name.
    function answerToBlocks(e) {
        var ws = e.blocks && e.blocks.workspace;
        if (!ws || !ws.blocks) { say('that answer has no block example', 'w'); return; }
        var snippet = JSON.stringify({
            name: clip(e.short || e.question, 90),
            note: (e.blocks.note || '') + ' From the checked answer ' + e.id + '.',
            blocks: ws.blocks
        }, null, 1);
        call('toblocks', { id: e.id, snippet: snippet }).then(function (r) {
            say(r.text || 'sent to the block editor', 'g');
        }, function (err) { say('could not hand it over: ' + (err && (err.why || err.text)), 'e'); });
    }

    // One card, two depths. The Examples panel wants the code; the Ask a
    // question panel wants the whole answer.
    function answerCard(e, full) {
        var c = el('div', 'card ok');
        var v = e.verified || {};
        var head = '<span class="pill ok">checked</span>' +
            (e.themes || []).map(function (t) { return '<span class="pill">' + esc(t) + '</span>'; }).join('') +
            '<div class="t" style="margin-top:5px">' + esc(e.question) + '</div>' +
            (e.short ? '<div style="margin-top:4px">' + esc(e.short) + '</div>' : '');
        c.innerHTML = head;

        if (full && e.answer) {
            var a = el('div');
            a.style.marginTop = '6px';
            a.textContent = e.answer;
            c.appendChild(a);
        }
        if (full && (e.gotchas || []).length) {
            var ul = el('ul', 'gots');
            e.gotchas.forEach(function (g) { ul.appendChild(el('li', null, g)); });
            c.appendChild(ul);
        }

        if (e.ts && e.ts.code) {
            var lab = el('div', 'h', 'In TypeScript' + (v.ts ? '' : ' (not verified)'));
            lab.style.marginTop = '10px';
            c.appendChild(lab);
            if (e.ts.note) c.appendChild(el('div', 'sub', e.ts.note));
            var pre = el('pre', 'code');
            pre.textContent = full ? e.ts.code : clip(e.ts.code, 700);
            c.appendChild(pre);
            var act = el('div', 'act');
            var b1 = el('button', null, 'Put this in my file');
            b1.onclick = function (ev) { ev.stopPropagation(); insertAnswerTs(e); };
            act.appendChild(b1);
            c.appendChild(act);
        }

        if (e.blocks && e.blocks.workspace) {
            var lab2 = el('div', 'h', 'As blocks' + (v.blocks ? '' : ' (not verified)'));
            lab2.style.marginTop = '10px';
            c.appendChild(lab2);
            if (e.blocks.note) c.appendChild(el('div', 'sub', e.blocks.note));
            var act2 = el('div', 'act');
            var b2 = el('button', null, 'Open this in the block editor');
            b2.onclick = function (ev) { ev.stopPropagation(); answerToBlocks(e); };
            act2.appendChild(b2);
            c.appendChild(act2);
        }

        if (full && (e.sources || []).length) {
            var s = el('div', 's');
            s.textContent = 'Written from ' + e.sources.length + ' community thread' +
                (e.sources.length === 1 ? '' : 's') + ': ' + e.sources.join('; ');
            c.appendChild(s);
        }
        return c;
    }

    // ---- FAQ -----------------------------------------------------------------
    var faq = null, faqLoaded = false;
    function onFaq(json) {
        try {
            faq = JSON.parse(json);
            var sel = $('faqTheme');
            sel.innerHTML = '<option value="">Any theme</option>';
            (faq.themes || []).forEach(function (t) {
                var o = el('option', null, t + ' (' + (faq.counts.byTheme[t] || 0) + ')');
                o.value = t; sel.appendChild(o);
            });
            say('community answers loaded: ' + faq.entries.length + ' questions', 'g');
            drawFaq();
        } catch (e) { say('faq.json could not be read: ' + e.message, 'e'); }
    }

    function drawFaq() {
        var q = $('faqQ').value.trim().toLowerCase();
        var th = $('faqTheme').value;
        var ed = $('faqEditor').value;
        var host = $('faqList');
        host.innerHTML = '';

        // The checked answers first, and labelled as what they are.
        var curated = searchAnswers(q, ed, th);
        var h1 = el('div', 'grp');
        h1.innerHTML = 'Checked answers<span class="why">Written out and verified, with an example you can take.</span>';
        host.appendChild(h1);
        if (!answers.length) host.appendChild(el('div', 'sub', 'Loading...'));
        else if (!curated.length) host.appendChild(el('div', 'sub', 'None of the checked answers cover that. The threads below might.'));
        else curated.slice(0, 10).forEach(function (e) { host.appendChild(answerCard(e, true)); });

        var h2 = el('div', 'grp');
        h2.innerHTML = 'Threads from the community<span class="why">What people actually asked and what they were told. Nobody checked these, and some of them are wrong.</span>';
        host.appendChild(h2);

        if (!faq) {
            host.appendChild(el('div', 'sub', 'Loading...'));
            $('faqCount').textContent = curated.length + ' checked answer' + (curated.length === 1 ? '' : 's');
            return;
        }

        var shown = 0, matched = 0;
        for (var i = 0; i < faq.entries.length; i++) {
            var e = faq.entries[i];
            if (th && e.th !== th) continue;
            if (ed && e.ed !== ed) continue;
            if (q) {
                var blob = (e.q + ' ' + e.a.join(' ') + ' ' + e.tt).toLowerCase();
                if (blob.indexOf(q) === -1) continue;
            }
            matched++;
            if (shown >= 40) continue;
            shown++;
            var c = el('div', 'card');
            c.innerHTML =
                '<span class="pill">' + esc(e.th) + '</span><span class="pill">' + esc(e.ed) + '</span>' +
                '<div class="t" style="margin-top:5px">' + esc(clip(e.q, 260)) + '</div>' +
                e.a.map(function (a) { return '<div style="margin-top:4px">' + esc(clip(a, 420)) + '</div>'; }).join('') +
                '<div class="s">' + esc(e.ch) + ' / ' + esc(e.tt) + '</div>';
            host.appendChild(c);
        }
        $('faqCount').textContent =
            curated.length + ' checked answer' + (curated.length === 1 ? '' : 's') + ', ' +
            matched + ' thread' + (matched === 1 ? '' : 's') +
            (matched > shown ? ', showing the first ' + shown : '');
    }
    function clip(s, n) { return s.length > n ? s.slice(0, n) + '...' : s; }

    $('faqQ').oninput = drawFaq;
    $('faqTheme').onchange = drawFaq;
    $('faqEditor').onchange = drawFaq;
    $('snipQ').oninput = drawSnippets;

    // A checked answer offered beside a finding, when one of them is about the
    // same subject. At most two, because a wall of suggestions beside an error
    // message is another wall.
    function answerLink(host, text) {
        if (!answers.length) return;
        var th = themeFor(text);
        var words = String(text).toLowerCase().split(/[^a-z]+/).filter(function (w) { return w.length > 4; });
        var hits = answers.filter(function (e) {
            if (th && (e.themes || []).some(function (x) {
                return th.toLowerCase().indexOf(x.toLowerCase()) !== -1 || x.toLowerCase().indexOf(th.toLowerCase()) !== -1;
            })) return true;
            var blob = answerBlob(e);
            var n = 0;
            for (var i = 0; i < words.length; i++) if (blob.indexOf(words[i]) !== -1) n++;
            return n >= 2;
        }).slice(0, 2);

        hits.forEach(function (e) {
            var a = el('button', 'link', 'Checked answer: ' + clip(e.question, 80));
            a.onclick = function (ev) {
                ev.stopPropagation();
                showPane('faq');
                $('faqTheme').value = '';
                $('faqEditor').value = '';
                $('faqQ').value = e.question.split(/\s+/).slice(0, 4).join(' ');
                drawFaq();
            };
            host.appendChild(a);
        });
    }

    // A theme guess for a diagnostic or a finding, so the panel can offer the
    // community's answers on the same subject.
    function themeFor(text) {
        if (!faq) return null;
        var t = String(text).toLowerCase();
        var map = [
            ['capture points', ['capturepoint', 'capture point', 'hq', 'conquest', 'objective']],
            ['bots and AI', ['ai', 'bot', 'spawner']],
            ['area triggers', ['areatrigger', 'area', 'interact', 'combat area', 'polygon']],
            ['vehicles', ['vehicle', 'spawner']],
            ['sound and VO', ['sound', 'sfx', 'playvo', 'music']],
            ['ui', ['ui', 'widget', 'anchor', 'hud', 'notification']],
            ['strings and localisation', ['strings.json', 'stringkeys', 'message']],
            ['teams', ['team', 'setteam']],
            ['spawn', ['spawn', 'deploy', 'undeploy']],
            ['timers and wait', ['wait', 'timer', 'interval', 'ongoing', 'tick']],
            ['events', ['subscribe', 'event', 'handler']],
            ['deploy and save errors', ['bundle', 'build', 'upload', 'deploy script']]
        ];
        for (var i = 0; i < map.length; i++) {
            for (var j = 0; j < map[i][1].length; j++) {
                if (t.indexOf(map[i][1][j]) !== -1) return map[i][0];
            }
        }
        return null;
    }

    function faqLink(host, text) {
        var th = themeFor(text);
        if (!th) return;
        var a = el('button', 'link', 'Community answers on ' + th);
        a.onclick = function () {
            showPane('faq');
            if (!faqLoaded) { faqLoaded = true; call('faq'); }
            $('faqTheme').value = th;
            $('faqQ').value = '';
            drawFaq();
        };
        host.appendChild(a);
    }

    // ---- EXPLAIN -------------------------------------------------------------
    $('btnExplain').onclick = function () {
        var sel = editor && editor.getSelection();
        if (!sel || sel.isEmpty()) { explainRange(1, Math.min(60, editor.getModel().getLineCount()), 'The whole file'); return; }
        explainRange(sel.startLineNumber, sel.endLineNumber, 'Your selection');
    };

    function explainRange(from, to, title) {
        showPane('explain');
        var host = $('explainOut');
        host.innerHTML = '';
        if (!editor) return;
        var model = editor.getModel();
        to = Math.min(to, model.getLineCount());

        var head = el('div', 'card');
        head.innerHTML = '<div class="t">' + esc(title) + '</div><div class="s">Lines ' + from + ' to ' + to + '</div>';
        host.appendChild(head);

        var decorations = [];
        for (var ln = from; ln <= to; ln++) {
            var raw = model.getLineContent(ln);
            if (!raw.trim()) continue;
            var over = (NOTE_OVERRIDES[activeRel] || {})[ln];
            var note = over ? { text: over } : explainLine(raw, activeRel);
            var c = el('div', 'card');
            var body = '<div style="font-family:Consolas,monospace;color:#aec0cc;font-size:11px;white-space:pre-wrap">' +
                esc(String(ln).padStart(4, ' ')) + '  ' + esc(raw.trim()) + '</div>';
            if (note && note.text) body += '<div style="margin-top:4px">' + esc(note.text) + '</div>';
            else body += '<div style="margin-top:4px;color:#75828a">Nothing special: this line is structure rather than an action.</div>';
            if (note && note.warn) body += '<div class="w">Careful. ' + esc(note.warn) + '</div>';
            c.innerHTML = body;
            if (note && note.link) {
                var a = el('button', 'link', note.label || 'Read more');
                a.onclick = (function (u) { return function () { call('openurl', { url: u }); }; })(note.link);
                c.appendChild(a);
            }
            faqLink(c, raw);
            c.onmouseenter = (function (l) {
                return function () {
                    decorations = editor.deltaDecorations(decorations, [{
                        range: new monaco.Range(l, 1, l, 1),
                        options: { isWholeLine: true, className: 'bf6-explain-line' }
                    }]);
                    editor.revealLineInCenterIfOutsideViewport(l);
                };
            })(ln);
            host.appendChild(c);
        }
    }

    // ---- DIAGNOSE ------------------------------------------------------------
    function drawDiagnose() {
        var host = $('diagOut');
        host.innerHTML = '';

        drawPortalResult(host);

        if (lastDiags.length) {
            host.appendChild(headEl('Errors in this file', lastDiags.length + ' from the compiler'));
            lastDiags.slice(0, 30).forEach(function (m) {
                var e = explainDiag(m);
                var c = el('div', 'card ' + (m.severity === monaco.MarkerSeverity.Error ? 'bad' : 'warn'));
                c.innerHTML = '<div class="t">Line ' + m.startLineNumber + (m.code ? ' (TS' + m.code + ')' : '') + '</div>' +
                    '<div><b>What it means.</b> ' + esc(e.cause) + '</div>' +
                    '<div style="margin-top:4px"><b>What to do.</b> ' + esc(e.fix) + '</div>' +
                    '<div class="s">' + esc(m.message) + '</div>';
                c.onclick = function () {
                    editor.revealLineInCenter(m.startLineNumber);
                    editor.setPosition({ lineNumber: m.startLineNumber, column: m.startColumn });
                    editor.focus();
                };
                faqLink(c, m.message);
                answerLink(c, m.message);
                host.appendChild(c);
            });
        } else if (mode === 'index') {
            host.appendChild(cardEl('warn', 'Live error checking is off',
                'The language service worker could not start on this page, so nothing is underlined as you type. BUILD still reports every real error, and CHECK MY SCRIPT still finds the traps the compiler cannot see.'));
        } else {
            host.appendChild(cardEl('ok', 'No compiler errors', 'Nothing in the open file is wrong as far as TypeScript is concerned. That is not the same as working, so run CHECK MY SCRIPT as well.'));
        }

        var found = checkScript();
        host.appendChild(headEl('Known traps', found.length ? found.length + ' to look at' : 'none found'));
        if (!found.length) {
            host.appendChild(cardEl('ok', 'Nothing caught', 'None of the shapes that compile cleanly and then fail in the game turned up in your files.'));
        }
        found.forEach(function (f) {
            var c = el('div', 'card warn');
            c.innerHTML = '<div class="t">' + esc(f.p.title) + '</div>' +
                '<div>' + esc(f.rel) + ' line ' + f.line + '</div>' +
                '<div style="margin-top:4px"><b>Why it matters.</b> ' + esc(f.p.why) + '</div>' +
                '<div style="margin-top:4px"><b>The fix.</b> ' + esc(f.p.fix) + '</div>' +
                (f.extra ? '<div style="margin-top:4px">' + esc(f.extra) + '</div>' : '') +
                '<div class="s">' + (f.p.verified ? 'Source: ' : 'UNVERIFIED. ') + esc(f.p.source) + '</div>';
            c.onclick = function () {
                if (files[f.rel]) { openFile(f.rel); editor.revealLineInCenter(f.line); }
            };
            if (f.p.recipe) {
                var b = el('button', 'link', 'Open the recipe');
                b.onclick = function (ev) {
                    ev.stopPropagation();
                    var r = G.recipes.filter(function (x) { return x.id === f.p.recipe; })[0];
                    if (r) { showPane('learn'); openRecipe(r); }
                };
                c.appendChild(b);
            }
            faqLink(c, f.p.title + ' ' + f.p.why);
            answerLink(c, f.p.title + ' ' + f.p.why + ' ' + f.p.fix);
            host.appendChild(c);
        });

        if (logLines.length) {
            host.appendChild(headEl('PortalLog', logLines.length + ' line' + (logLines.length === 1 ? '' : 's') + ' read'));
            logLines.slice(-40).reverse().forEach(function (L) {
                var c = el('div', 'card ' + (L.kind === 'error' ? 'bad' : ''));
                c.innerHTML = '<div style="font-family:Consolas,monospace;font-size:11px;white-space:pre-wrap">' + esc(L.raw) + '</div>' +
                    (L.say ? '<div style="margin-top:4px">' + esc(L.say) + '</div>' : '') +
                    (L.line ? '<div class="s">bundle line ' + L.line + '. Your source line is not the same number: the bundler joins every file into one.</div>' : '');
                host.appendChild(c);
            });
        }
    }

    function headEl(title, sub) {
        var d = document.createDocumentFragment();
        var h = el('div', 'h', title); d.appendChild(h);
        if (sub) d.appendChild(el('div', 'sub', sub));
        return d;
    }
    function cardEl(cls, title, body) {
        var c = el('div', 'card ' + cls);
        c.innerHTML = '<div class="t">' + esc(title) + '</div><div>' + esc(body) + '</div>';
        return c;
    }

    $('btnCheck').onclick = function () { showPane('diagnose'); drawDiagnose(); };

    var tailing = false;
    $('btnTail').onclick = function () {
        tailing = !tailing;
        $('btnTail').classList.toggle('on', tailing);
        $('btnTail').textContent = tailing ? 'Stop watching the game\'s log' : 'Watch the game\'s log';
        call('logtail', { on: tailing }).then(function (r) {
            say(tailing ? ('watching ' + r.path) : 'stopped watching PortalLog', tailing ? 'g' : 'd');
            if (tailing && r.missing) {
                say('that file does not exist yet. It is written only while you use Host Locally in the game.', 'w');
            }
        }, function (e) { say('could not watch the log: ' + (e && e.why), 'e'); });
    };

    var logLines = [];
    function onPortalLog(lines) {
        lines.forEach(function (raw) {
            var entry = { raw: raw, kind: 'log', say: '', line: 0 };
            for (var i = 0; i < G.loglines.length; i++) {
                var p = G.loglines[i];
                var m = raw.match(p.re);
                if (!m) continue;
                entry.kind = p.kind;
                entry.say = p.say;
                if (/\d+/.test(m[m.length - 1] || '')) {
                    var n = parseInt(m[m.length - 1], 10);
                    if (!isNaN(n)) entry.line = n;
                }
                break;
            }
            logLines.push(entry);
            if (logLines.length > 400) logLines.shift();
            say(raw, entry.kind === 'error' ? 'e' : 'd');
        });
        if ($('pane-diagnose').classList.contains('on')) drawDiagnose();
    }

    // ========================================================================
    // THE BANNER: session loss, a bundle waiting to be saved, a refusal.
    // ========================================================================
    //
    // NAMED buildPending, NOT pending. This file is one long function, so a
    // second "var pending" here was not a second variable: var hoists, and both
    // this and the bridge's table of unanswered calls were the same slot. The
    // declaration below reset the request table on the way past, and onPending
    // replaced it outright, so any call still waiting for its reply when a
    // status event arrived lost its resolve/reject and its promise never
    // settled. That is the build that finishes and leaves the buttons disabled
    // forever. Keep these two names apart.
    var buildPending = { state: 'none' };
    var signedOut = false;

    function banner(cls, msg, sub, actions) {
        var b = $('banner');
        b.className = cls || '';
        b.innerHTML = '';
        b.appendChild(el('div', 'msg', msg));
        (actions || []).forEach(function (a) {
            var btn = el('button', a.primary ? 'primary' : null, a.label);
            btn.onclick = a.run;
            b.appendChild(btn);
        });
        if (sub) b.appendChild(el('div', 'sub2', sub));
        b.hidden = false;
    }
    function clearBanner() { $('banner').hidden = true; }

    function refreshBanner() {
        if (signedOut) {
            banner('bad', 'Portal signed you out. Your script is safe here.',
                'Everything you have written is on disk in this project, autosaved, with the last ten versions of each file kept. Sign in again on the Portal panel; the tool will offer to push again.',
                [{ label: 'Open the Portal panel', primary: true, run: function () { call('opensite'); } }]);
            return;
        }
        if (buildPending.state === 'built' || buildPending.state === 'pushed') {
            var what = buildPending.state === 'pushed'
                ? 'Your bundle is in the Portal page but has not been saved there.'
                : 'A bundle is built and has not been put on Portal.';
            banner('warn', what, (buildPending.detail || '') + (buildPending.at ? '  Built at ' + buildPending.at + '.' : ''),
                [{ label: 'Push again', primary: true, run: function () { $('btnPush').click(); } },
                 { label: 'Dismiss', run: clearBanner }]);
            return;
        }
        clearBanner();
    }

    function onPending(e) {
        buildPending = e || { state: 'none' };
        refreshBanner();
    }

    function onSession(e) {
        if (e.state === 'lost') {
            signedOut = true;
            $('btnPush').disabled = true;
            $('btnPull').disabled = true;
            $('btnSavePortal').disabled = true;
            say('Portal signed you out. Nothing was lost: your script is on disk here.', 'w');
        } else {
            signedOut = false;
            say('Portal is signed in again.', 'g');
        }
        refreshBanner();
    }

    // ========================================================================
    // THE SITE'S VERDICT ON A SAVE.
    //
    // A refusal from Portal is about a line in the BUNDLE. The tool maps it
    // back to the file the user actually wrote and puts a marker there, tagged
    // Portal so it can be told apart from the compiler's own and cleared on
    // the next clean save. Anything that could not be mapped is shown whole,
    // with a copy button, rather than pinned to a line it may not belong to.
    // ========================================================================
    var portalVerdict = null;

    function clearPortalMarkers() {
        if (!monacoReady) return;
        Object.keys(files).forEach(function (rel) {
            try { monaco.editor.setModelMarkers(files[rel].model, 'portal', []); } catch (e) { }
        });
    }

    function onPortalVerdict(v) {
        portalVerdict = v;
        clearPortalMarkers();

        if (v.ok) {
            $('stPortalSave').textContent = 'Saved on Portal at ' + v.at;
            $('stPortalSave').style.color = '#4caf50';
            say('Portal saved it at ' + v.at + '.', 'g');
            refreshBanner();
            if ($('pane-diagnose').classList.contains('on')) drawDiagnose();
            return;
        }

        $('stPortalSave').textContent = 'Portal refused the save at ' + v.at;
        $('stPortalSave').style.color = '#f43c30';

        // Group the mapped ones by file and set them as markers.
        var byFile = {};
        (v.mapped || []).forEach(function (m) {
            (byFile[m.rel] || (byFile[m.rel] = [])).push(m);
        });
        Object.keys(byFile).forEach(function (rel) {
            if (!files[rel]) return;
            var model = files[rel].model;
            try {
                monaco.editor.setModelMarkers(model, 'portal', byFile[rel].map(function (m) {
                    return {
                        startLineNumber: m.line, startColumn: 1,
                        endLineNumber: m.line, endColumn: model.getLineMaxColumn(m.line),
                        message: 'Portal: ' + m.message,
                        severity: monaco.MarkerSeverity.Error,
                        source: 'Portal'
                    };
                }));
            } catch (e) { say('could not mark ' + rel + ': ' + e.message, 'w'); }
        });

        var n = (v.mapped || []).length, u = (v.unmapped || []).length;
        say('Portal refused the save' + (v.grpcMessage ? ': ' + v.grpcMessage : '') +
            '. ' + n + ' error' + (n === 1 ? '' : 's') + ' marked in your files' +
            (u ? ', ' + u + ' could not be traced back to a line' : '') + '.', 'e');
        showPane('diagnose');
        drawDiagnose();
        refreshBanner();
    }

    function drawPortalResult(host) {
        var v = portalVerdict;
        if (!v) return;
        host.appendChild(headEl('Portal result', v.ok ? 'saved at ' + v.at : 'refused at ' + v.at));

        if (v.ok) {
            host.appendChild(cardEl('ok', 'Portal accepted it', 'The site saved your script at ' + v.at + '. Restart or rehost the experience to play the new version; Portal does not hot reload.'));
            return;
        }

        var head = el('div', 'card bad');
        var status = (v.grpcStatus >= 0) ? ('status ' + v.grpcStatus + (v.grpcStatus === 3 ? ' (INVALID_ARGUMENT)' : '')) : 'no status read';
        head.innerHTML = '<div class="t">Portal would not save this</div>' +
            '<div>' + esc(v.grpcMessage || v.toast || 'The site refused it and gave no message.') + '</div>' +
            '<div class="s">' + esc(status) + '. Read from: ' + esc(v.where || 'nothing') +
            '. Line map covers ' + (v.mapKnown || 0) + ' source file' + ((v.mapKnown === 1) ? '' : 's') + '.</div>';
        var copy = el('button', 'link', 'Copy the raw text');
        copy.onclick = function () {
            var raw = JSON.stringify({ status: v.grpcStatus, message: v.grpcMessage, toast: v.toast, unmapped: v.unmapped }, null, 2);
            try { navigator.clipboard.writeText(raw); say('copied', 'd'); }
            catch (e) { say(raw, 'd'); }
        };
        head.appendChild(copy);
        host.appendChild(head);

        (v.mapped || []).forEach(function (m) {
            var c = el('div', 'card bad');
            var e2 = { cause: m.message, fix: 'Open the line and read it beside the message. If it names a string key, it is almost always a key that is not in src/strings.json.' };
            for (var code in G.errors) {
                if (m.message.indexOf('TS' + code) !== -1 || m.message.indexOf('(' + code + ')') !== -1) { e2 = G.errors[code]; break; }
            }
            c.innerHTML = '<div class="t">' + esc(m.rel) + ' line ' + m.line + '</div>' +
                '<div>' + esc(m.message) + '</div>' +
                '<div style="margin-top:4px"><b>What it means.</b> ' + esc(e2.cause) + '</div>' +
                '<div style="margin-top:4px"><b>What to do.</b> ' + esc(e2.fix) + '</div>' +
                '<div class="s">Portal said this about bundle line ' + m.bundleLine + '.</div>';
            c.onclick = function () {
                if (files[m.rel]) { openFile(m.rel); editor.revealLineInCenter(m.line); editor.setPosition({ lineNumber: m.line, column: 1 }); editor.focus(); }
                else call('read', { rel: m.rel });
            };
            faqLink(c, m.message);
            host.appendChild(c);
        });

        (v.unmapped || []).forEach(function (m) {
            var c = el('div', 'card warn');
            c.innerHTML = '<div class="t">Not traced to a line</div>' +
                '<div>' + esc(m.message) + '</div>' +
                '<div class="s">Portal said this about bundle line ' + m.bundleLine + ', which the line map could not place. That happens where the bundler rewrote a file rather than copying it through.</div>';
            host.appendChild(c);
        });
    }

    // ---- WALKTHROUGH ---------------------------------------------------------
    var WALK = [
        {
            t: 'Make a project',
            d: 'Copies Mike De Luca\'s template into this experience\'s own folder, fills in its name for you, and installs what it needs.',
            btn: 'Make it', run: function () { $('btnNew').click(); },
            done: function () { return !!project; }
        },
        {
            t: 'Write your first rule',
            d: 'Open src/index.ts and put one rule in it. The MY FIRST RULE recipe is six lines and shows text on screen when somebody spawns.',
            btn: 'Open the recipe', run: function () { showPane('learn'); var r = G.recipes[0]; if (r) openRecipe(r); },
            done: function () { return !!(files['src/index.ts'] && files['src/index.ts'].model.getValue().indexOf('subscribe') !== -1); }
        },
        {
            t: 'Build it',
            d: 'Turns everything under src into one dist/bundle.ts and one dist/bundle.strings.json. Errors show in the panel below.',
            btn: 'Build', run: function () { $('btnBuild').click(); },
            done: function () { return builtOnce; }
        },
        {
            t: 'Put it on Portal',
            d: 'Open the Portal panel on your experience\'s Script page, then press PUSH TO PORTAL here. The Save on the site is still your own press.',
            btn: 'Open the script page', run: function () { call('opensite'); },
            done: function () { return pushedOnce; }
        },
        {
            t: 'Host it locally and watch',
            d: 'Start the experience with Host Locally in the game. The game writes PortalLog to your Temp folder, and WATCH PORTALLOG in the DIAGNOSE panel reads it live.',
            btn: 'Watch the log', run: function () { showPane('diagnose'); if (!tailing) $('btnTail').click(); },
            done: function () { return logLines.length > 0; }
        }
    ];
    var builtOnce = false, pushedOnce = false;

    function drawWalk() {
        var host = $('walkList');
        host.innerHTML = '';
        WALK.forEach(function (s) {
            var isDone = false;
            try { isDone = s.done(); } catch (e) { }
            var li = el('li', isDone ? 'done' : '');
            li.innerHTML = '<div class="t" style="font-weight:600;color:#aec0cc">' + esc(s.t) + '</div>' +
                '<div style="color:#75828a;margin:3px 0 5px">' + esc(s.d) + '</div>';
            var b = el('button', null, isDone ? 'Do it again' : s.btn);
            b.onclick = s.run;
            li.appendChild(b);
            host.appendChild(li);
        });
    }

    // ========================================================================
    // TOOLBAR
    // ========================================================================
    // The explanation gutter stays ON by default. It is the thing that teaches,
    // so it is the one helper that is not hidden behind a handle.
    $('btnGuided').onclick = function () {
        guided = !guided;
        $('btnGuided').classList.toggle('on', guided);
        if (aiMode === 'ai') { drawAi(); } else { buildGutter(); }
        var g = $('gutter');
        if (g) { g.className = guided ? 'on' : ''; }
        setPref('guided', guided);
    };

    // ---- the AI half of the right column -------------------------------------
    if ($('sideExplain')) { $('sideExplain').onclick = function () { showSide('explain'); }; }
    if ($('sideAi')) { $('sideAi').onclick = function () { showSide('ai'); }; }
    if ($('askClose')) { $('askClose').onclick = closeAsk; }
    if ($('askSend')) { $('askSend').onclick = askSend; }
    if ($('askCopy')) {
        $('askCopy').onclick = function () {
            var q = ($('askText') || {}).value || '';
            var text = briefingFor(q, askAnchor || captureAnchor());
            copyText(text);
            say('the whole briefing is on the clipboard: ' + Math.round(text.length / 1024)
                + ' KB. Paste it into whatever you already talk to.', 'o');
        };
    }
    if ($('askText')) {
        $('askText').addEventListener('keydown', function (ev) {
            // Enter makes a new line, because questions run to more than one.
            // Ctrl+Enter sends, which is what every other box like this does.
            if (ev.key === 'Enter' && (ev.ctrlKey || ev.metaKey)) { ev.preventDefault(); askSend(); }
            if (ev.key === 'Escape') { ev.preventDefault(); closeAsk(); }
        });
    }

    $('btnSave').onclick = saveActive;

    $('btnBuild').onclick = function () {
        // Anything typed into the Text tab in the last moment is on a
        // 600 ms timer; a build that did not wait for it would bundle
        // the previous text and look like the edit never happened.
        strFlush();
        if (!project) { say('open or create a project first', 'w'); return; }
        saveAllDirty().then(function () {
            say('building...');
            return call('build');
        }).then(function (r) {
            builtOnce = true;
            say(r.text || 'build finished', r.ok === false ? 'e' : 'g');
            drawWalk();
        }, function (e) {
            // saveAllDirty has already said exactly which file and why; repeating
            // it as "build failed" would point at the wrong step.
            if (!(e && e.saveFailed)) {
                say('build failed: ' + (e && (e.text || e.why)), 'e');
                showPane('diagnose');
            }
        });
    };

    $('btnInstall').onclick = function () {
        if (!project) { say('open or create a project first', 'w'); return; }
        say('running npm install, this takes a minute the first time...');
        call('install').then(function (r) { say(r.text || 'install finished', 'g'); refreshTypes(); },
            function (e) { say('install failed: ' + (e && (e.text || e.why)), 'e'); });
    };

    $('btnNew').onclick = function () {
        var name = window.prompt('Name for the new script project');
        if (!name) return;
        var desc = window.prompt('One line describing it (optional)') || '';
        var kind = window.confirm('Start from the EXAMPLE experience (telemetry and vehicle spawning)?\n\nCancel gives you the minimal boilerplate, which is the better place to begin.')
            ? 'example' : 'minimal';
        say('creating ' + name + '...');
        call('new', { name: name, description: desc, boilerplate: kind }).then(function (r) {
            say('created at ' + r.path, 'g');
            // A fresh project opens on src/index.ts with the plain words beside
            // it and START HERE showing the one thing to do next. It must never
            // open on an empty window with nothing pointing anywhere.
            refreshProjects(r.path).then(function () {
                refreshFirstRun();
                if (!guided) $('btnGuided').click();
                showPane('walk');
            }, function () { refreshFirstRun(); });
        }, function (e) { say('could not create it: ' + (e && e.why), 'e'); });
    };

    // CREATING IS NOT THE SAME OPERATION AS REPLACING.
    //
    // This wrote a one line stub at whatever name was typed. Type index.ts,
    // which is the most likely name anybody types, and the project's entry
    // point became "// src/index.ts" with no prompt and nothing to undo: the
    // model for that file was not even open, so Monaco had no history of it.
    //
    // Reading the file first was NOT enough, and the reason is worth spelling
    // out because it is easy to write again. A read that FAILS is not evidence
    // that a file is absent: it fails for a locked file, for a refused
    // permission, for a path the host would not take, and for a reply that
    // never arrived. Every one of those fell through to the write. And a read
    // that SUCCEEDS does not carry the text in its reply at all - the text
    // arrives afterwards as a payload - so `r.content` was always undefined and
    // the file was opened blank on the way to being replaced anyway.
    //
    // So existence is settled from the host's own file listing, which is a
    // positive statement about what is on disk rather than the absence of an
    // error. Nothing is written unless that listing came back and does not have
    // the name in it. Windows compares filenames without case, so INDEX.TS is
    // index.ts here too.
    //
    // The listing covers .ts and .json under src and nothing else, so those are
    // the only extensions this can offer: for anything else it could not prove
    // absence, and "I could not check" must never turn into "so I will write
    // over it".
    $('btnNewFile').onclick = function () {
        if (!project) { say('open or create a project first', 'w'); return; }
        var name = window.prompt('New file under src, for example rules/scoring.ts');
        if (!name) return;
        var clean = String(name).replace(/^[\/\\]+/, '').replace(/^src[\/\\]/, '').replace(/\\/g, '/');
        if (!clean || /(^|\/)\.\.($|\/)/.test(clean) || /^[A-Za-z]:/.test(clean)) {
            say('that is not a name inside src: ' + name, 'e');
            return;
        }
        if (!/\.[A-Za-z0-9]+$/.test(clean)) clean += '.ts';
        var rel = 'src/' + clean;
        var body = '// ' + rel + '\n';
        /* THE HOST DECIDES WHETHER THE NAME IS FREE.
         *
         * This used to check here and write in a second call, which left a gap
         * between the two, and the extension restriction existed only because
         * the file listing could not speak for anything else. The `newfile` op
         * tests existence next to the write, refuses with reason "exists", and
         * never replaces anything, so the check no longer has to happen at a
         * distance and any extension is safe to offer. */
        call('newfile', { rel: rel, text: body }).then(function () {
            openFile(rel, body);
            refreshFiles();
        }, function (e) {
            if (e && e.reason === 'exists') {
                /* Opening it is what the user almost certainly wanted, and it is
                 * the one outcome that cannot destroy anything. A file open with
                 * unsaved edits is focused, not reloaded. */
                say(rel + ' already exists, so it has been opened rather than replaced.', 'w');
                if (files[rel]) { openFile(rel); return; }
                return call('read', { rel: rel }).catch(function (e2) {
                    say('could not open ' + rel + ': ' + (e2 && (e2.why || e2.error)), 'e');
                });
            }
            say('could not create the file: ' + (e && (e.why || e.error)) + '. Nothing was changed.', 'e');
        });
    };

    $('projSel').onchange = function () {
        var path = $('projSel').value;
        // A failed flush and a refused overlapping switch have both already
        // said what happened, and said it better than this line can.
        if (path) openProject(path).catch(function (e) { if (!(e && (e.saveFailed || e.busy))) { say('could not open: ' + (e && e.why), 'e'); } });
    };

    // ---- Portal sync ---------------------------------------------------------
    $('btnPush').onclick = function () {
        if (!project) { say('open or create a project first', 'w'); return; }
        // A stale refusal must not survive the push that answers it.
        clearPortalMarkers();
        portalVerdict = null;
        $('stPortalSave').textContent = '';
        saveAllDirty().then(function () { return call('build'); }).then(function () {
            return call('push');
        }).then(function (r) {
            pushedOnce = true;
            say(r.text || 'pushed to the Portal page', 'g');
            if (r.stringsTab === false) {
                say('the page has no separate strings tab, so bundle.strings.json was not placed. Upload it through MANAGE SCRIPTS on the site.', 'w');
            }
            drawWalk();
        }, function (e) {
            if (!(e && e.saveFailed)) { say('push failed: ' + (e && (e.why || e.text)), 'e'); }
        });
    };

    $('btnPull').onclick = function () {
        call('pull').then(function (r) {
            say(r.text || 'pulled from the Portal page', 'g');
            // Replacing the local copy IS what PULL is for, so this is the one
            // caller allowed past openFile's unsaved-work guard. It still has to
            // ask first: the text it is about to replace was never written
            // anywhere, so nothing would be left of it afterwards.
            if (r.rel) {
                var f = files[r.rel];
                if (f && f.dirty && r.content != null && f.model.getValue() !== r.content &&
                    !window.confirm(r.rel + ' has unsaved changes here.\n\nReplace them with the copy from the Portal page?')) {
                    say('kept your unsaved ' + r.rel + '. Nothing from the site was put over it.', 'w');
                    openFile(r.rel);
                } else {
                    openFile(r.rel, r.content, true);
                }
            }
            refreshFiles();
        }, function (e) { say('pull failed: ' + (e && (e.why || e.text)), 'e'); });
    };

    $('btnSavePortal').onclick = function () {
        if (!window.confirm('Press the Save button on the Portal page?\n\nThis saves your script onto the experience.')) return;
        call('sitesave').then(function (r) { say(r.text || 'the site reported: saved', 'g'); },
            function (e) { say('save on the site failed: ' + (e && (e.why || e.text)), 'e'); });
    };

    function onSiteState(e) {
        $('stSite').textContent = e.onScriptPage
            ? ('script page' + (e.hasMonaco ? ', editor found' : ', editor not found yet'))
            : (e.url ? 'not on a script page' : 'panel not open');
        var can = !!e.onScriptPage && !signedOut;
        $('btnPush').disabled = !can;
        $('btnPull').disabled = !can;
        $('btnSavePortal').disabled = !can;
        if (e.onScriptPage && signedOut) {
            // The panel is back on the right page: the sign-out is over even
            // if the profile has not said so yet.
            signedOut = false;
            refreshBanner();
        }
    }

    // ---- selected object -----------------------------------------------------
    var selectedObj = null;
    var objLabels = {};
    function setSelectedObject(e) {
        selectedObj = (e && e.objid != null) ? e : null;
        if (selectedObj) objLabels[selectedObj.objid] = selectedObj.label;
        $('btnInsertObj').disabled = !selectedObj;
        $('btnInsertObj').title = selectedObj
            ? ('Insert ' + selectedObj.label + ' (object ' + selectedObj.objid + ')')
            : 'Select an object in the Unreal scene first';
    }

    function askObjLabel(objid) {
        if (objLabels[objid] !== undefined) return;
        objLabels[objid] = null;
        call('objlabel', { objid: objid }).then(function (r) {
            objLabels[r.objid] = r.label || null;
        }, function () { });
    }

    var GETTER_FOR = {
        CapturePoint: 'GetCapturePoint', AreaTrigger: 'GetAreaTrigger', SFX: 'GetSFX',
        VO: 'GetVO', HQ: 'GetHQ', AISpawner: 'GetSpawner', VehicleSpawner: 'GetVehicleSpawner',
        InteractPoint: 'GetInteractPoint', WorldIcon: 'GetWorldIcon', SpawnPoint: 'GetSpawnPoint',
        Sector: 'GetSector', MCOM: 'GetMCOM'
    };

    $('btnInsertObj').onclick = function () {
        if (!selectedObj || !editor) return;
        var getter = GETTER_FOR[selectedObj.kind] || 'GetSpatialObject';
        var text = 'mod.' + getter + '(' + selectedObj.objid + ')';
        var sel = editor.getSelection();
        editor.executeEdits('objref', [{ range: sel, text: text, forceMoveMarkers: true }]);
        var ln = sel.startLineNumber;
        (NOTE_OVERRIDES[activeRel] || (NOTE_OVERRIDES[activeRel] = {}))[ln] =
            'Reaches the object you selected in the scene: ' + selectedObj.label + ', object ' + selectedObj.objid + '.';
        scheduleGutter();
        editor.focus();
        say('inserted ' + text + '  (' + selectedObj.label + ')', 'g');
    };

    // ========================================================================
    // STATE FROM THE TOOL
    // ========================================================================
    // The remembered layout arrives with the first status and is applied once.
    // Later status events are about the project, not about how you left the
    // window, so they must not move a panel out from under you.
    var prefsApplied = false;
    function applyPrefs(p) {
        if (prefsApplied) return;
        prefsApplied = true;
        prefs = p || {};
        setPinned(prefs.shelfPinned === 'true');
        if (prefs.console === 'true') toggleConsole(true);
        if (prefs.guided === 'false') $('btnGuided').click();
        var name = prefs.shelf && PANE_NAME[prefs.shelf] ? prefs.shelf : 'walk';
        // A first run has nothing remembered: open START HERE so the window is
        // never a blank editor with no way in.
        if (prefs.shelfOpen === undefined || prefs.shelfOpen === 'true') showPane(name);
        else { shelfPane = name; closeShelf(false); }
    }

    function applyStatus(s) {
        applyPrefs(s.prefs);
        $('stNode').textContent = s.node || 'not found';
        if (!s.node) {
            say('Node is not installed, or not where the tool looks for it. Everything except BUILD and INSTALL still works.', 'w');
            var b = el('button', 'link', 'Open nodejs.org to install it');
            b.onclick = function () { call('openurl', { url: 'https://nodejs.org/' }); };
            outEl.appendChild(b);
        }
        if (s.project) {
            project = s.project;
            $('stProj').textContent = project.name;
            $('projPath').textContent = project.path;
        }
        refreshFirstRun();
    }

    // FLUSH BEFORE LEAVING, FREEZE UNTIL IT HAS LANDED, AND STOP IF IT CANNOT
    // BE DONE.
    //
    // afterOpen disposes every Monaco model and empties the file table. That is
    // correct for the project being left BEHIND, and catastrophic for the edits
    // in it: autosave runs two seconds after the last keystroke, so anyone who
    // types and immediately changes project in the selector loses whatever was
    // in that window, with no warning and nothing on disk to recover from.
    //
    // Saving first was only half of it, and the missing half is the whole
    // point of this comment. A switch is TWO round trips, the write and then
    // the open, and the editor used to stay live in between. Type in that gap
    // and the flush had already been and gone: nothing was going to save that
    // keystroke, and afterOpen disposed the freshly dirty model a moment later.
    //
    // So the whole transition is one locked stretch rather than one saved
    // moment. The editor goes read only before anything is written and comes
    // back when the new project is open, which leaves no window in which an
    // edit can be made that nothing will save. Two switches cannot overlap
    // either; the second is refused rather than interleaved with the first.
    //
    // If the flush cannot be done the switch does not happen at all, and the
    // selector goes back to the project we are actually in, because a dropdown
    // naming a project you are not in is how the next edit goes into the wrong
    // one.
    function freezeEditing() {
        switching = true;
        // The queued autosave belongs to the outgoing project. saveAllDirty
        // below is doing that work now, under our control and with a result we
        // can act on.
        if (autosaveTimer) { clearTimeout(autosaveTimer); autosaveTimer = null; }
        if (editor) editor.updateOptions({ readOnly: true });
    }

    function thawEditing() {
        switching = false;
        // Handing the editor back and still not knowing which project the host
        // has open are the two halves of the same bug, so they are cleared in
        // one place and can never be left disagreeing. Nothing thaws until the
        // host has been heard from: see reconcileOpen.
        unresolvedOpen = null;
        if (editor) editor.updateOptions({ readOnly: false });
    }

    // The dropdown is the only thing on screen that names the project you are
    // in, so a refused or failed switch has to put it back.
    function restoreSelector() {
        var sel = $('projSel');
        if (sel && project && project.path) sel.value = project.path;
    }

    function openProject(path) {
        if (switching) {
            say('a project change is already going on. Wait for it to finish, then choose again.', 'w');
            restoreSelector();
            return Promise.reject({ why: 'a project change is already going on', busy: true });
        }
        var leaving = project && project.path;
        // The Text tab keeps its own unsaved state outside Monaco, so the file
        // sweep below cannot see it. Written now, against the project it
        // belongs to, before anything starts pointing at the next one.
        strFlush();
        var dirty = Object.keys(files).filter(function (rel) { return files[rel].dirty; });
        freezeEditing();
        var flush = dirty.length ? saveAllDirty(true) : Promise.resolve();
        return flush.then(function () {
            // From here until the host answers, WHICH PROJECT IS OPEN IS THE
            // HOST'S ANSWER, NOT OURS. Set before the request leaves, because
            // the request can be acted on the moment it arrives.
            unresolvedOpen = { path: path, from: leaving };
            return call('open', { path: path }).then(afterOpen, function (e) {
                // A REFUSAL IS NOT THE SAME AS NO ANSWER.
                //
                // A refusal, or a call the bridge never managed to send, is
                // proof the host did not switch: we are still in the old
                // project with its models untouched, so hand the editor back
                // rather than leaving a dead read only window.
                //
                // A call that WAS sent and then timed out proves nothing. The
                // host may have opened the project and lost the reply on the
                // way back. Thawing on that used to re-enable saves while the
                // page still showed the outgoing project, and the next write
                // went into the incoming one.
                if (e && e.timedOut && e.sent) { return reconcileOpen(e, path, leaving); }
                thawEditing();
                restoreSelector();
                throw e;
            });
        }, function (e) {
            thawEditing();
            restoreSelector();
            say('staying in ' + (leaving || 'this project') + ': ' +
                (dirty.length === 1 ? dirty[0] + ' could not be saved' :
                 dirty.length + ' file(s) could not be saved') +
                '. Nothing was closed, so your work is still open. Fix the problem and try again.', 'e');
            throw e;
        });
    }

    // THE HOST DID NOT ANSWER THE OPEN. ASK IT WHAT IT DID.
    //
    // This is the residual half of the project-switch defect. Freezing the
    // editor for the whole transition closed the gap where an edit could be
    // made that nothing would save. It did not close this one: the transition
    // ENDING on an unknown outcome. The page assumed the host had stayed put,
    // put the old project back in the selector, thawed, and the next write
    // named a rel path the host resolved against the project it had actually
    // moved to. Project A's text, saved into project B, with the screen still
    // saying A.
    //
    // The host is the only thing that knows, so it is asked, and nothing is
    // unfrozen and no model is disposed until it has said. Every outcome ends
    // either with the page agreeing with the host or with the editor still
    // locked and every character still on screen. The one thing that is not
    // available is guessing.
    function reconcileOpen(e, path, leaving) {
        say('the tool did not answer the request to open ' + path + ', so which project it has open is not known. ' +
            'Nothing can be saved until it says which. Asking it now.', 'w');
        return call('status').then(function (s) {
            var now = s && s.project && s.project.path;
            if (!now) {
                // "No project" from a host we already know is not answering
                // properly is not an answer either. Staying locked keeps the
                // text on screen and keeps it out of the wrong project.
                say('the tool did not say which project it has open. The editor is locked and nothing has been ' +
                    'thrown away: everything you can see is still here. Close and reopen this tab to start again.', 'e');
                throw e;
            }
            if (now === leaving) {
                say('the tool is still in ' + leaving + ', so nothing was opened and nothing was changed. ' +
                    'You can carry on here, or try the switch again.', 'w');
                thawEditing();
                restoreSelector();
                throw e;
            }
            // The host is somewhere else: the project we asked for, or a third
            // one somebody else opened. Either way the models in `files` belong
            // to a project that is no longer open, and afterOpen is exactly the
            // code that retires them safely, keeping anything unsaved against
            // the project it belongs to.
            say(now === path
                ? 'the tool had already opened ' + path + '. The editor has caught up with it.'
                : 'the tool has ' + now + ' open, which is neither the project you left nor the one you asked for. ' +
                  'The editor has caught up with it, and anything unsaved is kept against the project it came from.',
                'w');
            afterOpen(s);
            return s;
        }, function () {
            // Two unanswered requests in a row. Report the ORIGINAL failure,
            // because that is the one the user asked for, and stay locked.
            say('the tool has not answered at all, so which project it has open is still not known. The editor is ' +
                'locked so nothing can be saved into the wrong project, and nothing has been thrown away: ' +
                'everything you can see is still here. Close and reopen this tab to start again.', 'e');
            throw e;
        });
    }

    function afterOpen(r) {
        var leaving = project && project.path;
        // The host has spoken, whether we got here from the open's own reply or
        // from reconcileOpen. Everything below acts on that answer.
        unresolvedOpen = null;
        project = r.project || project;
        $('stProj').textContent = project ? project.name : 'none';
        $('projPath').textContent = project ? project.path : 'No project open.';
        refreshFirstRun();
        // The text belongs to the project, so it is re-read when the project
        // changes rather than left showing the last one's.
        findInvalidate();
        if ($('findPanel')) { $('findPanel').hidden = true; }
        strTree = null;
        if (isStringsOpen()) { loadStrings(); } else { drawStrings(); }
        quickList = null;
        if (isQuickOpen()) { loadQuick(); } else { drawQuick(); }
        // The AI column belongs to the project too: its turns are kept but only
        // this project's are shown, and a question box still open was framed
        // around a file in the project being left.
        closeAsk();
        askAnchor = null;
        if (aiMode === 'ai') { drawAi(); }

        // NOTHING UNSAVED IS THROWN AWAY HERE.
        //
        // With the transition locked there should be nothing dirty left to find
        // and this loop should never keep anything. But a dispose is final, and
        // being wrong about that costs somebody their afternoon, so this does
        // not assume: anything still dirty is kept as text against the project
        // it belongs to and goes back in when that project is opened again. It
        // cannot be written now, because the host has already moved and these
        // rel paths would land in the wrong project.
        var kept = [];
        Object.keys(files).forEach(function (rel) {
            var f = files[rel];
            if (f.dirty && leaving) {
                (stranded[leaving] || (stranded[leaving] = {}))[rel] = f.model.getValue();
                kept.push(rel);
            }
            try { f.model.dispose(); } catch (e) { }
        });
        files = {}; activeRel = null; NOTE_OVERRIDES = {};
        if (kept.length) {
            say(kept.join(', ') + ' still had unsaved text when the project changed. It has not been lost: open ' +
                leaving + ' again and it comes back.', 'w');
        }

        // The thaw is in a finally because a frozen editor is not a recoverable
        // state: if anything below threw, the window would stay read only with
        // no switch in progress to end it, and the only way out would be
        // reopening the panel.
        try {
            refreshFiles();
            refreshTypes();
            drawWalk();
            recoverStranded();
            // Whether a bundle is built and not yet on Portal is a fact about
            // the project, not about this session, so it is asked for on every
            // open.
            call('pending').catch(function () { });
        } finally {
            thawEditing();
        }
    }

    // Put back whatever the previous close could not write. The models are made
    // from the kept text rather than from disk, marked dirty, and autosaved, so
    // the recovery ends with the text where it always should have been.
    function recoverStranded() {
        var path = project && project.path;
        var held = path && stranded[path];
        if (!held) return;
        delete stranded[path];
        var rels = Object.keys(held);
        if (!rels.length) return;
        rels.forEach(function (rel) {
            openFile(rel, held[rel], true);
            if (files[rel]) files[rel].dirty = true;
        });
        drawTabs();
        say(rels.join(', ') + ' had unsaved text when this project was last closed. It is back in the editor and is being saved now.', 'w');
        scheduleAutosave();
    }

    function refreshProjects(select) {
        return call('projects').then(function (r) {
            var sel = $('projSel');
            sel.innerHTML = '';
            (r.list || []).forEach(function (p) {
                var o = el('option', null, p.name + (p.experience ? '  (linked)' : ''));
                o.value = p.path;
                sel.appendChild(o);
            });
            if (!r.list || !r.list.length) {
                var o2 = el('option', null, 'no project yet');
                o2.value = ''; sel.appendChild(o2);
            }
            // WHICH PROJECT TO SHOW, in order of who actually knows:
            //   1. an explicit ask from this page (switching projects)
            //   2. the host's own open project - it may have just imported an
            //      experience and opened its project, and that beats a guess
            //   3. whatever sorts first, which is only ever a fallback
            // Opening list[0] regardless is how a freshly imported experience
            // stayed hidden behind a leftover example project.
            var want = select || r.open || '';
            if (want) {
                for (var i = 0; i < sel.options.length; i++) {
                    if (sel.options[i].value === want) { sel.value = want; break; }
                }
            }
            if (sel.value) return openProject(sel.value);
        });
    }

    // The file list reads as the project, not as a directory listing: the src
    // folders ARE the features, and dist/bundle.ts is the one file Portal gets.
    // A beginner should never have to work that out from a path.
    function refreshFiles() {
        return call('files').then(function (r) {
            lastFiles = {};
            (r.files || []).forEach(function (rel) { lastFiles[rel] = 1; });

            var host = $('fileList');
            host.innerHTML = '';

            var groups = [];         // [{ label, why, items }]
            var byName = {};
            function group(key, label, why) {
                if (byName[key]) return byName[key];
                var g = { label: label, why: why, items: [] };
                byName[key] = g; groups.push(g); return g;
            }

            (r.files || []).forEach(function (rel) {
                var feat = /^src\/([^\/]+)\//.exec(rel);
                if (rel === 'src/index.ts') {
                    group('main', 'The main file', 'Everything starts here, and this is where each feature is switched on.').items.push(rel);
                } else if (rel === 'src/strings.json') {
                    group('main', 'The main file', '').items.push(rel);
                } else if (feat) {
                    group('f:' + feat[1], 'Feature: ' + feat[1], '').items.push(rel);
                } else if (rel.indexOf('dist/') === 0) {
                    group('dist', 'The built file', 'This is what Portal gets. Build makes it. Do not edit it by hand.').items.push(rel);
                } else if (rel.indexOf('src/') === 0) {
                    group('src', 'Loose files under src', '').items.push(rel);
                } else {
                    group('rest', 'Project settings', 'The template\'s own files. You rarely need to touch these.').items.push(rel);
                }
            });

            groups.forEach(function (g) {
                var h = el('div', 'grp');
                h.innerHTML = esc(g.label) + (g.why ? '<span class="why">' + esc(g.why) + '</span>' : '');
                host.appendChild(h);
                g.items.forEach(function (rel) {
                    var b = el('button', 'row' + (rel === activeRel ? ' sel' : ''));
                    b.innerHTML = '<span class="t">' + esc(rel.replace(/^.*\//, '')) + '</span>' +
                        '<span class="d">' + esc(rel) + '</span>';
                    b.onclick = function () {
                        // BACK TO THE EDITOR FIRST. The Text and Values tabs
                        // swap the editor out, so a file picked from this list
                        // while one of them was open opened correctly into a
                        // hidden pane and looked like the click did nothing.
                        showStrings(false);
                        showQuick(false);
                        if (files[rel]) { openFile(rel); return; }
                        call('read', { rel: rel });   // arrives as a 'file' payload
                    };
                    host.appendChild(b);
                });
            });

            if ($('pane-project').classList.contains('on')) drawProject();
            // The file list is the only thing that knows where this project
            // keeps its strings, so the key list is rebuilt from it.
            refreshStringKeys();
            // WHAT TO SHOW WHEN NOTHING IS OPEN YET.
            //
            // The host gets the first say. After an import it names the file
            // that actually holds the user's script, because src/index.ts in a
            // freshly scaffolded project is the TEMPLATE'S starter example -
            // opening that made a perfect import look like nothing had arrived.
            // Otherwise the old rule stands: the entry point is the right place
            // to start in a project somebody is writing.
            // Said out loud, because this decision was wrong for several
            // rounds and nothing in the log could say why.
            note('files: ' + (r.files || []).length + ', showFirst=' + (r.showFirst || 'none')
                 + ', activeRel=' + (activeRel || 'none'));
            if (!activeRel) {
                var first = r.showFirst && r.files && r.files.indexOf(r.showFirst) !== -1
                    ? r.showFirst
                    : (r.files && r.files.indexOf('src/index.ts') !== -1 ? 'src/index.ts' : '');
                note('opening first: ' + (first || 'nothing'));
                if (first) { call('read', { rel: first }); }
            }
        });
    }
    var lastFiles = {};

    // Three descriptions of the same API can be on this machine and they are
    // not the same size. The status has to say which ones were found and what
    // each one added, because "the editor does not know that command" and "this
    // machine does not have the richer type set" look identical otherwise.
    var typeSources = [];
    function refreshTypes() {
        libCount = 0;
        return call('types').then(function (r) {
            typeSources = r.sources || [];
            say('type definitions: ' + (r.count || 0) + ' files queued', 'd');
            typeSources.forEach(function (s) {
                if (s.functions < 0) {
                    say('type set not on this machine: ' + s.name + '. Everything still works from what is here.', 'd');
                } else if (s.functions > 0 || s.other > 0) {
                    say('type set ' + s.name + ': ' + s.functions + ' commands, ' + s.other + ' other names', 'd');
                }
            });
            drawTypeSources();
            drawApiSearch();
        }, function (e) {
            say('could not read the type definitions: ' + (e && e.why) + '. Run INSTALL first.', 'w');
        });
    }

    function drawTypeSources() {
        var host = $('typeSrc');
        if (!host) return;
        host.innerHTML = '';
        if (!typeSources.length) {
            host.appendChild(el('div', 'sub', 'Nothing loaded yet. Install the packages first.'));
            return;
        }
        typeSources.forEach(function (s) {
            var c = el('div', 'card' + (s.functions < 0 ? '' : ' ok'));
            var what = s.functions < 0
                ? 'Not on this machine. Nothing is missing from what you can already do.'
                : (s.functions + ' command' + (s.functions === 1 ? '' : 's') + ', ' +
                   s.other + ' other name' + (s.other === 1 ? '' : 's'));
            c.innerHTML = '<div class="t">' + esc(s.name) + '</div><div>' + esc(what) + '</div>' +
                (s.path ? '<div class="s">' + esc(s.path) + '</div>' : '');
            host.appendChild(c);
        });
    }

    // ========================================================================
    // BOOT
    // ========================================================================
    function afterEditorReady() {
        registerCommands();
        if (pendingLibs) { var p = pendingLibs; pendingLibs = null; p.forEach(function (x) { addExtraLib(x[0], x[1]); }); }
        drawRecipes();
        drawWalk();
        buildGutter();
        openFile('src/index.ts', '// Nothing open yet. Use NEW, or pick a project above.\n');
        refreshProjects().catch(function () { });
        call('snippets');
        // Completion, signature help, the inlay hints and FIND A COMMAND all
        // read this, so it is asked for at boot rather than on first use.
        askApi();
    }

    function onBuilt(e) {
        builtOnce = true;
        if (e.errors && e.errors.length) {
            showPane('diagnose');
            e.errors.forEach(function (x) { say(x, 'e'); });
        }
    }

    // The tool binds its object when the window is created, which can be after
    // this script has run. Poll briefly, then say hello.
    var tries = 0;
    (function waitForBridge() {
        if (bridge()) {
            drainHeld();
            bridge().ready(JSON.stringify({ src: 'editor', v: 1 }));
            // The worker source is a generated .js file the tool points us at.
            // A file:// page may LOAD a file:// script; it may not fetch one,
            // and it may not construct a worker from one either. So: load the
            // script, take the string it sets, and build the worker from a
            // Blob made of that string.
            call('workersrc').then(function (r) {
                if (!r || !r.url) {
                    say(r && r.why ? r.why : 'No language service worker source.', 'w');
                    bootMonaco();
                    return;
                }
                var s = document.createElement('script');
                s.src = r.url;
                s.onload = function () {
                    if (window.__bf6WorkerSrc) onWorkerSrc(window.__bf6WorkerSrc);
                    else { say('the worker source file loaded but set nothing', 'w'); bootMonaco(); }
                };
                s.onerror = function () {
                    say('the worker source file would not load from ' + r.url, 'w');
                    bootMonaco();
                };
                document.head.appendChild(s);
            }, function () { bootMonaco(); });
            call('status').then(applyStatus, function () { });
            // Which AI, if any, so the column can say where a question would go
            // before anybody asks one.
            assistStatus();
            return;
        }
        if (++tries > 100) {
            say('the tool bridge never appeared. The editor still opens, but nothing can be read or written.', 'e');
            bootMonaco();
            return;
        }
        setTimeout(waitForBridge, 100);
    })();

    // If the worker source never arrives, boot anyway after a moment: index
    // mode is a working editor, and a page that waits for ever is not.
    setTimeout(function () { if (!monacoReady) bootMonaco(); }, 6000);

    // If the tool never answered with a remembered layout, fall back to the
    // first-run one rather than opening on nothing at all.
    setTimeout(function () { applyPrefs(null); refreshFirstRun(); }, 2500);
})();
