#include "splat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

namespace bf6 {
namespace {

constexpr size_t kResHeader   = 0x22;   // container header before the typed blocks
constexpr size_t kBlock1Head  = 0x3D;   // §5.1: block 1's own 61-byte header
constexpr size_t kRecordSize  = 33;     // §5.1 record stride
constexpr uint16_t kNoPageFlag = 0x0100; // bit 8: IgnoreMask, no stored page
constexpr int kPageSide = 66;           // every page codec decodes to this

// §2.4: the child nibble is a quadrant in TRAVERSAL order, not bit order.
// Getting this wrong does not fail; it silently swaps quadrants of the map.
const int kChildX[4] = {0, 1, 1, 0};
const int kChildZ[4] = {0, 0, 1, 1};

const int kPageSizes[3] = {2592, 4356, 5184};

// §5.3 tile sizes -> side. 8712 is 132² BC1 (MP_Portal_Sand: 33x33 blocks of 8
// bytes - not even 16-divisible, so it cannot be a BC7 tile); the rest are BC7.
// 85,024 is deliberately absent: it is a 67,600 + 17,424 MIP PAIR trailer, not a
// tile, and reading it as a 260² image is 17,424 bytes too many.
int tile_side_of(int bytes)
{
    switch (bytes)
    {
        case 4624:  return 68;
        case 17424: return 132;
        case 67600: return 260;
        case 8712:  return 132;
        default:    return 0;
    }
}

// LARGEST FIRST, and that is load-bearing: 17,424 = 2 x 8,712, so every
// two-tile BC7 trailer also divides by the BC1 size. Smallest-first, the BC1
// branch steals every BC7 map. A larger size can never divide a smaller one, so
// descending order is unambiguous for these four.
const int kTileOrder[4] = {67600, 17424, 8712, 4624};

// §5.2 height-plane payload sizes, plus the sums observed on shipped maps.
// 189,216 is 149,297 + 39,919 and 298,594 is 2 x 149,297, so the set is closed
// under the combinations seen so far. These are PLANE SIZES, which is the whole
// reason the set has sums in it: a chunk carries a variable number of planes
// before the weight pages, so "the prefix" is the total of whatever preceded.
const int kPrefixes[5] = {0, 39919, 149297, 189216, 298594};

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

// RAW HEX of the 16 bytes, not the dashed .NET spelling - see terrain.cpp, where
// the dashed form resolved 0 of 100 chunks while everything else parsed
// byte-exactly.
std::string guid_hex(const std::vector<uint8_t>& b, size_t o)
{
    if (o + 16 > b.size()) return std::string();
    char t[33];
    for (int i = 0; i < 16; i++) std::snprintf(t + i * 2, 3, "%02x", b[o + (size_t)i]);
    return std::string(t, 32);
}

// ---------------------------------------------------------------------------
// BC4 (RGTC1 unsigned), 8 bytes per 4x4 block, single channel.
//
// Decoded here rather than through an image library because the 2,592-byte
// weight page is the only compressed thing this module ever meets, and a
// dependency for forty lines is a bad trade in a library that is meant to drop
// into two different engines.
// ---------------------------------------------------------------------------
void bc4_block(const uint8_t* b, uint8_t* out, int stride)
{
    uint8_t pal[8];
    pal[0] = b[0];
    pal[1] = b[1];
    if (pal[0] > pal[1])
        for (int i = 1; i < 7; i++)
            pal[i + 1] = (uint8_t)(((7 - i) * pal[0] + i * pal[1]) / 7);
    else
    {
        for (int i = 1; i < 5; i++)
            pal[i + 1] = (uint8_t)(((5 - i) * pal[0] + i * pal[1]) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
        {
            const int i = y * 4 + x;
            out[y * stride + x] = pal[(bits >> (3 * i)) & 7u];
        }
}

bool bc4_decode(const uint8_t* src, int side, uint8_t* out)
{
    if (side % 4 != 0) return false;
    const int bw = side / 4;
    for (int by = 0; by < bw; by++)
        for (int bx = 0; bx < bw; bx++)
            bc4_block(src + ((size_t)by * (size_t)bw + (size_t)bx) * 8,
                      out + ((size_t)by * 4 * (size_t)side) + (size_t)bx * 4, side);
    return true;
}

// side x side with an apron -> the inner 66x66.
void crop66(const uint8_t* d, int side, int apron, uint8_t* out)
{
    for (int z = 0; z < kPageSide; z++)
        std::memcpy(out + (size_t)z * kPageSide,
                    d + (size_t)(z + apron) * (size_t)side + (size_t)apron, kPageSide);
}

// The fraction of BC7 blocks in modes 4-7 (byte 0 nonzero, low nibble clear).
// Image tiles measure 0.86-1.00 across studied maps; the degenerate second
// raster some maps ship is modes 0-3 and measures ~0. Sampled every fourth
// block - the two populations are far enough apart that 272 samples settle it.
double mode47_frac(const std::vector<uint8_t>& d, size_t start)
{
    int hits = 0, n = 0;
    for (size_t b = start; b + 16 <= d.size() && n < 512; b += 64, n++)
        if (d[b] != 0 && (d[b] & 0x0F) == 0) hits++;
    return n ? (double)hits / (double)n : 0.0;
}

// BC1: opaque blocks order their two RGB565 endpoints c0 > c1. Colour tiles
// measure ~99% ordered, weight-page bytes ~14%, and constant filler (identical
// endpoints) ~0 - which is why the score is per FORMAT. Granite proved the BC7
// mode test is wrong for BC1 in both directions: real BC1 colour reads as
// "mode 3" and uniform filler reads as "mode 6".
double bc1_ordered_frac(const std::vector<uint8_t>& d, size_t start)
{
    int ordered = 0, n = 0;
    for (size_t b = start; b + 8 <= d.size() && n < 512; b += 32, n++)
    {
        uint16_t c0, c1;
        std::memcpy(&c0, d.data() + b, 2);
        std::memcpy(&c1, d.data() + b + 2, 2);
        if (c0 > c1) ordered++;
    }
    return n ? (double)ordered / (double)n : 0.0;
}

// HOW MANY OF THE SAMPLED BLOCKS ARE DISTINCT, and why neither format test can
// stand without it.
//
// Both tests above ask a question about ONE block and average the answer, so a
// raster made of a single repeated block answers it perfectly. mp_granite ships
// exactly that: its node trailers are [colour tile][filler][filler], where each
// filler is 1,089 copies of one BC1 block whose two endpoints happen to be
// ordered. bc1_ordered_frac scores the filler 1.000 and the real photograph
// 0.994, so the best-window scan picked the filler on every node with a
// multi-tile trailer, and the level's colour map came back as fields of flat
// navy over the whole playable centre of the map.
//
// A photograph never repeats a block: measured, real tiles return 0.98-1.00
// here on both codecs and every filler window returns 0.004 (one or two
// distinct blocks in 256 samples). Multiplied into the format test it separates
// the three populations completely - real tile 0.98+, weight page under 0.50,
// filler under 0.01 - with no new threshold to choose.
double block_variety(const std::vector<uint8_t>& d, size_t start, int block_bytes)
{
    std::set<uint64_t> seen;
    int n = 0;
    for (size_t b = start; b + 8 <= d.size() && n < 256; b += (size_t)block_bytes * 4, n++)
    {
        uint64_t v;
        std::memcpy(&v, d.data() + b, 8);
        seen.insert(v);
    }
    return n ? (double)seen.size() / (double)n : 0.0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Shared readers. Both are line-for-line what terrain.cpp does privately.
// ---------------------------------------------------------------------------
bool Splat::find_block(const std::vector<uint8_t>& res, int type,
                       std::vector<uint8_t>& out, std::string& err)
{
    if (res.size() < kResHeader) { err = "shorter than the 34-byte container header"; return false; }
    size_t o = kResHeader;
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
        if (t == type)
        {
            out.assign(res.begin() + (ptrdiff_t)(o + 5),
                       res.begin() + (ptrdiff_t)(o + 5 + (size_t)sz));
            return true;
        }
        o += 5 + (size_t)sz;
    }
    char m[64];
    std::snprintf(m, sizeof(m), "no block %d in this terrain resource", type);
    err = m;
    return false;
}

namespace {

bool read_dir_node(const std::vector<uint8_t>& res, size_t& p, uint64_t key,
                   SplatChunkDir& out, std::string& err)
{
    if (!fits(res, p, 21)) { err = "directory node past the end"; return false; }
    SplatDirEntry e;
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
    p += 1;                                    // persistentDedicatedServer
    const uint8_t has_children = res[p++];
    out[key] = std::move(e);
    if (has_children)
        for (int i = 0; i < 4; i++)
            if (!read_dir_node(res, p, (key << 4) | (uint64_t)i, out, err)) return false;
    return true;
}

}  // namespace

bool Splat::read_chunk_dir(const std::vector<uint8_t>& res, SplatChunkDir& out,
                           std::string& err)
{
    out.clear();
    size_t o = kResHeader;
    while (o + 5 <= res.size())
    {
        if (res[o] == 0xFF) { o += 1; break; }
        const int32_t sz = rd<int32_t>(res, o + 1);
        if (sz < 0 || o + 5 + (size_t)sz > res.size())
        { err = "block overruns while looking for the directory"; return false; }
        o += 5 + (size_t)sz;
    }
    size_t p = o;
    if (!read_dir_node(res, p, 3, out, err)) return false;
    if (p != res.size())
    {
        char m[112];
        std::snprintf(m, sizeof(m), "chunk directory consumed %zu of %zu bytes", p, res.size());
        err = m;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// §5.1 the metadata quadtree.
// ---------------------------------------------------------------------------
bool Splat::read_node(const std::vector<uint8_t>& b, size_t& p, uint64_t key, int depth,
                      std::string& err)
{
    if (!fits(b, p, 6)) { err = "node header past the end"; return false; }
    const int rec_count = rd<uint16_t>(b, p);
    const int stored    = rd<uint16_t>(b, p + 2);
    p += 6;
    if (!fits(b, p, (size_t)rec_count * kRecordSize))
    { err = "node records past the end"; return false; }

    SplatNode n;
    n.key = key;
    n.depth = depth;
    // A node stores NO bounds of its own - only its records carry world rects -
    // so the box comes from the quadtree path, which the key already is. Filled
    // in one pass after the walk rather than threaded through the recursion.
    n.records.reserve((size_t)rec_count);
    int page_ix = 0;
    for (int i = 0; i < rec_count; i++)
    {
        const size_t o = p + (size_t)i * kRecordSize;
        SplatRecord r;
        r.layer = rd<uint16_t>(b, o);
        r.id    = rd<uint16_t>(b, o + 2);
        r.lo[0] = rd<float>(b, o + 4);
        r.lo[1] = rd<float>(b, o + 8);
        r.hi[0] = rd<float>(b, o + 12);
        r.hi[1] = rd<float>(b, o + 16);
        r.flags = rd<uint16_t>(b, o + 20);
        // LAYER 0 IS A REAL LAYER. Nothing here treats it as a pass-through or
        // skips it when a base material is resolved; the hub is explicit that an
        // earlier "layer 0 means inherit" reading was wrong.
        const bool has_page = (r.flags & kNoPageFlag) == 0;
        r.page = has_page ? page_ix++ : -1;
        n.records.push_back(r);
    }
    p += (size_t)rec_count * kRecordSize;

    // The declared stored-page count is a FREE CHECK on the flag offset: read
    // the flags four bytes out and page_ix changes while nothing else complains.
    if (stored != page_ix)
    {
        char m[128];
        std::snprintf(m, sizeof(m), "node %llu declares %d stored pages, its records give %d",
                      (unsigned long long)key, stored, page_ix);
        err = m;
        return false;
    }
    n.pages = page_ix;
    nodes_.push_back(std::move(n));

    if (rec_count == 0)
    {
        if (p < b.size()) p += 1;
        return true;
    }
    if (!fits(b, p, 2)) { err = "node flags past the end"; return false; }
    const uint8_t has_children = b[p + 1];
    p += 2;
    if (p < b.size()) p += 1;                  // t1, omitted only on the terminal node
    if (has_children)
        for (int i = 0; i < 4; i++)
            if (!read_node(b, p, (key << 4) | (uint64_t)i, depth + 1, err)) return false;
    return true;
}

bool Splat::parse(const std::vector<uint8_t>& b1, std::string& err)
{
    err.clear();
    nodes_.clear();
    by_key_.clear();
    if (b1.size() < kBlock1Head)
    { err = "block 1 shorter than its 61-byte header"; return false; }

    root_min_[0] = rd<float>(b1, 0x18);
    root_min_[1] = rd<float>(b1, 0x1C);
    root_max_[0] = rd<float>(b1, 0x20);
    root_max_[1] = rd<float>(b1, 0x24);
    layer_slot_count_ = (int)rd<uint32_t>(b1, 0x28);
    declared_nodes_   = (int)rd<uint32_t>(b1, 0x2C);
    declared_records_ = (int)rd<uint32_t>(b1, 0x30);

    size_t p = kBlock1Head;
    if (!read_node(b1, p, 3, 0, err)) return false;
    for (SplatNode& n : nodes_) bounds_of(n.key, root_min_, root_max_, n.lo, n.hi);

    if (p != b1.size())
    {
        // Specified byte-exact. Short of that the record stream has slipped and
        // the layer indices belong to somebody else - which looks entirely
        // plausible right up until the ground is the wrong material.
        char m[96];
        std::snprintf(m, sizeof(m), "walk consumed %zu of %zu bytes", p, b1.size());
        err = m;
        return false;
    }
    if ((int)nodes_.size() != declared_nodes_)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "walked %zu nodes, header declares %d",
                      nodes_.size(), declared_nodes_);
        err = m;
        return false;
    }
    for (size_t i = 0; i < nodes_.size(); i++) by_key_[nodes_[i].key] = i;
    return true;
}

int Splat::depth_of(uint64_t key)
{
    int d = 0;
    for (uint64_t k = key; k > 3; k >>= 4) d++;
    return d;
}

void Splat::bounds_of(uint64_t key, const float root_lo[2], const float root_hi[2],
                      float lo[2], float hi[2])
{
    int path[16];
    int n = 0;
    for (uint64_t k = key; k > 3 && n < 16; k >>= 4) path[n++] = (int)(k & 0xF);
    lo[0] = root_lo[0]; lo[1] = root_lo[1];
    hi[0] = root_hi[0]; hi[1] = root_hi[1];
    for (int i = n - 1; i >= 0; i--)          // path was collected leaf-first
    {
        const float half[2] = { (hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f };
        lo[0] += half[0] * (float)kChildX[path[i]];
        lo[1] += half[1] * (float)kChildZ[path[i]];
        hi[0] = lo[0] + half[0];
        hi[1] = lo[1] + half[1];
    }
}

size_t Splat::stored_pages() const
{
    size_t n = 0;
    for (const SplatNode& s : nodes_) n += (size_t)s.pages;
    return n;
}

int Splat::page_side() const { return kPageSide; }

// ---------------------------------------------------------------------------
// WHICH PAGE CODEC THIS MAP USES, and where the pages start in each chunk.
//
// Neither is stored. The test that decides it has to be able to FAIL, and the
// first one here could not: it counted nodes where
// `primary_size - tile - pages*page_size >= 0`, which is monotonically
// decreasing in both parameters, so the smallest pair (2592, 4624) won on every
// map. That was wrong on nine of sixteen, and the visible symptom was a colour
// "tile" sliced out of the middle of a different raster.
//
// The decisive test, measured across maps: for the TRUE page size the per-node
// residual `primary_size - pages*page_size` collapses to a handful of distinct
// values (Tungsten 3, Aftermath 3) while every wrong page size scatters into
// dozens, many negative. Pick the page size with the fewest distinct residuals,
// then decompose those residuals as `prefix + k*tile_bytes`.
//
// WHAT THE PREFIX ACTUALLY IS, per the research hub, and this is where the
// GDScript's comments are wrong even though its arithmetic is right: a chunk is
// a sequence of PLANES consumed in ASCENDING index, not the fixed
// [block0][pages][block2][tiles] the GDScript asserts. Plane 2 (Density /
// DetailDisplacement, present on 8k maps - NOT a water heightfield, and not a
// rivers property) may sit before the colour plane, and plane 5 may follow it.
// So "the prefix" is the total size of whatever planes precede the weight
// pages, which is why the prefix set below contains sums. The consequence for
// this code is exactly one thing, and it is the thing that matters: THE PAGES
// ARE SLICED FORWARD from the end of the preceding planes, never backward from
// the end of the chunk. End-relative slicing lands on a later plane on every
// map that has one, and it did: it read plane 2 as weight pages on precisely
// the nodes that carry plane 2.
// ---------------------------------------------------------------------------
namespace {

// Where the weight pages start given a prefix. 298,594 is two 149,297 planes
// and the pages follow the FIRST of them.
int fwd_of(int prefix) { return prefix == 298594 ? 149297 : prefix; }

// [k tiles, chosen prefix], or k < 0 when no prefix decomposes the residual.
// k == 0 is legal: planes plus pages and no colour tile.
//
// `slack` allows a TRAILING remainder smaller than one tile. Some chunks carry
// a short variable payload after the tile - mp_subsurface has eight, of 1,077
// to 1,124 bytes, against twenty-one that decompose exactly - and with slack 0
// those eight drag the map's fit to 72% and veto a model that is right for the
// other twenty-one. The remainder is kept INSIDE the reported trailer, which is
// what keeps the slice correct: the trailer is measured backward from the end
// of the chunk, so `prefix + pages + tile + tail` still puts window 0 of the
// scan exactly on the tile.
void decomp(int residual, int tb, int& best_k, int& best_p, bool slack = false)
{
    best_k = -1; best_p = 0;
    int best_rem = 0;
    for (int p : kPrefixes)
    {
        const int rem = residual - p;
        if (rem < 0) continue;
        const int tail = rem % tb;
        if (tail != 0 && !slack) continue;
        const int k = rem / tb;
        if (k > 8) continue;
        if (slack && tail != 0 && k < 1) continue;   // a bare remainder is not a tile
        // Fewest tiles wins: a residual readable as either "big prefix + 1 tile"
        // or "no prefix + many tiles" is plane data plus one tile, not a stack.
        if (best_k < 0 || k < best_k || (k == best_k && tail < best_rem))
        { best_k = k; best_p = p; best_rem = tail; }
    }
}

int mip_fits(int residual)
{
    for (int p : kPrefixes) if (residual - p == 85024) return p;
    return -1;
}

}  // namespace

bool Splat::detect_layout(const SplatChunkDir& dir, std::string& err)
{
    err.clear();
    trailer_.clear(); mip_.clear(); pfx_.clear();
    tail_const_ = -1; no_colour_ = false;
    page_size_ = 0; tile_bytes_ = 0; tile_side_ = 0;

    // ---- the page size: fewest distinct residuals, none negative -----------
    // Residuals are weighted by NODE COUNT so the tolerance below can reason in
    // nodes: one map was found with 8 of 107 chunks carrying a variable extra
    // payload no algebra fits, and a fit test over distinct values alone would
    // let those 8 veto the model for the other 99.
    // A level can legitimately store NO pages: mp_portal_lobby ships one node,
    // four records, zero stored pages and a single base layer with nothing
    // painted on it. That is an empty terrain, not a layout we failed to fit,
    // and saying "no page size fits" sent an earlier fleet audit looking for a
    // parser bug that was not there. Separate the two answers.
    int paged_nodes = 0;
    for (const SplatNode& n : nodes_) if (n.pages > 0) paged_nodes++;
    if (paged_nodes == 0) {
        err = "the level stores no splat pages: nothing is painted on this terrain";
        return false;
    }

    std::map<int, int> best_resid;
    int best_ps = 0;
    for (int ps : kPageSizes)
    {
        std::map<int, int> resid;
        bool ok = true;
        for (const SplatNode& n : nodes_)
        {
            if (n.pages <= 0) continue;
            auto e = dir.find(n.key);
            if (e == dir.end() || e->second.primary_size == 0) continue;
            const int r = (int)e->second.primary_size - n.pages * ps;
            if (r < 0) { ok = false; break; }   // pages do not fit their own chunk
            resid[r]++;
        }
        if (!ok || resid.empty()) continue;
        if (best_ps == 0 || resid.size() < best_resid.size())
        { best_ps = ps; best_resid = resid; }
    }
    if (best_ps == 0) { err = "no page size fits the per-node chunk sizes"; return false; }
    page_size_ = best_ps;

    int total = 0;
    for (const auto& kv : best_resid) total += kv.second;

    // ---- MODEL A: the trailer is k colour tiles ----------------------------
    // Tried before model B because a constant can partially fit a tile map. A
    // 90% node fit accepts, so a handful of irregular chunks lose their own tile
    // and pages without vetoing the whole map.
    int best_fit = 0, best_exact = -1;
    // Pass 0 is the exact model. Pass 1 repeats it allowing a sub-tile trailing
    // remainder, and only runs when the exact one accepted nothing, so no map
    // that already decomposes exactly can change answer.
    //
    // In pass 1 the tie-break is the count of nodes that decompose EXACTLY, not
    // the count that decompose at all. Slack makes every tile size fit almost
    // everything - on mp_subsurface a residual of 166,721 reads as 2 x 67,600
    // plus 31,521 of slop as happily as it reads as 149,297 + 1 x 17,424 exactly
    // - and kTileOrder tries the largest first, so without this the slack model
    // hands back 260-square tiles sliced out of the middle of a plane.
    for (int pass = 0; pass < 2 && best_fit == 0; pass++)
    for (int tb : kTileOrder)
    {
        std::map<int, int> t_of, t_mip, t_pf;
        int okn = 0, exact = 0;
        for (const auto& kv : best_resid)
        {
            const int r = kv.first;
            int k, pfx;
            decomp(r, tb, k, pfx, pass == 1);
            if (k >= 0)
            {
                t_of[r] = r - pfx;                  // the remainder rides along
                t_pf[r] = fwd_of(pfx);
                okn += kv.second;
                if ((r - pfx) % tb == 0) exact += kv.second;
            }
            else if (tb == 17424 || tb == 67600)
            {
                const int mp = mip_fits(r);
                if (mp >= 0)
                {
                    t_of[r]  = 85024;
                    // WHICH component of a 260²+132² mip pair is "the" colour
                    // depends on the map's tile size: beside 132² tiles the
                    // pair's TAIL mip matches scale (offset 67,600); on a 260²
                    // map the pair's HEAD is the tile (offset 0).
                    t_mip[r] = (tb == 17424) ? 67600 : 0;
                    t_pf[r]  = fwd_of(mp);
                    okn += kv.second;
                }
            }
        }
        const bool better = pass == 0 ? okn > best_fit
                                      : (exact > best_exact ||
                                         (exact == best_exact && okn > best_fit));
        if (okn * 10 >= total * 9 && better)
        {
            best_fit    = okn;
            best_exact  = exact;
            tile_bytes_ = tb;
            tile_side_  = tile_side_of(tb);
            trailer_ = t_of; mip_ = t_mip; pfx_ = t_pf;
        }
    }
    if (tile_bytes_ > 0) return true;

    // ---- MODEL B: the trailer is a CONSTANT, and it is not a tile ----------
    // One map's primary chunks all end in the same 936 bytes of an unknown
    // raster, and its colour tiles live ONLY in the paired chunks.
    std::map<int, int> cand;
    for (const auto& kv : best_resid)
        for (int p : kPrefixes)
            if (kv.first - p >= 0) cand[kv.first - p] += kv.second;

    int best_c = -1, best_cn = 0;
    for (const auto& kv : cand) if (kv.second > best_cn) { best_cn = kv.second; best_c = kv.first; }
    if (best_c < 0 || best_cn * 10 < total * 9)
    {
        char m[160];
        std::snprintf(m, sizeof(m),
            "page size %d fits but no trailer model covers its %zu residuals",
            page_size_, best_resid.size());
        err = m;
        return false;
    }
    tail_const_ = best_c;
    for (const auto& kv : best_resid)
        for (int p : kPrefixes)
            if (kv.first - p == best_c)
            { trailer_[kv.first] = best_c; pfx_[kv.first] = fwd_of(p); break; }

    // The colour tile's size from the PAIRED trailers: four child tiles each, so
    // a quarter of the paired remainder names the tile.
    for (const auto& kv : dir)
    {
        if (kv.second.paired.empty() || kv.second.paired_size == 0) continue;
        int kids_pages = 0;
        bool known = true;
        for (int i = 0; i < 4; i++)
        {
            auto s = by_key_.find((kv.first << 4) | (uint64_t)i);
            if (s == by_key_.end()) { known = false; break; }
            kids_pages += nodes_[s->second].pages;
        }
        if (!known) continue;
        const int tp = (int)kv.second.paired_size - kids_pages * page_size_;
        if (tp > 0 && tp % 4 == 0 && tile_side_of(tp / 4) > 0)
        { tile_bytes_ = tp / 4; tile_side_ = tile_side_of(tile_bytes_); break; }
    }
    // NO COLOUR ANYWHERE IS A LEGAL OUTCOME: the weight pages and the layer
    // lists still decode, so the surface builds without a colour map rather than
    // failing outright.
    if (tile_bytes_ <= 0) no_colour_ = true;
    return true;
}

int Splat::trailer_bytes(uint32_t primary_size, int pages) const
{
    const int r = (int)primary_size - pages * page_size_;
    if (r < 0) return -1;
    auto it = trailer_.find(r);
    if (it != trailer_.end()) return it->second;
    // On-demand decomposition mirrors the model detect_layout settled on:
    // constant tail, mip pair, then k tiles - in that order of specificity.
    if (tail_const_ >= 0)
    {
        for (int p : kPrefixes)
            if (r - p == tail_const_)
            { trailer_[r] = tail_const_; pfx_[r] = fwd_of(p); return tail_const_; }
        return -1;
    }
    if (tile_bytes_ == 17424 || tile_bytes_ == 67600)
    {
        const int mp = mip_fits(r);
        if (mp >= 0)
        {
            trailer_[r] = 85024;
            mip_[r] = (tile_bytes_ == 17424) ? 67600 : 0;
            pfx_[r] = fwd_of(mp);
            return 85024;
        }
    }
    if (tile_bytes_ <= 0) return -1;
    int k, pfx;
    decomp(r, tile_bytes_, k, pfx);
    if (k < 0) return -1;
    trailer_[r] = k * tile_bytes_;
    pfx_[r]     = fwd_of(pfx);
    return k * tile_bytes_;
}

int Splat::pages_offset(uint32_t primary_size, int pages) const
{
    if (trailer_bytes(primary_size, pages) < 0) return -1;
    auto it = pfx_.find((int)primary_size - pages * page_size_);
    return it == pfx_.end() ? -1 : it->second;
}

// ---------------------------------------------------------------------------
// §5.2: one stored page -> a 66x66 byte weight grid.
//
//   2,592  BC4 72x72 with a 3-px apron   -> decompress, crop at (3,3)
//   4,356  raw 66x66                     -> as-is
//   5,184  raw 72x72 with a 3-px apron   -> crop at (3,3)
// ---------------------------------------------------------------------------
bool Splat::decode_page(const uint8_t* raw, int size, uint8_t* out66)
{
    if (size == 4356) { std::memcpy(out66, raw, (size_t)kPageSide * kPageSide); return true; }
    if (size == 5184) { crop66(raw, 72, 3, out66); return true; }
    if (size == 2592)
    {
        uint8_t full[72 * 72];
        if (!bc4_decode(raw, 72, full)) return false;
        crop66(full, 72, 3, out66);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// A node's stored weight pages, as pointers into a cached chunk.
//
// Primary chunk: FORWARD from the end of the preceding planes (see the long
//   note on detect_layout). Never backward from the end of the chunk.
// Paired chunk:  PLANE-MAJOR with the four children in reversed order
//   [3,2,1,0]: all four children's weight pages from offset 0, then all four
//   colour tiles. Paired packs carry no plane prefix. The hub settles what the
//   GDScript left open as a switch (`paired_child_swap`, choosing between
//   [3,2,1,0] and traversal order [2,3,1,0]): it is plain reversed index, so
//   the switch is gone rather than ported.
// ---------------------------------------------------------------------------
namespace {

struct ChunkCache {
    const Splat::FetchChunk* fetch = nullptr;
    std::map<std::string, std::vector<uint8_t>> map;

    const std::vector<uint8_t>& get(const std::string& guid)
    {
        static const std::vector<uint8_t> kEmpty;
        if (guid.empty()) return kEmpty;
        auto it = map.find(guid);
        if (it != map.end()) return it->second;
        return map.emplace(guid, (*fetch)(guid)).first->second;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// EVERY WEIGHT PAGE, RASTERISED INTO ONE TOP-4 SPLAT MAP.
//
// Coarse nodes first, so a deeper node's finer page overwrites what a coarse one
// laid down (§5.4). Per texel the four strongest layers are kept.
//
// TWO PASSES: DECODE ONCE, THEN PAINT IN BANDS THAT SHARE NOTHING. Order is
// preserved exactly, which is what makes the bands safe: the merge keeps the top
// four BY WEIGHT, so the sequence of records touching a texel decides the
// answer. Every band walks the same flat job list in the same order and a texel
// belongs to exactly one band, so each texel sees precisely the sequence it saw
// when this was one serial loop.
// ---------------------------------------------------------------------------
namespace {

struct PaintJob {
    std::vector<uint8_t> page;     // 66*66
    int   layer;
    float lo[2], hi[2];
    int   x0, x1, z0, z1;
};

}  // namespace

bool Splat::composite(const SplatChunkDir& dir, const FetchChunk& fetch, int size,
                      SplatCoverage& out, std::string& err,
                      const SplatCompositeOpts& opt) const
{
    err.clear();
    if (size <= 0) { err = "composite needs a raster size"; return false; }
    if (page_size_ <= 0) { err = "detect_layout has not run"; return false; }

    float span[2] = { root_max_[0] - root_min_[0], root_max_[1] - root_min_[1] };
    if (span[0] <= 0.f || span[1] <= 0.f)
    { err = "block 1 has an empty world bounds"; return false; }
    float org[2] = { root_min_[0], root_min_[1] };
    const bool windowed = opt.rect_size > 0.f;
    if (windowed)
    {
        org[0] = opt.rect_min[0]; org[1] = opt.rect_min[1];
        span[0] = opt.rect_size;  span[1] = opt.rect_size;
    }

    // Coarse first, and STABLE. The merge keeps the top four by weight, so two
    // records of equal weight at a texel are decided by which is painted last;
    // the GDScript sorts with Godot's Array.sort_custom, which is NOT stable, so
    // its tie-breaks are whatever the sort happened to do that run. This is the
    // one place the port is deliberately more determinate than the original, and
    // it is why a handful of texels out of 16.7 million differ between the two.
    std::vector<const SplatNode*> order;
    order.reserve(nodes_.size());
    for (const SplatNode& n : nodes_) order.push_back(&n);
    std::stable_sort(order.begin(), order.end(),
        [](const SplatNode* a, const SplatNode* b) { return a->depth < b->depth; });

    // ---- pass one: fetch and decode, in traversal order --------------------
    // Serial on purpose: it does the I/O, and doing it once here is what lets
    // every band read the pages without repeating it.
    ChunkCache cache;
    cache.fetch = &fetch;
    std::vector<PaintJob> jobs;
    int decoded = 0;

    for (const SplatNode* n : order)
    {
        if (n->pages <= 0) continue;
        if (windowed)
        {
            bool hit = false;
            for (const SplatRecord& r : n->records)
            {
                if (opt.max_span > 0.f && (r.hi[0] - r.lo[0]) > opt.max_span) continue;
                if (r.hi[0] > org[0] && r.hi[1] > org[1] &&
                    r.lo[0] < org[0] + span[0] && r.lo[1] < org[1] + span[1])
                { hit = true; break; }
            }
            if (!hit) continue;
        }

        // ---- the node's page bytes ----
        const uint8_t* base = nullptr;
        size_t         avail = 0;
        auto de = dir.find(n->key);
        if (de != dir.end() && !de->second.primary.empty())
        {
            const int fwd = pages_offset(de->second.primary_size, n->pages);
            if (fwd >= 0)
            {
                const size_t need = (size_t)fwd + (size_t)n->pages * (size_t)page_size_;
                if ((size_t)de->second.primary_size >= need)
                {
                    const std::vector<uint8_t>& d = cache.get(de->second.primary);
                    if (d.size() >= need) { base = d.data() + fwd; avail = d.size() - (size_t)fwd; }
                }
            }
        }
        if (!base)
        {
            // The parent's paired pack.
            auto pe = dir.find(n->key >> 4);
            if (pe == dir.end() || pe->second.paired.empty()) continue;
            const std::vector<uint8_t>& pd = cache.get(pe->second.paired);
            if (pd.empty()) continue;
            const uint64_t child = n->key & 0xF;
            size_t off = 0;
            for (int j = 3; j >= 0; j--)       // reversed child order [3,2,1,0]
            {
                if ((uint64_t)j == child) break;
                auto s = by_key_.find((n->key & ~(uint64_t)0xF) | (uint64_t)j);
                if (s != by_key_.end()) off += (size_t)nodes_[s->second].pages * (size_t)page_size_;
            }
            if (off + (size_t)n->pages * (size_t)page_size_ > pd.size()) continue;
            base = pd.data() + off;
            avail = pd.size() - off;
        }
        if (!base || avail < (size_t)n->pages * (size_t)page_size_) continue;

        for (const SplatRecord& r : n->records)
        {
            if (r.page < 0 || r.page >= n->pages) continue;
            if (opt.max_span > 0.f && (r.hi[0] - r.lo[0]) > opt.max_span) continue;
            if (r.hi[0] <= org[0] || r.hi[1] <= org[1] ||
                r.lo[0] >= org[0] + span[0] || r.lo[1] >= org[1] + span[1]) continue;

            PaintJob j;
            j.page.resize((size_t)kPageSide * kPageSide);
            if (!decode_page(base + (size_t)r.page * (size_t)page_size_, page_size_,
                             j.page.data())) continue;
            decoded++;
            int x0 = (int)std::floor((r.lo[0] - org[0]) / span[0] * (float)size);
            int x1 = (int)std::ceil ((r.hi[0] - org[0]) / span[0] * (float)size);
            int z0 = (int)std::floor((r.lo[1] - org[1]) / span[1] * (float)size);
            int z1 = (int)std::ceil ((r.hi[1] - org[1]) / span[1] * (float)size);
            x0 = std::clamp(x0, 0, size - 1); x1 = std::clamp(x1, 0, size);
            z0 = std::clamp(z0, 0, size - 1); z1 = std::clamp(z1, 0, size);
            if (x1 <= x0 || z1 <= z0) continue;
            j.layer = (int)(r.layer & 0xFF);
            j.lo[0] = r.lo[0]; j.lo[1] = r.lo[1];
            j.hi[0] = r.hi[0]; j.hi[1] = r.hi[1];
            j.x0 = x0; j.x1 = x1; j.z0 = z0; j.z1 = z1;
            jobs.push_back(std::move(j));
        }
    }

    const int slots = std::clamp(opt.max_slots, 1, 16);
    const size_t cells = (size_t)size * (size_t)size * (size_t)slots;
    const bool seeded = opt.seeded && out.idx.size() == cells && out.w.size() == cells;
    if (!seeded) { out.idx.assign(cells, 0); out.w.assign(cells, 0); }
    out.size = size;
    out.slots = slots;
    out.lo[0] = org[0]; out.lo[1] = org[1];
    out.hi[0] = org[0] + span[0]; out.hi[1] = org[1] + span[1];
    out.pages_painted = decoded;

    // ---- pass two: paint --------------------------------------------------
    int nthread = opt.threads;
    if (nthread <= 0) nthread = (int)std::max(1u, std::thread::hardware_concurrency());
    if (jobs.empty()) nthread = 1;
    nthread = std::min(nthread, size);
    const int band_h = (size + nthread - 1) / nthread;

    const float inner = (float)(kPageSide - 2);       // 64 interior texels
    uint8_t* pidx = out.idx.data();
    uint8_t* pw   = out.w.data();

    // WHAT THE FIXED-WIDTH MERGE THREW AWAY. The game keeps a mask per layer and
    // evaluates every layer in the tile's work list. Counting the rejects is
    // the only way to know whether the requested width is sufficient.
    std::vector<uint64_t> band_evict((size_t)nthread, 0);
    std::vector<double>   band_evict_w((size_t)nthread, 0.0);

    auto band = [&](int b)
    {
        uint64_t evicted = 0;
        double   evicted_w = 0.0;
        const int zlo = b * band_h;
        const int zhi = std::min(size, zlo + band_h);
        for (const PaintJob& j : jobs)
        {
            const int jz0 = std::max(j.z0, zlo);
            const int jz1 = std::min(j.z1, zhi);
            if (jz1 <= jz0) continue;
            const float rw = std::max(j.hi[0] - j.lo[0], 1e-6f);
            const float rh = std::max(j.hi[1] - j.lo[1], 1e-6f);
            const uint8_t* page = j.page.data();
            const int layer = j.layer;
            for (int gz = jz0; gz < jz1; gz++)
            {
                const float wz = org[1] + ((float)gz + 0.5f) / (float)size * span[1];
                const float fz = std::clamp((wz - j.lo[1]) / rh, 0.f, 1.f);
                // Both per-row setups, computed once per row. The smooth one uses
                // continuous page coords: interior texel i's centre sits at
                // fz = (i+0.5)/64 and page row 1+i, so cy = fz*64+0.5 hits it
                // exactly, and the apron makes the edge taps valid without
                // clamping artefacts.
                const float cy = fz * inner + 0.5f;
                const int   iy = std::clamp((int)(cy - 0.5f), 0, kPageSide - 2);
                const float ty = std::clamp(cy - 0.5f - (float)iy, 0.f, 1.f);
                const int   nrow = (1 + std::clamp((int)(fz * inner), 0, kPageSide - 3)) * kPageSide;
                const size_t drow = (size_t)gz * (size_t)size;
                for (int gx = j.x0; gx < j.x1; gx++)
                {
                    const float wx = org[0] + ((float)gx + 0.5f) / (float)size * span[0];
                    const float fx = std::clamp((wx - j.lo[0]) / rw, 0.f, 1.f);
                    int w;
                    if (opt.smooth)
                    {
                        const float cx = fx * inner + 0.5f;
                        const int   ix = std::clamp((int)(cx - 0.5f), 0, kPageSide - 2);
                        const float tx = std::clamp(cx - 0.5f - (float)ix, 0.f, 1.f);
                        const int   o00 = iy * kPageSide + ix;
                        const float a = (float)page[o00] + ((float)page[o00 + 1] - (float)page[o00]) * tx;
                        const float bb = (float)page[o00 + kPageSide]
                            + ((float)page[o00 + kPageSide + 1] - (float)page[o00 + kPageSide]) * tx;
                        w = (int)(a + (bb - a) * ty + 0.5f);
                    }
                    else
                    {
                        w = page[nrow + 1 + std::clamp((int)(fx * inner), 0, kPageSide - 3)];
                    }
                    // ---- the strongest-N merge, inlined ----
                    // The same layer is redeclared down the quadtree. Records are
                    // ordered coarse-to-fine, so the later/finer sample REPLACES
                    // the ancestor sample. Zero is load-bearing: it retracts the
                    // coarse layer inside the fine record's rectangle. Skipping
                    // zero here left ancestor maxima alive and produced the map-
                    // scale "quilt" on MP_Isolated (at -778.962,412.790, L22's
                    // coarse max is 1.0 while its finest sample is 0.0).
                    const size_t o = (drow + (size_t)gx) * (size_t)slots;
                    int at = -1;
                    for (int s = 0; s < slots; s++)
                        if (pw[o + s] > 0 && pidx[o + s] == layer) { at = s; break; }
                    int put = -1;
                    if (at >= 0)
                    {
                        if (w <= 0)
                        {
                            for (int s = at; s + 1 < slots; s++)
                            {
                                pidx[o + s] = pidx[o + s + 1];
                                pw[o + s] = pw[o + s + 1];
                            }
                            pidx[o + slots - 1] = 0;
                            pw[o + slots - 1] = 0;
                            continue;
                        }
                        pw[o + at] = (uint8_t)w;
                        put = at;
                    }
                    else
                    {
                        if (w <= 0) continue;
                        int freeslot = -1;
                        for (int s = 0; s < slots; s++)
                            if (pw[o + s] == 0) { freeslot = s; break; }
                        if (freeslot >= 0)
                        {
                            pidx[o + freeslot] = (uint8_t)layer;
                            pw[o + freeslot] = (uint8_t)w;
                            put = freeslot;
                        }
                        else if (w > pw[o + slots - 1])
                        {
                            evicted++; evicted_w += (double)pw[o + slots - 1] / 255.0;
                            pidx[o + slots - 1] = (uint8_t)layer;
                            pw[o + slots - 1] = (uint8_t)w;
                            put = slots - 1;
                        }
                        else {
                            evicted++;
                            evicted_w += (double)w / 255.0;
                            // The candidate lost and was not inserted, so
                            // `put` deliberately remains -1.  Falling through
                            // to the insertion-sort loops below indexes slot
                            // -1: dense Tsuru windows then corrupt the vector
                            // beside the raster and fail later during cleanup.
                            // A sparse control never fills all slots and hides
                            // the bug, which is why whole-map runs appeared
                            // healthy while the camera-relative probe died.
                            continue;
                        }
                    }
                    for (int k = put; k > 0 && pw[o + k] > pw[o + k - 1]; k--)
                    {
                        std::swap(pw[o + k], pw[o + k - 1]);
                        std::swap(pidx[o + k], pidx[o + k - 1]);
                        put = k - 1;
                    }
                    for (int k = put; k + 1 < slots && pw[o + k + 1] > pw[o + k]; k++)
                    {
                        std::swap(pw[o + k], pw[o + k + 1]);
                        std::swap(pidx[o + k], pidx[o + k + 1]);
                    }
                }
            }
        }
        band_evict[(size_t)b] = evicted;
        band_evict_w[(size_t)b] = evicted_w;
    };

    if (nthread > 1)
    {
        std::vector<std::thread> th;
        th.reserve((size_t)nthread);
        for (int b = 0; b < nthread; b++) th.emplace_back(band, b);
        for (std::thread& t : th) t.join();
    }
    else band(0);

    // ---- tally --------------------------------------------------------------
    out.slot_evictions = 0;
    out.evicted_weight = 0.0;
    for (int b = 0; b < nthread; b++)
    { out.slot_evictions += band_evict[(size_t)b]; out.evicted_weight += band_evict_w[(size_t)b]; }
    std::memset(out.layer_texels, 0, sizeof(out.layer_texels));
    out.empty_texels = 0;
    for (size_t i = 0; i < (size_t)size * (size_t)size; i++)
    {
        const size_t o = i * (size_t)slots;
        if (pw[o] == 0) { out.empty_texels++; continue; }
        for (int s = 0; s < slots; s++)
        {
            if (pw[o + s] == 0) break;
            out.layer_texels[pidx[o + s]]++;
        }
    }
    out.layer_count = 0;
    for (int l = 0; l < 256; l++) if (out.layer_texels[l]) out.layer_count++;
    return true;
}

// ---------------------------------------------------------------------------
// THE COLOUR TILE SLICES.
//
// The trailer at the end of a node's primary chunk holds one or two tiles, and
// WHICH one is the colour is decided by SIGNATURE, not by position: on one map
// the colour is first and the second is a degenerate constant raster, but a
// single-tile map makes first and last coincide, so a position rule is one
// unstudied map away from being wrong.
//
// Paired packs group the four children's tiles by RASTER (plane-major), colour
// group first, in reversed child order [3,2,1,0]. Both sources are needed.
//
// See the header for the known shortfall against the hub's plane-4 extractor.
// The GDScript's own comment here claims 272 + 832 = 1,104 tiles on mp_dumbo;
// that number predates its detect_layout rewrite. Measured against the SHIPPING
// plugin as it stands today - its session logs - mp_aftermath yields 240 tiles,
// which is exactly what this returns.
// ---------------------------------------------------------------------------
std::vector<ColorSlice> Splat::color_slices(const SplatChunkDir& dir,
                                            const FetchChunk& fetch) const
{
    std::vector<ColorSlice> out;
    if (tile_bytes_ <= 0) return out;

    ChunkCache cache;
    cache.fetch = &fetch;
    const bool bc1 = tile_is_bc1();
    auto score = [&](const std::vector<uint8_t>& d, size_t at)
    {
        // The format test times block variety - see block_variety above. Without
        // the second factor a single-block filler raster beats the photograph it
        // is stacked next to, which is how mp_granite's colour map came back as
        // flat navy over the whole centre of the map.
        const double f = bc1 ? bc1_ordered_frac(d, at) : mode47_frac(d, at);
        return f * block_variety(d, at, bc1 ? 8 : 16);
    };
    // Thresholds sit in the wide gap between measured populations: real tiles
    // read >= 0.64 (BC7, on the mode-1-heavy maps) to ~1.0, page bytes <= 0.14
    // in both codecs. A tile that fails is weight-page data wearing a tile-sized
    // coat, and shipping it as a picture is the failure this gate exists for.
    auto looks_real = [&](const std::vector<uint8_t>& d, size_t at)
    { return score(d, at) >= (bc1 ? 0.6 : 0.4); };

    std::map<uint64_t, bool> have;
    for (const SplatNode& n : nodes_)
    {
        auto e = dir.find(n.key);
        if (e == dir.end() || e->second.primary.empty()) continue;
        const int trailer = trailer_bytes(e->second.primary_size, n.pages);
        if (trailer < tile_bytes_) continue;
        const std::vector<uint8_t>& d = cache.get(e->second.primary);
        if (d.size() < (size_t)trailer) continue;
        size_t start = d.size() - (size_t)trailer;
        const int resid = (int)e->second.primary_size - n.pages * page_size_;
        auto mp = mip_.find(resid);
        if (mp != mip_.end())
        {
            // A signature scan must NOT run on a mip pair: every window of the
            // big tile is itself a valid image, so the scan happily returns a
            // piece of the wrong-resolution mip.
            start += (size_t)mp->second;
        }
        else if (trailer > tile_bytes_)
        {
            size_t best_off = 0;
            double best = -1.0;
            for (int off = 0; off + tile_bytes_ <= trailer; off += tile_bytes_)
            {
                const double f = score(d, start + (size_t)off);
                if (f > best) { best = f; best_off = (size_t)off; }
            }
            start += best_off;
        }
        if (!looks_real(d, start)) continue;
        ColorSlice s;
        s.key = n.key; s.chunk = e->second.primary;
        s.offset = start; s.bytes = (size_t)tile_bytes_;
        out.push_back(s);
        have[n.key] = true;
    }

    for (const auto& kv : dir)
    {
        if (kv.second.paired.empty()) continue;
        int kids_pages = 0;
        bool known = true;
        for (int i = 0; i < 4; i++)
        {
            auto s = by_key_.find((kv.first << 4) | (uint64_t)i);
            if (s == by_key_.end()) { known = false; break; }
            kids_pages += nodes_[s->second].pages;
        }
        if (!known) continue;
        const std::vector<uint8_t>& d = cache.get(kv.second.paired);
        const long long trailer = (long long)d.size() - (long long)kids_pages * page_size_;
        if (trailer < (long long)tile_bytes_ * 4) continue;
        size_t base = d.size() - (size_t)trailer;
        if (trailer >= (long long)tile_bytes_ * 8)
        {
            size_t best_g = 0;
            double best = -1.0;
            for (long long g = 0; (g + 4) * tile_bytes_ <= trailer; g += 4)
            {
                const double f = score(d, base + (size_t)(g * tile_bytes_));
                if (f > best) { best = f; best_g = (size_t)g; }
            }
            base += best_g * (size_t)tile_bytes_;
        }
        // AN ABSOLUTE GATE, not just a relative pick: on one map the paired
        // chunks hold the DESCENDANT nodes' weight pages and no tiles at all, so
        // the "trailer" computed from the four direct children is really deeper
        // pages - and slicing it ships weight pages as colour.
        if (!looks_real(d, base)) continue;
        for (int slot = 0; slot < 4; slot++)
        {
            const uint64_t ck = (kv.first << 4) | (uint64_t)(3 - slot);
            if (have.count(ck)) continue;
            ColorSlice s;
            s.key = ck; s.chunk = kv.second.paired;
            s.offset = base + (size_t)slot * (size_t)tile_bytes_;
            s.bytes = (size_t)tile_bytes_;
            out.push_back(s);
            have[ck] = true;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// §8's three ordered lists.
// ---------------------------------------------------------------------------
std::vector<int> Splat::full_list() const
{
    std::map<int, bool> seen;
    for (const SplatNode& n : nodes_)
        for (const SplatRecord& r : n.records) seen[(int)r.layer] = true;
    std::vector<int> out;
    out.reserve(seen.size());
    for (const auto& kv : seen) out.push_back(kv.first);
    return out;                                   // std::map iterates ascending
}

std::vector<int> Splat::global_base_list() const
{
    std::map<int, bool> seen;
    for (const SplatNode& n : nodes_)
        for (const SplatRecord& r : n.records)
            if (r.page < 0) seen[(int)r.layer] = true;
    std::vector<int> out;
    out.reserve(seen.size());
    for (const auto& kv : seen) out.push_back(kv.first);
    return out;
}

// The base list of the block-1 node that SPATIALLY matches a block-7 node:
// descend from the root by the query centre until the node is no wider than the
// block-7 node (or has no child there), then take the DEEPEST node on that chain
// with a non-empty no-page list.
//
// Fleet-measured over 35 trees: the pair nibbles index the MATCHED NODE's own
// ascending list, and the map-global list names a different layer on 23 of the
// 35. Aftermath and outskirts cannot tell the two apart - every node list there
// IS the global list - which is how a global-list reading once validated and
// then quietly mis-coloured everything else. Key-equality matching into block 1
// is NOT reliable either: the two trees differ in depth and key space. Bounds
// are the invariant.
std::vector<int> Splat::base_list_at(float cx, float cz, float width) const
{
    auto it = by_key_.find(3);
    if (it == by_key_.end()) return {};
    std::vector<size_t> chain;
    size_t cur = it->second;
    chain.push_back(cur);
    for (;;)
    {
        const SplatNode& nd = nodes_[cur];
        const float w = nd.hi[0] - nd.lo[0];
        if (w <= width * 1.001f) break;
        const float half = w * 0.5f;
        const bool ex = cx >= nd.lo[0] + half;
        const bool ez = cz >= nd.lo[1] + half;
        int c = 0;
        if (ex && !ez) c = 1;
        else if (ex && ez) c = 2;
        else if (!ex && ez) c = 3;
        auto ch = by_key_.find((nd.key << 4) | (uint64_t)c);
        if (ch == by_key_.end()) break;
        cur = ch->second;
        chain.push_back(cur);
    }
    for (size_t i = chain.size(); i-- > 0;)
    {
        // DISTINCT layers: a node may carry several no-page records for one
        // layer (one per coverage rect), and a duplicate shifts every nibble
        // index after it.
        std::map<int, bool> seen;
        for (const SplatRecord& r : nodes_[chain[i]].records)
            if (r.page < 0) seen[(int)r.layer] = true;
        if (!seen.empty())
        {
            std::vector<int> out;
            out.reserve(seen.size());
            for (const auto& kv : seen) out.push_back(kv.first);
            return out;
        }
    }
    return {};
}

void Splat::layer_usage(std::map<int, int>& painted, std::map<int, int>& base) const
{
    painted.clear(); base.clear();
    for (const SplatNode& n : nodes_)
        for (const SplatRecord& r : n.records)
        {
            if (r.page >= 0) painted[(int)r.layer]++;
            else             base[(int)r.layer]++;
        }
}

// ===========================================================================
// BLOCK 7 / BLOCK 8
// ===========================================================================
namespace {

// §7.3's engine decoder at 0x145728250, byte for byte.
//
// Two 4-bit texels per RLE byte, low nibble first. The first byte of a row is
// the initial value; after that a byte EQUAL to the current value means the next
// byte is a run length in 2-texel groups, and a byte that differs is an implicit
// single group and becomes the current value.
//
// A clean row produces exactly `dim` texels AND consumes exactly its bytes, and
// both halves of that are checked: the length check alone passes on a stream
// that has slipped by an even number of groups.
bool decode_row(const std::vector<uint8_t>& d, size_t at, int rl, int dim,
                uint8_t* out)
{
    if (rl <= 0 || at + (size_t)rl > d.size()) return false;
    int src = 0;
    int current = d[at];
    src++;
    int groups = 0;
    for (int x = 0; x < dim; x += 2)
    {
        while (groups * 2 < x)
        {
            if (src >= rl) return false;
            const int b = d[at + (size_t)src]; src++;
            int run = 1;
            if (b == current)
            {
                if (src >= rl) return false;
                run = d[at + (size_t)src]; src++;
            }
            current = b;
            groups += run;
        }
        out[x] = (uint8_t)(current & 0xF);
        if (x + 1 < dim) out[x + 1] = (uint8_t)((current >> 4) & 0xF);
    }
    return src == rl;
}

}  // namespace

bool MaterialTree::read_node(uint64_t key, int depth, std::string& err)
{
    const std::vector<uint8_t>& d = *d_;
    if (p_ + 2 > d.size())
    { err = "node flags past the end"; return false; }
    const int has_data       = d[p_];
    const int has_persistent = d[p_ + 1];
    // §7.2: all three flag bytes are strictly 0 or 1 on every shipped block, so
    // anything else means the frame has slipped. Cheap, and it catches the
    // framing trap this grammar is famous for - the hasChildren flag TRAILS the
    // payload, and reading three leading flags limps along for most of a stream
    // before failing near the end.
    if (has_data > 1 || has_persistent > 1)
    {
        char m[128];
        std::snprintf(m, sizeof(m), "node %llu flags are %d/%d, not 0/1 - the frame has slipped",
                      (unsigned long long)key, has_data, has_persistent);
        err = m;
        return false;
    }
    p_ += 2;

    std::vector<uint8_t> rows;
    if (has_data && has_persistent)
    {
        if (p_ + 4 > d.size()) { err = "node payload size past the end"; return false; }
        const int32_t sz = rd<int32_t>(d, p_);
        p_ += 4;
        if (sz < 0 || p_ + (size_t)sz + (size_t)dim_ * 2 > d.size())
        { err = "node payload past the end"; return false; }
        const size_t payload_at = p_;
        p_ += (size_t)sz;
        const size_t sizes_at = p_;
        p_ += (size_t)dim_ * 2;
        int total = 0;
        for (int r = 0; r < dim_; r++) total += rd<uint16_t>(d, sizes_at + (size_t)r * 2);
        if (total != sz)
        {
            char m[128];
            std::snprintf(m, sizeof(m), "node %llu: row sizes sum to %d, payload is %d",
                          (unsigned long long)key, total, sz);
            err = m;
            return false;
        }
        rows.resize((size_t)dim_ * (size_t)dim_);
        size_t at = payload_at;
        for (int r = 0; r < dim_; r++)
        {
            const int rl = rd<uint16_t>(d, sizes_at + (size_t)r * 2);
            if (!decode_row(d, at, rl, dim_, rows.data() + (size_t)r * (size_t)dim_))
            {
                char m[96];
                std::snprintf(m, sizeof(m), "node %llu row %d does not decode cleanly",
                              (unsigned long long)key, r);
                err = m;
                return false;
            }
            at += (size_t)rl;
        }
    }

    if (p_ + 1 > d.size()) { err = "node child flag past the end"; return false; }
    const int has_children = d[p_];
    if (has_children > 1)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "node %llu hasChildren is %d - the frame has slipped",
                      (unsigned long long)key, has_children);
        err = m;
        return false;
    }
    p_ += 1;
    if (!rows.empty())
    {
        MaterialNode n;
        n.key = key; n.depth = depth; n.rows = std::move(rows);
        nodes_.push_back(std::move(n));
    }
    if (has_children)
        for (int i = 0; i < 4; i++)
            if (!read_node((key << 4) | (uint64_t)i, depth + 1, err)) return false;
    return true;
}

bool MaterialTree::parse(const std::vector<uint8_t>& b7, std::string& err)
{
    err.clear();
    nodes_.clear(); pairs_.clear(); background_ = 0;
    constexpr size_t kHdr = 0x24;
    if (b7.size() < kHdr) { err = "block 7 shorter than its header"; return false; }
    d_ = &b7;
    dim_        = (int)rd<uint32_t>(b7, 0x00);
    world_min_[0] = rd<float>(b7, 0x08); world_min_[1] = rd<float>(b7, 0x0C);
    world_max_[0] = rd<float>(b7, 0x10); world_max_[1] = rd<float>(b7, 0x14);
    node_count_ = (int)rd<uint32_t>(b7, 0x18);
    levels_     = (int)rd<uint32_t>(b7, 0x20);
    if (dim_ <= 0 || dim_ > 4096)
    {
        char m[64];
        std::snprintf(m, sizeof(m), "implausible raster dim %d", dim_);
        err = m;
        return false;
    }
    p_ = kHdr;
    if (!read_node(3, 0, err)) return false;

    // §7.4: the footer follows the root's TRAILING hasChildren and the block ends
    // EXACTLY after it. That exactness is the check that the node walk did not
    // slip - a framing error leaves the footer somewhere else entirely.
    if (p_ + 8 > b7.size())
    {
        char m[64];
        std::snprintf(m, sizeof(m), "no room for the pair footer at %zu", p_);
        err = m;
        return false;
    }
    const uint32_t n = rd<uint32_t>(b7, p_);
    if (n > 256 || p_ + 4 + (size_t)n * 4 + 4 != b7.size())
    {
        char m[128];
        std::snprintf(m, sizeof(m), "pair table of %u does not end the block (%zu of %zu)",
                      n, p_, b7.size());
        err = m;
        return false;
    }
    pairs_.resize(n);
    for (uint32_t i = 0; i < n; i++) pairs_[i] = rd<uint32_t>(b7, p_ + 4 + (size_t)i * 4);
    background_ = rd<uint32_t>(b7, p_ + 4 + (size_t)n * 4);
    d_ = nullptr;
    return true;
}

bool MaterialTree::parse_mask(const std::vector<uint8_t>& b8, std::string& err)
{
    err.clear();
    nodes_.clear(); pairs_.clear(); background_ = 0;
    if (b8.size() < 0x28) { err = "block 8 shorter than its header"; return false; }
    d_ = &b8;
    dim_        = (int)rd<uint32_t>(b8, 0x00);
    world_min_[0] = rd<float>(b8, 0x08); world_min_[1] = rd<float>(b8, 0x0C);
    world_max_[0] = rd<float>(b8, 0x10); world_max_[1] = rd<float>(b8, 0x14);
    node_count_ = (int)rd<uint32_t>(b8, 0x18);
    levels_     = (int)rd<uint32_t>(b8, 0x20);
    if (dim_ <= 0 || dim_ > 4096)
    {
        char m[64];
        std::snprintf(m, sizeof(m), "implausible raster dim %d", dim_);
        err = m;
        return false;
    }
    p_ = 0x28;
    const bool ok = read_node(3, 0, err);
    d_ = nullptr;
    return ok;
}

// §8: a pair entry -> a layer index.
//
//   e & 0xFF          framing, 0x80
//   (e >> 8) & 0xF    X-lo: primary nibble index
//   (e >> 12) & 0xF   X-hi: secondary nibble index, 15 = none
//   (e >> 16) & 0xF   Y-lo: WHICH LIST (0 full, 1 node base, 2 linked)
//   (e >> 20) & 0xF   Y-hi: clipmap level tag, not needed to resolve
//   (e >> 24) & 0xFF  framing, 0x06
namespace {

int resolve_entry(uint32_t e, const std::vector<int>* lists[3])
{
    if (e == 0 || !MaterialTree::entry_is_framed(e)) return -1;
    const int kind = MaterialTree::entry_list_kind(e);
    // §8's practical fallback: a map with no linked layers resolves kind 2
    // against the base list, and a node with an empty base list falls through to
    // the map-global no-page list the caller supplied in its place.
    const std::vector<int>* arr = (kind >= 0 && kind < 3) ? lists[kind] : lists[1];
    if (!arr) arr = lists[0];
    if (!arr) return -1;
    const int nibs[2] = { MaterialTree::entry_primary(e), MaterialTree::entry_secondary(e) };
    for (int k = 0; k < 2; k++)
    {
        if (nibs[k] == 15) continue;          // the "none" sentinel, not an index
        if (nibs[k] < (int)arr->size()) return (*arr)[nibs[k]];
    }
    return -1;
}

}  // namespace

int MaterialTree::resolve(int texel, const std::vector<int>* lists[3]) const
{
    const int pi = texel & 0xF;
    if (pi >= (int)pairs_.size()) return -1;
    return resolve_entry(pairs_[(size_t)pi], lists);
}

std::vector<uint8_t> MaterialTree::hole_raster(int size) const
{
    std::vector<uint8_t> out((size_t)size * (size_t)size, 1);
    const float span[2] = { world_max_[0] - world_min_[0], world_max_[1] - world_min_[1] };
    if (span[0] <= 0.f || span[1] <= 0.f) return out;
    std::vector<const MaterialNode*> order;
    for (const MaterialNode& n : nodes_) order.push_back(&n);
    std::stable_sort(order.begin(), order.end(),
        [](const MaterialNode* a, const MaterialNode* b) { return a->depth < b->depth; });
    for (const MaterialNode* n : order)
    {
        float lo[2], hi[2];
        Splat::bounds_of(n->key, world_min_, world_max_, lo, hi);
        const int x0 = std::clamp((int)std::floor((lo[0] - world_min_[0]) / span[0] * (float)size), 0, size);
        const int x1 = std::clamp((int)std::ceil ((hi[0] - world_min_[0]) / span[0] * (float)size), 0, size);
        const int z0 = std::clamp((int)std::floor((lo[1] - world_min_[1]) / span[1] * (float)size), 0, size);
        const int z1 = std::clamp((int)std::ceil ((hi[1] - world_min_[1]) / span[1] * (float)size), 0, size);
        if (x1 <= x0 || z1 <= z0) continue;
        const float rw = std::max(hi[0] - lo[0], 1e-6f);
        const float rh = std::max(hi[1] - lo[1], 1e-6f);
        for (int gz = z0; gz < z1; gz++)
        {
            const float wz = world_min_[1] + ((float)gz + 0.5f) / (float)size * span[1];
            const int sy = std::clamp((int)(std::clamp((wz - lo[1]) / rh, 0.f, 1.f) * (float)(dim_ - 1)),
                                      0, dim_ - 1) * dim_;
            for (int gx = x0; gx < x1; gx++)
            {
                const float wx = world_min_[0] + ((float)gx + 0.5f) / (float)size * span[0];
                const int sx = std::clamp((int)(std::clamp((wx - lo[0]) / rw, 0.f, 1.f) * (float)(dim_ - 1)),
                                          0, dim_ - 1);
                out[(size_t)gz * (size_t)size + (size_t)gx] = (uint8_t)(n->rows[(size_t)(sy + sx)] & 1);
            }
        }
    }
    return out;
}

bool MaterialTree::rasterize(int size, const Splat::BaseListAt& base_at,
                             const std::vector<int>& full, const std::vector<int>& linked,
                             const std::vector<int>& global_base, MaterialRaster& out,
                             std::string& err) const
{
    err.clear();
    if (size <= 0) { err = "rasterize needs a raster size"; return false; }
    out.size = size;
    out.pair.assign((size_t)size * (size_t)size, 255);
    out.layer.assign((size_t)size * (size_t)size, 255);
    std::memset(out.pair_hist, 0, sizeof(out.pair_hist));
    const float span[2] = { world_max_[0] - world_min_[0], world_max_[1] - world_min_[1] };
    if (span[0] <= 0.f || span[1] <= 0.f)
    { err = "block 7 has an empty world bounds"; return false; }
    out.lo[0] = world_min_[0]; out.lo[1] = world_min_[1];
    out.hi[0] = world_max_[0]; out.hi[1] = world_max_[1];

    // THE BACKGROUND DOES NOT OVERWRITE A CLAIM. Measured over 35 trees, block-7
    // data nodes TILE each map exactly (zero overlapping rects), so no-override
    // and overwrite give byte-identical rasters; the earlier "backgrounds erased
    // the streets" symptom was a broken node lookup, not a format property. Kept
    // because it is free and stays correct if a map ever does overlap levels.
    //
    // DIVERGENCE FROM THE GDScript, deliberate: it searched the pair table for
    // the background VALUE and then resolved by that table INDEX, which is only
    // the same thing while the value sits in the first sixteen slots (resolve
    // masks the index with 0xF). Resolving the entry itself is identical there
    // and correct everywhere.
    const std::vector<int>* bg_lists[3] = {
        &full, global_base.empty() ? &full : &global_base, &linked };
    const int bg_layer = background_ ? resolve_entry(background_, bg_lists) : -1;

    std::vector<const MaterialNode*> order;
    order.reserve(nodes_.size());
    for (const MaterialNode& n : nodes_) order.push_back(&n);
    std::stable_sort(order.begin(), order.end(),
        [](const MaterialNode* a, const MaterialNode* b) { return a->depth < b->depth; });

    for (const MaterialNode* n : order)
    {
        float lo[2], hi[2];
        Splat::bounds_of(n->key, world_min_, world_max_, lo, hi);

        // THE NODE'S OWN LIST WINS - §8 as written. A global-list reading once
        // lived here and validated on aftermath/outskirts, where every node list
        // happens to EQUAL the global list; on 23 of 35 maps they differ
        // (mp_isolated: 98.9% of its kind-1 texels) because node lists omit or
        // insert layers mid-order, and a nibble means nothing except against the
        // matched node's own ascending list.
        std::vector<int> nb = base_at ? base_at((lo[0] + hi[0]) * 0.5f,
                                                (lo[1] + hi[1]) * 0.5f,
                                                hi[0] - lo[0])
                                      : std::vector<int>();
        if (nb.empty()) nb = global_base;
        const std::vector<int>* lists[3] = { &full, &nb, &linked };

        // One resolve per DISTINCT texel value rather than per texel: the values
        // are 4-bit, so there are at most sixteen answers for a whole 256x256
        // node, and resolving all 65,536 separately is the same answer 4,000
        // times over.
        int lut[16];
        for (int v = 0; v < 16; v++) lut[v] = resolve(v, lists);

        const int x0 = std::clamp((int)std::floor((lo[0] - world_min_[0]) / span[0] * (float)size), 0, size);
        const int x1 = std::clamp((int)std::ceil ((hi[0] - world_min_[0]) / span[0] * (float)size), 0, size);
        const int z0 = std::clamp((int)std::floor((lo[1] - world_min_[1]) / span[1] * (float)size), 0, size);
        const int z1 = std::clamp((int)std::ceil ((hi[1] - world_min_[1]) / span[1] * (float)size), 0, size);
        if (x1 <= x0 || z1 <= z0) continue;

        // Sampled by WORLD position within the node, not by index within the
        // clipped raster rect - the two agree only while nothing clips.
        const float rw = std::max(hi[0] - lo[0], 1e-6f);
        const float rh = std::max(hi[1] - lo[1], 1e-6f);
        for (int gz = z0; gz < z1; gz++)
        {
            const float wz = world_min_[1] + ((float)gz + 0.5f) / (float)size * span[1];
            const float fz = std::clamp((wz - lo[1]) / rh, 0.f, 1.f);
            const int sy = std::clamp((int)(fz * (float)(dim_ - 1)), 0, dim_ - 1) * dim_;
            const size_t drow = (size_t)gz * (size_t)size;
            for (int gx = x0; gx < x1; gx++)
            {
                const float wx = world_min_[0] + ((float)gx + 0.5f) / (float)size * span[0];
                const float fx = std::clamp((wx - lo[0]) / rw, 0.f, 1.f);
                const int sx = std::clamp((int)(fx * (float)(dim_ - 1)), 0, dim_ - 1);
                const int v = n->rows[(size_t)(sy + sx)] & 0xF;
                out.pair[drow + (size_t)gx] = (uint8_t)v;
                const int l = lut[v];
                if (l < 0 || l >= 255) continue;
                if (l == bg_layer && out.layer[drow + (size_t)gx] != 255) continue;
                out.layer[drow + (size_t)gx] = (uint8_t)l;
            }
        }
    }

    out.unset = 0;
    for (size_t i = 0; i < out.pair.size(); i++)
    {
        if (out.pair[i] == 255) { out.unset++; continue; }
        out.pair_hist[out.pair[i] & 0xF]++;
    }
    return true;
}

}  // namespace bf6
