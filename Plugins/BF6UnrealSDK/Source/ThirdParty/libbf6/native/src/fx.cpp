/* libbf6 internal - PARTICLE EFFECTS. See fx.h for the schema and the traps.
 *
 * WHAT THIS FILE DOES NOT DO, and why. fx.h documents the element layouts as
 * fixed strides - GpuExposedParameterInput 64, the layer's texture binding 24,
 * AtlasTextureShaderParameter 16 - because the reference decode was written in
 * Python against a raw byte reader. This port reads the same bytes through the
 * SCHEMA instead (module 4 + module 5), because the schema is what the rest of
 * libbf6 already trusts and a stride written down twice drifts. Two of the
 * strides do not survive that check, and one of them changes the answer:
 *
 *   - EmitterGraph.GlobalSorting (0xAF0E5C31) is NOT an array. It is an inline
 *     struct GlobalSortingInfo whose FIRST field,
 *     AtlasTextureParametersRuntime (0xEC5CA9C8), is an
 *     Array<AtlasTextureAsset*> - a pointer array at stride 8, not a 16-byte
 *     AtlasTextureShaderParameter array. Reading the array at the struct's own
 *     offset lands on the same bytes, so element 0 is identical either way.
 *     MEASURED, so that nobody has to worry about it: over the 374 EmitterGraph
 *     instances in one level's mount, 112 carry a non-empty list and 112 of 112
 *     hold exactly ONE element. The two readings therefore cannot disagree on
 *     shipped data. The struct is still what the schema says it is.
 *
 *   - ParticleLifeSpan, EmitterLifeSpan, MaxSpawnDistance, MinSpawnDistance,
 *     GpuParticleCullingDistance, SpawnRate and CullDistance are NOT floats.
 *     They are QualityScalableFloat: four floats, Low / Medium / High / Ultra;
 *     ParticleMaxCount and MaxActiveInstanceCount are QualityScalableInt. A raw
 *     four-byte read at the field offset returns the LOW member. This one is
 *     observable: over the same 374 templates the four members are identical
 *     for ParticleLifeSpan, EmitterLifeSpan, GpuParticleCullingDistance and
 *     MinSpawnDistance (0 of 374 differ), but MaxSpawnDistance differs on 39 of
 *     374, and always the same way - Low 40, Medium 60, High 80, Ultra 100. So
 *     a reader taking four bytes is quoting the low-quality spawn distance and
 *     is 60 metres short of what the game does on Ultra. This file takes .Low
 *     so the two readers stay comparable, and says so rather than leaving it
 *     to look like a plain float.
 *
 * The parts fx.h gets exactly right and that this file therefore does not
 * relitigate: the override-beats-template atlas rule, the PropertyId merge, the
 * IntValue-at-+36 read rule, UV frame rects, and the rid join.
 */
#include "fx.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include "ebx.h"
#include "walk.h"

namespace bf6 {
namespace {

/* ---- small helpers, deliberate duplicates of levellights.cpp's -------------
 * They are file-local there too. Sharing them would mean a fourth header for
 * six one-line functions. */

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool ends_with(const std::string& s, const std::string& t)
{
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::string leaf_of(const std::string& s)
{
    const size_t p = s.find_last_of("/\\");
    return p == std::string::npos ? s : s.substr(p + 1);
}

std::string strip_ebx(std::string n)
{
    if (ends_with(lower(n), ".ebx")) n.resize(n.size() - 4);
    return n;
}

// The .NET mixed-endian spelling the type tables use, back to the 16 raw bytes
// an EFIX type slot holds.
TypeGuid raw_guid(const char* dashed)
{
    TypeGuid g{};
    std::string h;
    for (const char* c = dashed; *c; c++) if (*c != '-') h += *c;
    if (h.size() != 32) return g;
    auto byte = [&](size_t i) {
        return (uint8_t)std::stoul(h.substr(i * 2, 2), nullptr, 16);
    };
    g[0] = byte(3); g[1] = byte(2); g[2] = byte(1); g[3] = byte(0);
    g[4] = byte(5); g[5] = byte(4);
    g[6] = byte(7); g[7] = byte(6);
    for (int i = 8; i < 16; i++) g[(size_t)i] = byte((size_t)i);
    return g;
}

float as_f(const EbxValue* v, float dflt = 0.f)
{
    if (!v) return dflt;
    if (v->kind == EbxValue::Kind::Real) return (float)v->f;
    if (v->kind == EbxValue::Kind::Int)  return (float)v->i;
    if (v->kind == EbxValue::Kind::Uint) return (float)v->u;
    return dflt;
}

int as_i(const EbxValue* v, int dflt = 0)
{
    if (!v) return dflt;
    switch (v->kind) {
    case EbxValue::Kind::Bool: return v->b ? 1 : 0;
    case EbxValue::Kind::Int:  return (int)v->i;
    case EbxValue::Kind::Uint: return (int)v->u;
    case EbxValue::Kind::Real: return (int)v->f;
    default: return dflt;
    }
}

std::string ref_name(const EbxValue* v)
{
    if (!v) return std::string();
    if (v->kind != EbxValue::Kind::ImportRef) return std::string();
    if (v->import_path.empty() || v->import_path == "<not indexed>") return std::string();
    return strip_ebx(v->import_path);
}

// Vec3 members, and LinearTransform's four rows.
constexpr uint32_t K_VEC_X = 956422932u, K_VEC_Y = 1123815262u, K_VEC_Z = 849976220u;
constexpr uint32_t F_LT_RIGHT   = 0xC478CC3B;
constexpr uint32_t F_LT_UP      = 0xBF151EF9;
constexpr uint32_t F_LT_FORWARD = 0x695D12A4;
constexpr uint32_t F_LT_TRANS   = 0xBC4B07B4;

void vec_of(const EbxValue* d, float out[3])
{
    out[0] = out[1] = out[2] = 0.f;
    if (!d || d->kind != EbxValue::Kind::Struct) return;
    out[0] = as_f(d->field(K_VEC_X));
    out[1] = as_f(d->field(K_VEC_Y));
    out[2] = as_f(d->field(K_VEC_Z));
}

// A LinearTransform into the 12 floats FxLayer::local holds: rows right, up,
// forward, translation. Same order Mat34 uses, so the two compare row for row.
void lt_to_local(const EbxValue* t, float out[12])
{
    static const uint32_t members[4] = { F_LT_RIGHT, F_LT_UP, F_LT_FORWARD, F_LT_TRANS };
    for (int i = 0; i < 12; i++) out[i] = 0.f;
    out[0] = out[4] = out[8] = 1.f;                      // identity
    if (!t || t->kind != EbxValue::Kind::Struct) return;
    float built[12];
    for (int i = 0; i < 4; i++) {
        const EbxValue* m = t->field(members[i]);
        if (!m) return;                                  // incomplete is identity
        vec_of(m, built + i * 3);
    }
    std::memcpy(out, built, sizeof(built));
}

/* ---- QualityScalable*, which is what most of these "floats" are ------------
 * Four members, Low/Medium/High/Ultra. Every published FX number is the LOW
 * member, because the reference read four bytes at the field's offset. Taking
 * .Low keeps the two readers comparable AND is the right default: it is the
 * value the field starts with and the one the shipping tables quote. */
constexpr uint32_t F_QS_LOW = 0xD0A94456;

float qs_float(const EbxValue* v, float dflt = 0.f)
{
    if (!v) return dflt;
    if (v->kind == EbxValue::Kind::Struct) return as_f(v->field(F_QS_LOW), dflt);
    return as_f(v, dflt);
}

int qs_int(const EbxValue* v, int dflt = 0)
{
    if (!v) return dflt;
    if (v->kind == EbxValue::Kind::Struct) return as_i(v->field(F_QS_LOW), dflt);
    return as_i(v, dflt);
}

/* ---- type identity ---------------------------------------------------------
 * The retail tables ship stripped names, so a type is its guid. These are the
 * six the decode needs, read out of the executable's own reflection. */
const char kGuidEffectEntity[]        = "5adc67ac-3074-d434-31ba-067699dbdae3";
const char kGuidEmitterGraphEntity[]  = "6dc3f782-ad22-75e2-92de-846a1ebbbf8c";
const char kGuidEmitterGraph[]        = "6680c747-ebad-6103-611e-394758591325";
const char kGuidAtlasTextureAsset[]   = "ab548518-e5c4-cec2-6fd1-1753d76efae3";
const char kGuidSpawnContinuous[]     = "3a5cfcea-c088-1408-822b-e91cd9a511d8";
const char kGuidSpawnBurst[]          = "92eaa3f2-43d5-4189-56a9-9dd1f3b7702e";

/* ---- field hashes this file needs beyond fx.h's set ---------------------- */
constexpr uint32_t F_GS_ATLAS_ARRAY = 0xEC5CA9C8;  // GlobalSortingInfo's array
constexpr uint32_t F_REF_SLOT       = 0x8CF424E7;  // the Ref member of a binding
constexpr uint32_t F_PARAM_NAME     = 0xF114959B;  // ParameterName, a CString
constexpr uint32_t F_PARAM_NAMEHASH = 0xEA178B6F;  // djb2x of it
constexpr uint32_t F_PROPERTY_ID    = 0xE03E9C5F;
constexpr uint32_t F_INT_VALUE      = 0xC82B46DD;
constexpr uint32_t F_CB_OFFSET      = 0x1B0BECEE;
constexpr uint32_t F_EXPOSABLE_TYPE = 0x885464E2;
constexpr uint32_t F_SPAWNRATE      = 0x1E554FB9;
constexpr uint32_t F_PARTICLEMAX    = 0x71363FE0;
constexpr uint32_t F_MINSCREENAREA  = 0x38DB135C;
constexpr uint32_t F_GPUCULLRADIUS  = 0xD4D6C58F;

/* ---- the graph's decoded defaults ---------------------------------------- */
struct GraphInfo {
    bool        ok = false;
    std::string path;                       // eg_* partition, no .ebx
    std::string atlas;                      // GlobalSorting[0], no .ebx
    std::string spawn_mode;
    bool   has_spawn_rate = false;
    float  spawn_rate = 0.f;
    int    particle_max = 0;
    float  particle_life = 0.f, emitter_life = 0.f;
    float  max_spawn_distance = 0.f, min_spawn_distance = 0.f;
    float  gpu_cull_distance = 0.f, gpu_cull_radius = 0.f, min_screen_area = 0.f;
    float  preroll_time = 0.f;
    int    draw_layer = 0, draw_pass = 0, sort_mode = 0;
    int    gs_particle_type = 0;
    std::vector<FxParam> params;            // the DEFAULTS
};

}  // namespace

/* ------------------------------------------------------------------------- */

// One decoder per run. Everything expensive - the partition-guid index, the
// resource-id index, and the per-graph and per-atlas decodes - is built once
// and shared: 3,983 placed layers across 14 levels reference 151 distinct
// graphs and 52 distinct sheets, so a reader that opens the template per layer
// does 26x the work for the same answer.
class FxDecoder {
public:
    FxDecoder(Source& src, TypeDb& types) : src_(src), types_(types)
    {
        g_effect_    = raw_guid(kGuidEffectEntity);
        g_layer_     = raw_guid(kGuidEmitterGraphEntity);
        g_graph_     = raw_guid(kGuidEmitterGraph);
        g_atlas_     = raw_guid(kGuidAtlasTextureAsset);
        g_spawn_con_ = raw_guid(kGuidSpawnContinuous);
        g_spawn_bur_ = raw_guid(kGuidSpawnBurst);
    }

    void build_index()
    {
        if (!gi_.empty()) return;
        gi_ = src_.partition_index();
    }

    bool decode_effect(const std::string& partition, FxEffect& out, std::string& err);
    const FxAtlas* atlas(const std::string& name);
    bool  mip0(const FxAtlas& a, std::vector<uint8_t>& out, std::string& err);

    // Counters. Every one of them is a way this can be wrong while still
    // producing a full-looking table.
    uint64_t n_effects = 0, n_effect_missing = 0, n_effect_parsefail = 0;
    uint64_t n_layers = 0, n_graph_ok = 0, n_graph_missing = 0;
    uint64_t n_atlas_ok = 0, n_atlas_override = 0, n_atlas_template = 0;
    uint64_t n_grid_ok = 0, n_rid_ok = 0, n_res_ok = 0, n_chunk_ok = 0;
    uint64_t n_lm = 0, n_align = 0;
    uint64_t n_components = 0, n_components_matched = 0;

private:
    const GraphInfo& graph(const std::string& path);
    std::vector<FxParam> read_params(const EbxValue* arr) const;
    void  build_rid_index();

    Source&  src_;
    TypeDb&  types_;
    std::map<std::string, std::string> gi_;
    std::map<std::string, GraphInfo>   graphs_;
    std::map<std::string, FxAtlas>     atlases_;
    std::unordered_map<uint64_t, std::string> by_rid_;
    bool     rid_built_ = false;

    TypeGuid g_effect_{}, g_layer_{}, g_graph_{}, g_atlas_{};
    TypeGuid g_spawn_con_{}, g_spawn_bur_{};
};

/* ---- parameters ---------------------------------------------------------- */

// A GpuExposedParameterInput array. THE VALUE IS AS WIDE AS THE TYPE SAYS: only
// the first width(ExposableType) components of the Vec4 exist, and Bool and Int
// carry their value in IntValue, not in the floats. Reading all four back gives
// a Float parameter as (180.0, 1.0, 1.0, 1.0) where only 180.0 was written.
std::vector<FxParam> FxDecoder::read_params(const EbxValue* arr) const
{
    std::vector<FxParam> out;
    if (!arr || arr->kind != EbxValue::Kind::Array) return out;
    out.reserve(arr->items.size());
    for (const EbxValue& e : arr->items) {
        if (e.kind != EbxValue::Kind::Struct) continue;
        FxParam p;
        p.pid = (uint32_t)as_i(e.field(F_PROPERTY_ID));
        if (!p.pid) continue;                  // an unset slot, not a parameter
        p.ivalue    = as_i(e.field(F_INT_VALUE));
        p.cb_offset = as_i(e.field(F_CB_OFFSET));
        p.type      = as_i(e.field(F_EXPOSABLE_TYPE));
        const EbxValue* v = e.field(F_REF_SLOT);   // the Vec4 at +0
        p.v[0] = p.v[1] = p.v[2] = p.v[3] = 0.f;
        if (v && v->kind == EbxValue::Kind::Struct) {
            // Vec4's members are Vec3's three plus W; reading by name rather
            // than by slot keeps it right if the order ever moves.
            static const uint32_t K_VEC_W = 0x7C8062F2u;
            p.v[0] = as_f(v->field(K_VEC_X));
            p.v[1] = as_f(v->field(K_VEC_Y));
            p.v[2] = as_f(v->field(K_VEC_Z));
            p.v[3] = as_f(v->field(K_VEC_W));
        }
        out.push_back(p);
    }
    return out;
}

/* ---- the emitter template ------------------------------------------------ */

const GraphInfo& FxDecoder::graph(const std::string& path)
{
    auto it = graphs_.find(path);
    if (it != graphs_.end()) return it->second;

    GraphInfo gi;
    gi.path = path;
    if (path.empty()) return graphs_.emplace(path, gi).first->second;

    std::string err;
    std::vector<uint8_t> data = src_.get_ebx(path, err);
    if (data.empty()) return graphs_.emplace(path, gi).first->second;

    Ebx e(types_);
    e.set_guid_index(&gi_);
    if (!e.parse(std::move(data), err)) return graphs_.emplace(path, gi).first->second;

    static const std::vector<uint32_t> want_graph = {
        F_EG_GLOBALSORTING, F_EG_PARAMS, F_EG_PARTICLELIFE, F_EG_EMITTERLIFE,
        F_EG_MAXSPAWNDIST, F_EG_MINSPAWNDIST, F_EG_GPUCULLDIST, F_EG_PREROLLTIME,
        F_EG_DRAWLAYER, F_EG_DRAWPASS, F_EG_SORTMODE, F_EG_GS_PARTICLETYPE,
        F_MINSCREENAREA, F_GPUCULLRADIUS
    };
    static const std::vector<uint32_t> want_spawn = { F_SPAWNRATE, F_PARTICLEMAX };

    for (size_t i = 0; i < e.instance_count(); i++) {
        const TypeGuid t = e.instance_type(i);
        if (t == g_graph_) {
            const EbxValue d = e.read_instance(i, &want_graph);
            if (d.kind != EbxValue::Kind::Struct) continue;
            gi.ok = true;

            // GlobalSorting is a STRUCT holding the array. Element 0 is the
            // sheet; the other elements are the template's other atlas slots
            // and are not the albedo.
            if (const EbxValue* gsi = d.field(F_EG_GLOBALSORTING))
                if (gsi->kind == EbxValue::Kind::Struct)
                    if (const EbxValue* arr = gsi->field(F_GS_ATLAS_ARRAY))
                        if (arr->kind == EbxValue::Kind::Array && !arr->items.empty())
                            gi.atlas = ref_name(&arr->items[0]);

            gi.particle_life      = qs_float(d.field(F_EG_PARTICLELIFE));
            gi.emitter_life       = qs_float(d.field(F_EG_EMITTERLIFE));
            gi.max_spawn_distance = qs_float(d.field(F_EG_MAXSPAWNDIST));
            gi.min_spawn_distance = qs_float(d.field(F_EG_MINSPAWNDIST));
            gi.gpu_cull_distance  = qs_float(d.field(F_EG_GPUCULLDIST));
            gi.gpu_cull_radius    = as_f(d.field(F_GPUCULLRADIUS));
            gi.min_screen_area    = as_f(d.field(F_MINSCREENAREA));
            gi.preroll_time       = as_f(d.field(F_EG_PREROLLTIME));
            gi.draw_layer         = as_i(d.field(F_EG_DRAWLAYER));
            gi.draw_pass          = as_i(d.field(F_EG_DRAWPASS));
            gi.sort_mode          = as_i(d.field(F_EG_SORTMODE));
            gi.gs_particle_type   = as_i(d.field(F_EG_GS_PARTICLETYPE), -1);
            gi.params             = read_params(d.field(F_EG_PARAMS));
        }
        else if (t == g_spawn_con_ || t == g_spawn_bur_) {
            const EbxValue d = e.read_instance(i, &want_spawn);
            gi.spawn_mode = (t == g_spawn_con_) ? "SpawnModeContinuous" : "SpawnModeBurst";
            if (d.kind != EbxValue::Kind::Struct) continue;
            gi.particle_max = qs_int(d.field(F_PARTICLEMAX));
            if (t == g_spawn_con_) {
                // Burst authors no rate at all - the type does not have the
                // field - so an absent rate is a MODE, not a missing read.
                gi.has_spawn_rate = true;
                gi.spawn_rate = qs_float(d.field(F_SPAWNRATE));
            }
        }
    }
    return graphs_.emplace(path, gi).first->second;
}

/* ---- the atlas ----------------------------------------------------------- */

void FxDecoder::build_rid_index()
{
    if (rid_built_) return;
    rid_built_ = true;
    // THE ATLAS RES IS FOUND BY RESOURCE ID, NEVER BY NAME. On mp_dumbo the
    // smoke sheet's RES lives in win32/game/glacierflow/flow_mainmenu, which no
    // index built from the level's own bundles contains; the name lookup fails
    // while the game streams it perfectly, and the failure renders as "no
    // texture" rather than as an error.
    for (const auto& kv : src_.res())
        if (kv.second.rid) by_rid_.emplace(kv.second.rid, kv.first);
}

const FxAtlas* FxDecoder::atlas(const std::string& name)
{
    if (name.empty()) return nullptr;
    auto it = atlases_.find(name);
    if (it != atlases_.end()) return it->second.cols ? &it->second : nullptr;

    FxAtlas a;
    a.name = name;
    std::string err;
    std::vector<uint8_t> data = src_.get_ebx(name, err);
    if (!data.empty()) {
        Ebx e(types_);
        e.set_guid_index(&gi_);
        if (e.parse(std::move(data), err)) {
            static const std::vector<uint32_t> want = {
                F_ATLAS_COLS, F_ATLAS_FRAMES, F_ATLAS_LR, F_ATLAS_RESOURCE
            };
            for (size_t i = 0; i < e.instance_count(); i++) {
                if (!(e.instance_type(i) == g_atlas_)) continue;
                const EbxValue d = e.read_instance(i, &want);
                if (d.kind != EbxValue::Kind::Struct) continue;
                // THE GRID COMES FROM THE ASSET, NEVER FROM THE FILENAME.
                // t_smoke_wispy_light_7x144_d authors 49 frames, not 144.
                a.cols   = as_i(d.field(F_ATLAS_COLS));
                a.frames = as_i(d.field(F_ATLAS_FRAMES));
                a.left_right = as_i(d.field(F_ATLAS_LR)) != 0;
                if (const EbxValue* r = d.field(F_ATLAS_RESOURCE))
                    if (r->kind == EbxValue::Kind::ResRef) a.rid = r->u;
                break;
            }
        }
    }

    // The RES, by rid, then its 92-byte header. Not fatal: a grid without a
    // resolved resource is still a usable sheet description.
    if (a.rid) {
        build_rid_index();
        auto ri = by_rid_.find(a.rid);
        if (ri != by_rid_.end()) {
            std::string e2;
            std::vector<uint8_t> res = src_.get_res(ri->second, e2);
            if (res.size() >= 92) {
                a.width  = (int32_t)(res[2] | (res[3] << 8));
                a.height = (int32_t)(res[4] | (res[5] << 8));
                a.mips   = res[6];
                char t[33];
                for (int i = 0; i < 16; i++)
                    std::snprintf(t + i * 2, 3, "%02x", res[0x10 + (size_t)i]);
                a.chunk_guid.assign(t, 32);
                std::memcpy(a.mip_sizes, res.data() + 0x20, sizeof(a.mip_sizes));
                a.res_name = ri->second;
            }
        }
    }

    FxAtlas& stored = atlases_.emplace(name, a).first->second;
    return stored.cols ? &stored : nullptr;
}

bool FxDecoder::mip0(const FxAtlas& a, std::vector<uint8_t>& out, std::string& err)
{
    out.clear();
    if (a.chunk_guid.empty()) { err = "atlas " + a.name + " has no chunk guid"; return false; }
    std::vector<uint8_t> chunk = src_.get_chunk(a.chunk_guid, err);
    if (chunk.empty()) { if (err.empty()) err = "chunk " + a.chunk_guid + " not in the mount"; return false; }
    const size_t n = a.mip_sizes[0] ? (size_t)a.mip_sizes[0] : chunk.size();
    if (n > chunk.size()) { err = "mip0 is longer than its chunk"; return false; }
    out.assign(chunk.begin(), chunk.begin() + (ptrdiff_t)n);
    return true;
}

/* ---- one effect ---------------------------------------------------------- */

bool FxDecoder::decode_effect(const std::string& partition, FxEffect& out, std::string& err)
{
    build_index();
    out = FxEffect();
    out.name = leaf_of(strip_ebx(partition));
    out.path = strip_ebx(partition);

    std::vector<uint8_t> data = src_.get_ebx(out.path, err);
    if (data.empty()) { n_effect_missing++; if (err.empty()) err = "no such partition"; return false; }

    Ebx e(types_);
    e.set_guid_index(&gi_);
    if (!e.parse(std::move(data), err)) { n_effect_parsefail++; return false; }
    n_effects++;

    static const std::vector<uint32_t> want_eff = {
        F_EFF_CULLDISTANCE, F_EFF_MAXINSTANCES, F_EFF_COMPONENTS
    };
    static const std::vector<uint32_t> want_layer = {
        F_EGE_TRANSFORM, F_EGE_GRAPH, F_EGE_PARAMS, F_EGE_TEX_BINDINGS
    };

    // COMPONENTS IS NOT THE LAYER LIST, and it is the obvious route to the
    // layers, so the number is kept as a counter rather than left to be
    // rediscovered. EffectEntityData.Components is an Array<GameObjectData*>
    // and the emitter layers are only part of it. Summed over each level's
    // distinct effects: mp_dumbo 157 components against 137 layers, mp_badlands
    // 436 against 310, mp_subsurface 785 against 665 - 15% to 41% more. The
    // reliable enumeration is the partition's INSTANCES filtered by type guid,
    // which also hands back the instance index that IS the layer id.
    for (size_t i = 0; i < e.instance_count(); i++) {
        if (!(e.instance_type(i) == g_effect_)) continue;
        const EbxValue d = e.read_instance(i, &want_eff);
        if (d.kind != EbxValue::Kind::Struct) continue;
        out.cull_distance = qs_float(d.field(F_EFF_CULLDISTANCE));
        out.max_instances = qs_int(d.field(F_EFF_MAXINSTANCES));
        if (const EbxValue* comp = d.field(F_EFF_COMPONENTS))
            if (comp->kind == EbxValue::Kind::Array) {
                n_components += comp->items.size();
                out.components = (int32_t)comp->items.size();
            }
        break;
    }

    for (size_t i = 0; i < e.instance_count(); i++) {
        if (!(e.instance_type(i) == g_layer_)) continue;
        const EbxValue d = e.read_instance(i, &want_layer);
        if (d.kind != EbxValue::Kind::Struct) continue;

        FxLayer L;
        L.instance = (int32_t)i;
        lt_to_local(d.field(F_EGE_TRANSFORM), L.local);
        L.graph = ref_name(d.field(F_EGE_GRAPH));
        n_layers++;

        const GraphInfo& gi = graph(L.graph);
        if (gi.ok) n_graph_ok++; else if (!L.graph.empty()) n_graph_missing++;

        L.spawn_mode        = gi.spawn_mode;
        L.spawn_rate        = gi.spawn_rate;
        L.has_spawn_rate    = gi.has_spawn_rate;
        L.particle_max      = gi.particle_max;
        L.particle_life     = gi.particle_life;
        L.emitter_life      = gi.emitter_life;
        L.max_spawn_distance= gi.max_spawn_distance;
        L.min_spawn_distance= gi.min_spawn_distance;
        L.gpu_cull_distance = gi.gpu_cull_distance;
        L.gpu_cull_radius   = gi.gpu_cull_radius;
        L.min_screen_area   = gi.min_screen_area;
        L.preroll_time      = gi.preroll_time;
        L.draw_layer        = gi.draw_layer;
        L.draw_pass         = gi.draw_pass;
        L.sort_mode         = gi.sort_mode;
        L.global_sorting_particle_type = gi.gs_particle_type;

        // THE SHEET IS USUALLY AN OVERRIDE. The layer's own binding array wins
        // where its parameter name is "AlbedoGS"; the template's
        // GlobalSorting[0] is the fallback. Measured over 3,983 placed layers:
        // 2,322 override, 255 template. Implementing only the template path
        // resolves 255 of 2,577 and calls the rest textureless.
        std::string atlas_name;
        if (const EbxValue* arr = d.field(F_EGE_TEX_BINDINGS))
            if (arr->kind == EbxValue::Kind::Array)
                for (const EbxValue& b : arr->items) {
                    if (b.kind != EbxValue::Kind::Struct) continue;
                    const EbxValue* nm = b.field(F_PARAM_NAME);
                    const EbxValue* nh = b.field(F_PARAM_NAMEHASH);
                    const bool is_albedo =
                        (nm && nm->kind == EbxValue::Kind::Str && nm->s == "AlbedoGS") ||
                        (nh && (uint32_t)as_i(nh) == FX_PARAMNAME_ALBEDOGS);
                    if (!is_albedo) continue;
                    const std::string r = ref_name(b.field(F_REF_SLOT));
                    if (!r.empty()) { atlas_name = r; L.atlas_from_override = true; }
                    break;
                }
        if (atlas_name.empty()) atlas_name = gi.atlas;

        if (!atlas_name.empty()) {
            if (const FxAtlas* a = atlas(atlas_name)) {
                L.atlas = *a;
                n_atlas_ok++;
                if (L.atlas_from_override) n_atlas_override++; else n_atlas_template++;
                if (a->cols) n_grid_ok++;
                if (a->rid)  n_rid_ok++;
                if (!a->res_name.empty()) n_res_ok++;
                if (!a->chunk_guid.empty()) n_chunk_ok++;
            } else {
                L.atlas.name = atlas_name;      // named but unreadable, said so
            }
        }

        // THE TEMPLATE'S DEFAULTS FIRST, THEN THIS LAYER'S OVERRIDES ON TOP.
        // The override table carries cb = 0 throughout - it patches a value and
        // leaves the buffer layout to the template - so the merge keeps the
        // template's cb offset where the template had one.
        L.params = gi.params;
        std::map<uint32_t, size_t> at;
        for (size_t k = 0; k < L.params.size(); k++) at[L.params[k].pid] = k;
        for (const FxParam& p : read_params(d.field(F_EGE_PARAMS))) {
            auto f = at.find(p.pid);
            if (f == at.end()) { at[p.pid] = L.params.size(); L.params.push_back(p); }
            else {
                FxParam& tgt = L.params[f->second];
                const int32_t cb = tgt.cb_offset;
                tgt = p;
                if (tgt.cb_offset == 0) tgt.cb_offset = cb;
            }
        }

        // The render mode, read from the parameters and NOT from the draw
        // config: 267 distinct draw-config signatures over 565 templates and
        // not one of their 21 booleans separates Emissive from GnomonLit.
        // Int and Bool carry their value in IntValue at +36.
        for (const FxParam& p : L.params) {
            if (p.type != FX_PARAM_INT && p.type != FX_PARAM_BOOL) continue;
            if (p.pid == PID_LIGHTING_MODEL_GS) { L.lighting_model = p.ivalue; n_lm++; }
            else if (p.pid == PID_ALIGNMENT_TYPE) { L.alignment = p.ivalue; n_align++; }
        }

        out.layers.push_back(std::move(L));
    }
    if ((int32_t)out.layers.size() == out.components) n_components_matched++;
    return true;
}

/* ---- the level traversal -------------------------------------------------
 *
 * THE WALK IS NOT REIMPLEMENTED HERE, and the first attempt at this file did
 * reimplement it, which is why the note is worth the space. A hand-rolled
 * descent that follows Blueprint / BundleName / Objects / DataRefs / Components
 * and stops at anything whose leaf starts with `fx_` looks right and is not:
 * on mp_dumbo it returned 14,576 placements of 259 distinct effects against the
 * 1,010 of 70 the real walk finds. The excess is entirely subtrees module 9
 * already knows to stop at - the ten DESTRUCTION-branch types, whose subtree is
 * a destroyed state full of break effects, and StaticModelGroup members - plus
 * the four non-playable subworld paths. Every one of those extra rows is a real
 * fx_ reference in the data; none of them is placed in the level as shipped.
 *
 * So this asks module 9 for the level and keeps the rows that name an fx_
 * partition. A placed effect surfaces there as a LEAF row: walk.cpp descends
 * into the effect, finds it emits no geometry, and emits the leaf itself
 * rather than letting the placement vanish.
 */

namespace {

bool is_fx_row(const std::string& mesh)
{
    return leaf_of(lower(mesh)).rfind("fx_", 0) == 0;
}

}  // namespace

/* ---- the family classifier ------------------------------------------------
 * From the GRAPH's own path and nothing else, so it is independent of the
 * lighting model it is cross-tabulated against. A layer with no sheet is a
 * classification, not a miss: volume decals, sparks, mesh shards, distortion
 * and the creature families bind no flipbook. */
const char* fx_family(const std::string& graph_path)
{
    const std::string g = lower(graph_path);
    auto has = [&](const char* s) { return g.find(s) != std::string::npos; };
    if (has("/globalsorting/")) return "billboard_globalsorting";
    if (has("volumedecal"))     return "volumedecal";
    if (has("spark"))           return "sparks";
    if (has("debris") || has("shard") || has("pebble")) return "debris_mesh";
    if (has("distortion"))      return "distortion";
    if (has("bird") || has("insect") || has("rat") || has("butterfly") || has("fish"))
        return "creature";
    if (has("lensflare"))       return "lensflare";
    if (has("ribbon") || has("trail")) return "ribbon";
    return "other";
}

/* ---- the public entry points --------------------------------------------- */

bool fx_decode_effect(Source& src, TypeDb& types, const std::string& partition,
                      FxEffect& out, std::string& err)
{
    FxDecoder d(src, types);
    return d.decode_effect(partition, out, err);
}

bool fx_level_effects(Source& src, TypeDb& types, const std::string& level,
                      std::vector<FxPlacement>& placements,
                      std::vector<FxEffect>& effects,
                      FxStats& st, std::string& err)
{
    Walk w(src, types);
    w.build_catalog();
    if (!w.run(level, err)) return false;
    return fx_from_walk(src, types, w.rows(), placements, effects, st, err);
}

bool fx_from_walk(Source& src, TypeDb& types, const std::vector<WalkRow>& rows,
                  std::vector<FxPlacement>& placements,
                  std::vector<FxEffect>& effects,
                  FxStats& st, std::string& err)
{
    placements.clear();
    effects.clear();
    st = FxStats();
    err.clear();

    FxDecoder dec(src, types);
    dec.build_index();

    for (const WalkRow& r : rows) {
        if (!is_fx_row(r.mesh)) continue;
        FxPlacement p;
        p.effect = strip_ebx(lower(r.mesh));
        for (int k = 0; k < 4; k++) {
            p.xf[k * 3 + 0] = r.xf.m[k].x;
            p.xf[k * 3 + 1] = r.xf.m[k].y;
            p.xf[k * 3 + 2] = r.xf.m[k].z;
        }
        placements.push_back(std::move(p));
    }

    std::map<std::string, int> count;
    for (const FxPlacement& p : placements) count[p.effect]++;
    st.placements = (int64_t)placements.size();
    st.distinct_effects = (int64_t)count.size();

    // Decode each distinct effect ONCE. 10,790 placements over 14 levels are
    // 1,368 distinct effects; decoding per placement is 8x the work.
    effects.reserve(count.size());
    for (const auto& kv : count) {
        FxEffect e;
        std::string e2;
        if (!dec.decode_effect(kv.first, e, e2)) { st.effect_failed++; continue; }
        e.placements = kv.second;
        effects.push_back(std::move(e));
    }

    st.layers          = (int64_t)dec.n_layers;
    st.graph_resolved  = (int64_t)dec.n_graph_ok;
    st.atlas_resolved  = (int64_t)dec.n_atlas_ok;
    st.atlas_override  = (int64_t)dec.n_atlas_override;
    st.atlas_template  = (int64_t)dec.n_atlas_template;
    st.grid_resolved   = (int64_t)dec.n_grid_ok;
    st.rid_present     = (int64_t)dec.n_rid_ok;
    st.res_resolved    = (int64_t)dec.n_res_ok;
    st.chunk_named     = (int64_t)dec.n_chunk_ok;
    st.lighting_model  = (int64_t)dec.n_lm;
    st.alignment       = (int64_t)dec.n_align;
    st.components      = (int64_t)dec.n_components;
    return true;
}

bool fx_atlas_mip0(Source& src, TypeDb& types, const FxAtlas& a,
                   std::vector<uint8_t>& out, std::string& err)
{
    FxDecoder d(src, types);
    return d.mip0(a, out, err);
}

}  // namespace bf6
