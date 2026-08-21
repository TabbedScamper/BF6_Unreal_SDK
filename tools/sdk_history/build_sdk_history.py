# Builds the Portal SDK version history the tool ships and extends.
#
# For every SDK version on the community archive (hoard.bfportal.gg), this
# fetches ONLY the zip's central directory plus a handful of small high-value
# files via HTTP range requests - no 3 GB downloads - then diffs consecutive
# versions into sdk_history.json (machine) and SDK-HISTORY.md (human).
#
# The signals mirror the proven release-day pipeline:
#   - manifest added/removed (changed is mostly regeneration noise)
#   - placeable types + level restrictions (FbExportData/asset_types.json)
#   - maps (FbExportData/level_info.json, levels/*.tscn)
#   - scripting API surface (code/types/mod/index.d.ts export names)
#   - playable bounds (addons/bf_portal/terrain_decal/bounds.json)
#
# Usage:  python build_sdk_history.py [workdir]
# Output: <workdir>/sdk_history.json + SDK-HISTORY.md
#         (bake both into Plugins/BF6UnrealSDK/Resources/sdkhistory/)

import io
import json
import os
import re
import struct
import sys
import urllib.request
import zlib

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) BF6UnrealSDK"
HOARD = "https://hoard.bfportal.gg"

TARGET_FILES = [
    "FbExportData/asset_types.json",
    "FbExportData/level_info.json",
    "code/types/mod/index.d.ts",
    "GodotProject/addons/bf_portal/terrain_decal/bounds.json",
    "sdk.version.json",
]

# manifest noise filters, straight from the release-day pipeline
NOISE_PREFIXES = ("python/", "FbExportData/thumbnails/", "GodotProject/.godot/")
INTERESTING_EXT = (".d.ts", ".ts", ".json", ".md", ".html", ".tscn", ".gd", ".cfg", ".py", ".glb")


def http(url, rng=None):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    if rng:
        req.add_header("Range", "bytes=%d-%d" % rng)
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def u16(b, i): return struct.unpack_from("<H", b, i)[0]
def u32(b, i): return struct.unpack_from("<I", b, i)[0]
def u64(b, i): return struct.unpack_from("<Q", b, i)[0]


def central_directory(url, total_size):
    """Locate and fetch the central directory via ranged reads (zip64 aware)."""
    tail = http(url, (max(0, total_size - 130 * 1024), total_size - 1))
    i = tail.rfind(b"PK\x05\x06")
    if i < 0:
        raise RuntimeError("no EOCD in tail")
    cd_size = u32(tail, i + 12)
    cd_off = u32(tail, i + 16)
    count = u16(tail, i + 10)
    if cd_off == 0xFFFFFFFF or count == 0xFFFF or cd_size == 0xFFFFFFFF:
        j = tail.rfind(b"PK\x06\x06", 0, i)
        if j < 0:
            raise RuntimeError("zip64 EOCD missing")
        count = u64(tail, j + 32)
        cd_size = u64(tail, j + 40)
        cd_off = u64(tail, j + 48)
    return http(url, (cd_off, cd_off + cd_size - 1)), count


def parse_entries(cd):
    """Yield (name, crc, comp_size, uncomp_size, method, local_offset)."""
    i = 0
    out = []
    while i + 4 <= len(cd) and cd[i:i + 4] == b"PK\x01\x02":
        method = u16(cd, i + 10)
        crc = u32(cd, i + 16)
        csize = u32(cd, i + 20)
        usize = u32(cd, i + 24)
        nlen = u16(cd, i + 28)
        elen = u16(cd, i + 30)
        clen = u16(cd, i + 32)
        off = u32(cd, i + 42)
        name = cd[i + 46:i + 46 + nlen].decode("utf-8", "replace")
        # zip64 extra: any 0xFFFFFFFF field is continued in extra id 0x0001
        if 0xFFFFFFFF in (csize, usize, off):
            e = i + 46 + nlen
            end = e + elen
            while e + 4 <= end:
                eid = u16(cd, e)
                esz = u16(cd, e + 2)
                if eid == 0x0001:
                    p = e + 4
                    if usize == 0xFFFFFFFF:
                        usize = u64(cd, p); p += 8
                    if csize == 0xFFFFFFFF:
                        csize = u64(cd, p); p += 8
                    if off == 0xFFFFFFFF:
                        off = u64(cd, p); p += 8
                    break
                e += 4 + esz
        out.append((name, crc, csize, usize, method, off))
        i += 46 + nlen + elen + clen
    return out


def fetch_file(url, entry):
    """Ranged fetch of one entry: local header + compressed bytes, inflated."""
    name, crc, csize, usize, method, off = entry
    head = http(url, (off, off + 29))
    if head[:4] != b"PK\x03\x04":
        raise RuntimeError("bad local header for " + name)
    nlen = u16(head, 26)
    elen = u16(head, 28)
    data = http(url, (off + 30 + nlen + elen, off + 30 + nlen + elen + csize - 1))
    if method == 0:
        return data
    if method == 8:
        return zlib.decompress(data, -15)
    raise RuntimeError("method %d for %s" % (method, name))


def strip_root(names):
    """Some zips wrap everything in a single root folder - normalize it away."""
    roots = {n.split("/", 1)[0] for n in names if "/" in n}
    if len(roots) == 1 and all("/" in n or n.rstrip("/") in roots for n in names):
        root = next(iter(roots)) + "/"
        return {n[len(root):] if n.startswith(root) else n for n in names}, root
    return set(names), ""


def stage_version(work, ver, key, size):
    vdir = os.path.join(work, ver)
    man = os.path.join(vdir, "manifest.tsv")
    if os.path.exists(man):
        return
    os.makedirs(vdir, exist_ok=True)
    url = HOARD + "/" + key
    print("fetching central directory for", ver, flush=True)
    cd, count = central_directory(url, size)
    entries = parse_entries(cd)
    print("  %d entries" % len(entries), flush=True)
    with io.open(man + ".tmp", "w", encoding="utf-8", newline="\n") as f:
        for (name, crc, csize, usize, method, off) in entries:
            f.write("%08x\t%d\t%s\n" % (crc, usize, name))
    by_name = {}
    root_guess = ""
    names = [e[0] for e in entries]
    _, root_guess = strip_root(names)
    for e in entries:
        by_name[e[0]] = e
    for want in TARGET_FILES:
        entry = by_name.get(want) or by_name.get(root_guess + want)
        if not entry or entry[3] == 0:
            continue
        try:
            data = fetch_file(url, entry)
        except Exception as ex:
            print("  ! %s: %s" % (want, ex), flush=True)
            continue
        dest = os.path.join(vdir, want.replace("/", "__"))
        with open(dest, "wb") as f:
            f.write(data)
        print("  + %s (%d B)" % (want, len(data)), flush=True)
    os.replace(man + ".tmp", man)


def load_manifest(work, ver):
    out = {}
    with io.open(os.path.join(work, ver, "manifest.tsv"), encoding="utf-8") as f:
        for line in f:
            crc, size, name = line.rstrip("\n").split("\t", 2)
            out[name] = (crc, int(size))
    names, root = strip_root(set(out))
    if root:
        out = {(k[len(root):] if k.startswith(root) else k): v for k, v in out.items()}
    return {k: v for k, v in out.items() if k and not k.endswith("/")}


def load_json(work, ver, want):
    p = os.path.join(work, ver, want.replace("/", "__"))
    if not os.path.exists(p):
        return None
    try:
        with open(p, "rb") as f:
            return json.loads(f.read().decode("utf-8-sig", "replace"))
    except Exception:
        return None


def load_text(work, ver, want):
    p = os.path.join(work, ver, want.replace("/", "__"))
    if not os.path.exists(p):
        return None
    with open(p, "rb") as f:
        return f.read().decode("utf-8", "replace")


API_RE = re.compile(r"^\s*export\s+(?:declare\s+)?(function|enum|const|class|interface|type)\s+([A-Za-z0-9_]+)", re.M)


def api_surface(text):
    if not text:
        return {}
    out = {}
    for kind, name in API_RE.findall(text):
        out.setdefault(kind, set()).add(name)
    return out


def interesting(path):
    if path.startswith(NOISE_PREFIXES):
        return False
    return path.endswith(INTERESTING_EXT)


def type_names(aj):
    """asset_types.json: {"AssetTypes": [{"type": Name, ...}]}."""
    if aj is None:
        return {}
    rows = aj.get("AssetTypes") if isinstance(aj, dict) else None
    if isinstance(rows, list):
        return {t.get("type") or t.get("name") or "": t for t in rows if isinstance(t, dict)}
    if isinstance(aj, dict):
        return {k: v for k, v in aj.items() if isinstance(v, dict)}
    return {}


def map_names(lj):
    if lj is None:
        return set()
    if isinstance(lj, dict) and isinstance(lj.get("levels"), list):
        return {l.get("name") or l.get("level") or "" for l in lj["levels"] if isinstance(l, dict)}
    if isinstance(lj, dict):
        return set(lj.keys())
    if isinstance(lj, list):
        return {l.get("name") or l.get("level") or "" for l in lj if isinstance(l, dict)}
    return set()


def diff_versions(work, old, new, meta):
    m_old = load_manifest(work, old)
    m_new = load_manifest(work, new)
    added = sorted(set(m_new) - set(m_old))
    removed = sorted(set(m_old) - set(m_new))
    d = {
        "version": new,
        "previous": old,
        "released": meta.get("lastModified", ""),
        "zip_bytes": meta.get("fileSize", 0),
        "entries": len(m_new),
        "entries_added": len(added),
        "entries_removed": len(removed),
        "added_interesting": [p for p in added if interesting(p)][:400],
        "removed_interesting": [p for p in removed if interesting(p)][:400],
        "models_added": sorted(p.rsplit("/", 1)[-1][:-4] for p in added if p.startswith("GodotProject/raw/models/") and p.endswith(".glb")),
        "models_removed": sorted(p.rsplit("/", 1)[-1][:-4] for p in removed if p.startswith("GodotProject/raw/models/") and p.endswith(".glb")),
        "levels_added": sorted(p.rsplit("/", 1)[-1][:-5] for p in added if re.match(r"GodotProject/levels/[^/]+\.tscn$", p)),
        "levels_removed": sorted(p.rsplit("/", 1)[-1][:-5] for p in removed if re.match(r"GodotProject/levels/[^/]+\.tscn$", p)),
        "scripts_changed": sorted(p for p in set(m_old) & set(m_new)
                                  if p.startswith(("GodotProject/scripts/", "GodotProject/addons/bf_portal/"))
                                  and p.endswith(".gd") and m_old[p] != m_new[p]),
    }
    # placeable types
    t_old = type_names(load_json(work, old, "FbExportData/asset_types.json"))
    t_new = type_names(load_json(work, new, "FbExportData/asset_types.json"))
    if t_old or t_new:
        d["types_added"] = sorted(set(t_new) - set(t_old))
        d["types_removed"] = sorted(set(t_old) - set(t_new))
    # maps
    l_old = map_names(load_json(work, old, "FbExportData/level_info.json"))
    l_new = map_names(load_json(work, new, "FbExportData/level_info.json"))
    if l_old or l_new:
        d["maps_added"] = sorted(x for x in l_new - l_old if x)
        d["maps_removed"] = sorted(x for x in l_old - l_new if x)
    # scripting API
    a_old = api_surface(load_text(work, old, "code/types/mod/index.d.ts"))
    a_new = api_surface(load_text(work, new, "code/types/mod/index.d.ts"))
    if a_old or a_new:
        api = {}
        for kind in sorted(set(a_old) | set(a_new)):
            plus = sorted(a_new.get(kind, set()) - a_old.get(kind, set()))
            minus = sorted(a_old.get(kind, set()) - a_new.get(kind, set()))
            if plus or minus:
                api[kind] = {"added": plus, "removed": minus}
        d["api"] = api
    # bounds (new-map dimensions)
    b_old = load_json(work, old, "GodotProject/addons/bf_portal/terrain_decal/bounds.json")
    b_new = load_json(work, new, "GodotProject/addons/bf_portal/terrain_decal/bounds.json")
    if isinstance(b_new, dict):
        if not isinstance(b_old, dict):
            d["bounds_note"] = "playable bounds catalog introduced (%d maps)" % len(b_new)
        else:
            nb = {k: b_new[k] for k in sorted(set(b_new) - set(b_old))}
            if nb:
                d["bounds_added"] = nb
    return d


def to_markdown(history):
    out = ["# Portal SDK version history", "",
           "Generated from the community SDK archive (hoard.bfportal.gg) by",
           "tools/sdk_history/build_sdk_history.py. Manifest added/removed is the",
           "signal; changed counts are mostly regeneration noise.", ""]
    for d in reversed(history):
        out.append("## %s  (released %s)" % (d["version"], (d.get("released") or "?")[:10]))
        out.append("")
        out.append("- Archive: %.2f GB, %d entries (%+d added, %-d removed vs %s)" % (
            d.get("zip_bytes", 0) / 1e9, d.get("entries", 0), d.get("entries_added", 0),
            d.get("entries_removed", 0), d.get("previous", "?")))
        for key, label in (("maps_added", "New maps"), ("maps_removed", "Maps removed"),
                           ("levels_added", "New level scenes"), ("levels_removed", "Level scenes removed")):
            if d.get(key):
                out.append("- %s: %s" % (label, ", ".join(d[key])))
        for key, label in (("types_added", "New placeable types"), ("types_removed", "Placeable types removed")):
            if d.get(key):
                t = d[key]
                shown = ", ".join(t[:30]) + (", ... and %d more" % (len(t) - 30) if len(t) > 30 else "")
                out.append("- %s: %d (%s)" % (label, len(t), shown))
        if d.get("models_added"):
            m = d["models_added"]
            out.append("- New models: %d%s" % (len(m), " (" + ", ".join(m[:12]) + (", ..." if len(m) > 12 else "") + ")"))
        if d.get("models_removed"):
            out.append("- Models removed: %s" % ", ".join(d["models_removed"][:20]))
        api = d.get("api") or {}
        for kind, ch in api.items():
            if ch.get("added"):
                out.append("- New script %ss: %s" % (kind, ", ".join(ch["added"][:40])))
            if ch.get("removed"):
                out.append("- Script %ss removed: %s" % (kind, ", ".join(ch["removed"][:40])))
        if d.get("bounds_note"):
            out.append("- " + d["bounds_note"])
        if d.get("bounds_added"):
            for k, v in d["bounds_added"].items():
                out.append("- Playable bounds added for %s: %s" % (k, json.dumps(v)))
        if d.get("scripts_changed"):
            s = d["scripts_changed"]
            out.append("- Entity scripts changed: %d%s" % (len(s), " (" + ", ".join(x.rsplit("/", 1)[-1] for x in s[:10]) + (", ..." if len(s) > 10 else "") + ")"))
        out.append("")
    return "\n".join(out)


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "work")
    os.makedirs(work, exist_ok=True)
    versions = json.loads(http(HOARD + "/versions.json").decode("utf-8"))["versions"]
    versions.sort(key=lambda v: [int(x) for x in v["version"].split(".")])
    for v in versions:
        try:
            stage_version(work, v["version"], v["key"], v["fileSize"])
        except Exception as ex:
            print("FAILED %s: %s" % (v["version"], ex), flush=True)
    history = []
    for prev, cur in zip(versions, versions[1:]):
        if not (os.path.exists(os.path.join(work, prev["version"], "manifest.tsv"))
                and os.path.exists(os.path.join(work, cur["version"], "manifest.tsv"))):
            continue
        print("diffing %s -> %s" % (prev["version"], cur["version"]), flush=True)
        history.append(diff_versions(work, prev["version"], cur["version"], cur))
    with io.open(os.path.join(work, "sdk_history.json"), "w", encoding="utf-8") as f:
        json.dump({"generated_from": HOARD, "history": history}, f, indent=1)
    with io.open(os.path.join(work, "SDK-HISTORY.md"), "w", encoding="utf-8") as f:
        f.write(to_markdown(history))
    print("wrote sdk_history.json + SDK-HISTORY.md with %d version diffs" % len(history), flush=True)


if __name__ == "__main__":
    main()
