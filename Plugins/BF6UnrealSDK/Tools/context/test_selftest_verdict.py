"""Proof that context_selftest cannot report a run as clean when it was not.

WHAT WENT WRONG, SO IT CANNOT COME BACK.

context_selftest is the release gate. It answered `clean (DONE 44 passed, 1
failed)`. Three separate mistakes had to line up for that sentence to exist:

  the completion line was quoted, never read   -> the counts in it were ignored
  the failure list was the only evidence used  -> and the backwards scan for
                                                  DONE stopped one line before
                                                  the FAIL that was right there
  any DONE in the last 200k of log counted     -> so an old green run could
                                                  answer today's question

The rules these tests hold to:

  a failure anywhere, by either count, is FAILED
  no completion inside this run's window is INCOMPLETE, never clean
  a check that could not run is SKIPPED, and the page counts those as passes
  a result is this run's only if it arrived after this run dispatched the test

The MCP module cannot be imported outside the editor: it registers its toolset
against unreal.uclass at import time. So the real function is lifted out by name
and executed here, the same way test_capabilities.py does it, which keeps this
pinned to the shipped source instead of to a copy of it.

Run:  python test_selftest_verdict.py
"""

import ast
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = next((p for p in HERE.parents if (p / "Content" / "Python").is_dir()), None)
if PROJECT is None:
    print("could not find the project root above " + str(HERE))
    sys.exit(1)

_SRC = (PROJECT / "Content" / "Python" / "bf6_context_mcp.py").read_text(encoding="utf-8")
_TREE = ast.parse(_SRC)
_NODE = next((n for n in ast.walk(_TREE)
              if isinstance(n, ast.FunctionDef) and n.name == "context_selftest"), None)
if _NODE is None:
    print("context_selftest is no longer in bf6_context_mcp.py")
    sys.exit(1)
_NODE.decorator_list = []
_CODE = compile(ast.fix_missing_locations(ast.Module(body=[_NODE], type_ignores=[])),
                "bf6_context_mcp.py", "exec")

# The real sentinel _capture_console writes immediately before it dispatches a
# command. The fixtures reproduce it because the verdict binds to it: a page
# reply can only be this run's if it landed after this line.
SENTINEL = "BF6ContextStateMarker"


def run(log_text="", reply=None, status_output=None, area="all",
        closing_sentinel=True, truncate_after=False):
    """Drive the real context_selftest against a fake log and a fake page.

    reply is what the page writes to the log after the SelfTest command is
    dispatched, which is where the real page writes it: after the capture
    window has already closed.
    """
    tmp = Path(tempfile.mkdtemp(prefix="bf6selftest"))
    log = tmp / "BF6.log"
    log.write_text(log_text, encoding="utf-8")

    def capture(cmds):
        results = {}
        for cmd in cmds:
            output = list(status_output or []) if cmd.endswith("Status") else []
            results[cmd] = {"command": cmd, "captured": bool(output),
                            "exec_error": None, "output": output}
            if cmd.endswith("SelfTest"):
                with log.open("a", encoding="utf-8") as handle:
                    handle.write(f"[2026.09.08-00.00.00:001][ 12]LogPython: "
                                 f"{SENTINEL} BEGIN abc123def456 {cmd}\n")
                    for line in (reply or []):
                        handle.write(
                            "[2026.09.08-00.00.00:641][ 12]LogBF6Blocks: " + line + "\n")
        if truncate_after:
            log.write_text("", encoding="utf-8")
        return {"results": results,
                "diagnostics": {"closing_sentinel_seen": closing_sentinel}}

    env = {"_emit": lambda payload: payload,
           "_capture_console": capture,
           "_live_log_path": lambda: log}
    exec(_CODE, env)
    return env["context_selftest"](area)


PASS_LINES = ["selftest: PASS every module loaded",
              "selftest: PASS every button is wired (18 checked)",
              "selftest: PASS canvas has size (900x600)"]


def main():
    fails = []
    checks = 0

    def check(cond, what):
        nonlocal checks
        checks += 1
        if not cond:
            fails.append(what)

    # 1. THE REPRODUCED DEFECT, EXACTLY. The completion line says one check
    #    failed and no FAIL line is collected anywhere. This answered "clean
    #    (DONE 44 passed, 1 failed)". The counts decide it now.
    got = run(reply=["selftest: DONE 44 passed, 1 failed"])
    check(got["verdict"].startswith("FAILED"),
          "a completion line reporting a failure must be FAILED, got " + got["verdict"])
    check("1 failed" in got["verdict"] and "44" in got["verdict"],
          "the verdict must show the counts it decided on, got " + got["verdict"])
    check(got["counts"]["page_reported_failed"] == 1,
          "the reported failed count must be parsed, got " + str(got["counts"]))
    check("count_disagreement" in got,
          "a page count with no matching FAIL line is a disagreement worth printing")

    # 2. THE DELAYED FAIL. The page reports after the capture window closes,
    #    which is the normal case, not the exception.
    got = run(reply=["selftest: FAIL every button is wired - Build does nothing",
                     "selftest: DONE 43 passed, 1 failed"])
    check(got["verdict"].startswith("FAILED"),
          "a FAIL that arrived after the capture window must be FAILED, got " + got["verdict"])
    check(any("Build does nothing" in f for f in got["failures"]),
          "the failing check must be named in the report, got " + str(got["failures"]))
    check(got["area_status"]["blocks"].startswith("failed"),
          "the tested area must read failed, got " + got["area_status"]["blocks"])

    # 3. NOTHING CAME BACK. The page was closed, or is still working. This is
    #    the state the old code called clean.
    got = run()
    check("INCOMPLETE" in got["verdict"] and "NO RESULT" in got["verdict"],
          "no results at all must be INCOMPLETE, got " + got["verdict"])
    check(got["area_status"]["blocks"].startswith("incomplete"),
          "an area with no completion must read incomplete, got " + got["area_status"]["blocks"])

    # 4. AN OLD GREEN RUN CANNOT ANSWER THIS ONE. The log is full of a passing
    #    run from before this call; this call produced nothing.
    old = "".join("[2026.09.07-23.00.00:000][ 1]LogBF6Blocks: " + line + "\n"
                  for line in PASS_LINES + ["selftest: DONE 45 passed, 0 failed"])
    got = run(log_text=old)
    check("INCOMPLETE" in got["verdict"],
          "an older run's completion must not satisfy this run, got " + got["verdict"])
    check(got["ignored_evidence"]["completed"] == "DONE 45 passed, 0 failed",
          "the rejected older result must be shown, got " + str(got.get("ignored_evidence")))
    check(got["completed"] is None,
          "no completion belongs to this run, got " + str(got["completed"]))

    # 5. AN OLD GREEN RUN CANNOT BURY A NEW FAILURE EITHER.
    got = run(log_text=old, reply=["selftest: FAIL edit guards balanced - applying left on",
                                   "selftest: DONE 44 passed, 1 failed"])
    check(got["verdict"].startswith("FAILED"),
          "this run's failure must win over an older pass, got " + got["verdict"])

    # 6. A GENUINELY CLEAN RUN STILL READS CLEAN, otherwise the gate is noise.
    got = run(reply=PASS_LINES + ["selftest: DONE 3 passed, 0 failed"])
    check(got["verdict"].startswith("clean"),
          "a real pass must read clean, got " + got["verdict"])
    check("3 passed" in got["verdict"],
          "the clean verdict must carry its counts, got " + got["verdict"])
    check(got["area_status"]["script"].startswith("skipped"),
          "an area with no self test must read skipped, got " + got["area_status"]["script"])

    # 7. A SKIP IS NOT A PASS. editor_ui.js has no skip counter: a check that
    #    could not run calls ok(), so the page's passed number includes them.
    got = run(reply=["selftest: PASS every module loaded",
                     "selftest: PASS the converter round trips (SKIPPED: no TypeScript "
                     "compiler loaded yet)",
                     "selftest: PASS workspace save (nothing loaded: empty workspace)",
                     "selftest: DONE 3 passed, 0 failed"])
    check(got["counts"]["checks_skipped"] == 2 and got["counts"]["checks_passed"] == 1,
          "skips must be counted apart from passes, got " + str(got["counts"]))
    check("2 skipped" in got["verdict"],
          "the verdict must say how much was skipped, got " + got["verdict"])

    # 8. A RUN THAT ONLY SKIPPED CHECKED NOTHING.
    got = run(reply=["selftest: PASS the converter round trips (SKIPPED: no compiler)",
                     "selftest: DONE 1 passed, 0 failed"])
    check(got["verdict"].startswith("INCOMPLETE"),
          "a run of nothing but skips must not read clean, got " + got["verdict"])

    # 9. NO SELF TEST EXISTS FOR THE REQUESTED AREA. Open and Status alone
    #    check nothing, and answering clean to that is the same lie in a
    #    smaller box.
    got = run(area="script")
    check(got["verdict"].startswith("INCOMPLETE"),
          "an area with no self test must not read clean, got " + got["verdict"])

    # 10. HOST SIDE PROBLEMS COUNT. A page that reports its own last error is
    #     not clean however green its self test was.
    got = run(reply=PASS_LINES + ["selftest: DONE 3 passed, 0 failed"],
              status_output=["last error: the bundler could not be reached"])
    check(got["verdict"].startswith("FAILED"),
          "a reported page error must fail the run, got " + got["verdict"])

    # 11. A LOG THAT ROTATED UNDER THE RUN PROVES NOTHING.
    got = run(reply=PASS_LINES + ["selftest: DONE 3 passed, 0 failed"], truncate_after=True)
    check("INCOMPLETE" in got["verdict"],
          "a rotated log must be incomplete, not clean, got " + got["verdict"])

    # 12. EVERY ANSWER CARRIES ITS RUN ID AND HOW IT WAS BOUND.
    got = run(reply=PASS_LINES + ["selftest: DONE 3 passed, 0 failed"])
    check(len(got.get("run_id", "")) >= 8, "every run needs an id")
    check(got["binding"]["run_id"] == got["run_id"] and got["binding"]["self_test_dispatch_seen"],
          "the binding must state the run it belongs to, got " + str(got.get("binding")))

    if fails:
        print(f"selftest verdict: {len(fails)} of {checks} checks failed")
        for f in fails:
            print("  FAIL " + f)
        return 1
    print(f"selftest verdict: {checks} checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
