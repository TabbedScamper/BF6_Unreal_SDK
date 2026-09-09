#include "decals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace bf6 {
namespace {

const uint32_t TYPE_RESOURCE = 0xCC84D53D;   // u8 count + 16 B pair + 16 B guid @ +17
const uint32_t TYPE_FLOAT    = 0x14A0B1C1;   // u32 count + count * 4
const uint32_t TYPE_VEC3     = 0x25F81AF1;   // u32 count + count * 12

const size_t VTX_STRIDE   = 32;
const size_t ANCHOR_WINDOW = 0x8000;
const size_t FIXED_GEO     = 0xB0;

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t off)
{
    T v{};
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

bool fits(const std::vector<uint8_t>& d, size_t off, size_t n)
{
    return off <= d.size() && d.size() - off >= n;
}

uint32_t u32at(const std::vector<uint8_t>& d, size_t o) { return fits(d, o, 4) ? rd<uint32_t>(d, o) : 0u; }
uint64_t u64at(const std::vector<uint8_t>& d, size_t o) { return fits(d, o, 8) ? rd<uint64_t>(d, o) : 0ull; }
float    f32at(const std::vector<uint8_t>& d, size_t o) { return fits(d, o, 4) ? rd<float>(d, o) : 0.f; }

// The half-float the vertex colour is stored in.
float half_of(uint16_t h)
{
    const int  s = (h >> 15) & 1;
    const int  e = (h >> 10) & 0x1F;
    const int  m = h & 0x3FF;
    float out;
    if (e == 0)        out = std::ldexp((float)m, -24);
    else if (e == 31)  out = m ? std::nanf("") : (float)INFINITY;
    else               out = std::ldexp((float)(m + 1024), e - 25);
    return s ? -out : out;
}

// 16 raw bytes -> the dashed spelling the partition index is keyed by. This
// MUST match the source module's spelling exactly or every decal texture
// misses, and misses silently: an unresolved guid looks like a decal with no
// material rather than like a bug here.
std::string guid_str(const std::vector<uint8_t>& d, size_t o)
{
    if (!fits(d, o, 16)) return std::string();
    char b[40];
    std::snprintf(b, sizeof(b),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        rd<uint32_t>(d, o), rd<uint16_t>(d, o + 4), rd<uint16_t>(d, o + 6),
        d[o + 8], d[o + 9], d[o + 10], d[o + 11], d[o + 12], d[o + 13], d[o + 14], d[o + 15]);
    return std::string(b);
}

bool all_zero(const std::vector<uint8_t>& d, size_t o, size_t n)
{
    for (size_t i = 0; i < n; i++) if (d[o + i]) return false;
    return true;
}

}  // namespace

static const uint32_t TYPE_VEC2  = 0x39AB6941u;
static const uint32_t TYPE_INT32 = 0x34791132u;
static const uint32_t TYPE_BOOL  = 0xA1C34F4Au;

void Decals::read_props(size_t p, size_t end, std::vector<DecalProp>& out) const
{
    while (p + 16 <= end)
    {
        DecalProp pr;
        pr.name = u64at(d_, p);
        const uint32_t tid = u32at(d_, p + 8);
        p += 16;
        if (tid == TYPE_RESOURCE)
        {
            if (p + 33 > end) return;
            pr.kind = DecalProp::Kind::Texture;
            pr.guid = guid_str(d_, p + 17);
            p += 33;
        }
        else if (tid == TYPE_FLOAT)
        {
            const uint32_t n = u32at(d_, p);
            p += 4;
            if (p + (size_t)n * 4 > end) return;
            pr.kind = DecalProp::Kind::Float;
            pr.values.resize(n);
            for (uint32_t k = 0; k < n; k++) pr.values[k] = f32at(d_, p + (size_t)k * 4);
            p += (size_t)n * 4;
        }
        else if (tid == TYPE_VEC3)
        {
            const uint32_t n = u32at(d_, p);
            p += 4;
            if (p + (size_t)n * 12 > end) return;
            pr.kind = DecalProp::Kind::Vec3;
            pr.values.resize((size_t)n * 3);
            for (uint32_t k = 0; k < n * 3; k++) pr.values[k] = f32at(d_, p + (size_t)k * 4);
            p += (size_t)n * 12;
        }
        else if (tid == TYPE_VEC2 || tid == TYPE_INT32 || tid == TYPE_BOOL)
        {
            // THE THREE TYPES THAT USED TO END THE WALK.
            //
            // Returning on an unknown type abandons the REST OF THE RECORD,
            // not just that property, and these three are common enough that
            // it cost 39.3% of every decal parameter in the game: 172,827
            // parsed against 284,789 present over 34 resources. The loss was
            // total on five property ids, including the packed-mask channel
            // selector, and 81% on the authored tint colour. Everything
            // downstream that read "this record has no tint" was reading the
            // truncation, not the data.
            const size_t esz = (tid == TYPE_VEC2) ? 8 : 4;
            const uint32_t n = u32at(d_, p);
            p += 4;
            if (n > 4096 || p + (size_t)n * esz > end) return;
            if (tid == TYPE_VEC2)
            {
                pr.kind = DecalProp::Kind::Vec2;
                pr.values.resize((size_t)n * 2);
                for (uint32_t k = 0; k < n * 2; k++)
                    pr.values[k] = f32at(d_, p + (size_t)k * 4);
            }
            else
            {
                pr.kind = (tid == TYPE_BOOL) ? DecalProp::Kind::Bool
                                             : DecalProp::Kind::Int;
                pr.ints.resize(n);
                for (uint32_t k = 0; k < n; k++)
                    pr.ints[k] = (int32_t)u32at(d_, p + (size_t)k * 4);
            }
            p += (size_t)n * esz;
        }
        else
        {
            // Still genuinely unknown, and an unknown size cannot be stepped
            // over, so the walk ends here. Now rare rather than routine.
            return;
        }
        out.push_back(std::move(pr));
    }
}

// Byte offset M of the record tail, or -1.
//
// Anchor shape: f32 1.0 @ M, f32 1.0 @ M+0x14, u64 0 @ M+0x18.
//
// STEPPED ONE BYTE, NOT FOUR. Odd property sizes ship, and they put every
// later tail on an unaligned offset that a 4-byte scan can never land on.
//
// AND WHEN THE PREVIOUS RECORD SAYS WHAT FirstIndex MUST BE, the candidate
// matching it is preferred. Long pads and property payloads contain stray
// 1.0/1.0/0 patterns, and taking the first one drifts the frame for every
// record after it; matching the expected value locks it instead.
int64_t Decals::find_anchor(size_t prop_end, int64_t expect_first) const
{
    if (d_.size() < 0x20) return -1;
    const size_t lim = std::min(prop_end + ANCHOR_WINDOW, d_.size() - 0x20);
    int64_t first_any = -1;

    for (size_t q = prop_end; q <= lim; q++)
    {
        if (f32at(d_, q) != 1.0f || f32at(d_, q + 0x14) != 1.0f || u64at(d_, q + 0x18) != 0)
            continue;
        if (q < 0x70) continue;
        if (first_any < 0) first_any = (int64_t)q;
        if (expect_first < 0) return (int64_t)q;
        if ((int64_t)u32at(d_, q - 0x70) == expect_first) return (int64_t)q;
    }

    // A CHAIN-LOCKED SECOND PASS, for the maps where the transform region stops
    // being identity-shaped partway through and the 1.0/1.0 pattern simply is
    // not there. With the chain saying what FirstIndex must be, the tail can be
    // found by that value directly and sanity-checked by TriCount and both
    // tilings.
    if (expect_first >= 0)
    {
        for (size_t q = prop_end; q <= lim; q++)
        {
            if ((int64_t)u32at(d_, q) != expect_first) continue;
            const uint32_t tri = u32at(d_, q + 4);
            const float t0 = std::fabs(f32at(d_, q + 0x0C));
            const float t1 = std::fabs(f32at(d_, q + 0x10));
            if (tri > 0 && tri < 2000000 &&
                t0 > 0.001f && t0 < 10000.f && t1 > 0.001f && t1 < 10000.f)
                return (int64_t)q + 0x70;   // q IS the tail; M sits 0x70 past it
        }
    }
    return first_any;
}

bool Decals::read_record(size_t p, int64_t expect_first, DecalRecord& out, size_t& next)
{
    if (p + 4 > d_.size()) return false;
    size_t prop_size = u32at(d_, p);
    size_t prop_end  = p + 4 + prop_size;
    std::vector<DecalProp> props;

    if (prop_size > d_.size() || prop_end > d_.size())
    {
        // Seven of the maps sampled carry a non-zero blob where record 0's
        // property size would be, so reading a size there gives garbage. Fall
        // back to scanning from the record start and let the FirstIndex
        // invariant find the tail.
        prop_end = p;
        prop_size = 0;
    }
    else if (prop_size > 0)
    {
        read_props(p + 8, prop_end, props);
    }

    const int64_t m = find_anchor(prop_end, expect_first);
    if (m < 0) return false;
    const size_t t = (size_t)m - 0x70;
    if (m < 0x70 || (size_t)m + 0x20 > d_.size()) return false;

    out = DecalRecord();
    out.first_index = u32at(d_, t);
    out.tri_count   = u32at(d_, t + 4);
    out.tiling0     = f32at(d_, t + 0x0C);
    out.tiling1     = f32at(d_, t + 0x10);
    for (int k = 0; k < 3; k++)
    {
        out.aabb_min[k] = f32at(d_, t + 0x20 + (size_t)k * 4);
        out.aabb_max[k] = f32at(d_, t + 0x30 + (size_t)k * 4);
    }
    out.asset_slot = u32at(d_, (size_t)m + 8);
    out.vb_off     = u32at(d_, (size_t)m + 12);
    out.vb_size    = u32at(d_, (size_t)m + 16);
    out.prop_size  = (uint32_t)prop_size;
    out.props      = std::move(props);
    next = (size_t)m + 0x20;
    return true;
}

// The fixed-header family: every field at a constant offset, no scanning.
// Returns the stream-end offset, or -1 to mean "this is not that family".
//
// REJECTION IS THE DETECTOR. A props-first map read this way breaks the
// FirstIndex chain immediately, so a walk that falls below 90% chain integrity
// or overruns the stream returns -1 and the caller falls back.
int64_t Decals::parse_fixed(size_t start)
{
    size_t p = start;
    std::vector<DecalRecord> out;
    int64_t expect_first = 0;
    size_t chain_hits = 0;

    // One family joins records to the slot table by GUID VALUE rather than by
    // index, so the reverse index is built once.
    std::map<std::string, uint32_t> slot_by_guid;
    for (size_t i = 0; i < slots_.size(); i++)
        if (!slots_[i].empty()) slot_by_guid[slots_[i]] = (uint32_t)i;

    for (uint32_t i = 0; i < declared_; i++)
    {
        if (p + FIXED_GEO + 4 > d_.size()) return -1;
        const uint32_t first = u32at(d_, p + 0x20);
        const uint32_t tri   = u32at(d_, p + 0x24);
        const uint32_t psize = u32at(d_, p + 0xB0);
        if (tri == 0 || tri > 2000000 || psize > 0x8000 ||
            p + FIXED_GEO + 4 + psize > d_.size()) return -1;

        if ((int64_t)first == expect_first) chain_hits++;
        else if ((size_t)i * 10 > declared_ && chain_hits * 10 < (size_t)i * 9) return -1;

        uint32_t slot = u32at(d_, p + 0x98);
        if (slot >= slots_.size())
        {
            // A 65535 sentinel, or the value-join family: resolve through the
            // record id instead.
            std::map<std::string, uint32_t>::const_iterator it =
                slot_by_guid.find(guid_str(d_, p));
            if (it != slot_by_guid.end()) slot = it->second;
        }

        DecalRecord r;
        r.first_index = first;
        r.tri_count   = tri;
        r.tiling0     = f32at(d_, p + 0x2C);
        r.tiling1     = f32at(d_, p + 0x30);
        for (int k = 0; k < 3; k++)
        {
            r.aabb_min[k] = f32at(d_, p + 0x40 + (size_t)k * 4);
            r.aabb_max[k] = f32at(d_, p + 0x50 + (size_t)k * 4);
        }
        r.asset_slot = slot;
        r.vb_off     = u32at(d_, p + 0x9C);
        r.vb_size    = u32at(d_, p + 0xA0);
        r.prop_size  = psize;
        if (psize > 0) read_props(p + 0xB0 + 8, p + 0xB0 + 4 + psize, r.props);
        out.push_back(std::move(r));

        expect_first = (int64_t)first + (int64_t)tri * 3;
        p += FIXED_GEO + 4 + psize;
    }
    if (out.size() < declared_ || chain_hits * 10 < out.size() * 9) return -1;
    records_  = std::move(out);
    chain_ok_ = chain_hits == records_.size();
    return (int64_t)p;
}

// The VB-end landmark, and why the arithmetic is not trusted.
//
// "The vertex buffer starts at the record-stream end rounded up to 4 KiB" is
// right on one map only, because its true base happens to land on a page
// boundary. Everywhere else the RES-level parameter block sits in between and
// its size varies per level, so that rule overshoots. Scanning for the trailing
// `u32 4, u32 0` and subtracting the span finds the real base.
int64_t Decals::find_vb_anchor(size_t span, size_t stream_end) const
{
    if (d_.size() < 8) return -1;
    const size_t page = (stream_end + 0xFFF) & ~(size_t)0xFFF;
    size_t q = page + span > 0x20000 ? page + span - 0x20000 : 0;
    const size_t end = std::min(d_.size() - 8, page + span + 0x200000);
    for (; q <= end; q += 4)
    {
        if (u32at(d_, q) != 4 || u32at(d_, q + 4) != 0) continue;
        if (q >= span && q - span >= stream_end) return (int64_t)(q - span);
    }
    return -1;
}

bool Decals::parse(std::vector<uint8_t> bytes, std::string& err)
{
    err.clear();
    d_ = std::move(bytes);
    slots_.clear();
    records_.clear();
    total_tris_ = 0;
    truncated_at_ = -1;
    chain_ok_ = true;

    if (d_.size() < 0x18) { err = "too small to be a TerrainDecals resource"; return false; }
    const uint32_t slot_count = u32at(d_, 16);
    if (slot_count == 0 || slot_count > 4096)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "slotCount %u outside the guard range", slot_count);
        err = m;
        return false;
    }

    size_t p = 0x14;
    for (uint32_t i = 0; i < slot_count; i++)
    {
        if (p + 16 > d_.size()) { err = "slot table runs past the end"; return false; }
        slots_.push_back(all_zero(d_, p, 16) ? std::string() : guid_str(d_, p));
        p += 16;
    }
    if (p + 4 > d_.size()) { err = "no record count"; return false; }
    declared_ = u32at(d_, p);
    p += 4;

    const int64_t fixed_end = parse_fixed(p);
    if (fixed_end >= 0)
    {
        framing_ = "fixed-header";
        p = (size_t)fixed_end;
    }
    else
    {
        framing_ = "props-first";
        records_.clear();
        chain_ok_ = true;
        // RECORD 0 IS PINNED BY AN INVARIANT rather than by its header: its
        // FirstIndex is 0 by definition. From record 1 on the chain carries the
        // frame, and that is what disambiguates the stray 1.0/1.0/0 patterns
        // inside long pads and property payloads.
        int64_t expect_first = 0;
        for (uint32_t i = 0; i < declared_; i++)
        {
            DecalRecord r;
            size_t next = 0;
            if (!read_record(p, expect_first, r, next))
            {
                // KEEP WHAT PARSED. A map's road network is worth having
                // partially: several maps truncate partway and still yield
                // thousands of good records, and refusing here discards all of
                // them for the sake of one.
                truncated_at_ = (int)i;
                break;
            }
            p = next;
            if ((int64_t)r.first_index != expect_first) chain_ok_ = false;
            expect_first = (int64_t)r.first_index + (int64_t)r.tri_count * 3;
            records_.push_back(std::move(r));
        }
    }

    size_t span = 0;
    for (const DecalRecord& r : records_)
    {
        span = std::max(span, (size_t)r.vb_off + (size_t)r.vb_size);
        total_tris_ += r.tri_count;
    }
    vb_size_  = span;
    vb_start_ = (p + 0xFFF) & ~(size_t)0xFFF;
    if (span > 0)
    {
        const int64_t cand = find_vb_anchor(span, p);
        if (cand >= 0) { vb_start_ = (size_t)cand; vb_from_anchor_ = true; }
    }
    const size_t a = vb_start_ + span;
    anchor_ok_ = fits(d_, a, 8) && u32at(d_, a) == 4 && u32at(d_, a + 4) == 0;

    if (records_.empty()) { err = "no records parsed"; return false; }
    return true;
}

std::vector<DecalVertex> Decals::vertices(const DecalRecord& r) const
{
    std::vector<DecalVertex> out;
    const size_t base = vb_start_ + r.vb_off;
    const size_t n    = r.vb_size / VTX_STRIDE;
    out.reserve(n);
    for (size_t i = 0; i < n; i++)
    {
        const size_t o = base + i * VTX_STRIDE;
        if (!fits(d_, o, VTX_STRIDE)) break;
        DecalVertex v;
        v.x = f32at(d_, o);
        v.z = f32at(d_, o + 4);
        // THE UVS ARE FULL f32s AT +0x10 AND +0x14. An earlier reading had
        // f16s at +0x12 and +0x16 divided by 1.875, which were the HIGH HALVES
        // of these very floats read as halves: f32 1.0 is 0x3F800000 and its
        // top 16 bits decode as an f16 to exactly 1.875. That one bit-pattern
        // accident invented the constant, the "u overflows half precision"
        // myth, and a whole world-projection fallback. Nothing overflows.
        v.u = f32at(d_, o + 0x10);
        v.v = f32at(d_, o + 0x14);
        v.r = half_of(rd<uint16_t>(d_, o + 0x18));
        v.g = half_of(rd<uint16_t>(d_, o + 0x1A));
        v.b = half_of(rd<uint16_t>(d_, o + 0x1C));
        v.a = half_of(rd<uint16_t>(d_, o + 0x1E));
        out.push_back(v);
    }
    return out;
}

bool Decals::is_planar(const std::vector<DecalVertex>& vs)
{
    if (vs.empty()) return false;
    const size_t n = std::min<size_t>(vs.size(), 12);
    for (size_t i = 0; i < n; i++)
        if (std::fabs(vs[i].u - vs[i].x) >= 0.5f || std::fabs(vs[i].v - vs[i].z) >= 0.5f)
            return false;
    return true;
}

}  // namespace bf6
