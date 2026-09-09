/* chardeform_ext.inc - the two character deformation tables that had a decode
 * but no read path: GPU deform adjacency, and the strand-hair scalp binding.
 *
 * A translation-unit fragment like the others. Wire it beside them.
 *
 * WHY BOTH LIVE HERE. They are the same shape of problem - a variable-length or
 * slot-indexed table that a consumer cannot use without knowing a convention
 * that is not in the bytes - and both are easy to read WRONG in a way that
 * still parses:
 *
 *   - the adjacency ids are FACES, not neighbouring vertices. Reading them as
 *     vertices produces indices that look out of range against the vertex count
 *     and a symmetry score near zero, which is what made two earlier attempts
 *     discard correct data.
 *   - the hair bind resource stores only two barycentric weights; the third is
 *     implied. A consumer that reads three lands on the next strand's data.
 */

/* ------------------------------------------------------------ adjacency */

/* Header: u32 count|0x80000000, u32 dataOffset (16), u32 0, u32 0.
 * Then one variable-length record per vertex:
 *     u32 aux, u16 valence, valence x u16 faceId, 0xFFFF padding to the next.
 * The padding is why the record boundary must be SCANNED and not strided: the
 * dominant span is 36 bytes and assuming it is wrong. */
struct AdjHandle {
    bf6_deform_adjacency    a{};
    std::vector<uint32_t>   first;   /* count+1 offsets into `face` */
    std::vector<uint16_t>   face;
    std::vector<uint32_t>   aux;
};

bf6_deform_adjacency* bf6_deform_adjacency_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    const uint8_t* d = nullptr;
    const int64_t n = bf6_read_raw(c, BF6_RAW_RES, res_name, &d);
    if (n < 16 || !d) return nullptr;

    auto rd32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };
    auto rd16 = [&](size_t o) { uint16_t v; std::memcpy(&v, d + o, 2); return v; };

    const uint32_t count = rd32(0) & 0x7FFFFFFFu;
    const uint32_t base  = rd32(4);
    if (!count || count > 4000000u || base < 16 || (size_t)base > (size_t)n) return nullptr;

    AdjHandle* h = new AdjHandle();
    h->first.reserve(count + 1);
    h->aux.reserve(count);
    h->face.reserve((size_t)count * 6);   /* mean valence is ~6 on a closed mesh */

    size_t p = base;
    for (uint32_t v = 0; v < count; v++) {
        if (p + 6 > (size_t)n) { delete h; return nullptr; }
        h->first.push_back((uint32_t)h->face.size());
        h->aux.push_back(rd32(p));
        const uint16_t val = rd16(p + 4);
        p += 6;
        /* Trust the stored valence for the READ, but stop at the buffer. A
         * corrupt valence must truncate, never walk off the end. */
        uint32_t got = 0;
        for (; got < val && p + 2 <= (size_t)n; got++, p += 2) {
            const uint16_t f = rd16(p);
            if (f == 0xFFFF) break;          /* padding started early */
            h->face.push_back(f);
        }
        /* Skip the 0xFFFF run that separates this record from the next. */
        while (p + 2 <= (size_t)n && rd16(p) == 0xFFFF) p += 2;
    }
    h->first.push_back((uint32_t)h->face.size());

    h->a.vertex_count = (int32_t)count;
    h->a.first        = h->first.data();
    h->a.faces        = h->face.empty() ? nullptr : h->face.data();
    h->a.face_count   = (int32_t)h->face.size();
    h->a.aux          = h->aux.data();
    c->handles[&h->a] = bf6_ctx::HK_ADJACENCY;
    return &h->a;
}

void bf6__adjacency_delete(void* p) { delete (AdjHandle*)p; }

/* ------------------------------------------------------------ hair bind */

/* A 12-byte slot directory: (u32 count, u32 offset, u32 flag) with ABSOLUTE
 * offsets, 49 slots, payload from 0x250.
 *   slots 6..11   per-strand barycentric pair (2 x f16), one per LOD
 *   slots 24..29  per-strand scalp triangle corner triple (3 x u16), per LOD
 *   slots 36..41  per-LOD scalp vertex-pool size
 *
 * THE HIGHDEF TIER FILLS NONE OF THESE. It uses slot 48 and binds every hair
 * vertex directly, so an empty result here is a legitimate answer about that
 * asset and not a parse failure - which is why lod_count 0 is returned rather
 * than NULL. */
struct HairBindHandle {
    bf6_hair_bind          b{};
    std::vector<uint16_t>  tri;      /* 3 per strand, per lod, concatenated */
    std::vector<float>     bary;     /* 2 per strand, per lod, concatenated */
    std::vector<int32_t>   lod_first_tri, lod_first_bary, lod_strands;
};

static float bf6__half(uint16_t h)
{
    const uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF, bits;
    if (e == 0)      { if (!m) bits = s; else { e = 1; while (!(m & 0x400)) { m <<= 1; e--; }
                       m &= 0x3FF; bits = s | ((e + 112) << 23) | (m << 13); } }
    else if (e == 31) bits = s | 0x7F800000u | (m << 13);
    else              bits = s | ((e + 112) << 23) | (m << 13);
    float f; std::memcpy(&f, &bits, 4); return f;
}

bf6_hair_bind* bf6_hair_bind_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    const uint8_t* d = nullptr;
    const int64_t n = bf6_read_raw(c, BF6_RAW_RES, res_name, &d);
    if (n < 0x250 || !d) return nullptr;

    auto rd32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };
    auto rd16 = [&](size_t o) { uint16_t v; std::memcpy(&v, d + o, 2); return v; };
    auto slot = [&](int i, uint32_t& cnt, uint32_t& off) {
        const size_t e = (size_t)i * 12;
        if (e + 12 > (size_t)n) { cnt = off = 0; return false; }
        cnt = rd32(e); off = rd32(e + 4);
        return off < (size_t)n;
    };

    HairBindHandle* h = new HairBindHandle();
    for (int lod = 0; lod < 6; lod++) {
        uint32_t bc, bo, tc, to;
        if (!slot(6 + lod, bc, bo) || !slot(24 + lod, tc, to)) break;
        if (!bc || !tc) break;                     /* highdef: nothing here */
        const uint32_t strands = bc;
        if (tc != strands * 3) break;              /* the triple must match */
        if (bo + (size_t)strands * 4 > (size_t)n) break;
        if (to + (size_t)tc * 2 > (size_t)n) break;

        h->lod_first_bary.push_back((int32_t)h->bary.size());
        h->lod_first_tri.push_back((int32_t)h->tri.size());
        h->lod_strands.push_back((int32_t)strands);
        for (uint32_t s = 0; s < strands; s++) {
            h->bary.push_back(bf6__half(rd16(bo + (size_t)s * 4)));
            h->bary.push_back(bf6__half(rd16(bo + (size_t)s * 4 + 2)));
        }
        for (uint32_t s = 0; s < tc; s++) h->tri.push_back(rd16(to + (size_t)s * 2));
    }

    h->b.lod_count      = (int32_t)h->lod_strands.size();
    h->b.lod_strands    = h->lod_strands.empty() ? nullptr : h->lod_strands.data();
    h->b.lod_first_tri  = h->lod_first_tri.empty() ? nullptr : h->lod_first_tri.data();
    h->b.lod_first_bary = h->lod_first_bary.empty() ? nullptr : h->lod_first_bary.data();
    h->b.tri            = h->tri.empty() ? nullptr : h->tri.data();
    h->b.bary           = h->bary.empty() ? nullptr : h->bary.data();
    c->handles[&h->b] = bf6_ctx::HK_HAIRBIND;
    return &h->b;
}

void bf6__hairbind_delete(void* p) { delete (HairBindHandle*)p; }

/* ------------------------------------------- pose-space deformation ------ */

/* The per-character PSD payload (RES type 1000150664): which vertices each
 * facial pose displaces, and by how much.
 *
 * THE STORAGE IS SPARSE, which is what three earlier attempts missed. Only
 * vertices a pose actually moves are stored, so no fixed stride divides the
 * payload and no dense N*V array exists to find - a dense one would be larger
 * than the whole file.
 *
 * Layout:
 *   +0x00  u32   opaque
 *   +0x04  u32   constant per mesh part
 *   +0x08  u32   a block size
 *   +0x0C  u32   V, the LOD vertex count
 *   +0x10  u32   opaque
 *   +0x14  [2N, N, 0, N, 2N]   N = pose count
 *   +0x28  f32   THE QUANTISATION SCALE (0.01). Not an epsilon or a tolerance -
 *                one 10-bit signed delta lane spans +/- this value.
 *   +0x40  108-byte pose-descriptor records, one per pose
 *   then   324*N + 24 bytes of part-constant metadata
 *   so the sparse index table begins at 88 + 432*N:
 *          {u32 0xFFFFFFFF, u32 vertexIndex, u32 cumulativeDwordCount}
 *          one record per AFFECTED vertex, ending at a (0xFFFFFFFF, 0, 0)
 *          sentinel followed by two constant dwords
 *   then   the dense payload, per affected vertex:
 *          u32 k, then k pairs of (u32 A, u32 B)
 *
 * A packs the delta as XYZ10_W2 signed fixed point, LSB first, scaled by
 * scale/512. B's high half is 2*poseIndex + 1.
 *
 * NOTE the 324*N + 24 term. An earlier reading recorded that block as a
 * constant 1,320 bytes, which is only true for N == 4; using the constant lands
 * mid-structure on every clip with a different pose count.
 */

struct PsdHandle {
    bf6_psd                    p{};
    std::vector<bf6_psd_delta> deltas;
};

static inline int bf6__s10(uint32_t v) { return (v & 0x200u) ? (int)v - 1024 : (int)v; }

bf6_psd* bf6_psd_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    const uint8_t* d = nullptr;
    const int64_t n = bf6_read_raw(c, BF6_RAW_RES, res_name, &d);
    if (n < 0x40 || !d) return nullptr;

    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };
    auto f32 = [&](size_t o) { float v;    std::memcpy(&v, d + o, 4); return v; };

    const uint32_t V = u32(0x0C);
    const uint32_t N = u32(0x18);          /* the N of [2N, N, 0, N, 2N] */
    const float scale = f32(0x28);
    if (!V || !N || N > 4096u || u32(0x14) != 2u * N) return nullptr;

    const uint64_t table = 88ull + 432ull * N;
    if (table + 12 > (uint64_t)n) return nullptr;

    PsdHandle* h = new PsdHandle();

    /* Walk the sparse index table to its sentinel, collecting (vertex, count).
     * The cumulative dword count is what locates each vertex's record in the
     * dense block, so it is kept rather than just the vertex id. */
    std::vector<std::pair<uint32_t,uint32_t>> idx;   /* vertex, cumulative */
    /* THE HEADER'S deformedVertexCount AT +0x10 IS THE RECORD COUNT, and it is
     * the authority - scanning for a terminator is not.
     *
     * Some payloads end with a (vertex 0, small) record and some do not, and
     * the header COUNTS that record either way. Treating vertex 0 as a
     * terminator without also honouring the declared count silently drops one
     * real record on the files that lack it; scanning until the marker breaks
     * without the count over-reads on the files that have it. Reading exactly
     * `declared` records and skipping a vertex-0 entry as non-data satisfies
     * both, and every marker being present over that span is the check that the
     * table really is where it was expected. */
    const uint32_t declared = u32(0x10);
    uint64_t p = table;
    uint32_t markers_ok = 0;
    for (uint32_t r = 0; r < declared && p + 12 <= (uint64_t)n; r++, p += 12) {
        const uint32_t marker = u32((size_t)p);
        const uint32_t vert   = u32((size_t)p + 4);
        const uint32_t cum    = u32((size_t)p + 8);
        if (marker != 0xFFFFFFFFu) break;
        markers_ok++;
        if (vert == 0) continue;                    /* terminator, still counted */
        if (vert >= V) { delete h; return nullptr; }
        idx.push_back({vert, cum});
    }
    if (markers_ok != declared) { delete h; return nullptr; }

    if (idx.empty()) { delete h; return nullptr; }

    /* LOCATE THE DENSE BLOCK BY SELF-CHECK rather than by a fixed pad.
     * The gap between the terminator and the payload is not constant across
     * files - 4 bytes on one, 8 on another - so the start is found by asking
     * which candidate makes the first record's STORED k equal the k derived
     * from the cumulative column. That is a two-source agreement, so a wrong
     * offset cannot satisfy it by luck. */
    const uint64_t first_span = idx[0].second;
    uint64_t dense_at = 0;
    if (first_span >= 3 && (first_span & 1u)) {
        const uint32_t want = (uint32_t)((first_span - 1) / 2);
        for (uint32_t pad = 0; pad <= 16; pad += 4) {
            const uint64_t cand = p + pad;
            if (cand + 4 > (uint64_t)n) break;
            if (u32((size_t)cand) == want) { dense_at = cand; break; }
        }
    }
    if (!dense_at) { delete h; return nullptr; }
    p = dense_at;

    /* The dense block. Each vertex's record is 1 + 2k dwords, and the cumulative
     * column gives the running dword total, so the record for entry i starts at
     * the previous cumulative value. */
    const uint64_t dense = p;
    h->deltas.reserve(idx.size() * 2);
    for (size_t i = 0; i < idx.size(); i++) {
        const uint64_t prev = i ? idx[i-1].second : 0ull;
        const uint64_t start = dense + 4ull * prev;
        if (start + 4 > (uint64_t)n) break;
        /* DERIVE k FROM THE CUMULATIVE COLUMN, and use the stored value only as
         * a cross-check. The cumulative differences are what actually locate
         * each record, so trusting the stored k first means one bad record
         * desynchronises everything after it - and a k of 0 silently drops the
         * whole file rather than reporting a problem. */
        const uint64_t span = idx[i].second - prev;       /* dwords, == 1 + 2k */
        if (span < 3 || (span & 1u) == 0u) continue;
        const uint32_t k = (uint32_t)((span - 1) / 2);
        if (k > N) continue;                              /* k never exceeds N */
        const uint32_t stored_k = u32((size_t)start);
        if (stored_k != k) continue;                      /* the cross-check */
        if (start + 4 + 8ull * k > (uint64_t)n) break;
        for (uint32_t j = 0; j < k; j++) {
            const uint32_t A = u32((size_t)start + 4 + 8ull*j);
            const uint32_t B = u32((size_t)start + 8 + 8ull*j);
            bf6_psd_delta e{};
            e.vertex_index = (int32_t)idx[i].first;
            /* B's high half is 2*poseIndex + 1 - always the odd slot of the
             * (2i, 2i+1) pairing the pose-descriptor table also uses. */
            const uint32_t hi = B >> 16;
            e.pose_index = (int32_t)((hi - 1u) / 2u);
            const float q = scale / 512.0f;
            e.delta[0] = (float)bf6__s10( A         & 0x3FFu) * q;
            e.delta[1] = (float)bf6__s10((A >> 10)  & 0x3FFu) * q;
            e.delta[2] = (float)bf6__s10((A >> 20)  & 0x3FFu) * q;
            h->deltas.push_back(e);
        }
    }

    h->p.vertex_count = (int32_t)V;
    h->p.pose_count   = (int32_t)N;
    h->p.scale        = scale;
    h->p.affected_vertex_count = (int32_t)idx.size();
    /* CROSS-CHECK against the header's own deformedVertexCount at +0x10: the
     * number of table records must equal the count the header declares. Two
     * independent sources agreeing is what says the table was walked correctly
     * and its terminator found in the right place. */
    h->p.declared_deformed_count = (int32_t)declared;
    h->p.deltas       = h->deltas.data();
    h->p.delta_count  = (int32_t)h->deltas.size();
    c->handles[&h->p] = bf6_ctx::HK_PSD;
    return &h->p;
}

void bf6__psd_delete(void* p) { delete (PsdHandle*)p; }

/* --------------------------------------- the PSD vertex map (index half) -- */

/* PSDMeshResource: source render vertex -> welded PSD vertex.
 *
 * THIS IS THE MISSING LINK between a mesh and its facial deformation. The delta
 * payload is keyed by PSD VERTEX, and a renderer has RENDER vertices. Without
 * this map the deltas cannot be applied to anything.
 *
 * The mapping is MANY-TO-ONE by design: render vertices split at UV and normal
 * seams, and several split copies collapse onto one deformation vertex, so
 * duplicate ids are correct rather than a sign of a bad read. 0xFFFFFFFF marks
 * a render vertex no pose deforms, and it is common - about 7% of elements
 * across the shipped population - so a consumer must skip it rather than treat
 * it as vertex 4294967295.
 *
 *   +0x00 u32 elementCount        source render vertices in this LOD
 *   +0x04 u32 meshVertexOffset    cumulative source offset of this LOD
 *   +0x08 u32 mapping id
 *   +0x0C u32 dataOffset == 32
 *   +0x20 u32 psdVertexId[elementCount]
 */

struct PsdMapHandle {
    bf6_psd_map           m{};
    std::vector<uint32_t> ids;
};

bf6_psd_map* bf6_psd_map_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    const uint8_t* d = nullptr;
    const int64_t n = bf6_read_raw(c, BF6_RAW_RES, res_name, &d);
    if (n < 32 || !d) return nullptr;

    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, d + o, 4); return v; };
    const uint32_t count = u32(0x00);
    const uint32_t off   = u32(0x04);
    const uint32_t dof   = u32(0x0C);
    /* dataOffset is 32 on every shipped payload; anything else is not this
     * resource and must not be forced into the layout. */
    if (dof != 32u || !count || (uint64_t)32 + 4ull*count > (uint64_t)n) return nullptr;

    PsdMapHandle* h = new PsdMapHandle();
    h->ids.resize(count);
    std::memcpy(h->ids.data(), d + 32, (size_t)count * 4);

    int32_t mapped = 0; uint32_t mx = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (h->ids[i] == 0xFFFFFFFFu) continue;
        mapped++;
        if (h->ids[i] > mx) mx = h->ids[i];
    }
    h->m.element_count      = (int32_t)count;
    h->m.mesh_vertex_offset = (int32_t)off;
    h->m.psd_vertex_id      = h->ids.data();
    h->m.mapped_count       = mapped;
    h->m.max_psd_vertex     = (int32_t)mx;
    c->handles[&h->m] = bf6_ctx::HK_PSDMAP;
    return &h->m;
}

void bf6__psdmap_delete(void* p) { delete (PsdMapHandle*)p; }
