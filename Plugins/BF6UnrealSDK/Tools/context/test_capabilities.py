"""Proof that the capability miner never turns a gap in a scan into a removal.

THE RULE THIS DEFENDS.

The miner compares two snapshots of one source. A key that was in the old one
and is not in the new one has two possible meanings, and they are nothing alike:

  the source dropped it            -> "removed", worth acting on
  we did not manage to look        -> "not observed", worth re-scanning

A settings page whose dropdowns never mounted, a route the sweep did not reach,
an SDK file that failed to download: each makes a whole scope look deleted. A
miner that reported those as removals would be worse than no miner, because it
would be confidently wrong exactly when the user most needs to trust it.

So removal is claimed ONLY when the scope was collected completely in BOTH
snapshots. Everything else is a gap. These tests run the real comparison out of
the MCP module so the Python side and the C++ side cannot drift apart silently.

TWO MORE RULES ARE DEFENDED HERE.

Identity is not history. A snapshot is stored under a hash of its contents, so
scanning A, then B, then A again stores two things and happens three times. The
store used to be read by listing those two directories, which lost the third
scan and reported B as current with the timestamp A was first seen at. What is
current, and what changed, come from the append-only observation log.

Evidence moves in both directions. A capability found in a running game and not
found in the next scan is the most important thing this tool can report, and it
used to be reported by neither side: the C++ compared only upward, and Python
ignored evidence altogether. "not observed" is a transition and not a rung, so
it is never ordered against one.

Run:  python test_capabilities.py
"""

import json
import sys
import tempfile
from pathlib import Path

# The MCP module imports `unreal`, which only exists inside the editor. The
# comparison helpers do not touch it, so a stub lets them be tested offline.
import ast  # noqa: E402

HERE = Path(__file__).resolve().parent
# Walk up to the project root rather than counting directories: this file sits
# four levels down today and counting broke the moment that was true.
PROJECT = next((p for p in HERE.parents if (p / "Content" / "Python").is_dir()), None)
if PROJECT is None:
    print("could not find the project root above " + str(HERE))
    sys.exit(1)

# The MCP module cannot be imported outside the editor: it registers its toolset
# at import time against unreal.uclass, which only exists in a running editor.
# Stubbing all of that would be a lot of fiction to maintain for four pure
# functions. So the real function bodies are lifted out by name and executed
# here, which is the same thing the JavaScript suites do and keeps the test
# pinned to the shipped source rather than to a copy of it.
_SRC = (PROJECT / "Content" / "Python" / "bf6_context_mcp.py").read_text(encoding="utf-8")
_TREE = ast.parse(_SRC)
_WANT = {"_caps_root", "_caps_list", "_caps_load", "_caps_observations",
         "_caps_scope_complete", "_caps_diff"}
_PICKED = [n for n in _TREE.body
           if isinstance(n, ast.FunctionDef) and n.name in _WANT]
_MISSING = _WANT - {n.name for n in _PICKED}
if _MISSING:
    print("these functions are no longer in bf6_context_mcp.py: " + ", ".join(sorted(_MISSING)))
    sys.exit(1)


class _M:
    pass


M = _M()
_NS = {"json": __import__("json"), "Path": Path, "_root": lambda: PROJECT}
exec(compile(ast.Module(body=_PICKED, type_ignores=[]), "bf6_context_mcp.py", "exec"), _NS)
for _name in _WANT:
    setattr(M, _name, _NS[_name])


def snap(records, scopes):
    return {"records": records, "scopes": scopes}


def rec(key, shape, scope, kind="function", evidence="structure"):
    return {"key": key, "kind": kind, "shape": shape, "scope": scope,
            "evidence": evidence}


def one(key, shape, evidence="structure"):
    """A one-record snapshot in a completed scope, for evidence and shape cases."""
    return snap([rec(key, shape, "api", evidence=evidence)],
                [{"name": "api", "state": "complete"}])


def write_store(root, source, scans):
    """Lay out a capability store on disk exactly as the C++ writes one.

    `scans` is the sequence of content ids observed, in order, so ("A", "B",
    "A") is the case the content-addressed store could not represent: two stored
    contents, three scans, and the newest state is the one seen first.
    """
    caps = root / "Saved" / "BF6UnrealSDK" / "capabilities"
    for seq, content in enumerate(scans, start=1):
        blob = caps / "snapshots" / source / content
        blob.mkdir(parents=True, exist_ok=True)
        marker = blob / "snapshot.json"
        if not marker.exists():
            # The blob keeps the time this content was FIRST stored, which is
            # why it can never stand in for the time of the latest scan.
            marker.write_text(json.dumps({
                "id": content, "source": source, "build": content,
                "capturedUtc": f"2026-09-07T00:0{seq}:00Z",
                "scopes": [{"name": "api", "state": "complete"}],
                "records": [{"key": content, "kind": "function", "scope": "api",
                             "shape": content + "()", "evidence": "structure"}],
            }), encoding="utf-8")
        obs = caps / "observations" / source
        obs.mkdir(parents=True, exist_ok=True)
        (obs / f"obs_{seq:08d}.json").write_text(json.dumps({
            "seq": seq, "source": source, "contentId": content,
            "observedUtc": f"2026-09-07T12:0{seq}:00Z",
            "context": f"scan {seq}",
            "scopes": [{"name": "api", "state": "complete"}],
        }), encoding="utf-8")
    return caps


def main():
    fails = []
    checks = 0

    def check(cond, what):
        nonlocal checks
        checks += 1
        if not cond:
            fails.append(what)

    complete = [{"name": "api", "state": "complete"}]
    partial = [{"name": "api", "state": "partial", "why": "a cap cut it short"}]
    failed = [{"name": "api", "state": "failed", "why": "the file would not load"}]

    # 1. THE REMOVAL CASE. Both scans finished the scope, so absence is real.
    old = snap([rec("A", "a()", "api"), rec("B", "b()", "api")], complete)
    new = snap([rec("A", "a()", "api")], complete)
    got = M._caps_diff(old, new)
    check(len(got) == 1 and got[0]["what"] == "removed",
          "a key absent from a scope completed twice should be removed, got " + str(got))

    # 2. THE GAP CASES. Same missing key, but the new scan did not finish.
    for name, scopes in (("partial", partial), ("failed", failed)):
        got = M._caps_diff(old, snap([rec("A", "a()", "api")], scopes))
        check(len(got) == 1 and got[0]["what"] == "not observed",
              f"a key missing from a {name} scope must be 'not observed', got {got}")
        check("not a removal" in got[0].get("why", ""),
              f"the {name} case should explain itself, got {got[0].get('why')!r}")

    # 3. A scope missing from the new snapshot entirely is also a gap, not a
    #    purge. This is the "route not observed" case.
    got = M._caps_diff(old, snap([], []))
    check(all(c["what"] == "not observed" for c in got),
          "an unvisited scope must not report its whole contents as removed, got " + str(got))

    # 4. ADDED, and only when genuinely new.
    got = M._caps_diff(snap([rec("A", "a()", "api")], complete),
                       snap([rec("A", "a()", "api"), rec("C", "c()", "api")], complete))
    check(len(got) == 1 and got[0]["what"] == "added" and got[0]["key"] == "C",
          "a new key should be added, got " + str(got))

    # 5. CHANGED means the shape moved. This is the case the old name-only
    #    comparison could not see at all: same name, different signature.
    got = M._caps_diff(snap([rec("A", "a(x: number): void", "api")], complete),
                       snap([rec("A", "a(x: number, y: number): void", "api")], complete))
    check(len(got) == 1 and got[0]["what"] == "changed",
          "a changed signature under the same name must be reported, got " + str(got))

    # 6. Identical snapshots produce nothing at all.
    same = snap([rec("A", "a()", "api")], complete)
    check(M._caps_diff(same, same) == [], "identical snapshots should differ in nothing")

    # 7. Kind is part of identity: a block and a function sharing a name are not
    #    the same capability, and must not cancel each other out.
    got = M._caps_diff(snap([rec("Water", "x", "api", kind="function")], complete),
                       snap([rec("Water", "x", "api", kind="block")], complete))
    kinds = sorted(c["what"] for c in got)
    check(kinds == ["added", "removed"],
          "same name in two kinds must not be treated as one capability, got " + str(got))

    # ---------------------------------------------------------------------
    # 8. EVIDENCE MOVES IN BOTH DIRECTIONS.
    #
    # A capability found in a running game and not found now is the single most
    # important thing this tool can say, and it used to say nothing at all: the
    # comparison ignored evidence entirely. A raise was equally silent.
    # ---------------------------------------------------------------------
    got = M._caps_diff(one("GetWaterHeight", "(): number", "structure"),
                       one("GetWaterHeight", "(): number", "verified in game"))
    check(len(got) == 1 and got[0]["what"] == "evidence raised",
          "structure -> verified in game must be reported as a raise, got " + str(got))

    got = M._caps_diff(one("GetWaterHeight", "(): number", "verified in game"),
                       one("GetWaterHeight", "(): number", "not observed"))
    check(len(got) == 1 and got[0]["what"] == "evidence lowered",
          "verified in game -> not observed must be reported as a drop, got " + str(got))
    check(got and "not seen this time" in got[0].get("why", ""),
          "the present-to-absent case must explain itself, got "
          + repr(got[0].get("why") if got else None))

    # 9. "not observed" is the last value in the C++ enum. Ranking it would make
    #    losing a capability compare as the strongest evidence there is, so it
    #    is handled by name. This is the check that catches that regression.
    got = M._caps_diff(one("AISetAwareness", "native", "not observed"),
                       one("AISetAwareness", "native", "present at runtime"))
    check(len(got) == 1 and got[0]["what"] == "evidence raised",
          "not observed -> present at runtime must be a raise, got " + str(got))

    # 10. Symbol presence and measured behavior are separate rungs, and dropping
    #     from one to the other is a drop rather than nothing.
    got = M._caps_diff(one("SetTickRate", "native", "verified in game"),
                       one("SetTickRate", "native", "present at runtime"))
    check(len(got) == 1 and got[0]["what"] == "evidence lowered",
          "verified in game -> present at runtime must be a drop, got " + str(got))

    # 11. A rung a later collector invented: the change is real, the direction is
    #     not claimed. Guessing it would be inventing evidence.
    got = M._caps_diff(one("Future", "native", "structure"),
                       one("Future", "native", "measured on a dedicated host"))
    check(len(got) == 1 and got[0]["what"] == "evidence changed",
          "an unknown rung must not be given a direction, got " + str(got))

    # 12. Evidence never overrides a shape change: a signature that moved is
    #     "changed", whatever happened to the evidence beside it.
    got = M._caps_diff(one("A", "a(): void", "structure"),
                       one("A", "a(): number", "verified in game"))
    check(len(got) == 1 and got[0]["what"] == "changed",
          "a shape change outranks an evidence change, got " + str(got))

    # ---------------------------------------------------------------------
    # 13. STRUCTURAL SIGNATURES. Four changes the old shapes could not see.
    #
    #     These are fixtures for the comparison, in the shapes the collector now
    #     produces. Each pair is a real edit that used to compare as identical
    #     because the shape threw away the part that moved.
    # ---------------------------------------------------------------------
    same_size_options = (
        "dropdown min=- max=- step=0 default=0 perTeam=no "
        "options=[0=No Bots,2=Backfill,1=Static]",
        "dropdown min=- max=- step=0 default=0 perTeam=no "
        "options=[0=No Bots,2=Backfill,1=Solo Only]",
    )
    non_final_overload = (
        "export function AISetTarget(aiPlayer: Player, targetPlayer: Player): void | "
        "export function AISetTarget(player: Player): void",
        "export function AISetTarget(aiPlayer: Player, targetPlayer: Vector): void | "
        "export function AISetTarget(player: Player): void",
    )
    enum_member_added = (
        "export enum AiInput { Crouch, FireWeapon, Interact }",
        "export enum AiInput { Crouch, FireWeapon, Interact, Jump }",
    )
    return_type = (
        "export function GetWaterHeight(p: Vector): number;",
        "export function GetWaterHeight(p: Vector): Vector;",
    )
    for label, (before, after) in (
            ("a dropdown whose options were replaced without changing the count",
             same_size_options),
            ("an overload that is not the last one in the set", non_final_overload),
            ("a member added to a multiline enum", enum_member_added),
            ("a changed return type", return_type)):
        got = M._caps_diff(one("K", before), one("K", after))
        check(len(got) == 1 and got[0]["what"] == "changed",
              f"{label} must be reported as changed, got {got}")

    # 14. And none of those fixtures may become a removal when the newer scan did
    #     not finish the scope. An incomplete scan is still never a claim.
    got = M._caps_diff(
        snap([rec("K", same_size_options[0], "api")], complete),
        snap([], partial))
    check(len(got) == 1 and got[0]["what"] == "not observed",
          "an unfinished scan must not turn a shape change into a removal, got " + str(got))

    # ---------------------------------------------------------------------
    # 15. IDENTITY IS NOT HISTORY: scan A, then B, then A again.
    #
    #     The store addresses content by its hash, so the third scan writes no
    #     new directory. Listing directories therefore loses it, and sorting them
    #     by capture time reports B as current. Both transitions must be visible
    #     and A must be current.
    # ---------------------------------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        write_store(root, "sdk", ("A", "B", "A"))
        _NS["_root"] = lambda: root
        try:
            ids = M._caps_list("sdk")
            check(ids == ["A", "B", "A"],
                  "A/B/A must list three scans of two contents, newest first, got " + str(ids))
            check(ids and ids[0] == "A",
                  "the return to A must be current, not the B that sorts newest by content")

            head = M._caps_observations("sdk")
            check(len(head) == 3 and head[0].get("seq") == 3,
                  "the head must be the third observation, got " + str([o.get("seq") for o in head]))
            check(len(head) > 1 and head[0].get("observedUtc", "") > head[1].get("observedUtc", ""),
                  "the newest scan must carry its own time, not the time its content was first seen")

            # Both transitions: B -> A now, and A -> B before it. A store that
            # lost a scan has fewer than three, and these then fail rather than
            # crashing, so the reason stays readable.
            def at(i):
                return (M._caps_load("sdk", ids[i]) if i < len(ids) else None) or {}
            newest, middle, oldest = at(0), at(1), at(2)
            back = M._caps_diff(middle, newest)
            forth = M._caps_diff(oldest, middle)
            check([c["what"] for c in back] == ["added", "removed"],
                  "the return from B to A must report both sides of the swap, got " + str(back))
            check([c["what"] for c in forth] == ["added", "removed"],
                  "the move from A to B must report both sides of the swap, got " + str(forth))

            # 16. Re-reading is what a restart does: the head must not move.
            check(M._caps_list("sdk") == ids, "the head must survive being read again")
        finally:
            _NS["_root"] = lambda: PROJECT

    # 17. A/A: scanning twice with nothing changed records a second scan and
    #     stores one content. "We looked again and it was the same" is a fact,
    #     and it is what makes a later return to older content visible.
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        caps = write_store(root, "portal", ("A", "A"))
        _NS["_root"] = lambda: root
        try:
            ids = M._caps_list("portal")
            check(ids == ["A", "A"], "A/A must be two scans, got " + str(ids))
            stored = list((caps / "snapshots" / "portal").iterdir())
            check(len(stored) == 1,
                  "A/A must store one content, got " + str([p.name for p in stored]))
            both = M._caps_load("portal", "A") or {}
            check(M._caps_diff(both, both) == [],
                  "two scans of identical content must report no changes")

            # 18. A store from before the log existed still reads, in capture
            #     order, so upgrading does not look like a source with no past.
            legacy = root / "Saved" / "BF6UnrealSDK" / "capabilities"
            for child in (legacy / "observations" / "portal").iterdir():
                child.unlink()
            check(M._caps_list("portal") == ["A"],
                  "a store with blobs and no log must still list its contents")
        finally:
            _NS["_root"] = lambda: PROJECT

    if fails:
        print(f"capabilities: {len(fails)} of {checks} checks failed")
        for f in fails:
            print("  FAIL " + f)
        return 1
    print(f"capabilities: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
