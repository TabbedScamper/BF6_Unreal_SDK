#!/usr/bin/env python3
"""gaps.py - what is missing or unwired in the BF6 Unreal tool.

Reads Saved/BF6Context/inventory.json, rebuilding it through inventory.py when
that file is stale or absent, and reports seven kinds of unfinished work.

The rule that governs every finding here: a gap report that cries wolf gets
ignored, and then it is worse than nothing. So every check below prefers to
miss a real gap over inventing a false one, and every finding carries the file
and line that a person can open to disagree with it.

    python gaps.py                  every subsystem
    python gaps.py blocks           one subsystem
    python gaps.py --json           the json only, nothing else on stdout

Python 3.11+, standard library only, no Unreal import. Runs with the editor
closed.
"""

import json
import os
import re
import sys
import datetime

# ---------------------------------------------------------------------------
# roots
# ---------------------------------------------------------------------------

UPROJECT = "BF6_Unreal_SDK.uproject"


def find_root(start=None):
    """Walk up until the uproject turns up. Never hard code a user name."""
    here = os.path.abspath(start or os.path.dirname(os.path.abspath(__file__)))
    while True:
        if os.path.isfile(os.path.join(here, UPROJECT)):
            return here.replace("\\", "/")
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit(
                "Could not find %s above %s. Run this from inside the project."
                % (UPROJECT, os.path.dirname(os.path.abspath(__file__)))
            )
        here = parent


ROOT = find_root()
PLUGIN = ROOT + "/Plugins/BF6UnrealSDK"
ADDON = ROOT + "/Plugins/Add-Ons/BF6HighPoly"
RESOURCES = PLUGIN + "/Resources"
OUT = ROOT + "/Saved/BF6Context"


def rel(path):
    p = os.path.abspath(path).replace("\\", "/")
    if p.lower().startswith(ROOT.lower() + "/"):
        return p[len(ROOT) + 1:]
    return p


def read_text(path):
    # The house rule: errors="replace" everywhere. A single bad byte in one
    # file must not take the whole report down.
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


# ---------------------------------------------------------------------------
# the source tree
# ---------------------------------------------------------------------------

# Generated and glue translation units. UnrealHeaderTool writes .gen.cpp and
# UnrealBuildTool writes Module.*.cpp, and neither is anybody's work.
SKIP_CPP = re.compile(r"(\.gen\.cpp$|^Module\.)", re.IGNORECASE)
SKIP_DIRS = ("/intermediate/", "/binaries/", "/thirdparty/", "/deriveddatacache/", "/saved/")

# Minified libraries we vendored. They are not our page code, so they are not
# scanned for ops and never reported as orphans, but they are still allowed to
# count as a reference to something else.
VENDOR = ("/vendor/", "/min/", "/node_modules/")


def is_skipped_dir(path):
    low = path.replace("\\", "/").lower()
    return any(d in low for d in SKIP_DIRS)


def source_files():
    """The 17-18 base Private/*.cpp plus the real add-on cpp files."""
    out = []
    base_dir = PLUGIN + "/Source/BF6UnrealSDK/Private"
    if os.path.isdir(base_dir):
        for name in sorted(os.listdir(base_dir)):
            if name.lower().endswith(".cpp") and not SKIP_CPP.search(name):
                out.append((base_dir + "/" + name, False))
    for dirpath, dirnames, filenames in os.walk(ADDON):
        if is_skipped_dir(dirpath + "/"):
            dirnames[:] = []
            continue
        for name in sorted(filenames):
            if name.lower().endswith(".cpp") and not SKIP_CPP.search(name):
                out.append((dirpath.replace("\\", "/") + "/" + name, True))
    return out


def page_files():
    """Resources/**/*.js that we wrote, vendor excluded."""
    out = []
    for dirpath, dirnames, filenames in os.walk(RESOURCES):
        d = dirpath.replace("\\", "/") + "/"
        if any(v in d.lower() for v in VENDOR):
            dirnames[:] = []
            continue
        for name in sorted(filenames):
            if name.lower().endswith(".js"):
                out.append(d + name)
    return out


def doc_files():
    out = []
    for dirpath, dirnames, filenames in os.walk(RESOURCES):
        d = dirpath.replace("\\", "/") + "/"
        if any(v in d.lower() for v in VENDOR):
            dirnames[:] = []
            continue
        for name in sorted(filenames):
            if name.lower().endswith(".md") and name.upper() != "LICENSE.MD":
                out.append(d + name)
    return out


def subsystem_of_source(path):
    """BF6<Name>.cpp is subsystem lower(<Name>), per the spec's derivation."""
    stem = os.path.splitext(os.path.basename(path))[0]
    if stem.startswith("SBF6"):
        stem = stem[4:]          # SBF6PreviewViewport -> previewviewport
    elif stem.startswith("BF6"):
        stem = stem[3:]
    if not stem:
        stem = os.path.splitext(os.path.basename(path))[0]
    name = stem.lower()
    # BF6UnrealSDK.cpp is the module itself, not a feature. It carries the
    # native core, so it needs an id of its own rather than being folded away.
    if name == "unrealsdk":
        name = "unrealsdk"
    if name == "highpolyviewportlibrary":
        name = "highpolyviewport"
    return name


# ---------------------------------------------------------------------------
# scanners, shared by the fallback inventory and by the gap checks
# ---------------------------------------------------------------------------

RE_AUTO_CMD = re.compile(
    r"FAutoConsoleCommand\w*\s*\w*\s*\(\s*(?:/\*.*?\*/\s*)?TEXT\(", re.DOTALL)
RE_REG_CMD = re.compile(r"RegisterConsoleCommand\s*\(\s*TEXT\(")
RE_STRING_RUN = re.compile(r'TEXT\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', re.DOTALL)
RE_ONE_STRING = re.compile(r'"((?:\\.|[^"\\])*)"')


def _joined_literal(text, pos):
    """Read the TEXT("a" "b") at pos and return (value, end offset).

    Unreal help strings are routinely split over several adjacent literals to
    keep the line length sane, and taking only the first piece silently turned
    long help into short help, which then looked like a documentation gap that
    was not there.
    """
    m = RE_STRING_RUN.match(text, pos) or RE_STRING_RUN.search(text, pos, pos + 400)
    if not m:
        return None, pos
    parts = RE_ONE_STRING.findall(m.group(1))
    value = "".join(parts)
    value = value.replace('\\"', '"').replace("\\n", " ").replace("\\t", " ")
    value = value.replace("\\\\", "\\")
    return value, m.end()


def _line_of(text, offset):
    return text.count("\n", 0, offset) + 1


def scan_commands(text, path):
    """Console commands: the BF6.* name and the next TEXT(...) as its help."""
    found = []
    starts = [m.end() - len("TEXT(") for m in RE_AUTO_CMD.finditer(text)]
    starts += [m.end() - len("TEXT(") for m in RE_REG_CMD.finditer(text)]
    for start in sorted(set(starts)):
        name, after = _joined_literal(text, start)
        if not name or not name.startswith("BF6."):
            continue
        help_text, _ = _joined_literal(text, after)
        if help_text is None:
            help_text = ""
        found.append({
            "name": name,
            "help": help_text.strip(),
            "file": rel(path),
            "line": _line_of(text, start),
        })
    return found


RE_LOG_CAT = re.compile(r"DEFINE_LOG_CATEGORY(?:_STATIC)?\s*\(\s*(\w+)")


def scan_log_categories(text):
    return sorted(set(RE_LOG_CAT.findall(text)))


# C++ receives an op by comparing it. The identifier is Op or something ending
# in Op, never a bare word, so Prop and Crop cannot slip in.
RE_CPP_IN = re.compile(r'\b(?:[A-Z]\w*)?Op\s*==\s*TEXT\(\s*"([^"]+)"\s*\)')
# C++ sends an op either through a json object or, in a couple of hot paths,
# as a hand written literal.
RE_CPP_OUT_FIELD = re.compile(r'SetStringField\(\s*TEXT\(\s*"op"\s*\)\s*,\s*TEXT\(\s*"([^"]+)"\s*\)')
RE_CPP_OUT_RAW = re.compile(r'\\"op\\"\s*:\s*\\"([^"\\]+)\\"')

# The page sends an op as a field of the object it posts, and site_sync also
# stamps op onto a measurement record it built earlier.
RE_JS_OUT_FIELD = re.compile(r"""(?<![\w.$])op\s*:\s*(['"])([^'"]+)\1""")
RE_JS_OUT_ASSIGN = re.compile(r"""\.\s*op\s*=\s*(['"])([^'"]+)\1""")
# The page receives by comparing, or by a switch over something dot op.
RE_JS_IN_CMP = re.compile(r"""\.\s*op\s*[=!]==?\s*(['"])([^'"]+)\1""")
RE_JS_SWITCH = re.compile(r"switch\s*\(\s*[\w$.]*\.\s*op\s*\)\s*\{")
RE_CASE = re.compile(r"""case\s+(['"])([^'"]+)\1\s*:""")


def _switch_body(text, open_brace):
    """Brace matched body of a switch, so case labels from an unrelated switch
    nested inside cannot leak in as ops the page handles."""
    depth = 0
    i = open_brace
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:i]
        i += 1
    return text[open_brace:]


def scan_ops_cpp(text):
    """(ops the file handles, ops the file sends), each name -> first line."""
    incoming, outgoing = {}, {}
    for m in RE_CPP_IN.finditer(text):
        incoming.setdefault(m.group(1), _line_of(text, m.start()))
    for regex in (RE_CPP_OUT_FIELD, RE_CPP_OUT_RAW):
        for m in regex.finditer(text):
            outgoing.setdefault(m.group(1), _line_of(text, m.start()))
    return incoming, outgoing


def scan_ops_js(text):
    """(ops the page sends, ops the page handles), each name -> first line."""
    sending, handling = {}, {}
    for regex in (RE_JS_OUT_FIELD, RE_JS_OUT_ASSIGN):
        for m in regex.finditer(text):
            sending.setdefault(m.group(2), _line_of(text, m.start()))
    for m in RE_JS_IN_CMP.finditer(text):
        handling.setdefault(m.group(2), _line_of(text, m.start()))
    for m in RE_JS_SWITCH.finditer(text):
        body = _switch_body(text, m.end() - 1)
        base = m.end() - 1
        # Only the top level of the switch. A nested switch on something else
        # sits inside a deeper brace pair and its cases are not op names.
        depth = 0
        for i, c in enumerate(body):
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            elif depth == 1 and c == "c":
                cm = RE_CASE.match(body, i)
                if cm:
                    handling.setdefault(cm.group(2), _line_of(text, base + i))
    return sending, handling


# ---------------------------------------------------------------------------
# inventory: import it if it is there, scan for ourselves if it is not
# ---------------------------------------------------------------------------

INVENTORY_JSON = OUT + "/inventory.json"


def _newest_source_mtime():
    newest = 0.0
    for path, _ in source_files():
        newest = max(newest, os.path.getmtime(path))
    for path in page_files():
        newest = max(newest, os.path.getmtime(path))
    return newest


def _looks_like_inventory(data):
    return (isinstance(data, dict)
            and isinstance(data.get("subsystems"), dict)
            and data["subsystems"]
            and isinstance(data.get("commands"), dict))


def load_inventory():
    """Returns (inventory dict, a sentence saying where it came from).

    inventory.py is written by somebody else and may not exist yet, so this
    tries three routes in order and always says out loud which one it used. A
    gap report built on a different reading of the tree than the reader thinks
    is the one failure mode that would make this file dangerous.
    """
    fresh = False
    if os.path.isfile(INVENTORY_JSON):
        try:
            fresh = os.path.getmtime(INVENTORY_JSON) >= _newest_source_mtime()
        except OSError:
            fresh = False
    if fresh:
        try:
            data = json.loads(read_text(INVENTORY_JSON))
            if _looks_like_inventory(data):
                return data, "inventory.json on disk, newer than every source file"
        except Exception:
            pass

    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    try:
        import inventory  # type: ignore
        builder = None
        for name in ("build", "build_inventory", "rebuild", "scan", "main"):
            fn = getattr(inventory, name, None)
            if callable(fn):
                builder = (name, fn)
                break
        if builder is None:
            raise RuntimeError("inventory.py has no build entry point")
        result = builder[1]()
        if _looks_like_inventory(result):
            return result, "inventory.py, through %s()" % builder[0]
        data = json.loads(read_text(INVENTORY_JSON))
        if _looks_like_inventory(data):
            return data, "inventory.py, rebuilt inventory.json through %s()" % builder[0]
        raise RuntimeError("inventory.py produced nothing this file recognises")
    except Exception as exc:
        reason = "%s: %s" % (type(exc).__name__, exc)

    return fallback_inventory(), (
        "gaps.py's own scan of the source tree, because inventory.py was not "
        "usable (%s)" % reason)


def fallback_inventory():
    """The same shape inventory.json has, built here so gaps.py stands alone."""
    subsystems = {}
    commands = {}
    log_categories = {}

    def slot(name, addon):
        return subsystems.setdefault(name, {
            "addon": addon,
            "sources": [],
            "log_categories": [],
            "commands": [],
            "web_ops": {"in": [], "out": []},
            "resources": [],
            "docs": [],
            "mcp_tools": [],
        })

    for path, addon in source_files():
        sid = subsystem_of_source(path)
        text = read_text(path)
        s = slot(sid, addon)
        s["addon"] = s["addon"] or addon
        s["sources"].append(rel(path))
        for cat in scan_log_categories(text):
            if cat not in s["log_categories"]:
                s["log_categories"].append(cat)
            log_categories[cat] = sid
        for cmd in scan_commands(text, path):
            s["commands"].append(cmd)
            commands[cmd["name"]] = sid
        incoming, outgoing = scan_ops_cpp(text)
        for op in sorted(incoming):
            if op not in s["web_ops"]["in"]:
                s["web_ops"]["in"].append(op)
        for op in sorted(outgoing):
            if op not in s["web_ops"]["out"]:
                s["web_ops"]["out"].append(op)

    # Resources folders map to a subsystem by name where the names agree, and
    # otherwise by whichever source mentions the folder.
    for path in page_files() + doc_files():
        r = rel(path)
        parts = r.split("/")
        folder = parts[parts.index("Resources") + 1] if "Resources" in parts else ""
        sid = folder.lower() if folder.lower() in subsystems else None
        if sid is None:
            continue
        key = "docs" if r.lower().endswith(".md") else "resources"
        rr = r[r.index("Resources"):]
        if rr not in subsystems[sid][key]:
            subsystems[sid][key].append(rr)

    return {
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "subsystems": subsystems,
        "commands": commands,
        "log_categories": log_categories,
    }


# ---------------------------------------------------------------------------
# the seven checks
# ---------------------------------------------------------------------------

def finding(kind, sid, addon, reason, file=None, line=None, evidence=None):
    f = {
        "category": kind,
        "subsystem": sid,
        "side": "add-on" if addon else "base tool",
        "reason": reason,
        "file": file,
        "line": line,
    }
    if evidence:
        f["evidence"] = evidence
    return f


def side_of(inv, sid):
    return bool(inv["subsystems"].get(sid, {}).get("addon"))


# --- 1. unreachable_op ------------------------------------------------------

# An op whose name is built at runtime, or that belongs to a conversation with
# something that is not our page, cannot be matched by reading literals. Each
# entry here was checked by hand against the file it names.
OP_EXEMPT = {
    # The page-to-tool log channel. C++ routes it by prefix, not by a literal
    # comparison, so the match would always fail on the C++ side.
    "log",
}


def check_unreachable_op(inv, cpp_scan, js_scan):
    cpp_in, cpp_out = {}, {}
    js_out, js_in = {}, {}
    for path, (incoming, outgoing) in cpp_scan.items():
        for op, line in incoming.items():
            cpp_in.setdefault(op, (path, line))
        for op, line in outgoing.items():
            cpp_out.setdefault(op, (path, line))
    for path, (sending, handling) in js_scan.items():
        for op, line in sending.items():
            js_out.setdefault(op, (path, line))
        for op, line in handling.items():
            js_in.setdefault(op, (path, line))

    # A name that appears as a quoted literal anywhere on the other side is not
    # reported, even when it was not matched by the dispatch patterns. That is
    # deliberate: a dispatch table or a computed name still wires the op up, and
    # a false dead-op claim costs more than a missed one.
    cpp_text = "\n".join(t for t in CPP_TEXT.values())
    js_text = "\n".join(t for t in JS_TEXT.values())

    def quoted_in(text, op):
        return ('"%s"' % op) in text or ("'%s'" % op) in text

    out = []
    for op, (path, line) in sorted(cpp_in.items()):
        if op in OP_EXEMPT or op in js_out or quoted_in(js_text, op):
            continue
        sid = subsystem_of_source(path)
        out.append(finding(
            "unreachable_op", sid, side_of(inv, sid),
            "C++ handles op '%s' but no page under Resources ever sends it" % op,
            rel(path), line))
    for op, (path, line) in sorted(js_out.items()):
        if op in OP_EXEMPT or op in cpp_in or quoted_in(cpp_text, op):
            continue
        sid = js_subsystem(inv, path)
        out.append(finding(
            "unreachable_op", sid, side_of(inv, sid),
            "the page sends op '%s' and no C++ file handles it" % op,
            rel(path), line))
    for op, (path, line) in sorted(cpp_out.items()):
        if op in OP_EXEMPT or op in js_in or quoted_in(js_text, op):
            continue
        sid = subsystem_of_source(path)
        out.append(finding(
            "unreachable_op", sid, side_of(inv, sid),
            "C++ sends op '%s' and no page handles it" % op,
            rel(path), line))
    for op, (path, line) in sorted(js_in.items()):
        if op in OP_EXEMPT or op in cpp_out or quoted_in(cpp_text, op):
            continue
        sid = js_subsystem(inv, path)
        out.append(finding(
            "unreachable_op", sid, side_of(inv, sid),
            "the page handles op '%s' and no C++ file sends it" % op,
            rel(path), line))
    return out


def js_subsystem(inv, path):
    r = rel(path)
    parts = r.split("/")
    if "Resources" in parts:
        folder = parts[parts.index("Resources") + 1].lower()
        if folder in inv["subsystems"]:
            return folder
        for sid, s in inv["subsystems"].items():
            if any(("Resources/" + folder + "/") in x for x in s.get("resources", [])):
                return sid
    return "unassigned"


# --- 2. no_status -----------------------------------------------------------

def check_no_status(inv):
    out = []
    for sid, s in sorted(inv["subsystems"].items()):
        cmds = s.get("commands") or []
        if not cmds:
            continue
        if any(c["name"].split(".")[-1].lower() == "status" for c in cmds):
            continue
        first = min(cmds, key=lambda c: (c.get("file") or "", c.get("line") or 0))
        out.append(finding(
            "no_status", sid, s.get("addon"),
            "%d command(s) and no Status command, so there is no way to ask "
            "this subsystem what it currently thinks" % len(cmds),
            first.get("file"), first.get("line")))
    return out


# --- 3. undocumented --------------------------------------------------------

RE_PLACEHOLDER = re.compile(r"^(todo|tbd|wip|n/?a|-+|\.+|\?+|help|test|debug)$", re.IGNORECASE)


def check_undocumented(inv):
    out = []
    for sid, s in sorted(inv["subsystems"].items()):
        for c in s.get("commands") or []:
            h = (c.get("help") or "").strip()
            if h and len(h) > 3 and not RE_PLACEHOLDER.match(h) and h != c["name"]:
                continue
            out.append(finding(
                "undocumented", sid, s.get("addon"),
                "%s has %s for a help string, so it does not explain itself in "
                "the console" % (c["name"], "no help" if not h else "'%s'" % h),
                c.get("file"), c.get("line")))
    return out


# --- 4. unfinished ----------------------------------------------------------

# "not yet" and "not implemented" turn up constantly in ordinary prose and in
# messages shown to a person ("not yet loaded"), so they are only counted when
# the line reads as a note to ourselves: a comment, and not a TEXT() literal.
RE_MARK_HARD = re.compile(r"\b(TODO|FIXME|HACK|XXX)\b")
RE_MARK_SOFT = re.compile(r"\b(not implemented|not yet implemented|not yet written|not yet wired|unimplemented)\b",
                          re.IGNORECASE)


def check_unfinished(inv, cpp_text):
    out = []
    for path, text in sorted(cpp_text.items()):
        sid = subsystem_of_source(path)
        addon = side_of(inv, sid)
        for n, line in enumerate(text.splitlines(), 1):
            stripped = line.strip()
            hard = RE_MARK_HARD.search(line)
            soft = RE_MARK_SOFT.search(line)
            if not hard and not soft:
                continue
            comment = stripped.startswith("//") or stripped.startswith("*") or stripped.startswith("/*")
            if soft and not hard:
                # A soft phrase inside a string is almost always a message to
                # the user, not an admission that the code is missing.
                if not comment or "TEXT(" in line:
                    continue
            mark = (hard.group(1) if hard else soft.group(1))
            out.append(finding(
                "unfinished", sid, addon,
                "%s left in the source: %s" % (mark, stripped[:150]),
                rel(path), n))
    return out


# --- 5. promised ------------------------------------------------------------

RE_BF6_CMD = re.compile(r"\bBF6\.[A-Za-z][A-Za-z0-9]*(?:\.[A-Za-z][A-Za-z0-9]*)*\b")
RE_TICKED = re.compile(r"`([^`\n]{2,80})`")
# Only our own namespace counts as a promise. Everything else inside backticks
# in these docs is the Portal site's vocabulary or a scrap of TypeScript, and
# flagging any of that would bury the real findings.
RE_OURS = re.compile(r"^BF6[A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$")
RE_FILEISH = re.compile(r"\.(cpp|h|js|md|json|ts|html|css|png|zip|uproject|tscn|xml|dll|py)$",
                        re.IGNORECASE)


def check_promised(inv, cpp_text, js_text, doc_text):
    known_commands = set(inv.get("commands") or {})
    haystack = "\n".join(list(cpp_text.values()) + list(js_text.values()))
    out = []
    seen = set()
    for path, text in sorted(doc_text.items()):
        lines = text.splitlines()
        candidates = {}
        for n, line in enumerate(lines, 1):
            for m in RE_BF6_CMD.finditer(line):
                name = m.group(0)
                if name.count(".") >= 1 and not RE_FILEISH.search(name):
                    candidates.setdefault(name, n)
            for m in RE_TICKED.finditer(line):
                name = m.group(1).strip().rstrip("()")
                if RE_OURS.match(name) and not RE_FILEISH.search(name):
                    candidates.setdefault(name, n)
        for name, n in sorted(candidates.items()):
            if name in seen:
                continue
            if name in known_commands or name in haystack:
                continue
            seen.add(name)
            sid = doc_subsystem(inv, path)
            kind = "command" if name.startswith("BF6.") else "symbol"
            out.append(finding(
                "promised", sid, side_of(inv, sid),
                "the doc names %s '%s' as if it exists, and it appears nowhere "
                "in the C++ or the page code" % (kind, name),
                rel(path), n))
    return out


def doc_subsystem(inv, path):
    return js_subsystem(inv, path)


# --- 6. orphan_resource -----------------------------------------------------

# Only files somebody authored and something is meant to load. Images, fonts
# and captured data are referenced by name from inside other data, and chasing
# those produced nothing but noise.
ORPHAN_EXTS = (".js", ".html", ".css", ".json")


NEAR = 400
# The only thing that makes an unnamed file live is code walking its folder.
ITERATORS = ("FindFiles", "IterateDirectory", "FindFilesRecursive", "DirectoryExists",
             "readdirSync", "readdir", "IFileManager::Get().FindFiles")


def _folder_enumerated(corpus, folder, skip_path):
    """True when C++ walks this folder, which loads whatever is inside it.

    Only C++ counts. The node test scripts under Resources/blocks/test walk
    folders of the same names under Saved for the site mirror, and letting
    those count suppressed real findings in Resources.
    """
    quoted = ['"%s"' % folder, "'%s'" % folder, "/%s/" % folder, "\\%s\\" % folder]
    for other, text in corpus.items():
        if other == skip_path or not other.lower().endswith(".cpp"):
            continue
        for q in quoted:
            at = text.find(q)
            while at != -1:
                window = text[max(0, at - NEAR): at + NEAR]
                if any(it in window for it in ITERATORS):
                    return True
                at = text.find(q, at + 1)
    return False


def check_orphan_resource(inv, cpp_text, js_text, doc_text):
    files = []
    for dirpath, dirnames, filenames in os.walk(RESOURCES):
        d = dirpath.replace("\\", "/") + "/"
        if any(v in d.lower() for v in VENDOR):
            dirnames[:] = []
            continue
        for name in sorted(filenames):
            if name.lower().endswith(ORPHAN_EXTS):
                files.append(d + name)

    # Everything that could name a resource, including html and the vendor
    # bundles, because a script tag in a vendored page still counts.
    corpus = {}
    corpus.update(cpp_text)
    corpus.update(js_text)
    corpus.update(doc_text)
    for path in files:
        if path.lower().endswith((".html", ".json")) and path not in corpus:
            corpus[path] = read_text(path)

    out = []
    for path in files:
        base = os.path.basename(path)
        r = rel(path)
        tail = r[r.index("Resources"):]
        tail_alt = tail.replace("/", "\\")
        names = [base, tail, tail_alt]
        hit = False
        for other, text in corpus.items():
            if other == path:
                continue
            if any(n in text for n in names):
                hit = True
                break
        # A folder the C++ enumerates loads everything inside it, so naming the
        # folder counts as naming the file. Without this, every snippet pack and
        # every faq answer was reported dead when all of them load. The folder
        # only counts when the name sits near the word Resources, because the
        # same folder names also exist under Saved for the site mirror, and
        # matching those suppressed findings that were real.
        folder = os.path.basename(os.path.dirname(path))
        if not hit and folder and folder.lower() != "resources":
            if _folder_enumerated(corpus, folder, path):
                hit = True
        if hit:
            continue
        sid = js_subsystem(inv, path)
        out.append(finding(
            "orphan_resource", sid, side_of(inv, sid),
            "nothing in the C++, the pages or the docs names %s, so no code "
            "path appears to load it" % base,
            r, None))
    return out


# --- 7. boundary ------------------------------------------------------------

# Reading the real game means one of three things: the libbf6 native core, the
# game install path, or the EBX and asset decode surface.
#
# Two names were tried here and dropped. PortalSDK is the Godot SDK download,
# which the base tool is supposed to handle, and counting it made five base
# files look like suspects. bf6mesh is our own container format, written by the
# SDK importer out of Godot data, so it says nothing about who read the game.
RE_GAME_READ = re.compile(
    r"\b(libbf6|bf6_core|GameInstallDir|g_gameDir|GameInstallPath|"
    r"[Ee]bx[A-Za-z]*|EBX|ResourceGuid|SuperBundle|ChunkGuid)\b")

# Evidence is ranked so the lines a reader sees first are calls, not a passing
# mention of EBX inside a log message.
STRONG_MARKERS = ("libbf6", "bf6_core", "GameInstallDir", "g_gameDir", "GameInstallPath")

# One passing mention in a comment is not a game read. Two or more lines of
# real code is a question worth asking.
BOUNDARY_MIN_LINES = 2


def _code_lines_matching(text, regex):
    out = []
    in_block = False
    for n, line in enumerate(text.splitlines(), 1):
        s = line.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block = True
            continue
        if s.startswith("//") or s.startswith("*"):
            continue
        code = line.split("//", 1)[0]
        m = regex.search(code)
        if m:
            out.append((n, s[:150], m.group(0)))
    return out


def check_boundary(inv, cpp_text, addon_flags):
    out = []
    for path, text in sorted(cpp_text.items()):
        sid = subsystem_of_source(path)
        addon = addon_flags.get(path, False)
        hits = _code_lines_matching(text, RE_GAME_READ)
        if addon and not hits:
            out.append(finding(
                "boundary", sid, True,
                "this add-on file shows no sign of reading the real game (no "
                "libbf6 call, no install path, no EBX or mesh decode). Should "
                "it live in the base tool instead?",
                rel(path), None))
        elif not addon and len(hits) >= BOUNDARY_MIN_LINES:
            ranked = sorted(hits, key=lambda h: (0 if h[2] in STRONG_MARKERS else 1, h[0]))
            out.append(finding(
                "boundary", sid, False,
                "this base tool file reads the real game on %d line(s) (%s). "
                "Should that part live in the add-on instead?"
                % (len(hits), ", ".join(sorted({h[2] for h in hits}))[:120]),
                rel(path), ranked[0][0],
                evidence=["%s:%d  %s" % (rel(path), n, s) for n, s, _ in ranked[:3]]))
    return out


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------

CPP_TEXT = {}
JS_TEXT = {}
DOC_TEXT = {}

CATEGORIES = ["unreachable_op", "no_status", "undocumented", "unfinished",
              "promised", "orphan_resource", "boundary"]


def build(subsystem=None):
    inv, provenance = load_inventory()

    addon_flags = {}
    cpp_scan = {}
    for path, addon in source_files():
        text = read_text(path)
        CPP_TEXT[path] = text
        addon_flags[path] = addon
        cpp_scan[path] = scan_ops_cpp(text)
    js_scan = {}
    for path in page_files():
        text = read_text(path)
        JS_TEXT[path] = text
        js_scan[path] = scan_ops_js(text)
    for path in doc_files():
        DOC_TEXT[path] = read_text(path)

    findings = []
    findings += check_unreachable_op(inv, cpp_scan, js_scan)
    findings += check_no_status(inv)
    findings += check_undocumented(inv)
    findings += check_unfinished(inv, CPP_TEXT)
    findings += check_promised(inv, CPP_TEXT, JS_TEXT, DOC_TEXT)
    findings += check_orphan_resource(inv, CPP_TEXT, JS_TEXT, DOC_TEXT)
    findings += check_boundary(inv, CPP_TEXT, addon_flags)

    if subsystem:
        want = subsystem.lower()
        findings = [f for f in findings if f["subsystem"].lower() == want]

    findings.sort(key=lambda f: (f["subsystem"], CATEGORIES.index(f["category"]),
                                 f["file"] or "", f["line"] or 0))

    counts = {c: 0 for c in CATEGORIES}
    for f in findings:
        counts[f["category"]] += 1

    return {
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "inventory_source": provenance,
        "subsystem_filter": subsystem or "",
        "counts": counts,
        "total": len(findings),
        "findings": findings,
    }, inv


def report(result, inv):
    lines = []
    add = lines.append
    add("BF6 gap report")
    add("  inventory from: " + result["inventory_source"])
    add("  generated:      " + result["generated"])
    if result["subsystem_filter"]:
        add("  subsystem:      " + result["subsystem_filter"])
    add("")
    add("Counts by category")
    for c in CATEGORIES:
        add("  %-16s %d" % (c, result["counts"][c]))
    add("  %-16s %d" % ("TOTAL", result["total"]))
    add("")

    by_sub = {}
    for f in result["findings"]:
        by_sub.setdefault(f["subsystem"], []).append(f)
    for sid in sorted(by_sub):
        s = inv["subsystems"].get(sid, {})
        side = "add-on" if s.get("addon") else "base tool"
        add("=" * 70)
        add("%s  (%s)  %d finding(s)" % (sid, side, len(by_sub[sid])))
        add("=" * 70)
        last = None
        for f in by_sub[sid]:
            if f["category"] != last:
                add("  [%s]" % f["category"])
                last = f["category"]
            where = ""
            if f["file"]:
                where = "    %s%s" % (f["file"], ":%d" % f["line"] if f["line"] else "")
            add("    - " + f["reason"])
            if where:
                add(where)
            for e in f.get("evidence", []):
                add("        " + e)
        add("")
    if not result["findings"]:
        add("Nothing found.")
    return "\n".join(lines)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    json_only = "--json" in argv[1:]
    subsystem = args[0] if args else None

    result, inv = build(subsystem)

    os.makedirs(OUT, exist_ok=True)
    path = OUT + "/gaps.json"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)

    if json_only:
        print(json.dumps(result, indent=2))
    else:
        print(report(result, inv))
        print("Saved " + rel(path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
