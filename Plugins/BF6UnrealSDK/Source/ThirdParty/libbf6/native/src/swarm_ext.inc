/* SWARM CROWDS: the SP-campaign crowd regions and their spawn points.
 *
 * A `*_area_swarm` partition holds one container instance whose child list
 * names every element, plus per-element instances. Two element shapes matter
 * to a consumer that wants to place a crowd:
 *
 *   - a SPAWN POINT, carrying a world position;
 *   - a REGION/LOGIC element, carrying a 64-byte LinearTransform and a
 *     reference to the shared swarm logic prefab.
 *
 * THE FIELD SHAPES HERE ARE NOT GUESSES. Each was confirmed against the
 * executable's own typeinfo section, where a field record points to a type
 * descriptor giving a 32-bit type hash and the type's byte size:
 *
 *     0xa4862c49  type cb53a662  size 16  Vec3            - the position
 *     0x7b554ef5  type 8efe44b3  size 64  LinearTransform - region transform
 *     0xd6351ede  type 8efe44b3  size 64  LinearTransform - element transform
 *
 * The names are NOT recoverable - retail reflection hashes them - so these are
 * confirmed SHAPES with meaning inferred from the values. A consumer gets
 * geometry it can trust and labels it cannot.
 *
 * Swarm content is SP-campaign only; a multiplayer level yields nothing, and
 * that is the correct answer rather than a failure. */

static const uint32_t kSwarmPos    = 0xa4862c49u;  /* Vec3, size 16           */
static const uint32_t kSwarmXformA = 0x7b554ef5u;  /* LinearTransform, size 64 */
static const uint32_t kSwarmXformB = 0xd6351edeu;  /* LinearTransform, size 64 */

struct SwarmHandle {
    bf6_swarm          s{};
    std::vector<float> points;   /* 3 per point  */
    std::vector<float> xforms;   /* 12 per xform */
};

/* A Vec3 arrives as a struct of three reals. Written to tolerate ints, because
 * a whole-numbered coordinate can serialise as one. */
static bool bf6__swarm_vec3(const bf6::EbxValue& v, float* out)
{
    if (v.kind != bf6::EbxValue::Kind::Struct || v.fields.size() < 3) return false;
    for (int i = 0; i < 3; i++) {
        const bf6::EbxValue& s = v.fields[(size_t)i].second;
        out[i] = (s.kind == bf6::EbxValue::Kind::Real) ? (float)s.f
               : (s.kind == bf6::EbxValue::Kind::Int)  ? (float)s.i
               : (s.kind == bf6::EbxValue::Kind::Uint) ? (float)s.u : 0.0f;
    }
    return true;
}

/* 4 rows of 3, matching the skeleton reader's LinearTransform convention. */
static bool bf6__swarm_xform(const bf6::EbxValue& v, float* m)
{
    if (v.kind != bf6::EbxValue::Kind::Struct || v.fields.size() < 4) return false;
    for (int r = 0; r < 4; r++) {
        const bf6::EbxValue& row = v.fields[(size_t)r].second;
        if (row.kind != bf6::EbxValue::Kind::Struct || row.fields.size() < 3) return false;
        for (int c2 = 0; c2 < 3; c2++) {
            const bf6::EbxValue& s = row.fields[(size_t)c2].second;
            m[r * 3 + c2] = (s.kind == bf6::EbxValue::Kind::Real) ? (float)s.f
                          : (s.kind == bf6::EbxValue::Kind::Int)  ? (float)s.i
                          : (s.kind == bf6::EbxValue::Kind::Uint) ? (float)s.u : 0.0f;
        }
    }
    return true;
}

bf6_swarm* bf6_swarm_read(bf6_ctx* c, const char* name)
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

    SwarmHandle* h = new SwarmHandle();

    /* Select on CONTENT, not on instance index: which instance carries a
     * position is not fixed across partitions. */
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::EbxValue v = e.read_instance(i);
        if (const bf6::EbxValue* p = v.field(kSwarmPos)) {
            float xyz[3];
            if (bf6__swarm_vec3(*p, xyz)) {
                h->points.insert(h->points.end(), xyz, xyz + 3);
            }
        }
        const bf6::EbxValue* x = v.field(kSwarmXformA);
        if (!x) x = v.field(kSwarmXformB);
        if (x) {
            float m[12];
            if (bf6__swarm_xform(*x, m)) {
                h->xforms.insert(h->xforms.end(), m, m + 12);
            }
        }
    }

    h->s.point_count = (int32_t)(h->points.size() / 3);
    h->s.xform_count = (int32_t)(h->xforms.size() / 12);
    h->s.points = h->points.empty() ? nullptr : h->points.data();
    h->s.xforms = h->xforms.empty() ? nullptr : h->xforms.data();
    c->handles[&h->s] = bf6_ctx::HK_SWARM;
    return &h->s;
}

void bf6__swarm_delete(void* p) { delete (SwarmHandle*)p; }
