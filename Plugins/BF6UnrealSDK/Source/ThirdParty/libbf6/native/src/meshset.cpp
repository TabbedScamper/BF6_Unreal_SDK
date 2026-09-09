#include <cstdlib>
#include "meshset.h"

#include <cstring>

namespace bf6 {

static const int BASE         = 16;    // every stored offset is relative to this
static const int SECTION_SIZE = 368;
static const int DECL_SIZE    = 100;

// PackedByteArray decode_* are little-endian; match them.
static uint16_t u16(const uint8_t* d, size_t at) {
    return (uint16_t)(d[at] | (d[at + 1] << 8));
}
static uint32_t u32(const uint8_t* d, size_t at) {
    return uint32_t(d[at]) | (uint32_t(d[at + 1]) << 8) |
           (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
}
static int32_t s32(const uint8_t* d, size_t at) { return (int32_t)u32(d, at); }
static uint64_t u64(const uint8_t* d, size_t at) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v |= (uint64_t)d[at + k] << (8 * k);
    return v;
}
static int64_t s64(const uint8_t* d, size_t at) { return (int64_t)u64(d, at); }

static std::string cstr(const uint8_t* d, size_t len, int64_t off) {
    if (off <= 0 || (size_t)off >= len) return std::string();
    size_t e = (size_t)off;
    while (e < len && d[e] != 0) e++;
    return std::string((const char*)d + off, e - off);
}

// 100-byte GeometryDeclaration -> elements + streams.
static MeshDecl decl_at(const uint8_t* d, size_t len, int64_t off) {
    MeshDecl md;
    if (off < 0 || (size_t)off + DECL_SIZE > len) return md;
    uint8_t ne = d[off + 96];
    uint8_t ns = d[off + 97];
    for (int i = 0; i < ne && i < 16; i++) {
        size_t p = (size_t)off + i * 4;
        md.elements.push_back({d[p], d[p + 1], d[p + 2], d[p + 3]});
    }
    for (int i = 0; i < ns && i < 16; i++) {
        size_t p = (size_t)off + 64 + i * 2;
        md.streams.push_back({d[p], d[p + 1]});
    }
    return md;
}

static std::vector<MeshSection> sections_at(const uint8_t* d, size_t len,
                                            int64_t off, int count) {
    std::vector<MeshSection> out;
    for (int i = 0; i < count; i++) {
        size_t p = (size_t)off + (size_t)i * SECTION_SIZE;
        if (p + SECTION_SIZE > len) break;
        MeshSection s;
        s.index = i;
        s.material = cstr(d, len, s64(d, p + 0x08) + BASE);
        s.bones_per_vertex = d[p + 0x1A];
        MeshDecl decl = decl_at(d, len, p + 0x64);
        if (s.bones_per_vertex > 0) {
            // Skinned: the second declaration supersedes the first, but only if
            // it actually declares formats.
            MeshDecl d2 = decl_at(d, len, p + 0xC8);
            bool any = false;
            for (const auto& e : d2.elements) if (e[1] != 0) { any = true; break; }
            if (any) decl = d2;
        }
        s.state_key     = u64(d, p + 0x130);
        s.material_id   = u16(d, p + 0x1C);
        s.stride        = d[p + 0x1E];
        s.prim_count    = u32(d, p + 0x20);
        s.start_index   = u32(d, p + 0x24);
        s.vertex_offset = u32(d, p + 0x28);
        s.vertex_count  = u32(d, p + 0x2C);
        s.decl  = decl;
        s.decl0 = decl_at(d, len, p + 0x64);
        s.decl1 = decl_at(d, len, p + 0xC8);
        out.push_back(std::move(s));
    }
    return out;
}

// Vertex-element usage codes.
static const int U_POS = 1, U_NORMAL = 6, U_UV0 = 33, U_UV4 = 37;
// BoneIndices. On a Rigid or Composite destructible this is the per-vertex
// destruction part index; on a Skinned mesh it is a skeleton bone id.
static const int U_BONE = 2;
// The rest of the skin binding. Usage 2 and 3 are the bone index lanes (u16,
// UShort2/UShort4); usage 4 and 5 are the matching weight lanes (UByte4N).
// A section with 8 influences declares all four; one with 4 declares 2 and 4.
static const int U_BONE2 = 3, U_BONE_W = 4, U_BONE_W2 = 5;

// A bone index with 0x8000 set addresses a RENDERBONE, and this reader does
// NOT resolve it - deliberately.
//
// The documented rule is ((raw & 0x7FFF) >> 1) + base, where `base` is the RIG
// BONE COUNT of the skeleton the mesh binds to: a flagged value names a slot in
// the mesh's Renderbones array, which is appended past the rig. The shifted
// slot is small (global max 20 across the character population).
//
// A mesh reader cannot know `base`. It is a property of the SKELETON, and the
// mesh does not carry it. Resolving here would mean inventing one, and both
// ways of inventing it are wrong in a way that still renders:
//   - dropping the + base binds the vertex to a low real bone
//   - masking the flag off without the shift maps 0x8001 to bone 1
// Either produces plausible-looking wrong deformation rather than a failure.
//
// So the raw value is passed through with its flag intact, and the consumer -
// which has the skeleton and therefore `base` - resolves it. The header
// documents the arithmetic. An unflagged value is already a skeleton bone id
// and needs nothing.
//
// (An earlier revision of this file remapped flagged values in place, and then
// a second revision narrowed that to values with bit 0 set. Both were wrong:
// the bit-0 rule came from a 55-mesh sample that the format spec has since
// superseded at full population, where 2.5% of flagged values have bit 0 clear
// and the shift holds for all of them.)
static inline uint16_t decode_bone_index(uint16_t raw) { return raw; }

static int fmt_size(int fmt) {
    switch (fmt) {
        case 1: return 4;  case 2: return 8;  case 3: return 12; case 4: return 16;
        case 5: return 2;  case 6: return 4;  case 7: return 6;  case 8: return 8;
        case 10: case 11: case 12: case 13: return 4;
        case 14: return 2; case 15: return 4; case 16: return 6; case 17: return 8;
        case 18: return 2; case 19: return 4; case 20: return 6; case 21: return 8;
        case 22: return 4; case 23: return 8; case 24: return 4; case 25: return 8;
        case 50: return 1;
    }
    return 0;
}
static int fmt_components(int fmt) {
    switch (fmt) {
        case 1: case 5: case 14: case 18: case 50: return 1;
        case 2: case 6: case 15: case 19: case 22: case 24: return 2;
        case 3: case 7: case 16: case 20: return 3;
        case 4: case 8: case 10: case 11: case 12: case 13:
        case 17: case 21: case 23: case 25: return 4;
    }
    return 0;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) { bits = sign; }
        else {
            exp = 127 - 15 + 1;
            while ((man & 0x400) == 0) { man <<= 1; exp--; }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000 | (man << 13);
    } else {
        bits = sign | ((exp + (127 - 15)) << 23) | (man << 13);
    }
    float f; std::memcpy(&f, &bits, 4); return f;
}

static float f32(const uint8_t* d, size_t at) {
    uint32_t u = u32(d, at); float f; std::memcpy(&f, &u, 4); return f;
}
static int16_t rd_s16(const uint8_t* d, size_t at) { return (int16_t)u16(d, at); }

// Decode one vertex element across `count` vertices -> (floats, components).
// SoA per stream: each stream's sub-buffer sits back to back from `base`.
static std::pair<std::vector<float>, int> read_attr(
    const uint8_t* buf, size_t buflen, int base, int count,
    const std::array<uint8_t, 4>& el, const std::vector<std::array<uint8_t, 2>>& streams) {
    std::vector<float> out;
    int fmt = el[1], off = el[2], si = el[3];
    if (si >= (int)streams.size()) return {out, 0};
    int sstride = streams[si][0];
    int size = fmt_size(fmt);
    if (size == 0 || sstride == 0) return {out, 0};
    size_t sbase = (size_t)base;
    for (int s = 0; s < si; s++) sbase += (size_t)streams[s][0] * count;
    if (sbase + (size_t)(count - 1) * sstride + off + size > buflen) return {out, 0};
    int comps = fmt_components(fmt);
    if (comps == 0) return {out, 0};
    out.resize((size_t)count * comps);
    for (int i = 0; i < count; i++) {
        size_t p = sbase + (size_t)i * sstride + off;
        int o = i * comps;
        switch (fmt) {
            case 1: case 2: case 3: case 4:
                for (int c = 0; c < comps; c++) out[o + c] = f32(buf, p + c * 4); break;
            case 5: case 6: case 7: case 8:
                for (int c = 0; c < comps; c++) out[o + c] = half_to_float(u16(buf, p + c * 2)); break;
            case 14: case 15: case 16: case 17:
                for (int c = 0; c < comps; c++) out[o + c] = (float)rd_s16(buf, p + c * 2); break;
            case 18: case 19: case 20: case 21:
                for (int c = 0; c < comps; c++) out[o + c] = (float)rd_s16(buf, p + c * 2) / 32767.0f; break;
            case 22: case 23:
                for (int c = 0; c < comps; c++) out[o + c] = (float)u16(buf, p + c * 2); break;
            case 24: case 25:
                for (int c = 0; c < comps; c++) out[o + c] = (float)u16(buf, p + c * 2) / 65535.0f; break;
            case 11: case 13:
                for (int c = 0; c < comps; c++) out[o + c] = (float)buf[p + c] / 255.0f; break;
            case 10: case 12:
                for (int c = 0; c < comps; c++) out[o + c] = (float)buf[p + c]; break;
            default: return {std::vector<float>(), 0};
        }
    }
    return {out, comps};
}

// Triangle indices for one section, CCW->CW wound, degenerate/out-of-range dropped.
static std::vector<uint32_t> read_indices(
    const uint8_t* buf, size_t buflen, int vsize, int isize, bool idx32,
    int start, int prim_count, int vcount, int voff) {
    std::vector<uint32_t> out;
    int stride = idx32 ? 4 : 2;
    int need = prim_count * 3;
    size_t first = (size_t)vsize + (size_t)start * stride;
    if (first + (size_t)need * stride > (size_t)vsize + isize) return out;
    if (first + (size_t)need * stride > buflen) return out;

    std::vector<int64_t> raw(need);
    if (idx32) for (int i = 0; i < need; i++) raw[i] = u32(buf, first + i * 4);
    else       for (int i = 0; i < need; i++) raw[i] = u16(buf, first + i * 2);

    // Some LOD0s store vertex-buffer-absolute indices; retry minus voff.
    int64_t hi = 0;
    for (int64_t v : raw) if (v > hi) hi = v;
    if (hi >= vcount && voff > 0) {
        bool ok = true;
        for (int i = 0; i < need; i++) { int64_t v2 = raw[i] - voff; if (v2 < 0 || v2 >= vcount) { ok = false; break; } }
        if (ok) for (int i = 0; i < need; i++) raw[i] -= voff;
    }

    out.resize(need);
    int w = 0;
    for (int t = 0; t < prim_count; t++) {
        int64_t a = raw[t * 3], b = raw[t * 3 + 1], c = raw[t * 3 + 2];
        if (a < 0 || b < 0 || c < 0 || a >= vcount || b >= vcount || c >= vcount) continue;
        if (a == b || b == c || a == c) continue;
        out[w] = (uint32_t)a; out[w + 1] = (uint32_t)c; out[w + 2] = (uint32_t)b;  // swap for CW
        w += 3;
    }
    out.resize(w);
    return out;
}

bool meshset_inline_lod(const MeshSet& ms, int lod, const uint8_t* d, size_t len,
                        const uint8_t** out, size_t* out_len) {
    if (!d || !out || !out_len) return false;
    if (lod < 0 || lod >= (int)ms.lods.size()) return false;
    const MeshLod& L = ms.lods[(size_t)lod];
    for (int i = 0; i < 16; i++) if (L.chunk_id[i]) return false;   // it has a chunk
    if (L.vertex_size <= 0 || L.index_size < 0) return false;

    // The base is at the TAIL, so it comes from the furthest end over ALL the
    // LODs and not from this one. Taking this LOD's own end would put LOD 0 -
    // whose offset is 0 - at the very end of the file.
    int64_t hi = 0;
    for (const MeshLod& o : ms.lods) {
        const int64_t end = (int64_t)o.inline_offset + o.vertex_size + o.index_size;
        if (end > hi) hi = end;
    }
    const int64_t base = (int64_t)len - hi;
    if (base < 0) return false;

    const int64_t start = base + (int64_t)L.inline_offset;
    const int64_t need  = (int64_t)L.vertex_size + L.index_size;
    if (start < 0 || start + need > (int64_t)len) return false;

    *out     = d + start;
    *out_len = (size_t)need;
    return true;
}

std::vector<MeshGeomSection> meshset_read_lod(const MeshSet& ms, int lod,
                                              const uint8_t* chunk, size_t clen,
                                              std::string& err) {
    std::vector<MeshGeomSection> out;
    if (lod < 0 || lod >= (int)ms.lods.size()) { err = "lod out of range"; return out; }
    const MeshLod& L = ms.lods[lod];
    int vsize = L.vertex_size, isize = L.index_size;
    if ((int64_t)clen < (int64_t)vsize + isize) { err = "geometry short"; return out; }

    for (const MeshSection& s : L.sections) {
        int vcount = s.vertex_count, pcount = s.prim_count;
        if (pcount == 0 || vcount == 0) continue;
        std::string low = s.material;
        for (char& ch : low) if (ch >= 'A' && ch <= 'Z') ch += 32;
        /* Depth-only and shadow passes carry no shading of interest, so they are
         * skipped - but the SKIP IS INVISIBLE to a caller, which matters when a
         * section is missing for some other reason. BF6_MESH_KEEP_ALL keeps
         * them so a diagnostic can see the full section list of a mesh. */
        static const bool keep_all = [](){ const char* e = std::getenv("BF6_MESH_KEEP_ALL");
                                           return e && *e && *e != '0'; }();
        if (!keep_all &&
            (low.find("shadow") != std::string::npos || low.find("zonly") != std::string::npos ||
             low.find("depth") != std::string::npos)) continue;

        int voff = s.vertex_offset;
        std::vector<float> pos, nrm;
        std::vector<float> bi[2], bw[2];      // BoneIndices/2, BoneWeights/2
        int bi_comps[2] = {0, 0}, bw_comps[2] = {0, 0};
        int pos_comps = 0, nrm_comps = 0;
        std::pair<std::vector<float>, int> uv_ch[5];   // by channel, not by order
        for (const auto& el : s.decl.elements) {
            int usage = el[0];
            if (usage == U_POS && pos.empty()) {
                auto r = read_attr(chunk, clen, voff, vcount, el, s.decl.streams);
                if (!r.first.empty()) { pos = std::move(r.first); pos_comps = r.second; }
            } else if (usage == U_NORMAL && nrm.empty()) {
                auto r = read_attr(chunk, clen, voff, vcount, el, s.decl.streams);
                if (!r.first.empty()) { nrm = std::move(r.first); nrm_comps = r.second; }
            } else if (usage == U_BONE || usage == U_BONE2) {
                const int k = (usage == U_BONE) ? 0 : 1;
                if (bi[k].empty()) {
                    auto r = read_attr(chunk, clen, voff, vcount, el, s.decl.streams);
                    if (!r.first.empty()) { bi[k] = std::move(r.first); bi_comps[k] = r.second; }
                }
            } else if (usage == U_BONE_W || usage == U_BONE_W2) {
                const int k = (usage == U_BONE_W) ? 0 : 1;
                if (bw[k].empty()) {
                    auto r = read_attr(chunk, clen, voff, vcount, el, s.decl.streams);
                    if (!r.first.empty()) { bw[k] = std::move(r.first); bw_comps[k] = r.second; }
                }
            } else if (usage >= U_UV0 && usage <= U_UV4) {
                // <= U_UV4, not <= 36: the old bound silently dropped TC4,
                // which is the channel a pictorial wrap lives on.
                const int ch = usage - U_UV0;
                if (uv_ch[ch].first.empty()) {
                    auto r = read_attr(chunk, clen, voff, vcount, el, s.decl.streams);
                    if (!r.first.empty() && r.second >= 2) uv_ch[ch] = std::move(r);
                }
            }
        }
        if (pos.empty() || pos_comps < 3) continue;

        MeshGeomSection g;
        g.material = s.material; g.state_key = s.state_key; g.material_id = s.material_id;
        g.category_flags = s.category_flags;
        g.positions.resize((size_t)vcount * 3);
        for (int i = 0; i < vcount; i++) {
            int o = i * pos_comps;
            g.positions[i * 3] = pos[o]; g.positions[i * 3 + 1] = pos[o + 1]; g.positions[i * 3 + 2] = pos[o + 2];
        }
        for (int ch = 0; ch < 5; ch++) {
            if (uv_ch[ch].first.empty()) continue;
            const auto& src = uv_ch[ch].first;
            const int c = uv_ch[ch].second;
            g.uv[ch].resize((size_t)vcount * 2);
            for (int i = 0; i < vcount; i++) {
                g.uv[ch][i * 2]     = src[i * c];
                g.uv[ch][i * 2 + 1] = src[i * c + 1];
            }
        }
        // TC0 is the primary for everything except car paint, and the caller
        // overrides it there once it has read the depot.
        g.uv0 = g.uv[0];
        if (nrm_comps >= 3) {
            g.normals.resize((size_t)vcount * 3);
            for (int i = 0; i < vcount; i++) {
                int o = i * nrm_comps;
                g.normals[i * 3] = nrm[o]; g.normals[i * 3 + 1] = nrm[o + 1]; g.normals[i * 3 + 2] = nrm[o + 2];
            }
        }
        // THE PART INDEX, READ RAW.
        //
        // Three things about this element are easy to get wrong, and each
        // corrupts a different subset of vertices rather than failing outright:
        //
        //   SLOT 0 IS STORED LAST inside the element, so the lane to read is
        //   laneCount-1 and not 0. Lane count comes from the format: the 2-lane
        //   formats are UShort2 (22) and UShort2N (24), the 4-lane ones are
        //   Short4 (17), Short4N (21), UShort4 (23) and UShort4N (25).
        //
        //   THE VALUE IS A DIRECT GLOBAL PART INDEX. The section's own bone
        //   list is the SET of parts it touches, not a palette to map through;
        //   mapping through it corrupts exactly those vertices whose part id
        //   happens to fall inside the palette's length.
        //
        //   IT MUST BE READ RAW, not through read_attr, which normalises the
        //   N formats and would turn part 17 into 0.00026.
        //
        //   AND IT MAY BE IN EITHER DECLARATION, so both are searched.
        for (const MeshDecl* dc : { &s.decl0, &s.decl1 }) {
            if (!g.parts.empty()) break;
            for (const auto& el : dc->elements) {
                if (el[0] != U_BONE) continue;
                const int fmt = el[1];
                int lanes = 0;
                if (fmt == 22 || fmt == 24) lanes = 2;
                else if (fmt == 17 || fmt == 21 || fmt == 23 || fmt == 25) lanes = 4;
                if (lanes == 0) continue;
                const int si = el[3];
                if (si >= (int)dc->streams.size()) continue;
                const int sstride = dc->streams[si][0];
                if (sstride == 0) continue;
                size_t sbase = (size_t)voff;
                for (int k = 0; k < si; k++) sbase += (size_t)dc->streams[k][0] * vcount;
                const size_t off = (size_t)el[2] + (size_t)(lanes - 1) * 2;
                if (sbase + (size_t)(vcount - 1) * sstride + off + 2 > clen) continue;
                g.parts.resize((size_t)vcount);
                for (int i = 0; i < vcount; i++)
                    g.parts[(size_t)i] = u16(chunk, sbase + (size_t)i * sstride + off);
                break;
            }
        }

        // THE SKIN BINDING. Concatenate the two index elements and the two
        // weight elements lane for lane, so an 8-influence vertex arrives as
        // one run of 8 rather than as two halves the caller has to know to join.
        //
        // The influence count comes from what the section DECLARES, not from
        // MeshSection.bones_per_vertex: the second pair is simply absent on a
        // 4-influence section, and trusting a count field over the declaration
        // would read four lanes of nothing.
        if (!bi[0].empty() && !bw[0].empty()) {
            const int n0 = bi_comps[0] < bw_comps[0] ? bi_comps[0] : bw_comps[0];
            int n1 = 0;
            if (!bi[1].empty() && !bw[1].empty())
                n1 = bi_comps[1] < bw_comps[1] ? bi_comps[1] : bw_comps[1];
            const int inf = n0 + n1;
            if (inf > 0) {
                g.influences = inf;
                g.skin_bones.resize((size_t)vcount * inf);
                g.skin_weights.resize((size_t)vcount * inf);
                for (int i = 0; i < vcount; i++) {
                    for (int c = 0; c < n0; c++) {
                        const size_t d = (size_t)i * inf + c;
                        g.skin_bones[d]   = decode_bone_index((uint16_t)bi[0][(size_t)i * bi_comps[0] + c]);
                        g.skin_weights[d] = bw[0][(size_t)i * bw_comps[0] + c];
                    }
                    for (int c = 0; c < n1; c++) {
                        const size_t d = (size_t)i * inf + n0 + c;
                        g.skin_bones[d]   = decode_bone_index((uint16_t)bi[1][(size_t)i * bi_comps[1] + c]);
                        g.skin_weights[d] = bw[1][(size_t)i * bw_comps[1] + c];
                    }
                }
            }
        }

        g.indices = read_indices(chunk, clen, vsize, isize, L.idx32, s.start_index, pcount, vcount, voff);
        if (g.indices.empty()) continue;
        out.push_back(std::move(g));
    }
    return out;
}

MeshSet meshset_parse(const uint8_t* d, size_t len, std::string& err) {
    MeshSet ms;
    if (len < (size_t)BASE + 0xA0) { err = "too short to be a MeshSet"; return ms; }

    int lod_stride = (int)u32(d, 0);
    if (lod_stride != 176 && lod_stride != 192) lod_stride = 176;
    ms.lod_stride = lod_stride;

    int h = BASE;
    int64_t lod_offs[6];
    for (int i = 0; i < 6; i++) lod_offs[i] = s64(d, h + 0x20 + i * 8);

    ms.name = cstr(d, len, s64(d, h + 0x60) + BASE);
    ms.mesh_type = d[h + 0x6C];
    ms.lod_count = u16(d, h + 0x9C);
    ms.section_count = u16(d, h + 0x9E);

    // The bone/part block sits at header+0xAC and exists only on a non-Rigid
    // mesh. Read it before the LODs so every section can point at one palette.
    if (ms.mesh_type != 0 && (size_t)(h + 0xAC + 4) <= len) {
        ms.bone_count      = (int)u16(d, h + 0xAC);
        const int npart     = (int)u16(d, h + 0xAE);
        // The two offsets follow ONLY IF either count is non-zero. Reading them
        // unconditionally on a mesh that declares neither walks into whatever
        // follows the block and yields a plausible-looking offset.
        if ((ms.bone_count || npart) && (size_t)(h + 0xAC + 20) <= len) {
            const int64_t bio = s64(d, h + 0xB0) + BASE;
            if (npart > 0 && bio > 0 && (size_t)bio + (size_t)npart * 2 <= len) {
                ms.bone_parts.resize((size_t)npart);
                for (int i = 0; i < npart; i++)
                    ms.bone_parts[(size_t)i] = u16(d, (size_t)bio + (size_t)i * 2);
            }
        }
    }

    int nl = ms.lod_count < 6 ? ms.lod_count : 6;
    for (int li = 0; li < nl; li++) {
        int64_t lo = lod_offs[li] + BASE;
        if (lo <= 0 || (size_t)lo >= len - (size_t)lod_stride) continue;
        int sec_count = (int)u32(d, lo + 0x08);
        int64_t sec_off = s64(d, lo + 0x0C) + BASE;
        int idx_fmt = (int)u32(d, lo + 0x54);
        int idx_size = s32(d, lo + 0x58);
        int vtx_size = s32(d, lo + 0x5C);
        if (sec_count > 4096 || sec_off <= 0 || (size_t)sec_off >= len) continue;

        MeshLod lod;
        lod.index = li;
        lod.section_count = sec_count;
        lod.sections = sections_at(d, len, sec_off, sec_count);
        // The render category is authored per section. Category 2 is the
        // executable's MeshSubsetCategory_TransparentDecal; a filename test
        // finds only 28.5% of those sections and has false positives.
        for (int cat = 0; cat < 5; cat++) {
            const size_t cp = (size_t)lo + 0x14 + (size_t)cat * 12;
            const int cnt = s32(d, cp);
            const int64_t off = s64(d, cp + 4) + BASE;
            if (cnt <= 0 || cnt > sec_count || off < 0 || (size_t)off + cnt > len)
                continue;
            for (int k = 0; k < cnt; k++) {
                const int si = d[(size_t)off + k];
                if (si >= 0 && si < (int)lod.sections.size())
                    lod.sections[(size_t)si].category_flags |= (uint8_t)(1u << cat);
            }
        }
        lod.idx32 = (idx_fmt == 46);
        lod.index_size = idx_size;
        lod.vertex_size = vtx_size;
        std::memcpy(lod.chunk_id.data(), d + lo + 0x74, 16);
        lod.inline_offset = u32(d, lo + 0x84);
        lod.name = cstr(d, len, s64(d, lo + 0x94) + BASE);
        ms.lods.push_back(std::move(lod));
    }
    ms.size = len;
    ms.ok = true;
    return ms;
}

}  // namespace bf6
