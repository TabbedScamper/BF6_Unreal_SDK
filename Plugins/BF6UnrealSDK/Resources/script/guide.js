// ============================================================================
// BF6 Script: the beginner layer's data.
//
// Everything the editor says in plain words lives here, and nothing here is
// generated at runtime. Three kinds of entry:
//
//   mod / utils / ts   one short line for a symbol or a language construct,
//                      shown in the explanation gutter beside the code.
//   errors             the compiler's own message rewritten as cause and fix.
//   pitfalls           a scan CHECK MY SCRIPT runs, each grounded in a source
//                      that is named on the finding itself.
//   recipes            a small annotated project the user can create or paste.
//
// SOURCES ARE NAMED, NOT IMPLIED. Every pitfall and every recipe carries a
// "source" string saying where the claim comes from: a law established in the
// tool's own build notes, an answered question from the Portal community, or
// the template's own code. A claim that could not be grounded in one of those
// is marked unverified and says so on screen.
//
// A symbol with no entry here is NOT missing information: the editor falls
// back to the JSDoc the type definitions carry, which is where most of the
// detail is. This table exists for the sixty or so calls a beginner meets in
// their first hour, where "Sets the value of a Variable" is true and useless.
// ============================================================================

window.BF6ScriptGuide = (function () {
    'use strict';

    // ---- links used more than once ----------------------------------------
    var L = {
        hub: 'https://www.ea.com/games/battlefield/battlefield-6/onboarding-hub/bf6-portal-hub',
        utils: 'https://github.com/deluca-mike/bf6-portal-utils',
        template: 'https://github.com/deluca-mike/bf6-portal-scripting-template',
        bundler: 'https://github.com/deluca-mike/bf6-portal-bundler',
        discord: 'https://discord.com/invite/battlefield-portal-community-870246147455877181',
        tsHandbook: 'https://www.typescriptlang.org/docs/handbook/2/everyday-types.html'
    };

    // ========================================================================
    // mod.* in plain words.
    //
    // "say" is the sentence the gutter shows. It answers "what happens when
    // this line runs", not "what type does it return" - the hover already
    // answers that from the JSDoc.
    // ========================================================================
    var MOD = {
        // ---- the basics ----
        Wait: { say: 'Pauses this function for the given number of seconds, then carries on. The rest of the game keeps running.', tag: 'time' },
        Message: { say: 'Turns a key from strings.json into text the game can show. Any text a player sees has to come through here.', tag: 'text' },
        GetObjId: { say: 'Reads the plain number that identifies a player or an object. Safe to keep; the player handle itself is not.', tag: 'basics' },
        IsPlayerValid: { say: 'True while this player is still really in the match. Check it before touching a player you stored earlier.', tag: 'basics' },
        IsValid: { say: 'True while this handle still points at something the game has. Guards a spawner or prop you may have unspawned.', tag: 'basics' },
        IsUndefined: { say: 'True when a value came back empty. Use it before doing arithmetic on something the engine may not have given you.', tag: 'basics' },
        GetSpatialObject: { say: 'Finds an object you placed on the map by its ObjId number. This is how script reaches into the level.', tag: 'objects' },

        // ---- players ----
        GetSoldierState: { say: 'Reads one live fact about a soldier: where they are, what they face, health, alive or not.', tag: 'player' },
        GetPlayer: { say: 'Finds a player by their id number.', tag: 'player' },
        AllPlayers: { say: 'The list of everyone in the match right now.', tag: 'player' },
        GetTeam: { say: 'The team a player is on, or the team object for a team number.', tag: 'team' },
        SetTeam: { say: 'Moves a player onto another team. Fussy: see the team recipe before you rely on it.', tag: 'team', warn: 'Only works on AI spawned from an AI Spawner, only when the player is undeployed, and never when the target team is the one they are already on.' },
        SwitchTeams: { say: 'Puts a player on the opposite team.', tag: 'team' },
        AutoBalanceTeams: { say: 'Lets the game even the teams out by itself.', tag: 'team' },
        Kill: { say: 'Kills a player through the engine\'s own death path, so everything that normally follows a death still happens.', tag: 'player' },
        Heal: { say: 'Adds health back to a player.', tag: 'player' },
        DealDamage: { say: 'Takes health off a player from script rather than from a weapon.', tag: 'player' },
        Teleport: { say: 'Moves a player to a position instantly.', tag: 'player' },
        SetPlayerMaxHealth: { say: 'Changes how much health this player has in total.', tag: 'player' },
        SetPlayerIncomingDamageFactor: { say: 'Scales the damage this player takes. 0.5 is half damage, 2 is double.', tag: 'player' },
        SetPlayerMovementSpeedMultiplier: { say: 'Makes this player faster or slower than normal.', tag: 'player' },
        ForceRevive: { say: 'Picks a downed player straight back up.', tag: 'player' },
        SkipManDown: { say: 'Sends this player straight to death rather than through the downed state.', tag: 'player' },
        ForceManDown: { say: 'Puts this player into the downed state.', tag: 'player' },
        Resupply: { say: 'Refills this player\'s ammo.', tag: 'player' },
        SpotTarget: { say: 'Marks a player as spotted for the other team to see.', tag: 'player' },
        ClosestPlayerTo: { say: 'Of a list of players, the one nearest a position.', tag: 'player' },
        FarthestPlayerFrom: { say: 'Of a list of players, the one furthest from a position.', tag: 'player' },
        GetPlayerKills: { say: 'How many kills this player has.', tag: 'player' },
        GetPlayerDeaths: { say: 'How many times this player has died.', tag: 'player' },
        IsSoldierClass: { say: 'True when the player is playing the class you name.', tag: 'player' },

        // ---- deploying and spawning ----
        DeployPlayer: { say: 'Puts this player into the world now, without waiting for them to press deploy.', tag: 'spawn' },
        DeployAllPlayers: { say: 'Deploys everyone at once.', tag: 'spawn' },
        UndeployPlayer: { say: 'Takes this player out of the world and back to the deploy screen.', tag: 'spawn' },
        UndeployAllPlayers: { say: 'Takes everybody out of the world at once.', tag: 'spawn' },
        EnablePlayerDeploy: { say: 'Lets this player deploy, or stops them. A common way to hold a team back at round start.', tag: 'spawn' },
        EnableAllPlayerDeploy: { say: 'Lets everybody deploy, or holds everybody.', tag: 'spawn' },
        SetSpawnMode: { say: 'Switches the whole match between normal deploying and spectating.', tag: 'spawn', warn: 'Only set Spectating while every player is alive and deployed, and never call deploy APIs on a player who is spectating their own death.' },
        SetRedeployTime: { say: 'How long a dead player waits before they can deploy again. Clamped to 0 to 60 seconds.', tag: 'spawn' },
        SpawnPlayerFromSpawnPoint: { say: 'Deploys a player at a spawn point you placed on the map.', tag: 'spawn' },
        DisablePlayerJoin: { say: 'Stops anyone new joining the match.', tag: 'spawn' },
        GetSpawnPoint: { say: 'Finds a spawn point you placed on the map by its ObjId.', tag: 'spawn' },

        // ---- objects in the world ----
        SpawnObject: { say: 'Creates a prop, spawner or marker in the world at a position. What you get back is the thing you then configure.', tag: 'objects', warn: 'The object is not ready to configure on the same tick. Put an await mod.Wait(1) between spawning it and setting it up.' },
        UnspawnObject: { say: 'Removes something script spawned. Do this when you are finished with it, or it costs the server for nothing.', tag: 'objects' },
        MoveObject: { say: 'Moves an object by an amount, not to a place. Both arguments are DELTAS added to where it already is.', tag: 'objects', warn: 'Never move an object that was spawned at a scale other than 1: the engine rebuilds a scaled basis on any move and the object comes back wrong.' },
        MoveObjectOverTime: { say: 'Slides an object by an amount over a number of seconds.', tag: 'objects' },
        SetObjectTransform: { say: 'Puts an object at an exact position and rotation. Carries no scale, so a scaled object loses its scale.', tag: 'objects', warn: 'Not safe on a scaled object. Unspawn and respawn with the four argument SpawnObject instead.' },
        SetObjectTransformOverTime: { say: 'Moves an object to an exact position and rotation over a number of seconds.', tag: 'objects' },
        RotateObject: { say: 'Turns an object.', tag: 'objects' },
        OrbitObjectOverTime: { say: 'Swings an object around a point over time.', tag: 'objects' },
        GetObjectPosition: { say: 'Where an object is.', tag: 'objects', warn: 'Returns roughly zero for things with no physical body, such as spawners and interact points. Put a real prop there if you need a readable position.' },
        GetObjectRotation: { say: 'Which way an object faces.', tag: 'objects' },
        GetObjectTransform: { say: 'Position and rotation together as one value.', tag: 'objects' },
        CreateTransform: { say: 'Builds a position and rotation pair. There is no scale in a transform.', tag: 'objects' },
        SetUnspawnDelayInSeconds: { say: 'How long a spawned thing lingers before the engine clears it.', tag: 'objects' },
        StopActiveMovementForObject: { say: 'Cancels a move that is still running on an object.', tag: 'objects' },

        // ---- vectors and maths ----
        CreateVector: { say: 'Builds a position or a direction from three numbers: x across, y up, z along.', tag: 'math' },
        XComponentOf: { say: 'The x number out of a vector.', tag: 'math' },
        YComponentOf: { say: 'The y number out of a vector. In Portal, y is up.', tag: 'math' },
        ZComponentOf: { say: 'The z number out of a vector.', tag: 'math' },
        DistanceBetween: { say: 'How far apart two positions are, in metres.', tag: 'math' },
        DirectionTowards: { say: 'A unit direction pointing from one position to another.', tag: 'math' },
        VectorTowards: { say: 'The full offset from one position to another, length included.', tag: 'math' },
        Normalize: { say: 'Shrinks a vector to length one, keeping its direction.', tag: 'math' },
        RandomReal: { say: 'A random number between the two you give it.', tag: 'math' },
        RandomValueInArray: { say: 'One item picked at random out of a list.', tag: 'math' },

        // ---- capture points, HQs, objectives ----
        GetCapturePoint: { say: 'Finds a capture point you placed on the map by its ObjId.', tag: 'objective' },
        GetCaptureProgress: { say: 'How far along a capture is, from 0 to 1.', tag: 'objective' },
        GetCurrentOwnerTeam: { say: 'Which team holds this point right now.', tag: 'objective' },
        GetPreviousOwnerTeam: { say: 'Which team held this point before the current owner.', tag: 'objective' },
        GetPlayersOnPoint: { say: 'Everyone standing in this capture point\'s area.', tag: 'objective' },
        SetCapturePointOwner: { say: 'Hands a capture point to a team from script.', tag: 'objective' },
        SetCapturePointCapturingTime: { say: 'How many seconds it takes to capture this point.', tag: 'objective' },
        SetCapturePointNeutralizationTime: { say: 'How many seconds it takes to knock this point back to neutral first.', tag: 'objective' },
        EnableCapturePointDeploying: { say: 'Lets players deploy onto this captured point, or stops them.', tag: 'objective' },
        AllCapturePoints: { say: 'Every capture point on the map.', tag: 'objective' },
        SetMaxCaptureMultiplier: { say: 'Caps how much faster a crowd captures than one player.', tag: 'objective' },
        GetHQ: { say: 'Finds a headquarters you placed on the map by its ObjId.', tag: 'objective' },
        EnableHQ: { say: 'Turns a headquarters on or off.', tag: 'objective' },
        SetHQTeam: { say: 'Says which team a headquarters belongs to.', tag: 'objective' },
        EnableGameModeObjective: { say: 'Turns one of the mode\'s objectives on or off.', tag: 'objective' },
        GetSector: { say: 'Finds a sector you placed on the map by its ObjId.', tag: 'objective' },
        GetMCOM: { say: 'Finds an MCOM you placed on the map by its ObjId.', tag: 'objective' },

        // ---- area triggers ----
        GetAreaTrigger: { say: 'Finds an area trigger you placed on the map by its ObjId. Entering and leaving it fire events.', tag: 'area' },
        EnableAreaTrigger: { say: 'Turns an area trigger on or off.', tag: 'area' },
        GetInteractPoint: { say: 'Finds a place players can hold the interact key on, by its ObjId.', tag: 'area' },
        EnableInteractPoint: { say: 'Turns an interact point on or off.', tag: 'area' },

        // ---- score, time, round flow ----
        SetGameModeScore: { say: 'Sets a team\'s score.', tag: 'flow' },
        GetGameModeScore: { say: 'Reads a team\'s score.', tag: 'flow' },
        SetGameModeTargetScore: { say: 'The score that wins the round.', tag: 'flow' },
        SetGameModeInitialScore: { say: 'What each team starts on.', tag: 'flow' },
        SetGameModeTimeLimit: { say: 'How long the round runs.', tag: 'flow' },
        SetGameModeCriteria: { say: 'What ends the round: the clock, the score, or both.', tag: 'flow' },
        EndGameMode: { say: 'Ends the round now and declares the winner.', tag: 'flow' },
        PauseGameModeTime: { say: 'Freezes and unfreezes the round clock.', tag: 'flow' },
        ResetGameModeTime: { say: 'Puts the round clock back to the start.', tag: 'flow' },
        GetMatchTimeElapsed: { say: 'Seconds since the match began.', tag: 'flow', warn: 'Do not call this once per event on a hot path. Read it once a tick and pass the number around.' },
        GetMatchTimeRemaining: { say: 'Seconds left in the match.', tag: 'flow' },
        GetRoundTime: { say: 'Seconds the current round has run.', tag: 'flow' },
        GetTargetScore: { say: 'The score that ends the round.', tag: 'flow' },
        SetFriendlyFire: { say: 'Whether teammates can hurt each other.', tag: 'flow' },

        // ---- messages and HUD ----
        DisplayNotificationMessage: { say: 'A short line of text in the middle of one player\'s screen, or everyone\'s.', tag: 'ui' },
        DisplayCustomNotificationMessage: { say: 'A notification you control the slot and duration of.', tag: 'ui' },
        DisplayHighlightedWorldLogMessage: { say: 'A line in the kill feed area, highlighted.', tag: 'ui' },
        ClearCustomNotificationMessage: { say: 'Removes one custom notification.', tag: 'ui' },
        ClearAllCustomNotificationMessages: { say: 'Removes every custom notification.', tag: 'ui' },
        SetHUDTicker: { say: 'The scrolling line of text along the HUD.', tag: 'ui' },

        // ---- UI widgets ----
        GetUIRoot: { say: 'The top of one player\'s widget tree. Everything you add hangs off this.', tag: 'ui' },
        AddUIText: { say: 'Puts a block of text on a player\'s screen.', tag: 'ui', warn: 'Offsets run INWARD from the anchored edge on a 1920 by 1080 canvas: with a Right anchor, a larger x moves the widget left.' },
        AddUIContainer: { say: 'An empty box to group and position other widgets inside.', tag: 'ui' },
        AddUIButton: { say: 'A button on a player\'s screen. You still have to handle the press in OnPlayerUIButtonEvent.', tag: 'ui' },
        AddUIImage: { say: 'A picture on a player\'s screen.', tag: 'ui' },
        AddUIIcon: { say: 'One of the game\'s own icons on a player\'s screen.', tag: 'ui' },
        SetUITextLabel: { say: 'Changes what an existing text widget says, without rebuilding it.', tag: 'ui' },
        SetUIWidgetVisible: { say: 'Shows or hides a widget and everything under it.', tag: 'ui' },
        SetUIWidgetPosition: { say: 'Moves a widget. The offset runs inward from its anchor.', tag: 'ui' },
        SetUIWidgetAnchor: { say: 'Which edge or corner of the parent the widget hangs from.', tag: 'ui' },
        DeleteUIWidget: { say: 'Removes one widget and its children.', tag: 'ui' },
        DeleteAllUIWidgets: { say: 'Clears a player\'s whole widget tree.', tag: 'ui' },
        EnableUIButtonEvent: { say: 'Turns button presses on for a player. Without it your buttons do nothing.', tag: 'ui' },
        EnableUIInputMode: { say: 'Gives the player a cursor so they can click your UI.', tag: 'ui' },
        FindUIWidgetWithName: { say: 'Looks a widget up again by the name you gave it.', tag: 'ui' },

        // ---- world icons ----
        GetWorldIcon: { say: 'Finds a world icon you placed on the map by its ObjId.', tag: 'ui' },
        SetWorldIconPosition: { say: 'Moves a marker floating in the world.', tag: 'ui' },
        SetWorldIconText: { say: 'What the world marker says.', tag: 'ui' },
        SetWorldIconImage: { say: 'Which picture the world marker shows.', tag: 'ui' },
        SetWorldIconOwner: { say: 'Who can see this world marker.', tag: 'ui' },
        EnableWorldIconText: { say: 'Shows or hides the marker\'s text.', tag: 'ui' },
        EnableWorldIconImage: { say: 'Shows or hides the marker\'s picture.', tag: 'ui' },

        // ---- sound ----
        GetSFX: { say: 'Finds a sound emitter you placed on the map by its ObjId. It is one emitter, not a template.', tag: 'sound' },
        PlaySound: { say: 'Plays a one shot through an emitter at a position, with a volume and a falloff range.', tag: 'sound', warn: 'Do not retrigger an emitter that is still sounding: moving it drags the sound already in flight across the map.' },
        StopSound: { say: 'Silences an emitter.', tag: 'sound' },
        SetSoundAmplitude: { say: 'How loud an emitter is.', tag: 'sound' },
        GetVO: { say: 'Finds a voice carrier you placed on the map by its ObjId.', tag: 'sound' },
        PlayVO: { say: 'Plays a spoken line through a carrier.', tag: 'sound', warn: 'A carrier spawned this tick is not ready. Wait a tick, and set the team before playing anything team scoped.' },
        LoadMusic: { say: 'Gets a music track ready.', tag: 'sound' },
        PlayMusic: { say: 'Starts a loaded music track.', tag: 'sound' },
        UnloadMusic: { say: 'Frees a music track.', tag: 'sound' },
        SetMusicParam: { say: 'Nudges a running music track, for example its intensity.', tag: 'sound' },

        // ---- vehicles ----
        GetVehicleSpawner: { say: 'Finds a vehicle spawner you placed on the map by its ObjId.', tag: 'vehicle' },
        SetVehicleSpawnerVehicleType: { say: 'Which vehicle this spawner produces.', tag: 'vehicle' },
        SetVehicleSpawnerAutoSpawn: { say: 'Whether the spawner refills by itself.', tag: 'vehicle' },
        SetVehicleSpawnerRespawnTime: { say: 'Seconds between one vehicle being lost and the next appearing.', tag: 'vehicle' },
        ForceVehicleSpawnerSpawn: { say: 'Makes the spawner produce a vehicle right now.', tag: 'vehicle' },
        GetVehicleState: { say: 'One live fact about a vehicle: where it is, its health, who is in it.', tag: 'vehicle' },
        GetVehicleFromPlayer: { say: 'The vehicle this player is in, if any.', tag: 'vehicle' },
        GetPlayerFromVehicleSeat: { say: 'Who is sitting in a given seat.', tag: 'vehicle' },
        GetAllPlayersInVehicle: { say: 'Everyone in this vehicle.', tag: 'vehicle' },
        ForcePlayerExitVehicle: { say: 'Throws a player out of a vehicle.', tag: 'vehicle' },
        ForcePlayerToSeat: { say: 'Moves a player into a particular seat.', tag: 'vehicle' },
        CompareVehicleName: { say: 'True when this vehicle is the type you name.', tag: 'vehicle' },
        SetVehicleMaxHealthMultiplier: { say: 'Makes vehicles tougher or weaker.', tag: 'vehicle' },
        AllVehicles: { say: 'Every vehicle in the match.', tag: 'vehicle' },

        // ---- bots ----
        GetSpawner: { say: 'Finds an AI spawner you placed on the map by its ObjId.', tag: 'bot' },
        SpawnAIFromAISpawner: { say: 'Produces one bot from an AI spawner.', tag: 'bot', warn: 'The bot is not fully there on the same tick. Wait a tick in OnSpawnerSpawned before you give it orders.' },
        UnspawnAllAIsFromAISpawner: { say: 'Removes every bot this spawner made.', tag: 'bot' },
        AIBattlefieldBehavior: { say: 'Tells a bot to play the objective the way a normal Battlefield bot does.', tag: 'bot' },
        AIMoveToBehavior: { say: 'Sends a bot to a position.', tag: 'bot' },
        AIValidatedMoveToBehavior: { say: 'Sends a bot to a position, checking the path is reachable first.', tag: 'bot' },
        AIDefendPositionBehavior: { say: 'Tells a bot to hold a position.', tag: 'bot' },
        AIIdleBehavior: { say: 'Tells a bot to stand down.', tag: 'bot' },
        AISetTarget: { say: 'Points a bot at somebody.', tag: 'bot' },
        AIEnableShooting: { say: 'Lets a bot fire, or stops it.', tag: 'bot' },
        AIEnableTargeting: { say: 'Lets a bot pick its own targets, or stops it.', tag: 'bot' },
        AISetMoveSpeed: { say: 'How fast a bot moves.', tag: 'bot' },
        AISetStance: { say: 'Standing, crouched or prone.', tag: 'bot' },
        AISetUnspawnOnDead: { say: 'Whether a dead bot is cleared away.', tag: 'bot' },
        SetAIToHumanDamageModifier: { say: 'Scales how hard bots hit people.', tag: 'bot' },

        // ---- effects and camera ----
        GetVFX: { say: 'Finds an effect you placed on the map by its ObjId.', tag: 'fx' },
        EnableVFX: { say: 'Turns an effect on or off.', tag: 'fx' },
        MoveVFX: { say: 'Moves an effect.', tag: 'fx' },
        SetVFXColor: { say: 'Recolours an effect.', tag: 'fx' },
        SetVFXScale: { say: 'Resizes an effect.', tag: 'fx' },
        EnableScreenEffect: { say: 'A full screen effect on a player.', tag: 'fx' },
        SetSoldierEffect: { say: 'An effect attached to a soldier.', tag: 'fx' },
        SetCameraTypeForPlayer: { say: 'Which camera this player looks through.', tag: 'fx' },
        SetCameraTypeForAll: { say: 'Which camera everybody looks through.', tag: 'fx' },

        // ---- diagnostics ----
        SendPortalLogToAdmin: { say: 'Pushes the server log to the admin\'s own PortalLog file. A no-op when you host locally, because the log is already on your disk.', tag: 'debug' },
        GetPortalAverageFrameTime: { say: 'How many milliseconds your script itself costs per tick. The number to watch when the game stutters.', tag: 'debug' },
        GetServerAverageFrameTime: { say: 'How many milliseconds the whole server tick costs.', tag: 'debug' },
        SendErrorReport: { say: 'Files a report with the game.', tag: 'debug' },
        RayCast: { say: 'Fires a line through the world and tells you what it hit.', tag: 'debug' },

        // ---- variables ----
        SetVariable: { say: 'Stores a value in a Portal variable.', tag: 'basics' },
        GetVariable: { say: 'Reads a value back out of a Portal variable.', tag: 'basics' },
        GlobalVariable: { say: 'A variable shared by the whole match.', tag: 'basics' },
        ObjectVariable: { say: 'A variable that belongs to one object or player.', tag: 'basics' }
    };

    // ========================================================================
    // bf6-portal-utils, one module at a time.
    //
    // "readme" is the path inside node_modules the tool opens; "link" is the
    // same document on the web for anyone who would rather read it there.
    // ========================================================================
    var UTILS = {
        events: {
            say: 'Wraps the game\'s event handlers so you can subscribe and unsubscribe in code instead of exporting one function per event.',
            readme: 'bf6-portal-utils/events/README.md', link: L.utils + '/tree/main/events'
        },
        timers: {
            say: 'setTimeout and setInterval for Portal. Always keep the number it returns so you can clear the timer later.',
            readme: 'bf6-portal-utils/timers/README.md', link: L.utils + '/tree/main/timers'
        },
        clocks: {
            say: 'A shared clock so every part of your mod reads the same time without each one asking the engine.',
            readme: 'bf6-portal-utils/clocks/README.md', link: L.utils + '/tree/main/clocks'
        },
        ui: {
            say: 'Buttons, text and containers as objects, so a HUD is a few lines instead of a wall of AddUI calls.',
            readme: 'bf6-portal-utils/ui/README.md', link: L.utils + '/tree/main/ui'
        },
        'solid-ui': {
            say: 'A reactive layer over the UI module: describe what the screen should say and it updates itself.',
            readme: 'bf6-portal-utils/solid-ui/README.md', link: L.utils + '/tree/main/solid-ui'
        },
        logger: {
            say: 'Prints text on screen while you play. A fixed set of rows, or a scrolling console.',
            readme: 'bf6-portal-utils/logger/README.md', link: L.utils + '/tree/main/logger'
        },
        logging: {
            say: 'Levelled logging that goes to PortalLog rather than the screen. Use this instead of bare console.log.',
            readme: 'bf6-portal-utils/logging/README.md', link: L.utils + '/tree/main/logging'
        },
        'map-detector': {
            say: 'Works out which map is running. The engine\'s own answer is unreliable, so this exists.',
            readme: 'bf6-portal-utils/map-detector/README.md', link: L.utils + '/tree/main/map-detector'
        },
        'multi-click-detector': {
            say: 'Notices a double or triple tap of the interact key. The usual way to open a hidden menu.',
            readme: 'bf6-portal-utils/multi-click-detector/README.md', link: L.utils + '/tree/main/multi-click-detector'
        },
        vectors: {
            say: 'Vector maths without spelling out XComponentOf every time, plus readable printing for logs.',
            readme: 'bf6-portal-utils/vectors/README.md', link: L.utils + '/tree/main/vectors'
        },
        raycast: {
            say: 'A friendlier wrapper around firing a line through the world.',
            readme: 'bf6-portal-utils/raycast/README.md', link: L.utils + '/tree/main/raycast'
        },
        sounds: {
            say: 'Sound and voice with the emitter pooling handled for you.',
            readme: 'bf6-portal-utils/sounds/README.md', link: L.utils + '/tree/main/sounds'
        },
        'callback-handler': {
            say: 'The subscribe and unsubscribe pattern on its own, for your own events.',
            readme: 'bf6-portal-utils/callback-handler/README.md', link: L.utils + '/tree/main/callback-handler'
        },
        benchmarker: {
            say: 'Times a block of your code so you can see what is costing the tick.',
            readme: 'bf6-portal-utils/benchmarker/README.md', link: L.utils + '/tree/main/benchmarker'
        },
        'performance-stats': {
            say: 'Running frame time and script cost, ready to put on screen.',
            readme: 'bf6-portal-utils/performance-stats/README.md', link: L.utils + '/tree/main/performance-stats'
        },
        'mod-extensions': {
            say: 'Small conveniences bolted onto the mod namespace.',
            readme: 'bf6-portal-utils/mod-extensions/README.md', link: L.utils + '/tree/main/mod-extensions'
        },
        'ffa-spawn-points': {
            say: 'Spawn placement for free for all, where nobody has a home side of the map.',
            readme: 'bf6-portal-utils/ffa-spawn-points/README.md', link: L.utils + '/tree/main/ffa-spawn-points'
        },
        'ffa-drop-ins': {
            say: 'Handles players arriving mid round in a free for all.',
            readme: 'bf6-portal-utils/ffa-drop-ins/README.md', link: L.utils + '/tree/main/ffa-drop-ins'
        },
        'scavenger-drop': {
            say: 'Notices scavenger drops so you can react to them.',
            readme: 'bf6-portal-utils/scavenger-drop/README.md', link: L.utils + '/tree/main/scavenger-drop'
        },
        'player-undeploy-fixer': {
            say: 'Works around undeploy behaving badly in some situations.',
            readme: 'bf6-portal-utils/player-undeploy-fixer/README.md', link: L.utils + '/tree/main/player-undeploy-fixer'
        },
        'portal-gadget': {
            say: 'Gadget helpers.',
            readme: 'bf6-portal-utils/portal-gadget/README.md', link: L.utils + '/tree/main/portal-gadget'
        }
    };

    // ========================================================================
    // TypeScript itself, for somebody who has never written any.
    // Keyed by a token the editor can find on a line.
    // ========================================================================
    var TS = {
        import: { say: 'Brings something in from another file or package so this file can use it. Nothing is copied; it is a reference.' },
        export: { say: 'Makes this available to other files. Portal also calls exported event functions by name, so an exported OnPlayerDeployed is what the game runs.' },
        from: { say: 'Names the file or package the import comes from.' },
        const: { say: 'A name for a value that will never be pointed at something else. Prefer this over let.' },
        let: { say: 'A name for a value you intend to change later.' },
        var: { say: 'The old way of naming a value. Use const or let instead.' },
        function: { say: 'A named block of code you can run later by calling its name.' },
        async: { say: 'Marks a function as one that can pause. It hands back a promise instead of a finished value.' },
        await: { say: 'Waits here until the thing on the right finishes, then carries on with its result. Only legal inside an async function.' },
        return: { say: 'Hands a value back to whoever called this function, and stops the function there.' },
        if: { say: 'Runs the block below only when the test in the brackets is true.' },
        else: { say: 'Runs when the if above it did not.' },
        for: { say: 'Repeats a block, usually once per item.' },
        while: { say: 'Repeats a block for as long as the test stays true. In Portal always put an await inside, or the tick never ends.' },
        'of': { say: 'In a for loop, walks the items of a list one at a time.' },
        interface: { say: 'A shape for an object: which fields it has and what type each one is. It disappears at build time.' },
        type: { say: 'A name for a shape, so you do not spell it out twice.' },
        class: { say: 'A blueprint for objects that carry both data and the functions that work on it.' },
        new: { say: 'Builds one object from a class.' },
        'undefined': { say: 'The value a name has when nothing was put in it. Test for it before using the value.' },
        'null': { say: 'A deliberate nothing, as opposed to never having been set.' },
        '=>': { say: 'An arrow function: a short function written inline, usually handed to something else to call later.' },
        '?.': { say: 'Optional chaining. Reads the property only if the thing on the left exists, otherwise gives undefined instead of crashing.' },
        '??': { say: 'Gives the value on the right only when the left one is null or undefined.' },
        '${': { say: 'A slot inside a backtick string. Whatever is in the braces is turned into text and dropped in.' },
        '...': { say: 'Spread: unpacks a list into separate items, or collects separate items into a list.' },
        '[]': { say: 'A list. Items are numbered from zero.' },
        '?:': { say: 'An optional field: the object is still valid without it.' },
        'as': { say: 'Tells the compiler to treat a value as a particular type. It changes nothing at runtime, so be sure you are right.' },
        'Promise': { say: 'A value that is not ready yet. await it to get the real one.' },
        'void': { say: 'This function hands nothing back.' },
        'number': { say: 'Any number. TypeScript does not separate whole numbers from decimals.' },
        'string': { say: 'Text.' },
        'boolean': { say: 'True or false.' }
    };

    // ========================================================================
    // The compiler's own messages, rewritten.
    // Keyed by the TypeScript error number, which Monaco reports as `code`.
    // ========================================================================
    var ERRORS = {
        1005: { cause: 'A bracket, brace or semicolon is missing where the compiler expected one.', fix: 'Look at the end of the line above as well as this one. A missing closing bracket usually shows up on the next line.' },
        1109: { cause: 'The compiler wanted a value here and found punctuation.', fix: 'Something is unfinished: an operator with nothing after it, or an extra comma.' },
        1128: { cause: 'A closing brace has nothing to close.', fix: 'There is one } too many, or one { too few further up.' },
        1308: { cause: 'await is only allowed inside a function marked async.', fix: 'Put async in front of the function that contains this line: async function name() or async (player) =>.' },
        2304: { cause: 'That name has not been defined anywhere the compiler can see.', fix: 'Check the spelling, and check the import at the top of the file. Names are case sensitive.' },
        2307: { cause: 'The file or package in the import does not exist at that path.', fix: 'If it is a package, run INSTALL. If it is your own file, the path is relative to this file and needs the .ts on the end.' },
        2322: { cause: 'The value you are assigning is not the type the target expects.', fix: 'Hover both sides to see what each one is. A common one is handing a number where a Player is wanted, or the other way round.' },
        2339: { cause: 'That property does not exist on this type.', fix: 'Type the object and a dot and let the list show you what is really there. Often the name is close but not exact.' },
        2345: { cause: 'An argument is the wrong type for this call.', fix: 'Hover the function name to see the order and the types it wants. Portal calls are strict about Player against number.' },
        2349: { cause: 'You are calling something that is not a function.', fix: 'Usually a missing pair of brackets earlier, or a property read where a method was meant.' },
        2355: { cause: 'A function that promises to return a value has a path that returns nothing.', fix: 'Add a return at the end, or change the return type to void.' },
        2454: { cause: 'The variable is used before anything was put in it.', fix: 'Give it a starting value on the line that declares it.' },
        2531: { cause: 'The value could be null here.', fix: 'Test for it first, or use ?. so the read is skipped when it is empty.' },
        2532: { cause: 'The value could be undefined here.', fix: 'Use ?. to read it safely, or return early when it is missing.' },
        2554: { cause: 'The call has the wrong number of arguments.', fix: 'Hover the function name: the signature shows how many it takes and which ones are optional.' },
        2564: { cause: 'A class field is never given a value.', fix: 'Set it in the constructor, or mark it optional with a question mark.' },
        2571: { cause: 'The value is of an unknown type, so nothing can be read off it.', fix: 'Narrow it with a typeof check first, or give the variable a real type.' },
        2580: { cause: 'A Node built-in is being used in code meant for the game.', fix: 'Portal scripts run in a small sandbox with no file system and no network. Remove it.' },
        7006: { cause: 'A parameter has no type, so the compiler assumed anything.', fix: 'Add the type: (player: mod.Player). The tool can show you the right one on hover.' },
        7053: { cause: 'An index is being used on an object whose keys are not known.', fix: 'Give the object a type with the keys spelled out, or use a Map.' }
    };

    // ========================================================================
    // CHECK MY SCRIPT.
    //
    // Each entry is a text scan with a plain-language finding. `source` names
    // where the rule comes from; anything not grounded in a named source is
    // marked verified:false and says "unverified" on screen.
    // ========================================================================
    var PITFALLS = [
        {
            id: 'spawn-then-configure',
            title: 'Object configured on the tick it was spawned',
            why: 'SpawnObject hands you a handle before the engine has finished building the thing. Settings applied on the same tick are silently lost.',
            fix: 'Put await mod.Wait(1) between the SpawnObject and the first Set call on it, and mark the function async.',
            source: 'Template src/index.ts: "Need to wait a bit before setting the vehicle spawner settings." plus the same pattern answered on the community server.',
            verified: true, recipe: 'vehicles'
        },
        {
            id: 'vo-same-tick',
            title: 'Voice played on a carrier spawned this tick',
            why: 'Same cause as above: the carrier is not ready, so the line does not play and nothing reports an error.',
            fix: 'Wait a tick after spawning the carrier, and set its team before playing anything team scoped.',
            source: 'Same tick rule as SpawnObject; team scoping from the spawner ordering answered on the community server.',
            verified: true, recipe: 'sounds'
        },
        {
            id: 'scaled-move',
            title: 'A scaled object moved by a transform write',
            why: 'The engine recomposes a scaled basis on any move, so the object comes back rotated or mirrored. SetObjectTransform carries no scale at all, so it also flattens the object back to scale 1.',
            fix: 'Keep anything you animate at scale 1. To reposition a scaled object, UnspawnObject it and SpawnObject it again with the four argument form that takes position, rotation and scale together.',
            source: 'Established in game over four deploy cycles, and independently confirmed by the bf6-MultiObjectTransform README.',
            verified: true, recipe: 'objects'
        },
        {
            id: 'move-absolute',
            title: 'MoveObject given an absolute position',
            why: 'Both arguments to MoveObject are DELTAS added to what the object already has. Passing a world position adds it to the current one and the object flies away.',
            fix: 'Pass the difference you want applied, not the destination. For a destination, use SetObjectTransform on an unscaled object.',
            source: 'Established in game on the barrier planks; the rotations added to themselves.',
            verified: true, recipe: 'objects'
        },
        {
            id: 'pooled-sfx',
            title: 'A pooled sound emitter reused while it is still sounding',
            why: 'A pooled emitter is one object, not a template. Moving it relocates the sound already playing, so the previous cue rushes across the map.',
            fix: 'Track a free-at time per emitter, assume roughly 2.5 seconds for a one shot, and pick one that has finished. Size the pool for the worst burst rather than stealing.',
            source: 'Established in game on the zombies mod; the symptom is easily mistaken for the MoveObject delta bug and fixing that does not fix this.',
            verified: true, recipe: 'sounds'
        },
        {
            id: 'damaged-async',
            title: 'An async handler on a high frequency event',
            why: 'OnPlayerDamaged itself is fine. What crashed was an async or Promise parameter leak, and any parameterised event callback leaks slowly with async making it worst.',
            fix: 'Make the handler strictly synchronous and returning void, turn the event\'s player into a number with GetObjId immediately, never store the handle, and defer real work to the tick.',
            source: 'The project optimisation spec\'s own written correction of an earlier claim. The older advice never to subscribe OnPlayerDamaged is retracted.',
            verified: true, recipe: 'first-rule'
        },
        {
            id: 'polygon-winding',
            title: 'A combat area polygon that may be wound the wrong way',
            why: 'Both official maps wind their combat volume polygons clockwise. A counter clockwise polygon is inside out, so the game treats the inside as out of bounds.',
            fix: 'Reverse the point order. The signed shoelace sum of (x2 - x1) * (z2 + z1) is positive for clockwise.',
            source: 'Diffing a broken export against MP_Aftermath and MP_Capstone. Note that combat areas are spatial JSON only: there is no script call to create, move or query one.',
            verified: true, recipe: 'areas'
        },
        {
            id: 'ui-anchor-outward',
            title: 'A UI offset that looks like it was meant to run outward',
            why: 'UIAnchor names a point on the parent and the offset runs INWARD from it. With a Right anchor a larger x moves the widget left, and a negative x pushes it off screen.',
            fix: 'Use positive insets from the anchored edge. Four boxes at the same 16,16 under the four corner anchors land one in each corner; that is the check.',
            source: 'Pinned by a community sandbox script that places four identical boxes under all four corner anchors.',
            verified: true, recipe: 'ui'
        },
        {
            id: 'setteam-rules',
            title: 'SetTeam used where it will not take',
            why: 'SetTeam only works on AI produced by an AI Spawner, only while the player is undeployed, and never onto the team they are already on. Backfill and static bots refuse it, and a call during a join throws SwitchTeamWhenPlayerJoining.',
            fix: 'Undeploy first, check the current team is different, and use AI Spawner bots when you need to move bots between teams.',
            source: 'Answered on the community server by someone who worked through all four cases, with the thrown exception quoted.',
            verified: true, recipe: 'teams'
        },
        {
            id: 'subscribe-no-unsubscribe',
            title: 'A per player subscription with no matching unsubscribe',
            why: 'A subscription made per player keeps running after that player leaves, and keeps whatever it captured alive with it.',
            fix: 'Keep the function subscribe hands back and call it when the context ends, usually in OnPlayerLeaveGame.',
            source: 'The template\'s own vehicle example, which unsubscribes from OnVehicleSpawned as soon as it has seen its vehicle.',
            verified: true, recipe: 'first-rule'
        },
        {
            id: 'setup-in-ongoing',
            title: 'One time setup inside an Ongoing handler',
            why: 'Ongoing handlers run about thirty times a second. Setting up capture points or HQs there redoes the same work every tick.',
            fix: 'Move it into OnGameModeStarted, which runs once.',
            source: 'Answered on the community server on a script that set up five capture points in OngoingGlobal.',
            verified: true, recipe: 'capture'
        },
        {
            id: 'event-handler-signature',
            title: 'An event handler that does not take what the engine sends',
            why: 'An exported handler is just an exported function, so TypeScript never compares it against the event. The wrong parameter compiles perfectly and is then refused when you save on Portal, which is where the community\'s most reported Action Needed failure comes from. Nothing in the editor points at the line, because as far as the compiler is concerned nothing is wrong.',
            fix: 'Match the declared shape parameter for parameter. node_modules/bf6-portal-mod-types/event-handler-signatures.d.ts is the list. The one that catches people is OnPlayerLeaveGame, which takes (eventNumber: number) and not a Player: by the time it fires the player is gone, so all the engine can give you is their id.',
            source: 'event-handler-signatures.d.ts in bf6-portal-mod-types, checked against a live case in this tool\'s own corpus where OnPlayerLeaveGame was written as (eventPlayer: mod.Player).',
            verified: true, recipe: 'diagnose'
        },
        {
            id: 'raw-string-message',
            title: 'Text shown to a player that is not in strings.json',
            why: 'Portal requires every displayed string to be declared. A mod.Message built from a key that does not exist tends to stop the script where it stands.',
            fix: 'Add the key to src/strings.json, then use mod.Message(mod.stringkeys.your.key).',
            source: 'The template README\'s troubleshooting section, and a community feature request asking for exactly this restriction to be lifted.',
            verified: true, recipe: 'ui'
        },
        {
            id: 'matchtime-hot-path',
            title: 'GetMatchTimeElapsed called inside a per event handler',
            why: 'It is an engine call, and on a hot path it is paid for on every single fire.',
            fix: 'Read the clock once in the tick and pass the number to whatever needs it.',
            source: 'The hot path rule from the zombies economy work.',
            verified: true, recipe: 'timers'
        },
        {
            id: 'console-log-hot',
            title: 'console.log on a path that runs every tick',
            why: 'PortalLog grows fast, and on a dedicated server the send is quota limited. A per tick log is also the easiest way to make your own match lag.',
            fix: 'Throttle it to a few times a second, or use the logging module\'s levels so you can turn it down.',
            source: 'The debug bridge telemetry protocol, which throttles its own periodic samples for this reason.',
            verified: true, recipe: 'diagnose'
        },
        {
            id: 'unguarded-native',
            title: 'An engine call on a handle that may be gone',
            why: 'A failing native may log rather than throw, and a throw stack dumps into PortalLog even when you caught it. A fifty millisecond loop on a stale player produced tens of thousands of log lines in one match and real in match lag.',
            fix: 'Guard with mod.IsPlayerValid or mod.IsValid before per tick native calls.',
            source: 'Four live hosted matches on the spectating work, plus the retracted claims note on natives that log instead of throwing.',
            verified: true, recipe: 'diagnose'
        }
    ];

    // ========================================================================
    // RECIPES.
    //
    // Each recipe is real code with a note per line. `lines` is an array of
    // [code, note]; a note of null means the line needs no explaining.
    // ========================================================================
    function R(id, title, when, source, links, lines, extra) {
        var r = { id: id, title: title, when: when, source: source, links: links || [], lines: lines };
        if (extra) for (var k in extra) r[k] = extra[k];
        return r;
    }

    var RECIPES = [
        R('first-rule', 'My first rule',
            'Start here. It puts one line of text on a player\'s screen the moment they spawn in, which is enough to prove your script is running at all.',
            'The template\'s own plain boilerplate, trimmed to the one thing that matters first.',
            [{ label: 'Template README', url: L.template + '#understanding-the-code' }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'Brings in the Events helper. It wraps the game\'s own handlers so you can add and remove listeners in code.'],
                ['', null],
                ['function onDeployed(player: mod.Player): void {', 'Our own function. It takes the player who just spawned. It is NOT async, so it cannot pause, which is exactly right for something this small.'],
                ['    const message = mod.Message(mod.stringkeys.myMod.welcome);', 'Turns the key myMod.welcome from strings.json into text the game can show. Text never goes straight from your code to the screen.'],
                ['    mod.DisplayNotificationMessage(message, player);', 'Puts that text in the middle of this one player\'s screen.'],
                ['}', null],
                ['', null],
                ['Events.OnPlayerDeployed.subscribe(onDeployed);', 'Registers what happens when a player deploys. Without this line the function above is never called.']
            ],
            {
                strings: { myMod: { welcome: 'Your script is running.' } },
                nextSteps: ['Change the text in src/strings.json and build again.', 'Add a second subscription for OnPlayerDied and log who killed whom.']
            }),

        R('teams', 'Team setup and spawn control',
            'Use it when one team should be held back at round start, or when you need players and bots on particular sides.',
            'The hold pattern is a community answer to exactly this question. The SetTeam restrictions are a community answer listing all four cases, with the thrown exception quoted.',
            [{ label: 'Portal hub', url: L.hub }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ['', null],
                ['const ATTACKERS = 1;', 'Team numbers in Portal are just 1 and 2. Naming them here keeps the rest readable.'],
                ['const HOLD_SECONDS = 20;', 'How long the attackers wait before they may deploy.'],
                ['', null],
                ['async function onJoin(player: mod.Player): Promise<void> {', 'async, because this function pauses. A function that contains await must say async.'],
                ['    if (!mod.IsPlayerValid(player)) return;', 'A player can be gone again before this runs. Always check before touching one.'],
                ['', null],
                ['    if (!mod.Equals(mod.GetTeam(player), mod.GetTeam(ATTACKERS))) return;', 'Only the attackers are held. Everyone else falls straight out of this function.'],
                ['', null],
                ['    mod.EnablePlayerDeploy(player, false);', 'Greys out the deploy button for this one player. Nobody else is affected.'],
                ['    await mod.Wait(HOLD_SECONDS);', 'Pauses HERE for twenty seconds. The match keeps running; only this function is waiting.'],
                ['', null],
                ['    if (!mod.IsPlayerValid(player)) return;', 'Twenty seconds is long enough for them to have quit. Check again after every wait.'],
                ['    mod.EnablePlayerDeploy(player, true);', 'Lets them in.'],
                ['}', null],
                ['', null],
                ['Events.OnPlayerJoinGame.subscribe(onJoin);', 'Runs the whole thing once per player as they arrive.']
            ],
            {
                warn: 'SetTeam is fussy. It works only on bots that came from an AI Spawner, only while the player is undeployed, and never onto the team they are already on. Calling it while a join is in progress throws SwitchTeamWhenPlayerJoining.',
                strings: {}
            }),

        R('capture', 'Capture points and HQs',
            'Use it when you are building a Conquest style mode on a map where you placed the points yourself.',
            'The one time setup placement is a community correction to a script that did this in OngoingGlobal. The requirement that HQs sit outside the combat volume comes from diffing official map exports.',
            [{ label: 'Portal hub', url: L.hub }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ['', null],
                ['const POINTS = [2001, 2002, 2003, 2004, 2005];', 'The ObjIds you gave the capture points in the map. INSERT SELECTED OBJECT fills these in from the scene for you.'],
                ['const HQS = [4001, 4002];', 'The two headquarters, one per team.'],
                ['', null],
                ['function onModeStarted(): void {', 'This runs ONCE. Setup does not belong in an Ongoing handler, which runs about thirty times a second.'],
                ['    for (const id of POINTS) {', 'Walks the list of ids one at a time.'],
                ['        const point = mod.GetCapturePoint(id);', 'Turns the number into the real capture point on the map.'],
                ['        mod.SetCapturePointCapturingTime(point, 12);', 'Twelve seconds to take a point.'],
                ['        mod.SetCapturePointNeutralizationTime(point, 6);', 'Six seconds to knock an enemy point back to neutral first.'],
                ['    }', null],
                ['', null],
                ['    for (const id of HQS) {', null],
                ['        mod.EnableHQ(mod.GetHQ(id), true);', 'Switches each headquarters on.'],
                ['    }', null],
                ['}', null],
                ['', null],
                ['function onCaptured(point: mod.CapturePoint): void {', 'Called when a point changes hands.'],
                ['    const owner = mod.GetCurrentOwnerTeam(point);', 'Which team took it.'],
                ['    mod.SetGameModeScore(owner, mod.GetGameModeScore(owner) + 100);', 'Reads the score, adds to it, writes it back.'],
                ['}', null],
                ['', null],
                ['Events.OnGameModeStarted.subscribe(onModeStarted);', 'Wires the one time setup.'],
                ['Events.OnCapturePointCaptured.subscribe(onCaptured);', 'Wires the scoring.']
            ],
            {
                warn: 'The out of bounds combat area is not scriptable at all. There is no create, move, resize or query call and no out of bounds event: it exists only in the map\'s spatial JSON, its polygon must be wound clockwise, and the HQs must sit outside it.'
            }),

        R('areas', 'Area triggers and zones',
            'Use it for a room that does something when you walk in: a safe zone, a trap, a team only area, the door to a locked part of the map.',
            'The team only area pattern is a community answer describing copying an HQ polygon into an area trigger. The hierarchy as configuration convention is the tool\'s own zombies work.',
            [{ label: 'Portal hub', url: L.hub }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ['', null],
                ['const ENEMY_HQ_AREA = 201;', 'The ObjId of the area trigger you placed over the other team\'s spawn.'],
                ['const OWNER_TEAM = 1;', 'The team allowed to stand in it.'],
                ['const visit = new Map<number, number>();', 'Which VISIT each player is currently on, by player id. Not just whether they are inside: see the check after the wait for why that is not enough.'],
                ['let nextVisit = 1;', 'A number handed out per entry, so two visits by the same player are never confused for one.'],
                ['', null],
                ['async function onEnter(player: mod.Player, area: mod.AreaTrigger): Promise<void> {', 'Fires the moment somebody crosses into the volume. The PLAYER comes first: the event supplies (player, areaTrigger) in that order.'],
                ['    if (mod.GetObjId(area) !== ENEMY_HQ_AREA) return;', 'One handler serves every area trigger on the map, so check which one fired first.'],
                ['    if (mod.Equals(mod.GetTeam(player), mod.GetTeam(OWNER_TEAM))) return;', 'The owning team is welcome. Leave them alone.'],
                ['', null],
                ['    const id = mod.GetObjId(player);', 'Their id, taken BEFORE the wait: it is what the exit handler will clear.'],
                ['    const mine = nextVisit++;', 'This visit\'s own number, kept in the handler that is about to sleep.'],
                ['    visit.set(id, mine);', 'Mark them as inside, on this visit.'],
                ['    mod.DisplayNotificationMessage(mod.Message(mod.stringkeys.myMod.leaveArea), player);', 'Warn them first. A kill with no warning reads as a bug.'],
                ['    await mod.Wait(5);', 'Five seconds of grace. The rest of the mode keeps running during this.'],
                ['', null],
                ['    if (visit.get(id) !== mine) return;', 'THEY DID WHAT THEY WERE TOLD, or they are on a later visit. Checking only whether they are inside is not enough: leave at two seconds and step back in at three, and this handler wakes at five, sees them inside again, and kills them two seconds into a grace period that is supposed to be five. Comparing the visit number means every entry gets its own full five seconds, and an old handler waking up can tell that it is stale. An await does not cancel itself.'],
                ['    if (!mod.IsPlayerValid(player)) return;', 'They may have left the match in those five seconds.'],
                ['    mod.Kill(player);', 'Kills them through the engine\'s own death path, so everything that normally follows a death still happens.'],
                ['}', null],
                ['', null],
                ['function onExit(player: mod.Player, area: mod.AreaTrigger): void {', 'Fires when somebody leaves the volume. Same order as above.'],
                ['    if (mod.GetObjId(area) !== ENEMY_HQ_AREA) return;', 'Same check as above.'],
                ['    visit.delete(mod.GetObjId(player));', 'This is what cancels the pending kill: the waiting handler compares its visit number against this map when it wakes up, and a missing entry can never match.'],
                ['    mod.DisplayNotificationMessage(mod.Message(mod.stringkeys.myMod.areaClear), player);', 'Tell them they are clear.'],
                ['}', null],
                ['', null],
                ['Events.OnPlayerEnterAreaTrigger.subscribe(onEnter);', 'Wires entering. The event name carries the Trigger suffix; OnPlayerEnterArea does not exist and will not compile.'],
                ['Events.OnPlayerExitAreaTrigger.subscribe(onExit);', 'Wires leaving.']
            ],
            {
                warn: 'An area trigger whose volume has no height never fires. Give it real height in the map. Interact points are no longer capped at eighteen: that was an engine bug and it is patched, so keep counts low for performance rather than for correctness.',
                strings: { myMod: { leaveArea: 'Leave the enemy spawn', areaClear: 'You are clear' } }
            }),

        R('timers', 'Timers and round flow',
            'Use it for anything on a clock: a countdown, a repeating tick, a round that ends itself.',
            'The template\'s own telemetry interval, which stores the handle and clears it when the player leaves.',
            [{ label: 'Timers module', url: L.utils + '/tree/main/timers' }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ["import { Timers } from 'bf6-portal-utils/timers/index.ts';", 'setInterval and setTimeout for Portal.'],
                ['', null],
                ['let tick: number | undefined;', 'Where we keep the timer\'s handle. Without it the timer can never be stopped.'],
                ['let secondsLeft = 300;', 'Five minutes.'],
                ['', null],
                ['function onModeStarted(): void {', 'Runs once when the round starts.'],
                ['    tick = Timers.setInterval(() => {', 'Runs the block below once a second, for ever, until something clears it.'],
                ['        secondsLeft -= 1;', 'Count down.'],
                ['', null],
                ['        if (secondsLeft === 60) {', null],
                ['            mod.DisplayNotificationMessage(mod.Message(mod.stringkeys.myMod.oneMinute));', 'A single warning at the one minute mark, to everybody: the overload with no player is the all-players one. Not SetHUDTicker, which picks which ticker STYLE the HUD uses (None or Ticker_Conquest) and takes no text at all.'],
                ['        }', null],
                ['', null],
                ['        if (secondsLeft <= 0) {', null],
                ['            Timers.clearInterval(tick);', 'Stop the timer FIRST. A timer that outlives its purpose keeps costing the tick.'],
                ['            tick = undefined;', 'Forget the handle so nothing tries to clear it twice.'],
                ['            mod.EndGameMode(mod.GetTeam(1));', 'Ends the round and declares team 1 the winner.'],
                ['        }', null],
                ['    }, 1000);', 'One thousand milliseconds: once a second.'],
                ['}', null],
                ['', null],
                ['Events.OnGameModeStarted.subscribe(onModeStarted);', 'Wires it up.']
            ],
            {
                warn: 'Ongoing handlers run about thirty times a second. Anything you can do on an event or a timer instead of a tick, do there.',
                strings: { myMod: { oneMinute: 'One minute left' } }
            }),

        R('ui', 'HUD text and widgets',
            'Use it to put your own text, counters or buttons on the screen.',
            'The UI module\'s own README. The inward inset rule was pinned by a community sandbox script that places four identical boxes under all four corner anchors.',
            [{ label: 'UI module', url: L.utils + '/tree/main/ui' }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ["import { UI } from 'bf6-portal-utils/ui/index.ts';", 'The shared UI namespace: colours, anchors and the base types.'],
                ["import { UIText } from 'bf6-portal-utils/ui/components/text/index.ts';", 'The text widget itself. Each component is imported from its own path; there is no UI.Text.'],
                ['', null],
                ['function onDeployed(player: mod.Player): void {', 'One HUD per player, built when they spawn in.'],
                ['    new UIText(', 'Creates a text widget. It shows itself; nothing else is needed.'],
                ['        {', null],
                ['            x: 24,', 'Twenty four pixels IN from the anchored edge. Not from the left of the screen.'],
                ['            y: 24,', 'Twenty four pixels in from the anchored edge vertically.'],
                ['            width: 400,', 'The canvas is the 1920 by 1080 safe area, whatever resolution the player runs.'],
                ['            height: 40,', null],
                ['            anchor: mod.UIAnchor.TopRight,', 'Hangs from the top right. With this anchor a LARGER x moves the widget further LEFT.'],
                ['            message: mod.Message(mod.stringkeys.myMod.hud),', 'Text always comes from strings.json through mod.Message.'],
                ['            textColor: UI.COLORS.WHITE,', null],
                ['            receiver: player', 'Whose screen it goes on. UI in Portal is always per player, and the receiver is a field of the parameters, not a second argument.'],
                ['        }', null],
                ['    );', null],
                ['}', null],
                ['', null],
                ['Events.OnPlayerDeployed.subscribe(onDeployed);', 'Wires it up.']
            ],
            {
                warn: 'Offsets are inward insets from the anchored edge, not screen coordinates. A negative offset pushes the widget off the screen, which looks exactly like the widget failing to appear.',
                strings: { myMod: { hud: 'Score: 0' } }
            }),

        R('vehicles', 'Vehicles and spawners',
            'Use it to put a vehicle in front of a player, or to control what a placed spawner produces.',
            'The template\'s own example experience, including its comment about waiting before configuring the spawner and its subscribe then unsubscribe lifecycle.',
            [{ label: 'Template README', url: L.template + '#what-this-template-does-out-of-the-box' }],
            [
                ['async function spawnAhead(player: mod.Player, type: mod.VehicleList): Promise<void> {', 'async, because we have to wait partway through.'],
                ['    const at = mod.GetSoldierState(player, mod.SoldierStateVector.GetPosition);', 'Where the player is standing.'],
                ['    const facing = mod.GetSoldierState(player, mod.SoldierStateVector.GetFacingDirection);', 'Which way they are looking, as a direction of length one.'],
                ['', null],
                ['    const position = mod.CreateVector(', 'Builds the spot twenty metres in front of them.'],
                ['        mod.XComponentOf(at) + mod.XComponentOf(facing) * 20,', 'Twenty metres along x in the direction they face.'],
                ['        mod.YComponentOf(at),', 'Same height as the player. y is up in Portal.'],
                ['        mod.ZComponentOf(at) + mod.ZComponentOf(facing) * 20', 'Twenty metres along z.'],
                ['    );', null],
                ['', null],
                ['    const spawner = mod.SpawnObject(', 'Creates a vehicle spawner in the world.'],
                ['        mod.RuntimeSpawn_Common.VehicleSpawner,', 'What to create. This one exists on every map.'],
                ['        position,', 'Where.'],
                ['        mod.CreateVector(0, 0, 0)', 'Rotation, in degrees. Zero means unturned.'],
                ['    ) as mod.VehicleSpawner;', 'Tells the compiler what kind of object came back, so the calls below type check.'],
                ['', null],
                ['    await mod.Wait(1);', 'THE IMPORTANT LINE. The spawner is not ready to configure on the tick it was created. Settings applied before this wait are silently lost.'],
                ['', null],
                ['    mod.SetVehicleSpawnerVehicleType(spawner, type);', 'Which vehicle it makes.'],
                ['    mod.SetVehicleSpawnerAutoSpawn(spawner, true);', 'Let it produce one by itself.'],
                ['    mod.SetVehicleSpawnerRespawnTime(spawner, 1);', 'One second, so the vehicle appears almost at once.'],
                ['}', null]
            ],
            {
                warn: 'A spawner you created and never removed keeps costing the server. When you are finished with it, UnspawnObject it.'
            }),

        R('sounds', 'Sounds and voice carriers',
            'Use it to play a cue at a place in the world, or a spoken line.',
            'Established in game on the zombies mod: a pooled emitter is one object, and retriggering it while it is still sounding drags the previous cue across the map.',
            [{ label: 'Sounds module', url: L.utils + '/tree/main/sounds' }],
            [
                ['const VOICES = [3001, 3002, 3003, 3004];', 'Four emitters placed on the map. A pool, because one emitter can only be in one place at a time.'],
                ['const CUE_SECONDS = 2.5;', 'How long a one shot runs. There is no way to ask the engine, so assume the longest cue you use.'],
                ['', null],
                ['const freeAt = new Map<number, number>();', 'When each emitter is free again, keyed by its ObjId.'],
                ['', null],
                ['function playAt(position: mod.Vector, now: number): void {', 'now is the clock read ONCE by the caller, not re-read here.'],
                ['    const id = VOICES.find((v) => (freeAt.get(v) ?? 0) <= now);', 'The first emitter that has finished. ?? 0 means an emitter never used yet counts as free.'],
                ['    if (id === undefined) return;', 'Everything is busy. Dropping the cue is better than dragging one that is still playing.'],
                ['', null],
                ['    const sfx = mod.GetSFX(id);', 'The emitter itself.'],
                ['    mod.PlaySound(sfx, 1, position, 60);', 'Full volume, at that position, audible out to sixty metres.'],
                ['    freeAt.set(id, now + CUE_SECONDS);', 'Book it out until the cue has finished.'],
                ['}', null]
            ],
            {
                warn: 'Never move a pooled emitter that is still sounding: MoveObject relocates the emitter, so the sound already in flight travels with it. A staggered cue must also read its subject\'s position at PLAY time, not at queue time, because the subject has moved.'
            }),

        R('bots', 'Bots',
            'Use it when you want bots that do what you tell them rather than whatever the mode gives you.',
            'Community answers: bots need a tick before they take orders, backfill and static bots ignore objectives, waypoint paths do not currently work, and AIBattlefieldBehavior is what makes a bot play the objective.',
            [{ label: 'Portal community', url: L.discord }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ['', null],
                ['const SPAWNER = 5001;', 'The ObjId of an AI Spawner you placed on the map.'],
                ['const OBJECTIVE = 2001;', 'A capture point to send them to.'],
                ['', null],
                ['function onModeStarted(): void {', 'Once, at round start.'],
                ['    for (let i = 0; i < 8; i += 1) {', 'Eight bots.'],
                ['        mod.SpawnAIFromAISpawner(mod.GetSpawner(SPAWNER));', 'Each call produces one bot from that spawner.'],
                ['    }', null],
                ['}', null],
                ['', null],
                ['async function onSpawned(bot: mod.Player): Promise<void> {', 'Fires as each bot arrives.'],
                ['    await mod.Wait(1);', 'THE IMPORTANT LINE. A bot is not fully there on the tick it spawns, and orders given before this are ignored.'],
                ['    if (!mod.IsPlayerValid(bot)) return;', 'It may already be gone.'],
                ['', null],
                ['    mod.AIBattlefieldBehavior(bot);', 'Makes it play the objective the way a normal Battlefield bot does.'],
                ['    mod.AIMoveToBehavior(bot, mod.GetObjectPosition(mod.GetSpatialObject(OBJECTIVE)));', 'Sends it to the point. Waypoint paths do not currently work, so give it a position.'],
                ['}', null],
                ['', null],
                ['Events.OnGameModeStarted.subscribe(onModeStarted);', 'Wires the spawning.'],
                ['Events.OnSpawnerSpawned.subscribe(onSpawned);', 'Wires the orders.']
            ],
            {
                warn: 'Only bots produced by an AI Spawner take these orders and accept SetTeam. Backfill and static bots, the ones the site\'s Teams tab creates, ignore both and only go for MCOMs.'
            }),

        R('objects', 'Props and moving things',
            'Use it to put a prop in the world and move it around: a barrier, a lift, a piece of debris that sinks when a door opens.',
            'Established in game across four deploy cycles: both MoveObject arguments are deltas, and a scaled object cannot be moved at all.',
            [],
            [
                ['const PROP = 6001;', 'The ObjId of a prop you placed on the map.'],
                ['', null],
                ['function sink(): void {', 'Drops the prop out of sight, which is how a debris door opens.'],
                ['    const prop = mod.GetSpatialObject(PROP);', 'The prop itself.'],
                ['    mod.MoveObject(prop, mod.CreateVector(0, -50, 0), mod.CreateVector(0, 0, 0));', 'BOTH arguments are DELTAS added to what the prop already has. This is fifty metres DOWN from wherever it is, not a position.'],
                ['}', null],
                ['', null],
                ['async function replaceScaled(): Promise<void> {', 'How to reposition something that was spawned at a scale other than 1.'],
                ['    const prop = mod.GetSpatialObject(PROP);', null],
                ['    mod.UnspawnObject(prop);', 'Remove it. A scaled object cannot be moved: the engine rebuilds a scaled basis on any move and it comes back wrong.'],
                ['    await mod.Wait(0.1);', 'Let the removal settle before creating the replacement.'],
                ['    mod.SpawnObject(', 'The four argument form is the only call that sets position, rotation and scale together.'],
                ['        mod.RuntimeSpawn_Common.CrateAmmo_01,', 'What to create. The members are specific: Crate_01_A, CrateAmmo_01 and so on. There is no plain Crate.'],
                ['        mod.CreateVector(0, 0, 0),', 'Where.'],
                ['        mod.CreateVector(0, 0, 0),', 'Rotation.'],
                ['        mod.CreateVector(0.35, 0.35, 0.35)', 'Scale. Anything you intend to ANIMATE should be left at 1 instead.'],
                ['    );', null],
                ['}', null]
            ],
            {
                warn: 'Runtime spawned props DO have collision. An older claim that they do not, repeated widely, is false and was checked against the live game.'
            }),

        R('diagnose', 'Logging and finding out what went wrong',
            'Use it from the first day. Without it a script that does nothing and a script that crashed on line two look identical.',
            'The debug bridge telemetry protocol and the logging module. PortalLog is written only while you host locally.',
            [{ label: 'Logging module', url: L.utils + '/tree/main/logging' }],
            [
                ["import { Events } from 'bf6-portal-utils/events/index.ts';", 'The event helper.'],
                ["import { Logger } from 'bf6-portal-utils/logger/index.ts';", 'Prints text on screen while you play.'],
                ['', null],
                ['let logger: Logger | undefined;', 'One logger, for the admin.'],
                ['', null],
                ['function onJoin(player: mod.Player): void {', null],
                ['    if (mod.GetObjId(player) !== 0) return;', 'Player zero is the admin on a non persistent test server. Everyone else gets a clean screen.'],
                ['', null],
                ['    logger = new Logger(player, {', 'A scrolling console in the corner of the admin\'s screen.'],
                ['        staticRows: false,', 'Scrolling rather than fixed rows.'],
                ['        visible: true,', null],
                ['        anchor: mod.UIAnchor.TopLeft,', null],
                ['    });', null],
                ['', null],
                ['    console.log(`[EVT] name=join id=${mod.GetObjId(player)}`);', 'Goes to PortalLog on disk while you host locally. The tool\'s DIAGNOSE panel reads that file live.'],
                ['}', null],
                ['', null],
                ['Events.OnPlayerJoinGame.subscribe(onJoin);', 'Wires it up.']
            ],
            {
                warn: 'PortalLog is written to your Temp folder only while you use Host Locally. Keep per tick logging throttled: it grows fast, and on a dedicated server the send is quota limited.'
            })
    ];

    // ========================================================================
    // Why every line of the template's boilerplate is there.
    // Keyed by a distinctive fragment of the line.
    // ========================================================================
    var BOILERPLATE = {
        "bf6-portal-utils/events": 'The Events helper. Portal\'s own handlers are exported functions with fixed names; this wraps them so you can subscribe and unsubscribe wherever you like.',
        'multi-click-detector': 'Notices a double or triple tap of the interact key. It is how the debug menu opens without taking a key away from the game.',
        'map-detector': 'Works out which map is running. The engine\'s own answer is unreliable, which is why this module exists at all.',
        "'./debug-tool/index.ts'": 'Your own debug tool, in the folder next door. A relative path with the .ts on the end, which is how this project imports its own files.',
        'let adminDebugTool': 'Somewhere to keep the tool between events. It starts undefined because nobody has joined yet.',
        'mod.GetObjId(player) != 0': 'Player zero is the admin on a non persistent test server. This is the whole of the admin check.',
        'new DebugTool(': 'Builds the tool for that one player. Nobody else sees it.',
        'new MultiClickDetector(': 'From now on, triple tapping interact opens the menu.',
        'adminDebugTool?.destroy()': 'The question mark means: only if it exists. Cleaning up when the admin leaves is what keeps the mod from leaking.',
        'Events.OnPlayerJoinGame.subscribe': 'Runs the line above once per player as they arrive.',
        'Events.OnPlayerLeaveGame.subscribe': 'The matching cleanup. Every per player subscription wants one of these.',
        'Events.OnPlayerDeployed.subscribe': 'Runs each time a player spawns into the world, which is more often than joining.',
        'MapDetector.currentMap()': 'Can come back undefined when the map cannot be worked out, which is why the next line tests it.',
        'mod.stringkeys.': 'A key into src/strings.json. Portal will not show text that was not declared there.',
        'await mod.Wait(1)': 'A freshly spawned object is not ready to configure on the same tick. This wait is the fix, and leaving it out fails silently.',
        'as mod.VehicleSpawner': 'Tells the compiler which kind of object came back from SpawnObject, so the calls that follow type check.',
        'unsubscribeFromOn': 'subscribe hands back the function that undoes it. Calling it as soon as this context is finished is what stops the handler running for ever.',
        'Timers.clearInterval': 'Stops a repeating timer. A timer nobody clears keeps costing the tick after the thing it served is gone.'
    };

    // ========================================================================
    // PortalLog line patterns, for the DIAGNOSE tail.
    // ========================================================================
    var LOGLINES = [
        { re: /QuickJS:\s*Exception:?\s*(.*)$/i, kind: 'error', say: 'The script threw and stopped where it stood. The line after this one in the log usually names the function.' },
        { re: /Exception:\s*SwitchTeamWhenPlayerJoining/i, kind: 'error', say: 'SetTeam was called while that player was still joining. Wait until they are in, and undeploy them first.' },
        { re: /Error info:\s*(.*)$/i, kind: 'error', say: 'The engine\'s own reason for refusing the call above.' },
        { re: /at\s+(\w+)\s*\(native\)/i, kind: 'error', say: 'The engine call that refused. Everything above it in the stack is your code.' },
        { re: /at\s+<eval>\s*\(<input>:(\d+)\)/i, kind: 'error', say: 'A line number in the bundle you uploaded, not in your source file.' },
        { re: /at\s+(\w+)\s*\(main:(\d+)\)/i, kind: 'error', say: 'Function and line in the uploaded bundle.' },
        { re: /QuickJS:\s*console\.log:\s*(.*)$/i, kind: 'log', say: 'Your own console.log.' },
        { re: /\[TLM\]\s*(.*)$/i, kind: 'telemetry', say: 'A periodic numeric sample from your own telemetry.' },
        { re: /\[EVT\]\s*(.*)$/i, kind: 'event', say: 'An event your own code reported.' },
        { re: /\[PARAM\]\s*(.*)$/i, kind: 'param', say: 'A tunable your own debug console published.' }
    ];

    return {
        version: 1,
        links: L,
        mod: MOD,
        utils: UTILS,
        ts: TS,
        errors: ERRORS,
        pitfalls: PITFALLS,
        recipes: RECIPES,
        boilerplate: BOILERPLATE,
        loglines: LOGLINES
    };
})();
