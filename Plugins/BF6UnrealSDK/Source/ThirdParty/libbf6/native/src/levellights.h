/* libbf6 internal - a level's LOCAL LIGHT placements.
 *
 * Every lamp, spotlight, ceiling fixture and emissive panel a map places. The
 * VisualEnvironment (velighting.h) is the sun and the sky; this is everything
 * else, and on an urban map it is thousands of fixtures. A map imported with a
 * sun and nothing else is lit at midday whatever its preset says, because all
 * of its artificial light is here.
 *
 * FOUR THINGS MAKE THIS WORK, AND EVERY ONE OF THEM FAILS SILENTLY.
 *
 * 1. A LIGHT IS NOT IN THE LIGHTING PARTITIONS. Sampling lay_art_*_lighting.ebx
 *    and _layers_content/lighting.ebx directly returns ZERO light entities:
 *    almost every light sits inside a placed fixture prefab, so the only way to
 *    find them is to walk the level graph the way a placement walk does. Hence
 *    the traversal below rather than a lookup.
 *
 * 2. THE PLACEMENT IS ON A DIFFERENT INSTANCE. A *LightEntityData carries its
 *    own `Transform` (0xD6351EDE) - NOT the `BlueprintTransform` a prefab
 *    reference uses, which it does not have at all - and reading that is
 *    already the difference between a fixture in the right place and one at its
 *    holder's origin. It is still not enough: measured on mp_dumbo, 6,848 of
 *    7,878 lights (86.9%) have an IDENTITY Transform.
 *
 *    Light fixtures ship as ObjectBlueprints, and inside one the light entity
 *    is not a placed object at all - it is reachable only through the
 *    blueprint's PropertyConnections and its own Transform is left identity.
 *    The placement sits on a SEPARATE component (type dcac04fc-...) in the
 *    owning object's Components array, and that component points back at the
 *    light through its `Light` field.
 *
 *    lf_com_streetlight_02: the light entity is at identity, its component is
 *    at (3.890, 9.923, -0.002) - 9.9 m up the pole and 3.9 m out along the arm,
 *    which is the lamp head. Reading the entity alone puts a street lamp's
 *    light on the pavement at the foot of its own pole, and that looks
 *    plausible enough on a map to survive for months.
 *
 * 3. WHICH FIELD IS THE POINTER, AND THIS ONE IS A TRAP WITH A NAME. The
 *    component's pointer to its light is `Light` = 0xE4B6881A, declared
 *    Class(LocalLightEntityData) - an ordinary PointerRef. The GDScript reader
 *    and the earlier write-up both follow 0x11F57ECA instead, "an integer field
 *    that actually holds a pointer". It is not one. Under the MULTIPLAYER
 *    schema 0x11F57ECA is a genuine Enum(PBRAnalyticLightShape) four bytes wide
 *    at offset +132, and the pointer is at +120.
 *
 *    Why following it still worked: those readers load the SINGLE-PLAYER
 *    executable, whose schema for the same component puts `Light` at +112 and
 *    0x11F57ECA at +120. Looking up 0x11F57ECA in the SP table therefore yields
 *    offset 120 - which is exactly where the pointer sits in the MP data being
 *    read. Two wrongs, one right answer, and it stops being right the moment a
 *    reader loads the correct executable. libbf6 prefers the MP exe, so it
 *    reads `Light` by name and gets the same instance for the right reason.
 *
 * 4. `Color` AND `Intensity` ARE NOT IN THE SDK NAME TABLE. They live only in
 *    engine_only_field_names.tsv. A reader that resolved names against the SDK
 *    table alone produced 11,640 structurally-correct lights that were all
 *    white at a default intensity - the worst failure mode, because the output
 *    is the right shape and the right count. The hashes below are constants for
 *    that reason: an RFL2 NameHash is not reversible, so a name can only ever
 *    be looked up, never computed.
 *
 * And one convention that inverts a map: A SPOT EMITS ALONG MINUS ITS FORWARD
 * AXIS. See the note on LevelLight::xf.
 *
 * All type guids and field hashes here were read off the game's own reflection
 * tables in the retail MULTIPLAYER executable and named by joining against the
 * SDK and engine-only field-name tables. They are SCHEMA constants, the same
 * kind of thing as velighting.cpp's VE field hashes; no per-map value is baked
 * anywhere.
 */
#ifndef LIBBF6_LEVELLIGHTS_H
#define LIBBF6_LEVELLIGHTS_H

#include <cstdint>
#include <string>
#include <vector>

#include "source.h"
#include "types.h"
#include "walk.h"     // Mat34, mat_mul, mat_identity

namespace bf6 {

// The four light classes the game actually places. Mirrors bf6_light_type in
// the public header. LocalLightEntityData, PointLightEntityData,
// SpotLightEntityData and PbrAnalyticLightEntityData are DECLARED by the engine
// but carry no light fields at all in the shipped schema (LocalLightEntityData
// declares three fields: Flags, Transform, DrawDebugTexturePool), and none was
// observed placed. They are registered anyway so a level that places one is
// counted rather than silently dropped.
enum LightKind {
    kLightSphere = 0,   // PbrSphereLightEntityData      - point, with a radius
    kLightSpot   = 1,   // PbrSpotLightEntityData
    kLightTube   = 2,   // PbrTubeLightEntityData        - line / capsule
    kLightRect   = 3,   // PbrRectangularLightEntityData - area
    kLightOther  = 4    // a declared class with no light fields
};

struct LevelLight {
    int   kind = kLightSphere;

    // World transform, GAME space, rows right/up/forward/translation - the same
    // shape and the same order as WalkRow::xf, so a consumer that already
    // places props places these the same way.
    //
    // A SPOT SHINES ALONG MINUS ROW 2. Measured, not assumed, off fixtures
    // whose real aim is not in question: lf_com_potlight_round_01 is a recessed
    // ceiling downlight and its forward is (0, +1, 0); so are
    // lf_com_flushmount_round_01, lf_ind_ceilingled_square_01, and both street
    // lamps sitting 7 to 10 m up their poles. Taking +forward as the beam has
    // every one of them lighting the ceiling it is set into. Across mp_dumbo's
    // 4,470 spots, -forward points 3,688 of them downwards and +forward points
    // 36.
    //
    // The basis rows carry the holder's SCALE (1.2 and 1.5 are ordinary), so
    // row 2 needs normalising before it is used as a direction, and an area
    // light's world size is its authored size times that scale.
    Mat34 xf = mat_identity();

    // ---- appearance ------------------------------------------------------
    float color[3] = {1.f, 1.f, 1.f};  // LINEAR, authored; 96% are non-white
    float intensity = 0.f;             // see `unit` - NOT a normalised 0..1
    int   unit = 0;                    // LightUnitType: 0 LuminousPower, 1 Luminance
    float dimmer = 1.f;                // authored multiplier on the above

    // ---- falloff ---------------------------------------------------------
    float attenuation_radius = 0.f;    // metres, the light's reach
    float attenuation_offset = 0.f;    // metres, an inner offset on the falloff

    // ---- shape -----------------------------------------------------------
    // Which of these is authored depends on `kind`; the rest stay at zero.
    float inner_angle = 0.f;   // spot, degrees, FULL cone
    float outer_angle = 0.f;   // spot and rect-frustum, degrees, FULL cone
    float shape_radius = 0.f;  // sphere SphereRadius / spot DiscRadius / tube TubeRadius
    float tube_width = 0.f;    // tube, metres end to end
    int   is_capsule = 0;      // tube
    float rect_height = 0.f;   // rect, metres. WIDTH IS NOT AUTHORED: the class
    float rect_aspect = 0.f;   // has Height and Aspect and no Width field at all.
    int   rect_shape = 0;      // RectangularLightShape: 0 Rect, 1 Frustum, 2 OrthoFrustum

    // ---- shadows and contribution ----------------------------------------
    // QualityScalableEnabled is the LOWEST quality preset at which the feature
    // is on - 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Disabled - so 4 means never
    // and 0 means always. Not a boolean, and reading it as one turns "on at
    // Ultra only" into "on".
    int   cast_shadows_enable = 0;
    int   cast_shadows = 0;         // QualityScalableEnabled
    int   cast_volumetric = 0;      // QualityScalableEnabled
    float volumetric_scattering = 0.f;
    int   affect_diffuse = 0, affect_specular = 0, affect_radiosity = 0;
    int   emissive_shape_enable = 0;

    // ---- goniometry ------------------------------------------------------
    std::string ies_profile;   // partition name of the IesProfileAsset, or empty
    float ies_multiplier = 0.f;
    int   ies_as_mask = 0;
    std::string texture;       // rect only: the AtlasTexture it projects, or empty

    // ---- culling ---------------------------------------------------------
    float cull_distance = 0.f;
    float fade_distance = 0.f;

    // ---- provenance ------------------------------------------------------
    std::string source;        // the partition the light was found in
    uint32_t flags = 0;        // the entity's Flags word, MEANING UNDECODED
    int  from_component = 0;   // 1 = placed by its spatial component (see 2 above)
    int  instance = -1;        // instance index inside `source`
};

struct LightStats {
    uint64_t partitions = 0, instances = 0, decoded = 0;
    uint64_t components = 0;        // placement components seen
    uint64_t comp_unlinked = 0;     // component whose Light pointer resolved to nothing
    uint64_t comp_excluded = 0;     // component authored Excluded, light dropped
    uint64_t placed_by_component = 0;
    uint64_t placed_by_own_transform = 0;
    uint64_t placed_at_holder = 0;  // neither: sits exactly on its holder's origin
    uint64_t missing = 0, parse_fail = 0, cycles = 0, unresolved_types = 0;
    uint64_t by_kind[5] = {0, 0, 0, 0, 0};
    // The old 0x11F57ECA join, measured beside the real one rather than
    // asserted wrong. See point 3 in the header comment.
    uint64_t comp_legacy_resolved = 0, comp_legacy_agree = 0;
};

// Walk `level` and collect every placed light. Needs a mounted Source and the
// type schema, and nothing else - in particular it does NOT need the placement
// walk to have run, and does its own lighter traversal instead (it never
// decodes a StaticModelGroup, which is where most of a walk's time goes).
bool level_lights(Source& src, TypeDb& types, const std::string& level,
                  std::vector<LevelLight>& out, LightStats& st, std::string& err,
                  bool bounded_frontend = false);

}  // namespace bf6

#endif
