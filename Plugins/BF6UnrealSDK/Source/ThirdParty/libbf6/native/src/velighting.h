/* libbf6 internal - the level's authored lighting, out of its VisualEnvironment.
 *
 * A VE preset is ~800 authored fields over 27 components: sun, sky, two cloud
 * layers, cloud shadows, fog, exposure, colour grading, white balance, ambient
 * occlusion, global illumination and the shadow cascades. This reads the subset
 * a renderer needs to light a map the way the game does, plus the texture
 * references that go with it.
 *
 * TWO THINGS MAKE THIS WORK, AND BOTH ARE EASY TO GET WRONG.
 *
 * 1. WHICH PRESET. A level ships several - interior, dark-alley,
 *    construction-site, thermal - and only one is the outdoor environment. The
 *    level ROOT partition names it, as an ordinary EFIX Imports[] record. Of
 *    the 19 partitions in mp_dumbo's lighting/ folder exactly two are imported
 *    by the root: the active VE and `thermal`. So the import list is a
 *    SELECTOR, not a dependency dump, and the rule is:
 *
 *      active VE = the non-thermal ve_* partition, under the LEVEL's own
 *                  directory, that the level root imports
 *
 *    Compare the PARTITION half of the import record. An Imports[] entry is
 *    PartitionGuid(16) + InstanceGuid(16) and the two are equal on about 12%
 *    of records, so comparing the instance half looks like a working join and
 *    then answers "no referrer" for the other 88%, silently.
 *
 *    The level root also imports four SHARED presets - ve_global_base,
 *    ve_global_performance, ve_renderlayer_mp and a lens-flare preset under
 *    common/lighting/. Those are engine-wide, carry no map identity, and are
 *    excluded by the "under the level's own directory" half of the rule. Drop
 *    that half and mp_dumbo has five candidates instead of one.
 *
 * 2. WHICH FIELD. An RFL2 NameHash is not reversible, so the field names are a
 *    lookup, and the lookup is MANY-TO-ONE in the name direction. Every
 *    constant below is keyed by (component type_guid, hash) and never by name:
 *
 *      f8b3f61a  8748d69f  SunRotationX = 124.8     <- the authored sun
 *      f8b3f61a  be17684d  SunRotationX =   0.0
 *      c4ea62ae  0a654b9f  SunScale     =   1.5     (the GI component's)
 *      5fb52ff8  0a654b9f  SunScale     = 300000    (the sky's)
 *
 *    A name-first lookup returns whichever comes first, which is 0.0 as often
 *    as not, and nothing about the result says so.
 *
 * The hashes were read off the game's own data with vedump_test and named by
 * joining against the SDK field-name tables. They are SCHEMA constants, the
 * same kind of thing as the water simulation's field hashes in bf6_core.cpp -
 * no per-map value is baked anywhere.
 */
#ifndef LIBBF6_VELIGHTING_H
#define LIBBF6_VELIGHTING_H

#include <string>
#include <vector>

#include "source.h"
#include "types.h"

namespace bf6 {

// Which of the VE's components the preset actually carried. A component that is
// absent leaves its whole block of the struct at zero, and zero is a legal
// authored value for most of these fields, so presence has to be said out loud
// rather than inferred. Mirrors bf6_ve_component in the public header.
enum VeComponentBit {
    kVeSun          = 1u << 0,
    kVeSky          = 1u << 1,
    kVeFog          = 1u << 2,
    kVeExposure     = 1u << 3,
    kVeGrading      = 1u << 4,
    kVeWhiteBalance = 1u << 5,
    kVeAmbientOcc   = 1u << 6,
    kVeGlobalIllum  = 1u << 7,
    kVeSunShadow    = 1u << 8
};

struct VeLighting {
    std::string preset;         // leaf, e.g. "ve_mp_aftermath_sunsetovercast_03"
    std::string preset_path;    // the full partition name it was read from
    int         preset_candidates = 0;   // how many the root selected; 1 normally
    uint32_t    components = 0;          // VeComponentBit mask
    float       visibility = 0.f;        // the blend weight the game applies it at
    int         component_count = 0;     // components the preset's entity declares

    // ---- sun (OutdoorLightComponentData) --------------------------------
    float sun_rotation_x = 0.f;      // compass bearing, degrees
    float sun_rotation_y = 0.f;      // elevation above the horizon, degrees
    float sun_color[3] = {0.f, 0.f, 0.f};   // linear, normalised to peak 1
    float sun_intensity = 0.f;       // lux
    float sun_angular_radius = 0.f;  // degrees
    float sun_specular_scale = 0.f;
    float sun_shadow_view_distance = 0.f;   // metres, highest quality level
    float cloud_shadow_size = 0.f;          // metres per tile
    float cloud_shadow_coverage = 0.f;
    float cloud_shadow_exponent = 0.f;
    float cloud_shadow_speed[2] = {0.f, 0.f};
    float cloud_shadow_translation[2] = {0.f, 0.f};
    int   cloud_radiosity = 0;
    float secondary_cloud_shadow_size = 0.f;
    float secondary_cloud_shadow_coverage = 0.f;
    float secondary_cloud_shadow_exponent = 0.f;
    float secondary_cloud_shadow_speed[2] = {0.f, 0.f};
    float secondary_cloud_shadow_translation[2] = {0.f, 0.f};
    int cloud_shadow_addressing_mode = 0;
    int secondary_cloud_shadow_addressing_mode = 0;
    int cloud_shadow_is_top_down = 0;
    int secondary_cloud_shadow_is_top_down = 0;
    float cloud_shadow_start_fade = 0.f;
    float cloud_shadows_fade_distance = 0.f;
    int cloud_shadow_height_fade_enable = 0;
    float cloud_shadow_start_height_fade = 0.f;
    float cloud_shadows_height_fade_distance = 0.f;

    // ---- sky (SkyComponentData) -----------------------------------------
    int   sky_type = 0;
    float sky_luminance_scale = 0.f;
    float sky_panoramic_rotation = 0.f;     // TURNS, not degrees
    float sky_panoramic_tile_factor = 0.f;
    float sky_panoramic_uv_min[2] = {0.f, 0.f};
    float sky_panoramic_uv_max[2] = {1.f, 1.f};
    float sky_flow_distance = 0.f;
    float sky_flow_direction = 0.f;         // degrees
    float sky_flow_period = 0.f;            // seconds
    float sky_flow_height_mask_scale = 0.f;
    float sky_flow_height_mask_bias = 0.f;
    int   sky_draw_sun_disc = 0;
    float sun_disc_size = 0.f;
    float sun_disc_scale = 0.f;
    float rayleigh[3] = {0.f, 0.f, 0.f};
    float rayleigh_scale = 0.f;
    float mie_coefficient = 0.f;
    float mie_g = 0.f;
    int   use_aerial_perspective = 0;
    float aerial_perspective_scale = 0.f;
    float aerial_perspective_intensity = 0.f;
    float earth_radius = 0.f;               // 1000 km units
    float atmosphere_radius = 0.f;
    float height_fog_color_add[3] = {0.f, 0.f, 0.f};   // HDR radiance
    // cloud layer 1, the one every map authors
    float cloud1_altitude = 0.f, cloud1_tile_factor = 0.f, cloud1_rotation = 0.f;
    float cloud1_speed = 0.f, cloud1_alpha_mul = 0.f;
    float cloud1_color[3] = {0.f, 0.f, 0.f};

    // ---- fog (DepthOfFieldless FogComponentData) ------------------------
    int   fog_height_enable = 0;
    int   fog_color_enable = 0;
    int   fog_gradient_enable = 0;
    float fog_color[3] = {0.f, 0.f, 0.f};   // HDR radiance, NOT 0..1
    float fog_dist_start = 0.f, fog_dist_end = 0.f;
    float fog_color_start = 0.f, fog_color_end = 0.f;
    float fog_height_start = 0.f, fog_height_end = 0.f;
    float fog_altitude = 0.f, fog_depth = 0.f, fog_visibility_range = 0.f;
    int   volumetrics_enable = 0;
    float sun_scatter_intensity = 0.f;
    float local_light_scatter_intensity = 0.f;

    // ---- exposure + bloom ------------------------------------------------
    int   auto_exposure = 0;
    float ev = 0.f, ev_max = 0.f, exposure_compensation = 0.f;
    float bloom_scale[3] = {0.f, 0.f, 0.f};
    int   bloom_method = 0;

    // ---- colour grading + white balance ----------------------------------
    int   grading_enable = 0;
    float grade_brightness[3] = {0.f, 0.f, 0.f};
    float grade_contrast[3] = {0.f, 0.f, 0.f};
    float grade_saturation[3] = {0.f, 0.f, 0.f};
    float grade_hue = 0.f;
    float white_temperature = 0.f;   // kelvin
    float white_tint = 0.f;

    // ---- ambient occlusion ------------------------------------------------
    int   ao_affects_outdoor_light = 0;
    int   ao_affects_local_light = 0;
    float ssao_max_distance_inner = 0.f, ssao_max_distance_outer = 0.f;
    float hbao_radius = 0.f, hbao_contrast = 0.f;
    float dynamic_ao_factor = 0.f;

    // ---- global illumination ----------------------------------------------
    float gi_terrain_color[3] = {0.f, 0.f, 0.f};
    float gi_sky_color[3] = {0.f, 0.f, 0.f};
    float gi_ground_color[3] = {0.f, 0.f, 0.f};
    float gi_sun_color[3] = {0.f, 0.f, 0.f};
    float gi_backlight_rotation_x = 0.f, gi_backlight_rotation_y = 0.f;
    float gi_bounce_scale = 0.f, gi_sun_scale = 0.f;

    // ---- the textures the preset binds ------------------------------------
    // Resource names, or empty. See the note in the .cpp: these come out of the
    // payload as raw import pointers, because the type table describes their
    // fields with a null type and read_instance therefore reports them absent.
    std::string panorama_res;
    std::string panorama_alpha_res;
    std::string sky_gradient_res;
    std::string flow_mask_res;
    std::string cloud_layer1_res;
    std::string cloud_shadow_res;
    std::string secondary_cloud_shadow_res;
    std::string grading_lut_res;
    std::string lens_dirt_res;

    // Everything the preset imports, resolved to names. A VE's import list is
    // fully accounted for: its level's own textures, a few shared/global ones,
    // and the enum types two of its fields are declared with. Kept because it
    // names assets no field convention would surface - Tungsten's five-element
    // lens-flare rig and its colour-grading LUT, for instance.
    std::vector<std::string> imports;

    int fields_found = 0;       // of the named fields below, how many resolved
    int fields_expected = 0;
};

// The active preset's partition name for `level`, or empty with err set.
std::string ve_active_preset(Source& src, TypeDb& types, const std::string& level,
                             int* candidates, std::string& err);

// Find the active preset and decode it. False with err set.
bool ve_lighting(Source& src, TypeDb& types, const std::string& level,
                 VeLighting& out, std::string& err);

// Decode one explicitly named VisualEnvironment partition. Front-end screens
// author their presets directly and have no playable level root from which an
// "active" preset could be selected.
bool ve_lighting_partition(Source& src, TypeDb& types, const std::string& partition,
                           VeLighting& out, std::string& err);

}  // namespace bf6

#endif
