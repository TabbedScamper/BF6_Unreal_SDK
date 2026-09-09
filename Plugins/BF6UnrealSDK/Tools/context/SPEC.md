# BF6 context tools - the shared contract

Why this exists. The tool is 30 C++ files, 145 console commands, 18 log
categories, a browser page, a converter and a high poly add-on. Working on it
means guessing which of those a symptom belongs to, and a wrong guess costs a
whole investigation. These tools replace the guessing with a lookup.

Four parts, each a separate file, each usable on its own from the command line
and together from one MCP toolset.

    Tools/context/log_index.py          what happened, in order
    Tools/context/inventory.py          what exists
    Tools/context/gaps.py               what is missing or unwired
    Content/Python/bf6_context_mcp.py   all three, plus live state, over MCP

Python 3.11. Standard library only. Windows paths. No Unreal import in the
first three, they must run from a plain shell with the editor closed.

## Roots

    ROOT     C:/Users/mwalt/Documents/Unreal Projects/BF6_High_Poly
    PLUGIN   ROOT/Plugins/BF6UnrealSDK
    ADDON    ROOT/Plugins/Add-Ons/BF6HighPoly
    LOGS     ROOT/Saved/Logs
    OUT      ROOT/Saved/BF6Context     generated json lives here, never in the plugin

Discover ROOT by walking up from __file__ until BF6_Unreal_SDK.uproject is
found. Never hard code a user name.

## The subsystem is the unit

Everything keys on a subsystem id. The list is derived, not hard coded, but
these are the expected ones and the derivation must produce them:

    blocks buildmode editoroverlay experience extension objids outliner
    portalprofile portalsettings portalweb project script sdkimport
    toolcommands uibuilder uisound previewviewport
    highpoly highpolycore highpolydiskcache highpolygamemode highpolyplaced
    highpolypreviews highpolyuisounds highpolyviewport highpolywaterfft
    highpolywaterlab highpolywatergpu highpolywatersurface

Rule for deriving: a Private/*.cpp file named BF6<Name>.cpp is subsystem
lower(<Name>). Files under Plugins/Add-Ons carry "addon": true, the rest
"addon": false. This boundary matters. Anything that does not read the real
game belongs in the base tool, so a gap report must say which side it is on.

## inventory.py

Writes OUT/inventory.json and prints a summary. Shape:

    {
      "generated": "<iso8601>",
      "subsystems": {
        "blocks": {
          "addon": false,
          "sources": ["Plugins/.../BF6Blocks.cpp"],
          "log_categories": ["LogBF6Blocks"],
          "commands": [ {"name":"BF6.Blocks.Open","help":"...","file":"...","line":123} ],
          "web_ops": { "in": ["ready","autosave"], "out": ["mapChanged","load"] },
          "resources": ["Resources/blocks/editor_ui.js"],
          "docs": ["Resources/blocks/ORGANISATION.md"],
          "mcp_tools": []
        }
      },
      "commands": { "BF6.Blocks.Open": "blocks" },
      "log_categories": { "LogBF6Blocks": "blocks" }
    }

All paths relative to ROOT. How to find each thing:

  commands       FAutoConsoleCommand, FAutoConsoleCommandWithWorldAndArgs, and
                 IConsoleManager::Get().RegisterConsoleCommand. Take the
                 TEXT("BF6...") name and the NEXT TEXT(...) literal as the help
                 string. Record file and line. There are about 145 of them. If
                 you find fewer than 140 your parser is wrong, and you should
                 say so rather than shipping it.
  log_categories DEFINE_LOG_CATEGORY and DEFINE_LOG_CATEGORY_STATIC.
  web_ops        The page and C++ talk in ops. C++ receiving looks like
                 Op == TEXT("x"). C++ sending builds json with a field named op.
                 Page side is Resources/**/*.js, msg.op == 'x' to receive and a
                 post of an object with op set to send. "in" is page to C++ and
                 "out" is C++ to page.
  resources      Resources/<subsystem-ish>/ plus any Resources path appearing as
                 a string literal in the subsystem source.
  docs           Markdown under Resources/ near those resources.

Command line:

    python inventory.py                             rebuild and summarise
    python inventory.py blocks                      all about one subsystem
    python inventory.py --command BF6.Portal.Link   which subsystem owns it

## log_index.py

The logs are the only record of what actually happened. Saved/Logs holds the
live log plus dated backups, and some are hundreds of MB, so stream. Never read
one whole into memory.

A line looks like:

    [2026.09.07-07.12.47:123][456]LogBF6Portal: Display: Portal resume: ...

Timestamp and frame counter are both optional on continuation lines. Attach a
continuation to the event above it.

API, importable and mirrored on the command line:

    sessions()
        [{"file":..,"start":iso,"end":iso,"lines":n,"engine":"5.8","cmdline":..}]
        a session starts at the log file open banner

    events(category=None, level=None, pattern=None, since=None, until=None,
           session=None, limit=500)
        [{"time":iso,"frame":int,"category":..,"level":..,"text":..,
          "file":..,"line":n}]

    around(pattern, before=40, after=40, occurrence="last")
        the events surrounding a match. This is the "what led up to it" call and
        it is the point of the whole file.

    loops(min_repeats=5)
        repeated near identical messages
        [{"text":..,"count":89,"first":iso,"last":iso,"period_s":2.1}]
        Normalise numbers and quoted strings to a placeholder before comparing,
        so messages differing only in an id collapse together.

    errors(since=None)
        Error and Warning events grouped by message

    summary(session=None)
        per category counts, first and last time, error count, loop count

Real case this must answer, use it as your own test. The log holds 89
occurrences of

    LogBF6Portal: Display: Portal resume: the page answered - kind 'blocks',
    signed-in list not present, state Sign in again

repeating every 1 to 3 seconds. loops() must surface it, and around() on it must
show what preceded the first one.

Command line:

    python log_index.py sessions
    python log_index.py loops
    python log_index.py around "Portal resume" --before 60
    python log_index.py events --category LogBF6Blocks --limit 100
    python log_index.py summary

## gaps.py

Reads inventory.json, rebuilding it by importing inventory.py if stale, and
reports what is unfinished. Every finding carries a subsystem, whether it is
base tool or add-on, and a file and line where one exists. Categories:

    unreachable_op    C++ handles an op no page ever sends, or a page sends one
                      no C++ handles. Both directions.
    no_status         a subsystem with commands but no Status command
    undocumented      a command whose help string is empty or a placeholder
    unfinished        TODO, FIXME, HACK, "not implemented", "not yet" in
                      subsystem source, with the line
    promised          a feature named in a Resources markdown doc that has no
                      matching command, op or symbol. Match on backticked
                      identifiers and BF6.* names in the docs.
    orphan_resource   a file under Resources referenced by no source file
    boundary          an add-on source that does not read the game, or a base
                      source that does. Flag as a question, not a verdict.

Output OUT/gaps.json and a readable printed report ordered by subsystem. Never
invent a finding to fill a category. An empty category prints as empty.

## bf6_context_mcp.py

Follows the existing Content/Python/bf6_water_mcp.py exactly in structure: a
_define_toolset() returning an unreal.uclass with @unreal.ufunction static
methods returning strings. Read that file first and match its idioms, its
comment voice and its error handling. Register a toolset named
BF6ContextToolset.

Tools:

    context_inventory(subsystem="")   inventory.json, or one subsystem
    context_commands(filter="")       command name, help, subsystem
    context_history(pattern, before=40, after=40)    log_index.around
    context_loops(min_repeats=5)      log_index.loops
    context_errors(minutes=60)        log_index.errors
    context_sessions()                log_index.sessions
    context_gaps(subsystem="")        gaps.json
    context_state()                   LIVE. Run every registered Status command
                                      through the console and return the merged
                                      output keyed by subsystem. Take the list
                                      from inventory.json so it stays correct as
                                      commands are added.
    context_trace(subsystem)          everything about one subsystem at once:
                                      inventory entry, its live Status, its last
                                      50 log events, its gaps.

Every ufunction argument must have a default, and every tool returns a json
string. Output can be large. Cap each tool at 60000 characters and say plainly
in the returned json when it truncated, with the count that was dropped. Never
truncate silently.

## House rules that apply to all four

  - No em dashes and no emoji in any string a person reads.
  - Comments explain why a thing is the way it is, not what the line does.
    Where a shape is surprising, say what went wrong that made it necessary.
  - Never write anything under C:/PortalSDK_1.4.2.0. It is read only.
  - Never launch the editor and never run a build.
  - Generated json goes to Saved/BF6Context, never into the plugin tree.
  - Open text files with encoding="utf-8", errors="replace". The logs contain
    bytes that are not valid utf-8, and a crash on one of them makes the tool
    useless exactly when it is needed most.
