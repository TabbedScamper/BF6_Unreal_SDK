/* libbf6 internal - PARTICLE EFFECTS: what a level places and how it looks.
 *
 * The decode written down in the codebase's own terms, with every hash and
 * offset the implementation needs. fx.cpp implements it; fx_ext.inc puts it on
 * the C ABI. Verified against data/fx_level_inventory.tsv in the research repo
 * over 14 retail levels: 3,983 of 3,983 layers, 3,983 of 3,983 emitter graphs,
 * 2,577 atlases (2,322 override, 255 template), 52 sheets, 151 graphs, and
 * 3,946 of 3,983 rows agreeing on every one of 67 columns.
 *
 * WHAT AN EFFECT IS, AND THE MISTAKE THAT MAKES IT LOOK WRONG.
 *
 * A placed `fx_*` is an EffectBlueprint holding one EmitterGraphEntityData per
 * EMITTER LAYER, and a layer is the unit that renders: its own sprite sheet,
 * its own spawn rate, its own parameters, its own local offset. 1,368 distinct
 * effects on 14 retail levels expand to 3,983 layers. Merging a partition's
 * instances into "the effect" throws away exactly the per-layer variation that
 * makes an explosion read as an explosion (BaseSize 100/200/35, Drag
 * 0.5/0.0/0.7/0.01 on one effect) and leaves one generic puff.
 *
 * FIVE THINGS THAT FAIL SILENTLY.
 *
 * 1. THE SHEET IS USUALLY AN OVERRIDE, NOT THE TEMPLATE'S. A layer's atlas
 *    comes from its OWN binding array (F_TEX_BINDINGS) when the parameter name
 *    is "AlbedoGS", and only otherwise from the shared emitter template's
 *    GlobalSorting[0]. Measured over 3,983 placed layers: 2,322 override, 255
 *    template. A reader that implements only the template path resolves 255 of
 *    2,577 and reports the rest as textureless.
 *
 * 2. THE ATLAS RES MUST BE FOUND BY RESOURCE ID, NEVER BY NAME. On mp_dumbo the
 *    smoke atlas's RES lives in win32/game/glacierflow/flow_mainmenu, a bundle
 *    no index built from the level's own bundles contains. The name lookup
 *    fails while the game streams it fine, and the failure renders as "no
 *    texture" rather than as an error.
 *
 * 3. FRAME RECTS ARE UV, NOT PIXELS. cols is often 6 or 7 and the sheet is a
 *    power of two, so the cell is 341.333 or 146.286 px. The frames tile
 *    exactly at u = col/cols; flooring to integer pixels shears the last
 *    column. Four of one level's fourteen atlases are affected.
 *
 * 4. A PARAMETER'S VALUE IS AS WIDE AS ITS TYPE SAYS. GpuExposedParameterInput
 *    stores a Vec4 at +0 whatever the parameter is, and Bool/Int values live in
 *    IntValue at +36, not in the floats. Reading all four components of a Float
 *    parameter yields (180.0, 1.0, 1.0, 1.0) where only 180.0 exists.
 *
 * 5. AN INSTANCE GUID IS INDEXED FROM THE PAYLOAD BASE. The 16 bytes before an
 *    instance image, counted from align16(EBXD offset) = 32, not from the raw
 *    EBXD offset = 20. Twelve bytes early returns the previous instance's
 *    trailing fields, which look exactly like a GUID. Only matters if the enum
 *    -instance path (below) is implemented; the PropertyId path avoids it.
 *
 * All hashes below are SCHEMA constants read from the retail multiplayer
 * executable's reflection tables, or djb2-xor of a name the game itself ships
 * in ParticleTypeAsset. No per-map value is baked anywhere.
 */
#ifndef LIBBF6_FX_H
#define LIBBF6_FX_H

#include <cstdint>
#include <string>
#include <vector>

#include "source.h"
#include "types.h"
#include "walk.h"

namespace bf6 {

/* ---- RFL2 field hashes -------------------------------------------------- */

/* EmitterGraph (type 6680c747-ebad-6103-611e-394758591325) */
constexpr uint32_t F_EG_PARAMS            = 0x4832AD52; /* EmitterGraphParams        */
constexpr uint32_t F_EG_GLOBALSORTING     = 0xAF0E5C31; /* GlobalSorting             */
constexpr uint32_t F_EG_PARTICLEDATA      = 0x6BD23581; /* ParticleDataElementInfos  */
constexpr uint32_t F_EG_SPAWNTEXTURES     = 0x03287328;
constexpr uint32_t F_EG_SIMULATETEXTURES  = 0x78A344B3;
constexpr uint32_t F_EG_PARTICLELIFE      = 0x8E72B110; /* QualityScalableFloat      */
constexpr uint32_t F_EG_EMITTERLIFE       = 0xBD5A8A76; /* QualityScalableFloat      */
constexpr uint32_t F_EG_MAXSPAWNDIST      = 0x66DE2067; /* 0 in shipped data         */
constexpr uint32_t F_EG_MINSPAWNDIST      = 0x955EF9A5;
constexpr uint32_t F_EG_GPUCULLDIST       = 0x3A2302DC; /* the REAL cull distance    */
constexpr uint32_t F_EG_PREROLLTIME       = 0x0710D2A9;
constexpr uint32_t F_EG_DRAWLAYER         = 0x95D62EDA;
constexpr uint32_t F_EG_DRAWPASS          = 0xB80D7E73;
constexpr uint32_t F_EG_SORTMODE          = 0xB6FC86A6;
constexpr uint32_t F_EG_GS_PARTICLETYPE   = 0x407E9C12; /* 0 Quad, 1 Ribbon, -1 none */

/* EmitterGraphEntityData - one per LAYER */
constexpr uint32_t F_EGE_GRAPH            = 0xB216DFE5; /* EmitterGraph, an import   */
constexpr uint32_t F_EGE_PARAMS           = 0x4832AD52; /* same hash as the template */
constexpr uint32_t F_EGE_PIDLOOKUP        = 0x1CFA984B; /* PropertyIdLookupTable     */
constexpr uint32_t F_EGE_TRANSFORM        = 0xD6351EDE;
constexpr uint32_t F_EGE_TEX_BINDINGS     = 0x1C1E4B15; /* texture override array    */
constexpr uint32_t F_EGE_EXPR_OVERRIDES   = 0x6730446A; /* enum/expr override array  */

/* EffectEntityData - the effect root */
constexpr uint32_t F_EFF_COMPONENTS       = 0x5E00055A; /* the layer list            */
constexpr uint32_t F_EFF_CULLDISTANCE     = 0xC97C7D4E;
constexpr uint32_t F_EFF_MAXINSTANCES     = 0x631A6AFE;

/* AtlasTextureAsset (type ab548518-e5c4-cec2-6fd1-1753d76efae3) */
constexpr uint32_t F_ATLAS_COLS           = 3460462605u; /* AnimationColumnCount     */
constexpr uint32_t F_ATLAS_FRAMES         =  866427092u; /* AnimationFrameCount      */
constexpr uint32_t F_ATLAS_LR             = 2836644381u; /* LeftRightTiles           */
constexpr uint32_t F_ATLAS_RESOURCE       = 2857347168u; /* Resource, a ResourceId   */

/* ---- element layouts ---------------------------------------------------- */

/* GpuExposedParameterInput, 1c1841ed-a95b-9f5d-da8f-a22a541d5563, stride 64.
 *   +0   Vec4   value        (only width(type) components are meaningful)
 *   +16  Guid   unused in shipped data
 *   +32  u32    PropertyId
 *   +36  i32    IntValue     (Bool and Int values live HERE)
 *   +40  i32    constant-buffer float offset
 *   +44  i32    ExposableType
 *   +48  bool   Normalize
 * cb[k+1] == cb[k] + width(type[k]) holds 31,039/31,039 pairs in eg_ templates
 * and is 0 throughout in fx_ layer overrides, which patch rather than lay out. */
constexpr int FX_PARAM_STRIDE = 64;

enum FxParamType {                    /* GpuExposableParameterType */
    FX_PARAM_FLOAT = 0, FX_PARAM_VEC2 = 1, FX_PARAM_VEC3 = 2,
    FX_PARAM_VEC4  = 3, FX_PARAM_BOOL = 4, FX_PARAM_INT  = 5
};
inline int fx_param_width(int t) {
    switch (t) { case FX_PARAM_VEC2: return 2; case FX_PARAM_VEC3: return 3;
                 case FX_PARAM_VEC4: return 4; default: return 1; }
}

/* AtlasTextureShaderParameter, 4efa4508-c418-b999-fbdf-21b98da55135, stride 16:
 *   +0 Ref -> AtlasTextureAsset,  +8 CString ParameterName                     */
constexpr int FX_ATLASPARAM_STRIDE = 16;

/* The layer's texture-binding element, stride 24:
 *   +0 Ref -> AtlasTextureAsset,  +8 CString name,  +16 u32 djb2x(name)
 * djb2x("AlbedoGS") == 0xDD3FACB0, verified against the shipped value.         */
constexpr int FX_TEXBIND_STRIDE = 24;
constexpr uint32_t FX_PARAMNAME_ALBEDOGS = 0xDD3FACB0;

/* ParticleDataElementInfo, 0deb608f-9f2e-2f87-8775-5f35fd018505, stride 8:
 *   +0 u32 byte offset in the GPU particle,  +4 u32 djb2x(field name)          */
constexpr int FX_PARTICLEFIELD_STRIDE = 8;

/* ---- PropertyIds whose NAME, TYPE, DEFAULT and TOOLTIP ship in the game ----
 * ParticleTypeAsset QuadGS_Gla / RibbonGS_Gla, VertexShaderParams, stride 80.
 * All 22 hash to observed PropertyIds and their declared type matches the type
 * read from parameter data, 22/22, against 0/22 for random control ids.        */
constexpr uint32_t PID_LIGHTING_MODEL_GS   = 0xE6A39680; /* Int  0 Emissive 1 VertexLit 2 GnomonLit */
constexpr uint32_t PID_ALIGNMENT_TYPE      = 0xD1B859C2; /* Int  0 Screen 1 Directional 2 ScreenStretch 3 FullRotation 4 Emitter 5 Up */
constexpr uint32_t PID_USE_RIGHT_TILE      = 0xCB4FC2D2; /* Bool alternate atlas tile, Emissive/VertexLit only */
constexpr uint32_t PID_DISABLE_FRAME_BLEND = 0x45056B2D; /* Bool default path CROSS-FADES frames */
constexpr uint32_t PID_ALPHA_CULL_THRESH   = 0x1487F4B0; /* Float default 0.005 */
constexpr uint32_t PID_CAMERA_BIAS         = 0xBFF3F865; /* Float metres toward camera, render only */
constexpr uint32_t PID_INV_ZFADE_MULT      = 0x141FF6C3; /* Float soft-particle fade */
constexpr uint32_t PID_SUN_LIGHT_SCALE     = 0xF36E7E8B;
constexpr uint32_t PID_LOCAL_LIGHT_SCALE   = 0x209877EE;
constexpr uint32_t PID_AMBIENT_LIGHT_SCALE = 0x1558A27B;
constexpr uint32_t PID_RECEIVED_SHADOW     = 0xEB0563F4;
constexpr uint32_t PID_CLOUD_SHADOW_SCALE  = 0xFF6899EA;
constexpr uint32_t PID_GNOMON_BACKLIGHT    = 0xCC64D81A;
constexpr uint32_t PID_VERTEX_BACKLIGHT    = 0xC0246618;
constexpr uint32_t PID_BACKLIGHT_CONTRAST  = 0x8454003A;
constexpr uint32_t PID_LOCAL_WRAP_SCALE    = 0x277170DA;
constexpr uint32_t PID_LOCAL_DIRECTIONALITY= 0x4FD6E2FA;
constexpr uint32_t PID_SHADOW_RADIUS_MULT  = 0x973336DB;
constexpr uint32_t PID_SHADOW_CENTER       = 0x61302AA8;
constexpr uint32_t PID_GNOMON_RIG_INDEX    = 0x27F4CED3;
constexpr uint32_t PID_LIGHT_MULT_TYPE     = 0x1F0B9D63; /* Int 0 Sun 1 Local 2 Both */
constexpr uint32_t PID_AVOID_TWISTING      = 0x697D30DD; /* Bool, ribbon only */

/* Appearance PropertyIds recovered by dictionary hunt (246 named, control 0 of
 * 2,524 fake). data/fx_property_ids_v2.tsv in the research repo is the full
 * table, including the 2,259 that stay unnamed but still carry a type. */
constexpr uint32_t PID_COLOR0        = 0xA1C184C8;
constexpr uint32_t PID_COLOR1        = 0xA1C184C9;
constexpr uint32_t PID_BASE_SIZE     = 0xBD355AD5;
constexpr uint32_t PID_DRAG          = 0x7C7FD695;
constexpr uint32_t PID_WIND_STRENGTH = 0xE0A01AD4;
constexpr uint32_t PID_ROT_SPEED     = 0x2FD2E956;
constexpr uint32_t PID_SPAWN_SPEED   = 0xCDB76C39;
constexpr uint32_t PID_RANDOM_FORCE  = 0x441BEE03;

enum FxLightingModel { FX_LM_EMISSIVE = 0, FX_LM_VERTEXLIT = 1, FX_LM_GNOMONLIT = 2 };
enum FxAlignment {
    FX_ALIGN_SCREEN = 0, FX_ALIGN_DIRECTIONAL = 1, FX_ALIGN_SCREENSTRETCH = 2,
    FX_ALIGN_FULLROTATION = 3, FX_ALIGN_EMITTER = 4, FX_ALIGN_UP = 5
};

/* ---- decoded structures ------------------------------------------------- */

struct FxParam {
    uint32_t pid;
    int32_t  type;        /* FxParamType                                       */
    int32_t  cb_offset;   /* float offset in the emitter constant buffer       */
    float    v[4];        /* only [0 .. fx_param_width(type)) is meaningful    */
    int32_t  ivalue;      /* the value when type is Bool or Int                */
};

struct FxAtlas {
    std::string name;     /* AtlasTextureAsset partition name                  */
    int32_t  cols = 0;    /* AnimationColumnCount                              */
    int32_t  frames = 0;  /* AnimationFrameCount, TOTAL frames                 */
    bool     left_right = false;
    uint64_t rid = 0;     /* resolve the AtlasTexture RES by THIS, not by name */
    std::string res_name; /* what the rid resolved to, "" when it did not      */
    int32_t  width = 0, height = 0, mips = 0;   /* from the 92-byte RES header */
    std::string chunk_guid;                     /* raw hex, RES header +0x10   */
    uint32_t mip_sizes[15] = {0};               /* RES header +0x20            */
    /* rows = (frames + cols - 1) / cols
     * cell in UV = 1/cols by 1/rows, and when left_right the LEFT half is the
     * sprite: u = (col + f) / (cols * 2). Never compute a pixel rect.         */
};

struct FxLayer {
    int32_t     instance = 0;         /* EBX instance index, the layer id      */
    std::string graph;                /* eg_* template partition path          */
    float       local[12] = {0};      /* Mat34, layer offset inside the effect */

    FxAtlas     atlas;                /* empty when the family draws no sheet  */
    bool        atlas_from_override = false;

    /* spawn, from the template. Every "float" below whose engine type is a
     * QualityScalableFloat is its LOW member; see the note at the top of
     * fx.cpp. */
    std::string spawn_mode;           /* SpawnModeContinuous | SpawnModeBurst  */
    bool   has_spawn_rate = false;    /* burst has no such field at all        */
    float  spawn_rate = 0;            /* particles/s                           */
    int32_t particle_max = 0;
    float  particle_life = 0, emitter_life = 0;
    float  max_spawn_distance = 0;    /* 0 in shipped data; NOT the cull dist  */
    float  min_spawn_distance = 0;
    float  gpu_cull_distance = 0;     /* the REAL cull distance                */
    float  gpu_cull_radius = 0, min_screen_area = 0;
    float  preroll_time = 0;
    int32_t draw_layer = 0, draw_pass = 0, sort_mode = 0;
    int32_t global_sorting_particle_type = 0;   /* 0 Quad, 1 Ribbon, -1 none   */

    /* render mode, read from the parameters and NOT from the draw config      */
    int32_t lighting_model = -1;      /* FxLightingModel, -1 = not authored    */
    int32_t alignment = -1;           /* FxAlignment,     -1 = not authored    */

    /* the template's defaults, then this layer's overrides applied on top     */
    std::vector<FxParam> params;
};

struct FxEffect {
    std::string name;                 /* fx_* leaf name                        */
    std::string path;                 /* fx_* partition path, no .ebx          */
    int32_t placements = 0;           /* how many times the level places it    */
    float  cull_distance = 0;         /* EffectEntityData.CullDistance (Low)   */
    int32_t max_instances = 0;
    int32_t components = 0;           /* Components[] length, a cross-check    */
    std::vector<FxLayer> layers;
};

struct FxPlacement {
    std::string effect;               /* key into the effect table, no .ebx    */
    float  xf[12];                    /* world transform from the level walk   */
};

/* Every counter is a way the read can be wrong while still looking full. A
 * layer with no atlas is NOT one of them: 1,406 of 3,983 shipped layers are the
 * volume-decal, sparks, mesh-shard, distortion and creature families and bind
 * no sheet at all. */
struct FxStats {
    int64_t placements = 0, distinct_effects = 0, effect_failed = 0;
    int64_t partitions = 0, layers = 0;
    int64_t graph_resolved = 0;
    int64_t atlas_resolved = 0, atlas_override = 0, atlas_template = 0;
    int64_t grid_resolved = 0, rid_present = 0, res_resolved = 0, chunk_named = 0;
    int64_t lighting_model = 0, alignment = 0;
    int64_t components = 0;
};

/* The emitter family, from the graph path alone - independent of the lighting
 * model, so the two can be cross-tabulated. */
const char* fx_family(const std::string& graph_path);

/* Decode one fx_* partition into its layers. Resolves the emitter template,
 * merges template parameters with the layer's overrides, resolves the atlas
 * through the override slot first and the template's GlobalSorting second, and
 * reads the atlas grid from the AtlasTextureAsset EBX (never from the
 * filename - t_smoke_wispy_light_7x144_d authors 49 frames, not 144).
 * Returns false and fills err on a hard failure; a layer that resolves no
 * atlas is NOT a failure, it is a non-billboard family. */
bool fx_decode_effect(Source& src, TypeDb& types, const std::string& partition,
                      FxEffect& out, std::string& err);

/* Every fx_* the level places, one entry per placement, plus the distinct
 * effects it references. Uses the same descent the placement walk uses, minus
 * StaticModelGroup members, which place no effects. */
bool fx_level_effects(Source& src, TypeDb& types, const std::string& level,
                      std::vector<FxPlacement>& placements,
                      std::vector<FxEffect>& effects,
                      FxStats& st, std::string& err);

/* The same, when the caller ALREADY has module 9's rows - which the C ABI does,
 * because a context that has opened a level keeps its walk. Reusing it is the
 * difference between seconds and a minute on a big map. */
bool fx_from_walk(Source& src, TypeDb& types, const std::vector<WalkRow>& rows,
                  std::vector<FxPlacement>& placements,
                  std::vector<FxEffect>& effects,
                  FxStats& st, std::string& err);

/* The atlas pixels: the rid is already resolved into `atlas`, so this fetches
 * the chunk named by the raw-hex GUID at RES+0x10 and hands back mip 0 -
 * mip_sizes[0] bytes from the front of the chunk. The payload is BCn as it
 * lies; nothing is decompressed. */
bool fx_atlas_mip0(Source& src, TypeDb& types, const FxAtlas& atlas,
                   std::vector<uint8_t>& out, std::string& err);

}  // namespace bf6

#endif  /* LIBBF6_FX_H */
