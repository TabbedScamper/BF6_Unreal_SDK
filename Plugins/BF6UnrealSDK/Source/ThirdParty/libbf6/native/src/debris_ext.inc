/* DEBRIS: authored clusters, and the material-relation markers.
 *
 * Debris is overwhelmingly a MATERIAL PROPERTY, not placed content. Across 28
 * shipped levels there are 2 `DebrisClusterData` (both on mp_subsurface), 2
 * `DebrisCollisionComponentData`, 3 `DefaultDebrisDynamicSpawnCallback` and
 * ZERO `StaticDebrisArea*`. What ships on 27 of 28 levels is
 * `MaterialRelationDebrisData` - exactly 11 per level, inside
 * `materialgrid_win32`.
 *
 *     a8ddbbc6  DebrisClusterData          0xE0, 30 fields
 *     1d1fe594  DebrisClusterPartInfoData  0x40,  9 fields
 *     78ff1a23  MaterialRelationDebrisData 0x18,  2 fields at UNRESOLVED offsets
 *
 * TWO THINGS A CONSUMER WILL GET WRONG:
 *
 *   1. `LinearVelocity` IS A DIRECTION, NOT A SPEED. Its magnitude is 1.00000
 *      on 5 of 5 parts measured. Treating it as a velocity gives every piece of
 *      debris a speed of 1. The magnitude comes from elsewhere - the material's
 *      impulse is the obvious candidate and is NOT yet read.
 *
 *   2. A MATERIAL RELATION DECODES TO ZERO FIELDS, and that is a property of
 *      the grid, not missing data. 7,737 of 7,833 grid instances across 52
 *      types decode to nothing, every MaterialRelation*Data among them, and the
 *      reflection table puts their fields at offset 0xFFFF. The payload very
 *      likely lives in MaterialGridData's InteractionGrid, addressed by grid
 *      position. This reader COUNTS the markers and does not pretend to read
 *      values it cannot. */

static const uint32_t kDbMaxActive  = 0x5bc93c45u;  /* MaxActivePartsCount */
static const uint32_t kDbHeightLim  = 0x76d668d9u;  /* HeightLimit         */
static const uint32_t kDbParts      = 0xb9734fedu;  /* PartHierarchy       */
static const uint32_t kDbPartIndex  = 0x65095170u;
static const uint32_t kDbLinVel     = 0xf69563efu;  /* a UNIT direction    */
static const uint32_t kDbAngVel     = 0xb5f6a9ffu;
static const uint32_t kDbDelay      = 0x648eb569u;
static const uint32_t kDbDelayMode  = 0xfd7450f5u;

static const uint8_t kDebrisClusterType[16] = {
    0xc6,0xbb,0xdd,0xa8, 0x1d,0xc3, 0xcb,0x24, 0x75,0x9f,0xfa,0xcf,0xf6,0xb3,0x5e,0xa1
};
static const uint8_t kMatRelDebrisType[16] = {
    0x23,0x1a,0xff,0x78, 0x6d,0x21, 0x58,0x58, 0x1e,0xbd,0xad,0x73,0x38,0x1a,0x08,0xb2
};

struct DebrisHandle {
    bf6_debris                  s{};
    std::vector<bf6_debris_part> parts;
};

bf6_debris* bf6_debris_read(bf6_ctx* c, const char* name)
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

    DebrisHandle* h = new DebrisHandle();

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid g = e.instance_type(i);
        const bool cluster = memcmp(g.data(), kDebrisClusterType, 16) == 0;
        const bool marker  = memcmp(g.data(), kMatRelDebrisType,  16) == 0;
        if (marker) { h->s.material_relation_markers++; continue; }
        if (!cluster) continue;

        bf6::EbxValue v = e.read_instance(i);
        h->s.clusters++;
        if (const bf6::EbxValue* f = v.field(kDbMaxActive))
            h->s.max_active_parts = (int32_t)bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kDbHeightLim))
            h->s.height_limit = bf6__spawn_num(*f);

        const bf6::EbxValue* a = v.field(kDbParts);
        if (!a || a->kind != bf6::EbxValue::Kind::Array) continue;
        for (const bf6::EbxValue& el : a->items) {
            if (el.kind != bf6::EbxValue::Kind::Struct) continue;
            bf6_debris_part p{};
            p.part_index = -1;
            if (const bf6::EbxValue* f = el.field(kDbPartIndex)) p.part_index = (int32_t)bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = el.field(kDbDelay))     p.delay      = bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = el.field(kDbDelayMode)) p.delay_mode = (int32_t)bf6__spawn_num(*f);
            if (const bf6::EbxValue* f = el.field(kDbLinVel)) bf6__swarm_vec3(*f, p.direction);
            if (const bf6::EbxValue* f = el.field(kDbAngVel)) bf6__swarm_vec3(*f, p.angular);
            h->parts.push_back(p);
        }
    }

    h->s.part_count = (int32_t)h->parts.size();
    h->s.parts      = h->parts.empty() ? nullptr : h->parts.data();
    c->handles[&h->s] = bf6_ctx::HK_DEBRIS;
    return &h->s;
}

void bf6__debris_delete(void* p) { delete (DebrisHandle*)p; }
