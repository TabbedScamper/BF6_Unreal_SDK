/* water_ext.inc - the full water render description, over the C ABI.
 *
 * NEW FILE. It is a translation-unit fragment, not a standalone .cpp: it uses
 * bf6_ctx, which only exists inside bf6_core.cpp. Wire it with ONE line in
 * src/bf6_core.cpp, immediately BEFORE that file's last line - the include has
 * to be INSIDE the extern "C" block so the new entry point gets C linkage:
 *
 *     #include "water_ext.inc"
 *     }  // extern "C"          <- the existing last line, unchanged
 *
 * and add the declarations in the comment block at the bottom of this file to
 * include/bf6_core.h, after the bf6_level_water_sim prototype. Nothing else in
 * either file changes.
 *
 * WHY A SECOND WATER CALL. bf6_level_water answers "where is the plane and
 * roughly what colour is it". A renderer needs more than that and, worse, some
 * of what it needs is a TRAP if read naively:
 *
 *   - the authored colour is a PER-METRE TRANSMISSION, not a surface colour.
 *     Painting it on a plane gives a flat pastel pane. This call returns both
 *     the raw authored value and the converted surface colour and extinction,
 *     so a consumer cannot get that wrong by accident.
 *   - ShoreDepth is meaningless unless OceanWavesAttenuationType == ShoreDepth.
 *     mp_atoll ships -18.3775 and mp_isolated 14.0251 while both run CoarseMask
 *     and their shaders do not even declare the parameter. attenuation_type is
 *     returned FIRST for that reason.
 *   - there are THREE shader families and they share no parameter. variant says
 *     which, and the fields belonging to the others are left at the "absent"
 *     sentinel rather than at zero, because zero is a legal authored value here.
 *
 * Sources: systems/WATER.md and the findings it lists, in the research repo.
 */

/* ---- material slot ids (Name32) -------------------------------------------
 * These are content-graph generated and are NOT recoverable names; they are
 * stable opaque slot ids, verified identical across every level that ships the
 * family. The ENGINE-side ids in cb0 are recoverable and are not needed here.
 */
static const uint32_t kW2_OceanColour   = 0xEACA953A;  /* ocean base colour     */
static const uint32_t kW2_AbsorbDist    = 0x2D8E51DC;  /* ocean, metres         */
static const uint32_t kW2_Overlay0      = 0xD45F0908;  /* applied in this order */
static const uint32_t kW2_Overlay1      = 0x68F68C0F;
static const uint32_t kW2_Overlay2      = 0xD5C71AFD;
static const uint32_t kW2_Overlay3      = 0xEC10E3B5;
static const uint32_t kW2_Shallow       = 0x50B54E74;  /* foam family           */
static const uint32_t kW2_Deep          = 0xDFCB439C;
static const uint32_t kW2_DepthRamp     = 0x5AD4BBEE;  /* foam family, metres   */
static const uint32_t kW2_DetailFade    = 0x0795990F;  /* Vec2, metres          */
static const uint32_t kW2_FoamThreshold = 0xFA9AA135;
static const uint32_t kW2_ShoreFoamSupp = 0x1AA264BA;
static const uint32_t kW2_FoamContrast  = 0x4B6CAED4;
static const uint32_t kW2_FoamW0        = 0x1E626FB8;
static const uint32_t kW2_FoamW1        = 0x83903BBB;
static const uint32_t kW2_FoamW2        = 0xF6A14FA0;
static const uint32_t kW2_FoamW3        = 0x5BCF1BA3;
static const uint32_t kW2_SmoothZero    = 0x35870FDC;
static const uint32_t kW2_SmoothFull    = 0xBD9F2E32;
static const uint32_t kW2_SmoothBias    = 0xC57449FF;
static const uint32_t kW2_SmoothNear    = 0x9A44CFD5;
static const uint32_t kW2_FoamSheetN    = 0xC4119C14;
static const uint32_t kW2_MicroSheetN   = 0xAB684B74;
static const uint32_t kW2_NoiseUvScale  = 0xAD0F5BEF;
static const uint32_t kW2_ReflLow       = 0x5629D818;
static const uint32_t kW2_ReflHigh      = 0xAE1243A2;
static const uint32_t kW2_ReflBias      = 0x02F07FDA;
static const uint32_t kW2_ContactFoam   = 0x6AC9C660;
static const uint32_t kW2_FoamRgb2      = 0xB4B73136;
/* MP_Isolated's extended ExpressionShader graph. The slot hashes are the
 * durable read path; the register numbers in one compiled permutation are
 * evidence, not runtime inputs. */
static const uint32_t kW2_MicroUvScale  = 0xC5625D31;
static const uint32_t kW2_FoamUvScale   = 0x1AFFA140;
static const uint32_t kW2_MicroFlow     = 0x9C172692;
static const uint32_t kW2_CompositeLow  = 0x2522B14D;
static const uint32_t kW2_CompositeHigh = 0x2522B253;
static const uint32_t kW2_ContactDiv    = 0xA93365F5;
static const uint32_t kW2_ContactLow    = 0x0DC163B0;
static const uint32_t kW2_ContactGain   = 0x54E5C060;
/* MP_Isolated pass-0 t32 crest-mask path.  The names are deliberately
 * semantic descriptions of their verified use, not guesses at lost author
 * labels. */
static const uint32_t kW2_BroadWorldMul   = 0x2442CDFB;
static const uint32_t kW2_BroadWorldScale = 0x16B4965A;
static const uint32_t kW2_BroadFloor      = 0x9702B2FC;

/* ---- WaterSurfaceEntityData field hashes ---------------------------------
 * Offsets and types from the MP executable's reflection records; see
 * data/exe_water_reflection_layouts.tsv. DO NOT read these through the SP
 * executable's schema - it lays the water classes out differently and every
 * field parses to a wrong value without erroring.
 */
static const uint32_t kW2_FShoreDepth   = 0x41737016;  /* Float32, metres  */
static const uint32_t kW2_FShoreBlend   = 0x3D672CE5;  /* Vec4 cubic       */
static const uint32_t kW2_FShoreEnable  = 0xF2EBF488;  /* Boolean          */
static const uint32_t kW2_FAttenType    = 0x4F78B966;  /* OceanWavesAttenuationType */
static const uint32_t kW2_FAddDepth     = 0xB62E36DE;  /* Float32, metres  */
static const uint32_t kW2_FQueryHalf    = 0xE6CF3A3F;  /* Vec3 half extent */
static const uint32_t kW2_FVisible      = 0x2A7B2AF9;  /* Boolean          */
static const uint32_t kW2_FTerrainVt    = 0xD25F75D1;  /* Boolean          */
static const uint32_t kW2_FWaveAmp      = 0xCF58D73D;  /* Float32          */
/* WaterSurfaceDynamicState fields consumed by the current cascade-overlap
 * path. Their semantic mapping is controlled by both the reflected layout and
 * the selected retail vertex shader; a shader mismatch leaves version zero. */
static const uint32_t kW2_FOverlapShear  = 0xB5C45E6B; /* Vec2              */
static const uint32_t kW2_FOverlapAngle  = 0xB177B14A; /* degrees           */
static const uint32_t kW2_FOverlapSizeA  = 0x8E02BE99; /* metres            */
static const uint32_t kW2_FOverlapHeight = 0x9646FBD1; /* vertical scale    */
static const uint32_t kW2_FOverlapSizeB  = 0x548C290B; /* metres            */
static const uint32_t kW2_FOverlapEnable = 0xE95D1D95; /* Boolean           */
/* THE RIVER FLAG. Unnamed in the reflection table; named by behaviour. True on
 * exactly the 8 of 14 surfaces whose shader permutation declares
 * Serac:WaterRiverFlowSampler, false on the other 6, and the byte the CPU gates
 * all three river parameters on. This is the only readable water TYPE. */
static const uint32_t kW2_FIsRiver      = 0x8325B2E3;  /* Boolean, +0x027B */

/* Active VisualEnvironment OceanComponentData.  Field hashes come from the
 * current executable's reflection and are joined to the active VE selected by
 * the level root.  The deferred shader register mapping is documented in
 * water-deferred-composite-decoded.md; source values remain exposed too so
 * the conversion can be independently checked by a caller. */
static const char* kW2_OceanComponent =
    "f7f42a90-6547-0ee2-cb65-9252e610e782";
static const uint32_t kW2_OCEnable            = 0x6E8C0B93;
static const uint32_t kW2_OCIOR               = 0x0F4981F7;
static const uint32_t kW2_OCSimplified        = 0xEBC53723;
static const uint32_t kW2_OCShoreFade         = 0x890C6021;
static const uint32_t kW2_OCPhaseG            = 0xECAD3CA0;
static const uint32_t kW2_OCSunShadow         = 0x1AF08A41;
static const uint32_t kW2_OCFoamEnable        = 0x8E1B8D70;
static const uint32_t kW2_OCFoamTint          = 0x860ABC66;
static const uint32_t kW2_OCFoamSmoothness    = 0x4B61E3CA;
static const uint32_t kW2_OCFoamShoreFade     = 0x450CF66A;
static const uint32_t kW2_OCAlbedo            = 0x3F39B649;
static const uint32_t kW2_OCAlbedoDistance    = 0xD689C9A0;
/* Vec3/Vec4 members are read BY OFFSET, never by hash order. */
static const uint32_t kW2_Vec4Members[4] =
    { 0x3901DB14, 0x42FC0F5E, 0x32A99B9C, 0x7C8062F2 };

static float W2_F(const bf6::EbxValue& d, uint32_t h, float dflt)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f) return dflt;
    if (f->kind == bf6::EbxValue::Kind::Real) return (float)f->f;
    if (f->kind == bf6::EbxValue::Kind::Int)  return (float)f->i;
    if (f->kind == bf6::EbxValue::Kind::Uint) return (float)f->u;
    return dflt;
}
static int W2_I(const bf6::EbxValue& d, uint32_t h, int dflt)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f) return dflt;
    if (f->kind == bf6::EbxValue::Kind::Bool) return f->b ? 1 : 0;
    if (f->kind == bf6::EbxValue::Kind::Int)  return (int)f->i;
    if (f->kind == bf6::EbxValue::Kind::Uint) return (int)f->u;
    if (f->kind == bf6::EbxValue::Kind::Real) return (int)f->f;
    return dflt;
}
static void W2_Vec(const bf6::EbxValue& d, uint32_t h, int n, float* out)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f || f->kind != bf6::EbxValue::Kind::Struct) return;
    for (int i = 0; i < n; i++) out[i] = W2_F(*f, kW2_Vec4Members[i], out[i]);
}

/* One inline constant out of a resolved depot record, by slot. Returns false
 * when the record does not carry the slot, which is NOT the same as zero. */
static bool W2_Const(const bf6::MaterialBinding& mb, uint32_t slot, int n, float* out)
{
    auto it = mb.constants.find(slot);
    if (it == mb.constants.end()) return false;
    if (it->second.size() < (size_t)n * 4) return false;
    std::memcpy(out, it->second.data(), (size_t)n * 4);
    return true;
}

static bool W2_VecRequired(const bf6::EbxValue& d, uint32_t h, int n, float* out)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f || f->kind != bf6::EbxValue::Kind::Struct) return false;
    for (int i = 0; i < n; ++i) {
        const bf6::EbxValue* q = f->field(kW2_Vec4Members[i]);
        if (!q || q->kind != bf6::EbxValue::Kind::Real) return false;
        out[i] = (float)q->f;
    }
    return true;
}

struct W2_OceanComposite
{
    uint32_t version = 0;
    std::string preset;
    int candidates = 0;
    int enable = -1;
    int simplified = -1;
    int foam_enable = -1;
    float ior = -1.f;
    float opacity_ramp = -1.f;
    float foam_ramp = -1.f;
    float phase_g = -1.f;
    float transmission[3] = { -1.f, -1.f, -1.f };
    float shadow = -1.f;
    float foam_tint[3] = { -1.f, -1.f, -1.f };
    float foam_smoothness = -1.f;
    float foam_roughness = -1.f;
    float albedo[3] = { -1.f, -1.f, -1.f };
    float albedo_distance = -1.f;
};

static bool W2_FieldPresent(const bf6::EbxValue& d, uint32_t h)
{
    return d.field(h) != nullptr;
}

static bool W2_ReadOceanComposite(bf6_ctx* c, const char* level,
                                  W2_OceanComposite& o)
{
    o = W2_OceanComposite();
    if (!c || !c->types || !level || !*level) return false;

    std::string err;
    o.preset = bf6::ve_active_preset(c->src, *c->types, level,
                                     &o.candidates, err);
    if (o.preset.empty()) return false;
    std::vector<uint8_t> raw = c->src.get_ebx(o.preset, err);
    if (raw.empty()) return false;
    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err)) return false;

    for (size_t i = 0; i < e.instance_count(); ++i) {
        if (bf6::TypeDb::guid_str(e.instance_type(i)) != kW2_OceanComponent)
            continue;
        const bf6::EbxValue d = e.read_instance(i);
        const bool complete =
            W2_FieldPresent(d, kW2_OCEnable) &&
            W2_FieldPresent(d, kW2_OCIOR) &&
            W2_FieldPresent(d, kW2_OCSimplified) &&
            W2_FieldPresent(d, kW2_OCShoreFade) &&
            W2_FieldPresent(d, kW2_OCPhaseG) &&
            W2_FieldPresent(d, kW2_OCSunShadow) &&
            W2_FieldPresent(d, kW2_OCFoamEnable) &&
            W2_FieldPresent(d, kW2_OCFoamTint) &&
            W2_FieldPresent(d, kW2_OCFoamSmoothness) &&
            W2_FieldPresent(d, kW2_OCFoamShoreFade) &&
            W2_FieldPresent(d, kW2_OCAlbedo) &&
            W2_FieldPresent(d, kW2_OCAlbedoDistance);

        o.enable = W2_I(d, kW2_OCEnable, -1);
        o.ior = W2_F(d, kW2_OCIOR, -1.f);
        o.simplified = W2_I(d, kW2_OCSimplified, -1);
        o.opacity_ramp = W2_F(d, kW2_OCShoreFade, -1.f);
        o.phase_g = W2_F(d, kW2_OCPhaseG, -1.f);
        o.shadow = W2_F(d, kW2_OCSunShadow, -1.f);
        o.foam_enable = W2_I(d, kW2_OCFoamEnable, -1);
        W2_Vec(d, kW2_OCFoamTint, 3, o.foam_tint);
        o.foam_smoothness = W2_F(d, kW2_OCFoamSmoothness, -1.f);
        o.foam_ramp = W2_F(d, kW2_OCFoamShoreFade, -1.f);
        W2_Vec(d, kW2_OCAlbedo, 3, o.albedo);
        o.albedo_distance = W2_F(d, kW2_OCAlbedoDistance, -1.f);

        if (o.foam_smoothness >= 0.f)
            o.foam_roughness = 1.f - o.foam_smoothness;
        if (o.albedo[0] >= 0.f) {
            for (int k = 0; k < 3; ++k) {
                const float col = o.albedo[k] > 1e-6f ? o.albedo[k] : 1e-6f;
                float v = o.albedo_distance > 0.01f
                    ? 1.f + std::log(col) / o.albedo_distance
                    : o.albedo[k];
                o.transmission[k] = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            }
        }
        if (complete && o.ior > 0.f && o.opacity_ramp > 0.f &&
            o.foam_ramp > 0.f && o.albedo_distance > 0.f)
            o.version = 1;
        return o.version != 0;
    }
    return false;
}

static uint32_t W2_Rd32(const std::vector<uint8_t>& d, size_t o)
{
    uint32_t v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}

static uint64_t W2_Rd64(const std::vector<uint8_t>& d, size_t o)
{
    uint64_t v = 0;
    if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8);
    return v;
}

/* Resolve pass zero through the mounted build's ShaderStateDatabase. Material
 * slots are not sufficient identity: a later cook may preserve them while
 * selecting different arithmetic. */
static bool W2_SelectedPass0Guids(bf6_ctx* c, const std::string& lvl,
                                 uint64_t state_key, uint8_t out_vertex[16],
                                 uint8_t out_pixel[16])
{
    std::memset(out_vertex, 0, 16);
    std::memset(out_pixel, 0, 16);
    if (!c || !state_key) return false;
    std::string dbname;
    for (const auto& kv : c->src.res()) {
        if (kv.first.find("shaderstate_db") != std::string::npos &&
            kv.first.find(lvl) != std::string::npos) {
            dbname = kv.first;
            break;
        }
    }
    if (dbname.empty()) return false;
    std::string err;
    std::vector<uint8_t> db = c->src.get_res(dbname, err);
    if (db.size() < 16) return false;
    const uint32_t program_count = W2_Rd32(db, 0);
    const uint64_t program_pointer = W2_Rd64(db, 4);
    size_t key_at = 0;
    bool found = false;
    for (size_t o = 0; o + 16 <= db.size(); o += 4) {
        if (W2_Rd64(db, o) != state_key) continue;
        const uint32_t count = W2_Rd32(db, o + 8);
        if (!count || count > 8 || o + 12ull + 4ull * count > db.size()) continue;
        bool slots_ok = true;
        for (uint32_t s = 0; s < count; ++s)
            if (W2_Rd32(db, o + 12 + 4ull * s) >= program_count) slots_ok = false;
        if (slots_ok) { key_at = o; found = true; break; }
    }
    if (!found) return false;
    const uint32_t slot = W2_Rd32(db, key_at + 12);
    const size_t rec = (size_t)program_pointer + (size_t)slot * 0xA8;
    if (rec + 0x68 > db.size()) return false;
    const uint32_t permutation_count = W2_Rd32(db, rec + 0x60);
    const uint32_t permutation_offset = W2_Rd32(db, rec + 0x64);
    if (!permutation_count || permutation_offset + 8 > db.size()) return false;
    const uint64_t permutation_id = W2_Rd64(db, permutation_offset);
    char id_decimal[32];
    std::snprintf(id_decimal, sizeof(id_decimal), "%llu",
                  (unsigned long long)permutation_id);
    std::string permutation_res;
    for (const auto& kv : c->src.res()) {
        const size_t p = kv.first.find("expressionshader/permutation");
        if (p == std::string::npos) continue;
        if (std::strcmp(kv.first.c_str() + p + 28, id_decimal) == 0) {
            permutation_res = kv.first;
            break;
        }
    }
    if (permutation_res.empty()) return false;
    std::vector<uint8_t> permutation = c->src.get_res(permutation_res, err);
    if (permutation.size() < 68) return false;
    std::memcpy(out_vertex, permutation.data() + 0x20, 16);
    std::memcpy(out_pixel, permutation.data() + 0x30, 16);
    return true;
}

int bf6_level_water_render(bf6_ctx* c, const char* level,
                           bf6_water_render* out, int out_max)
{
	if (!c || !level || !*level) return 0;

	std::string lvl;
	if (c->types && c->walk && c->walked_level == level) {
		lvl = c->walk->root;
		if (lvl.size() > 4 && lvl.compare(lvl.size() - 4, 4, ".ebx") == 0) lvl.resize(lvl.size() - 4);
		const size_t slash = lvl.find_last_of('/');
		lvl = slash == std::string::npos ? std::string() : lvl.substr(0, slash);
	} else {
		std::string mount_err;
		if (!ensure_mounted(c, level, mount_err) || !ensure_types(c, mount_err)) return 0;
		lvl = BF6_IsolatedLevelDir(c, level);
		if (lvl.empty()) return 0;
	}

    std::string part;
    {
        const std::string cands[] = { lvl + "/default", lvl + "/_layers_content/water",
                                      std::string(level) + "/default" };
        for (const std::string& cand : cands)
            if (c->src.ebx().count(cand) && BF6_CountsWater(c, cand)) { part = cand; break; }
        if (part.empty() && !lvl.empty()) {
            std::vector<std::string> rest;
            for (const auto& kv : c->src.ebx()) {
                const std::string& n = kv.first;
                if (n.compare(0, lvl.size(), lvl) != 0) continue;
                std::string low = n;
                for (char& ch : low) ch = (char)tolower((unsigned char)ch);
                if (low.find("water") != std::string::npos) {
                    if (BF6_CountsWater(c, n)) { part = n; break; }
                } else rest.push_back(n);
            }
            if (part.empty())
                for (const std::string& n : rest)
                    if (BF6_CountsWater(c, n)) { part = n; break; }
        }
    }
    if (part.empty()) return 0;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(part, err);
    if (raw.empty()) return 0;
    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err)) return 0;

    /* One level-wide VE join, copied onto each surface below. */
    W2_OceanComposite ocean;
    W2_ReadOceanComposite(c, level, ocean);

    int total = 0;
    for (size_t i = 0; i < e.instance_count(); i++) {
        if (bf6::TypeDb::guid_str(e.instance_type(i)) != kWaterTypeGuid) continue;

        const int64_t base = e.payload() + (int64_t)e.instance_offset(i);
        const std::vector<uint8_t>& d = e.raw();
        if (base < 0 || (size_t)base + 0x60 > d.size()) continue;
        float sx, sz, tx, ty, tz;
        std::memcpy(&sx, d.data() + base + 0x20, 4);
        std::memcpy(&sz, d.data() + base + 0x48, 4);
        std::memcpy(&tx, d.data() + base + 0x50, 4);
        std::memcpy(&ty, d.data() + base + 0x54, 4);
        std::memcpy(&tz, d.data() + base + 0x58, 4);
        if (std::fabs(sx) < 1.f || std::fabs(sz) < 1.f) continue;

        bf6_water_render w;
        std::memset(&w, 0, sizeof(w));
        /* Absent is NOT zero anywhere in this struct: zero is authored. */
        w.base_colour[0] = w.deep_colour[0] = -1.f;
        w.surface_colour[0] = -1.f;
        w.extinction[0] = w.extinction[1] = w.extinction[2] = -1.f;
        w.absorption_distance_m = -1.f;
        w.depth_ramp_m = -1.f;
        w.detail_normal = w.foam_normal = w.foam_rgb = w.noise = w.perlin =
            w.contact_foam = w.foam_rgb2 = -1;
        /* Every scalar below is a non-negative quantity when authored, and zero
         * is a legal authored value for all of them, so "absent" has to be a
         * NEGATIVE sentinel. This matters: MP_Isolated and MP_Atoll run a
         * different material graph and carry NONE of the named foam or
         * reflectance slots, so a consumer that read zero here would silently
         * turn their foam off instead of falling back. */
        w.detail_fade_start_m = w.detail_fade_end_m = -1.f;
        w.foam_threshold = w.shore_foam_suppression = w.foam_contrast_divisor = -1.f;
        w.smoothness_zero_foam = w.smoothness_full_foam = -1.f;
        w.smoothness_bias = w.smoothness_near_multiplier = -1.f;
        w.foam_sheet_normal_strength = w.micro_sheet_normal_strength = -1.f;
        w.noise_uv_scale = -1.f;
        w.reflectance_low = w.reflectance_high = w.reflectance_bias = -1.f;
        w.micro_sheet_uv_scale = w.foam_sheet_uv_scale = -1.f;
        w.micro_sheet_flow_speed = -1.f;
        w.foam_composite_low = w.foam_composite_high = -1.f;
        w.contact_world_divisor_m = w.contact_remap_low = w.contact_gain = -1.f;
        w.broad_pattern_world_mul = w.broad_pattern_world_scale = -1.f;
        w.broad_pattern_floor = -1.f;
        w.extended_graph_version = 0;
        w.cascade_overlap_version = 0;
        w.cascade_overlap_enabled = -1;
        w.cascade_overlap_height_scale = -1.f;
        w.ocean_component_version = ocean.version;
        w.ocean_preset_candidates = ocean.candidates;
        w.ocean_enable = ocean.enable;
        w.simplified_distortion = ocean.simplified;
        w.foam_enable = ocean.foam_enable;
        w.composite_ior = ocean.ior;
        w.opacity_ramp_m = ocean.opacity_ramp;
        w.foam_depth_ramp_m = ocean.foam_ramp;
        w.scatter_phase_g = ocean.phase_g;
        w.scatter_shadow_influence = ocean.shadow;
        w.foam_smoothness = ocean.foam_smoothness;
        w.foam_roughness = ocean.foam_roughness;
        w.authored_albedo_distance_m = ocean.albedo_distance;
        std::snprintf(w.ocean_preset, sizeof(w.ocean_preset), "%s",
                      ocean.preset.c_str());
        for (int q = 0; q < 3; ++q) {
            w.transmission_colour[q] = ocean.transmission[q];
            w.foam_tint[q] = ocean.foam_tint[q];
            w.authored_ocean_albedo[q] = ocean.albedo[q];
        }
        for (int q = 0; q < 4; ++q) {
            w.cascade_overlap_params[q] = -1.f;
            w.cascade_overlap_params2[q] = -1.f;
        }
        for (int r = 0; r < 22; ++r)
            for (int q = 0; q < 4; ++q) w.extended_cb1[r][q] = -1.f;
        for (int k = 0; k < 4; k++) w.cascade_foam_weight[k] = -1.f;

        w.center[0] = tx; w.center[1] = tz;
        w.size[0]   = std::fabs(sx); w.size[1] = std::fabs(sz);
        w.height    = ty;

        bf6::EbxValue inst = e.read_instance(i);
        w.state_key = 0;
        if (const bf6::EbxValue* f = inst.field(kWaterStateKeyField)) {
            if (f->kind == bf6::EbxValue::Kind::Uint) w.state_key = f->u;
            else if (f->kind == bf6::EbxValue::Kind::Int) w.state_key = (uint64_t)f->i;
        }
        const bool have_selected_programs = W2_SelectedPass0Guids(
            c, lvl, w.state_key, w.selected_pass0_vertex_guid,
            w.selected_pass0_pixel_guid);
        /* 1f91c608-0eca-e076-aaca-df444ef7bc38 in the .NET byte order used by
           the permutation record. */
        static const uint8_t kIsolatedPass0Pixel[16] = {
            0x08,0xC6,0x91,0x1F, 0xCA,0x0E, 0x76,0xE0,
            0xAA,0xCA,0xDF,0x44,0x4E,0xF7,0xBC,0x38 };
        const bool selected_extended_graph = have_selected_programs &&
            std::memcmp(w.selected_pass0_pixel_guid, kIsolatedPass0Pixel, 16) == 0;
        /* 330d12d8-9184-8305-e852-70246916e509, .NET record byte order. */
        static const uint8_t kIsolatedPass0Vertex[16] = {
            0xD8,0x12,0x0D,0x33, 0x84,0x91, 0x05,0x83,
            0xE8,0x52,0x70,0x24,0x69,0x16,0xE5,0x09 };
        const bool selected_overlap_graph = have_selected_programs &&
            std::memcmp(w.selected_pass0_vertex_guid, kIsolatedPass0Vertex, 16) == 0;
        w.attenuation_type        = W2_I(inst, kW2_FAttenType, 0);
        w.shore_enable            = W2_I(inst, kW2_FShoreEnable, 0);
        w.shore_depth_m           = W2_F(inst, kW2_FShoreDepth, 0.f);
        w.additional_water_depth_m= W2_F(inst, kW2_FAddDepth, 0.f);
        w.wave_amplitude_scale    = W2_F(inst, kW2_FWaveAmp, 1.f);
        w.visible                 = W2_I(inst, kW2_FVisible, 1);
        w.is_river                = W2_I(inst, kW2_FIsRiver, 0);
        w.terrain_vt_access       = W2_I(inst, kW2_FTerrainVt, 0);
        {
            const bf6::EbxValue* enabled = inst.field(kW2_FOverlapEnable);
            const bf6::EbxValue* angle = inst.field(kW2_FOverlapAngle);
            const bf6::EbxValue* size_a = inst.field(kW2_FOverlapSizeA);
            const bf6::EbxValue* height = inst.field(kW2_FOverlapHeight);
            const bf6::EbxValue* size_b = inst.field(kW2_FOverlapSizeB);
            float shear[2] = { 0.f, 0.f };
            const bool fields_ok = enabled && angle && size_a && height && size_b &&
                W2_VecRequired(inst, kW2_FOverlapShear, 2, shear);
            const float a = W2_F(inst, kW2_FOverlapSizeA, -1.f);
            const float b = W2_F(inst, kW2_FOverlapSizeB, -1.f);
            if (fields_ok && a > 0.f && b > 0.f) {
                const float degrees = W2_F(inst, kW2_FOverlapAngle, 0.f);
                const float radians = degrees * 0.01745329251994329577f;
                w.cascade_overlap_enabled = W2_I(inst, kW2_FOverlapEnable, 0);
                w.cascade_overlap_params[0] = std::sin(radians);
                w.cascade_overlap_params[1] = std::cos(radians);
                w.cascade_overlap_params[2] = shear[0];
                w.cascade_overlap_params[3] = shear[1];
                w.cascade_overlap_params2[0] = 1.f / a;
                w.cascade_overlap_params2[1] = 1.f / b;
                w.cascade_overlap_params2[2] = a;
                w.cascade_overlap_params2[3] = b;
                w.cascade_overlap_height_scale = W2_F(inst, kW2_FOverlapHeight, 1.f);
                if (selected_overlap_graph) w.cascade_overlap_version = 1;
            }
        }
        w.shore_blend[0] = w.shore_blend[1] = w.shore_blend[3] = 0.f;
        w.shore_blend[2] = 1.f;                       /* engine default f(t)=t */
        W2_Vec(inst, kW2_FShoreBlend, 4, w.shore_blend);
        {
            float qb[3] = { 0.f, 0.f, 0.f };
            W2_Vec(inst, kW2_FQueryHalf, 3, qb);
            w.query_box_half_extent[0] = qb[0];
            w.query_box_half_extent[1] = qb[2];
        }
        /* The shore fade only exists under attenuation type 1. Under 2 and 3
         * the shipped depth and cubic are dead fields, so say so rather than
         * handing a consumer a negative distance to divide by. */
        w.shore_fade_valid = (w.attenuation_type == 1) ? 1 : 0;

        if (w.state_key) {
            for (const auto& kv : c->src.res()) {
                const std::string& rn = kv.first;
                if (rn.find("shaderblockdepot") == std::string::npos) continue;
                if (!lvl.empty() && rn.find(lvl) == std::string::npos) continue;
                const std::vector<uint8_t>* dbytes = nullptr;
                bf6::Depot* dep = c->depot_named(rn, &dbytes);
                if (!dep || !dbytes || !dep->has_key(w.state_key)) continue;
                bf6::MaterialBinding mb = dep->textures_for(w.state_key, *dbytes);
                if (!mb.valid) continue;

                /* THREE families, not two. The Granite Portal sub-levels bind a
                 * third material that shares no slot with either of the others:
                 * seven float3 colours, each paired with a float, and the floats
                 * are world coordinates (-1024, 1024, -2048, 512) rather than
                 * distances - a region-banded water. Returning 0 for it would
                 * hand a consumer the foam family's semantics and the wrong
                 * colour, so it gets its own value and its colour fields stay
                 * absent until the family is decoded. */
                w.variant = mb.constants.count(kW2_OceanColour) ? 1
                          : (mb.constants.count(kW2_Shallow) ||
                             mb.constants.count(kW2_Deep))     ? 0
                          : 2;

                const auto& gi = c->src.partition_index();
                auto grab = [&](uint32_t slot) -> int32_t {
                    auto sit = mb.textures.find(slot);
                    if (sit == mb.textures.end()) return -1;
                    auto ait = gi.find(sit->second);
                    if (ait == gi.end()) return -1;
                    std::string t = ait->second;
                    if (t.size() > 4 && t.compare(t.size() - 4, 4, ".ebx") == 0) t.resize(t.size() - 4);
                    return c->texture_id(t);
                };
                w.detail_normal = grab(kWSlotDetailNsh);
                w.foam_normal   = grab(kWSlotFoamNsh);
                w.foam_rgb      = grab(kWSlotFoamRgb);
                w.noise         = grab(kWSlotNoise);
                w.perlin        = grab(kWSlotPerlin);
                w.contact_foam  = grab(kW2_ContactFoam);
                w.foam_rgb2     = grab(kW2_FoamRgb2);

                if (w.variant == 1) {
                    W2_Const(mb, kW2_OceanColour, 3, w.base_colour);
                    float dist = -1.f;
                    W2_Const(mb, kW2_AbsorbDist, 1, &dist);
                    w.absorption_distance_m = dist;
                    static const uint32_t ov[4] = { kW2_Overlay0, kW2_Overlay1,
                                                    kW2_Overlay2, kW2_Overlay3 };
                    for (int k = 0; k < 4; k++) {
                        float t3[3];
                        if (!W2_Const(mb, ov[k], 3, t3)) continue;
                        std::memcpy(w.overlay_colour[w.overlay_count], t3, 12);
                        w.overlay_count++;
                    }
                } else if (w.variant == 0) {
                    W2_Const(mb, kW2_Shallow, 3, w.base_colour);
                    W2_Const(mb, kW2_Deep, 3, w.deep_colour);
                    float ramp = -1.f;
                    W2_Const(mb, kW2_DepthRamp, 1, &ramp);
                    w.depth_ramp_m = ramp;
                }

                float fade[2] = { 0.f, 0.f };
                if (W2_Const(mb, kW2_DetailFade, 2, fade))
                    { w.detail_fade_start_m = fade[0]; w.detail_fade_end_m = fade[1]; }
                W2_Const(mb, kW2_FoamThreshold, 1, &w.foam_threshold);
                W2_Const(mb, kW2_ShoreFoamSupp, 1, &w.shore_foam_suppression);
                W2_Const(mb, kW2_FoamContrast,  1, &w.foam_contrast_divisor);
                W2_Const(mb, kW2_FoamW0, 1, &w.cascade_foam_weight[0]);
                W2_Const(mb, kW2_FoamW1, 1, &w.cascade_foam_weight[1]);
                W2_Const(mb, kW2_FoamW2, 1, &w.cascade_foam_weight[2]);
                W2_Const(mb, kW2_FoamW3, 1, &w.cascade_foam_weight[3]);
                W2_Const(mb, kW2_SmoothZero,   1, &w.smoothness_zero_foam);
                W2_Const(mb, kW2_SmoothFull,   1, &w.smoothness_full_foam);
                W2_Const(mb, kW2_SmoothBias,   1, &w.smoothness_bias);
                W2_Const(mb, kW2_SmoothNear,   1, &w.smoothness_near_multiplier);
                W2_Const(mb, kW2_FoamSheetN,   1, &w.foam_sheet_normal_strength);
                W2_Const(mb, kW2_MicroSheetN,  1, &w.micro_sheet_normal_strength);
                W2_Const(mb, kW2_NoiseUvScale, 1, &w.noise_uv_scale);
                W2_Const(mb, kW2_ReflLow,  1, &w.reflectance_low);
                W2_Const(mb, kW2_ReflHigh, 1, &w.reflectance_high);
                W2_Const(mb, kW2_ReflBias, 1, &w.reflectance_bias);
                W2_Const(mb, kW2_MicroUvScale,  1, &w.micro_sheet_uv_scale);
                W2_Const(mb, kW2_FoamUvScale,   1, &w.foam_sheet_uv_scale);
                W2_Const(mb, kW2_MicroFlow,     1, &w.micro_sheet_flow_speed);
                W2_Const(mb, kW2_CompositeLow,  1, &w.foam_composite_low);
                W2_Const(mb, kW2_CompositeHigh, 1, &w.foam_composite_high);
                W2_Const(mb, kW2_ContactDiv,    1, &w.contact_world_divisor_m);
                W2_Const(mb, kW2_ContactLow,    1, &w.contact_remap_low);
                W2_Const(mb, kW2_ContactGain,   1, &w.contact_gain);
                W2_Const(mb, kW2_BroadWorldMul,   1, &w.broad_pattern_world_mul);
                W2_Const(mb, kW2_BroadWorldScale, 1, &w.broad_pattern_world_scale);
                W2_Const(mb, kW2_BroadFloor,      1, &w.broad_pattern_floor);

                /* The current MP_Isolated ExpressionShader's common
                 * BindingSet, reconstructed by Name32 rather than copied from
                 * a dump.  This table is intentionally adjacent to the read:
                 * when the selected shader changes, the runtime closure must
                 * reject version 1 until its BindingSet is decoded again. */
                if (w.foam_rgb2 >= 0 && w.detail_normal >= 0 &&
                    w.foam_normal >= 0 && w.noise >= 0 && w.contact_foam >= 0) {
                    auto one = [&](int r, int q, uint32_t h) {
                        return W2_Const(mb, h, 1, &w.extended_cb1[r][q]);
                    };
                    bool ok = true;
                    ok &= W2_Const(mb, 0x86FF4BB6, 4, w.extended_cb1[0]);
                    ok &= W2_Const(mb, 0x6FE3C6FD, 4, w.extended_cb1[1]);
                    ok &= W2_Const(mb, 0xEC10E3B5, 3, w.extended_cb1[2]);
                    ok &= one(2,3,0x2D8E51DC);
                    ok &= W2_Const(mb, 0xEACA953A, 3, w.extended_cb1[3]);
                    ok &= one(3,3,0x762ACD8A);
                    ok &= W2_Const(mb, 0xD5C71AFD, 3, w.extended_cb1[4]);
                    ok &= one(4,3,0xA93365F5);
                    ok &= W2_Const(mb, 0x0795990F, 2, w.extended_cb1[5]);
                    const uint32_t scalar[17][4] = {
                        {0xA970CEB0,0xBE1E6645,0,0},
                        {0x727228C2,0xE3D706BC,0x528303A9,0xF6D64FC7},
                        {0x677F3864,0xA85CF17C,0x35870FDC,0xBD9F2E32},
                        {0xC4119C14,0xAB684B74,0x4EA00F26,0x1F9EBEF8},
                        {0xCA6E9E77,0x5F82A97A,0xA23006D7,0x0309A262},
                        {0x179766CF,0x9C172692,0xB737D9B5,0xA90CCEB2},
                        {0x428DBA1B,0xB3A4850E,0xBC49D160,0x166D6730},
                        {0x166D662E,0x9D6D7926,0x401DBC4F,0x0DC163B0},
                        {0x54E5C060,0x45576615,0x0786871D,0x8F14B8ED},
                        {0x9D975A80,0x2442CDFB,0x6C178CDD,0xE0B7D29A},
                        {0x9702B2FC,0x16B4965A,0xD5DB189C,0xD6F33B4B},
                        {0x7696FBA1,0x9A4F54F5,0x67FF3FB3,0x6CD08F39},
                        {0x195E4836,0x232FF100,0xC5625D31,0x1AFFA140},
                        {0xF62181E8,0x83903BBB,0x1E626FB8,0xAD0F5BEF},
                        {0x5629D818,0xAE1243A2,0x02F07FDA,0xC76D5B4E},
                        {0xC57449FF,0x9A44CFD5,0x2522B14D,0x2522B253},
                        {0x1CA1D2FA,0,0,0}
                    };
                    ok &= one(5,2,scalar[0][0]); ok &= one(5,3,scalar[0][1]);
                    for (int r = 6; r <= 21; ++r) {
                        const int si = r - 5;
                        for (int q = 0; q < 4; ++q) {
                            if (!scalar[si][q]) { w.extended_cb1[r][q] = 0.f; continue; }
                            ok &= one(r,q,scalar[si][q]);
                        }
                    }
                    if (ok && selected_extended_graph) w.extended_graph_version = 1;
                }
                break;
            }
        }

        /* THE CONVERSION, done here so no consumer has to know it exists.
         *
         *   the ocean family's authored colour is what the water REACHES after
         *   `absorption_distance_m` metres. The draw shader turns it into a
         *   per-unit transmission with saturate(1 + ln(c)/d) and hands that to
         *   the deferred pass as an albedo. extinction is the same quantity in
         *   the form a Beer-Lambert renderer wants: transmission over L metres
         *   is exp(-extinction * L).
         *
         *   d <= 0.01 is the shader's own bypass: the colour is used raw.
         */
        if (w.variant == 1 && w.base_colour[0] >= 0.f) {
            const float dst = w.absorption_distance_m;
            for (int k = 0; k < 3; k++) {
                const float col = w.base_colour[k] > 1e-6f ? w.base_colour[k] : 1e-6f;
                if (dst > 0.01f) {
                    float s = 1.f + std::log(col) / dst;
                    w.surface_colour[k] = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
                    w.extinction[k] = -std::log(col) / dst;
                } else {
                    const float s = w.base_colour[k];
                    w.surface_colour[k] = s < 0.f ? 0.f : (s > 1.f ? 1.f : s);
                    /* Raw colour bypass supplies no volumetric extinction.
                     * Keep the absent sentinel, including when distance was
                     * not decoded. Zero would falsely mean lossless water. */
                }
            }
        } else if (w.variant == 0 && w.base_colour[0] >= 0.f) {
            /* the foam family has no logarithm: the shallow colour IS the
             * surface colour at zero depth. */
            std::memcpy(w.surface_colour, w.base_colour, 12);
        }

        if (out && total < out_max) out[total] = w;
        total++;
    }
    return total;
}

/* ---- exact water utility raster -----------------------------------------
 * Current bf6.exe path:
 *   terrain slot 10 -> generic utility raster deserialize 0x14591e3e0
 *                   -> recursive node loader 0x14591e740
 *                   -> R8 page reader 0x14591ebb0
 *                   -> runtime indirection builder 0x14591f070
 *                   -> water resource wrapper 0x145971670
 * The selected water VS consumes the resulting atlas/indirection pair and
 * applies 1-mask to cascades 0, 1 and their overlap. */
struct W2_MaskNode {
    uint16_t x, y, child;
    uint8_t level, inline_value;
    bool has_inline;
};

struct W2_MaskPage {
    uint16_t x, y;
    uint8_t level;
};

struct W2_MaskParse {
    const std::vector<uint8_t>* data;
    size_t pos;
    uint32_t tile_side, bytes_per_texel;
    std::vector<W2_MaskNode> nodes;
    std::vector<W2_MaskPage> pages;
    std::vector<uint8_t>* atlas;
};

static uint32_t W2_MaskU32(const std::vector<uint8_t>& d, size_t p)
{
    uint32_t v = 0;
    if (p + 4 <= d.size()) std::memcpy(&v, d.data() + p, 4);
    return v;
}

static float W2_MaskF32(const std::vector<uint8_t>& d, size_t p)
{
    float v = 0.f;
    if (p + 4 <= d.size()) std::memcpy(&v, d.data() + p, 4);
    return v;
}

static bool W2_MaskByte(W2_MaskParse& s, uint8_t& v, std::string& err)
{
    if (!s.data || s.pos >= s.data->size()) {
        err = "utility raster reads past end";
        return false;
    }
    v = (*s.data)[s.pos++];
    return true;
}

static bool W2_MaskFlag(W2_MaskParse& s, bool& v, std::string& err)
{
    uint8_t b = 0;
    if (!W2_MaskByte(s, b, err)) return false;
    if (b > 1) {
        err = "utility raster has a non-boolean node flag at " +
              std::to_string(s.pos - 1);
        return false;
    }
    v = b != 0;
    return true;
}

static bool W2_MaskNodeRead(W2_MaskParse& s, uint32_t index,
                            uint16_t x, uint16_t y, uint8_t level,
                            uint16_t& next_child, std::string& err)
{
    if (index >= s.nodes.size()) {
        err = "utility raster node exceeds declared count";
        return false;
    }
    W2_MaskNode& n = s.nodes[index];
    n.x = x; n.y = y; n.level = level; n.child = 0xffff;
    n.inline_value = 0; n.has_inline = false;

    bool has_data = false, persistent = false, has_inline = false;
    if (!W2_MaskFlag(s, has_data, err) ||
        !W2_MaskFlag(s, persistent, err) ||
        !W2_MaskFlag(s, has_inline, err)) return false;
    n.has_inline = has_inline;
    if (has_inline && !W2_MaskByte(s, n.inline_value, err)) return false;

    if (has_data && persistent) {
        const uint64_t bytes64 = (uint64_t)s.tile_side * s.tile_side * s.bytes_per_texel;
        if (bytes64 > s.data->size() || s.pos + (size_t)bytes64 > s.data->size()) {
            err = "utility raster page exceeds block";
            return false;
        }
        W2_MaskPage p;
        p.x = x; p.y = y; p.level = level;
        s.pages.push_back(p);
        s.atlas->insert(s.atlas->end(),
            s.data->begin() + (ptrdiff_t)s.pos,
            s.data->begin() + (ptrdiff_t)(s.pos + (size_t)bytes64));
        s.pos += (size_t)bytes64;
    }

    bool children = false;
    if (!W2_MaskFlag(s, children, err)) return false;
    if (!children) return true;
    if ((uint32_t)next_child + 3 >= s.nodes.size()) {
        err = "utility raster child group exceeds declared count";
        return false;
    }
    const uint16_t base = next_child;
    next_child = (uint16_t)(next_child + 4);
    n.child = base;
    const uint16_t x2 = (uint16_t)(x * 2), y2 = (uint16_t)(y * 2);
    return W2_MaskNodeRead(s, base + 0, x2 + 0, y2 + 0, level + 1, next_child, err) &&
           W2_MaskNodeRead(s, base + 1, x2 + 1, y2 + 0, level + 1, next_child, err) &&
           W2_MaskNodeRead(s, base + 2, x2 + 1, y2 + 1, level + 1, next_child, err) &&
           W2_MaskNodeRead(s, base + 3, x2 + 0, y2 + 1, level + 1, next_child, err);
}

static void W2_MaskFill(std::vector<uint32_t>& table, uint32_t side,
                        uint16_t x, uint16_t y, uint32_t shift, uint32_t packed)
{
    const uint32_t span = 1u << shift;
    const uint32_t x0 = (uint32_t)x << shift;
    const uint32_t y0 = (uint32_t)y << shift;
    for (uint32_t yy = 0; yy < span; ++yy)
        for (uint32_t xx = 0; xx < span; ++xx)
            if (x0 + xx < side && y0 + yy < side)
                table[(size_t)(y0 + yy) * side + x0 + xx] = packed;
}

static int W2_MaskFail(char* err, int cap, const std::string& text)
{
    if (err && cap > 0) std::snprintf(err, (size_t)cap, "%s", text.c_str());
    return 0;
}

int bf6_level_water_mask(bf6_ctx* c, const char* level,
                         bf6_water_mask* out, char* err, int err_cap)
{
    if (out) std::memset(out, 0, sizeof(*out));
    if (!c || !level || !*level || !out)
        return W2_MaskFail(err, err_cap, "invalid water-mask arguments");
    std::string why;
    if (!ensure_mounted(c, level, why)) return W2_MaskFail(err, err_cap, why);

    std::string low_level(level), tree;
    for (char& ch : low_level) ch = (char)std::tolower((unsigned char)ch);
    for (const auto& kv : c->src.res()) {
        std::string n = kv.first;
        for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
        if (n.find("streamingtree") != std::string::npos &&
            n.find(low_level) != std::string::npos) {
            tree = kv.first;
            break;
        }
    }
    if (tree.empty())
        return W2_MaskFail(err, err_cap, "level has no terrain streaming tree");
    std::vector<uint8_t> resource = c->src.get_res(tree, why), block;
    if (resource.empty() || !bf6::Splat::find_block(resource, 10, block, why))
        return W2_MaskFail(err, err_cap, "water CoarseMask block 10: " + why);
    if (block.size() < 44)
        return W2_MaskFail(err, err_cap, "water CoarseMask header is truncated");

    const uint32_t tile = W2_MaskU32(block, 0);
    const uint32_t texel_shift = W2_MaskU32(block, 4);
    const uint32_t nodes = W2_MaskU32(block, 24);
    const uint32_t declared_pages = W2_MaskU32(block, 28);
    const uint32_t max_level = W2_MaskU32(block, 32);
    const uint32_t border = W2_MaskU32(block, 36);
    const uint32_t default_value = W2_MaskU32(block, 40);
    if (!tile || texel_shift > 3 || !nodes || nodes > 65535 || max_level > 15 ||
        border * 2 + 1 >= tile || default_value > 255)
        return W2_MaskFail(err, err_cap, "water CoarseMask header values are invalid");

    c->water_mask_atlas.clear();
    c->water_mask_indirection.clear();
    W2_MaskParse parse;
    parse.data = &block;
    parse.pos = 44;
    parse.tile_side = tile;
    parse.bytes_per_texel = 1u << texel_shift;
    parse.nodes.resize(nodes);
    parse.atlas = &c->water_mask_atlas;
    uint16_t next_child = 1;
    if (!W2_MaskNodeRead(parse, 0, 0, 0, 0, next_child, why))
        return W2_MaskFail(err, err_cap, why);
    if (next_child != nodes || parse.pages.size() != declared_pages ||
        parse.pos != block.size()) {
        char mismatch[192];
        std::snprintf(mismatch, sizeof(mismatch),
            "water CoarseMask closure mismatch: nodes %u/%u pages %zu/%u bytes %zu/%zu",
            (unsigned)next_child, (unsigned)nodes, parse.pages.size(),
            (unsigned)declared_pages, parse.pos, block.size());
        return W2_MaskFail(err, err_cap, mismatch);
    }

    const uint32_t side = 1u << max_level;
    const uint32_t inline_default = 0xffff0000u | 0x8000u | default_value;
    c->water_mask_indirection.assign((size_t)side * side, inline_default);
    for (uint32_t i = 0; i < (uint32_t)parse.pages.size(); ++i) {
        const W2_MaskPage& p = parse.pages[i];
        if (p.level > max_level) return W2_MaskFail(err, err_cap, "page level exceeds maximum");
        const uint32_t shift = max_level - p.level;
        W2_MaskFill(c->water_mask_indirection, side, p.x, p.y, shift,
                    (shift << 16) | i);
    }
    for (const W2_MaskNode& n : parse.nodes) {
        if (!n.has_inline) continue;
        if (n.level > max_level) return W2_MaskFail(err, err_cap, "inline level exceeds maximum");
        W2_MaskFill(c->water_mask_indirection, side, n.x, n.y,
                    max_level - n.level,
                    0xffff0000u | 0x8000u | n.inline_value);
    }

    out->version = 1;
    out->tile_side = tile;
    out->interior_side = tile - border * 2 - 1;
    out->border = border;
    out->page_count = declared_pages;
    out->indirection_side = side;
    out->bounds_min[0] = W2_MaskF32(block, 8);
    out->bounds_min[1] = W2_MaskF32(block, 12);
    out->bounds_max[0] = W2_MaskF32(block, 16);
    out->bounds_max[1] = W2_MaskF32(block, 20);
    const float span = out->bounds_max[0] - out->bounds_min[0];
    out->coverage_side_rcp = span != 0.f ? 1.f / span : 0.f;
    out->border_fraction = (float)border / (float)(tile - 1);
    out->atlas_r8 = c->water_mask_atlas.empty() ? nullptr : c->water_mask_atlas.data();
    out->indirection_u32 = c->water_mask_indirection.empty() ? nullptr :
                           c->water_mask_indirection.data();
    return 1;
}

/* =====================================================================
 * PASTE INTO include/bf6_core.h, immediately after the bf6_water_sim
 * declarations (anchor: the line
 *     BF6_API int bf6_level_water_sim(bf6_ctx*, const char* level, bf6_water_sim* out);
 * ), keeping the existing bf6_water and bf6_level_water untouched:
 *
 * ------------------------------------------------------------------- water 2
 * / THE FULL RENDER DESCRIPTION of a level's water, enough to drive a
 *   conventional forward or deferred renderer without reading the game again.
 *
 *   ABSENT IS NOT ZERO. Zero is a legal authored value for nearly everything
 *   here, so EVERY float in this struct that is not a coordinate is left at a
 *   NEGATIVE sentinel when the level does not carry it, and every texture id at
 *   -1. That is not defensive style: MP_Isolated and MP_Atoll run a different
 *   material graph and carry NONE of the named foam or reflectance slots, so a
 *   consumer reading zero would turn their foam off rather than fall back.
 *
 *   THREE FAMILIES. variant is 1 for the ocean shader (mp_tungsten, mp_eastwood,
 *   mp_isolated, mp_atoll), 0 for the reduced foam shader (mp_dumbo,
 *   mp_aftermath, mp_aftermath_portal), and 2 for the region-banded material the
 *   Granite Portal sub-levels bind, which is UNDECODED: its colour fields are
 *   left absent rather than guessed. The three share no material parameter, so
 *   the fields of the family you are not on are absent by design, not missing.
 *
 *   READ attenuation_type BEFORE shore_depth_m. Only type 1 (ShoreDepth) uses
 *   the shore depth and cubic; under 2 and 3 the shipped values are dead
 *   fields and one of them is negative in retail. shore_fade_valid carries
 *   that decision so a consumer does not have to know the enum. /
 * typedef struct {
 *     uint64_t state_key;
 *     int32_t  variant;                  / 1 ocean, 0 foam, 2 region-banded
 *                                          (undecoded; colours absent)        /
 *
 *     float center[2];                   / world X, Z of the plane centre   /
 *     float size[2];                     / metres, FULL width               /
 *     float height;                      / world Y                          /
 *     float query_box_half_extent[2];    / metres, half width; redundant    /
 *
 *     int32_t attenuation_type;          / 0 None 1 ShoreDepth 2 CoarseMask
 *                                          3 CoarseAndDetailMask            /
 *     int32_t shore_fade_valid;          / 1 only when attenuation_type==1   /
 *     int32_t shore_enable;
 *     float   shore_depth_m;             / metres; t = saturate(depth/this)  /
 *     float   shore_blend[4];            / cubic (a,b,c,d): dot4(t^3,t^2,t,1)/
 *     float   additional_water_depth_m;  / metres, added to the depth sample /
 *     float   wave_amplitude_scale;      / dimensionless                     /
 *     int32_t visible;
 *     int32_t terrain_vt_access;
 *     / THE ONLY READABLE WATER TYPE. There is no ocean/river/pool enum; this
 *       bool is the river flag, and it agrees 14 of 14 with whether the level's
 *       shader permutation declares Serac:WaterRiverFlowSampler. /
 *     int32_t is_river;
 *
 *     / COLOUR. base_colour is the AUTHORED value and on the ocean family it is
 *       a per-metre transmission, NOT a surface colour: it is what the water
 *       reaches after absorption_distance_m metres. surface_colour and
 *       extinction are that value converted, so a renderer never needs the
 *       formula. transmission over L metres is exp(-extinction * L). /
 *     float base_colour[3];              / linear RGB, authored              /
 *     float deep_colour[3];              / linear RGB, foam family only      /
 *     float surface_colour[3];           / linear RGB, DERIVED               /
 *     float extinction[3];               / per metre, DERIVED                /
 *     float absorption_distance_m;       / metres, ocean family              /
 *     float depth_ramp_m;                / metres, foam family               /
 *     int32_t overlay_count;             / 0..4 extra lerp-stack colours     /
 *     float overlay_colour[4][3];        / linear RGB, in application order;
 *                                          their weights are RUNTIME terms and
 *                                          cannot be evaluated offline       /
 *
 *     float detail_fade_start_m, detail_fade_end_m;
 *     float foam_threshold;              / 0..1                              /
 *     float shore_foam_suppression;      / 0..1                              /
 *     float foam_contrast_divisor;
 *     float cascade_foam_weight[4];      / cascade 0..3                      /
 *     float smoothness_zero_foam, smoothness_full_foam;
 *     float smoothness_bias, smoothness_near_multiplier;
 *     float foam_sheet_normal_strength, micro_sheet_normal_strength;
 *     float noise_uv_scale;              / 1/metres                          /
 *     float reflectance_low, reflectance_high, reflectance_bias;
 *
 *     / for bf6_texture_at, or -1 /
 *     int32_t detail_normal, foam_normal, foam_rgb, noise, perlin;
 *     int32_t contact_foam, foam_rgb2;
 * } bf6_water_render;
 *
 * / Requires bf6_open_level for this level first. Same convention as
 *   bf6_level_instances: returns the count, fills out[] to out_max. /
 * BF6_API int bf6_level_water_render(bf6_ctx*, const char* level,
 *                                    bf6_water_render* out, int out_max);
 * =====================================================================
 */
