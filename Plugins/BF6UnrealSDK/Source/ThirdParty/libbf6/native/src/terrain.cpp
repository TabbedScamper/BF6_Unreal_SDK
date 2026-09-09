#include "terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bf6 {
namespace {

constexpr size_t kHeaderSize  = 0x22;
constexpr size_t kBlockHeader = 0x48;
constexpr int    kBlockHeights = 0;

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t off)
{
    T v{};
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

bool fits(const std::vector<uint8_t>& d, size_t off, size_t n)
{
    return off + n <= d.size();
}

// The min/max pyramid, and the occluder stack beside it. Both are skipped
// rather than read, but their SIZE decides where a node's samples start, so
// getting this wrong shifts every payload.
size_t stack_size(int depth, bool occluder)
{
    if (depth <= 0) return 0;
    const int b0 = 1 << (depth - 1);
    size_t total = 0;
    for (int c = 0; c < depth; c++)
    {
        const size_t b = (size_t)(b0 >> c);
        total += occluder ? (b + 1) * (b + 1) : 2 * b * b;
    }
    return total;
}

// RAW HEX of the 16 bytes, not the dashed .NET spelling. The chunk index is
// keyed on the bytes as they lie, so the canonical text form that reads nicely
// in a spec resolves nothing at all: the first attempt at this in GDScript
// produced dashed guids and found 0 of 100 chunks while every other stage
// parsed byte-exactly.
std::string guid_hex(const std::vector<uint8_t>& b, size_t o)
{
    if (o + 16 > b.size()) return std::string();
    char t[33];
    for (int i = 0; i < 16; i++) std::snprintf(t + i * 2, 3, "%02x", b[o + (size_t)i]);
    return std::string(t, 32);
}

std::string reverse_guid(const std::string& hex)
{
    if (hex.size() != 32) return hex;
    std::string out(32, '0');
    for (int i = 0; i < 16; i++)
    {
        out[(size_t)(15 - i) * 2]     = hex[(size_t)i * 2];
        out[(size_t)(15 - i) * 2 + 1] = hex[(size_t)i * 2 + 1];
    }
    return out;
}

}  // namespace

// The 0xFF terminator is an empirical rule that holds on every shipped map but
// was never located in engine code, so a malformed walk fails loudly rather
// than running off the end: every step is bounds-checked.
bool Terrain::find_block(const std::vector<uint8_t>& res, int want,
                         std::vector<uint8_t>& out, std::string& err) const
{
    if (res.size() < kHeaderSize) { err = "shorter than the 34-byte container header"; return false; }
    size_t o = kHeaderSize;
    while (o + 5 <= res.size())
    {
        const int t = res[o];
        if (t == 0xFF) break;
        const int32_t sz = rd<int32_t>(res, o + 1);
        if (sz < 0 || o + 5 + (size_t)sz > res.size())
        {
            char m[128];
            std::snprintf(m, sizeof(m), "block %d claims %d bytes at %zu, past the end", t, sz, o);
            err = m;
            return false;
        }
        if (t == want)
        {
            out.assign(res.begin() + (ptrdiff_t)(o + 5), res.begin() + (ptrdiff_t)(o + 5 + (size_t)sz));
            return true;
        }
        o += 5 + (size_t)sz;
    }
    err = "no heights block in this terrain resource";
    return false;
}

bool Terrain::read_block_header(const std::vector<uint8_t>& b, std::string& err)
{
    if (b.size() < kBlockHeader) { err = "heightfield block shorter than its 72-byte header"; return false; }
    xs_ = rd<int32_t>(b, 0x00);
    // NOT a constant 4. Every map measured has 4, but the header carries the
    // number and a map that shipped a different one would silently shift the
    // whole heightfield.
    border_ = rd<int32_t>(b, 0x44);
    if (border_ < 0 || border_ * 2 >= xs_) border_ = 0;
    world_size_y_ = rd<float>(b, 0x10);

    const int32_t minmax_depth   = rd<int32_t>(b, 0x1C);
    const int32_t occluder_depth = rd<int32_t>(b, 0x20);
    const int32_t density_side   = rd<int32_t>(b, 0x24);
    if (xs_ <= 0 || xs_ > 8192)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "implausible NodeSamplesPerSide %d", xs_);
        err = m;
        return false;
    }
    // Cross-check the spec's identity rather than trusting one field: a
    // mismatch means the header is being read at the wrong offset, which would
    // otherwise show up much later as a stride that is slightly wrong.
    const int32_t expect = (xs_ - 1) / 4 + 1;
    if (density_side != expect)
    {
        char m[128];
        std::snprintf(m, sizeof(m),
            "density side %d != (xs-1)/4+1 = %d - header offsets are wrong", density_side, expect);
        err = m;
        return false;
    }
    packed_bytes_  = stack_size(minmax_depth, false) * 2;
    section_bytes_ = packed_bytes_ + stack_size(occluder_depth, true) * 2;
    density_bytes_ = (size_t)density_side * (size_t)density_side;
    data_size_     = (size_t)xs_ * (size_t)xs_ * 2 + section_bytes_ + density_bytes_;
    return true;
}

bool Terrain::read_node(const std::vector<uint8_t>& b, size_t& p, uint64_t key, int depth,
                        std::string& err)
{
    if (!fits(b, p, 29)) { err = "node header past the end"; return false; }
    TerrainNode n;
    n.key = key;
    n.depth = depth;
    for (int i = 0; i < 3; i++) n.lo[i] = rd<float>(b, p + (size_t)i * 4);
    for (int i = 0; i < 3; i++) n.hi[i] = rd<float>(b, p + 12 + (size_t)i * 4);
    p += 28;                                  // 6 floats plus the unused 7th

    const uint8_t kind = b[p++];
    if (kind == 1) return true;               // EMPTY leaf: no flags, no children

    if (!fits(b, p, 4)) { err = "node flags past the end"; return false; }
    const uint8_t skip_packed   = b[p];
    const uint8_t inline_values = b[p + 3];
    p += 4;

    if (skip_packed != 0)
    {
        // PRUNED: only the min/max pyramid is inline, no samples.
        if (!fits(b, p, packed_bytes_)) { err = "pruned payload past the end"; return false; }
        p += packed_bytes_;
    }
    else if (inline_values != 0)
    {
        if (!fits(b, p, data_size_)) { err = "inline payload past the end"; return false; }
        const size_t want = (size_t)xs_ * (size_t)xs_;
        n.values.resize(want);
        std::memcpy(n.values.data(), b.data() + p, want * 2);
        p += data_size_;
    }
    else
    {
        // EXTERNAL: the samples live in a CAS chunk, resolved by the caller
        // through the chunk directory.
        n.external = true;
    }

    nodes_.push_back(std::move(n));

    if (!fits(b, p, 1)) { err = "child flag past the end"; return false; }
    const uint8_t has_children = b[p++];
    if (has_children)
        for (int i = 0; i < 4; i++)
            if (!read_node(b, p, (key << 4) | (uint64_t)i, depth + 1, err)) return false;
    return true;
}

bool Terrain::walk_nodes(const std::vector<uint8_t>& b, std::string& err)
{
    nodes_.clear();
    size_t p = kBlockHeader;
    if (!read_node(b, p, 3, 0, err)) return false;
    if (p != b.size())
    {
        // The walk is specified as byte-exact. Anything else means the grammar
        // desynchronised, and the nodes read so far are not trustworthy even
        // though they look plausible.
        char m[96];
        std::snprintf(m, sizeof(m), "walk consumed %zu of %zu bytes", p, b.size());
        err = m;
        return false;
    }
    return true;
}

bool Terrain::read_dir_node(const std::vector<uint8_t>& res, size_t& p, uint64_t key,
                            std::string& err)
{
    if (!fits(res, p, 21)) { err = "directory node past the end"; return false; }
    DirEntry e;
    e.primary_size = rd<uint32_t>(res, p);
    e.primary      = guid_hex(res, p + 4);
    p += 20;
    const uint8_t has_pair = res[p++];
    if (has_pair)
    {
        if (!fits(res, p, 20)) { err = "paired chunk past the end"; return false; }
        e.paired_size = rd<uint32_t>(res, p);
        e.paired      = guid_hex(res, p + 4);
        p += 20;
    }
    if (!fits(res, p, 2)) { err = "directory flags past the end"; return false; }
    p += 1;                                   // persistentDedicatedServer
    const uint8_t has_children = res[p++];
    dir_[key] = std::move(e);
    if (has_children)
        for (int i = 0; i < 4; i++)
            if (!read_dir_node(res, p, (key << 4) | (uint64_t)i, err)) return false;
    return true;
}

bool Terrain::read_chunk_directory(const std::vector<uint8_t>& res, std::string& err)
{
    dir_.clear();
    size_t o = kHeaderSize;
    while (o + 5 <= res.size())
    {
        if (res[o] == 0xFF) { o += 1; break; }
        const int32_t sz = rd<int32_t>(res, o + 1);
        if (sz < 0 || o + 5 + (size_t)sz > res.size())
        { err = "block overruns while looking for the directory"; return false; }
        o += 5 + (size_t)sz;
    }
    size_t p = o;
    if (!read_dir_node(res, p, 3, err)) return false;
    if (p != res.size())
    {
        char m[112];
        std::snprintf(m, sizeof(m), "chunk directory consumed %zu of %zu bytes", p, res.size());
        err = m;
        return false;
    }
    return true;
}

bool Terrain::parse(const std::vector<uint8_t>& res, std::string& err)
{
    return parse_block(res, kBlockHeights, err);
}

bool Terrain::parse_water_surface(const std::vector<uint8_t>& res, std::string& err)
{
    return parse_block(res, 2, err);
}

bool Terrain::parse_block(const std::vector<uint8_t>& res, int block, std::string& err)
{
    err.clear();
    block_id_ = block;
    primary_prefix_bytes_ = 0;
    stream_page_bytes_ = 0;
    if (res.size() >= kHeaderSize) node_count_ = rd<int32_t>(res, 0x19);

    // Primary chunks are ordered [block0 payload][streamed pages][block2
    // payload][trailing pages].  Derive both units from the live block-0
    // header; no exported offset or per-map table is consumed at runtime.
    if (block != kBlockHeights)
    {
        std::vector<uint8_t> ground;
        if (!find_block(res, kBlockHeights, ground, err)) return false;
        Terrain header;
        if (!header.read_block_header(ground, err)) return false;
        primary_prefix_bytes_ = header.data_size_;
        const int page_side = std::max(0, (header.xs_ - 1) / 4);
        stream_page_bytes_ = (size_t)page_side * (size_t)page_side;
    }

    std::vector<uint8_t> blk;
    if (!find_block(res, block, blk, err)) return false;
    if (!read_block_header(blk, err)) return false;
    if (!walk_nodes(blk, err)) return false;

    // A directory that will not parse is not fatal on its own: a map whose
    // nodes are all inline does not need one. It is only fatal if the nodes
    // then turn out to be external, and resolve_external reports that.
    std::string dir_err;
    if (!read_chunk_directory(res, dir_err)) dir_.clear();
    return true;
}

// A node's values are in its OWN primary chunk when the directory has one for
// that key; otherwise in the PARENT's paired chunk, where the four children are
// concatenated in REVERSED order at a fixed stride. Getting that reversal wrong
// does not fail, it silently swaps quadrants of the map.
int Terrain::resolve_external(const FetchChunk& fetch)
{
    const size_t want = (size_t)xs_ * (size_t)xs_;
    std::map<std::string, std::vector<uint8_t>> cache;

    auto grab = [&](const std::string& guid) -> const std::vector<uint8_t>&
    {
        static const std::vector<uint8_t> kEmpty;
        if (guid.empty()) return kEmpty;
        auto it = cache.find(guid);
        if (it != cache.end()) return it->second;
        // BOTH SPELLINGS, for the reason in the header.
        std::vector<uint8_t> got = fetch(guid);
        if (got.empty()) got = fetch(reverse_guid(guid));
        return cache.emplace(guid, std::move(got)).first->second;
    };

    int got_n = 0;
    for (TerrainNode& n : nodes_)
    {
        if (!n.external) continue;

        auto de = dir_.find(n.key);
        if (de != dir_.end() && !de->second.primary.empty())
        {
            const std::vector<uint8_t>& d = grab(de->second.primary);
            size_t payload = 0;
            bool valid = d.size() >= want * 2;

            if (valid && block_id_ == 2)
            {
                // The block-2 offset is not stored in the directory. Search
                // only offsets permitted by the live block-0 page unit, then
                // validate candidate sample heights against this node's own
                // Y AABB. Wrong offsets tend to contain colour/tile bytes and
                // score near zero; accepting them would produce convincing
                // but false water geometry.
                valid = false;
                double best_score = -1.0;
                size_t best_off = 0;
                if (stream_page_bytes_ > 0 && d.size() >= data_size_)
                {
                    const double scale = world_size_y_ > 0.f
                        ? 65536.0 / (double)world_size_y_ : 0.0;
                    const int lo = (int)std::floor(((double)n.lo[1] - 0.5) * scale);
                    const int hi = (int)std::ceil (((double)n.hi[1] + 0.5) * scale);
                    for (size_t tail = 0; tail + data_size_ <= d.size(); tail += stream_page_bytes_)
                    {
                        const size_t off = d.size() - tail - data_size_;
                        if (off < primary_prefix_bytes_) break;
                        size_t hit = 0, sampled = 0;
                        for (size_t s = 0; s < want; s += 97)
                        {
                            const uint16_t v = rd<uint16_t>(d, off + s * 2);
                            hit += ((int)v >= lo && (int)v <= hi) ? 1u : 0u;
                            sampled++;
                        }
                        const double score = sampled ? (double)hit / (double)sampled : 0.0;
                        if (score > best_score) { best_score = score; best_off = off; }
                    }
                }
                if (best_score >= 0.90) { payload = best_off; valid = true; }
            }

            if (valid && payload + want * 2 <= d.size())
            {
                n.values.resize(want);
                std::memcpy(n.values.data(), d.data() + payload, want * 2);
                n.external = false;
                got_n++;
                continue;
            }
        }

        // Block 2 is presently verified in each node's own primary chunk.
        // Ground's paired-child layout cannot be reused: block 2 follows a
        // variable number of streamed pages, so guessing its paired offset is
        // worse than leaving that node unresolved.
        if (block_id_ == 2) continue;

        const uint64_t parent = n.key >> 4;
        const uint64_t child  = n.key & 0xF;
        auto pe = dir_.find(parent);
        if (pe == dir_.end() || pe->second.paired.empty()) continue;
        const std::vector<uint8_t>& pd = grab(pe->second.paired);
        if (pd.empty()) continue;
        const size_t off = (size_t)(3 - child) * data_size_;
        if (off + want * 2 <= pd.size())
        {
            n.values.resize(want);
            std::memcpy(n.values.data(), pd.data() + off, want * 2);
            n.external = false;
            got_n++;
        }
    }
    return got_n;
}

size_t Terrain::nodes_with_values() const
{
    size_t n = 0;
    for (const TerrainNode& t : nodes_) if (!t.values.empty()) n++;
    return n;
}

int Terrain::native_size() const
{
    int d = -1;
    for (const TerrainNode& n : nodes_)
        if (!n.values.empty()) d = std::max(d, n.depth);
    if (d < 0) return 0;
    return (1 << d) * std::max(1, xs_ - 1 - border_ * 2) + 1;
}

bool Terrain::sample_window(float min_x,float min_z,float size_m,int core_size,
                            int border,TerrainWindow& out,std::string& err) const
{
    out=TerrainWindow();err.clear();
    if(!(size_m>0.f)||core_size<=0||border<0||border>16){err="invalid terrain window request";return false;}
    std::vector<const TerrainNode*> with;for(const TerrainNode& n:nodes_)if(!n.values.empty())with.push_back(&n);
    if(with.empty()){err="no nodes carry height values";return false;}
    std::stable_sort(with.begin(),with.end(),[](const TerrainNode* a,const TerrainNode* b){return a->depth<b->depth;});
    out.core_size=core_size;out.border=border;out.size=core_size+2*border;out.core_lo[0]=min_x;out.core_lo[1]=min_z;
    out.core_size_m=size_m;out.texel_m=size_m/(float)core_size;out.world_size_y=world_size_y_;
    out.heights.assign((size_t)out.size*out.size,0);std::vector<uint8_t> found((size_t)out.size*out.size,0);
    const int s0=border_,s1=xs_-1-border_;const float span=(float)std::max(1,s1-s0);
    for(const TerrainNode* n:with){
        const float nx=n->hi[0]-n->lo[0],nz=n->hi[2]-n->lo[2];if(!(nx>0.f&&nz>0.f))continue;
        for(int z=0;z<out.size;z++){
            const float wz=min_z+((float)(z-border)+.5f)*out.texel_m;if(wz<n->lo[2]||wz>n->hi[2])continue;
            const float fz=std::clamp((wz-n->lo[2])/nz,0.f,1.f);const float sy=(float)s0+fz*span;const int y0=std::min(xs_-2,std::max(0,(int)std::floor(sy)));const float ty=sy-y0;
            for(int x=0;x<out.size;x++){
                const float wx=min_x+((float)(x-border)+.5f)*out.texel_m;if(wx<n->lo[0]||wx>n->hi[0])continue;
                const float fx=std::clamp((wx-n->lo[0])/nx,0.f,1.f);const float sx=(float)s0+fx*span;const int x0=std::min(xs_-2,std::max(0,(int)std::floor(sx)));const float tx=sx-x0;
                const size_t a=(size_t)y0*xs_+x0;if(a+(size_t)xs_+1>=n->values.size())continue;
                const float h0=(float)n->values[a]+((float)n->values[a+1]-n->values[a])*tx;
                const float h1=(float)n->values[a+xs_]+((float)n->values[a+xs_+1]-n->values[a+xs_])*tx;
                const float hv=std::clamp(h0+(h1-h0)*ty,0.f,65535.f);const size_t at=(size_t)z*out.size+x;
                out.heights[at]=(uint16_t)(hv+.5f);found[at]=1;
            }
        }
    }
    for(uint8_t v:found)if(!v)out.missing++;
    if(out.missing==found.size()){err="terrain window lies outside every resolved height node";return false;}
    return true;
}

bool Terrain::composite(TerrainGrid& out, int size, std::string& err) const
{
    err.clear();
    if (size <= 0) size = native_size();
    if (size <= 0) { err = "no nodes carry height values"; return false; }
    size = std::min(size, 16385);   // 512 MB of u16, far past anything observed

    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    std::vector<const TerrainNode*> with;
    for (const TerrainNode& n : nodes_)
    {
        for (int i = 0; i < 3; i++)
        {
            lo[i] = std::min(lo[i], n.lo[i]);
            hi[i] = std::max(hi[i], n.hi[i]);
        }
        if (!n.values.empty()) with.push_back(&n);
    }
    if (with.empty()) { err = "no nodes carry height values"; return false; }

    // Deepest last, so the finest data available wins everywhere without
    // needing to know which levels a map happens to have.
    std::stable_sort(with.begin(), with.end(),
        [](const TerrainNode* a, const TerrainNode* b) { return a->depth < b->depth; });

    const float span_x = std::max(0.001f, hi[0] - lo[0]);
    const float span_z = std::max(0.001f, hi[2] - lo[2]);

    out.size = size;
    out.heights.assign((size_t)size * (size_t)size, 0);
    for (int i = 0; i < 3; i++) { out.lo[i] = lo[i]; out.hi[i] = hi[i]; }
    out.world_size_y = world_size_y_;

    // EVERY TILE HAS A PAD BORDER, and it is not the tile's own ground. A
    // node's AABB is spanned by grid[border .. xs-1-border]; the rest is a copy
    // of whatever is next door. Sampling 0..xs-1 instead does two things at
    // once: it shrinks every feature by about 3%, and it paints the NEIGHBOUR's
    // terrain into each tile edge, so boundaries carry a doubled ridge.
    const int   s0  = border_;
    const int   s1  = xs_ - 1 - border_;
    const float ssp = (float)std::max(1, s1 - s0);
    const float hi_f = (float)(xs_ - 1);
    const int   sx_max = xs_ - 2;

    for (const TerrainNode* n : with)
    {
        int x0 = (int)std::floor((n->lo[0] - lo[0]) / span_x * (float)size);
        int x1 = (int)std::ceil ((n->hi[0] - lo[0]) / span_x * (float)size);
        int z0 = (int)std::floor((n->lo[2] - lo[2]) / span_z * (float)size);
        int z1 = (int)std::ceil ((n->hi[2] - lo[2]) / span_z * (float)size);
        x0 = std::clamp(x0, 0, size); x1 = std::clamp(x1, 0, size);
        z0 = std::clamp(z0, 0, size); z1 = std::clamp(z1, 0, size);
        if (x1 <= x0 || z1 <= z0) continue;

        const std::vector<uint16_t>& v = n->values;
        const float zspan = (float)std::max(1, z1 - z0 - 1);
        const float xspan = (float)std::max(1, x1 - x0 - 1);
        const bool  zmany = z1 - z0 > 1;
        const bool  xmany = x1 - x0 > 1;

        // The x mapping depends only on the column, so it is built once per
        // node rather than recomputed on every row.
        const int ncol = x1 - x0;
        std::vector<int>   col_sx((size_t)ncol);
        std::vector<float> col_tx((size_t)ncol);
        for (int ci = 0; ci < ncol; ci++)
        {
            const float fx  = xmany ? (float)ci / xspan : 0.f;
            float       sfx = (float)s0 + fx * ssp;
            sfx = std::clamp(sfx, 0.f, hi_f);
            int sxi = (int)sfx;
            if (sxi > sx_max) sxi = sx_max;
            col_sx[(size_t)ci] = sxi;
            col_tx[(size_t)ci] = sfx - (float)sxi;
        }

        for (int gz = z0; gz < z1; gz++)
        {
            const float fz  = zmany ? (float)(gz - z0) / zspan : 0.f;
            float       sfy = (float)s0 + fz * ssp;
            sfy = std::clamp(sfy, 0.f, hi_f);
            int sy0 = (int)sfy;
            if (sy0 > sx_max) sy0 = sx_max;
            const float tz = sfy - (float)sy0;

            const size_t rowb  = (size_t)sy0 * (size_t)xs_;
            const size_t dbase = (size_t)gz * (size_t)size + (size_t)x0;

            for (int ci = 0; ci < ncol; ci++)
            {
                const size_t r0 = rowb + (size_t)col_sx[(size_t)ci];
                const size_t r1 = r0 + (size_t)xs_;
                if (r1 + 1 >= v.size()) continue;
                // BILINEAR, not nearest. The pyramid is concentric, so away
                // from the middle a coarse node is upsampled many times into
                // this grid and nearest turns every outskirt into stair steps.
                const float txc = col_tx[(size_t)ci];
                const float a0 = (float)v[r0],     b0 = (float)v[r0 + 1];
                const float a1 = (float)v[r1],     b1 = (float)v[r1 + 1];
                const float h0 = a0 + (b0 - a0) * txc;
                const float h1 = a1 + (b1 - a1) * txc;
                float hv = h0 + (h1 - h0) * tz;
                if (hv < 0.f) hv = 0.f;
                if (hv > 65535.f) hv = 65535.f;
                out.heights[dbase + (size_t)ci] = (uint16_t)(hv + 0.5f);
            }
        }
    }
    return true;
}

}  // namespace bf6
