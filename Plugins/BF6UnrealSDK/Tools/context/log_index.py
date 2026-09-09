"""What happened, in order.

Reads the Unreal logs under Saved/Logs and answers questions about them:
which sessions exist, what a category said, what led up to a message, and
which messages are repeating.

Standard library only, Python 3.11, no Unreal import. It has to run from a
plain shell with the editor closed, because the times you most want to read
the log are the times the editor will not start.

Everything streams. Saved/Logs holds the live log plus dated backups and some
of them are hundreds of MB, so no function here ever calls read() on a whole
file, and nothing keeps more of a file in memory than the current query needs.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import sys
from collections import deque

# ---------------------------------------------------------------------------
# Roots
# ---------------------------------------------------------------------------

UPROJECT = "BF6_Unreal_SDK.uproject"


def find_root(start: str | None = None) -> str:
    """Walk up from this file until the uproject is found.

    Never hard code a user name. The tools live inside the plugin, which lives
    inside the project, so walking up always lands on the right place no matter
    where the repository has been cloned.
    """
    here = os.path.abspath(start or __file__)
    d = os.path.dirname(here)
    while True:
        if os.path.isfile(os.path.join(d, UPROJECT)):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            # Fall back to four levels up: Tools/context -> Tools -> plugin ->
            # Plugins -> root. Better a wrong guess than a crash on import.
            return os.path.abspath(os.path.join(os.path.dirname(here), "..", "..", "..", ".."))
        d = parent


ROOT = find_root()
LOGS = os.path.join(ROOT, "Saved", "Logs")


# ---------------------------------------------------------------------------
# Line grammar
#
# Three shapes occur in a real Unreal log and all three matter:
#
#   [2026.09.07-07.14.57:496][ 37]LogBF6Portal: Display: Portal resume: ...
#   LogPluginManager: Mounting Engine plugin Water
#       <- no timestamp; everything logged before the timestamped log device is
#          installed comes out bare, and that is most of the first 200 lines
#   \t- TDR settings OK - Level: Recover, Delay: 2
#       <- a continuation of the event above it, indented or not
#
# The frame counter is space padded to three columns, so [  0], [ 37], [383].
# The level is optional and only ever one of a fixed set of words. That fixed
# set is load bearing: "LogModuleManager: InternalLoadLibrary: '...'" looks
# exactly like a level if you match any word, and mis-reading it puts a
# thousand lines under a level that does not exist.
# ---------------------------------------------------------------------------

TS_RE = re.compile(
    r"^\[(?P<y>\d{4})\.(?P<mo>\d{2})\.(?P<d>\d{2})"
    r"-(?P<h>\d{2})\.(?P<mi>\d{2})\.(?P<s>\d{2}):(?P<ms>\d{3})\]"
    r"(?:\[\s*(?P<frame>\d+)\s*\])?"
    r"(?P<rest>.*)$"
)

CAT_RE = re.compile(r"^(?P<cat>[A-Za-z_][A-Za-z0-9_]*):[ \t](?P<rest>.*)$")

LEVELS = ("Fatal", "Error", "Warning", "Display", "Log", "Verbose", "VeryVerbose")
LEVEL_RE = re.compile(r"^(?P<lvl>" + "|".join(LEVELS) + r"):[ \t](?P<rest>.*)$")

OPEN_RE = re.compile(r"^Log file open,\s*(?P<when>.*)$")
CLOSE_RE = re.compile(r"^Log file closed,\s*(?P<when>.*)$")

ENGINE_RE = re.compile(r"^Engine Version:\s*(?P<v>\S+)")
CMDLINE_RE = re.compile(r"^Command Line:\s*(?P<c>.*)$")


def _iso(m: re.Match) -> str:
    return "{}-{}-{}T{}:{}:{}.{}".format(
        m.group("y"), m.group("mo"), m.group("d"),
        m.group("h"), m.group("mi"), m.group("s"), m.group("ms"),
    )


def _seconds(iso: str | None) -> float | None:
    """Seconds since the start of the iso day. Only used for periods.

    Loops are measured inside one session, and no session in practice spans a
    day boundary, so a full datetime parse would buy nothing but cost time on
    every one of several million lines.
    """
    if not iso:
        return None
    try:
        day = iso[8:10]
        h = int(iso[11:13])
        mi = int(iso[14:16])
        s = int(iso[17:19])
        ms = int(iso[20:23])
        return int(day) * 86400.0 + h * 3600.0 + mi * 60.0 + s + ms / 1000.0
    except (ValueError, IndexError):
        return None


# Unreal writes the log with a utf-8 byte order mark. Decoding as plain utf-8
# leaves it as a real character on the front of line one, which is enough to
# make the open banner not match and the whole directory look empty.
BOM = "﻿"


def _open(path: str):
    # The logs contain bytes that are not valid utf-8. Crashing on one of them
    # makes the tool useless exactly when it is needed most.
    return io.open(path, "r", encoding="utf-8", errors="replace", newline="")


# ---------------------------------------------------------------------------
# Streaming parse
# ---------------------------------------------------------------------------

class Event(dict):
    """A parsed log event. A dict so json.dumps takes it unchanged."""


def _new_event(time, frame, category, level, text, filename, lineno, session):
    return Event(
        time=time, frame=frame, category=category, level=level,
        text=text, file=filename, line=lineno, session=session,
    )


def iter_events(path: str, filename: str | None = None):
    """Yield every event in one log file, in order, one at a time.

    A continuation line is attached to the event above it rather than becoming
    an event of its own, because the multi line status blocks the tool prints
    (BF6 editors, water status) are one message and splitting them loses the
    only thing they were printed for.
    """
    filename = filename or os.path.basename(path)
    session_n = 0
    session = filename
    pending = None

    with _open(path) as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip("\r\n")
            if line[:1] == BOM:
                line = line.lstrip(BOM)

            mo = OPEN_RE.match(line)
            if mo:
                if pending is not None:
                    yield pending
                    pending = None
                session_n += 1
                session = filename if session_n == 1 else "{}#{}".format(filename, session_n)
                yield _new_event(None, None, "LogFile", "Display",
                                 "Log file open, " + mo.group("when"),
                                 filename, lineno, session)
                continue

            m = TS_RE.match(line)
            if m:
                if pending is not None:
                    yield pending
                    pending = None
                time = _iso(m)
                frame = int(m.group("frame")) if m.group("frame") is not None else None
                rest = m.group("rest")
            else:
                time = None
                frame = None
                rest = line

            mc = CAT_RE.match(rest)
            if mc is None:
                # Not a category line. Either a continuation of the event above
                # or, if there is nothing above, a stray line kept as its own
                # event so nothing in the file is silently dropped.
                if pending is not None:
                    pending["text"] += "\n" + rest
                    continue
                if not rest.strip():
                    continue
                pending = _new_event(time, frame, "", "Log", rest, filename, lineno, session)
                continue

            category = mc.group("cat")
            body = mc.group("rest")
            ml = LEVEL_RE.match(body)
            if ml:
                level = ml.group("lvl")
                body = ml.group("rest")
            else:
                level = "Log"

            if pending is not None:
                yield pending
            pending = _new_event(time, frame, category, level, body, filename, lineno, session)

    if pending is not None:
        yield pending


def _is_unreal_log(path: str) -> bool:
    """Saved/Logs also holds cef3*.log, which is Chromium's format, not ours.

    Rather than blocklist a name, ask the file: an Unreal log opens with the
    banner and nothing else does.
    """
    try:
        with _open(path) as fh:
            return fh.readline().lstrip(BOM).startswith("Log file open,")
    except OSError:
        return False


def log_files(logs_dir: str | None = None) -> list[str]:
    d = logs_dir or LOGS
    if not os.path.isdir(d):
        return []
    out = []
    for name in sorted(os.listdir(d)):
        if not name.lower().endswith(".log"):
            continue
        p = os.path.join(d, name)
        if os.path.isfile(p) and _is_unreal_log(p):
            out.append(p)
    return out


# ---------------------------------------------------------------------------
# sessions
# ---------------------------------------------------------------------------

def sessions(logs_dir: str | None = None) -> list[dict]:
    """One record per log file open banner, oldest first.

    Scanned with the bare regexes rather than through iter_events because this
    walks every byte of every log in the directory and does not need events.
    """
    out = []
    for path in log_files(logs_dir):
        filename = os.path.basename(path)
        cur = None
        n = 0
        with _open(path) as fh:
            for raw in fh:
                line = raw.rstrip("\r\n")
                if line[:1] == BOM:
                    line = line.lstrip(BOM)
                mo = OPEN_RE.match(line)
                if mo:
                    if cur is not None:
                        out.append(cur)
                    n += 1
                    cur = {
                        "file": filename,
                        "session": filename if n == 1 else "{}#{}".format(filename, n),
                        "opened": mo.group("when"),
                        "start": None,
                        "end": None,
                        "lines": 0,
                        "engine": None,
                        "cmdline": None,
                        "closed": False,
                    }
                    continue
                if cur is None:
                    continue
                cur["lines"] += 1
                m = TS_RE.match(line)
                rest = m.group("rest") if m else line
                if m:
                    t = _iso(m)
                    if cur["start"] is None:
                        cur["start"] = t
                    cur["end"] = t
                if cur["engine"] is None or cur["cmdline"] is None:
                    mc = CAT_RE.match(rest)
                    if mc is not None and mc.group("cat") == "LogInit":
                        body = mc.group("rest")
                        me = ENGINE_RE.match(body)
                        if me and cur["engine"] is None:
                            cur["engine"] = me.group("v")
                        mcl = CMDLINE_RE.match(body)
                        if mcl and cur["cmdline"] is None:
                            cur["cmdline"] = mcl.group("c").strip()
                if CLOSE_RE.match(rest):
                    cur["closed"] = True
        if cur is not None:
            out.append(cur)

    out.sort(key=lambda r: (r["start"] or "", r["file"]))
    return out


def _ordered_sessions(logs_dir=None, only=None):
    """Session records in chronological order, filtered to `only` if given."""
    recs = sessions(logs_dir)
    if only:
        want = only.lower()
        recs = [r for r in recs if want in r["session"].lower() or want in r["file"].lower()]
    return recs


def _stream(logs_dir=None, only=None):
    """Every event in the directory, oldest session first."""
    d = logs_dir or LOGS
    recs = _ordered_sessions(logs_dir, only)
    seen_files = []
    for rec in recs:
        if rec["file"] in seen_files:
            continue
        seen_files.append(rec["file"])
    wanted_sessions = {r["session"] for r in recs}
    for filename in seen_files:
        for ev in iter_events(os.path.join(d, filename), filename):
            if ev["session"] in wanted_sessions:
                yield ev


# ---------------------------------------------------------------------------
# events
# ---------------------------------------------------------------------------

def _matcher(pattern, regex=False, ignore_case=True):
    if pattern is None:
        return lambda s: True
    if regex:
        rx = re.compile(pattern, re.IGNORECASE if ignore_case else 0)
        return lambda s: rx.search(s) is not None
    if ignore_case:
        needle = pattern.lower()
        return lambda s: needle in s.lower()
    return lambda s: pattern in s


def events(category=None, level=None, pattern=None, since=None, until=None,
           session=None, limit=500, logs_dir=None, regex=False):
    match = _matcher(pattern, regex)
    cat = category.lower() if category else None
    lvl = level.lower() if level else None
    out = []
    for ev in _stream(logs_dir, session):
        if cat and ev["category"].lower() != cat:
            continue
        if lvl and ev["level"].lower() != lvl:
            continue
        if since and (ev["time"] is None or ev["time"] < since):
            continue
        if until and (ev["time"] is None or ev["time"] > until):
            continue
        if not match(ev["text"]):
            continue
        out.append(ev)
        if limit and len(out) >= limit:
            break
    return out


# ---------------------------------------------------------------------------
# around
# ---------------------------------------------------------------------------

def around(pattern, before=40, after=40, occurrence="last", logs_dir=None,
           session=None, category=None, regex=False):
    """The events surrounding a match. The "what led up to it" call.

    Two passes. The first records only the position of each match, which is a
    pair of small integers; the second re-streams the one session that holds
    the chosen match and keeps a ring buffer of `before` events. A single pass
    cannot do this: asking for the last match with a large `before` would mean
    holding every event since the match, and that is unbounded.
    """
    match = _matcher(pattern, regex)
    cat = category.lower() if category else None

    hits = []  # (session, ordinal within that session)
    counters = {}
    for ev in _stream(logs_dir, session):
        s = ev["session"]
        i = counters.get(s, -1) + 1
        counters[s] = i
        if cat and ev["category"].lower() != cat:
            continue
        if match(ev["text"]):
            hits.append((s, i))

    if not hits:
        return {"pattern": pattern, "occurrence": occurrence, "matches": 0, "events": []}

    if occurrence == "first":
        pick = 0
    elif occurrence == "last":
        pick = len(hits) - 1
    else:
        n = int(occurrence)
        pick = n - 1 if n > 0 else len(hits) + n
        pick = max(0, min(pick, len(hits) - 1))

    want_session, want_index = hits[pick]
    filename = want_session.split("#")[0]

    ring = deque(maxlen=before if before > 0 else 1)
    collected = []
    tail = 0
    i = -1
    d = logs_dir or LOGS
    for ev in iter_events(os.path.join(d, filename), filename):
        if ev["session"] != want_session:
            continue
        i += 1
        if i < want_index:
            if before > 0:
                ring.append(ev)
            continue
        if i == want_index:
            collected = list(ring) + [ev]
            if after <= 0:
                break
            continue
        collected.append(ev)
        tail += 1
        if tail >= after:
            break

    return {
        "pattern": pattern,
        "occurrence": occurrence,
        "matches": len(hits),
        "session": want_session,
        "match_index": want_index,
        "before": before,
        "after": after,
        "events": collected,
    }


# ---------------------------------------------------------------------------
# loops
# ---------------------------------------------------------------------------

_GUID = re.compile(r"\b[0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}\b")
_HEX = re.compile(r"\b0[xX][0-9a-fA-F]+\b")
_QUOTED = re.compile(r"'[^'\n]*'|\"[^\"\n]*\"")
_LONGHEX = re.compile(r"\b[0-9a-fA-F]{16,}\b")
_NUM = re.compile(r"[-+]?\d[\d,]*(?:\.\d+)?(?:[eE][-+]?\d+)?")
_WS = re.compile(r"\s+")


def normalise(text: str) -> str:
    """Collapse the parts of a message that change between repeats.

    Order matters. Quoted strings go before numbers so an id inside quotes
    becomes one placeholder rather than a placeholder plus punctuation, which
    would leave two spellings of the same message that never group together.
    """
    s = text.split("\n", 1)[0]
    s = _GUID.sub("<id>", s)
    s = _HEX.sub("<n>", s)
    s = _QUOTED.sub("<s>", s)
    s = _LONGHEX.sub("<id>", s)
    s = _NUM.sub("<n>", s)
    s = _WS.sub(" ", s).strip()
    return s[:400]


_MAX_KEYS = 200000


# A group of messages that share a normalised shape is not automatically a
# loop. Engine start up fires 800 InternalLoadLibrary lines whose only
# difference is the quoted dll name, and normalising quotes away makes them
# look like one message repeated 800 times. They are a template family, not a
# repeat, and left in they bury every real loop in the report.
#
# The two signals together separate them. A template family has many distinct
# raw texts AND no period, because it is one burst inside a few milliseconds.
# A real loop is either the same text over and over (one variant) or it carries
# a changing id but arrives on a clock. Either signal alone would be wrong: a
# per frame spam loop has one variant and a zero period, and a counting loop
# has as many variants as occurrences.
_VARIANT_LIMIT = 8
_BURST_PERIOD_S = 0.05


def loops(min_repeats=5, logs_dir=None, session=None, category=None, limit=40,
          max_variants=_VARIANT_LIMIT, include_templates=False):
    """Repeated near identical messages, grouped inside one session.

    Grouped per session on purpose. The same loop appears in several backups,
    and merging them across files would report a period measured across the
    gap between two editor runs, which is not a period at all.
    """
    cat = category.lower() if category else None
    groups: dict[tuple, dict] = {}
    cap = (max_variants or _VARIANT_LIMIT) + 1

    for ev in _stream(logs_dir, session):
        if cat and ev["category"].lower() != cat:
            continue
        first_line = ev["text"].split("\n", 1)[0]
        key = (ev["session"], ev["category"], ev["level"], normalise(first_line))
        g = groups.get(key)
        t = _seconds(ev["time"])
        if g is None:
            if len(groups) >= _MAX_KEYS:
                # A long log can hold a very large number of one-off messages.
                # Dropping the singletons keeps memory flat without touching
                # anything that could still become a loop.
                for k in [k for k, v in groups.items() if v["count"] == 1]:
                    del groups[k]
            groups[key] = {
                "count": 1, "first": ev["time"], "last": ev["time"],
                "sample": first_line,
                "_prev": t, "_deltas": [],
                "_variants": {first_line}, "_variants_capped": False,
                "file": ev["file"], "line": ev["line"],
            }
            continue
        g["count"] += 1
        if not g["_variants_capped"]:
            g["_variants"].add(first_line)
            if len(g["_variants"]) > cap:
                g["_variants_capped"] = True
                g["_variants"] = set()
        if ev["time"]:
            if g["first"] is None:
                g["first"] = ev["time"]
            g["last"] = ev["time"]
        # 512 gaps is far more than the median needs and keeps the per group
        # cost flat. A 300 MB log holds tens of thousands of groups, and at
        # 5000 gaps each the report cost more memory than the query it answered.
        if t is not None and g["_prev"] is not None and len(g["_deltas"]) < 512:
            dt = t - g["_prev"]
            if dt >= 0:
                g["_deltas"].append(dt)
        if t is not None:
            g["_prev"] = t

    out = []
    for (sess, category_name, level, norm), g in groups.items():
        if g["count"] < min_repeats:
            continue
        deltas = sorted(g["_deltas"])
        period = None
        if deltas:
            period = round(deltas[len(deltas) // 2], 2)
        elif g["first"] and g["last"] and g["count"] > 1:
            a, b = _seconds(g["first"]), _seconds(g["last"])
            if a is not None and b is not None:
                period = round((b - a) / (g["count"] - 1), 2)
        span = None
        a, b = _seconds(g["first"]), _seconds(g["last"])
        if a is not None and b is not None:
            span = round(b - a, 2)

        variants = "{}+".format(cap) if g["_variants_capped"] else len(g["_variants"])
        is_template = (
            g["_variants_capped"]
            and period is not None
            and period < _BURST_PERIOD_S
        )
        if is_template and not include_templates:
            continue

        out.append({
            "text": g["sample"],
            "normalised": norm,
            "category": category_name,
            "level": level,
            "session": sess,
            "count": g["count"],
            "variants": variants,
            "template_family": is_template,
            "first": g["first"],
            "last": g["last"],
            "span_s": span,
            "period_s": period,
            "file": g["file"],
            "line": g["line"],
        })

    out.sort(key=lambda r: -r["count"])
    return out[:limit] if limit else out


# ---------------------------------------------------------------------------
# errors
# ---------------------------------------------------------------------------

def errors(since=None, logs_dir=None, session=None, limit=60):
    groups: dict[tuple, dict] = {}
    for ev in _stream(logs_dir, session):
        if ev["level"] not in ("Error", "Warning", "Fatal"):
            continue
        if since and (ev["time"] is None or ev["time"] < since):
            continue
        key = (ev["level"], ev["category"], normalise(ev["text"]))
        g = groups.get(key)
        if g is None:
            groups[key] = {
                "level": ev["level"], "category": ev["category"],
                "text": ev["text"].split("\n", 1)[0], "count": 1,
                "first": ev["time"], "last": ev["time"],
                "file": ev["file"], "line": ev["line"], "sessions": {ev["session"]},
            }
            continue
        g["count"] += 1
        g["last"] = ev["time"] or g["last"]
        if g["first"] is None:
            g["first"] = ev["time"]
        g["sessions"].add(ev["session"])

    out = []
    for g in groups.values():
        g["sessions"] = sorted(g["sessions"])
        out.append(g)
    order = {"Fatal": 0, "Error": 1, "Warning": 2}
    out.sort(key=lambda r: (order.get(r["level"], 3), -r["count"]))
    return out[:limit] if limit else out


# ---------------------------------------------------------------------------
# summary
# ---------------------------------------------------------------------------

def summary(session=None, logs_dir=None):
    cats: dict[str, dict] = {}
    total = 0
    errs = 0
    warns = 0
    seen_sessions = []
    for ev in _stream(logs_dir, session):
        total += 1
        if ev["session"] not in seen_sessions:
            seen_sessions.append(ev["session"])
        c = ev["category"] or "(continuation)"
        rec = cats.get(c)
        if rec is None:
            rec = cats[c] = {"category": c, "count": 0, "first": None, "last": None,
                             "errors": 0, "warnings": 0}
        rec["count"] += 1
        if ev["time"]:
            if rec["first"] is None:
                rec["first"] = ev["time"]
            rec["last"] = ev["time"]
        if ev["level"] in ("Error", "Fatal"):
            rec["errors"] += 1
            errs += 1
        elif ev["level"] == "Warning":
            rec["warnings"] += 1
            warns += 1

    lp = loops(min_repeats=5, logs_dir=logs_dir, session=session, limit=0)
    by_cat = {}
    for l in lp:
        by_cat[l["category"]] = by_cat.get(l["category"], 0) + 1
    for c, rec in cats.items():
        rec["loops"] = by_cat.get(c, 0)

    ordered = sorted(cats.values(), key=lambda r: -r["count"])
    return {
        "sessions": seen_sessions,
        "events": total,
        "errors": errs,
        "warnings": warns,
        "loops": len(lp),
        "categories": ordered,
    }


# ---------------------------------------------------------------------------
# Printing
# ---------------------------------------------------------------------------

def fmt_event(ev: dict) -> str:
    t = ev["time"] or "-" * 23
    f = "{:>4}".format(ev["frame"]) if ev["frame"] is not None else "   -"
    head = "[{}][{}]{}: {}: ".format(t, f, ev["category"] or "-", ev["level"])
    lines = ev["text"].split("\n")
    out = head + lines[0]
    pad = " " * 4
    for extra in lines[1:]:
        out += "\n" + pad + extra
    return out


def _print_sessions(recs, logs_dir=None):
    where = logs_dir or LOGS
    if not recs:
        print("No Unreal logs found under " + where)
        return
    print("{} session(s) under {}".format(len(recs), where))
    print("")
    for r in recs:
        print(r["session"])
        print("    opened   {}   engine {}".format(r["opened"], r["engine"] or "?"))
        print("    events   {} .. {}".format(r["start"] or "?", r["end"] or "?"))
        print("    lines    {}{}".format(r["lines"], "" if r["closed"] else "   (no close banner, ended abruptly)"))
        if r["cmdline"]:
            print("    cmdline  {}".format(r["cmdline"]))


def _print_loops(rows):
    if not rows:
        print("No repeating message reached the threshold.")
        return
    for r in rows:
        per = "{} s".format(r["period_s"]) if r["period_s"] is not None else "?"
        print("{:>6} x  every {:>7}  {} variant(s)  {}  [{}]".format(
            r["count"], per, r["variants"], r["category"], r["session"]))
        print("        {}".format(r["text"]))
        print("        {} .. {}   span {} s".format(r["first"] or "?", r["last"] or "?",
                                                    r["span_s"] if r["span_s"] is not None else "?"))
        print("")


def _print_errors(rows):
    if not rows:
        print("No errors or warnings matched.")
        return
    for r in rows:
        print("{:>6} x  {:<8} {}".format(r["count"], r["level"], r["category"]))
        print("        {}".format(r["text"]))
        print("        {} .. {}   {}:{}".format(r["first"] or "?", r["last"] or "?", r["file"], r["line"]))
        print("")


def _print_summary(s):
    print("sessions {}   events {}   errors {}   warnings {}   loops {}".format(
        len(s["sessions"]), s["events"], s["errors"], s["warnings"], s["loops"]))
    print("")
    print("{:<28}{:>9}{:>8}{:>10}{:>7}  {}".format("category", "events", "errors", "warnings", "loops", "first .. last"))
    for r in s["categories"]:
        print("{:<28}{:>9}{:>8}{:>10}{:>7}  {} .. {}".format(
            r["category"][:28], r["count"], r["errors"], r["warnings"], r["loops"],
            (r["first"] or "?")[:23], (r["last"] or "?")[:23]))


def _print_around(res):
    if not res["events"]:
        print("No event matched {!r}.".format(res["pattern"]))
        return
    print("{} match(es) for {!r}. Showing the {} one in session {}, "
          "{} event(s) before and {} after.".format(
              res["matches"], res["pattern"], res["occurrence"], res["session"],
              res["before"], res["after"]))
    print("")
    for ev in res["events"]:
        print(fmt_event(ev))


# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(
        prog="log_index.py",
        description="What happened, in order. Reads Saved/Logs without loading a log into memory.")
    p.add_argument("--logs", default=None, help="log directory (default Saved/Logs under the project)")
    p.add_argument("--json", action="store_true", help="print json instead of a report")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("sessions", help="one line per log file open banner")

    pe = sub.add_parser("events", help="filtered events")
    pe.add_argument("--category")
    pe.add_argument("--level")
    pe.add_argument("--pattern")
    pe.add_argument("--regex", action="store_true")
    pe.add_argument("--since")
    pe.add_argument("--until")
    pe.add_argument("--session")
    pe.add_argument("--limit", type=int, default=500)

    pa = sub.add_parser("around", help="the events surrounding a match")
    pa.add_argument("pattern")
    pa.add_argument("--before", type=int, default=40)
    pa.add_argument("--after", type=int, default=40)
    pa.add_argument("--occurrence", default="last", help="first, last, or a 1 based number")
    pa.add_argument("--session")
    pa.add_argument("--category")
    pa.add_argument("--regex", action="store_true")

    pl = sub.add_parser("loops", help="repeated near identical messages")
    pl.add_argument("--min-repeats", type=int, default=5)
    pl.add_argument("--session")
    pl.add_argument("--category")
    pl.add_argument("--limit", type=int, default=40)
    pl.add_argument("--max-variants", type=int, default=_VARIANT_LIMIT,
                    help="how many distinct raw texts a group may hold before it "
                         "counts as a template family rather than a loop")
    pl.add_argument("--include-templates", action="store_true",
                    help="also show template families, such as the start up burst "
                         "of dll loads that differ only in the quoted name")

    px = sub.add_parser("errors", help="errors and warnings grouped by message")
    px.add_argument("--since")
    px.add_argument("--session")
    px.add_argument("--limit", type=int, default=60)

    ps = sub.add_parser("summary", help="per category counts for a session or the whole directory")
    ps.add_argument("--session")

    a = p.parse_args(argv)
    d = a.logs

    if a.cmd == "sessions":
        res = sessions(d)
        print(json.dumps(res, indent=2)) if a.json else _print_sessions(res, d)
    elif a.cmd == "events":
        res = events(a.category, a.level, a.pattern, a.since, a.until, a.session,
                     a.limit, d, a.regex)
        if a.json:
            print(json.dumps(res, indent=2))
        else:
            for ev in res:
                print(fmt_event(ev))
            print("")
            print("{} event(s).".format(len(res)))
    elif a.cmd == "around":
        res = around(a.pattern, a.before, a.after, a.occurrence, d, a.session,
                     a.category, a.regex)
        print(json.dumps(res, indent=2)) if a.json else _print_around(res)
    elif a.cmd == "loops":
        res = loops(a.min_repeats, d, a.session, a.category, a.limit,
                    a.max_variants, a.include_templates)
        print(json.dumps(res, indent=2)) if a.json else _print_loops(res)
    elif a.cmd == "errors":
        res = errors(a.since, d, a.session, a.limit)
        print(json.dumps(res, indent=2)) if a.json else _print_errors(res)
    elif a.cmd == "summary":
        res = summary(a.session, d)
        print(json.dumps(res, indent=2)) if a.json else _print_summary(res)
    return 0


if __name__ == "__main__":
    sys.exit(main())
