"""BF6 tool context - what exists, what happened, what is missing, over MCP.

The SDK is 30 C++ files, about 145 console commands and 18 log categories, so
the first minute of any investigation is spent guessing which subsystem a
symptom belongs to, and a wrong guess costs the whole investigation.  This
module is the lookup that replaces the guess.

The three analysers it reads live in Plugins/BF6UnrealSDK/Tools/context and
know nothing about Unreal on purpose, so they still run with the editor closed.
Only context_state needs the running editor, and it is the reason this file
exists at all: it is the only one of the four that can ask the live tool what
state it is actually in.
"""

from __future__ import annotations

import importlib
import json
import re
import sys
import time
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path

import toolset_registry
import unreal


_TOOLSET_CLASS = None


# Every tool answers in one json string and the transport is not free, so a
# runaway inventory or a 200k-line log slice has to be refused rather than
# shipped.  SPEC.md fixes the number.
_MAX_CHARS = 60000


# Console output in Unreal never comes back to the caller, so the only record
# of what a Status command said is the log file.  Sentinels are written around
# each command because the editor keeps logging on its own ticks; a byte offset
# on its own hands back whatever else the frame happened to print.
_SENTINEL = "BF6ContextStateMarker"


# Log lines carry an optional "[2026.09.07-07.12.47:123][456]" prefix that is
# noise once the line has already been placed between two sentinels.
_LOG_PREFIX_RE = re.compile(
    r"^\[\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3}\](?:\[\s*\d+\])?")


def _caps_root() -> Path:
    """Where the capability snapshots live."""
    return _root() / "Saved" / "BF6UnrealSDK" / "capabilities"


def _caps_observations(source: str):
    """Every recorded scan of one source, newest first.

    THE LOG IS THE HISTORY. THE SNAPSHOT DIRECTORIES ARE ONLY STORAGE.

    A snapshot directory is named after a hash of its contents, so a scan that
    finds content already on disk writes no new directory. Counting directories
    therefore loses that scan entirely, and sorting them by capture time reports
    the first moment that content was ever seen as though it were the latest
    news. Scan A, then B, then A again used to come back as "still B", with B's
    original timestamp. Anything asking what is current, or what changed, reads
    this log.
    """
    base = _caps_root() / "observations" / source
    rows = []
    if base.is_dir():
        for child in base.iterdir():
            # A ".writing" file is an append that did not finish.
            if (not child.is_file() or not child.name.startswith("obs_")
                    or child.suffix != ".json"):
                continue
            try:
                with child.open("r", encoding="utf-8") as handle:
                    obs = json.load(handle)
            except (OSError, ValueError):
                continue
            if not isinstance(obs, dict) or not obs.get("contentId"):
                continue
            if not obs.get("seq"):
                digits = child.stem[4:]
                obs["seq"] = int(digits) if digits.isdigit() else 0
            rows.append(obs)
    rows.sort(key=lambda o: o.get("seq", 0), reverse=True)
    return rows


def _caps_list(source: str):
    """Content ids for one source, newest observation first.

    One id per SCAN, so the same id appears twice when a scan returned to
    content that had been seen before. A store written before the observation
    log existed has directories and no log, and is read in capture order so an
    upgrade does not look like a source with no history.
    """
    observed = _caps_observations(source)
    if observed:
        return [o["contentId"] for o in observed]

    base = _caps_root() / "snapshots" / source
    if not base.is_dir():
        return []
    rows = []
    for child in base.iterdir():
        # A ".writing" directory is a publish that did not finish. Treating one
        # as a snapshot would make a half-collected source a permanent baseline.
        if not child.is_dir() or child.name.endswith(".writing"):
            continue
        snap = _caps_load(source, child.name)
        if snap is None:
            continue
        rows.append((snap.get("capturedUtc", ""), child.name))
    rows.sort(reverse=True)
    return [name for _, name in rows]


def _caps_load(source: str, snapshot_id: str):
    path = _caps_root() / "snapshots" / source / snapshot_id / "snapshot.json"
    try:
        with path.open("r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return None


def _caps_scope_complete(snapshot, scope_name: str) -> bool:
    for scope in snapshot.get("scopes", []):
        if scope.get("name") == scope_name:
            return scope.get("state") == "complete"
    return False


def _caps_diff(old, new):
    """The same comparison the tool makes, so both agree about removals.

    The rule that matters: a key missing from the newer snapshot is only
    "removed" when its scope was collected completely BOTH times. Otherwise it
    is "not observed", which is a gap in the scan rather than a claim about the
    source. Reporting a page that failed to load as a pile of deleted settings
    would be worse than reporting nothing at all.

    Evidence transitions are reported too, in both directions. This used to
    ignore evidence entirely, so a capability that was found in a running game
    and is not found now produced nothing at all here while the C++ side
    reported it: the two disagreed about the single most important thing either
    of them can say.
    """
    # The ladder, weakest to strongest, in the order BF6Capabilities.h declares.
    # "not observed" is deliberately NOT a rung. It is a transition, and giving
    # it a number would make losing a capability compare as the strongest
    # evidence there is, because it sits last in the enum.
    rank = {"observed": 0, "structure": 1, "exposed": 2, "serialized": 3,
            "accepted by the site": 4, "present at runtime": 5,
            "verified in game": 6}

    def index(snapshot):
        return {(r.get("kind", ""), r.get("key", "")): r
                for r in snapshot.get("records", [])}

    old_ix, new_ix = index(old), index(new)
    changes = []
    for ident, rec in new_ix.items():
        was = old_ix.get(ident)
        if was is None:
            changes.append({"what": "added", "kind": rec.get("kind"),
                            "key": rec.get("key"), "shape": rec.get("shape"),
                            "evidence": rec.get("evidence")})
            continue
        if was.get("shape") != rec.get("shape"):
            changes.append({"what": "changed", "kind": rec.get("kind"),
                            "key": rec.get("key"),
                            "was": was.get("shape"), "now": rec.get("shape"),
                            "evidence": rec.get("evidence")})
            continue
        before, after = was.get("evidence", ""), rec.get("evidence", "")
        if before == after:
            continue
        # "not observed" is handled by name, before any ranking, for the reason
        # above. Then, and only then, the two rungs can be ordered.
        if after == "not observed":
            what = "evidence lowered"
            why = (f"it was {before or 'unrecorded'} before and was looked for "
                   f"and not seen this time. The record is kept so the change "
                   f"is visible.")
        elif before == "not observed":
            what = "evidence raised"
            why = "it was recorded as not observed and has been seen again."
        elif before in rank and after in rank:
            what = "evidence raised" if rank[after] > rank[before] else "evidence lowered"
            why = ""
        else:
            # A rung this build does not know, from a snapshot a later collector
            # wrote. The change is real; guessing its direction would not be.
            what = "evidence changed"
            why = "one of these rungs is not one this build knows, so the direction is not claimed."
        changes.append({"what": what, "kind": rec.get("kind"), "key": rec.get("key"),
                        "shape": rec.get("shape"), "was": before, "now": after,
                        "why": why})
    for ident, rec in old_ix.items():
        if ident in new_ix:
            continue
        scope = rec.get("scope", "")
        both_complete = _caps_scope_complete(old, scope) and _caps_scope_complete(new, scope)
        changes.append({
            "what": "removed" if both_complete else "not observed",
            "kind": rec.get("kind"), "key": rec.get("key"),
            "was": rec.get("shape"), "scope": scope,
            "why": (f"{scope} was collected completely both times"
                    if both_complete else
                    f"{scope} was not collected completely, so this is a gap in "
                    f"the scan and not a removal"),
        })
    order = {"added": 0, "changed": 1, "evidence raised": 2, "evidence lowered": 3,
             "evidence changed": 4, "removed": 5, "not observed": 6}
    changes.sort(key=lambda c: (order.get(c["what"], 9), c.get("key", "")))
    return changes


def _root() -> Path:
    """The project root, found by walking up rather than by user name."""
    here = Path(__file__).resolve()
    for candidate in (here, *here.parents):
        if (candidate / "BF6_Unreal_SDK.uproject").exists():
            return candidate
    # Content/Python/<this file>, so two levels up is the root even when the
    # uproject has been renamed out from under us.
    return here.parents[2]


ROOT = _root()
CONTEXT_DIR = ROOT / "Plugins" / "BF6UnrealSDK" / "Tools" / "context"
OUT_DIR = ROOT / "Saved" / "BF6Context"


# The analysers are separate files landing on their own schedule.  Putting their
# directory on sys.path at import time is what lets this module be reloaded in
# a running editor and pick up an analyser that did not exist at startup.
if str(CONTEXT_DIR) not in sys.path:
    sys.path.insert(0, str(CONTEXT_DIR))


_ANALYSER_CACHE = {}


def _analyser(module_name: str):
    """Import one analyser, or explain the failure instead of raising.

    A toolset that refuses to register because one dependency is missing is
    invisible from inside the editor, which is exactly the moment you need it.
    So a broken analyser degrades the one tool that uses it and nothing else,
    and the import is retried on every call so a file that lands later works
    without an editor restart.
    """
    cached = _ANALYSER_CACHE.get(module_name)
    if cached is not None:
        return cached, None
    try:
        module = importlib.import_module(module_name)
    except Exception as exc:
        return None, {
            "error": f"{module_name}.py is unavailable: {type(exc).__name__}: {exc}",
            "module": module_name,
            "expected_path": str(CONTEXT_DIR / f"{module_name}.py"),
            "remedy": (
                f"create or repair {module_name}.py under {CONTEXT_DIR}, then "
                "reload this module; no editor restart is needed"
            ),
        }
    _ANALYSER_CACHE[module_name] = module
    return module, None


def _analyser_call(module_name: str, function_name: str, *args, **kwargs):
    """Call one analyser entry point, returning (result, error_payload)."""
    module, error = _analyser(module_name)
    if module is None:
        return None, error
    function = getattr(module, function_name, None)
    if not callable(function):
        return None, {
            "error": f"{module_name}.{function_name} is missing",
            "module": module_name,
            "expected_signature": f"{function_name}(...) as fixed by SPEC.md",
            "remedy": f"implement {function_name} in {CONTEXT_DIR / (module_name + '.py')}",
        }
    try:
        return function(*args, **kwargs), None
    except Exception as exc:
        return None, {
            "error": (f"{module_name}.{function_name} failed: "
                      f"{type(exc).__name__}: {exc}"),
            "module": module_name,
        }


def _read_generated(name: str, module_name: str):
    """Read one generated json, asking its builder for it when it is absent."""
    path = OUT_DIR / f"{name}.json"
    builder_note = None
    if not path.exists():
        module, error = _analyser(module_name)
        if module is None:
            return None, error
        # SPEC.md fixes the file each analyser writes but not the name of the
        # function that writes it, so try the usual entry points rather than
        # betting the tool on a single guess.  SystemExit is caught with the
        # rest because a command-line style main() will call it.
        for entry in ("build", "rebuild", "generate", f"build_{name}", "main"):
            function = getattr(module, entry, None)
            if not callable(function):
                continue
            try:
                function()
                builder_note = f"{module_name}.{entry}()"
                break
            except (Exception, SystemExit) as exc:
                builder_note = f"{module_name}.{entry}() raised {type(exc).__name__}: {exc}"
        if not path.exists():
            return None, {
                "error": f"{name}.json has not been generated",
                "expected_path": str(path),
                "builder_attempt": builder_note,
                "remedy": f"run: python {CONTEXT_DIR / (module_name + '.py')}",
            }
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
        return json.loads(text), None
    except Exception as exc:
        return None, {
            "error": f"{name}.json is unreadable: {type(exc).__name__}: {exc}",
            "path": str(path),
            "remedy": f"delete it and re-run python {module_name}.py",
        }


def _emit(payload) -> str:
    """Serialise one answer, refusing to drop anything without saying so."""
    text = json.dumps(payload, indent=2, sort_keys=True, default=str)
    if len(text) <= _MAX_CHARS:
        return text
    total = len(text)

    def envelope_for(keep: int) -> str:
        return json.dumps({
            "truncated": True,
            "characters_total": total,
            "characters_returned": keep,
            "characters_dropped": total - keep,
            "note": (
                "The answer exceeded the 60000 character cap. partial_json is "
                "the leading fragment of the real payload and is deliberately "
                "not valid json on its own. Narrow the request with a "
                "subsystem, filter or limit argument."
            ),
            "partial_json": text[:keep],
        }, indent=2, sort_keys=True)

    # The fragment is re-escaped when it goes back inside json and a quote and
    # newline heavy payload grows by an unpredictable amount doing it, so the
    # envelope is measured and then shrunk in proportion to its own overshoot.
    # A fixed step either undershoots by ten thousand characters or loops.
    keep = min(total, _MAX_CHARS)
    for _ in range(16):
        envelope = envelope_for(keep)
        size = len(envelope)
        if size <= _MAX_CHARS:
            return envelope
        keep = int(keep * (_MAX_CHARS - 400) / size)
        if keep <= 0:
            break
    return json.dumps({
        "truncated": True,
        "characters_total": total,
        "characters_returned": 0,
        "characters_dropped": total,
        "note": "The answer could not be summarised inside the 60000 character cap.",
    }, indent=2, sort_keys=True)


def _exec_console(command: str) -> None:
    """Run one editor console command, the same way bf6_water_mcp does."""
    world = unreal.EditorLevelLibrary.get_editor_world()
    unreal.SystemLibrary.execute_console_command(world, str(command))


def _configured_log_path(command_line: str, log_dir: Path):
    """Match UE's LOG / LogFileName / ABSLOG precedence, including spaces."""
    for key in ("LOG", "LogFileName", "ABSLOG"):
        match = re.search(r'(?:^|\s)-?' + key + r'=(?:"([^"]*)"|([^\s]+))',
                          command_line, re.IGNORECASE)
        if match:
            value = match.group(1) if match.group(1) is not None else match.group(2)
            # UE ignores unsupported extensions, rather than trying the next flag.
            if Path(value).suffix not in (".log", ".txt"):
                return None
            return Path(value) if key == "ABSLOG" else log_dir / value
    return None


def _live_log_path():
    """Use the process's configured log; Chromium activity is not editor output."""
    try:
        log_dir = Path(unreal.Paths.convert_relative_path_to_full(
            unreal.Paths.project_log_dir()))
    except Exception:
        return None
    try:
        configured = _configured_log_path(unreal.SystemLibrary.get_command_line(), log_dir)
        if configured is not None:
            # Never report another session's output if the requested log is absent.
            return configured if configured.is_file() else None
        project = Path(unreal.Paths.get_project_file_path()).stem
        normal = log_dir / (project + ".log")
        if project and normal.is_file():
            return normal
    except Exception:
        pass
    logs = []
    for path in log_dir.glob("*.log"):
        name = path.name.lower()
        if "-backup-" in name or name.startswith("cef"):
            continue
        try:
            with path.open("rb") as handle:
                header = handle.read(4096)
            if b"Log file open" in header or b"LogInit:" in header:
                logs.append(path)
        except OSError:
            continue
    if not logs:
        return None
    logs.sort(key=lambda path: path.stat().st_mtime, reverse=True)
    return logs[0]


def _read_since(log_path: Path, offset: int) -> str:
    """Read the log bytes appended since offset.

    Binary, because a text handle cannot seek to a byte offset it did not
    produce itself, and because the logs carry bytes that are not valid utf-8
    and a crash on one of them would break the tool exactly when it is needed.
    """
    with log_path.open("rb") as handle:
        handle.seek(offset)
        return handle.read().decode("utf-8", errors="replace")


def _capture_console(commands: list) -> dict:
    """Run console commands and recover what each one printed.

    Unreal returns nothing from execute_console_command and every BF6 Status
    command answers through UE_LOG, so the live log is the only place the
    answer exists.  The sequence is: sentinel, command, sentinel, FlushLog,
    then read back only the bytes appended since we started.  FlushLog matters
    because the file writer can still be holding the lines we just caused, and
    without it the capture comes back empty for no visible reason.
    """
    commands = [str(command) for command in commands if str(command).strip()]
    results = {command: {
        "command": command,
        "captured": False,
        "exec_error": None,
        "output": [],
    } for command in commands}
    if not commands:
        return {"results": results, "diagnostics": {"reason": "no commands selected"}}

    token = uuid.uuid4().hex[:12]
    closing = f"{_SENTINEL} CLOSE {token}"

    # Log first, then pick the file, so the live log is guaranteed to be the
    # most recently modified one.
    try:
        unreal.log(f"{_SENTINEL} OPEN {token}")
    except Exception:
        pass
    log_path = _live_log_path()
    if log_path is None:
        for row in results.values():
            row["exec_error"] = "no live Unreal log file was found to capture from"
        return {"results": results,
                "diagnostics": {"captured_from": None,
                                "reason": "no live log file"}}
    start = log_path.stat().st_size

    for command in commands:
        try:
            unreal.log(f"{_SENTINEL} BEGIN {token} {command}")
            _exec_console(command)
        except Exception as exc:
            results[command]["exec_error"] = f"{type(exc).__name__}: {exc}"
        try:
            unreal.log(f"{_SENTINEL} END {token} {command}")
        except Exception:
            pass

    attempts = 0
    tail = ""
    for attempts in range(1, 5):
        try:
            _exec_console("FlushLog")
            unreal.log(closing)
            _exec_console("FlushLog")
        except Exception:
            pass
        try:
            tail = _read_since(log_path, start)
        except Exception as exc:
            return {"results": results,
                    "diagnostics": {"captured_from": str(log_path),
                                    "reason": f"log read failed: {exc}"}}
        if closing in tail:
            break
        # The write is asynchronous on the log thread; a short wait is cheaper
        # than reporting an empty capture.
        time.sleep(0.25)

    current = None
    for raw_line in tail.splitlines():
        if _SENTINEL in raw_line and token in raw_line:
            body = raw_line.split(_SENTINEL, 1)[1].strip()
            parts = body.split(None, 2)
            kind = parts[0] if parts else ""
            if kind == "BEGIN" and len(parts) >= 3:
                current = parts[2].strip()
                current = current if current in results else None
            else:
                current = None
            continue
        if current is None:
            continue
        line = _LOG_PREFIX_RE.sub("", raw_line).rstrip()
        if not line.strip():
            continue
        results[current]["output"].append(line)
        results[current]["captured"] = True

    return {
        "results": results,
        "diagnostics": {
            "mechanism": (
                "sentinel-bracketed tail read of the live editor log, flushed "
                "with the FlushLog exec; Unreal does not return console output "
                "to the caller"
            ),
            "captured_from": str(log_path),
            "bytes_read": len(tail.encode("utf-8", errors="replace")),
            "read_attempts": attempts,
            "closing_sentinel_seen": closing in tail,
        },
    }


def _subsystem_from_command(name: str) -> str:
    """Derive a subsystem id from a command name when inventory is unavailable.

    inventory.json is the real owner map. This is only used by the console
    fallback, and the answer is labelled as derived so nobody reads it as fact.
    """
    parts = [part for part in str(name).split(".") if part]
    if parts and parts[0].upper() == "BF6":
        parts = parts[1:]
    if parts and parts[-1].endswith("Status"):
        parts = parts[:-1] or [str(name).split(".")[-1]]
    return "".join(parts).lower() or "unknown"


def _status_commands(inventory) -> tuple:
    """Every registered Status command, and where the list came from.

    SPEC.md says take the list from inventory.json so it stays correct as
    commands are added.  The match is "ends with Status" rather than
    "ends with .Status" because BF6.HighPoly.WaterTreeStatus and
    BF6.Blocks.StyleStatus are Status commands that do not carry the dot.
    """
    if isinstance(inventory, dict):
        commands = inventory.get("commands") or {}
        if isinstance(commands, dict) and commands:
            selected = sorted(name for name in commands
                              if str(name).endswith("Status"))
            if selected:
                return selected, "inventory.json", commands
    return [], None, {}


def _status_commands_from_console() -> tuple:
    """Fallback list, asked of the live console through the same capture path.

    Used only when inventory.json is missing.  DumpConsoleCommands prints every
    registered command, which is the one place the truth exists without the
    analysers.
    """
    captured = _capture_console(["DumpConsoleCommands"])
    lines = captured["results"].get("DumpConsoleCommands", {}).get("output", [])
    found = set()
    for line in lines:
        for match in re.findall(r"\bBF6\.[A-Za-z0-9_.]*Status\b", line):
            found.add(match)
    return sorted(found), captured["diagnostics"]


def _context_state() -> dict:
    """Run every registered Status command and merge the output by subsystem."""
    inventory, inventory_error = _read_generated("inventory", "inventory")
    commands, source, owners = _status_commands(inventory)
    fallback_diagnostics = None
    if not commands:
        commands, fallback_diagnostics = _status_commands_from_console()
        source = "live console DumpConsoleCommands (inventory.json unavailable)"
        owners = {}

    if not commands:
        return {
            "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "command_source": source,
            "commands_run": [],
            "subsystems": {},
            "inventory_error": inventory_error,
            "console_fallback_diagnostics": fallback_diagnostics,
            "error": "no Status commands could be found to run",
        }

    captured = _capture_console(commands)
    merged = {}
    uncaptured = []
    for command in commands:
        row = captured["results"].get(command, {})
        owner = owners.get(command) if isinstance(owners, dict) else None
        subsystem = owner or _subsystem_from_command(command)
        entry = merged.setdefault(subsystem, {
            "subsystem": subsystem,
            "subsystem_source": "inventory.json" if owner else "derived from command name",
            "commands": {},
        })
        entry["commands"][command] = {
            "captured": bool(row.get("captured")),
            "exec_error": row.get("exec_error"),
            "output": row.get("output", []),
        }
        if not row.get("captured"):
            uncaptured.append(command)

    return {
        "generated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "command_source": source,
        "commands_run": commands,
        "command_count": len(commands),
        "captured_count": len(commands) - len(uncaptured),
        "uncaptured_commands": uncaptured,
        "subsystems": merged,
        "capture": captured["diagnostics"],
        "inventory_error": inventory_error,
        "console_fallback_diagnostics": fallback_diagnostics,
    }


def _gaps_for(gaps, subsystem: str) -> dict:
    """Filter a gaps report to one subsystem without assuming its exact shape.

    gaps.py owns that shape; all this needs is that a finding carries a
    "subsystem" field, which SPEC.md requires of every finding.
    """
    needle = str(subsystem).strip().lower()
    if not needle or not isinstance(gaps, dict):
        return gaps if isinstance(gaps, dict) else {"gaps": gaps}
    filtered = {}
    for key, value in gaps.items():
        if isinstance(value, list):
            rows = [row for row in value
                    if isinstance(row, dict) and
                    str(row.get("subsystem", "")).lower() == needle]
            filtered[key] = rows
        elif isinstance(value, dict) and needle in {str(k).lower() for k in value}:
            filtered[key] = {k: v for k, v in value.items()
                             if str(k).lower() == needle}
        else:
            filtered[key] = value
    filtered["filtered_to_subsystem"] = needle
    return filtered


def _trace(subsystem: str) -> dict:
    """Everything about one subsystem: what it is, how it is, what it lacks."""
    needle = str(subsystem).strip().lower()
    if not needle:
        return {"error": "subsystem is required, for example blocks or highpolywatersurface"}

    result = {"subsystem": needle}

    inventory, inventory_error = _read_generated("inventory", "inventory")
    entry = None
    if isinstance(inventory, dict):
        subsystems = inventory.get("subsystems") or {}
        entry = subsystems.get(needle)
        if entry is None:
            result["known_subsystems"] = sorted(subsystems)
    result["inventory"] = entry
    result["inventory_error"] = inventory_error

    status_commands = []
    if isinstance(entry, dict):
        for command in entry.get("commands") or []:
            name = command.get("name") if isinstance(command, dict) else str(command)
            if name and str(name).endswith("Status"):
                status_commands.append(str(name))
    if status_commands:
        captured = _capture_console(sorted(set(status_commands)))
        result["state"] = {name: {
            "captured": bool(row.get("captured")),
            "exec_error": row.get("exec_error"),
            "output": row.get("output", []),
        } for name, row in captured["results"].items()}
        result["state_capture"] = captured["diagnostics"]
    else:
        result["state"] = {}
        result["state_note"] = "this subsystem registers no Status command"

    events = []
    event_errors = []
    categories = (entry.get("log_categories") if isinstance(entry, dict) else None) or []

    # THE NEWEST SESSION, NOT THE FIRST FIFTY LINES EVER LOGGED.
    #
    # events() takes its limit from the FRONT of the scan, and the scan walks
    # every log in Saved/Logs, so limit=50 returned fifty lines from the oldest
    # backup and the [-50:] below then sorted rubbish. Asked for a subsystem's
    # recent history straight after a build, this answered with events from the
    # previous day and looked entirely plausible doing it. Narrow to the live
    # session first, take a generous slice of it, and keep the tail.
    newest_session = None
    sessions, sessions_error = _analyser_call("log_index", "sessions")
    if sessions_error:
        event_errors.append(sessions_error)
    elif sessions:
        last = sessions[-1]
        newest_session = last.get("file") if isinstance(last, dict) else None

    for category in categories:
        kwargs = {"category": str(category), "limit": 2000}
        if newest_session:
            kwargs["session"] = newest_session
        rows, error = _analyser_call("log_index", "events", **kwargs)
        if error:
            event_errors.append(error)
        elif rows:
            events.extend(rows)
    events.sort(key=lambda row: str(row.get("time") or "") if isinstance(row, dict) else "")
    result["log_categories"] = list(categories)
    result["recent_events"] = events[-50:]
    result["recent_events_errors"] = event_errors or None

    gaps, gaps_error = _read_generated("gaps", "gaps")
    result["gaps"] = _gaps_for(gaps, needle) if gaps is not None else None
    result["gaps_error"] = gaps_error
    return result


def _define_toolset():
    global _TOOLSET_CLASS
    if _TOOLSET_CLASS is not None:
        return _TOOLSET_CLASS

    @unreal.uclass()
    class BF6ContextToolset(unreal.ToolsetDefinition):
        """Look up what the BF6 SDK contains, what it logged, and how it is now."""

        @toolset_registry.tool_call
        @staticmethod
        def context_inventory(subsystem: str = "") -> str:
            """Return inventory.json whole, or one subsystem's entry from it."""
            inventory, error = _read_generated("inventory", "inventory")
            if error:
                return _emit(error)
            needle = str(subsystem).strip().lower()
            if not needle:
                return _emit(inventory)
            subsystems = inventory.get("subsystems") or {}
            entry = subsystems.get(needle)
            if entry is None:
                return _emit({
                    "error": f"no subsystem named {needle!r}",
                    "known_subsystems": sorted(subsystems),
                })
            return _emit({"subsystem": needle, "entry": entry})

        @toolset_registry.tool_call
        @staticmethod
        def context_commands(filter: str = "") -> str:
            """List console commands with their help string and owning subsystem."""
            inventory, error = _read_generated("inventory", "inventory")
            if error:
                return _emit(error)
            needle = str(filter).strip().lower()
            owners = inventory.get("commands") or {}
            rows = []
            for name, subsystem in sorted(owners.items()):
                rows.append({"name": name, "subsystem": subsystem, "help": "",
                             "file": None, "line": None})
            # The owner map has no help text, so the per-subsystem command rows
            # are folded back over it; they are the ones carrying file and line.
            detail = {}
            for subsystem, entry in (inventory.get("subsystems") or {}).items():
                for command in (entry or {}).get("commands") or []:
                    if isinstance(command, dict) and command.get("name"):
                        detail[command["name"]] = (subsystem, command)
            for row in rows:
                found = detail.get(row["name"])
                if not found:
                    continue
                subsystem, command = found
                row["subsystem"] = row["subsystem"] or subsystem
                row["help"] = command.get("help") or ""
                row["file"] = command.get("file")
                row["line"] = command.get("line")
            for name, (subsystem, command) in sorted(detail.items()):
                if name not in owners:
                    rows.append({"name": name, "subsystem": subsystem,
                                 "help": command.get("help") or "",
                                 "file": command.get("file"),
                                 "line": command.get("line")})
            if needle:
                rows = [row for row in rows
                        if needle in row["name"].lower()
                        or needle in str(row["help"]).lower()
                        or needle in str(row["subsystem"]).lower()]
            return _emit({
                "filter": str(filter),
                "command_count": len(rows),
                "commands": sorted(rows, key=lambda row: row["name"]),
            })

        @toolset_registry.tool_call
        @staticmethod
        def context_history(pattern: str = "", before: int = 40,
                            after: int = 40) -> str:
            """Show the log events surrounding a match: what led up to it."""
            if not str(pattern).strip():
                return _emit({"error": "pattern is required, for example 'Portal resume'"})
            rows, error = _analyser_call("log_index", "around", str(pattern),
                                         before=int(before), after=int(after))
            if error:
                return _emit(error)
            return _emit({
                "pattern": str(pattern),
                "before": int(before),
                "after": int(after),
                "events": rows,
            })

        @toolset_registry.tool_call
        @staticmethod
        def context_loops(min_repeats: int = 5) -> str:
            """Find near identical log messages that repeat, with their period."""
            rows, error = _analyser_call("log_index", "loops",
                                         min_repeats=int(min_repeats))
            if error:
                return _emit(error)
            return _emit({"min_repeats": int(min_repeats), "loops": rows})

        @toolset_registry.tool_call
        @staticmethod
        def context_errors(minutes: int = 60) -> str:
            """Group the errors and warnings logged in the last N minutes."""
            # Unreal stamps its log in UTC, and the index compares the cutoff
            # against those stamps directly. Building the cutoff from the local
            # clock therefore compared two different timezones: on a UTC-5
            # machine "the last 20 minutes" quietly became the last five hours
            # and twenty minutes, so failures from a session that ended before
            # lunch were reported as current. The cutoff is built in UTC to
            # match the log, and the window is reported in full so a wrong
            # answer is visible rather than plausible.
            minutes = max(1, int(minutes))
            as_of = datetime.now(timezone.utc)
            since = (as_of - timedelta(minutes=minutes)).replace(
                tzinfo=None).isoformat(timespec="seconds")
            rows, error = _analyser_call("log_index", "errors", since=since)
            if error:
                return _emit(error)
            return _emit({
                "minutes": minutes,
                "since_utc": since,
                "as_of_utc": as_of.replace(tzinfo=None).isoformat(timespec="seconds"),
                "clock": "log timestamps and this window are both UTC",
                "errors": rows,
            })

        @toolset_registry.tool_call
        @staticmethod
        def context_sessions() -> str:
            """List the editor sessions held in Saved/Logs, newest information first."""
            rows, error = _analyser_call("log_index", "sessions")
            if error:
                return _emit(error)
            return _emit({"sessions": rows})

        @toolset_registry.tool_call
        @staticmethod
        def context_gaps(subsystem: str = "") -> str:
            """Report what is unfinished or unwired, whole or for one subsystem."""
            gaps, error = _read_generated("gaps", "gaps")
            if error:
                return _emit(error)
            return _emit(_gaps_for(gaps, subsystem))

        @toolset_registry.tool_call
        @staticmethod
        def context_state() -> str:
            """Run every registered Status command live and merge it by subsystem."""
            return _emit(_context_state())

        @toolset_registry.tool_call
        @staticmethod
        def context_trace(subsystem: str = "") -> str:
            """One subsystem at once: inventory, live status, last events, gaps."""
            return _emit(_trace(subsystem))

        @toolset_registry.tool_call
        @staticmethod
        def context_selftest(area: str = "", collect: bool = False) -> str:
            """Audit the tool before a release: open each page, run its self test, collect every failure.

            Answers one of four states for THIS run: failed, incomplete,
            nothing-to-run or clean, with the pass, fail and skip counts it
            decided on. No results is INCOMPLETE and never clean; call it again.
            """
            # WHY A TOOL AND NOT A CHECKLIST.
            #
            # The faults this project ships are not logic errors. They are a
            # panel drawn over the control it was meant to sit beside, a button
            # wired to nothing, a module loaded but never installed, a guard
            # left switched off by a bulk load. None of them throw, none of
            # them fail a compile, and every one of them was found by the user
            # rather than by the tool.
            #
            # So the pages audit themselves and this collects the verdicts. It
            # opens what it needs, because a page that is closed reports
            # nothing and "nothing" must never read as "fine".
            #
            # THREE WAYS THIS TOOL LIED, AND WHAT NOW STOPS EACH ONE.
            #
            # 1. It answered "clean (DONE 44 passed, 1 failed)". The completion
            #    line was treated as a label to quote, and the verdict was
            #    decided only by whether the host had collected FAIL lines of
            #    its own. It had not: the old scan walked the log backwards and
            #    stopped at DONE, so the FAIL line printed just before it was
            #    never read. The two numbers in that line are now parsed, and a
            #    non-zero failed count is a failure on its own evidence no
            #    matter what else was or was not collected.
            #
            # 2. It accepted an older run's DONE line. The scan walked back
            #    200k of log with nothing tying a result to the request that
            #    was supposed to produce it, so a green run from an hour ago
            #    answered the question asked now. Every run now has an id and a
            #    window: the log position this call started at, and the BEGIN
            #    sentinel _capture_console writes immediately before it
            #    dispatches the SelfTest command. A line the page produced for
            #    this run can only come after that sentinel, because the page
            #    cannot answer a message that has not been sent yet. Anything
            #    earlier is listed as ignored evidence, never used.
            #
            # 3. It called a run that produced nothing a pass, and it counted a
            #    skipped check as a passed one, because the page increments its
            #    own pass counter for a skip. Pass, fail, skip and incomplete
            #    are four separate states here, and all four counts are printed
            #    beside the verdict so a wrong verdict is visible instead of
            #    plausible.
            #
            # The page's lines carry no run id and this file cannot give them
            # one: that format belongs to editor_ui.js. Position after this
            # run's dispatch sentinel is therefore the binding, and the single
            # case it cannot separate - a previous invocation's reply still in
            # flight when this one dispatches - is stated in binding.limitation
            # rather than quietly assumed away.
            #
            # The imports are local and every editor call is optional so this
            # entry point can be driven outside Unreal with only _emit,
            # _capture_console and _live_log_path stubbed. The probe that found
            # the false clean verdict does exactly that, and a verdict rule
            # that cannot be tested offline is how the false pass survived.
            import re as _re
            import uuid as _uuid

            wanted = str(area).strip().lower()
            plan = []
            if wanted in ("", "all", "blocks"):
                plan.append(("blocks", "BF6.Blocks.Open", "BF6.Blocks.SelfTest",
                             "BF6.Blocks.Status"))
            if wanted in ("", "all", "script"):
                plan.append(("script", "BF6.Script.Open", None, "BF6.Script.Status"))
            if wanted in ("", "all", "ui", "uibuilder"):
                plan.append(("uibuilder", "BF6.UI.Open", None, "BF6.UI.Status"))
            if not plan:
                return _emit({"error": f"no area named {area!r}",
                              "known": ["blocks", "script", "ui", "all"]})

            run_id = _uuid.uuid4().hex[:12]
            test_cmds = [test for _n, _o, test, _s in plan if test]

            # Log lines carry an optional "[2026.09.07-07.12.47:123][456]"
            # prefix that has to come off before a line is matched.
            strip_prefix = _re.compile(
                r"^\[\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3}\](?:\[\s*\d+\])?")
            # editor_ui.js ends a run with "selftest: DONE n passed, m failed".
            # Both numbers are captured: quoting the sentence and ignoring the
            # numbers in it is the exact shape of the false clean verdict.
            # The trailing skipped count is optional so a page from before it
            # existed still parses. A run reporting no skip count is not the
            # same as one reporting zero, and the verdict says which it saw.
            done_re = _re.compile(
                r"selftest:\s*DONE\s+(\d+)\s+passed,\s*(\d+)\s+failed"
                r"(?:,\s*(\d+)\s+skipped)?", _re.I)

            def scan(lines):
                """Sort one set of lines into pass, fail, skip, note and completion."""
                out = {"passed": [], "failed": [], "skipped": [], "notes": [],
                       "done_line": None, "reported_passed": None,
                       "reported_failed": None, "reported_skipped": None,
                       "completions": 0}
                for raw in lines:
                    line = str(raw)
                    if "selftest:" not in line:
                        continue
                    body = line.split("selftest:", 1)[1].strip()
                    hit = done_re.search(line)
                    if hit:
                        out["completions"] += 1
                        out["done_line"] = body
                        out["reported_passed"] = int(hit.group(1))
                        out["reported_failed"] = int(hit.group(2))
                        out["reported_skipped"] = (
                            int(hit.group(3)) if hit.group(3) is not None else None)
                        continue
                    upper = body.upper()
                    if upper.startswith("FAIL"):
                        out["failed"].append(body)
                    elif upper.startswith("SKIP"):
                        # The page says SKIP outright now. It used to call ok()
                        # with the reason buried in the name, so the heuristic
                        # below had to guess; that stays for a page older than
                        # this change, but a real SKIP line needs no guessing.
                        out["skipped"].append(body)
                    elif upper.startswith("PASS"):
                        # A SKIP IS NOT A PASS.
                        #
                        # An older page has no skip counter: a check that could
                        # not run calls ok() with the reason in its name, so a
                        # missing TypeScript compiler and an empty workspace
                        # both arrive as passes and inflate the total. They are
                        # separated here so a run that checked nothing cannot
                        # read as a run that checked everything.
                        if "SKIPPED:" in body or "nothing loaded:" in body:
                            out["skipped"].append(body)
                        else:
                            out["passed"].append(body)
                    else:
                        out["notes"].append(body)
                return out

            # WHERE THIS RUN'S EVIDENCE HAS TO START.
            #
            # Taken before a single command is dispatched, so nothing already
            # in the log can be mistaken for an answer to this call.
            log_path = _live_log_path()
            anchor_offset = None
            window_error = None
            if log_path is None:
                window_error = "there is no live editor log to read this run's results from"
            else:
                try:
                    anchor_offset = log_path.stat().st_size
                except Exception as exc:
                    window_error = (f"this run's start position in the log could not be "
                                    f"taken: {type(exc).__name__}: {exc}")

            # COLLECTING A RUN THAT WAS DISPATCHED EARLIER.
            #
            # The page answers asynchronously, and this tool runs on the game
            # thread, so it cannot wait for the answer: sleeping here freezes
            # the editor, which is the thing this whole module exists to avoid.
            # Dispatch and collection are therefore two calls. Verified live:
            # the page finished "DONE 12 passed" well after the dispatching call
            # had already returned INCOMPLETE, so a tool that can only dispatch
            # can never observe a pass and is useless as a release gate.
            #
            # The dispatching call records where in the log this run began.
            # collect=True re-reads from that mark instead of dispatching again,
            # which is what makes the binding rule usable rather than merely
            # correct: a second dispatch would open a new window and miss the
            # first run's answer for ever.
            # OUT_DIR is a module global. The offline suite lifts this function out
            # on its own, so it is read defensively: without it the run simply is
            # not persisted and cannot be collected, which is the honest
            # degradation rather than a crash in a test harness.
            _out_dir = globals().get("OUT_DIR")
            state_file = (_out_dir / "selftest_run.json") if _out_dir is not None else None
            if collect:
                try:
                    if state_file is None: raise OSError("no state directory")
                    with state_file.open("r", encoding="utf-8") as handle:
                        prior = json.load(handle)
                except (OSError, ValueError):
                    return _emit({
                        "verdict": "INCOMPLETE (NOTHING TO COLLECT): no self test has been "
                                   "dispatched from here yet. Run it without collect first.",
                        "run_id": None,
                    })
                run_id = prior.get("run_id") or run_id
                anchor_offset = prior.get("anchor_offset")
                # Collect the areas the dispatching call actually ran, not
                # whatever this call happened to ask for.
                prior_areas = prior.get("areas") or []
                if prior_areas:
                    plan = [p for p in plan if p[0] in prior_areas] or plan
                test_cmds = [test for _n, _o, test, _s in plan if test]

            # Best effort, for a human correlating a verdict with the log
            # later. The verdict never depends on this line arriving, which is
            # why the failure is swallowed.
            if not collect:
                try:
                    unreal.log(f"BF6ContextSelfTestRun {run_id} START")
                except Exception:
                    pass

            # Recorded before dispatch, so a collecting call can find exactly
            # where this run's evidence begins. Written even if the run then
            # fails: knowing which run was last attempted is what makes a later
            # collection meaningful.
            if not collect and anchor_offset is not None and state_file is not None:
                try:
                    state_file.parent.mkdir(parents=True, exist_ok=True)
                    with state_file.open("w", encoding="utf-8") as handle:
                        json.dump({"run_id": run_id, "anchor_offset": anchor_offset,
                                   "areas": [p[0] for p in plan],
                                   "dispatchedUtc": datetime.now(timezone.utc).isoformat(
                                       timespec="seconds")}, handle)
                except OSError:
                    pass

            report = {"run_id": run_id, "areas": {}, "area_status": {},
                      "capture_complete": {}, "failures": [], "opened": []}
            for name, open_cmd, test_cmd, status_cmd in plan:
                # Opening and testing are separate calls to the console, but the
                # page needs a moment between them. There is no sleep here on
                # purpose: sleeping runs on the game thread and freezes the
                # editor. The open is issued now and the test is issued in the
                # same batch, which is enough because the page queues messages
                # it receives before it is ready.
                cmds = [open_cmd] + ([test_cmd] if test_cmd else []) + [status_cmd]
                # A collecting call dispatches nothing: it is reading the answer
                # to a run that already went out. Re-issuing the commands here
                # would start a fresh run and move the window past the results
                # it was asked to fetch.
                captured = {"results": {}, "diagnostics": {}} if collect else _capture_console(cmds)
                if not collect:
                    report["opened"].append(open_cmd)
                rows = captured.get("results") or {}
                diagnostics = captured.get("diagnostics") or {}
                report["capture_complete"][name] = bool(
                    diagnostics.get("closing_sentinel_seen"))
                area_lines = []
                for c in cmds:
                    row = rows.get(c) or {}
                    area_lines.extend(row.get("output") or [])
                report["areas"][name] = area_lines
                for line in area_lines:
                    # Host-side signals only. The page's own selftest lines are
                    # read once, from this run's log window, so that a line can
                    # neither be counted twice nor counted without being tied
                    # to this run.
                    if "selftest:" in line:
                        continue
                    if "last error" in line and "none" not in line:
                        report["failures"].append(f"{name}: {line.strip()}")
                    elif "page error" in line or "reportFault" in line:
                        report["failures"].append(f"{name}: {line.strip()}")
            host_problems = len(report["failures"])

            # THE PAGE ANSWERS AFTER THE CAPTURE WINDOW HAS CLOSED.
            #
            # _capture_console reads only the bytes between its own sentinels,
            # which is right for a console command that logs synchronously and
            # wrong for this one: the command posts a message to the browser and
            # returns, and the page reports when it is ready, which is normally
            # after the closing sentinel. In the run that started this fix the
            # closing sentinel was at 23:58:35.582 and the results landed at
            # .641. So the log is read again here, from this run's anchor.
            #
            # The editor still cannot be made to wait: this runs on the game
            # thread and sleeping on it freezes the editor. A first call may
            # legitimately find nothing and a second call a moment later will
            # find it. That is INCOMPLETE, and INCOMPLETE is not clean.
            window_text = None
            window_truncated = False
            if log_path is not None and anchor_offset is not None:
                try:
                    size = log_path.stat().st_size
                    if size < anchor_offset:
                        window_error = ("the log was rotated or truncated during this run, "
                                        "so this run's window no longer exists and no result "
                                        "can be proved to belong to it")
                    else:
                        start = anchor_offset
                        # A page that logs megabytes cannot be allowed to blow
                        # the tool up; the cut is recorded because it can drop
                        # this run's earlier FAIL lines.
                        if size - start > 2_000_000:
                            start = size - 2_000_000
                            window_truncated = True
                        with log_path.open("rb") as handle:
                            handle.seek(start)
                            window_text = handle.read().decode("utf-8", errors="replace")
                except Exception as exc:
                    window_error = (f"this run's log window could not be read: "
                                    f"{type(exc).__name__}: {exc}")

            # WHAT WAS IN THE LOG BEFORE THIS RUN STARTED, FOR THE REPORT ONLY.
            #
            # This is the evidence the old code used and must never use again.
            # It is read so the answer can say "the newest completion in the log
            # is older than this run and was not used", because a bare
            # INCOMPLETE next to a log full of green DONE lines looks like the
            # tool is broken, and the difference between the two is the whole
            # point of the fix.
            pre_run_text = ""
            if log_path is not None and anchor_offset:
                try:
                    with log_path.open("rb") as handle:
                        handle.seek(max(0, anchor_offset - 200_000))
                        pre_run_text = handle.read(
                            min(anchor_offset, 200_000)).decode("utf-8", errors="replace")
                except Exception:
                    pre_run_text = ""

            # Everything before this run dispatched its self test belongs to an
            # earlier run by definition. Splitting on the BEGIN sentinel rather
            # than on time is what makes an old green DONE unusable here.
            before_dispatch = [strip_prefix.sub("", raw).rstrip()
                               for raw in pre_run_text.splitlines()]
            after_dispatch = []
            dispatched = False
            for raw_line in (window_text or "").splitlines():
                line = strip_prefix.sub("", raw_line).rstrip()
                # Both halves are required, and neither is anchored to the end
                # of the line, because the sentinel is written through
                # unreal.log and the log writer owns what it puts around it.
                # An exact-shape match here would silently classify every
                # result as somebody else's and report a working run as
                # incomplete forever.
                if not dispatched and " BEGIN " in line and any(
                        cmd in line for cmd in test_cmds):
                    dispatched = True
                    continue
                (after_dispatch if dispatched else before_dispatch).append(line)

            if window_truncated and not dispatched and not window_error:
                # The cut removed the sentinel this run binds to, so the verdict
                # has to say that rather than blame the page for not answering.
                window_error = ("this run logged more than 2 MB, so the start of its own "
                                "window was cut off and its results can no longer be "
                                "distinguished from an earlier run's")

            scanned = scan(after_dispatch)
            ignored = scan(before_dispatch)

            failed_checks = list(scanned["failed"])
            report["failures"].extend(failed_checks)
            reported_passed = scanned["reported_passed"]
            reported_failed = scanned["reported_failed"]
            # None when the page is older than the separate skip count.
            reported_skipped = scanned.get("reported_skipped")
            counts = {
                "checks_passed": len(scanned["passed"]),
                "checks_failed": len(failed_checks),
                "checks_skipped": len(scanned["skipped"]),
                "host_reported_problems": host_problems,
                "page_reported_passed": reported_passed,
                "page_reported_failed": reported_failed,
                "completion_lines_seen": scanned["completions"],
                "areas_with_a_self_test": [n for n, _o, t, _s in plan if t],
                "areas_without_a_self_test": [n for n, _o, t, _s in plan if not t],
            }
            report["counts"] = counts
            report["checks"] = {"passed": scanned["passed"],
                                "failed": failed_checks,
                                "skipped": scanned["skipped"],
                                "notes": scanned["notes"]}
            report["completed"] = scanned["done_line"]
            report["binding"] = {
                "run_id": run_id,
                "log": str(log_path) if log_path is not None else None,
                "started_at_byte": anchor_offset,
                "self_test_dispatch_seen": dispatched,
                "window_truncated": window_truncated,
                "window_error": window_error,
                "rule": ("a result counts only if it appears after this run's own BEGIN "
                         "sentinel for the SelfTest command"),
                "limitation": ("the page's lines carry no run id, so a reply from an earlier "
                               "invocation that was still in flight when this one dispatched "
                               "cannot be told apart from this one's"),
            }
            if ignored["done_line"] or ignored["failed"] or ignored["passed"]:
                report["ignored_evidence"] = {
                    "why": ("these selftest lines are older than this run's dispatch, so they "
                            "belong to an earlier run and were not used for this verdict"),
                    "completed": ignored["done_line"],
                    "failed": ignored["failed"],
                    "counts": {"passed": len(ignored["passed"]),
                               "failed": len(ignored["failed"]),
                               "skipped": len(ignored["skipped"])},
                }
            if reported_failed is not None and reported_failed != len(failed_checks):
                # The two numbers disagreeing is itself a finding: it means FAIL
                # lines were lost, or lines from another page were read in. The
                # larger number wins, because the cost of an unnecessary rerun
                # is a minute and the cost of a false pass is a bad release.
                report["count_disagreement"] = (
                    f"the page reported {reported_failed} failed but {len(failed_checks)} "
                    "FAIL line(s) were read back from this run's window; the larger of the "
                    "two decides the verdict")
            if len(test_cmds) > 1:
                report["attribution_warning"] = (
                    "more than one area has a self test command, and the page's lines do not "
                    "say which page wrote them, so the counts below are for the run as a "
                    "whole and not per area")

            for name, _open_cmd, test_cmd, _status_cmd in plan:
                if not test_cmd:
                    report["area_status"][name] = (
                        "skipped: this area has no self test command, so Open and Status ran "
                        "and nothing in it was checked")
                elif scanned["done_line"] is None:
                    report["area_status"][name] = (
                        "incomplete: no completion line arrived inside this run's window")
                elif failed_checks or (reported_failed or 0) > 0:
                    report["area_status"][name] = (
                        f"failed: {max(len(failed_checks), reported_failed or 0)} check(s) failed")
                else:
                    report["area_status"][name] = (
                        f"passed: {len(scanned['passed'])} checks, "
                        f"{len(scanned['skipped'])} skipped")

            if reported_passed is None:
                page_part = "the page reported no completion line"
            else:
                page_part = (f"the page reported {reported_passed} passed and "
                             f"{reported_failed} failed")
                # Whether the page's own pass count INCLUDES the skips depends on
                # how old the page is: it used to call ok() for a skipped check
                # and now reports them separately. Saying which of the two this
                # was matters, because "8 passed" means different things in each
                # and the difference is exactly what the false clean verdict was
                # built on.
                if reported_skipped is not None:
                    page_part += f" and {reported_skipped} skipped, counted separately"
                elif counts["checks_skipped"]:
                    page_part += (f", {counts['checks_skipped']} of those passes being "
                                  "checks that were skipped")
            summary = (f"{counts['checks_passed']} passed, {counts['checks_failed']} failed, "
                       f"{counts['checks_skipped']} skipped, {host_problems} host problem(s); "
                       f"{page_part}")

            # THE VERDICT IS DECIDED BY THE WORST THING SEEN, NOT THE LAST.
            failure_count = max(len(failed_checks), reported_failed or 0) + host_problems
            if failure_count > 0:
                report["verdict"] = f"FAILED: {failure_count} problem(s). Observed {summary}."
            elif not test_cmds:
                report["verdict"] = (
                    "INCOMPLETE (NOTHING TO RUN): no area in this request has a self test "
                    f"command, so nothing was checked. Observed {summary}.")
            elif scanned["done_line"] is None:
                report["verdict"] = (
                    "INCOMPLETE (NO RESULT): no self test completion arrived inside the "
                    f"window of run {run_id}. Observed {summary}. "
                    + (window_error + ". " if window_error else "")
                    + "The page may still be running, or the panel may not be open. Run it "
                      "again rather than reading this as a pass.")
            elif counts["checks_passed"] == 0:
                report["verdict"] = (
                    "INCOMPLETE: the run completed but nothing was actually checked. "
                    f"Observed {summary}.")
            else:
                report["verdict"] = f"clean: {summary}."
            return _emit(report)

        @toolset_registry.tool_call
        @staticmethod
        def context_maptest(experience_file: str = "", purge: bool = False) -> str:
            """Import a Portal experience export from disk and report what the tool did with it."""
            # THE IMPORT PATH, END TO END, WITH NO SITE INVOLVED.
            #
            # BF6.Portal.ImportFile reads an export off disk and needs no
            # network, which matters twice: it can run unattended, and it
            # cannot write to anybody's live experience. The standing rule is
            # that an existing experience is never modified, only duplicated,
            # and a local file import touches nothing on the site at all.
            #
            # Purging first is deliberate. An import over an existing save
            # tests a different thing from a first import, and the bug worth
            # catching - where the tool lands on the map screen instead of the
            # experience you just imported - only shows on a clean run.
            import shutil

            path = str(experience_file).strip().strip('"')
            if not path:
                return _emit({"error": "give the path to an experience export .json"})
            if not Path(path).is_file():
                return _emit({"error": f"no file at {path}"})

            out = {"file": path, "purged": [], "steps": {}}

            # DELETING SAVES IS NOT A DEFAULT.
            #
            # This defaulted to true and matched on the first twelve characters
            # of a flattened name, so importing "night_ops_breakthrough_copy"
            # would also have matched a save called "Night Ops Breakthrough" -
            # somebody's actual work, removed by a test helper nobody asked to
            # delete anything. It was found by an audit that mocked the
            # deletions; had it run for real it would have taken the saves with
            # it.
            #
            # Now: off unless asked for, exact name match only, and every
            # candidate is listed whether or not it is removed, so the caller
            # can see what the match WOULD have taken.
            saves = _root() / "Saved" / "BF6UnrealSDK" / "saves"
            stem = Path(path).stem
            candidates = []
            if saves.is_dir():
                for child in saves.iterdir():
                    if child.name.lower() == stem.lower():
                        candidates.append(child)
            out["purge_candidates"] = [c.name for c in candidates]
            out["purge_requested"] = bool(purge)
            if purge:
                for child in candidates:
                    try:
                        shutil.rmtree(child) if child.is_dir() else child.unlink()
                        out["purged"].append(child.name)
                    except Exception as exc:
                        out.setdefault("purge_errors", []).append(f"{child.name}: {exc}")
            elif candidates:
                out.setdefault("notes", []).append(
                    f"{len(candidates)} save(s) match this file exactly and were LEFT ALONE. "
                    "Pass purge=true to remove them.")

            # Before, so the after can be compared against something.
            before = _capture_console(["BF6.Project.Status", "BF6.Editors.Status"])
            out["steps"]["before"] = {
                c: (r.get("output") or []) for c, r in (before.get("results") or {}).items()
            }

            after = _capture_console([
                f'BF6.Portal.ImportFile "{path}"',
                "BF6.Project.Status",
                "BF6.Experience.Status",
                "BF6.Editors.Status",
                "BF6.Portal.Status",
            ])
            out["steps"]["after"] = {
                c: (r.get("output") or []) for c, r in (after.get("results") or {}).items()
            }

            # The things worth asserting rather than eyeballing.
            findings = []
            flat_after = [ln for lines in out["steps"]["after"].values() for ln in lines]
            joined = "\n".join(flat_after)

            if "nothing open" in joined and "project" in joined.lower():
                findings.append("after import no project is open: the import did not land, "
                                "or it landed and the tool did not open it")
            # The reported complaint: it jumps to map selection instead of
            # letting you pick a map inside the experience you just imported.
            for line in flat_after:
                if "screen showing" in line and "map selection" in line:
                    findings.append("the tool is on the MAP SELECTION screen after an import; "
                                    "the imported experience's own maps should be selectable "
                                    "without going back to the top")
            # Portal link dropping during menu use.
            for line in flat_after:
                if "linked experience" in line and "no custom map open" in line:
                    findings.append("Portal reports no linked experience after the import")
                if "saved session" in line and "no" in line.split(":")[-1]:
                    findings.append("the Portal session is gone after the import: the link dropped")

            out["findings"] = findings
            out["verdict"] = "clean" if not findings else f"{len(findings)} thing(s) to look at"
            return _emit(out)

        @toolset_registry.tool_call
        @staticmethod
        def context_hitches(min_ms: int = 250, limit: int = 25) -> str:
            """Find stalls and time every progress bar, so slow sections can be attributed rather than guessed at."""
            # WHERE THE TIME ACTUALLY WENT.
            #
            # Every long operation in this tool narrates itself to the log with
            # a millisecond stamp, which means the log is already a profile
            # nobody was reading. Two questions it can answer exactly:
            #
            #   where did the editor stop responding, and what was it doing
            #   how fast is each progress bar, per item, section by section
            #
            # The second matters more than it sounds. "Importing takes eight
            # minutes" is a complaint; "the first fifty items cost 162 seconds
            # and the rest cost fifty seconds per fifty" is a lead, because it
            # says the cost is front loaded and points at what to look at.
            log_path = _live_log_path()
            if log_path is None:
                return _emit({"error": "no live Unreal log to read"})

            stamp = re.compile(r"^\[(\d{4})\.(\d{2})\.(\d{2})-(\d{2})\.(\d{2})\.(\d{2}):(\d{3})\]"
                               r"\[\s*\d+\]\s*(.*)$")

            def when(m):
                # Milliseconds since the day started. Enough for deltas; a run
                # crossing midnight is not worth the complexity.
                return (int(m.group(4)) * 3600000 + int(m.group(5)) * 60000 +
                        int(m.group(6)) * 1000 + int(m.group(7)))

            try:
                text = log_path.read_text(encoding="utf-8", errors="replace")
            except Exception as exc:
                return _emit({"error": f"could not read {log_path}: {exc}"})

            rows = []
            for line in text.splitlines():
                m = stamp.match(line)
                if m:
                    rows.append((when(m), m.group(8)))

            if len(rows) < 2:
                return _emit({"error": "the log has no timestamped lines to measure"})

            floor = max(1, int(min_ms))
            gaps = []
            for i in range(1, len(rows)):
                delta = rows[i][0] - rows[i - 1][0]
                if delta >= floor:
                    gaps.append({
                        "stall_ms": delta,
                        "was_doing": rows[i - 1][1][:160],
                        "resumed_with": rows[i][1][:160],
                    })
            gaps.sort(key=lambda g: -g["stall_ms"])

            # Progress bars: "<n> of <total>" lines, grouped into runs, timed.
            prog = re.compile(r"(\d+) of (\d+)")
            bars, current = [], None
            for at, msg in rows:
                m = prog.search(msg)
                if not m:
                    continue
                done, total = int(m.group(1)), int(m.group(2))
                label = prog.sub("N of T", msg)[:90]
                if (current is None or current["label"] != label or done < current["last_done"]):
                    if current and current["samples"] > 1:
                        bars.append(current)
                    current = {"label": label, "first_at": at, "last_at": at,
                               "first_done": done, "last_done": done,
                               "total": total, "samples": 1, "sections": []}
                else:
                    step_items = done - current["last_done"]
                    step_ms = at - current["last_at"]
                    if step_items > 0:
                        current["sections"].append({
                            "through": done,
                            "items": step_items,
                            "ms": step_ms,
                            "ms_per_item": round(step_ms / step_items, 1),
                        })
                    current["last_at"] = at
                    current["last_done"] = done
                    current["samples"] += 1
            if current and current["samples"] > 1:
                bars.append(current)

            for b in bars:
                span = b["last_at"] - b["first_at"]
                moved = b["last_done"] - b["first_done"]
                b["elapsed_ms"] = span
                b["ms_per_item"] = round(span / moved, 1) if moved else None
                if b["ms_per_item"] and b["total"]:
                    b["projected_total_ms"] = round(b["ms_per_item"] * b["total"])
                # The lead: is the cost even, or front loaded?
                if len(b["sections"]) >= 2:
                    rates = [s["ms_per_item"] for s in b["sections"]]
                    b["slowest_section_ms_per_item"] = max(rates)
                    b["fastest_section_ms_per_item"] = min(rates)
                    if min(rates) > 0 and max(rates) / min(rates) >= 2.0:
                        b["shape"] = ("uneven: the slowest stretch is "
                                      f"{round(max(rates) / min(rates), 1)}x the fastest, "
                                      "so the cost is not per item and something specific is expensive")
                    else:
                        b["shape"] = "even: cost is roughly per item"

            return _emit({
                "read_from": str(log_path),
                "timestamped_lines": len(rows),
                "stall_floor_ms": floor,
                "stalls": gaps[:max(1, int(limit))],
                "stall_count": len(gaps),
                "progress_bars": bars,
            })

        @toolset_registry.tool_call
        @staticmethod
        def context_run(commands: str = "") -> str:
            """Run one or more console commands (semicolon separated) and return what they printed."""
            # WHY THIS EXISTS.
            #
            # Everything above reports.  Nothing above could DO anything, so a
            # change to a page could be built, compiled and shipped into the
            # editor and still never be exercised: the panels were closed and
            # there was no way to open one.  Verification stopped at "it
            # parses", which is where several of this session's real bugs lived.
            #
            # _capture_console already runs a command and recovers its output by
            # bracketing the live log with sentinels, because Unreal returns
            # nothing from execute_console_command.  It was only ever handed the
            # registered Status commands.  This hands it whatever is asked for.
            wanted = [part.strip() for part in str(commands).split(";") if part.strip()]
            if not wanted:
                return _emit({"error": "no command given",
                              "hint": "context_run('BF6.Blocks.Open; BF6.Blocks.Status')"})

            # An editor that exits mid-verification looks exactly like a crash,
            # and the whole point of this tool is to leave the editor running so
            # the next call can read it.  Nothing here needs to close it.
            refused = [c for c in wanted
                       if c.split()[0].lower() in {"quit", "exit", "quit_editor"}]
            if refused:
                return _emit({
                    "error": "refused to run a command that closes the editor",
                    "refused": refused,
                    "why": "this tool exists to inspect a running editor; close it "
                           "deliberately from outside instead",
                })

            captured = _capture_console(wanted)
            return _emit({
                "ran": wanted,
                "results": captured.get("results"),
                "diagnostics": captured.get("diagnostics"),
            })

        @toolset_registry.tool_call
        @staticmethod
        def context_page(which: str = "") -> str:
            """Open one of the embedded pages (blocks, script, ui, portal) and report its status."""
            # Opening and reading are two calls on purpose.  A page takes a
            # moment to load, and the obvious fix - sleep here, then read - runs
            # on the game thread and freezes the editor, which is the exact
            # failure this session already spent hours on.  So this opens and
            # reports immediately; call context_run with the Status command
            # again a moment later to see the loaded page.
            pages = {
                "blocks": ("BF6.Blocks.Open", "BF6.Blocks.Status"),
                "script": ("BF6.Script.Open", "BF6.Script.Status"),
                "ui":     ("BF6.UI.Open", "BF6.UI.Status"),
                "uibuilder": ("BF6.UI.Open", "BF6.UI.Status"),
                # The Portal web panel is deliberately absent. It may appear to
                # sign in, or because the user asked to see the site, and for
                # nothing else - least of all a verification step running by
                # itself. BF6.Portal.Status reports it without showing it.
            }
            key = str(which).strip().lower()
            if key not in pages:
                return _emit({"error": f"no page named {which!r}",
                              "known_pages": sorted(set(pages))})
            open_cmd, status_cmd = pages[key]
            captured = _capture_console([open_cmd, status_cmd])
            return _emit({
                "page": key,
                "opened_with": open_cmd,
                "results": captured.get("results"),
                "note": "the page loads asynchronously; run the Status command "
                        "again in a moment to see it settled",
            })

        # ------------------------------------------------------------------
        # CAPABILITIES: what each source says this build can do.
        #
        # These read the snapshot store on disk rather than driving the editor,
        # so they answer even with no editor running and cannot disturb one that
        # is. Only the scan needs the editor, because collecting is the editor's
        # job.
        # ------------------------------------------------------------------
        @toolset_registry.tool_call
        @staticmethod
        def capabilities_status() -> str:
            """List capability snapshots per source, with the coverage of the newest."""
            out = {}
            for source in ("sdk", "portal", "game", "watchlist"):
                snaps = _caps_list(source)
                if not snaps:
                    out[source] = {"scans": 0,
                                   "note": "nothing collected yet; run capabilities_start_scan"}
                    continue
                observed = _caps_observations(source)
                head = observed[0] if observed else {}
                newest = _caps_load(source, snaps[0]) or {}
                out[source] = {
                    # Scans and stored contents are two different numbers. Five
                    # scans of an unchanged SDK are one stored content and five
                    # scans, and reporting only the one made a repeat scan look
                    # like it never happened.
                    "scans": len(snaps),
                    "distinctContents": len(set(snaps)),
                    "newest": snaps[0],
                    "observation": head.get("seq", 0),
                    "build": newest.get("build", ""),
                    # When this scan happened, not when this content was first
                    # ever seen. Those are the same only until a scan returns to
                    # content that was stored before.
                    "observedUtc": head.get("observedUtc", newest.get("capturedUtc", "")),
                    "contentFirstSeenUtc": newest.get("capturedUtc", ""),
                    "context": head.get("context", newest.get("context", "")),
                    "records": len(newest.get("records", [])),
                    # Coverage is reported per scope and never collapsed to one
                    # verdict: a caller that reads "complete" for a whole source
                    # will draw exactly the wrong conclusion from a missing key.
                    "coverage": [
                        {k: s.get(k) for k in ("name", "state", "why", "items", "cap")}
                        for s in newest.get("scopes", [])
                    ],
                }
            return _emit({"store": str(_caps_root()), "sources": out})

        @toolset_registry.tool_call
        @staticmethod
        def capabilities_start_scan(sources: str = "all") -> str:
            """Collect the capability sources in the editor. 'all', or names separated by commas."""
            wanted = str(sources).strip().lower()
            if wanted in ("", "all"):
                captured = _capture_console(["BF6.Caps.Scan", "BF6.Caps.Status"])
            else:
                # One command covers every source; a subset is a UI choice, not
                # a console one, so this says so rather than pretending.
                captured = _capture_console(["BF6.Caps.Scan", "BF6.Caps.Status"])
            return _emit({
                "ran": ["BF6.Caps.Scan", "BF6.Caps.Status"],
                "results": captured.get("results"),
                "diagnostics": captured.get("diagnostics"),
                "note": "a first scan of a source records a baseline and reports "
                        "no changes; the scan after it is the useful one",
            })

        @toolset_registry.tool_call
        @staticmethod
        def capabilities_diff(source: str = "sdk", limit: int = 80) -> str:
            """Compare the two newest scans of one source and report what moved."""
            src = str(source).strip().lower()
            # Two successive SCANS, not two stored contents. Comparing the two
            # newest directories skips the scan that returned to content already
            # on disk, which is exactly the change worth seeing.
            snaps = _caps_list(src)
            if len(snaps) < 2:
                return _emit({
                    "source": src,
                    "scans": len(snaps),
                    "changes": [],
                    "note": "fewer than two scans, so there is nothing to "
                            "compare. This is not the same as no changes.",
                })
            observed = _caps_observations(src)
            new = _caps_load(src, snaps[0]) or {}
            old = _caps_load(src, snaps[1]) or {}
            # A collector that changed what a shape looks like makes every record
            # compare as changed, and the report would then describe this tool
            # being edited as the source moving. A difference in the instrument
            # is never a finding about the thing being measured.
            old_collector = old.get("collector", "")
            new_collector = new.get("collector", "")
            if old_collector and new_collector and old_collector != new_collector:
                return _emit({
                    "source": src,
                    "changes": [],
                    "from": {"id": snaps[1], "collector": old_collector},
                    "to": {"id": snaps[0], "collector": new_collector},
                    "note": "these two scans were taken by different collectors, "
                            "so their shapes are not comparable and no changes "
                            "are being claimed. Scan again to compare against "
                            "the newer one.",
                })
            changes = _caps_diff(old, new)
            def when(i):
                obs = observed[i] if i < len(observed) else {}
                snap = new if i == 0 else old
                return {"id": snaps[i], "observation": obs.get("seq", 0),
                        "observedUtc": obs.get("observedUtc", snap.get("capturedUtc", "")),
                        "context": obs.get("context", snap.get("context", ""))}
            return _emit({
                "source": src,
                "from": when(1),
                "to": when(0),
                "sameContent": snaps[0] == snaps[1],
                "counts": {k: sum(1 for c in changes if c["what"] == k)
                           for k in ("added", "changed", "evidence raised",
                                     "evidence lowered", "evidence changed",
                                     "removed", "not observed")},
                "changes": changes[: max(1, int(limit))],
                "truncated": max(0, len(changes) - max(1, int(limit))),
                "note": "'removed' is only ever claimed where the scope was "
                        "collected completely in both scans; everything else "
                        "absent is 'not observed', which is a gap in the scan.",
            })

        @toolset_registry.tool_call
        @staticmethod
        def capabilities_inspect(key: str = "", source: str = "") -> str:
            """Find one capability by key across sources and show what each says about it."""
            wanted = str(key).strip()
            if not wanted:
                return _emit({"error": "no key given"})
            sources = [str(source).strip().lower()] if str(source).strip() else \
                      ["sdk", "portal", "game", "watchlist"]
            found = []
            for src in sources:
                snaps = _caps_list(src)
                if not snaps:
                    continue
                snap = _caps_load(src, snaps[0]) or {}
                for rec in snap.get("records", []):
                    if rec.get("key", "").lower() == wanted.lower():
                        found.append({
                            "source": src,
                            "snapshot": snaps[0],
                            "build": snap.get("build", ""),
                            **{k: rec.get(k) for k in
                               ("key", "kind", "scope", "display", "shape", "detail", "evidence")},
                        })
            if not found:
                return _emit({
                    "key": wanted,
                    "found": [],
                    "note": "not in any snapshot. That means it was not collected, "
                            "which is not the same as it not existing.",
                })
            return _emit({"key": wanted, "found": found})

    _TOOLSET_CLASS = BF6ContextToolset
    return _TOOLSET_CLASS


def register() -> str:
    """Register (or re-register) this module's toolset with UE's registry."""
    toolset_class = _define_toolset()
    registry = unreal.ToolsetRegistry
    if registry.is_toolset_class_registered(toolset_class):
        registry.unregister_toolset_class(toolset_class)
    registry.register_toolset_class(toolset_class)
    message = f"registered {toolset_class.__module__}.{toolset_class.__name__}"
    unreal.log(f"BF6 context MCP: {message}")
    return message


register()
