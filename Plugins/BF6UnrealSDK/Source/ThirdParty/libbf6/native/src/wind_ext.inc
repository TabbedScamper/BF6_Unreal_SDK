/* WIND: authored MeshWind components and vector-field volumes.
 *
 *     630dda6a  MeshWindComponentData   0xB0, 18 fields
 *     cc88d99d  VectorFieldEntityData   0x70,  5 fields
 *     a0275a08  VectorFieldData         0xC0, 11 fields
 *
 * WHERE THEY LIVE, which is not obvious from the type names:
 *
 *   MeshWindComponentData ships 1,561 instances over 12 levels, ONE PER
 *   PARTITION, and those partitions are `_layers_world/cables/cableset_<guid>`
 *   - the wind component belongs to a cable set. `BonePositions` is EMPTY on
 *   them, which is consistent with the separately established result that
 *   BonePositions is not a cable chain: the cable's shape comes from elsewhere
 *   and this component only carries the spring parameters.
 *
 *   VectorFieldEntityData ships 20 instances over 7 levels and lives in
 *   BUILDING PREFABS (`prefabs/pf_mp_dumbo_tenement_*`), i.e. authored wind
 *   volumes around architecture rather than level-wide weather.
 *
 * The two-spring integrator this feeds is decoded elsewhere; what this reader
 * adds is the authored input side - the primary (Mass, Area) and secondary
 * (SecondaryMass, SecondaryDamping, SecondaryArea) spring constants, and the
 * VectorFieldMask that selects which fields a component responds to. */

static const uint32_t kWindXform     = 0xd6351edeu;
static const uint32_t kWindBones     = 0x17586882u;  /* BonePositions          */
static const uint32_t kWindMass      = 0x6e58e5b7u;
static const uint32_t kWindArea      = 0x31ebb6c2u;
static const uint32_t kWindSecMass   = 0x0e9552fau;
static const uint32_t kWindSecDamp   = 0xdbf5c20eu;
static const uint32_t kWindSecArea   = 0x21d9af44u;
static const uint32_t kWindFieldMask = 0x1021c09cu;  /* VectorFieldMask        */
static const uint32_t kWindExcluded  = 0x4cd269a8u;
static const uint32_t kWindClientIdx = 0x6c02476cu;
/* vector field */
static const uint32_t kVfMagScale    = 0x754def54u;  /* MagnitudeScale         */
static const uint32_t kVfData        = 0x7fbc9210u;
static const uint32_t kVfName        = 0x0c59fa06u;
static const uint32_t kVfMagnitude   = 0x79600a6au;
static const uint32_t kVfBottomPct   = 0xc5bfc01cu;
static const uint32_t kVfTags        = 0x605f79afu;

/* Mixed-endian on disk. */
static const uint8_t kMeshWindType[16] = {
    0x6a,0xda,0x0d,0x63, 0x39,0xb3, 0x07,0x39, 0x77,0x15,0x92,0xff,0xce,0x3e,0x05,0x5e
};
static const uint8_t kVectorFieldEntType[16] = {
    0x9d,0xd9,0x88,0xcc, 0x85,0xde, 0xe8,0x13, 0x5d,0x3b,0x85,0x4a,0x25,0x5a,0x13,0xe1
};
static const uint8_t kVectorFieldDataType[16] = {
    0x08,0x5a,0x27,0xa0, 0xa5,0xb9, 0x0e,0x4d, 0x70,0x3e,0x64,0x2e,0x5f,0x17,0xcf,0x75
};

struct WindHandle {
    bf6_wind                        s{};
    std::vector<bf6_wind_component> comps;
    std::vector<bf6_vector_field>   fields;
    std::vector<std::string>        names;
};

bf6_wind* bf6_wind_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(name, err);
    if (raw.empty()) return nullptr;

    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return nullptr;

    WindHandle* h = new WindHandle();

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid g = e.instance_type(i);
        const bool mw = memcmp(g.data(), kMeshWindType,        16) == 0;
        const bool vf = memcmp(g.data(), kVectorFieldEntType,  16) == 0;
        const bool vd = memcmp(g.data(), kVectorFieldDataType, 16) == 0;
        if (!mw && !vf && !vd) continue;

        bf6::EbxValue v = e.read_instance(i);

        if (mw) {
            bf6_wind_component w{};
            if (const bf6::EbxValue* f = v.field(kWindMass))      w.mass = bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = v.field(kWindArea))      w.area = bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = v.field(kWindSecMass))   w.secondary_mass = bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = v.field(kWindSecDamp))   w.secondary_damping = bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = v.field(kWindSecArea))   w.secondary_area = bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = v.field(kWindFieldMask)) w.vector_field_mask = (uint32_t)bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = v.field(kWindClientIdx)) w.client_index = (int32_t)bf6__spawn_num(*f);
            w.excluded = bf6__spawn_bool(v.field(kWindExcluded));
            if (const bf6::EbxValue* f = v.field(kWindBones))
                if (f->kind == bf6::EbxValue::Kind::Array) w.bone_count = (int32_t)f->items.size();
            if (const bf6::EbxValue* f = v.field(kWindXform))
                w.has_transform = bf6__spawn_xform(*f, w.transform) ? 1u : 0u;
            h->comps.push_back(w);
            continue;
        }

        bf6_vector_field vfd{};
        vfd.is_entity = vf ? 1u : 0u;
        if (const bf6::EbxValue* f = v.field(kVfMagScale))  vfd.magnitude_scale = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kVfMagnitude)) vfd.magnitude = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kVfBottomPct)) vfd.bottom_radius_pct = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kVfTags))      vfd.tags = (uint32_t)bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kWindXform))
            vfd.has_transform = bf6__spawn_xform(*f, vfd.transform) ? 1u : 0u;
        vfd.name_index = -1;
        if (const bf6::EbxValue* f = v.field(kVfName)) {
            if (f->kind == bf6::EbxValue::Kind::Str && !f->s.empty()) {
                h->names.push_back(f->s);
                vfd.name_index = (int32_t)h->names.size() - 1;
            }
        }
        h->fields.push_back(vfd);
    }

    h->s.component_count = (int32_t)h->comps.size();
    h->s.components      = h->comps.empty() ? nullptr : h->comps.data();
    h->s.field_count     = (int32_t)h->fields.size();
    h->s.fields          = h->fields.empty() ? nullptr : h->fields.data();
    c->handles[&h->s] = bf6_ctx::HK_WIND;
    return &h->s;
}

void bf6__wind_delete(void* p) { delete (WindHandle*)p; }
