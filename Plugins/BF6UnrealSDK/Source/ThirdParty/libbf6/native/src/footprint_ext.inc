/* FOOTPRINTS: the battle-royale procedural placement system.
 *
 * NOT snow or sand deformation - that is a separate system sharing the word
 * (`sandfootprints` terrain textures and `fx_footprints_receive` emitters). A
 * "footprint" here is a level-design footprint: a reserved slot with a category
 * that the runtime fills from a weighted prefab database.
 *
 * Two readers, because the system splits into placements and vocabulary:
 *
 *   PLACEMENTS  `FootprintEntityData` 8da4488c-4c2a-06e6-d2f2-a523e0606d47
 *               on a level partition. A transform, a Category reference and
 *               the wall/interior constraints.
 *
 *   CATEGORIES  `granite_footprint_size.ebx` and its siblings: one root
 *               (d905947b) holding an array of 79 category instances
 *               (7df601a4), each a DebugName - LootLocation_Chest_Rare,
 *               Vehicle_Heli_UH60, MissionObject_CTFDeposit and so on.
 *
 * Unlike spawn points, a placement is NOT yaw-only: 18 of 26 measured are, the
 * rest carry pitch and roll, and they spread over 52 m of height. These sit on
 * the surface they are placed against, walls included, which is consistent
 * with the type having NeedWall and IsNextToWall as concepts.
 *
 * `NameHash` on a category is 0 in shipped data. Where it IS populated
 * elsewhere in this system it is djb2-XOR of the DebugName with basis 5381 -
 * see bf6_name_hash below. Treat a stored 0 as "not baked" and hash the name. */

static const uint32_t kFpFlags     = 0x5645e663u;
static const uint32_t kFpXform     = 0xd6351edeu;
static const uint32_t kFpPrefabOvr = 0x8d7a921fu;  /* FootprintPrefabOverride */
static const uint32_t kFpCategory  = 0x18d8777eu;  /* Category                */
static const uint32_t kFpSuppress  = 0x032a545bu;  /* SuppressionType         */
static const uint32_t kFpNextWall  = 0x24adffbcu;  /* IsNextToWall            */
static const uint32_t kFpInterior  = 0x6b4ccc16u;  /* IsInterior              */
static const uint32_t kFpDrawDebug = 0x666021f3u;  /* DrawDebugTexturePool    */
static const uint32_t kFpDebugName = 0x820ec965u;  /* DebugName               */
static const uint32_t kFpNameHash  = 0xc124c7d8u;  /* NameHash, often 0       */

/* Mixed-endian on disk. */
static const uint8_t kFootprintType[16] = {
    0x8c,0x48,0xa4,0x8d, 0x2a,0x4c, 0xe6,0x06, 0xd2,0xf2,0xa5,0x23,0xe0,0x60,0x6d,0x47
};
static const uint8_t kFpCategoryType[16] = {
    0xa4,0x01,0xf6,0x7d, 0x95,0xd3, 0xc1,0x3f, 0xaa,0x54,0x88,0xf5,0x36,0xb5,0xed,0xd4
};

/* The engine's name hash: djb2-XOR, basis 5381, CASE-SENSITIVE. Identified on
 * 62 FootprintBiomeTheme DebugName/NameHash pairs (62 of 62), held out on a
 * second biome list (4 of 4) and on the telemetry member ids (3 of 3). FNV-1,
 * FNV-1a, plain additive djb2, sdbm and CRC32 all score 0. */
uint32_t bf6_name_hash(const char* s)
{
    uint32_t h = 5381u;
    if (!s) return h;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++)
        h = (h * 33u) ^ (uint32_t)*p;
    return h;
}

struct FootprintHandle {
    bf6_footprints                 s{};
    std::vector<bf6_footprint>     pts;
    std::vector<std::string>       names;   /* categories */
    std::vector<const char*>       nameptr;
};

bf6_footprints* bf6_footprints_read(bf6_ctx* c, const char* name)
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

    FootprintHandle* h = new FootprintHandle();

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid g = e.instance_type(i);
        const bool place = memcmp(g.data(), kFootprintType,  16) == 0;
        const bool cat   = memcmp(g.data(), kFpCategoryType, 16) == 0;
        if (!place && !cat) continue;

        bf6::EbxValue v = e.read_instance(i);

        if (cat) {
            const bf6::EbxValue* n = v.field(kFpDebugName);
            if (n && n->kind == bf6::EbxValue::Kind::Str) h->names.push_back(n->s);
            continue;
        }

        bf6_footprint p{};
        if (const bf6::EbxValue* f = v.field(kFpFlags))    p.flags = (uint32_t)f->u;
        if (const bf6::EbxValue* f = v.field(kFpSuppress)) p.suppression_type = (int32_t)bf6__spawn_num(*f);
        p.is_next_to_wall  = bf6__spawn_bool(v.field(kFpNextWall));
        p.is_interior      = bf6__spawn_bool(v.field(kFpInterior));
        p.draw_debug_pool  = bf6__spawn_bool(v.field(kFpDrawDebug));
        /* A null override means "use the database"; it is null in every sample. */
        if (const bf6::EbxValue* f = v.field(kFpPrefabOvr))
            p.has_prefab_override = (f->kind != bf6::EbxValue::Kind::Null) ? 1u : 0u;
        if (const bf6::EbxValue* f = v.field(kFpCategory))
            p.has_category = (f->kind != bf6::EbxValue::Kind::Null) ? 1u : 0u;
        if (const bf6::EbxValue* f = v.field(kFpXform))
            p.has_transform = bf6__spawn_xform(*f, p.transform) ? 1u : 0u;
        h->pts.push_back(p);
    }

    for (const std::string& n : h->names) h->nameptr.push_back(n.c_str());

    h->s.count           = (int32_t)h->pts.size();
    h->s.points          = h->pts.empty() ? nullptr : h->pts.data();
    h->s.category_count  = (int32_t)h->nameptr.size();
    h->s.categories      = h->nameptr.empty() ? nullptr : h->nameptr.data();
    c->handles[&h->s] = bf6_ctx::HK_FOOTPRINTS;
    return &h->s;
}

void bf6__footprints_delete(void* p) { delete (FootprintHandle*)p; }
