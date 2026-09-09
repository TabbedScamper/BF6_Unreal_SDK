/* Light probe volumes - the authored half of diffuse GI placement.
 *
 * 12,983 LightProbeVolumeData over 25 levels. A volume is a BLEND REGION: one
 * BlendDistance plus a per-axis min/max override on each of X, Y and Z, a
 * Weight, and a DistanceOffsetAlongNormal that pushes sample points off
 * surfaces. Six of the eight named fields exist only to override the single
 * BlendDistance per face, which is what makes this an authored falloff box
 * rather than a grid cell.
 *
 * ONLY THE NAMED FIELDS ARE READ. The reflection volume types alongside these
 * carry 35 fields of which none resolve in data/sdk_field_names.tsv, so they
 * are COUNTED here and not decoded - guessing at unnamed Vec3s would produce
 * plausible numbers with nothing behind them.
 */

/* 483bb2e9-0cb9-8df4-0e15-6505efa15104  LightProbeVolumeData */
static const uint8_t kLpVolume[16] = {
    0xe9, 0xb2, 0x3b, 0x48, 0xb9, 0x0c, 0xf4, 0x8d,
    0x0e, 0x15, 0x65, 0x05, 0xef, 0xa1, 0x51, 0x04
};

static const uint32_t kLpBlend      = 0xc487b29au;  /* BlendDistance              */
static const uint32_t kLpMinX       = 0x70016c9au;  /* BlendDistanceOverrideMinX  */
static const uint32_t kLpMaxX       = 0x47180a16u;  /* ...MaxX                    */
static const uint32_t kLpMinY       = 0x17928cf5u;
static const uint32_t kLpMaxY       = 0x522c4bf2u;
static const uint32_t kLpMinZ       = 0x0a4bfd5fu;
static const uint32_t kLpMaxZ       = 0x9ea25fbau;
static const uint32_t kLpOffset     = 0x4f829dedu;  /* DistanceOffsetAlongNormal  */
static const uint32_t kLpWeight     = 0x89b14df8u;  /* Weight                     */
static const uint32_t kLpUnnamedInt = 0x3f680d24u;  /* unnamed, 20 where sampled  */

struct LpHandle {
    bf6_light_probes            p{};
    std::vector<bf6_light_probe> vols;
};

bf6_light_probes* bf6_light_probes_read(bf6_ctx* c, const char* ebx_name)
{
    if (!c || !ebx_name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx x(*c->types);
    if (!bf6__gl_open(c, ebx_name, x)) return nullptr;

    LpHandle* h = new LpHandle();
    for (size_t i = 0; i < x.instance_count(); i++) {
        if (memcmp(x.instance_type(i).data(), kLpVolume, 16) != 0) continue;
        bf6::EbxValue v = x.read_instance(i);
        bf6_light_probe p{};
        p.instance = (int32_t)i;
        auto f = [&](uint32_t k) {
            const bf6::EbxValue* e = v.field(k);
            return e ? (float)bf6__spawn_num(*e) : 0.0f;
        };
        p.blend_distance = f(kLpBlend);
        p.blend_min[0] = f(kLpMinX); p.blend_max[0] = f(kLpMaxX);
        p.blend_min[1] = f(kLpMinY); p.blend_max[1] = f(kLpMaxY);
        p.blend_min[2] = f(kLpMinZ); p.blend_max[2] = f(kLpMaxZ);
        p.distance_offset_along_normal = f(kLpOffset);
        p.weight = f(kLpWeight);
        if (const bf6::EbxValue* e = v.field(kLpUnnamedInt)) p.unnamed_int = bf6__ws_u32(e);
        h->vols.push_back(p);
    }
    if (h->vols.empty()) { delete h; return nullptr; }
    h->p.count = (int32_t)h->vols.size();
    h->p.volumes = h->vols.data();
    c->handles[&h->p] = bf6_ctx::HK_LIGHTPROBE;
    return &h->p;
}

void bf6__lightprobe_delete(void* p) { delete (LpHandle*)p; }
