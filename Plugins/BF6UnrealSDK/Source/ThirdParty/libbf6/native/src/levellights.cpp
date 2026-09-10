#include "levellights.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>

#include "ebx.h"

namespace bf6 {
namespace {

// ---- the graph fields the descent reads -------------------------------------
// The same set walk.cpp uses, MINUS the StaticModelGroup members. A group is
// mesh instancing and holds no lights, and walk.cpp returns without descending
// the moment it sees one - so not decoding them at all is an equivalence here,
// and it is where the saving is: a group's per-instance transform array is the
// single most expensive thing a level traversal decodes.
constexpr uint32_t F_OBJECTS      = 0x5A6115B2;
constexpr uint32_t F_DATAREFS     = 0xCDA4739B;
constexpr uint32_t F_COMPONENTS   = 0x5E00055A;
constexpr uint32_t F_BUNDLENAME   = 0x095E926D;
constexpr uint32_t F_BP_TRANSFORM = 0x7B554EF5;
constexpr uint32_t F_BLUEPRINT    = 0x6B831B88;
constexpr uint32_t F_EXCLUDED     = 0x4CD269A8;
constexpr uint32_t F_SMG_MEMBERS  = 0xA43BD992;

const uint32_t kDescendFields[] = {
    F_BP_TRANSFORM, F_EXCLUDED, F_BUNDLENAME, F_BLUEPRINT,
    F_OBJECTS, F_DATAREFS, F_COMPONENTS
};

// ---- the light's own fields -------------------------------------------------
// Read off PbrSphereLightEntityData / PbrSpotLightEntityData /
// PbrRectangularLightEntityData / PbrTubeLightEntityData in the retail MP
// executable's reflection tables and named against the two field-name tables.
//
// Color and Intensity are in engine_only_field_names.tsv and NOT in the SDK
// table. Resolving names against the SDK table alone yields a full set of
// structurally-correct lights that are all white at a default intensity.
constexpr uint32_t F_TRANSFORM   = 0xD6351EDE;
constexpr uint32_t F_COLOR       = 0x33ED1C78;   // engine-only table
constexpr uint32_t F_INTENSITY   = 0x13763A5B;   // engine-only table
constexpr uint32_t F_LIGHT_UNIT  = 0xC07607F5;
constexpr uint32_t F_ATTEN_RAD   = 0xC21D1F46;
constexpr uint32_t F_ATTEN_OFF   = 0x59B4B7F1;
constexpr uint32_t F_DIMMER      = 0x3490FF2F;
constexpr uint32_t F_INNER_ANGLE = 0xFADCE54A;
constexpr uint32_t F_OUTER_ANGLE = 0x56FC2180;
constexpr uint32_t F_SPHERE_RAD  = 0xC1E3E627;   // SphereRadius, sphere only
constexpr uint32_t F_DISC_RAD    = 0x5B5C4983;   // DiscRadius, spot only
constexpr uint32_t F_TUBE_RAD    = 0xE5114673;
constexpr uint32_t F_TUBE_WIDTH  = 0x9263A1BF;
constexpr uint32_t F_IS_CAPSULE  = 0xE58452AB;
constexpr uint32_t F_RECT_HEIGHT = 0x28CEA299;   // Height, rect only
constexpr uint32_t F_RECT_ASPECT = 0x41347E83;   // Aspect, rect only
constexpr uint32_t F_RECT_SHAPE  = 0x8C6204F7;   // Shape, RectangularLightShape
constexpr uint32_t F_CAST_SHAD_E = 0x3CDCF4DF;   // CastShadowsEnable, Bool
constexpr uint32_t F_CAST_SHAD   = 0x54C82854;   // CastShadows, QualityScalableEnabled
constexpr uint32_t F_CAST_VOL    = 0xCE1C23E5;   // CastVolumetric, QualityScalableEnabled
constexpr uint32_t F_VOL_SCATTER = 0xB486AA26;
constexpr uint32_t F_AFF_DIFFUSE = 0x65097A70;
constexpr uint32_t F_AFF_SPEC    = 0xBF34D1FF;
constexpr uint32_t F_AFF_RADIO   = 0xFBE93326;
constexpr uint32_t F_EMISSIVE_SH = 0x65D3E175;
constexpr uint32_t F_IES_PROFILE = 0x6821A47E;   // Class(IesProfileAsset)
constexpr uint32_t F_IES_MULT    = 0x0F4BC061;
constexpr uint32_t F_IES_MASK    = 0x88492AE3;
constexpr uint32_t F_TEXTURE     = 0xFBED94F3;   // Class(AtlasTextureBaseAsset), rect
constexpr uint32_t F_CULL_DIST   = 0xC97C7D4E;
constexpr uint32_t F_FADE_DIST   = 0xAA6E133A;
constexpr uint32_t F_FLAGS       = 0x5645E663;

// NOT IN THIS LIST, AND THAT IS THE RESULT: `Enabled` and `Visible`. Neither
// appears on any of the four light classes in the shipped schema - the SDK name
// table carries six distinct hashes spelled Enabled and a Visible, and the
// readers that came before this one tested all seven on every light. None of
// them can ever be present, so the test is a no-op that reads as a safety
// check. What DOES turn a placed light off is its component's `Excluded`, which
// is read below, and `Dimmer`, which is reported.

const uint32_t kLightFields[] = {
    F_TRANSFORM, F_COLOR, F_INTENSITY, F_LIGHT_UNIT, F_ATTEN_RAD, F_ATTEN_OFF,
    F_DIMMER, F_INNER_ANGLE, F_OUTER_ANGLE, F_SPHERE_RAD, F_DISC_RAD,
    F_TUBE_RAD, F_TUBE_WIDTH, F_IS_CAPSULE, F_RECT_HEIGHT, F_RECT_ASPECT,
    F_RECT_SHAPE, F_CAST_SHAD_E, F_CAST_SHAD, F_CAST_VOL, F_VOL_SCATTER,
    F_AFF_DIFFUSE, F_AFF_SPEC, F_AFF_RADIO, F_EMISSIVE_SH, F_IES_PROFILE,
    F_IES_MULT, F_IES_MASK, F_TEXTURE, F_CULL_DIST, F_FADE_DIST, F_FLAGS
};

// ---- the placement component ------------------------------------------------
// dcac04fc is an object component carrying a Transform, a Components array, an
// Excluded flag, an IESProfile and a `Light` pointer at the light it places. It
// has no name in sdk_type_guids.tsv; it was identified by behaviour and by
// count correlation, and the MP reflection tables then named its fields.
//
// F_COMP_LIGHT is `Light`, declared Class(LocalLightEntityData) - a plain
// PointerRef. See the header for why 0x11F57ECA, which earlier readers follow,
// is NOT this pointer and only appeared to be one.
const char* kCompGuid   = "dcac04fc-2a7a-e798-1382-328a95b9484a";
constexpr uint32_t F_COMP_LIGHT  = 0xE4B6881A;
constexpr uint32_t F_COMP_LEGACY = 0x11F57ECA;   // Enum(PBRAnalyticLightShape)

const uint32_t kCompFields[] = { F_TRANSFORM, F_COMP_LIGHT, F_EXCLUDED, F_IES_PROFILE };

// ---- LinearTransform --------------------------------------------------------
constexpr uint32_t F_LT_RIGHT   = 0xC478CC3B;
constexpr uint32_t F_LT_UP      = 0xBF151EF9;
constexpr uint32_t F_LT_FORWARD = 0x695D12A4;
constexpr uint32_t F_LT_TRANS   = 0xBC4B07B4;
constexpr uint32_t K_VEC_X = 956422932, K_VEC_Y = 1123815262, K_VEC_Z = 849976220;
const char* kLtGuid = "06ce1d10-9a4e-fc64-2a9f-a9d482576ffa";

// ---- the light classes ------------------------------------------------------
// TYPE GUIDS COME IN PAIRS in the SDK tables: two spellings of the same type,
// the second with the first three groups byte-swapped. Both are registered
// because which one a given partition writes is not worth guessing at.
struct KindEntry { const char* guid; int kind; };
const KindEntry kLightTypes[] = {
    { "d0528228-e56b-10ab-434c-834bb3d8a821", kLightSphere },   // PbrSphereLightEntityData
    { "10abe56b-4c43-4b83-b3d8-a82181ba3193", kLightSphere },
    { "00215d53-9aaa-8dc4-b3de-cdc707ebb39f", kLightSpot },     // PbrSpotLightEntityData
    { "8dc49aaa-deb3-c7cd-07eb-b39fcbe829d8", kLightSpot },
    { "3769daef-26a1-5435-a6a2-2041f97521c8", kLightTube },     // PbrTubeLightEntityData
    { "543526a1-a2a6-4120-f975-21c84b940f78", kLightTube },
    { "f9310135-b610-00d8-6a82-067a0e8e36f2", kLightRect },     // PbrRectangularLightEntityData
    { "00d8b610-826a-7a06-0e8e-36f289697545", kLightRect },
    // Declared, and carrying no light fields at all in the shipped schema.
    // Registered so a level that places one is COUNTED rather than silently
    // absent; none was observed on any map measured.
    { "02addd9b-6abc-a282-5950-a6f9bc3f87d9", kLightOther },    // LocalLightEntityData
    { "a2826abc-5059-f9a6-bc3f-87d98e7e8131", kLightOther },
    { "173542d2-6bcc-b1b7-f7dc-9a9f9bc17467", kLightOther },    // PbrAnalyticLightEntityData
    { "b1b76bcc-dcf7-9f9a-9bc1-746783feaff0", kLightOther },
    { "9785f711-e676-ea9a-9fd8-00146ed5e43c", kLightOther },    // PointLightEntityData
    { "f2841e1a-79ec-eae8-c5d8-46998d444f0d", kLightOther },    // SpotLightEntityData
};

// Subworld paths that are not part of a playable map. Same list as walk.cpp:
// dev and marketing subworlds, plus the frontend flow scenes, which a level
// really does reference and which park menu furniture below the map.
const char* kSkipSubworld[] = { "_autotests", "_tools", "marketing", "glacierflow/" };

// Destruction state branches. Walking through one puts everything below it in
// destruction state, so a light found down there is lighting the wreck rather
// than the intact map. Same list as walk.cpp.
const char* kDestructionBranch[] = {
    "464ab605-1fb8-3d21-202f-f3feb43dffb9", "591bcc82-5380-7cbd-543e-faabbc873c09",
    "ef0c40ad-226b-f6fe-04dd-7b26c26350a2", "96e64a4c-c7e1-644c-dedb-499e51d965c2",
    "570f0b4f-fcd0-d977-580e-d409f464c6f1", "e788769d-af38-c1e6-d413-10cffb99e5f8",
    "643b508d-7f3e-af14-3022-033b9ced8d79", "b3120863-e2a0-5eb4-4523-a3f4dc814e20",
    "a0e4f0a2-5007-928a-20de-1f613062529c", "0f86bea6-d792-51ab-8d91-b724ea871296",
};

constexpr int kMaxDepth = 24;

std::string lower(std::string s)
{
    for (char& c : s) {
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

std::string leaf_of(const std::string& s)
{
    const size_t i = s.find_last_of('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}

bool ends_with(const std::string& s, const std::string& t)
{
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

std::string strip_ebx(std::string n)
{
    if (ends_with(lower(n), ".ebx")) n.resize(n.size() - 4);
    return n;
}

// The inverse of TypeDb::guid_str: the first three groups are little-endian, so
// a guid written the way this file writes them can be compared against raw type
// bytes without formatting a string per instance.
TypeGuid raw_guid(const std::string& dashed)
{
    TypeGuid g{};
    std::string h;
    for (char c : dashed) if (c != '-') h += c;
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

const TypeGuid& lt_guid()
{
    static const TypeGuid g = raw_guid(kLtGuid);
    return g;
}

bool is_lt(const EbxValue* v)
{
    return v && v->kind == EbxValue::Kind::Struct && v->guid == lt_guid();
}

Vec3 vec_of(const EbxValue* d)
{
    Vec3 out;
    if (!d || d->kind != EbxValue::Kind::Struct) return out;
    auto get = [&](uint32_t h) -> float {
        const EbxValue* f = d->field(h);
        if (!f) return 0.f;
        if (f->kind == EbxValue::Kind::Real) return (float)f->f;
        if (f->kind == EbxValue::Kind::Int)  return (float)f->i;
        if (f->kind == EbxValue::Kind::Uint) return (float)f->u;
        return 0.f;
    };
    out.x = get(K_VEC_X);
    out.y = get(K_VEC_Y);
    out.z = get(K_VEC_Z);
    return out;
}

Mat34 lt_to_mat(const EbxValue* t)
{
    static const uint32_t members[4] = { F_LT_RIGHT, F_LT_UP, F_LT_FORWARD, F_LT_TRANS };
    Mat34 out = mat_identity();
    if (!t || t->kind != EbxValue::Kind::Struct) return out;
    Mat34 built;
    for (int i = 0; i < 4; i++) {
        const EbxValue* m = t->field(members[i]);
        if (!m) return out;              // an incomplete transform is identity
        built.m[i] = vec_of(m);
    }
    return built;
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

// An asset reference field, resolved to a partition name. The pointer is an
// ImportRef for a cross-partition asset, which is what every IES profile and
// area-light texture is; an internal one carries no name and is reported as
// present-but-unnamed rather than dropped.
std::string ref_name(const EbxValue* v)
{
    if (!v) return std::string();
    if (v->kind != EbxValue::Kind::ImportRef) return std::string();
    if (v->import_path.empty() || v->import_path == "<not indexed>") return std::string();
    return strip_ebx(v->import_path);
}

// The transform a component carries, plus what else it says about its light.
struct CompPlacement {
    Mat34       xf = mat_identity();
    bool        has_xf = false;
    bool        excluded = false;
    std::string ies;
};

class LightWalk {
public:
    LightWalk(Source& src, TypeDb& types, bool bounded_frontend)
        : src_(src), types_(types), bounded_frontend_(bounded_frontend) {}

    bool run(const std::string& level_rel, std::string& err);

    std::vector<LevelLight> lights;
    LightStats st;

private:
    void walk_ref(const std::string& ref, const Mat34& parent,
                  std::set<std::string> guard, int depth);
    void visit(const EbxValue& inst, const Mat34& parent, const std::string& ref,
               const std::set<std::string>& guard, int depth);
    void collect(Ebx& e, size_t idx, const Mat34& parent, const std::string& ref, int kind);
    bool type_matters(Ebx& e, size_t i);
    int  light_kind(const TypeGuid& g) const;
    std::string resolve_name(const std::string& n);
    void build_catalog();

    Source& src_;
    TypeDb& types_;
    std::unordered_map<std::string, std::string> by_name_;
    const std::unordered_map<std::string, std::string>* names_ = &by_name_;
    // Borrow Source's generation-stable partition index. The light walker is
    // short-lived, but duplicating the complete map dominated exact rig reads.
    const std::map<std::string, std::string>*    gi_ = nullptr;
    std::map<TypeGuid, int>                      kinds_;
    std::map<TypeGuid, bool>                     matters_;
    std::map<int32_t, CompPlacement>             comp_;   // per partition
    TypeGuid                                     comp_guid_{};
    std::set<std::string>                        destruction_;
    bool                                         bounded_frontend_ = false;
    bool                                         name_catalog_built_ = false;
};

void LightWalk::build_catalog()
{
    by_name_.clear();
    names_ = &by_name_;
    // Asset-light callers supply a full front-end path. Resolve it and all
    // full BundleName references against Source's existing EBX table instead
    // of allocating a duplicate all-name/leaf-name catalogue.
    name_catalog_built_ = false;
    gi_ = bounded_frontend_ ? &src_.armory_partition_index()
                            : &src_.partition_index();
    for (const KindEntry& k : kLightTypes) kinds_[raw_guid(k.guid)] = k.kind;
    comp_guid_ = raw_guid(kCompGuid);
    for (const char* d : kDestructionBranch) destruction_.insert(d);
}

std::string LightWalk::resolve_name(const std::string& name)
{
    if (name.empty()) return std::string();
    std::string n = lower(name);
    if (ends_with(n, ".ebx")) n.resize(n.size() - 4);
    const auto exact = src_.ebx().find(n);
    if (exact != src_.ebx().end()) return exact->first + ".ebx";
    // Placed fixtures normally supply exact, fully qualified names. Building
    // an all-partition alias table for each one costs far more than the light
    // traversal, including the common case of a prop with no lights. Preserve
    // the legacy alias fallback, but construct it only if an exact lookup fails.
    if (!bounded_frontend_ && !name_catalog_built_) {
        name_catalog_built_ = true;
        names_ = &src_.light_name_index();
    }
    auto it = names_->find(n);
    if (it != names_->end()) return it->second;
    it = names_->find(leaf_of(n));
    if (it != names_->end()) return it->second;
    it = names_->find(n + ".ebx");
    if (it != names_->end()) return it->second;
    return std::string();
}

int LightWalk::light_kind(const TypeGuid& g) const
{
    auto it = kinds_.find(g);
    return it == kinds_.end() ? -1 : it->second;
}

bool LightWalk::type_matters(Ebx& e, size_t i)
{
    const TypeGuid tb = e.instance_type(i);
    bool all_zero = true;
    for (uint8_t b : tb) if (b) { all_zero = false; break; }
    if (all_zero) return false;

    auto it = matters_.find(tb);
    if (it != matters_.end()) return it->second;

    const TypeLayout& lay = types_.layout_full(tb);
    bool yes = false;
    if (!lay.valid) {
        // AN UNKNOWN TYPE IS NOT A BORING ONE. The skip is sound only as an
        // equivalence - a type that DECLARES none of the descent fields cannot
        // affect the descent - and an empty layout is not that statement, it is
        // "the type database could not describe this type". Reading that as
        // "no" makes every instance skippable, the walk descends into nothing,
        // and the level comes back with no lights and no error.
        st.unresolved_types++;
        yes = true;
    } else {
        bool smg = false;
        for (const FieldInfo& f : lay.fields) {
            if (f.name_hash == F_SMG_MEMBERS) { smg = true; break; }
            for (uint32_t w : kDescendFields)
                if (f.name_hash == w) { yes = true; break; }
        }
        // A StaticModelGroup: walk.cpp emits its members and returns WITHOUT
        // descending, so for lights it is a dead end and the cheapest correct
        // thing is not to decode it at all.
        if (smg) yes = false;
    }
    matters_[tb] = yes;
    return yes;
}

void LightWalk::collect(Ebx& e, size_t idx, const Mat34& parent,
                        const std::string& ref, int kind)
{
    st.by_kind[(size_t)(kind < 0 || kind > kLightOther ? kLightOther : kind)]++;

    static const std::vector<uint32_t> want(std::begin(kLightFields),
                                            std::end(kLightFields));
    const EbxValue d = e.read_instance(idx, &want);
    if (d.kind != EbxValue::Kind::Struct) return;

    LevelLight L;
    L.kind = kind;
    L.source = strip_ebx(ref);
    L.instance = (int)idx;

    // THE COMPONENT WINS when there is one. In the authoring model the entity's
    // own Transform is identity, so this is not a preference between two
    // answers but the only non-empty one; the entity's Transform is the answer
    // for lights placed as ordinary objects rather than inside a fixture
    // blueprint. Which one fired is counted so the split stays a measurement.
    auto ci = comp_.find((int32_t)idx);
    if (ci != comp_.end() && ci->second.has_xf) {
        if (ci->second.excluded) { st.comp_excluded++; return; }
        L.xf = mat_mul(parent, ci->second.xf);
        L.from_component = 1;
        L.ies_profile = ci->second.ies;
        st.placed_by_component++;
    } else {
        if (ci != comp_.end() && ci->second.excluded) { st.comp_excluded++; return; }
        const EbxValue* own = d.field(F_TRANSFORM);
        if (is_lt(own)) {
            const Mat34 m = lt_to_mat(own);
            L.xf = mat_mul(parent, m);
            const bool zero = m.m[3].x == 0.f && m.m[3].y == 0.f && m.m[3].z == 0.f;
            if (zero) st.placed_at_holder++;
            else st.placed_by_own_transform++;
        } else {
            L.xf = parent;
            st.placed_at_holder++;
        }
        if (ci != comp_.end()) L.ies_profile = ci->second.ies;
    }

    if (const EbxValue* c = d.field(F_COLOR)) {
        if (c->kind == EbxValue::Kind::Struct) {
            const Vec3 v = vec_of(c);
            L.color[0] = v.x; L.color[1] = v.y; L.color[2] = v.z;
        }
    }
    L.intensity  = as_f(d.field(F_INTENSITY));
    L.unit       = as_i(d.field(F_LIGHT_UNIT));
    L.dimmer     = as_f(d.field(F_DIMMER), 1.f);
    L.attenuation_radius = as_f(d.field(F_ATTEN_RAD));
    L.attenuation_offset = as_f(d.field(F_ATTEN_OFF));
    L.inner_angle = as_f(d.field(F_INNER_ANGLE));
    L.outer_angle = as_f(d.field(F_OUTER_ANGLE));
    // One slot for three differently-named emitter radii, because they are the
    // same quantity: the physical size of the emitting shape, which is what
    // softens a shadow. Only one of them exists on any given class.
    if (kind == kLightSphere)    L.shape_radius = as_f(d.field(F_SPHERE_RAD));
    else if (kind == kLightSpot) L.shape_radius = as_f(d.field(F_DISC_RAD));
    else if (kind == kLightTube) L.shape_radius = as_f(d.field(F_TUBE_RAD));
    L.tube_width  = as_f(d.field(F_TUBE_WIDTH));
    L.is_capsule  = as_i(d.field(F_IS_CAPSULE));
    L.rect_height = as_f(d.field(F_RECT_HEIGHT));
    L.rect_aspect = as_f(d.field(F_RECT_ASPECT));
    L.rect_shape  = as_i(d.field(F_RECT_SHAPE));
    L.cast_shadows_enable = as_i(d.field(F_CAST_SHAD_E));
    L.cast_shadows        = as_i(d.field(F_CAST_SHAD));
    L.cast_volumetric     = as_i(d.field(F_CAST_VOL));
    L.volumetric_scattering = as_f(d.field(F_VOL_SCATTER));
    L.affect_diffuse   = as_i(d.field(F_AFF_DIFFUSE));
    L.affect_specular  = as_i(d.field(F_AFF_SPEC));
    L.affect_radiosity = as_i(d.field(F_AFF_RADIO));
    L.emissive_shape_enable = as_i(d.field(F_EMISSIVE_SH));
    L.ies_multiplier = as_f(d.field(F_IES_MULT));
    L.ies_as_mask    = as_i(d.field(F_IES_MASK));
    L.cull_distance  = as_f(d.field(F_CULL_DIST));
    L.fade_distance  = as_f(d.field(F_FADE_DIST));
    L.flags          = (uint32_t)as_i(d.field(F_FLAGS));
    // The entity's own profile overrides whatever its component carried: the
    // component's IESProfile is the fixture's default and the entity's is the
    // authored one.
    {
        const std::string own_ies = ref_name(d.field(F_IES_PROFILE));
        if (!own_ies.empty()) L.ies_profile = own_ies;
    }
    L.texture = ref_name(d.field(F_TEXTURE));

    lights.push_back(std::move(L));
}

void LightWalk::visit(const EbxValue& inst, const Mat34& parent, const std::string& ref,
                      const std::set<std::string>& guard, int depth)
{
    if (destruction_.count(TypeDb::guid_str(inst.guid))) return;

    const EbxValue* local = inst.field(F_BP_TRANSFORM);
    const Mat34 world = is_lt(local) ? mat_mul(parent, lt_to_mat(local)) : parent;

    if (const EbxValue* ex = inst.field(F_EXCLUDED))
        if ((ex->kind == EbxValue::Kind::Bool && ex->b) ||
            (ex->kind == EbxValue::Kind::Uint && ex->u) ||
            (ex->kind == EbxValue::Kind::Int && ex->i))
            return;

    if (const EbxValue* bn = inst.field(F_BUNDLENAME))
        if (bn->kind == EbxValue::Kind::Str && !bn->s.empty()) {
            const std::string low = lower(bn->s);
            for (const char* s : kSkipSubworld)
                if (low.find(s) != std::string::npos) return;
            const std::string sub = resolve_name(bn->s);
            if (!sub.empty()) walk_ref(sub, world, guard, depth + 1);
            return;
        }

    if (const EbxValue* bp = inst.field(F_BLUEPRINT)) {
        const std::string tgt = ref_name(bp);
        if (!tgt.empty()) {
            // No leaf-row fallback here. walk.cpp emits one so a blueprint that
            // produced no geometry is still PLACED; a light either exists down
            // there or it does not, and inventing a row for it would invent a
            // light.
            walk_ref(bp->import_path, world, guard, depth + 1);
            return;
        }
    }

    for (uint32_t f : { F_OBJECTS, F_DATAREFS, F_COMPONENTS })
        if (const EbxValue* arr = inst.field(f))
            if (arr->kind == EbxValue::Kind::Array)
                for (const EbxValue& child : arr->items)
                    if (child.kind == EbxValue::Kind::Struct)
                        visit(child, world, ref, guard, depth);
}

void LightWalk::walk_ref(const std::string& ref, const Mat34& parent,
                         std::set<std::string> guard, int depth)
{
    if (depth > kMaxDepth || ref.empty()) return;
    const std::string key = lower(ref);
    if (guard.count(key)) { st.cycles++; return; }

    std::string name = ref;
    if (ends_with(lower(name), ".ebx")) name.resize(name.size() - 4);
    std::string err;
    std::vector<uint8_t> data = src_.get_ebx(name, err);
    if (data.empty()) { st.missing++; return; }

    Ebx e(types_);
    e.set_guid_index(gi_);
    if (!e.parse(std::move(data), err)) { st.parse_fail++; return; }

    guard.insert(key);
    st.partitions++;

    // THE PLACEMENT COMPONENTS FIRST, before the instance loop, because a
    // component sits AFTER the light it places - in lf_com_streetlight_02 the
    // light is instance 10 and its component is 29 - so by the time the light
    // is reached there would be nothing to look up.
    //
    // Costs one guid compare per instance and a four-field decode for the
    // handful that match.
    std::map<int32_t, CompPlacement> prev;
    prev.swap(comp_);
    {
        static const std::vector<uint32_t> want(std::begin(kCompFields),
                                                std::end(kCompFields));
        for (size_t i = 0; i < e.instance_count(); i++) {
            if (e.instance_type(i) != comp_guid_) continue;
            st.components++;
            const EbxValue d = e.read_instance(i, &want);
            if (d.kind != EbxValue::Kind::Struct) { st.comp_unlinked++; continue; }
            const EbxValue* p = d.field(F_COMP_LIGHT);
            if (!p || p->kind != EbxValue::Kind::InstanceRef || p->instance < 0) {
                st.comp_unlinked++;
                continue;
            }
            CompPlacement cp;
            const EbxValue* t = d.field(F_TRANSFORM);
            if (is_lt(t)) { cp.xf = lt_to_mat(t); cp.has_xf = true; }
            const EbxValue* ex = d.field(F_EXCLUDED);
            cp.excluded = ex && ex->kind == EbxValue::Kind::Bool && ex->b;
            cp.ies = ref_name(d.field(F_IES_PROFILE));
            comp_[p->instance] = cp;

            // THE OLD JOIN, MEASURED RATHER THAN ASSUMED WRONG. 0x11F57ECA is
            // what the GDScript reader follows as "an int field holding a
            // pointer". Under the MP schema it is an enum four bytes wide at a
            // different offset, so following it should resolve to nothing or to
            // something else; this counts how often it agrees so the claim in
            // the header is a number rather than an argument.
            const int32_t legacy = e.int_pointer(i, F_COMP_LEGACY);
            if (legacy >= 0) {
                st.comp_legacy_resolved++;
                if (legacy == p->instance) st.comp_legacy_agree++;
            }
        }
    }

    static const std::vector<uint32_t> descend(std::begin(kDescendFields),
                                               std::end(kDescendFields));
    for (size_t i = 0; i < e.instance_count(); i++) {
        st.instances++;
        // A LIGHT DECLARES NONE OF THE DESCENT FIELDS, so the "can this
        // instance matter" test below would correctly say no and would drop
        // every light on the map while the traversal stayed perfect. Wanted
        // types are handled first, exactly as they must be.
        const int kind = light_kind(e.instance_type(i));
        if (kind >= 0) {
            st.decoded++;
            collect(e, i, parent, ref, kind);
            continue;
        }
        if (!type_matters(e, i)) continue;
        st.decoded++;
        const EbxValue inst = e.read_instance(i, &descend);
        if (inst.kind != EbxValue::Kind::Struct) continue;
        visit(inst, parent, ref, guard, depth);
    }

    comp_.swap(prev);
}

bool LightWalk::run(const std::string& level_rel, std::string& err)
{
    err.clear();
    lights.clear();
    st = LightStats();
    build_catalog();

    std::string rel = lower(level_rel);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    const std::string leaf = leaf_of(rel);

    // Prefer the exact level root, then the exact asset. Calling the alias
    // resolver on the speculative doubled asset path rebuilt the whole name
    // catalogue before we even tried the fully qualified prefab we were given.
    std::string start;
    const auto root = src_.ebx().find(rel + "/" + leaf);
    const auto asset = src_.ebx().find(strip_ebx(rel));
    if (root != src_.ebx().end()) start = root->first + ".ebx";
    else if (asset != src_.ebx().end()) start = asset->first + ".ebx";
    else start = resolve_name(rel + "/" + leaf);
    if (start.empty()) start = resolve_name(rel);
    if (start.empty()) start = resolve_name(leaf);
    if (start.empty() && rel.find('/') == std::string::npos) {
        // A BARE LEVEL NAME. Anchored on '/levels/<leaf>/<leaf>' rather than a
        // substring: a loose match happily picks a neighbouring level whose
        // name merely CONTAINS this one.
        std::vector<std::string> tails{ "/levels/" + leaf + "/" + leaf };
        if (leaf.rfind("mp_", 0) != 0)
            tails.push_back("/levels/mp_" + leaf + "/mp_" + leaf);
        for (const std::string& tail : tails) {
            std::vector<std::string> hits;
            for (const auto& kv : *names_)
                if (ends_with(kv.first, tail)) hits.push_back(kv.first);
            if (hits.empty()) continue;
            std::sort(hits.begin(), hits.end());
            start = names_->at(hits[0]);
            break;
        }
    }
    if (start.empty()) {
        err = "could not resolve the level root for " + level_rel;
        return false;
    }
    walk_ref(start, mat_identity(), {}, 0);
    return true;
}

}  // namespace

bool level_lights(Source& src, TypeDb& types, const std::string& level,
                  std::vector<LevelLight>& out, LightStats& st, std::string& err,
                  bool bounded_frontend)
{
    LightWalk w(src, types, bounded_frontend);
    if (!w.run(level, err)) return false;
    out = std::move(w.lights);
    st = w.st;
    return true;
}

}  // namespace bf6
