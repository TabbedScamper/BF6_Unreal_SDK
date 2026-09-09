/* OccluderMesh - hand-authored low-poly occlusion geometry (RES 0x30B4A553).
 * Ported from findings/occluder-entities-decoded.md.
 *
 * WHY THIS ONE MATTERS. Draw calls are the measured root of the rebuild's
 * frame cost, and this is the system the artists used to cut them. Every prop
 * that ships an occluder mesh carries a cheap stand-in for its own silhouette.
 *
 * The header is SELF-CHECKING in a way worth exploiting: vertexOffset and
 * indexOffset are stored AND derivable from the two u16 counts, so a correct
 * reader can predict both and compare. A layout error breaks the prediction on
 * every file rather than producing plausible geometry.
 */

static const uint32_t kOccluderMeshResType = 0x30b4a553u;

struct OccHandle {
    bf6_occluder_mesh  m{};
    std::vector<float> verts;   /* xyz, w dropped */
    std::vector<uint16_t> idx;
};

bf6_occluder_mesh* bf6_occluder_mesh_read(bf6_ctx* c, const char* res_name)
{
    if (!c || !res_name) return nullptr;
    std::string err;
    std::vector<uint8_t> b = c->src.get_res(res_name, err);
    if (b.size() < 64) return nullptr;
    const size_t n = b.size();

    auto u16 = [&](size_t o) { return (uint16_t)((uint32_t)b[o] | ((uint32_t)b[o+1] << 8)); };
    auto u32 = [&](size_t o) {
        return (uint32_t)b[o] | ((uint32_t)b[o+1] << 8) |
               ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24);
    };
    auto f32 = [&](size_t o) { const uint32_t v = u32(o); float f; memcpy(&f, &v, 4); return f; };

    OccHandle* h = new OccHandle();
    for (int k = 0; k < 3; k++) h->m.aabb_a[k] = f32((size_t)k * 4);
    for (int k = 0; k < 3; k++) h->m.aabb_b[k] = f32(0x10 + (size_t)k * 4);
    const uint16_t vc = u16(0x20), ic = u16(0x22);
    const uint32_t vo = u32(0x24), io = u32(0x28), so = u32(0x2C);
    h->m.vertex_count = vc;
    h->m.index_count  = ic;
    h->m.scratch_at   = so;
    h->m.flag         = u32(0x30);

    /* THE PREDICTION. scratch is 24 bytes per triangle with the triangle count
     * rounded UP TO AN EVEN NUMBER, so both offsets follow from the counts. */
    const uint32_t tris = ic / 3u;
    const uint32_t tris_even = (tris + 1u) & ~1u;
    const uint32_t pred_vo = 64u + tris_even * 24u;
    const uint32_t pred_io = pred_vo + (uint32_t)vc * 16u;
    h->m.predicted_vertex_offset = pred_vo;
    h->m.predicted_index_offset  = pred_io;
    h->m.stored_vertex_offset    = vo;
    h->m.stored_index_offset     = io;
    h->m.offsets_predicted       = (pred_vo == vo && pred_io == io) ? 1 : 0;

    if ((ic % 3u) != 0u || vo + (size_t)vc * 16 > n || io + (size_t)ic * 2 > n) {
        /* A misread header must yield nothing rather than floats taken from a
         * scratch block - the scratch is zero-filled on 282 of 324 files, so
         * bad geometry here would look like a degenerate mesh, not an error. */
        h->m.vertex_count = 0; h->m.index_count = 0;
    } else {
        h->verts.reserve((size_t)vc * 3);
        for (uint32_t k = 0; k < vc; k++) {
            const size_t o = vo + (size_t)k * 16;
            h->verts.push_back(f32(o));
            h->verts.push_back(f32(o + 4));
            h->verts.push_back(f32(o + 8));
        }
        h->idx.reserve(ic);
        for (uint32_t k = 0; k < ic; k++) h->idx.push_back(u16(io + (size_t)k * 2));
    }
    h->m.vertices = h->verts.empty() ? nullptr : h->verts.data();
    h->m.indices  = h->idx.empty()   ? nullptr : h->idx.data();
    c->handles[&h->m] = bf6_ctx::HK_OCCLUDER;
    return &h->m;
}

void bf6__occluder_delete(void* p) { delete (OccHandle*)p; }
