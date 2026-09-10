"""Sequential high-poly performance controls, each in its own disposable project."""
import argparse
import csv
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

CASES = {
    "resident": dict(texture_streaming=0, render_profile="current", build_batch=128),
    "streaming": dict(texture_streaming=1, render_profile="current", build_batch=128),
    "performance": dict(texture_streaming=1, render_profile="performance", build_batch=32),
    "game-lods": dict(texture_streaming=1, render_profile="performance", build_batch=32, game_lods=1),
    "no-nanite": dict(texture_streaming=1, render_profile="performance", build_batch=32, game_lods=1, nanite="off"),
    "pressure12": dict(texture_streaming=1, render_profile="performance", build_batch=32, profile="pressure12"),
    "pressure10": dict(texture_streaming=1, render_profile="performance", build_batch=32, profile="pressure10"),
    "pressure8": dict(texture_streaming=1, render_profile="performance", build_batch=32, profile="pressure8"),
    "sync-water": dict(texture_streaming=1, render_profile="performance", build_batch=32, water_async=0),
    "unshared-vertices": dict(texture_streaming=1, render_profile="performance", build_batch=32, compact_vertices=0),
    "terrain-unbatched": dict(texture_streaming=1, render_profile="performance", build_batch=32, terrain_batch=256),
    "host-workers": dict(texture_streaming=1, render_profile="performance", build_batch=32, engine_workers="host"),
    "low-pressure12": dict(render_profile="performance", profile="pressure12", mode="low"),
    "quick-exit": dict(render_profile="performance", profile="pressure12", mode="low", cycles=1, flight_seconds=5),
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--project", type=Path, required=True)
    p.add_argument("--editor", type=Path, required=True)
    p.add_argument("--output-root", type=Path, required=True)
    p.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES)[:6])
    p.add_argument("--level", default="MP_Dumbo")
    p.add_argument("--save", default="")
    p.add_argument("--min-placed", type=int, default=0)
    p.add_argument("--camera-start", nargs=6, type=float)
    p.add_argument("--cycles", type=int, default=2)
    p.add_argument("--flight-seconds", type=float, default=30)
    p.add_argument("--timeout", type=float, default=900)
    p.add_argument("--visible", action="store_true")
    p.add_argument("--memory-report", action="store_true")
    p.add_argument("--prepare-only", action="store_true")
    a = p.parse_args()
    out = a.output_root / ("matrix-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S"))
    out.mkdir(parents=True)
    results = []
    for name in a.cases:
        print("Starting " + name, flush=True)
        options = dict(project=a.project, editor=a.editor, output_root=out / name,
                       level=a.level, save=a.save, min_placed=a.min_placed,
                       cycles=a.cycles, flight_seconds=a.flight_seconds, timeout=a.timeout,
                       profile="six-core", cache="fresh-highpoly", mode="high")
        options.update(CASES[name])
        command = [sys.executable, str(Path(__file__).with_name("run.py"))]
        for key, value in options.items(): command += ["--" + key.replace("_", "-"), str(value)]
        if a.camera_start: command += ["--camera-start", *map(str, a.camera_start)]
        if a.visible: command.append("--visible")
        if a.memory_report: command.append("--memory-report")
        if a.prepare_only: command.append("--prepare-only")
        status = subprocess.call(command)
        reports = sorted((out / name).glob("*/report.json"))
        row = dict(case=name, exit_code=status, report=str(reports[-1]) if reports else "")
        if reports:
            report = json.loads(reports[-1].read_text())
            row.update({key: report.get(key) for key in ("stability", "open_10s", "flight_60fps", "hitches_100ms", "gpu_budget",
                        "peak_job_commit_bytes", "peak_dedicated_gpu_bytes")})
            workflow = report.get("workflow") or {}
            row["open_seconds"] = [x["seconds"] for x in workflow.get("opens", [])]
            row["flight_p95_ms"] = [x["frames"]["p95_ms"] for x in workflow.get("flights", [])]
        results.append(row)
        (out / "matrix.json").write_text(json.dumps(results, indent=2))
        keys = list(dict.fromkeys(key for item in results for key in item))
        with (out / "matrix.csv").open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=keys); writer.writeheader(); writer.writerows(results)
        # A failed limit test is evidence and must not suppress the remaining cases.
    print("Matrix: " + str(out), flush=True)
    return int(any(r["exit_code"] for r in results))

if __name__ == "__main__": raise SystemExit(main())
