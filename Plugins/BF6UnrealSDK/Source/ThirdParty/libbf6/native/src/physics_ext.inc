/* PhysicsResource - authored collision. 669,966 resources, the second largest
 * type in the game. Ported from formats/PHYSICS_COLLISION.md sections 2-4.
 *
 * TWO TRAPS, both of which produce PLAUSIBLE OUTPUT when you fall into them,
 * which is why both are asserted in the test rather than merely commented:
 *
 *   1. OFFSETS ARE SELF-RELATIVE - `fieldAddress + storedValue`, never
 *      `payloadBase + storedValue`. The header's first region offset resolves
 *      to 0x38 under BOTH readings, so a base-relative parser passes the
 *      obvious spot check and then reads vertices out of a descriptor region.
 *      Floats come out. Geometry draws. It is wrong.
 *
 *   2. THE SHAPE INDEX IS AT +0x38, NOT +0x24. The +0x24 field is small,
 *      non-negative and less than the shape count on any asset with a handful
 *      of shapes, so it passes for the index on props and only betrays itself
 *      at scale: on one mp_dumbo group it takes 6 distinct values across
 *      47,693 instances referencing 9,681 shapes. Reading it as the index
 *      collapses a level's whole collision world onto six shapes, and the
 *      geometry still draws.
 *
 * The container is SELF-CHECKING and this reader exploits that: five monotonic
 * bounds, and every region must satisfy `span == count * stride` EXACTLY. Four
 * simultaneous equations cannot be satisfied by luck, so any violation is a
 * hard parse error with no fallback.
 */

static const uint32_t kPhysMagic = 0x62feef49u;

static inline uint32_t phys_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t phys_u16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static inline float phys_f32(const uint8_t* p) {
    const uint32_t v = phys_u32(p); float f; memcpy(&f, &v, 4); return f;
}

/* SELF-RELATIVE resolve. `at` is the offset OF THE FIELD; the stored value is
 * added to that, not to the payload base. Returns -1 when the result leaves
 * the payload. */
static int64_t phys_rel(const uint8_t* b, size_t n, size_t at)
{
    if (at + 4 > n) return -1;
    const int64_t t = (int64_t)at + (int64_t)(int32_t)phys_u32(b + at);
    return (t < 0 || (uint64_t)t > n) ? -1 : t;
}

struct PhysHandle {
    bf6_physics                  p{};
    std::vector<bf6_phys_body>   bodies;
    std::vector<bf6_phys_shape>  shapes;
    std::vector<bf6_phys_inst>   insts;
    std::vector<float>           verts;    /* all shapes' vertices, packed */
    std::vector<uint16_t>        idx;      /* all shapes' indices, packed  */
    std::vector<uint16_t>        starts;   /* all shapes' face starts      */
};

bf6_physics* bf6_physics_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    std::string err;
    std::vector<uint8_t> b = c->src.get_res(res_name, err);
    if (b.size() < 0x38) return nullptr;
    const size_t n = b.size();

    if (phys_u32(b.data() + 0x04) != kPhysMagic) return nullptr;

    const uint32_t bodyCount  = phys_u32(b.data() + 0x08);
    const uint32_t instCount  = phys_u32(b.data() + 0x0C);
    const uint32_t shapeCount = phys_u32(b.data() + 0x18);
    const uint32_t regCCount  = phys_u32(b.data() + 0x1C);

    const int64_t ra = phys_rel(b.data(), n, 0x24);
    const int64_t rb = phys_rel(b.data(), n, 0x28);
    const int64_t rc = phys_rel(b.data(), n, 0x2C);
    const int64_t rd = phys_rel(b.data(), n, 0x30);
    const int64_t gb = phys_rel(b.data(), n, 0x34);
    if (ra < 0 || rb < 0 || rc < 0 || rd < 0 || gb < 0) return nullptr;

    /* Monotonic bounds AND all four span equations, exactly. */
    if (!(ra <= rb && rb <= rc && rc <= rd && rd <= gb)) return nullptr;
    if (rb - ra != (int64_t)bodyCount  * 76) return nullptr;
    if (rc - rb != (int64_t)shapeCount * 72) return nullptr;
    if (rd - rc != (int64_t)regCCount  * 44) return nullptr;
    if (gb - rd != (int64_t)instCount  * 72) return nullptr;

    const uint8_t* b_ptr = b.data();
    PhysHandle* h = new PhysHandle();
    h->p.flags        = phys_u32(b.data() + 0x00);
    h->p.body_count   = (int32_t)bodyCount;
    h->p.region_c     = (int32_t)regCCount;
    h->p.geometry_at  = (int64_t)gb;
    h->p.payload_size = (int64_t)n;

    /* ---- region B: shapes ------------------------------------------------ */
    h->shapes.reserve(shapeCount);
    for (uint32_t i = 0; i < shapeCount; i++) {
        const size_t r = (size_t)rb + (size_t)i * 72;
        bf6_phys_shape s{};
        s.vertex_count = phys_u16(b.data() + r + 0x18);
        s.index_count  = phys_u16(b.data() + r + 0x1A);
        /* +0x1C IS A u16 PAIR. Read as a u32 the midphase count bleeds into
         * the top half and FaceCount reads in the millions, which is what made
         * this field look unreliable for a long time. */
        s.face_count      = phys_u16(b.data() + r + 0x1C);
        s.midphase_nodes  = phys_u16(b.data() + r + 0x1E);
        s.plane_ptr       = phys_u32(b.data() + r + 0x40) != 0 ? 1 : 0;
        s.mesh_ptr        = phys_u32(b.data() + r + 0x44) != 0 ? 1 : 0;
        s.vertex_first = s.index_first = s.face_start_first = -1;

        /* Vertices, indices and face starts must ALL resolve inside the
         * geometry blob and fit. A table that resolves before the blob, or
         * overruns, is a misread - yield nothing rather than floats decoded
         * out of a descriptor region. */
        const int64_t vo = phys_rel(b.data(), n, r + 0x20);
        if (vo >= gb && s.vertex_count &&
            vo + (int64_t)s.vertex_count * 12 <= (int64_t)n) {
            s.vertex_first = (int32_t)(h->verts.size() / 3);
            for (uint32_t k = 0; k < s.vertex_count; k++) {
                const uint8_t* v = b.data() + vo + (size_t)k * 12;
                h->verts.push_back(phys_f32(v));
                h->verts.push_back(phys_f32(v + 4));
                h->verts.push_back(phys_f32(v + 8));
            }
        } else {
            s.vertex_count = 0;
        }

        const int64_t io = phys_rel(b.data(), n, r + 0x2C);
        if (io >= gb && s.index_count &&
            io + (int64_t)s.index_count * 2 <= (int64_t)n) {
            s.index_first = (int32_t)h->idx.size();
            for (uint32_t k = 0; k < s.index_count; k++)
                h->idx.push_back(phys_u16(b.data() + io + (size_t)k * 2));
        } else {
            s.index_count = 0;
        }

        /* Face i spans Indices[FaceStarts[i] .. FaceStarts[i+1]), so the table
         * has FaceCount + 1 entries. Faces have MIXED ARITY - boxes are
         * all-quad, big shapes mix triangles through 9-gons - so IndexCount
         * cannot be split into faces without this. */
        const int64_t fo = phys_rel(b.data(), n, r + 0x3C);
        if (fo >= gb && s.face_count &&
            fo + ((int64_t)s.face_count + 1) * 2 <= (int64_t)n) {
            s.face_start_first = (int32_t)h->starts.size();
            for (uint32_t k = 0; k <= s.face_count; k++)
                h->starts.push_back(phys_u16(b.data() + fo + (size_t)k * 2));
        } else {
            s.face_count = 0;
        }
        h->shapes.push_back(s);
    }


    /* ---- region A: rigid bodies (76 bytes) - spec section 5.1 ------------ */
    /* A body is its world transform AT THE CENTRE OF MASS, its mass
     * properties and its surface material. Merged static-model groups carry
     * NONE, because static geometry has no mass - so bodyCount 0 is normal and
     * is not a misread. Three fields are -1 to mean "take the world default",
     * which a reader must not clamp away. */
    h->bodies.reserve(bodyCount);
    for (uint32_t i = 0; i < bodyCount; i++) {
        const size_t r = (size_t)ra + (size_t)i * 76;
        bf6_phys_body body{};
        for (int k = 0; k < 4; k++) body.quat[k] = phys_f32(b_ptr + r + (size_t)k * 4);
        for (int k = 0; k < 3; k++) body.inv_inertia[k] = phys_f32(b_ptr + r + 0x10 + (size_t)k * 4);
        for (int k = 0; k < 3; k++) body.center_of_mass[k] = phys_f32(b_ptr + r + 0x1C + (size_t)k * 4);
        body.reciprocal_mass  = phys_f32(b_ptr + r + 0x28);
        body.motion_type      = phys_u32(b_ptr + r + 0x2C);
        body.group_index      = phys_u32(b_ptr + r + 0x30);   /* u32, NOT a float */
        body.static_friction  = phys_f32(b_ptr + r + 0x34);
        body.dynamic_friction = phys_f32(b_ptr + r + 0x38);
        body.restitution      = phys_f32(b_ptr + r + 0x3C);
        body.linear_drag      = phys_f32(b_ptr + r + 0x40);   /* f32, NOT a u32 */
        body.angular_drag     = phys_f32(b_ptr + r + 0x44);
        body.keyframed_contacts = b_ptr[r + 0x48];
        body.autosleep          = b_ptr[r + 0x49];
        body.start_asleep       = b_ptr[r + 0x4A];
        h->bodies.push_back(body);
    }

    /* ---- region D: instances --------------------------------------------- */
    h->insts.reserve(instCount);
    for (uint32_t i = 0; i < instCount; i++) {
        const size_t r = (size_t)rd + (size_t)i * 72;
        bf6_phys_inst d{};
        for (int k = 0; k < 4; k++) d.quat[k]  = phys_f32(b.data() + r + (size_t)k * 4);
        for (int k = 0; k < 3; k++) d.pos[k]   = phys_f32(b.data() + r + 0x10 + (size_t)k * 4);
        /* +0x1C is a UNION: uniform scale for hull/mesh kinds, the capsule
         * HALF-HEIGHT for kind 1. */
        d.scale_or_halfheight = phys_f32(b.data() + r + 0x1C);
        d.radius              = phys_f32(b.data() + r + 0x20);
        d.shape_type          = phys_u32(b.data() + r + 0x24);
        d.preset_a            = phys_u32(b.data() + r + 0x28);
        d.preset_b            = phys_u32(b.data() + r + 0x2C);
        d.preset_c            = phys_u32(b.data() + r + 0x30);
        d.object_id           = phys_u32(b.data() + r + 0x34);
        d.shape_index         = (int32_t)phys_u32(b.data() + r + 0x38);  /* NOT +0x24 */
        d.body_index          = phys_u32(b.data() + r + 0x3C);
        d.material_packed     = phys_u32(b.data() + r + 0x40);
        d.trap_0x24           = phys_u32(b.data() + r + 0x24);
        h->insts.push_back(d);
    }

    h->p.body_rec_count = (int32_t)h->bodies.size();
    h->p.bodies      = h->bodies.empty() ? nullptr : h->bodies.data();
    h->p.shape_count = (int32_t)h->shapes.size();
    h->p.shapes      = h->shapes.empty() ? nullptr : h->shapes.data();
    h->p.inst_count  = (int32_t)h->insts.size();
    h->p.instances   = h->insts.empty() ? nullptr : h->insts.data();
    h->p.vertex_total = (int32_t)(h->verts.size() / 3);
    h->p.vertices     = h->verts.empty() ? nullptr : h->verts.data();
    h->p.index_total  = (int32_t)h->idx.size();
    h->p.indices      = h->idx.empty() ? nullptr : h->idx.data();
    h->p.face_start_total = (int32_t)h->starts.size();
    h->p.face_starts  = h->starts.empty() ? nullptr : h->starts.data();
    c->handles[&h->p] = bf6_ctx::HK_PHYSICS;
    return &h->p;
}

void bf6__physics_delete(void* p) { delete (PhysHandle*)p; }
