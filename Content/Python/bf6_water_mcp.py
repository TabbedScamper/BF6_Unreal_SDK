"""Live BF6 water inspection/control exposed through UE 5.8's MCP registry.

This module never reads an exported intermediate.  It inspects the editor's
live transient water materials and delegates reload to BF6HighPoly, whose
reader goes back to the installed Steam game.
"""

from __future__ import annotations

import json
import ctypes
import os
import re
from datetime import datetime
from pathlib import Path

import toolset_registry
import unreal


# Automation screenshots complete on a later editor tick.  Keep the task
# objects alive until process shutdown; dropping the final Python reference
# made UE 5.8 report "started" without ever committing the PNG.
_pending_screenshot_tasks = []


_TOOLSET_CLASS = None


def _world():
    return unreal.EditorLevelLibrary.get_editor_world()


def _actors():
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return subsystem.get_all_level_actors() if subsystem else []


def _name(obj) -> str:
    try:
        return obj.get_name()
    except Exception:
        return "<invalid>"


def _component_tags(component) -> list[str]:
    try:
        return [str(tag) for tag in component.get_editor_property("component_tags")]
    except Exception:
        return []


def _scalar(mid, parameter: str):
    try:
        return float(mid.get_scalar_parameter_value(parameter))
    except Exception:
        return None


def _texture_name(mid, parameter: str):
    try:
        texture = mid.get_texture_parameter_value(parameter)
        return _name(texture) if texture else None
    except Exception:
        return None


def _vector(mid, parameter: str):
    try:
        value = mid.get_vector_parameter_value(parameter)
        return [float(value.r), float(value.g), float(value.b), float(value.a)]
    except Exception:
        return None


def _debug_startup_report() -> dict:
    """Measure the active editor from its own current-session log and scene."""
    log_dir = Path(unreal.Paths.project_log_dir())
    logs = sorted(log_dir.glob("*.log"), key=lambda path: path.stat().st_mtime,
                  reverse=True)
    if not logs:
        return {"ready": False, "error": "No current Unreal log was found"}

    log_path = logs[0]
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    lines = log_text.splitlines()
    stamp_re = re.compile(
        r"^\[(\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}:\d{3})\]")

    def stamp(line):
        match = stamp_re.match(line)
        return (datetime.strptime(match.group(1), "%Y.%m.%d-%H.%M.%S:%f")
                if match else None)

    # UE's "Log file open" line has no bracket timestamp. The first stamped
    # line is emitted in the same startup millisecond and is the stable launch
    # origin available inside the editor-owned log.
    opened = next((stamp(line) for line in lines if stamp(line)), None)
    engine = next((stamp(line) for line in lines if "Startup complete" in line), None)
    water_live_lines = [line for line in lines if "GAME VIEW LIVE:" in line]
    full_live_lines = [line for line in lines if "LogBF6HighPoly:" in line and
                       "MP_Isolated:" in line and "placement" in line]
    phase_lines = [line for line in lines if "FULL LOAD PHASES:" in line]
    live_lines = full_live_lines if full_live_lines else water_live_lines
    live = stamp(live_lines[-1]) if live_lines else None
    warnings = [line for line in lines if ": Warning:" in line]
    errors = [line for line in lines if ": Error:" in line or "EnsureFailed:" in line]

    water_actors = []
    component_count = 0
    instance_count = 0
    layer_counts = {
        "terrain": 0, "roads": 0, "objects": 0,
        "scatter": 0, "water": 0, "lights": 0, "fx": 0,
    }
    for actor in _actors():
        label = actor.get_actor_label()
        if label not in ("BF6 RAW WATER STACK (runtime game data)", "HighPoly"):
            continue
        water_actors.append(label)
        try:
            components = actor.get_components_by_class(unreal.StaticMeshComponent)
            component_count += len(components)
            for component in components:
                component_name = _name(component)
                if component_name.startswith("TerrainMesh_"):
                    layer_counts["terrain"] += 1
                elif component_name.startswith(("Road_", "RoadMesh_")):
                    layer_counts["roads"] += 1
                elif component_name.startswith("Water_"):
                    layer_counts["water"] += 1
                elif component_name.startswith("Scatter_"):
                    layer_counts["scatter"] += 1
                elif component_name.startswith("I_"):
                    layer_counts["objects"] += 1
                elif "FX" in component_name or "Effect" in component_name:
                    layer_counts["fx"] += 1
                getter = getattr(component, "get_instance_count", None)
                if getter:
                    instance_count += int(getter())
            layer_counts["lights"] += len(
                actor.get_components_by_class(unreal.LightComponent))
        except Exception:
            pass

    def elapsed(end):
        return round((end - opened).total_seconds(), 3) if opened and end else None

    required_layers = {
        key: value > 0 for key, value in layer_counts.items()
    }
    launch_to_ready = elapsed(live)
    acceptance = {
        "engine_under_10_seconds": elapsed(engine) is not None and elapsed(engine) <= 10.0,
        "full_high_poly_under_10_seconds": launch_to_ready is not None and launch_to_ready <= 10.0,
        "zero_warnings": len(warnings) == 0,
        "zero_errors": len(errors) == 0,
        "all_required_layers_present": all(required_layers.values()),
    }
    acceptance["passed"] = all(acceptance.values())

    return {
        "ready": bool(live and water_actors),
        "mode": "isolated_raw_water_debug" if any(
            label.startswith("BF6 RAW WATER") for label in water_actors) else "full_high_poly",
        "runtime_input": "current Steam install; no exported intermediate",
        "log": str(log_path),
        "launch_to_engine_seconds": elapsed(engine),
        "launch_to_high_poly_ready_seconds": launch_to_ready,
        "warning_count": len(warnings),
        "error_count": len(errors),
        "warnings": warnings[:20],
        "errors": errors[:20],
        "water_actor_labels": water_actors,
        "static_mesh_component_count": component_count,
        "instanced_mesh_instance_count": instance_count,
        "layer_component_counts": layer_counts,
        "required_full_layers_present": required_layers,
        "acceptance": acceptance,
        "full_phase_status": phase_lines[-1] if phase_lines else None,
        "live_status": live_lines[-1] if live_lines else None,
    }


def _lighting_status() -> dict:
    """Inspect the built scene's lighting consumers without changing it."""
    sky_rows = []
    directional_rows = []
    fog_rows = []
    post_rows = []
    local_light_intensities = []
    cloud_projection_components = []
    for actor in _actors():
        try:
            actor_label = actor.get_actor_label()
            components = actor.get_components_by_class(unreal.ActorComponent)
        except Exception:
            continue
        if isinstance(actor, unreal.PostProcessVolume):
            row = {"actor": actor_label}
            for key in ("enabled", "unbound", "blend_weight", "priority"):
                try:
                    value = actor.get_editor_property(key)
                    row[key] = float(value) if isinstance(value, (int, float)) else bool(value)
                except Exception:
                    row[key] = None
            try:
                settings = actor.get_editor_property("settings")
            except Exception:
                settings = None
            for key in (
                "auto_exposure_bias",
                "auto_exposure_min_brightness",
                "auto_exposure_max_brightness",
                "color_grading_intensity",
                "white_temp",
                "ambient_occlusion_intensity",
                "ambient_occlusion_radius",
            ):
                try:
                    value = settings.get_editor_property(key) if settings else None
                    row[key] = float(value) if isinstance(value, (int, float)) else value
                except Exception:
                    row[key] = None
            try:
                lut = settings.get_editor_property("color_grading_lut") if settings else None
                row["color_grading_lut"] = _name(lut) if lut else None
            except Exception:
                row["color_grading_lut"] = None
            post_rows.append(row)
        for component in components:
            component_name = _name(component)
            if component_name.startswith("Light_CloudShadow"):
                cloud_projection_components.append(
                    f"{actor_label}/{component_name}")
            if component_name == "Light_SkyDome":
                try:
                    material = component.get_material(0)
                except Exception:
                    material = None
                sky_rows.append({
                    "component": f"{actor_label}/{component_name}",
                    "material": _name(material) if material else None,
                    "panorama": _texture_name(material, "Panorama") if material else None,
                    "rotation_turns": _scalar(material, "SkyRotationTurns") if material else None,
                    "v_min": _scalar(material, "SkyVMin") if material else None,
                    "v_max": _scalar(material, "SkyVMax") if material else None,
                    "emissive_scale": _scalar(material, "SkyEmissiveScale") if material else None,
                    "flow_enabled": _scalar(material, "SkyFlowEnabled") if material else None,
                    "flow_mask": _texture_name(material, "SkyFlowMask") if material else None,
                    "flow_distance": _scalar(material, "SkyFlowDistance") if material else None,
                    "flow_direction_deg": _scalar(material, "SkyFlowDirectionDeg") if material else None,
                    "flow_period_seconds": _scalar(material, "SkyFlowPeriodSeconds") if material else None,
                    "flow_height_scale": _scalar(material, "SkyFlowHeightScale") if material else None,
                    "flow_height_bias": _scalar(material, "SkyFlowHeightBias") if material else None,
                })
            if isinstance(component, unreal.DirectionalLightComponent):
                row = {"component": f"{actor_label}/{component_name}"}
                for key in ("intensity", "temperature", "use_temperature",
                            "light_source_angle", "dynamic_shadow_distance_movable_light"):
                    try:
                        value = component.get_editor_property(key)
                        row[key] = float(value) if isinstance(value, (int, float)) else str(value)
                    except Exception:
                        row[key] = None
                directional_rows.append(row)
            if isinstance(component, unreal.LocalLightComponent):
                try:
                    local_light_intensities.append(
                        float(component.get_editor_property("intensity")))
                except Exception:
                    pass
            if isinstance(component, unreal.ExponentialHeightFogComponent):
                row = {"component": f"{actor_label}/{component_name}"}
                for key in ("fog_density", "fog_height_falloff", "start_distance",
                            "fog_max_opacity", "volumetric_fog"):
                    try:
                        value = component.get_editor_property(key)
                        row[key] = float(value) if isinstance(value, (int, float)) else bool(value)
                    except Exception:
                        row[key] = None
                fog_rows.append(row)
            if isinstance(component, unreal.PostProcessComponent):
                # Component-owned post effects are distinct from the
                # APostProcessVolume actor handled above.
                row = {"component": f"{actor_label}/{component_name}"}
                for key in ("enabled", "unbound", "blend_weight", "priority"):
                    try:
                        value = component.get_editor_property(key)
                        row[key] = (float(value) if isinstance(value, (int, float))
                                    else bool(value))
                    except Exception:
                        row[key] = None
                try:
                    settings = component.get_editor_property("settings")
                except Exception:
                    settings = None
                for key in (
                    "auto_exposure_bias",
                    "auto_exposure_min_brightness",
                    "auto_exposure_max_brightness",
                    "color_grading_intensity",
                    "white_temp",
                    "ambient_occlusion_intensity",
                    "ambient_occlusion_radius",
                ):
                    try:
                        value = settings.get_editor_property(key) if settings else None
                        row[key] = (float(value) if isinstance(value, (int, float))
                                    else value)
                    except Exception:
                        row[key] = None
                post_rows.append(row)
    sky_flow_present = any(row.get("flow_mask") for row in sky_rows)
    local_light_intensities.sort()
    local_summary = {
        "count": len(local_light_intensities),
        "min": local_light_intensities[0] if local_light_intensities else None,
        "median": (local_light_intensities[len(local_light_intensities) // 2]
                   if local_light_intensities else None),
        "max": local_light_intensities[-1] if local_light_intensities else None,
    }
    return {
        "runtime_input": "current Steam install; no exported intermediate",
        "sky": sky_rows,
        "directional_lights": directional_rows,
        "local_light_intensity_summary": local_summary,
        "height_fog": fog_rows,
        "post_process_components": post_rows,
        "sky_flow_consumer_present": sky_flow_present,
        "cloud_shadow_projection_components": cloud_projection_components,
        "cloud_shadow_projection_present": bool(cloud_projection_components),
        "honest_gaps": [
            *( [] if cloud_projection_components else [
                "Both game cloud-shadow textures decode, but no Unreal ground-projection consumer is active yet."
            ]),
            *( [] if sky_flow_present else [
                "The game sky flow mask and motion constants decode, but the Unreal sky material does not consume them yet."
            ]),
        ],
        "camera_unchanged": _viewport_camera(),
    }


def _parent_wpo_evidence(parent) -> dict:
    evidence = {
        "custom_expression_count": 0,
        "has_broad_crest_code": False,
        "has_foam_wave_height_input": False,
        "has_independent_carrier": False,
        "has_foam_shoulder_ramp": False,
        "has_spatially_filtered_crest": False,
        "has_broad_wave_size_input": False,
        "has_broad_time_scale_input": False,
        "has_fast_sheet_gate": False,
        "uses_rejected_triangle_broad_normal": False,
        "uses_pixel_broad_normal": False,
    }
    if not parent:
        return evidence
    # Inspect the connected Normal root directly.  Scanning the expression
    # collection is useful corroboration, but graph ownership/serialization can
    # omit an otherwise connected transient expression from that Python view.
    try:
        normal_root = unreal.MaterialEditingLibrary.get_material_property_input_node(
            parent, unreal.MaterialProperty.MP_NORMAL)
        evidence["normal_root_node"] = _name(normal_root) if normal_root else None
        if isinstance(normal_root, unreal.MaterialExpressionCustom):
            normal_code = normal_root.get_editor_property("code") or ""
            evidence["uses_rejected_triangle_broad_normal"] = (
                "ddx(BroadHeightCm)" in normal_code)
            evidence["uses_pixel_broad_normal"] = (
                "ddx(broadHeightM)" in normal_code and
                "Texture2DSampleLevel(BroadPattern" in normal_code)
            evidence["normal_input_count"] = len(
                normal_root.get_editor_property("inputs"))
    except Exception as exc:
        evidence["normal_root_inspection_error"] = str(exc)
    try:
        root = unreal.MaterialEditingLibrary.get_material_property_input_node(
            parent, unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)
        evidence["root_node"] = _name(root) if root else None
        if isinstance(root, unreal.MaterialExpressionCustom):
            code = root.get_editor_property("code") or ""
            evidence["has_broad_crest_code"] = "crestM" in code and "broadCoverage" in code
            evidence["has_independent_carrier"] = (
                "independentCarrier" in code and "waveProfile" in code)
            evidence["has_foam_shoulder_ramp"] = (
                "foamShoulder" in code and "waveProfile" in code)
            evidence["has_spatially_filtered_crest"] = (
                "broadMip" in code and "log2(carrierSize)" in code)
            evidence["has_fast_sheet_gate"] = "UseSheets*(" in code
            inputs = root.get_editor_property("inputs")
            names = [str(item.get_editor_property("input_name")) for item in inputs]
            evidence["has_foam_wave_height_input"] = "FoamWaveHeight" in names
            evidence["has_broad_wave_size_input"] = "BroadWaveSize" in names
            evidence["has_broad_time_scale_input"] = "BroadTimeScale" in names
            evidence["input_count"] = len(names)
    except Exception as exc:
        evidence["root_inspection_error"] = str(exc)
    try:
        collection = parent.get_editor_property("expression_collection")
        expressions = collection.get_editor_property("expressions")
    except Exception as exc:
        evidence["inspection_error"] = str(exc)
        return evidence
    for expression in expressions:
        if not isinstance(expression, unreal.MaterialExpressionCustom):
            continue
        evidence["custom_expression_count"] += 1
        try:
            code = expression.get_editor_property("code") or ""
        except Exception:
            code = ""
        if "crestM" in code and "broadCoverage" in code:
            evidence["has_broad_crest_code"] = True
        if "independentCarrier" in code and "waveProfile" in code:
            evidence["has_independent_carrier"] = True
        if "foamShoulder" in code and "waveProfile" in code:
            evidence["has_foam_shoulder_ramp"] = True
        if "broadMip" in code and "log2(carrierSize)" in code:
            evidence["has_spatially_filtered_crest"] = True
        if "UseSheets*(" in code:
            evidence["has_fast_sheet_gate"] = True
        if "ddx(BroadHeightCm)" in code:
            evidence["uses_rejected_triangle_broad_normal"] = True
        if ("ddx(broadHeightM)" in code and
                "Texture2DSampleLevel(BroadPattern" in code):
            evidence["uses_pixel_broad_normal"] = True
        try:
            inputs = expression.get_editor_property("inputs")
            names = [str(item.get_editor_property("input_name")) for item in inputs]
            if "FoamWaveHeight" in names:
                evidence["has_foam_wave_height_input"] = True
            if "BroadWaveSize" in names:
                evidence["has_broad_wave_size_input"] = True
            if "BroadTimeScale" in names:
                evidence["has_broad_time_scale_input"] = True
        except Exception:
            pass
    return evidence


def _water_materials():
    """Return unique live water MIDs and where each one is used."""
    found = {}
    for actor in _actors():
        try:
            actor_label = actor.get_actor_label()
            components = actor.get_components_by_class(unreal.StaticMeshComponent)
        except Exception:
            continue
        for component in components:
            component_name = _name(component)
            if not (component_name.startswith("Water_") or
                    component_name == "AccumulatedGameWater" or
                    component_name.startswith("RawLayer")):
                continue
            try:
                material_count = component.get_num_materials()
            except Exception:
                material_count = 1
            for slot in range(material_count):
                try:
                    material = component.get_material(slot)
                except Exception:
                    continue
                if not isinstance(material, unreal.MaterialInstanceDynamic):
                    continue
                key = material.get_path_name()
                entry = found.setdefault(key, {
                    "material": material,
                    "uses": [],
                })
                entry["uses"].append(f"{actor_label}/{component_name}[{slot}]")
    return list(found.values())


def _ground_materials():
    """Return unique live terrain MIDs and the terrain components using them."""
    found = {}
    for actor in _actors():
        try:
            actor_label = actor.get_actor_label()
            components = actor.get_components_by_class(unreal.StaticMeshComponent)
        except Exception:
            continue
        for component in components:
            component_name = _name(component)
            if not component_name.startswith("TerrainMesh_"):
                continue
            try:
                material_count = component.get_num_materials()
            except Exception:
                material_count = 1
            for slot in range(material_count):
                try:
                    material = component.get_material(slot)
                except Exception:
                    continue
                if not isinstance(material, unreal.MaterialInstanceDynamic):
                    continue
                key = material.get_path_name()
                entry = found.setdefault(key, {"material": material, "uses": []})
                entry["uses"].append(
                    f"{actor_label}/{component_name}[{slot}]")
    return list(found.values())


def _set_ground_scalar(parameter: str, value: float) -> int:
    count = 0
    for entry in _ground_materials():
        entry["material"].set_scalar_parameter_value(parameter, value)
        count += 1
    bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
    if bridge:
        bridge.redraw_visible_level_viewport()
    else:
        unreal.EditorLevelLibrary.editor_invalidate_viewports()
    return count


def _status_dict() -> dict:
    materials = _water_materials()
    geometry = []
    for actor in _actors():
        if actor.get_actor_label() != "HighPoly":
            continue
        for component in actor.get_components_by_class(unreal.StaticMeshComponent):
            component_name = _name(component)
            if not component_name.startswith("Water_"):
                continue
            instance_getter = getattr(component, "get_instance_count", None)
            geometry.append({
                "component": component_name,
                "class": component.get_class().get_name(),
                "instances": int(instance_getter()) if instance_getter else 1,
                "streaming_contract": (
                    "camera-snapped ring instances for Water_0; the authored "
                    "WaterSurfaceEntityData rectangle remains the exact clip"
                ),
            })
    result = {
        "world": _name(_world()) if _world() else None,
        "water_material_count": len(materials),
        "water_geometry": geometry,
        "materials": [],
        "interpretation": (
            "BF6WaveAmplitudeScale is recovered FFT displacement. "
            "BF6InteractiveBridge* is an explicitly provisional stand-in for the "
            "missing WaterInteractiveDisplacement atlas and is never evidence of "
            "exact closure. BF6FoamWaveHeightM remains the retracted foam-as-geometry "
            "control. BF6BroadCarrierSize and BF6BroadTimeScale are "
            "diagnostic pixel-graph multipliers, not decoded BF6 values; raw is 1. "
            "The exact BF6 tile-list producer for t26-t29 remains unimplemented."
        ),
    }
    for entry in materials:
        mid = entry["material"]
        parent = None
        try:
            parent = mid.get_editor_property("parent")
        except Exception:
            pass
        result["materials"].append({
            "name": _name(mid),
            "parent": _name(parent) if parent else None,
            "parent_wpo_evidence": _parent_wpo_evidence(parent),
            "uses": entry["uses"],
            "BF6WaveAmplitudeScale": _scalar(mid, "BF6WaveAmplitudeScale"),
            "BF6InteractiveBridgeEnabled": _scalar(mid, "BF6InteractiveBridgeEnabled"),
            "BF6InteractiveBridgeHeightM": _scalar(mid, "BF6InteractiveBridgeHeightM"),
            "BF6InteractiveBridgeLengthM": _scalar(mid, "BF6InteractiveBridgeLengthM"),
            "BF6InteractiveBridgeRate": _scalar(mid, "BF6InteractiveBridgeRate"),
            "BF6FoamWaveHeightM": _scalar(mid, "BF6FoamWaveHeightM"),
            "BF6BroadCarrierSize": _scalar(mid, "BF6BroadCarrierSize"),
            "BF6BroadTimeScale": _scalar(mid, "BF6BroadTimeScale"),
            "BF6FoamCrestStart": _scalar(mid, "BF6FoamCrestStart"),
            "BF6FoamCrestFull": _scalar(mid, "BF6FoamCrestFull"),
            "FoamCoverage": _scalar(mid, "FoamCoverage"),
            "FoamRoughness": _scalar(mid, "FoamRoughness"),
            "CompositeFoamTint": _vector(mid, "CompositeFoamTint"),
            "UseDetailSheets": _scalar(mid, "UseDetailSheets"),
            "UseBroadPattern": _scalar(mid, "UseBroadPattern"),
            "ExtendedGraphVersion": _scalar(mid, "ExtendedGraphVersion"),
            "BF6WaterHeightAvailable": _scalar(mid, "BF6WaterHeightAvailable"),
            "BF6DepthAvailable": _scalar(mid, "BF6DepthAvailable"),
            "BroadPattern": _texture_name(mid, "BroadPattern"),
            "OceanNoise": _texture_name(mid, "OceanNoise"),
            "cascade0": _vector(mid, "BF6Cascade0"),
            "cascade1": _vector(mid, "BF6Cascade1"),
            "cascade2": _vector(mid, "BF6Cascade2"),
            "cascade3": _vector(mid, "BF6Cascade3"),
        })
    return result


def _compact_status() -> dict:
    full = _status_dict()
    compact = {
        "world": full["world"],
        "water_material_count": full["water_material_count"],
        "materials": [],
    }
    for material in full["materials"]:
        evidence = material["parent_wpo_evidence"]
        compact["materials"].append({
            "name": material["name"],
            "uses": material["uses"],
            "BF6FoamWaveHeightM": material["BF6FoamWaveHeightM"],
            "BF6InteractiveBridgeEnabled": material["BF6InteractiveBridgeEnabled"],
            "BF6InteractiveBridgeHeightM": material["BF6InteractiveBridgeHeightM"],
            "BF6InteractiveBridgeLengthM": material["BF6InteractiveBridgeLengthM"],
            "BF6InteractiveBridgeRate": material["BF6InteractiveBridgeRate"],
            "BF6BroadCarrierSize": material["BF6BroadCarrierSize"],
            "BF6BroadAnimationRate": material["BF6BroadTimeScale"],
            "BF6FoamCrestStart": material["BF6FoamCrestStart"],
            "BF6FoamCrestFull": material["BF6FoamCrestFull"],
            "FoamCoverage": material["FoamCoverage"],
            "FoamRoughness": material["FoamRoughness"],
            "CompositeFoamTint": material["CompositeFoamTint"],
            "UseDetailSheets": material["UseDetailSheets"],
            "UseBroadPattern": material["UseBroadPattern"],
            "BroadPattern": material["BroadPattern"],
            "OceanNoise": material["OceanNoise"],
            "has_independent_carrier": evidence.get("has_independent_carrier", False),
            "has_foam_shoulder_ramp": evidence.get("has_foam_shoulder_ramp", False),
            "has_spatially_filtered_crest": evidence.get("has_spatially_filtered_crest", False),
            "has_broad_wave_size_input": evidence.get("has_broad_wave_size_input", False),
            "has_broad_time_scale_input": evidence.get("has_broad_time_scale_input", False),
            "has_fast_sheet_gate": evidence.get("has_fast_sheet_gate", False),
            "uses_rejected_triangle_broad_normal": evidence.get(
                "uses_rejected_triangle_broad_normal", False),
            "uses_pixel_broad_normal": evidence.get("uses_pixel_broad_normal", False),
            "normal_root_node": evidence.get("normal_root_node"),
            "normal_input_count": evidence.get("normal_input_count"),
        })
    return compact


def _viewport_camera() -> dict:
    try:
        bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
        if bridge:
            result = bridge.get_visible_level_viewport_camera()
        else:
            result = unreal.EditorLevelLibrary.get_level_viewport_camera_info()
        if len(result) == 3:
            valid, location, rotation = result
        else:
            location, rotation = result
            valid = True
        return {
            "valid": bool(valid),
            "location_cm": [float(location.x), float(location.y), float(location.z)],
            "rotation_deg": [float(rotation.pitch), float(rotation.yaw), float(rotation.roll)],
        }
    except Exception as exc:
        return {"valid": False, "error": str(exc)}


def _set_viewport_camera(location: unreal.Vector, rotation: unreal.Rotator) -> None:
    bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
    if bridge:
        if not bridge.set_visible_level_viewport_camera(location, rotation):
            raise RuntimeError("No visible level viewport client is available")
        return
    unreal.EditorLevelLibrary.set_level_viewport_camera_info(location, rotation)


def _map_overlay_actors():
    """Find only the SDK reference decal; never infer from arbitrary decals."""
    result = []
    for actor in _actors():
        try:
            label = actor.get_actor_label()
            tags = [str(tag) for tag in actor.tags]
        except Exception:
            continue
        if label.endswith("_MapImage") or "BF6MapDecal" in tags:
            result.append(actor)
    return result


def _set_map_overlay_visible(visible: bool) -> dict:
    actors = _map_overlay_actors()
    changed = []
    for actor in actors:
        label = actor.get_actor_label()
        hidden = not bool(visible)
        # Temporary editor hiding is deliberately non-destructive: the cached
        # official reference image and its carefully aligned transform survive.
        actor.set_is_temporarily_hidden_in_editor(hidden)
        actor.set_actor_hidden_in_game(hidden)
        changed.append(label)
    bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
    if bridge:
        bridge.redraw_visible_level_viewport()
    else:
        unreal.EditorLevelLibrary.editor_invalidate_viewports()
    return {
        "visible": bool(visible),
        "hidden": not bool(visible),
        "matched_actor_count": len(actors),
        "matched_actor_labels": changed,
        "camera_unchanged": _viewport_camera(),
    }


def _set_scalar(parameter: str, value: float, include_debug_layers: bool = False) -> int:
    count = 0
    for entry in _water_materials():
        if not include_debug_layers and all("/RawLayer" in use for use in entry["uses"]):
            continue
        entry["material"].set_scalar_parameter_value(parameter, value)
        count += 1
    bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
    if bridge:
        bridge.redraw_visible_level_viewport()
    else:
        unreal.EditorLevelLibrary.editor_invalidate_viewports()
    return count


def _component_material_names(component) -> list[str]:
    names = []
    try:
        for index in range(int(component.get_num_materials())):
            material = component.get_material(index)
            names.append(material.get_path_name() if material else "<none>")
    except Exception:
        pass
    return names


def _component_family(component) -> str:
    name = _name(component)
    class_name = component.get_class().get_name()
    if name.startswith("TerrainMesh_"):
        return "terrain"
    if name.startswith(("Road_", "RoadMesh_")):
        return "roads"
    if name.startswith("Water_") or name == "AccumulatedGameWater":
        return "water"
    if name.startswith("Scatter_"):
        return "scatter"
    if name.startswith("I_"):
        return "objects"
    if "Decal" in class_name:
        return "decal_volumes"
    return "other"


def _scene_geometry_audit(filter_text: str = "", max_rows: int = 300) -> dict:
    """Read the active scene only; do not infer identity from the screenshot."""
    needle = str(filter_text).casefold().strip()
    max_rows = max(1, min(int(max_rows), 2000))
    rows = []
    family_counts = {}
    total_components = 0
    for actor in _actors():
        try:
            actor_label = actor.get_actor_label()
            components = actor.get_components_by_class(unreal.PrimitiveComponent)
        except Exception:
            continue
        for component in components:
            total_components += 1
            family = _component_family(component)
            family_counts[family] = family_counts.get(family, 0) + 1
            materials = _component_material_names(component)
            mesh_name = None
            try:
                mesh = component.get_editor_property("static_mesh")
                mesh_name = mesh.get_path_name() if mesh else None
            except Exception:
                pass
            tags = _component_tags(component)
            searchable = " ".join(
                [actor_label, _name(component), component.get_class().get_name(),
                 mesh_name or ""] + materials + tags).casefold()
            if needle and needle not in searchable:
                continue
            visible = None
            hidden_in_game = None
            world_location = None
            bounds_origin = None
            bounds_extent = None
            cast_shadow = None
            instance_count = None
            try:
                visible = bool(component.get_editor_property("visible"))
                hidden_in_game = bool(component.get_editor_property("hidden_in_game"))
            except Exception:
                pass
            try:
                loc = component.get_world_location()
                world_location = [round(float(loc.x), 3), round(float(loc.y), 3),
                                  round(float(loc.z), 3)]
            except Exception:
                pass
            try:
                bounds = component.get_local_bounds()
                origin = bounds[0]
                extent = bounds[1]
                bounds_origin = [round(float(origin.x), 3), round(float(origin.y), 3),
                                 round(float(origin.z), 3)]
                bounds_extent = [round(float(extent.x), 3), round(float(extent.y), 3),
                                 round(float(extent.z), 3)]
            except Exception:
                pass
            try:
                cast_shadow = bool(component.get_editor_property("cast_shadow"))
            except Exception:
                pass
            if isinstance(component, unreal.InstancedStaticMeshComponent):
                try:
                    instance_count = int(component.get_instance_count())
                except Exception:
                    pass
            rows.append({
                "actor": actor_label,
                "component": _name(component),
                "class": component.get_class().get_name(),
                "family": family,
                "visible": visible,
                "hidden_in_game": hidden_in_game,
                "static_mesh": mesh_name,
                "materials": materials,
                "component_tags": tags,
                "world_location_cm": world_location,
                "local_bounds_origin_cm": bounds_origin,
                "local_bounds_extent_cm": bounds_extent,
                "cast_shadow": cast_shadow,
                "instance_count": instance_count,
            })
            if len(rows) >= max_rows:
                return {
                    "filter": filter_text,
                    "rows": rows,
                    "truncated": True,
                    "total_primitive_components_seen": total_components,
                    "family_counts_seen": family_counts,
                }
    return {
        "filter": filter_text,
        "rows": rows,
        "truncated": False,
        "total_primitive_components_seen": total_components,
        "family_counts_seen": family_counts,
    }


def _process_memory() -> dict:
    """Current editor memory from the OS; no command or external sampler."""
    if os.name != "nt":
        return {"available": False}

    class ProcessMemoryCountersEx(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_ulong),
            ("page_fault_count", ctypes.c_ulong),
            ("peak_working_set_size", ctypes.c_size_t),
            ("working_set_size", ctypes.c_size_t),
            ("quota_peak_paged_pool_usage", ctypes.c_size_t),
            ("quota_paged_pool_usage", ctypes.c_size_t),
            ("quota_peak_non_paged_pool_usage", ctypes.c_size_t),
            ("quota_non_paged_pool_usage", ctypes.c_size_t),
            ("pagefile_usage", ctypes.c_size_t),
            ("peak_pagefile_usage", ctypes.c_size_t),
            ("private_usage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    get_process_memory_info = psapi.GetProcessMemoryInfo
    get_process_memory_info.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ProcessMemoryCountersEx),
        ctypes.c_ulong,
    ]
    get_process_memory_info.restype = ctypes.c_int

    counters = ProcessMemoryCountersEx()
    counters.cb = ctypes.sizeof(counters)
    ok = get_process_memory_info(
        kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb)
    gib = float(1024 ** 3)
    result = {
        "available": bool(ok),
        "working_set_gib": round(counters.working_set_size / gib, 3),
        "private_gib": round(counters.private_usage / gib, 3),
        "peak_working_set_gib": round(counters.peak_working_set_size / gib, 3),
    }
    if not ok:
        result["windows_error"] = int(ctypes.get_last_error())
    return result


def _performance_status() -> dict:
    """Measure live renderer-facing scene cost without changing the scene."""
    counts = {
        "primitive_components": 0,
        "static_mesh_components": 0,
        "hism_components": 0,
        "hism_instances": 0,
        "local_light_components": 0,
        "shadow_casting_local_lights": 0,
        "shadow_casting_hisms": 0,
        "fallback_nonshadow_hisms": 0,
    }
    families = {}
    tagged_sources = 0
    for actor in _actors():
        try:
            components = actor.get_components_by_class(unreal.ActorComponent)
        except Exception:
            continue
        for component in components:
            if isinstance(component, unreal.PrimitiveComponent):
                counts["primitive_components"] += 1
                family = _component_family(component)
                families[family] = families.get(family, 0) + 1
            if isinstance(component, unreal.StaticMeshComponent):
                counts["static_mesh_components"] += 1
            if isinstance(component, unreal.HierarchicalInstancedStaticMeshComponent):
                counts["hism_components"] += 1
                try:
                    counts["hism_instances"] += int(component.get_instance_count())
                except Exception:
                    pass
                try:
                    casts = bool(component.get_editor_property("cast_shadow"))
                except Exception:
                    casts = False
                if casts:
                    counts["shadow_casting_hisms"] += 1
                else:
                    counts["fallback_nonshadow_hisms"] += 1
                if any(tag.startswith("BF6SourceMesh=")
                       for tag in _component_tags(component)):
                    tagged_sources += 1
            if isinstance(component, unreal.LocalLightComponent):
                counts["local_light_components"] += 1
                try:
                    if bool(component.get_editor_property("cast_shadows")):
                        counts["shadow_casting_local_lights"] += 1
                except Exception:
                    pass
    startup = _debug_startup_report()
    return {
        "runtime_input": "live Unreal scene built from current Steam install",
        "process_memory": _process_memory(),
        "scene": counts,
        "component_families": families,
        "source_tagged_hisms": tagged_sources,
        "latest_build_seconds": startup.get("launch_to_high_poly_ready_seconds"),
        "warning_count": startup.get("warning_count"),
        "warnings": startup.get("warnings", [])[:10],
        "camera_unchanged": _viewport_camera(),
    }


def _nearest_light_orientation(x_cm: float, y_cm: float, z_cm: float) -> dict:
    """Find the actual live light nearest an oracle position without moving it."""
    target = unreal.Vector(float(x_cm), float(y_cm), float(z_cm))
    nearest = None
    nearest_distance = float("inf")
    for actor in _actors():
        try:
            components = actor.get_components_by_class(unreal.LocalLightComponent)
        except Exception:
            continue
        for component in components:
            try:
                location = component.get_world_location()
                distance = float((location - target).length())
            except Exception:
                continue
            if distance < nearest_distance:
                nearest = (actor, component, location)
                nearest_distance = distance
    if nearest is None:
        return {"found": False, "target_cm": [x_cm, y_cm, z_cm]}
    actor, component, location = nearest
    direction = component.get_forward_vector()
    return {
        "found": True,
        "target_cm": [float(x_cm), float(y_cm), float(z_cm)],
        "distance_cm": nearest_distance,
        "actor": actor.get_actor_label(),
        "component": _name(component),
        "class": component.get_class().get_name(),
        "location_cm": [float(location.x), float(location.y), float(location.z)],
        "unreal_emission_axis_plus_x": [
            float(direction.x), float(direction.y), float(direction.z)],
        "intensity": float(component.get_editor_property("intensity")),
        "camera_unchanged": _viewport_camera(),
    }


def _material_runtime_state(material) -> dict:
    """Report the parameters used by the transient high-poly material only."""
    parent = None
    try:
        parent = material.get_editor_property("parent")
    except Exception:
        pass
    return {
        "material": material.get_path_name() if material else None,
        "parent": parent.get_path_name() if parent else None,
        "emissive_enable": _scalar(material, "EmissiveEnable"),
        "emissive_texture": _texture_name(material, "Emissive"),
        "base_color_texture": _texture_name(material, "BaseColor"),
        "opacity_texture": _texture_name(material, "Opacity"),
    }


def _instance_translation(component, index: int):
    """Handle both Transform and (success, Transform) UE Python signatures."""
    result = component.get_instance_transform(index, world_space=True)
    transform = result
    if isinstance(result, tuple):
        if len(result) == 2 and isinstance(result[0], bool):
            if not result[0]:
                return None
            transform = result[1]
        elif result:
            transform = result[-1]
    try:
        return transform.translation
    except Exception:
        try:
            return transform.get_editor_property("translation")
        except Exception:
            return None


def _nearest_mesh_instances(x_cm: float, y_cm: float, z_cm: float,
                            radius_cm: float = 500.0,
                            max_rows: int = 30) -> dict:
    """Resolve individual live HISM instances near an exact control point."""
    target = unreal.Vector(float(x_cm), float(y_cm), float(z_cm))
    radius_cm = max(1.0, min(float(radius_cm), 10000.0))
    max_rows = max(1, min(int(max_rows), 200))
    candidates = []
    components_seen = 0
    instances_seen = 0
    for actor in _actors():
        try:
            components = actor.get_components_by_class(
                unreal.InstancedStaticMeshComponent)
        except Exception:
            continue
        for component in components:
            if _component_family(component) != "objects":
                continue
            components_seen += 1
            try:
                instance_count = int(component.get_instance_count())
            except Exception:
                continue
            tags = _component_tags(component)
            mesh = None
            try:
                mesh = component.get_editor_property("static_mesh")
            except Exception:
                pass
            materials = []
            try:
                for material_index in range(int(component.get_num_materials())):
                    material = component.get_material(material_index)
                    materials.append(_material_runtime_state(material))
            except Exception:
                pass
            for index in range(instance_count):
                instances_seen += 1
                try:
                    location = _instance_translation(component, index)
                    if location is None:
                        continue
                    distance = float((location - target).length())
                except Exception:
                    continue
                if distance > radius_cm:
                    continue
                candidates.append({
                    "distance_to_origin_cm": distance,
                    "actor": actor.get_actor_label(),
                    "component": _name(component),
                    "instance_index": index,
                    "location_cm": [float(location.x), float(location.y),
                                    float(location.z)],
                    "static_mesh": mesh.get_path_name() if mesh else None,
                    "component_tags": tags,
                    "materials": materials,
                })
    candidates.sort(key=lambda row: row["distance_to_origin_cm"])
    return {
        "target_cm": [float(x_cm), float(y_cm), float(z_cm)],
        "radius_cm": radius_cm,
        "components_seen": components_seen,
        "instances_seen": instances_seen,
        "matches": candidates[:max_rows],
        "matches_in_radius": len(candidates),
        "truncated": len(candidates) > max_rows,
        "control": (
            "Distances are measured to decoded instance origins. No identity is "
            "inferred from the screenshot or component name."
        ),
        "camera_unchanged": _viewport_camera(),
    }


def _set_component_family_visible(family: str, visible: bool) -> dict:
    allowed = {"terrain", "roads", "water", "scatter", "objects", "decal_volumes"}
    family = str(family).strip().casefold()
    if family not in allowed:
        raise RuntimeError("family must be terrain, roads, water, scatter, objects, or decal_volumes")
    matched = []
    for actor in _actors():
        try:
            components = actor.get_components_by_class(unreal.PrimitiveComponent)
        except Exception:
            continue
        for component in components:
            if _component_family(component) != family:
                continue
            component.set_visibility(bool(visible), True)
            matched.append(f"{actor.get_actor_label()}/{_name(component)}")
    bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
    if bridge:
        bridge.redraw_visible_level_viewport()
    else:
        unreal.EditorLevelLibrary.editor_invalidate_viewports()
    return {
        "family": family,
        "visible": bool(visible),
        "matched_count": len(matched),
        "matched": matched[:200],
        "camera_unchanged": _viewport_camera(),
    }


def _find_component(component_name: str):
    """A primitive component of the add-on's actors by exact name, or None."""
    for actor in _actors():
        try:
            components = actor.get_components_by_class(unreal.PrimitiveComponent)
        except Exception:
            continue
        for component in components:
            if _name(component) == component_name:
                return actor, component
    return None, None


def _set_component_material(component_name: str, material_path: str) -> dict:
    actor, component = _find_component(component_name)
    if component is None:
        raise RuntimeError(f"no add-on component named {component_name!r}")
    material = unreal.load_object(None, material_path) if material_path else None
    if material_path and material is None:
        raise RuntimeError(f"material not found: {material_path}")
    before = []
    for index in range(int(component.get_num_materials())):
        current = component.get_material(index)
        before.append(current.get_path_name() if current else None)
        component.set_material(index, material)
    bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
    if bridge:
        bridge.redraw_visible_level_viewport()
    else:
        unreal.EditorLevelLibrary.editor_invalidate_viewports()
    return {
        "component": f"{actor.get_actor_label()}/{component_name}",
        "slots": len(before),
        "before": before,
        "after": material.get_path_name() if material else None,
        "camera_unchanged": _viewport_camera(),
    }


def _exec_console(command: str) -> dict:
    world = unreal.EditorLevelLibrary.get_editor_world()
    unreal.SystemLibrary.execute_console_command(world, str(command))
    return {"executed": str(command), "camera_unchanged": _viewport_camera()}


def _define_toolset():
    global _TOOLSET_CLASS
    if _TOOLSET_CLASS is not None:
        return _TOOLSET_CLASS

    @unreal.uclass()
    class BF6WaterToolset(unreal.ToolsetDefinition):
        """Inspect and control BF6HighPoly terrain and water live in this editor."""

        @toolset_registry.tool_call
        @staticmethod
        def set_component_material(component_name: str, material_path: str = "") -> str:
            """Swap every material slot on one add-on component; empty path restores the mesh default."""
            return json.dumps(_set_component_material(component_name, material_path),
                              indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def exec_console(command: str) -> str:
            """Run one editor console command (e.g. BF6.HighPoly.WaterTreeStatus) and return."""
            return json.dumps(_exec_console(command), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_status() -> str:
            """Report the live water MIDs, raw FFT scale, broad-WPO gate and textures."""
            return json.dumps(_status_dict(), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_compact_status() -> str:
            """Report only the live carrier, foam and graph evidence needed for iteration."""
            return json.dumps(_compact_status(), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_debug_startup_report() -> str:
            """Return MCP-measured launch timing, scene readiness, and exact log health."""
            return json.dumps(_debug_startup_report(), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_lighting_status() -> str:
            """Report live sky, sun, fog, grading presence and cloud-shadow consumer gaps."""
            return json.dumps(_lighting_status(), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_performance_status() -> str:
            """Measure live memory, instances, shadow producers and current log health."""
            return json.dumps(_performance_status(), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def audit_nearest_light_orientation(x_cm: float, y_cm: float,
                                            z_cm: float) -> str:
            """Report the live Unreal emission axis nearest an exact world position."""
            return json.dumps(
                _nearest_light_orientation(x_cm, y_cm, z_cm),
                indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def audit_nearest_mesh_instances(x_cm: float, y_cm: float,
                                         z_cm: float,
                                         radius_cm: float = 500.0,
                                         max_rows: int = 30) -> str:
            """Resolve nearby live HISM instances and their raw material state."""
            return json.dumps(
                _nearest_mesh_instances(x_cm, y_cm, z_cm,
                                        radius_cm, max_rows),
                indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_ground_photo_mix(mix: float = 0.0) -> str:
            """A/B the decoded terrain sheets against the low-frequency aerial colour map."""
            mix = max(0.0, min(float(mix), 1.0))
            materials = _ground_materials()
            before = [_scalar(entry["material"], "PhotoMix") for entry in materials]
            changed = _set_ground_scalar("PhotoMix", mix)
            after = [_scalar(entry["material"], "PhotoMix") for entry in materials]
            return json.dumps({
                "requested_mix": mix,
                "ground_materials_updated": changed,
                "before": before,
                "after": after,
                "decoded_layer_sheets_visible": mix < 0.5,
                "control": "1 replaces layer sheets with the aerial colour raster",
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_ground_material_status() -> str:
            """Report the live map-wide terrain material controls without changing the scene."""
            materials = _ground_materials()
            rows = []
            for entry in materials:
                material = entry["material"]
                rows.append({
                    "material": _name(material),
                    "uses": entry["uses"],
                    "photo_mix": _scalar(material, "PhotoMix"),
                    "blend_near_metres": _scalar(material, "BlendNear"),
                    "blend_far_metres": _scalar(material, "BlendFar"),
                    "coverage_union": _scalar(material, "CoverageUnion"),
                    "map_detail_strength": _scalar(material, "MapDetailStrength"),
                    "ground_grass_mode": _scalar(material, "GroundGrassMode"),
                    "ground_grass_strength": _scalar(material, "GroundGrassStrength"),
                    "stochastic_tiling": _scalar(material, "StochasticTiling"),
                    "slope_projection": _scalar(material, "SlopeProjection"),
                })
            return json.dumps({
                "ground_material_count": len(materials),
                "materials": rows,
                "runtime_input": "current Steam install; no exported intermediate",
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_ground_detail_extent(near_metres: float = 0.0,
                                     far_metres: float = 1.0) -> str:
            """A/B the retracted generic layer colour; production leaves it disabled."""
            near_metres = max(0.0, float(near_metres))
            far_metres = max(near_metres + 1.0, float(far_metres))
            materials = _ground_materials()
            before = [{
                "near_metres": _scalar(entry["material"], "BlendNear"),
                "far_metres": _scalar(entry["material"], "BlendFar"),
            } for entry in materials]
            changed_near = _set_ground_scalar("BlendNear", near_metres)
            changed_far = _set_ground_scalar("BlendFar", far_metres)
            after = [{
                "near_metres": _scalar(entry["material"], "BlendNear"),
                "far_metres": _scalar(entry["material"], "BlendFar"),
            } for entry in materials]
            return json.dumps({
                "requested": {
                    "near_metres": near_metres,
                    "far_metres": far_metres,
                },
                "ground_materials_updated": min(changed_near, changed_far),
                "before": before,
                "after": after,
                "control": (
                    "The generic colour evaluator produces the quilt when expanded. "
                    "It is diagnostic only; the production tiled path must consume "
                    "the shipped-DXIL material/normal pages."
                ),
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_ground_map_detail_strength(strength: float = 0.75) -> str:
            """Set authored high-frequency sheet detail over the continuous map bake; 0 is control."""
            strength = max(0.0, min(float(strength), 1.0))
            materials = _ground_materials()
            before = [_scalar(entry["material"], "MapDetailStrength")
                      for entry in materials]
            changed = _set_ground_scalar("MapDetailStrength", strength)
            after = [_scalar(entry["material"], "MapDetailStrength")
                     for entry in materials]
            return json.dumps({
                "requested_strength": strength,
                "ground_materials_updated": changed,
                "before": before,
                "after": after,
                "scope": "whole decoded playable terrain; camera independent",
                "control": "0 disables the authored high-frequency residual",
                "runtime_input": "current Steam install; no exported intermediate",
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_ground_stochastic_tiling(enabled: bool = True) -> str:
            """A/B transformed-neighbour terrain sampling against direct periodic repetition."""
            value = 1.0 if bool(enabled) else 0.0
            materials = _ground_materials()
            before = [_scalar(entry["material"], "StochasticTiling")
                      for entry in materials]
            changed = _set_ground_scalar("StochasticTiling", value)
            after = [_scalar(entry["material"], "StochasticTiling")
                     for entry in materials]
            return json.dumps({
                "requested_enabled": bool(enabled),
                "ground_materials_updated": changed,
                "before": before,
                "after": after,
                "control": "false restores direct periodic sampling at identical scale and material pairing",
                "claim_scope": (
                    "transformed-neighbour breakup approximation; not yet claimed as "
                    "the exact Frostbite transform law"
                ),
                "runtime_input": "current Steam install; no exported intermediate",
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_ground_grass(mode: int = 1, strength: float = 1.0) -> str:
            """A/B runtime-identified grass sheets: 0 off, 1 real pairing, 2 shuffled control."""
            mode = max(0, min(int(mode), 2))
            strength = max(0.0, min(float(strength), 2.0))
            materials = _ground_materials()
            before = [{
                "mode": _scalar(entry["material"], "GroundGrassMode"),
                "strength": _scalar(entry["material"], "GroundGrassStrength"),
            } for entry in materials]
            changed_mode = _set_ground_scalar("GroundGrassMode", float(mode))
            changed_strength = _set_ground_scalar("GroundGrassStrength", strength)
            after = [{
                "mode": _scalar(entry["material"], "GroundGrassMode"),
                "strength": _scalar(entry["material"], "GroundGrassStrength"),
            } for entry in materials]
            return json.dumps({
                "requested": {"mode": mode, "strength": strength},
                "ground_materials_updated": min(changed_mode, changed_strength),
                "before": before,
                "after": after,
                "mode_meaning": {
                    "0": "off/null control",
                    "1": "current-install material-name pairing",
                    "2": "equal-population shuffled pairing control",
                },
                "runtime_input": "current Steam install; no exported intermediate",
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_viewport_camera() -> str:
            """Report the active level viewport camera for paired frame captures."""
            return json.dumps(_viewport_camera(), indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def show_build_viewport() -> str:
            """Reveal the already-loaded SDK build viewport without reopening the map."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge or not bridge.show_build_viewport():
                raise RuntimeError("BF6 build viewport bridge is unavailable")
            return json.dumps({
                "visible": True,
                "map_reopened": False,
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def start_full_high_poly_build() -> str:
            """Start the add-on's native direct-Steam full build without cursor or console input."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge:
                raise RuntimeError("BF6 build bridge is unavailable")
            started = bool(bridge.start_full_high_poly_build())
            return json.dumps({
                "started": started,
                "runtime_input": "current Steam install; no exported intermediate",
                "route": "same native StartRead path as High Poly BUILD",
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def open_base_map(level: str) -> str:
            """Open one shipped MP map through the SDK's native map loader."""
            level = str(level).strip()
            if not re.fullmatch(r"(?i)MP_[A-Z0-9_]+", level):
                raise ValueError("level must be a shipped MP_* identifier")
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge or not bridge.open_base_map(level):
                raise RuntimeError(f"SDK could not open base map {level}")
            return json.dumps({
                "opened": str(bridge.get_current_level()),
                "requested": level,
                "save": None,
                "route": "BF6Ext::OpenMap -> SDK EnterBuild",
                "runtime_input": "current Steam install; no exported intermediate",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_current_level() -> str:
            """Report the SDK level currently mounted for direct runtime reads."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            return json.dumps({
                "level": str(bridge.get_current_level()) if bridge else "",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_viewport_camera(x_cm: float, y_cm: float, z_cm: float,
                                pitch: float, yaw: float,
                                roll: float = 0.0) -> str:
            """Set an exact level-viewport camera without mouse or console input."""
            location = unreal.Vector(float(x_cm), float(y_cm), float(z_cm))
            rotation = unreal.Rotator()
            rotation.pitch = float(pitch)
            rotation.yaw = float(yaw)
            rotation.roll = float(roll)
            _set_viewport_camera(location, rotation)
            return json.dumps({
                "source": "explicit MCP camera transform",
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_map_overlay_status() -> str:
            """Report the exact Display/Sun -> Map image state and actor."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            state = int(bridge.get_map_image_state()) if bridge else -1
            actors = _map_overlay_actors()
            rows = []
            for actor in actors:
                try:
                    hidden = bool(actor.is_temporarily_hidden_in_editor())
                except Exception:
                    hidden = None
                rows.append({
                    "label": actor.get_actor_label(),
                    "temporarily_hidden_in_editor": hidden,
                })
            return json.dumps({
                "display_sun_state": state,
                "display_sun_state_name": {
                    0: "off", 1: "hidden", 2: "downloading", 3: "shown",
                }.get(state, "bridge unavailable"),
                "matched_actor_count": len(rows),
                "actors": rows,
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_map_overlay_visible(visible: bool = False) -> str:
            """Set the exact Display/Sun -> Map image control without moving the camera."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge:
                raise RuntimeError("BF6 Display/Sun MCP bridge is unavailable")
            state = int(bridge.set_map_image_visible(bool(visible)))
            return json.dumps({
                "requested_visible": bool(visible),
                "display_sun_state": state,
                "display_sun_state_name": {
                    0: "off", 1: "hidden", 2: "downloading", 3: "shown",
                }.get(state, "unknown"),
                "matched_actor_count": len(_map_overlay_actors()),
                "camera_unchanged": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_scene_geometry_audit(filter_text: str = "", max_rows: int = 300) -> str:
            """Report live primitive components, meshes and materials without changing the scene."""
            return json.dumps(_scene_geometry_audit(filter_text, max_rows),
                              indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def capture_level_viewport(label: str = "bf6-mcp",
                                   width: int = 1920,
                                   height: int = 1080) -> str:
            """Capture only the rendered level viewport, excluding SDK/desktop Slate UI."""
            safe_label = re.sub(r"[^A-Za-z0-9_.-]+", "-", str(label)).strip("-.")
            if not safe_label:
                safe_label = "bf6-mcp"
            width = max(320, min(int(width), 7680))
            height = max(180, min(int(height), 4320))
            saved_dir = unreal.Paths.convert_relative_path_to_full(
                unreal.Paths.project_saved_dir())
            capture_dir = Path(saved_dir) / "BF6McpCaptures"
            capture_dir.mkdir(parents=True, exist_ok=True)
            output = capture_dir / f"{safe_label}.png"
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            native_capture = False
            actual_width = width
            actual_height = height
            if bridge and hasattr(bridge, "capture_visible_level_viewport"):
                captured = bridge.capture_visible_level_viewport(str(output))
                if isinstance(captured, tuple):
                    native_capture = bool(captured[0])
                    if len(captured) >= 3:
                        actual_width = int(captured[1])
                        actual_height = int(captured[2])
                else:
                    native_capture = bool(captured)
            task = None
            if not native_capture:
                if bridge:
                    bridge.redraw_visible_level_viewport()
                task = unreal.AutomationLibrary.take_high_res_screenshot(
                    width, height, str(output))
                if task is not None:
                    _pending_screenshot_tasks.append(task)
            return json.dumps({
                "completed": native_capture,
                "started": native_capture or task is not None,
                "output": str(output),
                "resolution": [actual_width, actual_height],
                "requested_resolution": [width, height],
                "camera": _viewport_camera(),
                "capture_surface": "rendered level viewport; Slate/desktop excluded",
                "capture_path": ("synchronous visible viewport readback"
                                 if native_capture else "latent automation fallback"),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_component_family_visible(family: str, visible: bool) -> str:
            """Toggle one named High Poly family for a live visual control."""
            return json.dumps(_set_component_family_visible(family, visible),
                              indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def generate_exact_terrain_at_camera(span_metres: float = 256.0,
                                             resolution: int = 512) -> str:
            """Generate a current-install shipped-DXIL terrain page at the visible camera."""
            span_metres = max(8.0, min(float(span_metres), 2048.0))
            resolution = max(64, min(int(resolution), 2048))
            resolution = max(64, (resolution // 8) * 8)
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge:
                raise RuntimeError("BF6HighPoly terrain MCP bridge is unavailable")
            started = bridge.start_exact_terrain_at_visible_camera(
                span_metres, resolution)
            return json.dumps({
                "started": bool(started),
                "runtime_input": "current Steam install and mounted level",
                "camera": _viewport_camera(),
                "span_metres": span_metres,
                "resolution": resolution,
                "status": bridge.get_exact_terrain_status(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def get_exact_terrain_status() -> str:
            """Report whether the shipped-DXIL terrain AOV rectangle is busy or active."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge:
                raise RuntimeError("BF6HighPoly terrain MCP bridge is unavailable")
            return json.dumps({
                "status": bridge.get_exact_terrain_status(),
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def disable_exact_terrain() -> str:
            """Restore the approximate terrain control without rebuilding the map."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge:
                raise RuntimeError("BF6HighPoly terrain MCP bridge is unavailable")
            bridge.disable_exact_terrain()
            return json.dumps({
                "status": bridge.get_exact_terrain_status(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_tsuru_reference_view() -> str:
            """Restore the saved MP_Isolated Tsuru-overlook viewport used by the BF6 frame oracle."""
            location = unreal.Vector(-102007.328125, 17811.326172, 12586.099609)
            rotation = unreal.Rotator()
            rotation.pitch = -26.799999
            rotation.yaw = 34.800003
            rotation.roll = 0.0
            _set_viewport_camera(location, rotation)
            return json.dumps({
                "source": "EditorPerProjectUserSettings saved MP_Isolated Tsuru view",
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_captured_game_match_view() -> str:
            """Restore the camera the user aligned to the live BF6 viewpoint on 2026-08-28."""
            location = unreal.Vector(-101752.313875, 25305.169095, 11597.914760)
            rotation = unreal.Rotator()
            rotation.pitch = -9.799999
            rotation.yaw = -259.000005
            rotation.roll = 0.0
            _set_viewport_camera(location, rotation)
            return json.dumps({
                "source": "user-aligned live BF6/Unreal paired view",
                "camera": _viewport_camera(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def show_water_lab() -> str:
            """Open or focus Water Lab and start its direct Steam read."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge or not bridge.show_water_lab():
                raise RuntimeError("BF6HighPoly Water Lab bridge is unavailable")
            return "Water Lab opened through the native MCP bridge"

        @toolset_registry.tool_call
        @staticmethod
        def reload_live_raw() -> str:
            """Re-read only Water Lab data from the installed Steam game, asynchronously."""
            bridge = getattr(unreal, "BF6HighPolyViewportLibrary", None)
            if not bridge or not bridge.reload_water_lab():
                raise RuntimeError("BF6HighPoly Water Lab bridge is unavailable")
            return "Native direct-game reload started; call get_debug_startup_report for readiness."

        @toolset_registry.tool_call
        @staticmethod
        def fix_safe_validation_issues(save: bool = True) -> str:
            """Fix only SDK validator rows carrying an explicit safe-fix action."""
            changed = unreal.BF6HighPolyViewportLibrary.fix_safe_validation_issues(save)
            return json.dumps({"changed_actors": int(changed), "saved": bool(save)})

        @toolset_registry.tool_call
        @staticmethod
        def set_broad_wave_height(metres: float) -> str:
            """Retired: broad foam is pixel-only and cannot be used as water geometry."""
            requested = float(metres)
            count = _set_scalar("BF6FoamWaveHeightM", 0.0)
            return json.dumps({
                "requested_metres": requested,
                "applied_metres": 0.0,
                "live_materials_updated": count,
                "status": "retracted; water-broad-foam-is-pixel-only",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_interactive_wave_bridge(enabled: bool = True, height_metres: float = 4.0,
                                        wavelength_metres: float = 120.0,
                                        rate: float = 0.35) -> str:
            """Tune the provisional eWave bridge used until the exact BF6 interactive atlas is exposed."""
            height = max(0.0, min(float(height_metres), 8.0))
            wavelength = max(4.0, min(float(wavelength_metres), 512.0))
            motion_rate = max(0.0, min(float(rate), 4.0))
            changed = 0
            for entry in _water_materials():
                if all("/RawLayer" in use for use in entry["uses"]):
                    continue
                mid = entry["material"]
                mid.set_scalar_parameter_value("BF6InteractiveBridgeEnabled", 1.0 if enabled else 0.0)
                mid.set_scalar_parameter_value("BF6InteractiveBridgeHeightM", height)
                mid.set_scalar_parameter_value("BF6InteractiveBridgeLengthM", wavelength)
                mid.set_scalar_parameter_value("BF6InteractiveBridgeRate", motion_rate)
                changed += 1
            return json.dumps({
                "status": "provisional_not_exact",
                "enabled": bool(enabled),
                "height_metres": height,
                "wavelength_metres": wavelength,
                "rate": motion_rate,
                "live_materials_updated": changed,
                "control": "set enabled=false at the unchanged camera",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_broad_carrier_size(multiplier: float) -> str:
            """Diagnostic pixel-graph retile only; 1 is raw and it never changes geometry."""
            multiplier = max(1.0, min(float(multiplier), 64.0))
            count = _set_scalar("BF6BroadCarrierSize", multiplier)
            return json.dumps({
                "requested_multiplier": multiplier,
                "bridge_primary_uv_scale": 1.0 / multiplier,
                "native_pixel_graph_uv_scale": 1.0,
                "live_materials_updated": count,
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_broad_motion_rate(multiplier: float) -> str:
            """Diagnostic pixel-graph time only; 1 is raw and it never changes geometry."""
            multiplier = max(0.0, min(float(multiplier), 1.0))
            count = _set_scalar("BF6BroadTimeScale", multiplier)
            return json.dumps({
                "requested_time_multiplier": multiplier,
                "live_materials_updated": count,
                "control": "0 freezes broad draw foam and its matching WPO together",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_foam_crest_window(start_profile: float,
                                  full_profile: float) -> str:
            """Set where open-water foam starts and reaches full coverage on the wave ramp."""
            start_profile = max(-0.1, min(float(start_profile), 0.99))
            full_profile = max(start_profile + 0.01,
                               min(float(full_profile), 1.0))
            start_count = _set_scalar("BF6FoamCrestStart", start_profile)
            full_count = _set_scalar("BF6FoamCrestFull", full_profile)
            return json.dumps({
                "requested_start_profile": start_profile,
                "requested_full_profile": full_profile,
                "live_materials_updated": min(start_count, full_count),
                "control": "0.99..1.0 is the near-empty strict crest control",
                "geometry_height_unchanged": True,
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_foam_layer_coverage(scale: float) -> str:
            """Scale Unreal's opaque layer-over-water coverage without changing the foam mask."""
            scale = max(0.0, min(float(scale), 1.0))
            count = _set_scalar("FoamCoverage", scale)
            return json.dumps({
                "requested_scale": scale,
                "live_materials_updated": count,
                "foam_mask_unchanged": True,
                "foam_tint_and_roughness_unchanged": True,
                "control": "0 keeps the decoded foam mask visible in color/roughness but removes Unreal layer opacity",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_fast_sheet_detail(enabled: bool) -> str:
            """Enable the fast dotted micro/foam sheet only for diagnostic comparison."""
            value = 1.0 if bool(enabled) else 0.0
            count = _set_scalar("UseDetailSheets", value)
            return json.dumps({
                "enabled": bool(enabled),
                "live_materials_updated": count,
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_mask_polarity(use_mask_directly: bool) -> str:
            """A/B the wave-mask law: attenuation = mask, or attenuation = 1 - mask."""
            value = 1.0 if bool(use_mask_directly) else 0.0
            count = _set_scalar("BF6MaskPolarity", value)
            return json.dumps({
                "law": "attenuation = mask" if value else "attenuation = 1 - mask",
                "live_materials_updated": count,
                "evidence": (
                    "only ~40 atlas pages exist, covering the island; EVERY cell "
                    "outside them is inline 255. Under 1-mask that default means "
                    "no waves across the whole open ocean, which is what a "
                    "mirror-flat sea at (-2401,-26) looks like. Under mask it "
                    "means open ocean at full waves with suppression painted in "
                    "near land, which matches the terrain overlay."
                ),
                "control": "open ocean should gain waves; the lagoon should stay calm",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_foam_chain(enabled: bool) -> str:
            """A/B the game's cascade fold-channel foam chain (1) against the old Gerstner crest (0)."""
            n = _set_scalar("BF6FoamChain", 1.0 if enabled else 0.0)
            return json.dumps({"BF6FoamChain": 1.0 if enabled else 0.0, "materials": n}, indent=2)

        @toolset_registry.tool_call
        @staticmethod
        def set_cascade_overlap(enabled: bool) -> str:
            """A/B the cascade-0 overlap: the game's rotated second sample of the swell band."""
            value = 1.0 if bool(enabled) else 0.0
            count = _set_scalar("BF6CascadeOverlapEnabled", value)
            return json.dumps({
                "enabled": bool(enabled),
                "live_materials_updated": count,
                "what_this_is": (
                    "the shipped draw path samples cascade 0 a SECOND time through a "
                    "rotation and anisotropic scale, described in the DXIL decode as "
                    "breaking up the largest cascade's visible tiling"
                ),
                "why_it_matters": (
                    "cascade 0's spectrum is a corrugation - 99.7% of its energy inside "
                    "45 degrees, confirmed by executing the shipped H0 builder against "
                    "the authored lobe - so this is the only thing in the draw path that "
                    "can add a second wave direction"
                ),
                "control": (
                    "off vs on at a fixed camera separates the overlap's contribution "
                    "from the spectrum's own directionality"
                ),
                "note": "the authored value returns on the next water build",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def set_raw_fft_height_multiplier(multiplier: float) -> str:
            """Scale recovered FFT height only; 1 is raw and 0 is the flat control."""
            multiplier = max(0.0, min(float(multiplier), 1000.0))
            materials = _water_materials()
            changed = 0
            for entry in materials:
                if all("/RawLayer" in use for use in entry["uses"]):
                    continue
                mid = entry["material"]
                current = _scalar(mid, "BF6WaveAmplitudeScale")
                if current is None:
                    continue
                # The raw loaded surface value is exposed separately by the
                # Water Lab UI but not to Python. Reload before using this tool
                # when an absolute multiplier is required. This operation is
                # reported as a live-value multiplication, not a decoded fact.
                mid.set_scalar_parameter_value("BF6WaveAmplitudeScale", current * multiplier)
                changed += 1
            return json.dumps({
                "operation": "multiplied_current_live_BF6WaveAmplitudeScale",
                "multiplier": multiplier,
                "materials_updated": changed,
                "control": "reload_live_raw, then use multiplier 0 for flat",
                "status": _status_dict(),
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def run_broad_wave_control(test_metres: float = 4.0) -> str:
            """Retired negative control: broad foam is verified pixel-only."""
            requested = float(test_metres)
            _set_scalar("BF6FoamWaveHeightM", 0.0)
            flat_values = [
                _scalar(entry["material"], "BF6FoamWaveHeightM")
                for entry in _water_materials()
                if not all("/RawLayer" in use for use in entry["uses"])
            ]
            return json.dumps({
                "flat_control_values": flat_values,
                "requested_but_rejected_metres": requested,
                "status": "retracted; broad foam resources are not vertex inputs",
            }, indent=2, sort_keys=True)

        @toolset_registry.tool_call
        @staticmethod
        def run_layered_wave_control(test_metres: float = 4.0,
                                     carrier_size: float = 10.0) -> str:
            """Retired negative control; restores raw pixel scale and zero false WPO."""
            requested_height = float(test_metres)
            requested_carrier = float(carrier_size)
            materials = _water_materials()
            active = [entry for entry in materials
                      if not all("/RawLayer" in use for use in entry["uses"])]
            for entry in active:
                mid = entry["material"]
                mid.set_scalar_parameter_value("BF6BroadCarrierSize", 1.0)
                mid.set_scalar_parameter_value("BF6BroadTimeScale", 1.0)
                mid.set_scalar_parameter_value("BF6FoamWaveHeightM", 0.0)
            flat = [_scalar(entry["material"], "BF6FoamWaveHeightM") for entry in active]
            return json.dumps({
                "active_materials": len(active),
                "flat_control_values": flat,
                "requested_but_rejected_height": requested_height,
                "requested_but_rejected_carrier": requested_carrier,
                "carrier_size_values": [
                    _scalar(entry["material"], "BF6BroadCarrierSize") for entry in active],
                "status": "retracted; foam/noise sheets are pixel inputs, not displacement",
            }, indent=2, sort_keys=True)

    _TOOLSET_CLASS = BF6WaterToolset
    return _TOOLSET_CLASS


def register() -> str:
    """Register (or re-register) this module's toolset with UE's registry."""
    toolset_class = _define_toolset()
    registry = unreal.ToolsetRegistry
    if registry.is_toolset_class_registered(toolset_class):
        registry.unregister_toolset_class(toolset_class)
    registry.register_toolset_class(toolset_class)
    message = f"registered {toolset_class.__module__}.{toolset_class.__name__}"
    unreal.log(f"BF6 water MCP: {message}")
    return message


register()
