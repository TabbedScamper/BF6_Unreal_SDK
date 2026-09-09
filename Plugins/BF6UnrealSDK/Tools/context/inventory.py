"""inventory.py - what exists in the BF6 Unreal SDK tool.

Answers "which of the thirty C++ files owns this?" without opening any of them.
Writes Saved/BF6Context/inventory.json and prints a summary.

Standard library only, Python 3.11+, and no unreal import: this has to run from
a plain shell with the editor closed, which is exactly when it is needed.

    python inventory.py                             rebuild and summarise
    python inventory.py blocks                      all about one subsystem
    python inventory.py --command BF6.Portal.Link   which subsystem owns it
"""

from __future__ import annotations

import datetime
import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# Roots
# ---------------------------------------------------------------------------

UPROJECT = "BF6_Unreal_SDK.uproject"


def find_root(start: str | None = None) -> str:
    """Walk up until the uproject is found.

    Never hard code a user name: this tool is checked in and other machines put
    the project somewhere else entirely.
    """
    here = os.path.abspath(start or os.path.dirname(os.path.abspath(__file__)))
    while True:
        if os.path.isfile(os.path.join(here, UPROJECT)):
            return here
        parent = os.path.dirname(here)
        if parent == here:
            raise RuntimeError(
                "Could not find %s above %s. Run this from inside the project."
                % (UPROJECT, start or __file__))
        here = parent


ROOT = find_root()
PLUGIN = os.path.join(ROOT, "Plugins", "BF6UnrealSDK")
ADDON = os.path.join(ROOT, "Plugins", "Add-Ons", "BF6HighPoly")
RESOURCES = os.path.join(PLUGIN, "Resources")
OUT_DIR = os.path.join(ROOT, "Saved", "BF6Context")
OUT_FILE = os.path.join(OUT_DIR, "inventory.json")
MCP_FILE = os.path.join(ROOT, "Content", "Python", "bf6_water_mcp.py")

# Directories that hold generated or compiled copies of the same sources. Left
# in, every command would be found twice and the counts would be nonsense.
SKIP_DIRS = {"intermediate", "binaries", "saved", "deriveddatacache", "node_modules"}

# Extensions worth naming as resources. Resources also holds thumbnails and
# icons by the hundred; expanding a directory into those buries the page files
# that a person actually wants to see.
TEXTUAL_RES = {".js", ".css", ".html", ".htm", ".json", ".md", ".txt", ".ts"}

# Two file names do not follow the BF6<Name>.cpp rule but are still subsystems.
# SBF6PreviewViewport carries the Slate S prefix, and the viewport library was
# named for the class inside it rather than for the subsystem it is. The spec
# names the ids these two must produce, so they are corrected here rather than
# left to leak a wrong id into every command and gap report downstream.
ID_ALIASES = {
    "highpolyviewportlibrary": "highpolyviewport",
}


def rel(path: str) -> str:
    """Project relative, forward slashes. Every path in the json is one of these."""
    return os.path.relpath(path, ROOT).replace("\\", "/")


def read(path: str) -> str:
    # errors="replace" throughout: a single bad byte in one source must not take
    # the whole inventory down.
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# C++ literal scanning
# ---------------------------------------------------------------------------

_STRING = re.compile(r'"((?:[^"\\]|\\.)*)"')

_UNESCAPE = {
    "n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'", "0": "",
}


def _unescape(raw: str) -> str:
    out = []
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == "\\" and i + 1 < len(raw):
            out.append(_UNESCAPE.get(raw[i + 1], raw[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_text_macro(src: str, pos: int) -> tuple[str, int]:
    """Read one TEXT(...) starting at pos, returning its text and the end offset.

    C++ splits a long help string across lines as adjacent literals inside the
    one macro, TEXT("first " "second "), so taking only the first quoted run
    would truncate most help strings to their opening clause. Everything quoted
    inside the macro's parentheses is joined instead.
    """
    if not src.startswith("TEXT(", pos):
        return "", pos
    i = pos + len("TEXT(")
    depth = 1
    parts: list[str] = []
    while i < len(src) and depth > 0:
        c = src[i]
        if c == '"':
            m = _STRING.match(src, i)
            if not m:
                break
            parts.append(_unescape(m.group(1)))
            i = m.end()
            continue
        if c == "/" and src.startswith("//", i):
            i = src.find("\n", i)
            if i < 0:
                break
            continue
        if c == "/" and src.startswith("/*", i):
            i = src.find("*/", i)
            if i < 0:
                break
            i += 2
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return "".join(parts), i


# Both registration forms the tool uses. FAutoConsoleCommand declares a static
# object whose name sits between the type and the open paren; the console
# manager form has nothing between them.
_CMD_SITE = re.compile(
    r"\b(?:FAutoConsoleCommand\w*\s+\w+\s*|RegisterConsoleCommand\s*)\(\s*(?=TEXT\()")

_LOG_CAT = re.compile(r"\bDEFINE_LOG_CATEGORY(?:_STATIC)?\s*\(\s*(\w+)")

# C++ receiving. Every dispatcher in the tool binds the op field to a local
# called Op, so the variable name is part of the pattern on purpose: matching
# any string comparison at all would drag in half the file.
_OP_IN = re.compile(r"\bOp\s*(?:==\s*TEXT\(|\.Equals\s*\(\s*TEXT\()\"([A-Za-z0-9_.\-]+)\"")

# C++ sending, two shapes. The json object builder is the common one; a handful
# of places hand-write the json into a TEXT literal, where the quotes are
# escaped, and those ops are invisible to the first pattern.
_OP_OUT_FIELD = re.compile(
    r'SetStringField\s*\(\s*TEXT\("op"\)\s*,\s*TEXT\("([A-Za-z0-9_.\-]+)"\)')
_OP_OUT_RAW = re.compile(r'\\"op\\"\s*:\s*\\"([A-Za-z0-9_.\-]+)\\"')

# Resource paths are built with FPaths::Combine, so the folder and the file
# arrive as separate TEXT literals in one call rather than as one path string.
_RES_ANCHOR = re.compile(r'TEXT\("Resources"\)')
_RES_PATH = re.compile(r'TEXT\("(Resources/[A-Za-z0-9_./-]+)"\)')
_BARE_FILE = re.compile(r'TEXT\("([A-Za-z0-9_.-]+\.(?:js|css|html|md))"\)')


def line_of(src: str, offset: int) -> int:
    return src.count("\n", 0, offset) + 1


# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

def subsystem_id(filename: str) -> str | None:
    base = os.path.splitext(os.path.basename(filename))[0]
    if base.startswith("SBF6"):
        base = base[1:]
    if not base.startswith("BF6") or len(base) <= 3:
        return None
    ident = base[3:].lower()
    return ID_ALIASES.get(ident, ident)


def source_files() -> list[tuple[str, str, bool]]:
    """(path, subsystem id, is add-on) for every subsystem .cpp.

    Headers carry no registrations, ops or log categories anywhere in this tree,
    so scanning them would only add paths to the report without adding facts.
    """
    found: list[tuple[str, str, bool]] = []
    for base, addon in ((PLUGIN, False), (ADDON, True)):
        src_root = os.path.join(base, "Source")
        for dirpath, dirnames, filenames in os.walk(src_root):
            dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS]
            for name in filenames:
                if not name.endswith(".cpp"):
                    continue
                # Unity and generated files are copies of these same sources.
                if name.endswith(".gen.cpp") or name.startswith("Module."):
                    continue
                ident = subsystem_id(name)
                if not ident:
                    continue
                found.append((os.path.join(dirpath, name), ident, addon))
    found.sort(key=lambda t: (t[1], t[0]))
    return found


# ---------------------------------------------------------------------------
# Resources
# ---------------------------------------------------------------------------

def resource_index() -> tuple[list[str], dict[str, list[str]]]:
    """Every file under Resources, plus a basename lookup.

    The lookup exists because C++ often names only the file, TEXT("capture.js"),
    with the folder coming from a helper somewhere else in the file.

    Vendored third-party trees are kept out of the lookup. BF6Blocks names
    TEXT("typescript.js") for a path under Resources/convert/vendor that does
    not exist on this checkout, and the only typescript.js that does exist is
    Monaco's, four folders away under the script editor. Matching on the name
    alone handed the blocks subsystem a file belonging to nobody.
    """
    files: list[str] = []
    by_base: dict[str, list[str]] = {}
    for dirpath, dirnames, filenames in os.walk(RESOURCES):
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS]
        vendored = "/vendor/" in (rel(dirpath).replace("\\", "/") + "/")
        for name in filenames:
            path = rel(os.path.join(dirpath, name))
            files.append(path)
            if not vendored:
                by_base.setdefault(name.lower(), []).append(path)
    files.sort()
    return files, by_base


def _expand(path_rel: str, all_files: list[str], out: set[str]) -> None:
    """Add a Resources path, or if it names a folder, the textual files in it."""
    absolute = os.path.join(ROOT, path_rel.replace("/", os.sep))
    if os.path.isfile(absolute):
        out.add(path_rel)
        return
    if not os.path.isdir(absolute):
        return
    prefix = path_rel.rstrip("/") + "/"
    for f in all_files:
        if not f.startswith(prefix):
            continue
        if "/" in f[len(prefix):]:
            continue  # immediate children only; vendor subtrees are not the point
        if os.path.splitext(f)[1].lower() in TEXTUAL_RES:
            out.add(f)


def resources_for(src: str, ident: str, all_files: list[str],
                  by_base: dict[str, list[str]]) -> set[str]:
    found: set[str] = set()

    # A folder named for the subsystem. portalweb, portalprofile and
    # portalsettings all share Resources/portal, hence the prefix test.
    res_prefix = rel(RESOURCES) + "/"
    for f in all_files:
        top = f[len(res_prefix):].split("/")[0]
        if not top or "." in top or len(top) < 4:
            continue
        low = top.lower()
        if ident == low or ident.startswith(low):
            _expand(res_prefix + top, all_files, found)

    for m in _RES_PATH.finditer(src):
        _expand(m.group(1), all_files, found)

    # FPaths::Combine(ToolPluginDir(), TEXT("Resources"), TEXT("blocks"), ...)
    for m in _RES_ANCHOR.finditer(src):
        i = m.end()
        parts: list[str] = []
        while True:
            nxt = re.compile(r"\s*,\s*").match(src, i)
            if not nxt:
                break
            j = nxt.end()
            if not src.startswith("TEXT(", j):
                break
            text, i = read_text_macro(src, j)
            if not text or "\\" in text or " " in text:
                break
            parts.append(text)
            _expand("Resources/" + "/".join(parts), all_files, found)
            if os.path.splitext(text)[1]:
                break

    for m in _BARE_FILE.finditer(src):
        hits = by_base.get(m.group(1).lower(), [])
        if len(hits) == 1:  # an ambiguous basename proves nothing
            found.add(hits[0])

    return found


# ---------------------------------------------------------------------------
# MCP toolset cross reference
# ---------------------------------------------------------------------------

_TOOL_DEF = re.compile(r"tool_call\b.*?\n\s*(?:@staticmethod\s*\n\s*)?def\s+(\w+)",
                       re.S)


def mcp_tools_by_subsystem(commands: dict[str, str],
                           idents: list[str]) -> dict[str, list[str]]:
    """Attach each MCP tool to a subsystem, on evidence rather than on a guess.

    Two kinds of evidence are accepted: the tool mentions a console command
    whose owner is known, or the tool's name and text carry the subsystem's own
    compound name. A tool that shows neither is left unattached, because a wrong
    owner here would send someone to the wrong file, which is the exact cost
    this whole tool exists to remove.

    Name evidence is only allowed to claim an add-on subsystem, and only through
    a compound form. The first cut let any subsystem match any word: "project"
    claimed three tools because unreal.Paths.project_dir mentions it, and
    "viewport", left over from highpolyviewportlibrary, claimed twenty-three
    because a viewport toolset naturally talks about viewports. Neither was a
    fact about who owns the tool.
    """
    out: dict[str, list[str]] = {i: [] for i in idents}
    if not os.path.isfile(MCP_FILE):
        return out
    text = read(MCP_FILE)

    tokens: dict[str, set[str]] = {}
    for ident in idents:
        if not ident.startswith("highpoly"):
            tokens[ident] = set()
            continue
        forms: set[str] = set()
        if ident == "highpoly":
            # The add-on module itself, spelled both ways python spells it.
            forms |= {"highpoly", "high_poly", "high poly"}
        else:
            residual = ident[len("highpoly"):]
            for base in (ident, residual):
                parts = re.sub(r"(highpoly|water|ground|game|disk|ui)",
                               r"\1 ", base).split()
                if len(parts) > 1:
                    forms.add("_".join(parts))
                    forms.add(" ".join(parts))
                elif len(base) >= 9:
                    forms.add(base)
        tokens[ident] = forms

    starts = [(m.start(), m.group(1)) for m in _TOOL_DEF.finditer(text)]
    for n, (start, name) in enumerate(starts):
        end = starts[n + 1][0] if n + 1 < len(starts) else len(text)
        body = text[start:end]
        low = (name + "\n" + body).lower()
        owners: set[str] = set()
        for cmd in re.findall(r"\bBF6\.[A-Za-z][A-Za-z0-9_.]*", body):
            owner = commands.get(cmd) or commands.get(cmd.rstrip("."))
            if owner:
                owners.add(owner)
        for ident, forms in tokens.items():
            if any(f in low for f in forms):
                owners.add(ident)
        for owner in owners:
            out.setdefault(owner, []).append(name)
    for ident in out:
        out[ident] = sorted(set(out[ident]))
    return out


# ---------------------------------------------------------------------------
# Page side ops
# ---------------------------------------------------------------------------

_JS_SEND = re.compile(r"\bop\s*:\s*['\"]([A-Za-z0-9_.\-]+)['\"]")
_JS_RECV_CMP = re.compile(r"\.op\s*===?\s*['\"]([A-Za-z0-9_.\-]+)['\"]")
_JS_SWITCH = re.compile(r"switch\s*\(\s*\w+\.op\s*\)\s*\{")
_JS_CASE = re.compile(r"\bcase\s+['\"]([A-Za-z0-9_.\-]+)['\"]\s*:")


def page_ops() -> dict[str, dict[str, list[str]]]:
    """{resource path: {"sends": [...], "receives": [...]}} for Resources/**/*.js.

    Not part of inventory.json, whose web_ops describe the C++ end. It is here
    so gaps.py can import it and compare the two ends without writing a second
    javascript parser.
    """
    out: dict[str, dict[str, list[str]]] = {}
    for dirpath, dirnames, filenames in os.walk(RESOURCES):
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS
                       and d.lower() != "vendor"]
        for name in filenames:
            if not name.endswith(".js"):
                continue
            path = os.path.join(dirpath, name)
            src = read(path)
            sends = set(_JS_SEND.findall(src))
            recvs = set(_JS_RECV_CMP.findall(src))
            for m in _JS_SWITCH.finditer(src):
                depth = 0
                i = m.end() - 1
                while i < len(src):
                    if src[i] == "{":
                        depth += 1
                    elif src[i] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    i += 1
                for c in _JS_CASE.finditer(src, m.end(), i):
                    recvs.add(c.group(1))
            if sends or recvs:
                out[rel(path)] = {"sends": sorted(sends), "receives": sorted(recvs)}
    return out


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def build() -> dict:
    all_res, by_base = resource_index()
    subs: dict[str, dict] = {}
    commands: dict[str, str] = {}
    log_categories: dict[str, str] = {}

    for path, ident, addon in source_files():
        src = read(path)
        entry = subs.setdefault(ident, {
            "addon": addon,
            "sources": [],
            "log_categories": [],
            "commands": [],
            "web_ops": {"in": [], "out": []},
            "resources": [],
            "docs": [],
            "mcp_tools": [],
        })
        entry["sources"].append(rel(path))

        for m in _CMD_SITE.finditer(src):
            name, after = read_text_macro(src, m.end())
            if not name:
                continue
            help_text = ""
            comma = re.compile(r"\s*,\s*").match(src, after)
            if comma and src.startswith("TEXT(", comma.end()):
                help_text, _ = read_text_macro(src, comma.end())
            entry["commands"].append({
                "name": name,
                "help": help_text.strip(),
                "file": rel(path),
                "line": line_of(src, m.end()),
            })
            commands[name] = ident

        for m in _LOG_CAT.finditer(src):
            cat = m.group(1)
            if cat not in entry["log_categories"]:
                entry["log_categories"].append(cat)
            log_categories[cat] = ident

        ops_in = set(entry["web_ops"]["in"]) | set(_OP_IN.findall(src))
        ops_out = set(entry["web_ops"]["out"])
        ops_out |= set(_OP_OUT_FIELD.findall(src))
        ops_out |= set(_OP_OUT_RAW.findall(src))
        entry["web_ops"]["in"] = sorted(ops_in)
        entry["web_ops"]["out"] = sorted(ops_out)

        res = set(entry["resources"]) | set(entry["docs"])
        res |= resources_for(src, ident, all_res, by_base)
        docs = {r for r in res if r.lower().endswith(".md")}
        # A markdown file sitting beside a resource this subsystem uses is its
        # documentation even when no source ever names the file.
        for r in sorted(res - docs):
            folder = r.rsplit("/", 1)[0] + "/"
            for f in all_res:
                if f.startswith(folder) and "/" not in f[len(folder):] \
                        and f.lower().endswith(".md"):
                    docs.add(f)
        entry["resources"] = sorted(r for r in res if r not in docs)
        entry["docs"] = sorted(docs)

    for ident, sub in subs.items():
        sub["commands"].sort(key=lambda c: (c["name"], c["line"]))
        sub["sources"].sort()

    for ident, tools in mcp_tools_by_subsystem(commands, sorted(subs)).items():
        if ident in subs:
            subs[ident]["mcp_tools"] = tools

    # Nothing about a conflict is written to the json: the flat commands and
    # log_categories maps are one owner per name by definition, so a name two
    # subsystems both register would silently lose one owner there. It is
    # recovered from the per subsystem lists instead, which keep both.
    return {
        "generated": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "subsystems": dict(sorted(subs.items())),
        "commands": dict(sorted(commands.items())),
        "log_categories": dict(sorted(log_categories.items())),
    }


def conflicts(data: dict) -> dict[str, dict[str, list[str]]]:
    """Names more than one subsystem claims. Empty is the expected answer."""
    cmds: dict[str, set[str]] = {}
    cats: dict[str, set[str]] = {}
    for ident, sub in data["subsystems"].items():
        for c in sub["commands"]:
            cmds.setdefault(c["name"], set()).add(ident)
        for cat in sub["log_categories"]:
            cats.setdefault(cat, set()).add(ident)
    return {
        "commands": {k: sorted(v) for k, v in sorted(cmds.items()) if len(v) > 1},
        "log_categories": {k: sorted(v) for k, v in sorted(cats.items()) if len(v) > 1},
    }


def write(data: dict) -> str:
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_FILE, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, sort_keys=False)
        fh.write("\n")
    return OUT_FILE


def load(rebuild: bool = True) -> dict:
    """The entry point gaps.py and the MCP toolset use."""
    if rebuild or not os.path.isfile(OUT_FILE):
        data = build()
        write(data)
        return data
    with open(OUT_FILE, "r", encoding="utf-8", errors="replace") as fh:
        return json.load(fh)


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def print_summary(data: dict, path: str) -> None:
    subs = data["subsystems"]
    total_cmds = sum(len(s["commands"]) for s in subs.values())
    print("BF6 inventory  %s" % data["generated"])
    print("root      %s" % ROOT)
    print("written   %s" % rel(path))
    print("")
    print("%-22s %-5s %5s %5s %5s %5s %5s %5s" %
          ("subsystem", "side", "cmds", "logs", "opIn", "opOut", "res", "mcp"))
    print("-" * 70)
    for ident in sorted(subs, key=lambda i: (subs[i]["addon"], i)):
        s = subs[ident]
        print("%-22s %-5s %5d %5d %5d %5d %5d %5d" % (
            ident,
            "addon" if s["addon"] else "base",
            len(s["commands"]),
            len(s["log_categories"]),
            len(s["web_ops"]["in"]),
            len(s["web_ops"]["out"]),
            len(s["resources"]) + len(s["docs"]),
            len(s["mcp_tools"]),
        ))
    print("-" * 70)
    print("%-22s %-5s %5d %5d" % ("total", "", total_cmds, len(data["log_categories"])))
    print("")
    print("%d subsystems, %d console commands, %d log categories"
          % (len(subs), total_cmds, len(data["log_categories"])))

    if total_cmds < 140:
        print("")
        print("WARNING: only %d commands found. The tool registers about 145, so a "
              "registration form is being missed and this inventory is wrong."
              % total_cmds)

    clash = conflicts(data)
    for kind in ("commands", "log_categories"):
        bad = clash.get(kind, {})
        if bad:
            print("")
            print("%s claimed by more than one subsystem:" % kind)
            for name, owners in bad.items():
                print("  %s  %s" % (name, ", ".join(owners)))

    empty = [i for i in sorted(subs) if not subs[i]["commands"]]
    if empty:
        print("")
        print("no commands: %s" % ", ".join(empty))


def print_subsystem(data: dict, ident: str) -> int:
    subs = data["subsystems"]
    if ident not in subs:
        near = [i for i in sorted(subs) if ident in i]
        print("No subsystem '%s'." % ident)
        if near:
            print("Did you mean: %s" % ", ".join(near))
        else:
            print("Known: %s" % ", ".join(sorted(subs)))
        return 2
    s = subs[ident]
    print("%s  (%s)" % (ident, "add-on" if s["addon"] else "base tool"))
    print("")
    for label, key in (("sources", "sources"), ("log categories", "log_categories")):
        print("%s:" % label)
        for v in s[key] or ["(none)"]:
            print("  %s" % v)
        print("")
    print("commands (%d):" % len(s["commands"]))
    for c in s["commands"]:
        print("  %-42s %s:%d" % (c["name"], c["file"].rsplit("/", 1)[-1], c["line"]))
        if c["help"]:
            one = " ".join(c["help"].split())
            print("      %s" % (one if len(one) <= 150 else one[:147] + "..."))
    if not s["commands"]:
        print("  (none)")
    print("")
    print("web ops in (page to C++)  (%d): %s"
          % (len(s["web_ops"]["in"]), ", ".join(s["web_ops"]["in"]) or "(none)"))
    print("web ops out (C++ to page) (%d): %s"
          % (len(s["web_ops"]["out"]), ", ".join(s["web_ops"]["out"]) or "(none)"))
    print("")
    print("resources (%d):" % len(s["resources"]))
    for r in s["resources"] or []:
        print("  %s" % r)
    if not s["resources"]:
        print("  (none)")
    print("")
    print("docs (%d):" % len(s["docs"]))
    for d in s["docs"] or []:
        print("  %s" % d)
    if not s["docs"]:
        print("  (none)")
    print("")
    print("mcp tools (%d): %s"
          % (len(s["mcp_tools"]), ", ".join(s["mcp_tools"]) or "(none)"))
    return 0


def print_command(data: dict, name: str) -> int:
    owner = data["commands"].get(name)
    if not owner:
        # Console commands are compared case insensitively by Unreal, and the
        # tool registers one of its own in lower case, so a case miss here would
        # be a false negative.
        for cmd, sub in data["commands"].items():
            if cmd.lower() == name.lower():
                owner, name = sub, cmd
                break
    if not owner:
        near = [c for c in sorted(data["commands"]) if name.lower() in c.lower()]
        print("No command '%s'." % name)
        if near:
            print("Close: %s" % ", ".join(near[:20]))
        return 2
    entry = None
    for c in data["subsystems"][owner]["commands"]:
        if c["name"] == name:
            entry = c
            break
    print(owner)
    if entry:
        print("%s  %s:%d" % (entry["name"], entry["file"], entry["line"]))
        if entry["help"]:
            print(" ".join(entry["help"].split()))
    return 0


def main(argv: list[str]) -> int:
    args = argv[1:]
    if args and args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if args and args[0] == "--command":
        if len(args) < 2:
            print("usage: inventory.py --command BF6.Portal.Link")
            return 2
        return print_command(load(), args[1])
    data = load()
    if args:
        return print_subsystem(data, args[0].lower())
    print_summary(data, OUT_FILE)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
