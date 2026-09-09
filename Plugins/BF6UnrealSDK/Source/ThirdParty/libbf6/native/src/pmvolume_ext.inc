/* Participating-media volumes - the authored half of volumetric fog.
 *
 * 2,886 ParticipatingMediaVolumeEntityData over 25 levels, plus 40
 * ParticipatingMediaGraph and 103 FogComponentData. The volume is the placed
 * object; the graph is what it evaluates.
 *
 * ITS PARAMETERS USE THE EXPOSED-PARAMETER RECORD, the same shape the FX
 * emitter work decoded: PropertyId (0xE03E9C5F), a Vec4 value, IntValue
 * (0xC82B46DD), ExposableType (0x885464E2) and Normalize (0x0A57AD9F). The
 * SHAPE is shared; the id vocabulary is not shown to be - none of the volume
 * PropertyIds measured appear in data/fx_property_ids_v2.tsv, but that table
 * covers FX parameters and these are fog parameters, so a disjoint result is
 * what BOTH a shared and a separate namespace would produce. Do not read it as
 * evidence either way.
 */

/* c4de43ab-1c1c-e7cd-fe16-05ce2bd49fd8  ParticipatingMediaVolumeEntityData */
static const uint8_t kPmVolume[16] = {
    0xab, 0x43, 0xde, 0xc4, 0x1c, 0x1c, 0xcd, 0xe7,
    0xfe, 0x16, 0x05, 0xce, 0x2b, 0xd4, 0x9f, 0xd8
};
/* 74f2925c-2b4d-a0bb-5d6c-8865e3950dae  ParticipatingMediaGraph */
static const uint8_t kPmGraph[16] = {
    0x5c, 0x92, 0xf2, 0x74, 0x4d, 0x2b, 0xbb, 0xa0,
    0x5d, 0x6c, 0x88, 0x65, 0xe3, 0x95, 0x0d, 0xae
};

static const uint32_t kPmGraphParams = 0xd1097b29u;  /* PmGraphParams          */
static const uint32_t kPmObjectLayers= 0x483a7a3cu;  /* ObjectLayers           */
static const uint32_t kPmParams      = 0x6e1fbcdbu;  /* the parameter array    */
static const uint32_t kPmPropertyId  = 0xe03e9c5fu;  /* PropertyId             */
static const uint32_t kPmValue       = 0x8cf424e7u;  /* Vec4 value             */
static const uint32_t kPmIntValue    = 0xc82b46ddu;  /* IntValue               */
static const uint32_t kPmExposable   = 0x885464e2u;  /* ExposableType          */
static const uint32_t kPmNormalize   = 0x0a57ad9fu;  /* Normalize              */

struct PmHandle {
    bf6_pm_volumes            v{};
    std::vector<bf6_pm_param> params;
};

bf6_pm_volumes* bf6_pm_volumes_read(bf6_ctx* c, const char* ebx_name)
{
    if (!c || !ebx_name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx x(*c->types);
    if (!bf6__gl_open(c, ebx_name, x)) return nullptr;

    PmHandle* h = new PmHandle();
    for (size_t i = 0; i < x.instance_count(); i++) {
        const bf6::TypeGuid g = x.instance_type(i);
        const bool vol = memcmp(g.data(), kPmVolume, 16) == 0;
        const bool gr  = memcmp(g.data(), kPmGraph,  16) == 0;
        if (!vol && !gr) continue;
        if (gr) { h->v.graph_count++; continue; }

        h->v.volume_count++;
        bf6::EbxValue v = x.read_instance(i);
        if (const bf6::EbxValue* ol = v.field(kPmObjectLayers))
            h->v.object_layers = bf6__ws_u32(ol);
        if (const bf6::EbxValue* gp = v.field(kPmGraphParams))
            h->v.graph_param_count += (int32_t)gp->items.size();
        const bf6::EbxValue* arr = v.field(kPmParams);
        if (!arr) continue;
        for (const bf6::EbxValue& it : arr->items) {
            bf6_pm_param p{};
            p.volume_index = h->v.volume_count - 1;
            /* PropertyId IS A HASH and must not pass through a float - a 32-bit
             * id read as a float loses its low bits and still looks like an id. */
            if (const bf6::EbxValue* f = it.field(kPmPropertyId)) p.property_id = bf6__ws_u32(f);
            if (const bf6::EbxValue* f = it.field(kPmIntValue))   p.int_value   = bf6__ws_u32(f);
            if (const bf6::EbxValue* f = it.field(kPmExposable))  p.exposable_type = bf6__ws_u32(f);
            p.normalize = bf6__spawn_bool(it.field(kPmNormalize));
            if (const bf6::EbxValue* val = it.field(kPmValue)) {
                int k = 0;
                for (const auto& kv : val->fields) {
                    if (k >= 4) break;
                    p.value[k++] = (float)bf6__spawn_num(kv.second);
                }
                p.value_components = k;
            }
            h->params.push_back(p);
        }
    }
    if (h->v.volume_count == 0 && h->v.graph_count == 0) { delete h; return nullptr; }
    h->v.param_count = (int32_t)h->params.size();
    h->v.params = h->params.empty() ? nullptr : h->params.data();
    c->handles[&h->v] = bf6_ctx::HK_PMVOLUME;
    return &h->v;
}

void bf6__pmvolume_delete(void* p) { delete (PmHandle*)p; }
