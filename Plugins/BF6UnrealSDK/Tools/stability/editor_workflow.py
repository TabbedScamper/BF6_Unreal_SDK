"""Runs inside a disposable Unreal editor, driven by Slate ticks, never blocking waits."""
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback
import unreal

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).parent))
from report import frames_summary


class Workflow:
    def __init__(self):
        unreal.EditorPythonScripting.set_keep_python_script_alive(True)
        self.config = json.loads(Path(os.environ["BF6_STABILITY_CONFIG"]).read_text(encoding="utf-8"))
        self.out = Path(self.config["output"])
        self.result = dict(completed=False, finished=False, opens=[], flights=[], checks=[], snapshots=[])
        self.cycle = 0
        self.state = "boot"
        self.next_at = time.perf_counter() + 3
        self.last = time.perf_counter()
        self.frames = []
        self.engine_frames = []
        self.last_engine_frame = unreal.SystemLibrary.get_frame_count()
        self.last_engine_time = self.last
        self.in_tick = False
        self.finished = False
        self.actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        self.levels = unreal.EditorLoadingAndSavingUtils
        self.handle = unreal.register_slate_post_tick_callback(self.tick)
        self.event("started")

    def write(self, filename, data):
        path = self.out / filename
        temp = path.with_suffix(path.suffix + ".writing")
        temp.write_text(json.dumps(data, indent=2), encoding="utf-8")
        # Windows readers/virus scanners can briefly hold the destination
        # without FILE_SHARE_DELETE. A transient sharing violation must not
        # abort an otherwise valid editor workflow. Writes occur outside flight.
        for attempt in range(20):
            try:
                os.replace(temp, path)
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(.01)

    def event(self, event, **kwargs):
        with (self.out / "events.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps(dict(event=event, monotonic=time.perf_counter(), cycle=self.cycle, **kwargs)) + "\n")
        self.write("heartbeat.json", dict(state=self.state, monotonic=time.perf_counter(), cycle=self.cycle))
        self.write("workflow.json", self.result)
        unreal.log("BF6Stability: " + event)

    def command(self, command):
        self.event("command", command=command)
        unreal.SystemLibrary.execute_console_command(None, command)

    def snapshot(self):
        result = {}
        commands = [("sdk", "BF6.Test.State")]
        if self.config["mode"] == "high":
            commands += [("highpoly", "BF6.HighPoly.TestState")]
        for key, command in commands:
            path = self.out / (key + "-state.json")
            if path.exists():
                path.unlink()  # Never accept an old snapshot from a missing command.
            unreal.SystemLibrary.execute_console_command(None, command + ' "' + str(path) + '"')
            if not path.exists():
                raise RuntimeError(command + " did not write a state file. Rebuild the plugins.")
            result[key] = json.loads(path.read_text(encoding="utf-8-sig"))
        return result

    def finish(self, error=None):
        self.finished = True
        self.result["finished"] = True
        self.result["completed"] = error is None
        self.result["error"] = error
        self.write("workflow.json", self.result)
        self.event("failed" if error else "completed", error=error)
        unreal.unregister_slate_post_tick_callback(self.handle)
        # Never save the source project. The launcher copied Content, Config and Saves.
        unreal.SystemLibrary.quit_editor()

    def check(self, label, condition):
        record = next((r for r in self.result["checks"] if r.get("cycle") == self.cycle and r["name"] == label), None)
        if record is None:
            self.result["checks"].append(dict(name=label, cycle=self.cycle, passed=bool(condition)))
        else:
            record["passed"] = bool(condition)
        if not condition:
            raise RuntimeError("Assertion failed: " + label)

    def begin_open(self):
        self.cycle += 1
        self.ready_frame = None
        self.state = "opening"
        self.event("open_begin", level=self.config["level"])
        # Start BEFORE the same native entry point used by the map chooser.
        self.open_at = time.perf_counter()
        level, save = self.config["level"], self.config.get("save") or ""
        self.command(f'BF6.OpenSave {level}' + (f' "{save}"' if save else ""))
        if self.config["mode"] == "high":
            self.command("BF6.HighPoly.Build")
        elif self.config["has_highpoly"]:
            self.command("BF6.HighPoly.Detail low")
        self.state = "ready"
        self.next_at = 0

    def start_flight(self, snapshot):
        self.result["snapshots"].append(snapshot)
        self.event("ready", snapshot=snapshot)
        sdk = snapshot["sdk"]
        self.check("build viewport camera exists", sdk.get("hasCamera"))
        self.location = unreal.Vector(*sdk["cameraLocation"])
        p, y, r = sdk["cameraRotation"]
        self.rotation = unreal.Rotator(pitch=p, yaw=y, roll=r)
        if self.config.get("camera_start"):
            self.location = unreal.Vector(*self.config["camera_start"][:3])
            p, y, r = self.config["camera_start"][3:]
            self.rotation = unreal.Rotator(pitch=p, yaw=y, roll=r)
        self.set_camera(self.location, self.rotation)
        if self.config.get("trace"):
            self.command('Trace.File "' + str(self.out / ('flight-' + str(self.cycle) + '.utrace')) + '" cpu,frame,gpu,bookmark')
            self.command("Trace.RegionBegin BF6_Flight")
        if self.config["has_highpoly"]:
            self.command('BF6.HighPoly.PerfCapture start "' + str(self.out / ('render-timings-' + str(self.cycle) + '.csv')) + '"')
        self.flight_at = time.perf_counter()
        self.last = self.flight_at
        self.frames = []
        self.engine_frames = []
        self.last_engine_frame = unreal.SystemLibrary.get_frame_count()
        self.last_engine_time = self.flight_at
        self.state = "flight"
        self.event("flight_begin", camera_location=[self.location.x, self.location.y, self.location.z],
                   camera_rotation=[self.rotation.pitch, self.rotation.yaw, self.rotation.roll])

    def set_camera(self, pos, rot):
        # The native seam updates the actual tool view even after layout changes.
        # Avoid per-frame action logging or disk snapshots in measured intervals.
        command = "BF6.Test.Camera " + " ".join(str(v) for v in (pos.x, pos.y, pos.z, rot.pitch, rot.yaw, rot.roll))
        unreal.SystemLibrary.execute_console_command(None, command)

    def edit_save_reopen(self):
        # Exercise Unreal transaction, selection and map persistence in copied Content.
        # This is an editor smoke check, not certification of every SDK custom-object editor.
        self.check("create scratch world", self.levels.new_blank_map(False))
        with unreal.ScopedEditorTransaction("Stability test placement"):
            actor = self.actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(100, 200, 300))
            actor.set_actor_label("BF6_Stability_Probe")
            actor.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Cube"))
        self.actors.set_selected_level_actors([actor])
        self.check("select probe", actor in self.actors.get_selected_level_actors())
        before = actor.get_actor_location()
        with unreal.ScopedEditorTransaction("Stability test transform"):
            actor.modify()
            actor.set_actor_location(unreal.Vector(400, 500, 600), False, False)
        self.command("TRANSACTION UNDO")
        after = actor.get_actor_location()
        self.check("undo transform", (after - before).length() < .01)
        self.check("save disposable level", self.levels.save_map(actor.get_world(), "/Game/StabilityProbe"))
        self.check("open empty level", self.levels.load_map("/Engine/Maps/Entry"))
        self.check("reopen disposable level", self.levels.load_map("/Game/StabilityProbe"))
        matches = [a for a in self.actors.get_all_level_actors() if a.get_actor_label() == "BF6_Stability_Probe"]
        self.check("saved probe survives reopen", len(matches) == 1 and (matches[0].get_actor_location() - before).length() < .01)

    def finish_scene(self):
        if self.config.get("save"):
            exported = self.out / "experience-export.json"
            self.command('BF6.Project.Export "' + str(exported) + '"')
            self.check("experience export exists", exported.is_file())
            document = json.loads(exported.read_text(encoding="utf-8-sig"))
            self.check("experience export has maps", bool(document.get("mapRotation")))
        # Do not serialize thousands of transient game meshes as a smoke-test map.
        self.levels.load_map("/Engine/Maps/Entry")
        self.edit_save_reopen()
        self.finish()

    def tick(self, dt):
        # Slow-task dialogs can pump Slate recursively while a command is active.
        # Never issue another open/save/build from that nested callback.
        if self.in_tick or self.finished:
            return
        self.in_tick = True
        now = time.perf_counter()
        try:
            if now < self.next_at:
                return
            if self.state == "boot":
                self.command("t.MaxFPS 0")
                self.command("r.VSync 0")
                # The clone can render while unattended. Hidden runs are still NOT FPS acceptance.
                self.command("Slate.bAllowThrottling 0")
                self.command("t.IdleWhenNotForeground 0")
                self.command("r.TextureStreaming 1")
                self.command("r.Streaming.PoolSize " + str(self.config["texture_pool_mb"]))
                if self.config["has_highpoly"]:
                    self.command("BF6.HighPoly.TextureStreaming " + str(self.config.get("texture_streaming", 1)))
                    self.command("BF6.HighPoly.BuildBatch " + str(self.config.get("build_batch", 0)))
                    self.command("BF6.HighPoly.TerrainBatch " + str(self.config.get("terrain_batch", 16)))
                    self.command("BF6.HighPoly.GameLODs " + str(self.config.get("game_lods", 0)))
                    self.command("BF6.HighPoly.WaterAsync " + str(self.config.get("water_async", 1)))
                    self.command("BF6.HighPoly.CompactVertices " + str(self.config.get("compact_vertices", 1)))
                    profile = self.config.get("render_profile", "current")
                    if profile != "current":
                        self.command("BF6.HighPoly.Quality " + profile)
                    if self.config.get("nanite", "default") != "default":
                        self.command("BF6.HighPoly.Nanite " + self.config["nanite"])
                    self.command("r.Streaming.PoolSize " + str(self.config["texture_pool_mb"]))
                if self.config["mode"] == "smoke":
                    self.edit_save_reopen()
                    self.snapshot()  # Also verifies the installed native test bridge.
                    self.finish()
                else:
                    # Apply queued console-variable sinks while Entry is still
                    # empty. Slate can tick repeatedly within one engine frame;
                    # immediately opening the fixture defers a global quality
                    # rebuild until the first measured flight instead.
                    self.configured_frame = unreal.SystemLibrary.get_frame_count()
                    self.state = "configured"
                    self.event("configured")
            elif self.state == "configured":
                if unreal.SystemLibrary.get_frame_count() < self.configured_frame + 2:
                    return
                self.begin_open()
            elif self.state == "ready":
                state = self.snapshot()
                sdk = state["sdk"]
                self.check("correct level", sdk["level"] == self.config["level"])
                if self.config.get("save"):
                    self.check("correct save", sdk["save"] == self.config["save"])
                hp = state.get("highpoly", {})
                ready = sdk["actors"] > 0 and sdk["compiling"] == 0
                if hp:
                    ready = ready and hp["completed"] and hp["builtAnything"] and not (
                        hp["building"] or hp["coreBusy"] or hp["previewsBusy"] or hp["resolving"]
                        or hp.get("incompleteParentMaterials"))
                if not ready:
                    self.ready_frame = None
                    if now - self.open_at > self.config["stage_timeout"]:
                        raise RuntimeError("Open/build readiness timeout: " + json.dumps(state))
                    self.next_at = now + .5
                    return
                # Registration can leave initial context-mesh render updates
                # deferred until end of frame. Keep that work inside open time,
                # rather than starting the flight from a nested Slate callback.
                frame = unreal.SystemLibrary.get_frame_count()
                if self.ready_frame is None:
                    self.ready_frame = frame
                    return
                if frame < self.ready_frame + 2:
                    return
                self.check("placed fixture count", sdk["placed"] >= self.config["min_placed"])
                self.result["opens"].append(dict(cycle=self.cycle, seconds=time.perf_counter() - self.open_at))
                self.start_flight(state)
            elif self.state == "flight":
                elapsed = now - self.flight_at
                self.frames.append((now - self.last) * 1000)
                self.last = now
                frame = unreal.SystemLibrary.get_frame_count()
                if frame == self.last_engine_frame:
                    return  # Slate can tick multiple times inside one engine frame.
                self.engine_frames.append((now - self.last_engine_time) * 1000)
                self.last_engine_frame = frame
                self.last_engine_time = now
                # First half: stationary spin. Second half: reproducible orbit/fly path.
                duration = self.config["flight_seconds"]
                phase = elapsed / duration
                angle = phase * math.tau * 2
                radius = self.config["flight_radius_m"] * 100 if phase >= .5 else 0
                pos = unreal.Vector(self.location.x + math.sin(angle) * radius,
                                    self.location.y + (math.cos(angle) - 1) * radius, self.location.z)
                rot = unreal.Rotator(pitch=self.rotation.pitch, yaw=self.rotation.yaw + elapsed * 45, roll=self.rotation.roll)
                self.set_camera(pos, rot)
                if elapsed >= duration:
                    if self.config.get("trace"):
                        self.command("Trace.RegionEnd BF6_Flight")
                        self.command("Trace.Stop")
                    if self.config["has_highpoly"]:
                        self.command("BF6.HighPoly.PerfCapture stop")
                    self.result["flights"].append(dict(cycle=self.cycle, start=self.flight_at, end=now,
                        measurement="distinct_engine_frames", frames=frames_summary(self.engine_frames),
                        slate_callbacks=frames_summary(self.frames)))
                    self.write("frames-" + str(self.cycle) + ".json", self.engine_frames)
                    self.event("flight_complete", summary=self.result["flights"][-1])
                    after = self.snapshot()["sdk"]
                    self.check("camera actually moved", (unreal.Vector(*after["cameraLocation"]) - pos).length() < .1)
                    self.check("camera actually turned", all(abs((a - b + 180) % 360 - 180) < .01 for a, b in
                        zip(after["cameraRotation"], (rot.pitch, rot.yaw, rot.roll))))
                    self.set_camera(self.location, self.rotation)
                    if self.cycle < self.config["cycles"]:
                        self.begin_open()
                    else:
                        if self.config.get("memory_report"):
                            self.memory_dir = Path(unreal.Paths.project_saved_dir()) / "Profiling/MemReports"
                            self.memory_before = set(self.memory_dir.rglob("*.memreport"))
                            self.memory_at = time.perf_counter()
                            self.state = "memory_report"
                            self.command("MemReport -full")
                        else:
                            self.finish_scene()
            elif self.state == "memory_report":
                # MemReport is deferred by Unreal. Keep the populated scene alive
                # until it executes, otherwise the report describes the scratch map.
                reports = set(self.memory_dir.rglob("*.memreport")) - self.memory_before
                if reports:
                    self.event("memory_report_complete", paths=[str(p) for p in sorted(reports)])
                    self.finish_scene()
                elif now - self.memory_at > 60:
                    raise RuntimeError("Populated-scene memory report timed out")
        except BaseException:
            self.finish(traceback.format_exc())
        finally:
            self.in_tick = False


try:
    WORKFLOW = Workflow()  # Keep callback owner alive until the final report is written.
except BaseException:
    config = json.loads(Path(os.environ["BF6_STABILITY_CONFIG"]).read_text())
    Path(config["output"], "workflow.json").write_text(json.dumps(dict(
        completed=False, finished=True, error=traceback.format_exc(), opens=[], flights=[], checks=[])))
    unreal.SystemLibrary.quit_editor()
