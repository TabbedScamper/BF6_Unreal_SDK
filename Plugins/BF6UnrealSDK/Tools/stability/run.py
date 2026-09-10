"""Create a disposable project, launch under a Windows Job, collect a local report."""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import traceback
import zipfile

sys.dont_write_bytecode = True
from windows_job import Job, GPUCounters, foreground_pid, show_editor, editor_windows
from report import evaluate, markdown

PROFILES = {"baseline": (0, 0), "six-core": (6, 0),
            "pressure12": (6, 12), "pressure10": (6, 10), "pressure8": (6, 8)}


def engine_cpu_arguments(cores, mask, workers):
    result = ["-corelimit=" + str(cores)] if cores and workers == "profile" else []
    # UE's physical-affinity flag assumes interleaved SMT numbering. Use it only
    # when Windows topology confirms that exact mask. This also tells UE to stop
    # assigning worker affinities outside the externally imposed process mask.
    if cores and mask == sum(1 << (2 * i) for i in range(cores)):
        result.append("-processaffinityphysical=" + str(cores))
    return result


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_tree(source, target):
    if source.is_dir():
        shutil.copytree(source, target, ignore=shutil.ignore_patterns(
            "__pycache__", ".git", ".ai", "node_modules", ".backups"))


def prepare(project, out, cache):
    """Private Content/Config/Saved/Binaries; shared plugin assets and binaries only."""
    source = project.parent
    clone = out / "project"
    clone.mkdir()
    shutil.copy2(project, clone / project.name)
    for folder in ("Content", "Config", "Binaries"):
        copy_tree(source / folder, clone / folder)
    # Junction is deliberately never recursively deleted by this harness.
    # Shared plugin files are loaded only; the test writes under private Saved/Content.
    result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                             "New-Item -ItemType Junction -Path $env:BF6_TEST_LINK -Target $env:BF6_TEST_PLUGINS | Out-Null"],
                            env={**os.environ, "BF6_TEST_LINK": str(clone / "Plugins"),
                                 "BF6_TEST_PLUGINS": str(source / "Plugins")},
                            capture_output=True, text=True, creationflags=0x08000000)
    if result.returncode:
        raise RuntimeError("Cannot link test plugins: " + result.stderr)
    saved = clone / "Saved" / "BF6UnrealSDK"
    saved.mkdir(parents=True)
    copy_tree(source / "Saved/BF6UnrealSDK/saves", saved / "saves")
    # Offline documents and attachments are inputs to imported experience tests.
    copy_tree(source / "Saved/BF6UnrealSDK/portal/experiences", saved / "portal/experiences")
    # Installed SDK extraction is an input, not a high-poly game cache. Omitting
    # it makes an implausibly fast empty map and drops the creator's real props.
    print("Copying installed SDK geometry into the disposable project...", flush=True)
    copy_tree(source / "Saved/BF6UnrealSDK/sdkdata", saved / "sdkdata")
    for name in ("objmodels", "mapmesh", "sdkhistory"):
        destination = saved / "sdkdata" / name
        if not destination.exists():
            copy_tree(source / "Plugins/BF6UnrealSDK/Source/ThirdParty/libbf6/data" / name, destination)
            destination.mkdir(parents=True, exist_ok=True)
        # Having these private destinations also prevents legacy startup migration
        # from moving a generated folder out of the shared source plugin.
    original_hp = source / "Saved/BF6UnrealSDK/HighPoly"
    if cache == "copy-existing":
        copy_tree(original_hp, saved / "HighPoly")
    else:
        (saved / "HighPoly").mkdir()
        if (original_hp / "install.txt").exists():
            shutil.copy2(original_hp / "install.txt", saved / "HighPoly/install.txt")
    # No Portal credentials, browser profiles, update state or telemetry consent copied.
    editor_config = clone / "Saved/Config/WindowsEditor"
    editor_config.mkdir(parents=True)
    sdk = source / "Saved/BF6UnrealSDK/sdk"
    candidates = sorted(sdk.glob("PortalSDK-*")) if sdk.exists() else []
    settings = "[/Script/UnrealEd.EditorPerformanceSettings]\nbThrottleCPUWhenNotForeground=False\n"
    settings += "[/Script/UnrealEd.EditorLoadingSavingSettings]\nbAutoSaveEnable=False\n"
    if candidates:
        settings += "[BF6UnrealSDK]\nSdkRoot=" + str(candidates[-1]).replace("\\", "/") + "\n"
    else:
        # Preserve an explicitly configured SDK root when the automatic layout is absent.
        original = source / "Saved/Config/WindowsEditor/EditorPerProjectUserSettings.ini"
        if original.exists():
            match = re.search(r"(?m)^SdkRoot=(.+)$", original.read_text(encoding="utf-8-sig"))
            if match:
                value = Path(match[1].strip())
                if not value.is_absolute():
                    raise ValueError("Relative custom SdkRoot cannot be copied safely; use an absolute SDK setting")
                settings += "[BF6UnrealSDK]\nSdkRoot=" + str(value) + "\n"
    (editor_config / "EditorPerProjectUserSettings.ini").write_text(settings, encoding="utf-8")
    # The test uses -ExecutePythonScript, not MCP. Avoid port collisions and startup clients.
    init = clone / "Content/Python/init_unreal.py"
    if init.exists():
        init.write_text("# Disposable stability test: no MCP startup.\n", encoding="utf-8")
    with (clone / "Config/DefaultEditorPerProjectUserSettings.ini").open("a", encoding="utf-8") as f:
        f.write("\n[/Script/ModelContextProtocolEngine.ModelContextProtocolSettings]\nbAutoStartServer=False\n")
    return clone / project.name


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--profile", choices=PROFILES, default="six-core")
    parser.add_argument("--mode", choices=("smoke", "low", "high"), default="low")
    parser.add_argument("--level", default="MP_Dumbo")
    parser.add_argument("--save", default="")
    parser.add_argument("--min-placed", type=int, default=0)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--flight-seconds", type=float, default=30)
    parser.add_argument("--flight-radius-m", type=float, default=150)
    parser.add_argument("--camera-start", nargs=6, type=float, metavar=("X", "Y", "Z", "PITCH", "YAW", "ROLL"),
                        help="Starting view in Unreal centimetres/degrees; otherwise use map-open camera")
    parser.add_argument("--stage-timeout", type=float, default=600)
    parser.add_argument("--timeout", type=float, default=1200)
    parser.add_argument("--cpu-percent", type=float, default=0, help="Optional hard quota as percent of TOTAL host CPU")
    parser.add_argument("--texture-pool-mb", type=int, default=1536)
    parser.add_argument("--gpu-budget-gib", type=float, default=4.5)
    parser.add_argument("--cache", choices=("fresh-highpoly", "copy-existing"), default="fresh-highpoly")
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--visible", action="store_true", help="Show benchmark viewport; required to grade flight timing")
    parser.add_argument("--render-profile", choices=("current", "performance", "balanced"), default="current")
    parser.add_argument("--texture-streaming", choices=(0, 1), type=int, default=1)
    parser.add_argument("--nanite", choices=("default", "on", "off"), default="default")
    parser.add_argument("--build-batch", choices=(0, 32, 64, 128, 256), type=int, default=0)
    parser.add_argument("--terrain-batch", choices=(4, 8, 16, 32, 64, 128, 256), type=int, default=16)
    parser.add_argument("--game-lods", choices=(0, 1), type=int, default=0)
    parser.add_argument("--water-async", choices=(0, 1), type=int, default=1)
    parser.add_argument("--compact-vertices", choices=(0, 1), type=int, default=1)
    parser.add_argument("--engine-workers", choices=("profile", "host"), default="profile",
                        help="Profile sizes Unreal worker pools to the CPU limit; host retains oversubscription for stress comparisons")
    parser.add_argument("--memory-report", action="store_true")
    parser.add_argument("--trace", action="store_true", help="Record local Unreal Insights traces during flight; profiling overhead affects timing")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if not args.project.is_file() or args.project.suffix != ".uproject" or not args.editor.is_file():
        parser.error("Existing .uproject and UnrealEditor.exe paths are required")
    if args.cycles < 1 or args.flight_seconds < 1 or args.timeout < 1 or args.stage_timeout < 1:
        parser.error("Cycles, flight duration and timeouts must be positive")
    if not 0 <= args.cpu_percent <= 100 or 0 < args.cpu_percent < .01:
        parser.error("CPU percent must be 0 (disabled) or 0.01 through 100")
    if args.texture_pool_mb < 1 or args.gpu_budget_gib <= 0 or args.min_placed < 0 or args.flight_radius_m < 0:
        parser.error("Invalid memory, actor or camera limits")
    if not re.fullmatch(r"[A-Za-z0-9_]+", args.level) or any(c in args.save for c in '\"\r\n/\\:;') or args.save in (".", ".."):
        parser.error("Level/save contains console control characters")
    return args


def main():
    args = parse_args()
    output_root = args.output_root or args.project.parent / "Saved/StabilityRuns"
    output_root.mkdir(parents=True, exist_ok=True)
    out = output_root / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S") + "-" + args.profile + "-" + args.mode)
    out.mkdir()  # A collision must not overwrite earlier evidence.
    config = {key: str(value.resolve()) if isinstance(value, Path) else value for key, value in vars(args).items()}
    config["output"] = str(out.resolve())
    config["has_highpoly"] = (args.project.parent / "Plugins/Add-Ons/BF6HighPoly/BF6HighPoly.uplugin").exists()
    config["cores"], config["commit_limit_gib"] = PROFILES[args.profile]
    config["source_plugin_versions"] = {}
    config["binary_sha256"] = {}
    native = args.project.parent / "Plugins/BF6UnrealSDK/Source/ThirdParty/libbf6/bin/Win64/bf6_core.dll"
    if native.is_file():
        config["binary_sha256"]["bf6_core"] = sha256(native)
    for plugin in ("BF6UnrealSDK", "Add-Ons/BF6HighPoly"):
        path = args.project.parent / "Plugins" / plugin / (Path(plugin).name + ".uplugin")
        if path.exists():
            config["source_plugin_versions"][plugin] = json.loads(path.read_text(encoding="utf-8-sig")).get("VersionName")
            binary = path.parent / "Binaries/Win64" / ("UnrealEditor-" + path.stem + ".dll")
            if binary.exists():
                config["binary_sha256"][plugin] = sha256(binary)
    print("Test output: " + str(out), flush=True)
    samples, workflow, failure, code = [], None, None, None
    job, gpu = None, None
    try:
        if not args.prepare_only:
            existing = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                "@(Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue).Count"],
                capture_output=True, text=True, creationflags=0x08000000, check=True)
            if int(existing.stdout.strip()):
                raise RuntimeError("Close other Unreal editors before running a benchmark. Shared binaries and concurrent rendering invalidate comparisons.")
        if args.mode != "smoke":
            geometry = args.project.parent / "Saved/BF6UnrealSDK/sdkdata/mapmesh"
            if not any(geometry.glob(args.level + "_*.bf6mesh")):
                legacy = args.project.parent / "Plugins/BF6UnrealSDK/Source/ThirdParty/libbf6/data/mapmesh"
                if not any(legacy.glob(args.level + "_*.bf6mesh")):
                    raise ValueError("No installed SDK map geometry for " + args.level + ". Import the SDK before benchmarking this map.")
        if args.save:
            saves = args.project.parent / "Saved/BF6UnrealSDK/saves"
            fixture = saves / "experiences" / args.save / "maps" / args.level / (args.level + ".json")
            if not fixture.exists():
                fixture = saves / args.save / (args.level + ".json")
            if not fixture.exists():
                raise ValueError("Saved map fixture not found: " + args.save + " / " + args.level)
            config["fixture_sha256"] = sha256(fixture)
            config["fixture_objects"] = len(json.loads(fixture.read_text(encoding="utf-8-sig")).get("objects", []))
        hardware = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
            "@{cpu=@(Get-CimInstance Win32_Processor | Select-Object Name,NumberOfCores,NumberOfLogicalProcessors); "
            "memoryBytes=(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory; "
            "gpu=@(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion)} | ConvertTo-Json -Depth 5"],
            capture_output=True, text=True, timeout=30, creationflags=0x08000000)
        (out / "hardware.json").write_text(hardware.stdout if hardware.returncode == 0 else json.dumps({"error": hardware.stderr}), encoding="utf-8")
        clone = prepare(args.project.resolve(), out.resolve(), args.cache)
        copy_tree(Path(__file__).parent, out.resolve() / "driver")
        config["test_project"] = str(clone)
        config_path = out.resolve() / "config.json"
        write_json(config_path, config)
        if args.prepare_only:
            print("Prepared. No editor launched.")
            return 0
        job = Job(config["cores"], args.cpu_percent, config["commit_limit_gib"])
        config["affinity_mask"] = hex(job.mask)
        config["host_logical_cpus"] = os.cpu_count()
        write_json(config_path, config)
        gpu = GPUCounters()
        command = [str(args.editor.resolve()), str(clone), "/Engine/Maps/Entry", "-unattended",
                   "-UserDir=" + str(clone.parent) + "/",
                   "-NoSplash", "-NoLiveCoding", "-NoRestoreOpenAssetTabs", "-NoSound",
                   "-windowed", "-ResX=1280", "-ResY=720", "-ForceRes", "-NoVSync",
                   "-abslog=" + str(out.resolve() / "editor.log"),
                   "-ExecutePythonScript=" + str(out.resolve() / "driver/editor_workflow.py")]
        command += engine_cpu_arguments(config["cores"], job.mask, args.engine_workers)
        write_json(out / "launch.json", dict(argv=command, gpu_counter_error=gpu.error))
        job.launch(command, args.editor.parent, {**os.environ, "BF6_STABILITY_CONFIG": str(config_path)}, args.visible)
        print(f"Editor PID {job.pid}; affinity {config['affinity_mask']}; commit limit {config['commit_limit_gib']} GiB (0 = none).", flush=True)
        start, announced = time.perf_counter(), ""
        completed_at = None
        with (out / "resources.jsonl").open("a", encoding="utf-8", buffering=1) as log:
            while (code := job.poll()) is None:
                elapsed = time.perf_counter() - start
                if elapsed > args.timeout:
                    raise TimeoutError(f"Editor exceeded {args.timeout}s total timeout")
                if completed_at and time.perf_counter() - completed_at > 90:
                    raise TimeoutError("Workflow finished but editor did not exit within 90 seconds")
                sample = job.sample()
                sample.update(seconds=elapsed, monotonic=time.perf_counter(), foreground=foreground_pid() == job.pid,
                              gpu=gpu.sample(sample["pids"]), windows=editor_windows(job.pid))
                samples.append(sample)
                log.write(json.dumps(sample) + "\n")
                result_path = out / "workflow.json"
                if result_path.exists() and completed_at is None:
                    if json.loads(result_path.read_text(encoding="utf-8")).get("finished"):
                        completed_at = time.perf_counter()
                heartbeat = out / "heartbeat.json"
                if heartbeat.exists():
                    state = json.loads(heartbeat.read_text(encoding="utf-8"))
                    if not completed_at and time.perf_counter() - state["monotonic"] > args.stage_timeout:
                        raise TimeoutError("No completed action within stage timeout: " + state["state"])
                    status = f"cycle {state['cycle']} / {state['state']}"
                    if status != announced:
                        if args.visible and state["state"] == "flight":
                            show_editor(job.pid)
                        print(status, flush=True)
                        announced = status
                time.sleep(1)
            if (out / "workflow.json").exists():
                workflow = json.loads((out / "workflow.json").read_text(encoding="utf-8"))
    except BaseException as exc:
        failure = f"{type(exc).__name__}: {exc}"
        (out / "launcher-error.txt").write_text(traceback.format_exc(), encoding="utf-8")
    finally:
        if job:
            job.close()  # Timeout, Ctrl+C or launcher error tears down descendants too.
        if gpu:
            gpu.close()
    if not workflow and (out / "workflow.json").exists():
        workflow = json.loads((out / "workflow.json").read_text(encoding="utf-8"))
    result = evaluate(workflow, samples, code, failure, args.visible, args.gpu_budget_gib)
    write_json(out / "report.json", result)
    (out / "REPORT.md").write_text(markdown(result, config), encoding="utf-8")
    # Portable diagnostics, without copying SDK/game geometry or full creator exports.
    names = ("REPORT.md", "report.json", "config.json", "launch.json", "hardware.json", "workflow.json",
             "editor.log", "events.jsonl", "resources.jsonl", "sdk-state.json", "highpoly-state.json", "launcher-error.txt")
    with zipfile.ZipFile(out / "diagnostics.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            if (out / name).is_file():
                archive.write(out / name, name)
        for path in sorted(out.glob("frames-*.json")):
            archive.write(path, path.name)
        for path in sorted(out.glob("render-timings-*.csv")):
            archive.write(path, path.name)
        for path in sorted((out / "project/Saved/Profiling/MemReports").rglob("*.memreport")):
            archive.write(path, "memory/" + path.name)
        for path in sorted((out / "driver").glob("*.py")):
            archive.write(path, "driver/" + path.name)
    print(f"Stability: {result['stability']}; load: {result['open_10s']}; flight: {result['flight_60fps']}; hitches: {result['hitches_100ms']}; GPU: {result['gpu_budget']}", flush=True)
    print("Report: " + str(out / "REPORT.md"), flush=True)
    # Performance misses are separate from workflow failure but still fail the run.
    return 0 if result["stability"] == "PASS" and all(result[k] != "FAIL" for k in ("open_10s", "flight_60fps", "hitches_100ms", "gpu_budget")) else 1


if __name__ == "__main__":
    sys.exit(main())
