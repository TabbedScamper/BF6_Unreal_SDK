#include "velighting.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "ebx.h"

namespace bf6 {
namespace {

// ---- the component type guids, by their first eight hex digits --------------
//
// Eight digits, not sixteen: the prefix is unique across the 27 components a VE
// carries and it keeps the table below readable. The full guid is compared for
// nothing else here, so there is no scope in which a prefix could collide with
// something it must not match.
const char* kSun    = "f8b3f61a";   // OutdoorLight: sun angle, colour, lux, cloud shadows
const char* kSky    = "5fb52ff8";   // Sky: panorama, atmosphere, cloud layers
const char* kFog    = "8ab9baf8";   // Fog: depth fog, height fog, participating media
const char* kExpo   = "5b6bbccc";   // Exposure + bloom + lens dirt
const char* kGrade  = "ef4eec57";   // Colour grading
const char* kWhite  = "eeeab5a6";   // White balance
const char* kAO     = "2f9e79a0";   // Ambient occlusion (SSAO / HBAO / RTAO / AAO)
const char* kGI     = "c4ea62ae";   // Global illumination (Enlighten sky box)
const char* kShadow = "1c6df12b";   // Sun shadowmap cascades

// ---- vector member hashes ---------------------------------------------------
// A Vec2/Vec3/Vec4 is an inline struct whose members are themselves hashed.
const uint32_t kVx = 0x3901DB14, kVy = 0x42FC0F5E, kVz = 0x32A99B9C;
// A field that varies BY QUALITY LEVEL is a four-member struct instead, one per
// preset. Element 0 is the highest, which is the one worth reporting.
const uint32_t kQ0 = 0xD0A94456;

// ---- the fields, keyed by (component, hash) ---------------------------------
// SunRotationX exists twice on the sun component (8748d69f = 124.8 on Dumbo,
// be17684d = 0.0) and SunScale exists on two different components with values
// 1.5 and 300000. Nothing in this table may be looked up by name.
const uint32_t kSunRotX      = 0x8748D69F;   // NOT be17684d
const uint32_t kSunRotY      = 0x04FF06A2;   // NOT fcba05b8
const uint32_t kSunColor     = 0x7215CE32;
const uint32_t kSunIntensity = 0x130859CD;
const uint32_t kSunAngRadius = 0xCC658DE2;
const uint32_t kSunSpecScale = 0x6E07F509;
const uint32_t kCsSize       = 0x5F1906FC;
const uint32_t kCsCoverage   = 0xC09678FB;
const uint32_t kCsExponent   = 0xB453AD6A;
const uint32_t kCsSpeed      = 0x7209795B;
const uint32_t kCsTranslate  = 0x98E1C6A5;
const uint32_t kCsRadiosity  = 0x79764BCD;
const uint32_t kCsTexture    = 0x3EF85CF5;
const uint32_t kCs2Texture   = 0x7C9018D2;
const uint32_t kCs2Speed     = 0x56DC1C70;
const uint32_t kCs2Size      = 0x514C1243;
const uint32_t kCs2Coverage  = 0x3578E33D;
const uint32_t kCs2Exponent  = 0x3D209359;
const uint32_t kCs2Translate = 0xF0A5F091;
const uint32_t kCsAddress    = 0x569EADF9;
const uint32_t kCs2Address   = 0xF064FB16;
const uint32_t kCsTopDown    = 0x487133AF;
const uint32_t kCs2TopDown   = 0x7EE5650B;
const uint32_t kCsStartFade  = 0xED9AF476;
const uint32_t kCsFadeDist   = 0x1A72CA69;
const uint32_t kCsHeightEn   = 0xD31DE602;
const uint32_t kCsHeightFrom = 0x3D448389;
const uint32_t kCsHeightDist = 0x334F8124;

const uint32_t kSkyType      = 0xFF6D65E7;
const uint32_t kSkyLumScale  = 0x5EBAF2B1;
const uint32_t kPanoRotation = 0x89F2223A;
const uint32_t kPanoTile     = 0x54FCDDF6;
const uint32_t kPanoUvMinX   = 0xA024201A;
const uint32_t kPanoUvMaxX   = 0x6EAAB0C6;
const uint32_t kPanoUvMinY   = 0x9482E7A9;
const uint32_t kPanoUvMaxY   = 0x7827E1A9;
const uint32_t kFlowDistance = 0x8507484E;
const uint32_t kFlowDirection= 0x08D30301;
const uint32_t kFlowPeriod   = 0x60158673;
const uint32_t kFlowHeightScl= 0xFF89FAA5;
const uint32_t kFlowHeightBias=0x3F08367A;
const uint32_t kDrawSunDisc  = 0x029A4B53;
const uint32_t kSunDiscSize  = 0xEF6748FB;
const uint32_t kSunDiscScale = 0x0A654B9F;   // the SKY's SunScale, 300000
const uint32_t kRayleigh     = 0xEA9412CC;   // a VEC3, not the scalar the name implies
const uint32_t kRayleighScl  = 0x25ADDC21;
const uint32_t kMieCoef      = 0x0CB78199;
const uint32_t kMieG         = 0xFA63730B;
const uint32_t kUseAerial    = 0x39007A54;
const uint32_t kAerialScale  = 0x406A871B;
const uint32_t kAerialInten  = 0x9C856374;
const uint32_t kEarthRadius  = 0x5FB77DFB;
const uint32_t kAtmoRadius   = 0x11D6C34E;
const uint32_t kHfColorAdd   = 0xAA716BC1;
const uint32_t kCloud1Alt    = 0xC5D1B01C;
const uint32_t kCloud1Tile   = 0x3648F7DA;
const uint32_t kCloud1Rot    = 0xE5F12D9F;
const uint32_t kCloud1Speed  = 0xAF8AB15A;
const uint32_t kCloud1Alpha  = 0x93643C95;
const uint32_t kCloud1Color  = 0x3C6D3426;
const uint32_t kPanoTexture  = 0xC4184CD8;
const uint32_t kPanoAlphaTex = 0x45154E7C;
const uint32_t kSkyGradTex   = 0x3A63DCCD;
const uint32_t kFlowMaskTex  = 0x78C6F106;
const uint32_t kCloud1Tex    = 0x88624A3C;

const uint32_t kFogHeightEn  = 0x93D3A211;
const uint32_t kFogColorEn   = 0xE341E90D;
const uint32_t kFogGradEn    = 0xF9763CB8;
const uint32_t kFogColor     = 0x0501B552;   // HDR radiance
const uint32_t kFogStart     = 0x682D7A3F;
const uint32_t kFogEnd       = 0x4D8196A9;
const uint32_t kFogColStart  = 0x962333D7;
const uint32_t kFogColEnd    = 0x27072D58;
const uint32_t kHfStart      = 0xA9A080E3;
const uint32_t kHfEnd        = 0x1CD62D9F;
const uint32_t kHfAltitude   = 0x8CC2F99E;
const uint32_t kHfDepth      = 0x0D3FCE6F;
const uint32_t kHfVisRange   = 0xEF283ED3;
const uint32_t kPmEnable     = 0x00CBC378;
const uint32_t kSunScatter   = 0xCA5E0FED;
const uint32_t kLocalScatter = 0x23A02A07;

const uint32_t kEv           = 0xE1877E34;
const uint32_t kEvMax        = 0xA8AE270A;
const uint32_t kExpComp      = 0xB308393B;
const uint32_t kAutoExposure = 0xF56BC9AC;
const uint32_t kBloomScale   = 0x4EC18B88;
const uint32_t kBloomMethod  = 0xCD5B13B6;
const uint32_t kLensDirtTex  = 0xC6AE592C;

const uint32_t kGradingEn    = 0x565A3EB0;
const uint32_t kGradeBright  = 0xB3B14547;
const uint32_t kGradeContr   = 0x5FF33110;
const uint32_t kGradeSat     = 0x3C6395F8;
const uint32_t kGradeHue     = 0x3BF14B5A;
const uint32_t kGradeLut     = 0xF2B5C787;

const uint32_t kWbTemp       = 0xDF6C5DBF;
const uint32_t kWbTint       = 0xC155FC7D;

const uint32_t kAffectOutdoor = 0x816CF0B4;
const uint32_t kAffectLocal   = 0x6DD97D12;
const uint32_t kSsaoInner     = 0xB61CBCFB;
const uint32_t kSsaoOuter     = 0x3811AD09;
const uint32_t kHbaoRadius    = 0x34444CFD;
const uint32_t kHbaoContrast  = 0xB2994B82;
const uint32_t kDynamicAO     = 0x86BC17F5;

const uint32_t kGiTerrainCol  = 0x3C77E4C8;
const uint32_t kGiSkyCol      = 0xF97F2EC3;
const uint32_t kGiGroundCol   = 0x045FF8C7;
const uint32_t kGiSunCol      = 0xBFF74A5B;
const uint32_t kGiBackRotX    = 0x10F271F6;
const uint32_t kGiBackRotY    = 0x463D89BF;
const uint32_t kGiBounce      = 0x744AFFE7;
const uint32_t kGiSunScale    = 0x0A654B9F;  // the GI's SunScale, 1.5

const uint32_t kSunShadowDist = 0x217E6BAB;

// The VE ENTITY itself, above the components: a6eee1ae carries the preset's own
// Name and 5d092d08 carries the blend weight the game applies it at.
const char* kVeRoot     = "a6eee1ae";
const char* kVeEntity   = "5d092d08";
const uint32_t kVeName       = 0x0C59FA06;
const uint32_t kVeVisibility = 0x66BCDFFB;
const uint32_t kVeCompCount  = 0x825A1C00;

// One VE, flattened: (component prefix, field hash) -> value, plus the instance
// index each component was found at so the import-pointer fields can be read.
struct Flat {
    std::map<std::pair<std::string, uint32_t>, EbxValue> f;
    std::map<std::string, size_t> inst;

    const EbxValue* get(const char* comp, uint32_t h) const
    {
        auto it = f.find(std::make_pair(std::string(comp), h));
        return it == f.end() ? nullptr : &it->second;
    }
};

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string leaf_of(const std::string& n)
{
    const size_t s = n.find_last_of('/');
    return s == std::string::npos ? n : n.substr(s + 1);
}

std::string strip_ebx(std::string n)
{
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ebx") == 0) n.resize(n.size() - 4);
    return n;
}

// The mount spells a level root "<something>/levels/<level>/<level>", with no
// extension. Matched on the tail rather than the whole name because the prefix
// differs per game package (glaciermp, kingstonlegacy, ...).
std::string find_level_root(Source& src, const std::string& level)
{
    const std::string want = lower(level + "/" + level);
    for (const auto& kv : src.ebx()) {
        const std::string& n = kv.first;
        const size_t p = n.find("/levels/");
        if (p == std::string::npos) continue;
        if (lower(n.substr(p + 8)) == want) return n;
    }
    return std::string();
}

void read_vec(const EbxValue* v, float* out, int n)
{
    if (!v || v->kind != EbxValue::Kind::Struct) return;
    const uint32_t m[3] = { kVx, kVy, kVz };
    for (int i = 0; i < n && i < 3; i++)
        if (const EbxValue* c = v->field(m[i]))
            if (c->kind == EbxValue::Kind::Real) out[i] = (float)c->f;
}

float as_f(const EbxValue* v, bool* found = nullptr)
{
    if (!v) return 0.f;
    if (v->kind == EbxValue::Kind::Real) { if (found) *found = true; return (float)v->f; }
    // A quality-scaled field is a four-member struct, one value per preset;
    // element 0 is the highest quality level.
    if (v->kind == EbxValue::Kind::Struct)
        if (const EbxValue* q = v->field(kQ0))
            if (q->kind == EbxValue::Kind::Real) { if (found) *found = true; return (float)q->f; }
    return 0.f;
}

int as_i(const EbxValue* v, bool* found = nullptr)
{
    if (!v) return 0;
    switch (v->kind) {
    case EbxValue::Kind::Bool: if (found) *found = true; return v->b ? 1 : 0;
    case EbxValue::Kind::Int:  if (found) *found = true; return (int)v->i;
    case EbxValue::Kind::Uint: if (found) *found = true; return (int)v->u;
    default: return 0;
    }
}

}  // namespace

namespace {

// What a candidate preset IS, read from the preset itself rather than guessed
// from its name.
struct Cand {
    std::string name;
    bool  has_sun = false;   // does it carry the OutdoorLight component at all
    float visibility = 0.f;  // the blend weight the game applies it at
};

bool inspect(Source& src, TypeDb& types, const std::string& part, Cand& c)
{
    std::string err;
    std::vector<uint8_t> raw = src.get_ebx(part, err);
    if (raw.empty()) return false;
    Ebx e(types);
    if (!e.parse(std::move(raw), err)) return false;
    c.name = part;
    for (size_t i = 0; i < e.instance_count(); i++) {
        const std::string tg = TypeDb::guid_str(e.instance_type(i)).substr(0, 8);
        if (tg == kSun) { c.has_sun = true; continue; }
        if (tg != kVeEntity) continue;
        const std::vector<uint32_t> want = { kVeVisibility };
        EbxValue d = e.read_instance(i, &want);
        if (const EbxValue* v = d.field(kVeVisibility))
            if (v->kind == EbxValue::Kind::Real) c.visibility = (float)v->f;
    }
    return true;
}

}  // namespace

std::string ve_active_preset(Source& src, TypeDb& types, const std::string& level,
                             int* candidates, std::string& err)
{
    if (candidates) *candidates = 0;
    const std::string root = find_level_root(src, level);
    if (root.empty()) { err = "no level root partition for " + level; return std::string(); }

    std::vector<uint8_t> raw = src.get_ebx(root, err);
    if (raw.empty()) { err = "level root unreadable: " + root; return std::string(); }
    Ebx e(types);
    e.set_guid_index(&src.partition_index());
    if (!e.parse(std::move(raw), err)) return std::string();

    const std::map<std::string, std::string>& gi = src.partition_index();
    std::vector<std::string> hits;
    for (size_t i = 0; i < e.import_count(); i++) {
        // The PARTITION half. The instance half matches on ~12% of records and
        // silently returns nothing for the rest.
        auto it = gi.find(e.import_at(i).partition);
        if (it == gi.end()) continue;
        const std::string n = strip_ebx(it->second);
        const std::string ln = lower(n);
        // UNDER A LEVEL, not under common/. A root also imports the shared
        // ve_global_base, ve_global_performance, ve_renderlayer_* and a
        // lens-flare preset, which are engine-wide and carry no map identity.
        //
        // NOT "under THIS level" - that was the first rule here and it is wrong
        // for every Portal variant: mp_granite_clubhouse_portal is lit by
        // mp_granite's preset, from another level's directory entirely, and
        // mp_aftermath_portal by mp_aftermath's. Six levels decoded nothing at
        // all under the narrower rule.
        if (ln.find("/levels/") == std::string::npos) continue;
        const std::string lf = lower(leaf_of(n));
        if (lf.compare(0, 3, "ve_") != 0) continue;
        // thermal is an optics mode, not the environment.
        if (lf.find("thermal") != std::string::npos) continue;
        hits.push_back(n);
    }
    std::sort(hits.begin(), hits.end());
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    if (hits.empty()) {
        err = "level root imports no outdoor ve_ preset";
        return std::string();
    }

    // MORE THAN ONE SURVIVES ON SOME LEVELS, and the tie is broken from the
    // data. mp_granite imports three: midday_base_06, thermal and `nolut`.
    // Dropping thermal leaves two, and picking alphabetically would be luck.
    //
    // What separates them is what they ARE. The active preset carries 25
    // components including the sun; `nolut` carries 2, has no OutdoorLight at
    // all, and its VE entity's Visibility is 0.15 against the active one's 1.0
    // - it is a thin blend layered on top, not the environment. So: a
    // candidate must actually carry a sun, and among those the one the game
    // blends in hardest wins.
    //
    // Visibility on its own is not enough: the shared common/ presets are 1.0
    // too, which is why they are excluded by path first.
    //
    // ONE LEVEL STILL ENDS AMBIGUOUS and it is left that way rather than
    // decided by a rule invented for it. mp_contaminated imports Base_NEW (15
    // components) and Base_NEW2 (17) as well as Residue (1, no sun); the sun
    // filter drops Residue and the other two are both Visibility 1.0 and author
    // the SAME sun - 14.17 / 45 / 125000 either way. `preset_candidates` comes
    // back 2 so a caller can see it. If a level ever appears where two
    // sun-carrying presets disagree, that is the point to find a real
    // discriminator, not now.
    std::vector<Cand> cands;
    for (const std::string& n : hits) {
        Cand c;
        if (inspect(src, types, n, c)) cands.push_back(c);
    }
    if (cands.empty()) { err = "no readable ve_ preset among the level root's imports";
                         return std::string(); }

    bool any_sun = false;
    for (const Cand& c : cands) any_sun = any_sun || c.has_sun;

    const Cand* best = nullptr;
    int kept = 0;
    for (const Cand& c : cands) {
        if (any_sun && !c.has_sun) continue;   // a preset with no sun is not the environment
        kept++;
        if (!best || c.visibility > best->visibility) best = &c;
    }
    if (candidates) *candidates = kept;
    return best ? best->name : std::string();
}

bool ve_lighting_partition(Source& src, TypeDb& types, const std::string& partition,
                           VeLighting& out, std::string& err)
{
    out = VeLighting();
    std::string part = partition;
    const std::string part_lower = lower(part);
    if (part_lower.size() >= 4 &&
        part_lower.compare(part_lower.size() - 4, 4, ".ebx") == 0)
        part.resize(part.size() - 4);
    if (part.empty()) { err = "no VisualEnvironment partition"; return false; }
    out.preset_path = part;
    out.preset = leaf_of(part);
    out.preset_candidates = 1;

    std::vector<uint8_t> raw = src.get_ebx(part, err);
    if (raw.empty()) { err = "preset unreadable: " + part; return false; }
    Ebx e(types);
    e.set_guid_index(&src.partition_index());
    if (!e.parse(std::move(raw), err)) return false;

    const std::map<std::string, std::string>& gi = src.partition_index();
    for (size_t i = 0; i < e.import_count(); i++) {
        auto it = gi.find(e.import_at(i).partition);
        out.imports.push_back(it == gi.end() ? e.import_at(i).partition
                                            : strip_ebx(it->second));
    }
    std::sort(out.imports.begin(), out.imports.end());
    out.imports.erase(std::unique(out.imports.begin(), out.imports.end()), out.imports.end());

    Flat flat;
    for (size_t i = 0; i < e.instance_count(); i++) {
        const std::string tg = TypeDb::guid_str(e.instance_type(i)).substr(0, 8);
        EbxValue d = e.read_instance(i);
        // FIRST INSTANCE WINS. A preset carries one of each lighting component;
        // the repeated instances in a VE are the property-override records and
        // the cloud-layer entries, none of which share these type guids.
        if (flat.inst.count(tg)) continue;
        flat.inst[tg] = i;
        for (const auto& kv : d.fields)
            flat.f.emplace(std::make_pair(tg, kv.first), kv.second);
    }

    // The preset's own blend weight and declared component count, off the VE
    // entity above the components. Reported rather than acted on: a consumer
    // asking for "the level's lighting" wants the dominant outdoor preset, and
    // this says how dominant it is.
    if (const EbxValue* v = flat.get(kVeEntity, kVeVisibility))
        if (v->kind == EbxValue::Kind::Real) out.visibility = (float)v->f;
    if (const EbxValue* v = flat.get(kVeEntity, kVeCompCount))
        out.component_count = as_i(v);
    // Prefer the preset's own authored Name over the partition leaf where it
    // has one: it carries the studio's capitalisation.
    if (const EbxValue* v = flat.get(kVeRoot, kVeName))
        if (v->kind == EbxValue::Kind::Str && !v->s.empty()) out.preset = leaf_of(v->s);

    if (flat.inst.count(kSun))    out.components |= kVeSun;
    if (flat.inst.count(kSky))    out.components |= kVeSky;
    if (flat.inst.count(kFog))    out.components |= kVeFog;
    if (flat.inst.count(kExpo))   out.components |= kVeExposure;
    if (flat.inst.count(kGrade))  out.components |= kVeGrading;
    if (flat.inst.count(kWhite))  out.components |= kVeWhiteBalance;
    if (flat.inst.count(kAO))     out.components |= kVeAmbientOcc;
    if (flat.inst.count(kGI))     out.components |= kVeGlobalIllum;
    if (flat.inst.count(kShadow)) out.components |= kVeSunShadow;

    // Every read goes through these three so `fields_found` counts what really
    // resolved rather than what was asked for.
    auto F = [&](const char* comp, uint32_t h, float& dst) {
        out.fields_expected++;
        bool ok = false;
        const float v = as_f(flat.get(comp, h), &ok);
        if (ok) { dst = v; out.fields_found++; }
    };
    auto I = [&](const char* comp, uint32_t h, int& dst) {
        out.fields_expected++;
        bool ok = false;
        const int v = as_i(flat.get(comp, h), &ok);
        if (ok) { dst = v; out.fields_found++; }
    };
    auto V = [&](const char* comp, uint32_t h, float* dst, int n) {
        out.fields_expected++;
        const EbxValue* v = flat.get(comp, h);
        if (v && v->kind == EbxValue::Kind::Struct) { read_vec(v, dst, n); out.fields_found++; }
    };
    auto T = [&](const char* comp, uint32_t h, std::string& dst) {
        out.fields_expected++;
        auto it = flat.inst.find(comp);
        if (it == flat.inst.end()) return;
        std::string pg, path;
        if (!e.import_ref(it->second, h, pg, path)) return;
        dst = path.empty() ? pg : strip_ebx(path);
        out.fields_found++;
    };

    // ---- sun -------------------------------------------------------------
    F(kSun, kSunRotX, out.sun_rotation_x);
    F(kSun, kSunRotY, out.sun_rotation_y);
    V(kSun, kSunColor, out.sun_color, 3);
    F(kSun, kSunIntensity, out.sun_intensity);
    F(kSun, kSunAngRadius, out.sun_angular_radius);
    F(kSun, kSunSpecScale, out.sun_specular_scale);
    F(kSun, kCsSize, out.cloud_shadow_size);
    F(kSun, kCsCoverage, out.cloud_shadow_coverage);
    F(kSun, kCsExponent, out.cloud_shadow_exponent);
    V(kSun, kCsSpeed, out.cloud_shadow_speed, 2);
    V(kSun, kCsTranslate, out.cloud_shadow_translation, 2);
    I(kSun, kCsRadiosity, out.cloud_radiosity);
    T(kSun, kCsTexture, out.cloud_shadow_res);
    T(kSun, kCs2Texture, out.secondary_cloud_shadow_res);
    F(kSun, kCs2Size, out.secondary_cloud_shadow_size);
    F(kSun, kCs2Coverage, out.secondary_cloud_shadow_coverage);
    F(kSun, kCs2Exponent, out.secondary_cloud_shadow_exponent);
    V(kSun, kCs2Speed, out.secondary_cloud_shadow_speed, 2);
    V(kSun, kCs2Translate, out.secondary_cloud_shadow_translation, 2);
    I(kSun, kCsAddress, out.cloud_shadow_addressing_mode);
    I(kSun, kCs2Address, out.secondary_cloud_shadow_addressing_mode);
    I(kSun, kCsTopDown, out.cloud_shadow_is_top_down);
    I(kSun, kCs2TopDown, out.secondary_cloud_shadow_is_top_down);
    F(kSun, kCsStartFade, out.cloud_shadow_start_fade);
    F(kSun, kCsFadeDist, out.cloud_shadows_fade_distance);
    I(kSun, kCsHeightEn, out.cloud_shadow_height_fade_enable);
    F(kSun, kCsHeightFrom, out.cloud_shadow_start_height_fade);
    F(kSun, kCsHeightDist, out.cloud_shadows_height_fade_distance);

    // ---- sky -------------------------------------------------------------
    I(kSky, kSkyType, out.sky_type);
    F(kSky, kSkyLumScale, out.sky_luminance_scale);
    F(kSky, kPanoRotation, out.sky_panoramic_rotation);
    F(kSky, kPanoTile, out.sky_panoramic_tile_factor);
    F(kSky, kPanoUvMinX, out.sky_panoramic_uv_min[0]);
    F(kSky, kPanoUvMinY, out.sky_panoramic_uv_min[1]);
    F(kSky, kPanoUvMaxX, out.sky_panoramic_uv_max[0]);
    F(kSky, kPanoUvMaxY, out.sky_panoramic_uv_max[1]);
    F(kSky, kFlowDistance, out.sky_flow_distance);
    F(kSky, kFlowDirection, out.sky_flow_direction);
    F(kSky, kFlowPeriod, out.sky_flow_period);
    F(kSky, kFlowHeightScl, out.sky_flow_height_mask_scale);
    F(kSky, kFlowHeightBias, out.sky_flow_height_mask_bias);
    I(kSky, kDrawSunDisc, out.sky_draw_sun_disc);
    F(kSky, kSunDiscSize, out.sun_disc_size);
    F(kSky, kSunDiscScale, out.sun_disc_scale);
    V(kSky, kRayleigh, out.rayleigh, 3);
    F(kSky, kRayleighScl, out.rayleigh_scale);
    F(kSky, kMieCoef, out.mie_coefficient);
    F(kSky, kMieG, out.mie_g);
    I(kSky, kUseAerial, out.use_aerial_perspective);
    F(kSky, kAerialScale, out.aerial_perspective_scale);
    F(kSky, kAerialInten, out.aerial_perspective_intensity);
    F(kSky, kEarthRadius, out.earth_radius);
    F(kSky, kAtmoRadius, out.atmosphere_radius);
    V(kSky, kHfColorAdd, out.height_fog_color_add, 3);
    F(kSky, kCloud1Alt, out.cloud1_altitude);
    F(kSky, kCloud1Tile, out.cloud1_tile_factor);
    F(kSky, kCloud1Rot, out.cloud1_rotation);
    F(kSky, kCloud1Speed, out.cloud1_speed);
    F(kSky, kCloud1Alpha, out.cloud1_alpha_mul);
    V(kSky, kCloud1Color, out.cloud1_color, 3);
    T(kSky, kPanoTexture, out.panorama_res);
    T(kSky, kPanoAlphaTex, out.panorama_alpha_res);
    T(kSky, kSkyGradTex, out.sky_gradient_res);
    T(kSky, kFlowMaskTex, out.flow_mask_res);
    T(kSky, kCloud1Tex, out.cloud_layer1_res);

    // ---- fog -------------------------------------------------------------
    I(kFog, kFogHeightEn, out.fog_height_enable);
    I(kFog, kFogColorEn, out.fog_color_enable);
    I(kFog, kFogGradEn, out.fog_gradient_enable);
    V(kFog, kFogColor, out.fog_color, 3);
    F(kFog, kFogStart, out.fog_dist_start);
    F(kFog, kFogEnd, out.fog_dist_end);
    F(kFog, kFogColStart, out.fog_color_start);
    F(kFog, kFogColEnd, out.fog_color_end);
    F(kFog, kHfStart, out.fog_height_start);
    F(kFog, kHfEnd, out.fog_height_end);
    F(kFog, kHfAltitude, out.fog_altitude);
    F(kFog, kHfDepth, out.fog_depth);
    F(kFog, kHfVisRange, out.fog_visibility_range);
    I(kFog, kPmEnable, out.volumetrics_enable);
    F(kFog, kSunScatter, out.sun_scatter_intensity);
    F(kFog, kLocalScatter, out.local_light_scatter_intensity);

    // ---- exposure --------------------------------------------------------
    I(kExpo, kAutoExposure, out.auto_exposure);
    F(kExpo, kEv, out.ev);
    F(kExpo, kEvMax, out.ev_max);
    F(kExpo, kExpComp, out.exposure_compensation);
    V(kExpo, kBloomScale, out.bloom_scale, 3);
    I(kExpo, kBloomMethod, out.bloom_method);
    T(kExpo, kLensDirtTex, out.lens_dirt_res);

    // ---- grading + white balance -----------------------------------------
    I(kGrade, kGradingEn, out.grading_enable);
    V(kGrade, kGradeBright, out.grade_brightness, 3);
    V(kGrade, kGradeContr, out.grade_contrast, 3);
    V(kGrade, kGradeSat, out.grade_saturation, 3);
    F(kGrade, kGradeHue, out.grade_hue);
    T(kGrade, kGradeLut, out.grading_lut_res);
    F(kWhite, kWbTemp, out.white_temperature);
    F(kWhite, kWbTint, out.white_tint);

    // ---- ambient occlusion ------------------------------------------------
    I(kAO, kAffectOutdoor, out.ao_affects_outdoor_light);
    I(kAO, kAffectLocal, out.ao_affects_local_light);
    F(kAO, kSsaoInner, out.ssao_max_distance_inner);
    F(kAO, kSsaoOuter, out.ssao_max_distance_outer);
    F(kAO, kHbaoRadius, out.hbao_radius);
    F(kAO, kHbaoContrast, out.hbao_contrast);
    F(kAO, kDynamicAO, out.dynamic_ao_factor);

    // ---- global illumination ----------------------------------------------
    V(kGI, kGiTerrainCol, out.gi_terrain_color, 3);
    V(kGI, kGiSkyCol, out.gi_sky_color, 3);
    V(kGI, kGiGroundCol, out.gi_ground_color, 3);
    V(kGI, kGiSunCol, out.gi_sun_color, 3);
    F(kGI, kGiBackRotX, out.gi_backlight_rotation_x);
    F(kGI, kGiBackRotY, out.gi_backlight_rotation_y);
    F(kGI, kGiBounce, out.gi_bounce_scale);
    F(kGI, kGiSunScale, out.gi_sun_scale);

    // ---- shadows -----------------------------------------------------------
    F(kShadow, kSunShadowDist, out.sun_shadow_view_distance);

    return true;
}

bool ve_lighting(Source& src, TypeDb& types, const std::string& level,
                 VeLighting& out, std::string& err)
{
    int cands = 0;
    const std::string part = ve_active_preset(src, types, level, &cands, err);
    if (part.empty()) return false;
    if (!ve_lighting_partition(src, types, part, out, err)) return false;
    out.preset_candidates = cands;
    return true;
}

}  // namespace bf6
