/* ALTERNATE SPAWN POINTS: authored spawn placements on a gameplay layer.
 *
 * `AlternateSpawnEntityData` (f7fbc419-e145-394f-7086-b81c1935e8ab, size 0x80)
 * ships several hundred times per map, on the per-game-mode layer partitions
 * under `_layers_gameplay/`. All 11 of its fields were read on 247 instances
 * across mp_abbasid, mp_granite and mp_eastwood.
 *
 * Field names and offsets come from the executable's reflection table, so
 * unlike the swarm reader this one can label what it returns:
 *
 *     0x018  0x5645E663  Flags                 UInt32
 *     0x020  0xD6351EDE  Transform             LinearTransform, size 64
 *     0x060  0x54F22136  (unnamed)             Float32   - 0.1 on all 247
 *     0x064  0x2ADBF2A3  Team                  Enum TeamId
 *     0x068  0x3F680D24  InitialSpawnDelay     Float32
 *     0x06C  0x5389A5F9  (unnamed)             Struct 4
 *     0x070  0x60D18AE7  (unnamed)             Float32   - 0.01 on all 247
 *     0x074  0x666021F3  DrawDebugTexturePool  Bool
 *     0x075  0x3A6F09AD  UseAsFallback         Bool
 *     0x076  0xF97D7309  Enabled               Bool
 *     0x077  0xCF690BA2  (unnamed)             Bool
 *
 * TEAM IS ALWAYS 0. Measured on 529 instances over three levels; no shipped
 * instance selects a side. A consumer must NOT use this field to sort spawns
 * into teams - that reading was tested and failed. `Enabled` by contrast is
 * genuinely authored (49 of 124 true on eastwood conquest), so a constant
 * field here is a real measurement and not an unprinted default.
 *
 * The transform is yaw-only: row1 == (0,1,0) and row0.y == row2.y == 0 on
 * 247/247, with translations tracking level ground height. Position and facing,
 * nothing else. */

static const uint32_t kSpawnFlags     = 0x5645e663u;
static const uint32_t kSpawnXform     = 0xd6351edeu;  /* LinearTransform, 64  */
static const uint32_t kSpawnF060      = 0x54f22136u;
static const uint32_t kSpawnTeam      = 0x2adbf2a3u;  /* Enum TeamId          */
static const uint32_t kSpawnDelay     = 0x3f680d24u;  /* InitialSpawnDelay    */
static const uint32_t kSpawnF070      = 0x60d18ae7u;
static const uint32_t kSpawnDrawDebug = 0x666021f3u;  /* DrawDebugTexturePool */
static const uint32_t kSpawnFallback  = 0x3a6f09adu;  /* UseAsFallback        */
static const uint32_t kSpawnEnabled   = 0xf97d7309u;  /* Enabled              */
static const uint32_t kSpawnB077      = 0xcf690ba2u;

/* AlternateSpawnEntityData, as EBX stores a type GUID: MIXED-ENDIAN. The first
 * three groups are byte-reversed, the last eight bytes are as written. So
 * `f7fbc419-e145-394f-7086-b81c1935e8ab` is on disk:
 *
 *     19 c4 fb f7 | 45 e1 | 4f 39 | 70 86 b8 1c 19 35 e8 ab
 *
 * A GUID comparison that gets this wrong matches NOTHING and returns a clean
 * zero on every partition - which reads as "this level has no spawns", not as
 * an error. That is exactly what happened here: two typos in this array (0x94
 * for 0xc4, and a dropped 0x19) produced 0 of 493 across six partitions while
 * every other check in the test still reported green. The exact per-partition
 * counts asserted by spawn_test are the only thing that catches it. */
static const uint8_t kSpawnType[16] = {
    0x19,0xc4,0xfb,0xf7, 0x45,0xe1, 0x4f,0x39, 0x70,0x86,0xb8,0x1c,0x19,0x35,0xe8,0xab
};

struct SpawnHandle {
    bf6_spawns                   s{};
    std::vector<bf6_spawn_point> pts;
};

static float bf6__spawn_num(const bf6::EbxValue& v)
{
    return (v.kind == bf6::EbxValue::Kind::Real) ? (float)v.f
         : (v.kind == bf6::EbxValue::Kind::Int)  ? (float)v.i
         : (v.kind == bf6::EbxValue::Kind::Uint) ? (float)v.u : 0.0f;
}

static uint8_t bf6__spawn_bool(const bf6::EbxValue* v)
{
    if (!v) return 0;
    if (v->kind == bf6::EbxValue::Kind::Bool) return v->b ? 1u : 0u;
    if (v->kind == bf6::EbxValue::Kind::Uint) return v->u ? 1u : 0u;
    if (v->kind == bf6::EbxValue::Kind::Int)  return v->i ? 1u : 0u;
    return 0;
}

static bool bf6__spawn_debug_enabled()
{
#if defined(_WIN32)
    char* value = nullptr;
    size_t length = 0;
    const errno_t result = _dupenv_s(&value, &length, "BF6_SPAWN_DEBUG");
    const bool enabled = result == 0 && value != nullptr && value[0] != '\0';
    std::free(value);
    return enabled;
#else
    const char* value = std::getenv("BF6_SPAWN_DEBUG");
    return value != nullptr && value[0] != '\0';
#endif
}

/* 4 rows of 3, the same LinearTransform convention the skeleton reader uses. */
static bool bf6__spawn_xform(const bf6::EbxValue& v, float* m)
{
    if (v.kind != bf6::EbxValue::Kind::Struct || v.fields.size() < 4) return false;
    for (int r = 0; r < 4; r++) {
        const bf6::EbxValue& row = v.fields[(size_t)r].second;
        if (row.kind != bf6::EbxValue::Kind::Struct || row.fields.size() < 3) return false;
        for (int c2 = 0; c2 < 3; c2++)
            m[r * 3 + c2] = bf6__spawn_num(row.fields[(size_t)c2].second);
    }
    return true;
}

bf6_spawns* bf6_spawns_read(bf6_ctx* c, const char* name)
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

    SpawnHandle* h = new SpawnHandle();

    const bool spawn_debug = bf6__spawn_debug_enabled();
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid g = e.instance_type(i);
        if (spawn_debug && i < 12) {
            std::fprintf(stderr, "  inst %2zu type ", i);
            for (int q = 0; q < 16; q++) std::fprintf(stderr, "%02x", g[(size_t)q]);
            std::fprintf(stderr, "\n");
        }
        if (memcmp(g.data(), kSpawnType, 16) != 0) continue;

        bf6::EbxValue v = e.read_instance(i);

        bf6_spawn_point p{};
        /* Team is read and returned, but it is 0 in all shipped data measured.
         * Kept so a consumer can confirm that for itself rather than trust it. */
        if (const bf6::EbxValue* f = v.field(kSpawnTeam))  p.team  = (int32_t)bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kSpawnFlags)) p.flags = (uint32_t)bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kSpawnDelay)) p.initial_spawn_delay = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kSpawnF060))  p.unnamed_060 = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kSpawnF070))  p.unnamed_070 = bf6__spawn_num(*f);

        p.enabled          = bf6__spawn_bool(v.field(kSpawnEnabled));
        p.use_as_fallback  = bf6__spawn_bool(v.field(kSpawnFallback));
        p.draw_debug_pool  = bf6__spawn_bool(v.field(kSpawnDrawDebug));
        p.unnamed_077      = bf6__spawn_bool(v.field(kSpawnB077));

        if (const bf6::EbxValue* f = v.field(kSpawnXform))
            p.has_transform = bf6__spawn_xform(*f, p.transform) ? 1u : 0u;

        h->pts.push_back(p);
    }

    h->s.count  = (int32_t)h->pts.size();
    h->s.points = h->pts.empty() ? nullptr : h->pts.data();
    c->handles[&h->s] = bf6_ctx::HK_SPAWNS;
    return &h->s;
}

void bf6__spawns_delete(void* p) { delete (SpawnHandle*)p; }
