"""Strict verdicts: completed workflow, responsiveness and hardware targets differ."""
import math


def frames_summary(values):
    values = sorted(x for x in values if math.isfinite(x) and x > 0)
    if not values:
        return {"samples": 0, "p95_ms": None, "p99_ms": None, "average_fps": None}
    percentile = lambda p: values[max(0, math.ceil(len(values) * p) - 1)]
    return dict(samples=len(values), p95_ms=percentile(.95), p99_ms=percentile(.99),
                worst_ms=values[-1], average_fps=1000 * len(values) / sum(values),
                over_33ms=sum(x > 33.333 for x in values), over_100ms=sum(x > 100 for x in values),
                over_500ms=sum(x > 500 for x in values))


def evaluate(workflow, samples, exit_code, failure, visible, gpu_budget_gib):
    passes = bool(workflow and workflow.get("completed") and not failure and exit_code == 0)
    flights = workflow.get("flights", []) if workflow else []
    opens = workflow.get("opens", []) if workflow else []
    gpu = [s["gpu"]["dedicated_bytes"] for s in samples if s["gpu"]["dedicated_bytes"] is not None]
    foreground_ok = True
    for flight in flights:
        observed = [s for s in samples if flight.get("start", float("inf")) <= s.get("monotonic", 0) <= flight.get("end", 0)]
        if len(observed) < 2 or sum(s.get("foreground", False) for s in observed) / len(observed) < .9:
            foreground_ok = False
    return dict(
        stability="PASS" if passes else "FAIL",
        failure=failure or (workflow or {}).get("error") or (None if passes else
            f"Editor exited with 0x{exit_code:08x}" if exit_code else "Workflow or clean editor exit missing"),
        exit_code=exit_code,
        open_10s=("NOT_TESTED" if not opens else "PASS" if all(o["seconds"] <= 10 for o in opens) else "FAIL"),
        flight_60fps=("NOT_TESTED" if not visible or not flights or not foreground_ok else "PASS" if all(
            f["frames"]["samples"] >= 60 and f["frames"]["p95_ms"] <= 1000 / 60 for f in flights) else "FAIL"),
        hitches_100ms=("NOT_TESTED" if not flights else "PASS" if all(
            f["frames"]["samples"] >= 60 and f["frames"].get("over_100ms", 0) == 0 for f in flights) else "FAIL"),
        gpu_budget=("NOT_MEASURED" if not gpu else "FAIL" if max(gpu) > gpu_budget_gib * 1024**3
                    else "NOT_TESTED" if not passes or not visible or not flights or not foreground_ok else "PASS"),
        peak_dedicated_gpu_bytes=max(gpu) if gpu else None,
        peak_job_commit_bytes=max((s["peak_job_commit_bytes"] for s in samples), default=0),
        workflow=workflow,
        limitations=[
            "Resource pressure does not emulate a CPU/GPU model or its driver.",
            "Commit ceilings inject allocation failures; they do not simulate physical RAM and paging.",
            "GPU budget is a measured threshold, not a VRAM cap. WDDM process counters can double-count shared allocations.",
            "Current frame samples use distinct engine frames, observed from Slate; older reports used raw Slate callbacks. Neither is GPU timing or presented-frame capture.",
            "Fresh high-poly cache does not mean cold OS file cache or cold engine DDC.",
            "Visual/material correctness and real AMD, Nvidia and Intel hardware still require testing.",
        ])


def markdown(report, config):
    def gib(v):
        return "unavailable" if v is None else f"{v / 1024**3:.2f} GiB"
    lines = ["# Local stability test", "", f"Workflow and clean exit: **{report['stability']}**",
             f"Open within 10 seconds: **{report['open_10s']}**",
             f"Flight p95 within 16.67 ms: **{report['flight_60fps']}**",
             f"No editor callback stalls over 100 ms: **{report['hitches_100ms']}**",
             f"GPU threshold: **{report['gpu_budget']}**", "",
             f"Profile: {config['profile']}; mode: {config['mode']}; level: {config.get('level') or 'Entry smoke test'}.",
             f"Engine worker sizing: {config.get('engine_workers', 'host (legacy)')}; compact render corners: {config.get('compact_vertices', 'not recorded')}.",
             f"Terrain preparation batch: {config.get('terrain_batch', 'binary default (legacy run)')}.",
             f"Rendering: {config.get('render_profile', 'current')}; Nanite: {config.get('nanite', 'default')}; authored LODs: {config.get('game_lods', 0)}; texture streaming: {config.get('texture_streaming', 1)}; build batch: {config.get('build_batch', 0)}; async water: {config.get('water_async', 'not recorded')}.",
             f"Peak job commit: {gib(report['peak_job_commit_bytes'])}.",
             f"Peak sampled process-tree dedicated GPU allocation: {gib(report['peak_dedicated_gpu_bytes'])}.", ""]
    if report["failure"]:
        lines += [f"Failure: {report['failure']}", ""]
    for item in (report.get("workflow") or {}).get("opens", []):
        lines.append(f"- Open {item['cycle']}: {item['seconds']:.3f} seconds to observed ready state.")
    for item in (report.get("workflow") or {}).get("flights", []):
        f = item["frames"]
        timing = lambda name: "unavailable" if f.get(name) is None else f"{f[name]:.2f}"
        lines.append(f"- Flight {item['cycle']}: {f['samples']} frames, p95 {timing('p95_ms')} ms, p99 {timing('p99_ms')} ms, {f.get('over_100ms', 0)} intervals over 100 ms.")
    lines += ["", "## Measurement limits", ""] + ["- " + s for s in report["limitations"]]
    return "\n".join(lines) + "\n"
