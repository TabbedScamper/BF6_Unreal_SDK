# Local stability testing

## High Poly performance comparisons

`Test-LowSpec.ps1` also accepts `-RenderProfile current|performance|balanced`, `-TextureStreaming 0|1`, `-Nanite default|on|off`, `-GameLODs 0|1`, `-BuildBatch 0|32|64|128|256`, `-TerrainBatch 4|8|16|32|64|128|256`, `-WaterAsync 0|1`, `-CompactVertices 0|1`, and `-MemoryReport`. The game LOD option is experimental and defaults off. Keep the fixture, camera, viewport dimensions and binary versions identical when comparing settings.

For sequential comparisons, run `Tools/stability/matrix.py` with Unreal's Python and supply `--project`, `--editor`, `--output-root`, and the same `--save`, `--level`, `--min-placed`, `--camera-start` and `--visible` arguments as the individual driver. Its default cases are resident textures, streaming textures, the Performance preset, authored LODs, Nanite off, and a 12 GiB commit ceiling. `--cases` can select a subset. Additional controls are `pressure10`, `pressure8`, `sync-water`, `unshared-vertices`, `terrain-unbatched`, `host-workers` and `low-pressure12`. Every case has a fresh disposable project. A failed case does not suppress the remaining cases. Results, including hitch verdicts, are collected into `matrix.json` and `matrix.csv`.

High Poly readiness also checks that its built parent-material shader maps are complete. A zero pending compilation count alone can hide shader jobs that were never submitted.

The driver lets queued rendering settings take effect in the empty entry map before opening the fixture. It also keeps two engine-frame boundaries after readiness indicators become true inside the open timer, allowing initial deferred context-mesh updates to run before the flight. This is not a GPU presentation fence. Earlier reports did not include these boundaries; do not describe the resulting reduction in measured flight stalls as a plugin speedup.

The additional `quick-exit` matrix case uses a five-second Low Poly flight under the 12 GiB cap before export, unload and exit. It exercises shutdown while background mining may still be active. The driver allows up to 90 seconds for a clean exit and grades an access violation after successful editing as a failed workflow.

Flight reports now sample distinct engine frames rather than counting multiple Slate callbacks within one engine frame. Raw callback summaries remain available for comparison with older reports. Rendering counters are saved in `render-timings-*.csv` when the matching High Poly build supports capture. `-MemoryReport` runs Unreal's full memory report after the final measured flight. Neither operation measures presented frames on a real target GPU.

The tool creates its own window and may override command-line resolution. Do not treat requested `ResX`/`ResY` as verified viewport dimensions. Resource samples include observed window/client dimensions; these include the surrounding controls. The September 9 development runs used a measured 1800 by 1200 tool window despite requesting 1280 by 720.

`-Trace` adds local `flight-*.utrace` files for Unreal Insights. It marks each measured interval as the `BF6_Flight` region so buffered startup events can be excluded. Tracing adds overhead; compare untraced runs for final acceptance. Trace files stay beside the report and are not included in the support ZIP. Insights' `TimingInsights.ExportTimingEvents <path> -threads=GameThread -region=BF6_Flight` exports that interval. UE 5.8's timer-statistics exporter includes GPU queue aggregates even with a CPU thread filter, so do not treat every row of its statistics CSV as CPU work.

## Running an individual test

Run `Tools/Test-LowSpec.ps1` from this plugin to exercise the installed tool and High Poly under repeatable resource pressure. Windows and the matching compiled plugin versions are required. It uses Unreal's bundled Python and needs no additional Python packages.

Close other Unreal editors first. Each run makes a disposable project with copied Content, Config, Binaries, creator saves, offline experience documents/attachments and installed SDK geometry. Allow several gigabytes of disk space per run. The plugin directory is shared through a junction. Tests write into the disposable project, use a separate browser cache, retain their reports locally, and upload nothing. They do not rebuild plugin binaries.

## Run a test

From the project directory:

```powershell
# Verify selection, transaction undo, saving and reopening in a small scratch level.
& .\Plugins\BF6UnrealSDK\Tools\Test-LowSpec.ps1 -Mode smoke -Profile six-core

# Open the same map twice; spin in place and fly a fixed route after each open.
& .\Plugins\BF6UnrealSDK\Tools\Test-LowSpec.ps1 -Mode low -Profile six-core -Level MP_Dumbo -Visible

# Include the normal High Poly BUILD route and wait for actual finalization.
& .\Plugins\BF6UnrealSDK\Tools\Test-LowSpec.ps1 -Mode high -Profile six-core -Level MP_Dumbo -Visible

# Exercise an existing imported experience copied from your local saves.
# Use its exact save identifier, as shown by BF6.Project.Status in the editor.
& .\Plugins\BF6UnrealSDK\Tools\Test-LowSpec.ps1 -Mode high -Profile pressure12 -Level MP_Dumbo -Save 'Airport TDM' -MinPlaced 1 -Visible
```

Pass `-Project 'C:\path\Tool.uproject'` and `-Editor 'C:\path\Engine\Binaries\Win64\UnrealEditor.exe'` if automatic discovery is unsuitable. Keep the benchmark viewport visible, unobstructed and foreground for performance comparisons. Hidden runs still exercise stability but report flight timing as NOT_TESTED.

Available profiles:

| Profile | CPU affinity | Combined editor/worker commit ceiling |
| --- | --- | --- |
| baseline | All available CPUs | None |
| six-core | One logical processor on each of six physical cores | None |
| pressure12 | Six physical cores | 12 GiB |
| pressure10 | Six physical cores | 10 GiB |
| pressure8 | Six physical cores | 8 GiB |

Start with baseline and six-core, then descend through the pressure profiles. A commit ceiling deliberately injects allocation failures. It does not recreate a machine with that amount of physical RAM, its pagefile, or its paging behavior. Leave other applications out of the benchmark.

`-CpuPercent 10` optionally limits the process tree to 10% of total host CPU capacity. This is independent of affinity and is not a calibrated Ryzen 2600 or i5-8400 simulation. The default leaves that quota off. The launcher assigns the editor to its Windows Job while suspended, before any workers start. CPU/memory limits and teardown therefore cover descendants as well as the editor. Machines spanning more than 64 logical CPUs are rejected instead of assigning an incorrect mask.

With `-EngineWorkers profile` (the default), restricted profiles also pass Unreal's `-corelimit=6` so engine worker pools reflect six available CPUs. Affinity alone leaves Unreal sizing pools from the host processor; the initial September 9 runs therefore created 16 shader workers despite six-core affinity. `-EngineWorkers host` preserves that oversubscription as a stress control. Keep this setting consistent in comparisons. Neither configuration reproduces a particular processor's speed or SMT behavior.

When Windows confirms the exact interleaved physical-core mask assumed by Unreal's `-processaffinityphysical` option, the launcher supplies that hint too. This prevents engine worker-affinity requests from conflicting with the job's mask. Other CPU topologies retain the Windows job restriction without using that assumption.

Useful options: `-Cycles 3`, `-FlightSeconds 60`, `-Timeout 1200`, `-OutputRoot 'D:\BF6Tests'`, and `-PrepareOnly`. The Python runner exposes additional stage-timeout, camera radius, texture pool and GPU-threshold settings through `--help`. The default 1536 MiB texture pool is consistent between profiles. It is not a limit on total GPU memory.

Use the Python runner's `--camera-start X Y Z PITCH YAW ROLL` to benchmark a specific busy location, in Unreal centimetres/degrees. Without it, the route starts at the tool's map-open camera; do not assume this covers every part of a map. The native camera command updates the actual build view after layout changes and the workflow reads its final position back.

The PowerShell equivalent is `-CameraStart X,Y,Z,Pitch,Yaw,Roll`. For example, a fixture-specific camera near Airport TDM's team spawn is `-CameraStart -1769,-37314,13484,-10,90,0`. Use coordinates appropriate to your own map.

## Cache policy and timing

The default `-Cache fresh-highpoly` creates an empty High Poly cache and carries over only the selected game installation. Opening the first map exercises that fresh cache. Later cycles in the same process exercise reopening and memory retention. `-Cache copy-existing` copies the source project's current High Poly cache into the disposable project instead. Neither option clears the Windows file cache or Unreal's shared Derived Data Cache. Compare first opens across fresh processes separately from repeated opens.

The level timer begins immediately before the native map-open request, not at editor launch or after loading. High Poly timing includes the normal BUILD route, placed-object/loadout finalization, library work and outstanding asset compilation. A cancelled, partial or absent high-poly build cannot satisfy its completion check. The 10-second target includes all of this work; the harness does not shorten it by prebuilding a map.

## Reports and acceptance

Every run writes a new folder under `Saved/StabilityRuns` unless `-OutputRoot` is supplied. Read `REPORT.md` first. `report.json`, `config.json`, `resources.jsonl`, `events.jsonl`, per-flight frame intervals, native state snapshots and `editor.log` retain the underlying evidence. A crash, timeout, missing workflow result or failed assertion fails stability. A nonzero editor exit after a successful workflow also fails. A missed performance target fails the launcher exit code separately from stability.

The default performance goals are open-to-ready within 10 seconds, flight p95 editor frame interval at or below 16.67 ms, and sampled process-tree dedicated GPU allocation within 4.5 GiB. This last value is an investigation threshold for a 6 GB target card, not enforced VRAM emulation. Vendor-neutral WDDM counters cover supported Nvidia, AMD and Intel drivers. Missing counters remain unknown, never zero. Their per-process accounting can double-count shared allocations; use a GPU capture and actual target hardware to resolve borderline results.

Slate callback intervals capture responsiveness during stationary spinning and camera movement. They are not GPU timings or proof that every frame was presented. The report lists 33 ms, 100 ms and 500 ms hitches, average rate, p95 and p99. A separate zero-hitch gate fails when any interval exceeds 100 ms, even if average FPS looks good. Background, minimized, remote-desktop and occluded viewport runs cannot establish real GPU performance. Visible tests attempt to foreground the tool at the start of each flight; FPS acceptance stays NOT_TESTED unless at least 90% of sampled flight time was foreground.

High Poly snapshots report actual `IsStreamable()` registration and resident mip memory for its material texture cache. This is a subset of total GPU memory and excludes other engine textures, render targets, geometry and driver allocations. It is useful for detecting textures that cannot shed mips under pressure.

The final edit check selects and transforms a scratch cube, undoes the transform, saves the disposable level and verifies the same object and transform after reopening. Map cycles use the real SDK open and add-on BUILD commands. Runs with a saved fixture also export the experience and require a readable JSON document with a map rotation. These tests do not yet certify every custom-object editor, attachment combination, material appearance, Portal upload, or import format. They do not click through account login or publish modes.

Keep reports from a repeatable fixture before and after fixes. Real RX 5600 XT, RTX 2060 and Arc A380 runs are still needed for driver and GPU acceptance. Process restrictions on a faster workstation cannot reproduce those cards.

`diagnostics.zip` collects the report, logs, measurements and test driver for local support handoff. It excludes copied SDK/game geometry and the full exported experience. Nothing sends it automatically. Each run also records hardware/driver information, plugin binary hashes and the saved fixture hash for comparison.

## Maintain the tests

For memory comparisons, the Python runner accepts `--release-hidden-context 0`
and `--generated-texture-backing 0` to disable the respective memory reductions.
Both default to `1`. Apply these before opening the map, and keep all other
settings and the saved fixture identical between runs. Compare process-tree
private bytes during the same flight interval as well as peak job commit.

Add `--context-recovery` to a High Poly run to switch the populated map to Low
Poly and back twice after measurement. It checks that both SDK context meshes
regain drawing sections at their original locations and release those sections
again in High Poly. Restoration time is recorded in `events.jsonl`. Placement
ray preservation is covered separately by the native automation test
`BF6.Editor.ContextBufferRecovery`. `BF6.HighPoly.Streaming.GeneratedExactBacking`
checks exact texture bytes, including GPU readback when run with a real RHI.
Experience-export checks accept Unreal's UTF-8 and UTF-16 JSON output.

Run `Tools/stability/test_stability.py` with Unreal's bundled Python. It verifies real descendant job membership, physical-core affinity, allocation failure under a commit ceiling, worker teardown, CPU quota installation, and strict report verdicts. Rebuild both plugins after changing their test-state console commands.

Run folders include a `project/Plugins` junction to your installation. Remove that junction itself before recursively deleting the remaining disposable project. The harness deliberately performs no automatic recursive cleanup.

Windows control semantics: [Job memory limits](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information) and [CPU rate control](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information).
