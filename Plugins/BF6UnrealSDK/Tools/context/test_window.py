"""Proof that "the last N minutes" means the same N minutes everywhere.

THE BUG THIS EXISTS TO CATCH.

Unreal stamps its log in UTC. The log index compares a cutoff string straight
against those stamps. The cutoff used to be built from datetime.now(), the local
wall clock, so on a UTC-5 machine the two sides of the comparison were five
hours apart and "the last 20 minutes" selected the last five hours and twenty
minutes. Warnings from a session that had already ended were reported as
current, which is the worst possible failure for a tool whose whole job is
saying what just went wrong.

The bug is invisible in UTC, which is why this runs the same fixture at several
offsets and across midnight instead of just checking the code once.

Run:  python test_window.py
"""

import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import log_index  # noqa: E402


def iso(dt):
    """The naive-UTC form the log index stores in ev["time"]."""
    return dt.replace(tzinfo=None).isoformat(timespec="milliseconds")


def cutoff(as_of_utc, minutes):
    """The cutoff the MCP tool builds. Mirrors context_errors."""
    return (as_of_utc - timedelta(minutes=minutes)).replace(
        tzinfo=None).isoformat(timespec="seconds")


def old_cutoff(as_of_utc, minutes, tz):
    """What it used to build: the same instant read off a local clock."""
    return (as_of_utc.astimezone(tz) - timedelta(minutes=minutes)).replace(
        tzinfo=None).isoformat(timespec="seconds")


def selected(events, since):
    """The index's own comparison, lifted out of errors()/events()."""
    return [e for e in events
            if not (e["time"] is None or e["time"] < since)]


def main():
    fails = []
    checks = 0

    def check(cond, what):
        nonlocal checks
        checks += 1
        if not cond:
            fails.append(what)

    # The audit's case: asked at 23:55 UTC for 20 minutes, with a warning from
    # 23:27 that is 28 minutes old and must not be included.
    as_of = datetime(2026, 9, 7, 23, 55, 0, tzinfo=timezone.utc)
    events = [
        {"time": iso(as_of - timedelta(minutes=28)), "text": "old warning"},
        {"time": iso(as_of - timedelta(minutes=5)), "text": "recent error"},
        {"time": iso(as_of - timedelta(minutes=1)), "text": "newest error"},
    ]
    want = {"recent error", "newest error"}

    # Every offset picks the same events, because the instant is the same.
    for name, tz in [
        ("UTC", timezone.utc),
        ("UTC-5 (Chicago in DST)", timezone(timedelta(hours=-5))),
        ("UTC+9 (Tokyo)", timezone(timedelta(hours=9))),
        ("UTC+5:30 (Kolkata)", timezone(timedelta(hours=5, minutes=30))),
    ]:
        got = {e["text"] for e in selected(events, cutoff(as_of, 20))}
        check(got == want, "in {} the window selected {} instead of {}".format(
            name, sorted(got), sorted(want)))

        # And the old behaviour is genuinely wrong wherever the offset is not
        # zero, so this fixture really does exercise the bug. The direction
        # depends on the sign: west of UTC the window widens and sweeps in old
        # events, east of it the window slides into the future and returns
        # nothing. Both are wrong, and neither is what the caller asked for.
        was = {e["text"] for e in selected(events, old_cutoff(as_of, 20, tz))}
        if tz is timezone.utc:
            check(was == want, "UTC should have been correct even before the fix")
        else:
            check(was != want,
                  "the old local-clock cutoff should have given the wrong answer "
                  "in {}, but it matched: {}".format(name, sorted(was)))

    # Across midnight: the window starts on the previous day, which is where a
    # comparison done on times rather than full datetimes falls apart.
    at_midnight = datetime(2026, 9, 8, 0, 8, 0, tzinfo=timezone.utc)
    over = [
        {"time": iso(at_midnight - timedelta(minutes=45)), "text": "before, excluded"},
        {"time": iso(at_midnight - timedelta(minutes=15)), "text": "yesterday, included"},
        {"time": iso(at_midnight - timedelta(minutes=2)), "text": "today, included"},
    ]
    got = {e["text"] for e in selected(over, cutoff(at_midnight, 20))}
    check(got == {"yesterday, included", "today, included"},
          "the window across midnight selected " + str(sorted(got)))

    # The cutoff has to stay lexicographically comparable with the stamps the
    # parser produces, because that is how the index compares them.
    m = log_index.TS_RE.match("[2026.09.08-01.24.47:650][  0]LogBF6: Display: x")
    check(m is not None, "the log timestamp pattern no longer matches a real line")
    if m:
        parsed = log_index._iso(m)
        check(parsed > cutoff(datetime(2026, 9, 8, 1, 20, 0, tzinfo=timezone.utc), 0),
              "a 01:24:47 stamp should sort after a 01:20:00 cutoff, got " + parsed)
        check(parsed < cutoff(datetime(2026, 9, 8, 1, 30, 0, tzinfo=timezone.utc), 0),
              "a 01:24:47 stamp should sort before a 01:30:00 cutoff, got " + parsed)

    if fails:
        print("window: {} of {} checks failed".format(len(fails), checks))
        for f in fails:
            print("  FAIL " + f)
        return 1
    print("window: {} checks passed".format(checks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
