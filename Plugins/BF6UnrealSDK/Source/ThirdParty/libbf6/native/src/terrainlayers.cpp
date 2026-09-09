#include "terrainlayers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "depot.h"
#include "source.h"

namespace bf6 {
namespace {

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t o)
{
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

// A half-MD5 is a DIGEST, so its bytes are in digest order and the number they
// spell is big-endian. Reading it as a little-endian u64 gives a value that is
// perfectly stable and compares equal to nothing anybody has ever written down
// - including the MD5("") that marks an empty layer.
uint64_t rd_digest64(const std::vector<uint8_t>& d, size_t o)
{
    if (o + 8 > d.size()) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | d[o + i];
    return v;
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

float f32(const std::vector<uint8_t>& b, size_t o)
{
    float v = 0.f;
    if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
    return v;
}

const std::string kNoString;

struct Named { uint32_t n32; const char* name; };

const Named kNames[] = {
    { tl::kTexBaseColorA,    "basecolor_a" },
    { tl::kTexNormalHeightA, "normalheight_a" },
    { tl::kTexThirdA,        "third_a" },
    { tl::kTexThirdA2,       "third_a2" },
    { tl::kTexBaseColorB,    "basecolor_b" },
    { tl::kTexNormalHeightB, "normalheight_b" },
    { tl::kTexThirdB,        "third_b" },
    { tl::kTexThirdB2,       "third_b2" },
    { tl::kTexBaseColorC,    "basecolor_c" },
    { tl::kTexNormalHeightC, "normalheight_c" },
    { tl::kTexThirdC,        "third_c" },
    { tl::kTexDefault,       "default" },
    { tl::kTexBreakup,       "breakup" },
    { tl::kTexNcs,           "ncs" },
    { tl::kTexHfd,           "hfd" },
    { tl::kTexScatterNoise,  "scatternoise" },
    { tl::kTexSsmDistance,   "ssm_distance" },
    { tl::kCOverlayStrength, "overlay_strength" },
    { tl::kCUvOffset,        "uv_offset" },
    { tl::kCDisplaceRange,   "displace_range" },
    { tl::kCUvTiling,        "uv_tiling" },
    { tl::kCUvRotationDeg,   "uv_rotation_deg" },
    { tl::kCCoordScaleX,     "coord_scale_x" },
    { tl::kCCoordScaleY,     "coord_scale_y" },
    { tl::kCTint,            "tint" },
    { tl::kCBaseHeight,      "base_height" },
    { tl::kCMaskRampExp,     "mask_ramp_exp" },
    { tl::kCHeightBlend,     "height_blend" },
    { tl::kCSurfaceClass,    "surface_class" },
};

// Turn one depot binding into a layer material. Everything is keyed by name32:
// see the row-map note in the header for why field position cannot be used.
void fill_material(const MaterialBinding& mb, TerrainLayerMaterial& m)
{
    m.resolved = mb.valid;
    for (const auto& kv : mb.textures)
    {
        m.raw_textures[kv.first] = kv.second;
        switch (kv.first)
        {
        case tl::kTexBaseColorA:    m.set_a.base_color    = kv.second; break;
        case tl::kTexNormalHeightA: m.set_a.normal_height = kv.second; break;
        case tl::kTexThirdA:        m.set_a.third         = kv.second; break;
        case tl::kTexBaseColorB:    m.set_b.base_color    = kv.second; break;
        case tl::kTexNormalHeightB: m.set_b.normal_height = kv.second; break;
        case tl::kTexThirdB:        m.set_b.third         = kv.second; break;
        case tl::kTexBaseColorC:    m.set_c.base_color    = kv.second; break;
        case tl::kTexNormalHeightC: m.set_c.normal_height = kv.second; break;
        case tl::kTexThirdC:        m.set_c.third         = kv.second; break;
        // The second third-map hash of sets A and B. Fleet measurement over
        // every terrain level found the two hashes of a set NEVER co-occur -
        // they mark two different graph templates - so either fills the slot.
        case tl::kTexThirdA2:
            if (m.set_a.third.empty()) m.set_a.third = kv.second;
            else m.other_textures[kv.first] = kv.second;
            break;
        case tl::kTexThirdB2:
            if (m.set_b.third.empty()) m.set_b.third = kv.second;
            else m.other_textures[kv.first] = kv.second;
            break;
        default:
            // AN UNKNOWN SLOT IS NOT AN ABSENT TEXTURE. Keeping it by hash lets
            // a consumer recognise it later without re-reading the depot;
            // dropping it is how a layer that binds real sheets reads as
            // shader-computed.
            m.other_textures[kv.first] = kv.second;
            break;
        }
    }

    for (const auto& kv : mb.constants)
    {
        m.raw_constants[kv.first] = kv.second;
        const std::vector<uint8_t>& b = kv.second;
        switch (kv.first)
        {
        case tl::kCOverlayStrength:
            if (b.size() >= 4) { m.overlay_strength = f32(b, 0); m.overlay_strength_set = true; }
            break;
        case tl::kCUvOffset:
            if (b.size() >= 8)
            { m.uv_offset[0] = f32(b, 0); m.uv_offset[1] = f32(b, 4); m.uv_offset_set = true; }
            break;
        case tl::kCDisplaceRange:
            if (b.size() >= 4) { m.displace_range = f32(b, 0); m.displace_range_set = true; }
            break;
        case tl::kCUvTiling:
            if (b.size() >= 4) { m.uv_tiling = f32(b, 0); m.uv_tiling_set = true; }
            break;
        case tl::kCUvRotationDeg:
            if (b.size() >= 4) { m.uv_rotation_deg = f32(b, 0); m.uv_rotation_deg_set = true; }
            break;
        case tl::kCCoordScaleX:
            if (b.size() >= 4) { m.coord_scale[0] = f32(b, 0); m.coord_scale_set = true; }
            break;
        case tl::kCCoordScaleY:
            if (b.size() >= 4) { m.coord_scale[1] = f32(b, 0); m.coord_scale_set = true; }
            break;
        case tl::kCTint:
            if (b.size() >= 12)
            {
                m.tint[0] = f32(b, 0); m.tint[1] = f32(b, 4); m.tint[2] = f32(b, 8);
                m.tint_set = true;
            }
            break;
        case tl::kCBaseHeight:
            if (b.size() >= 4) { m.base_height = f32(b, 0); m.base_height_set = true; }
            break;
        case tl::kCMaskRampExp:
            if (b.size() >= 4) { m.mask_ramp_exp = f32(b, 0); m.mask_ramp_exp_set = true; }
            break;
        case tl::kCHeightBlend:
            if (b.size() >= 4) { m.height_blend = f32(b, 0); m.height_blend_set = true; }
            break;
        case tl::kCSurfaceClass:
            if (b.size() >= 4)
            { m.surface_class = (int32_t)rd<uint32_t>(b, 0); m.surface_class_set = true; }
            break;
        default:
            m.other_constants[kv.first] = b;
            break;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// TerrainLayerMaterial
// ---------------------------------------------------------------------------

const std::string& TerrainLayerMaterial::base_color() const
{
    if (!set_a.base_color.empty()) return set_a.base_color;
    if (!set_b.base_color.empty()) return set_b.base_color;
    if (!set_c.base_color.empty()) return set_c.base_color;
    return kNoString;
}

const std::string& TerrainLayerMaterial::normal_height() const
{
    if (!set_a.normal_height.empty()) return set_a.normal_height;
    if (!set_b.normal_height.empty()) return set_b.normal_height;
    if (!set_c.normal_height.empty()) return set_c.normal_height;
    return kNoString;
}

float TerrainLayerMaterial::metres_per_repeat(float fallback) const
{
    if (!uv_tiling_set || uv_tiling <= 1e-5f) return fallback;
    return 1.0f / uv_tiling;
}

// ---------------------------------------------------------------------------
// TerrainLayers
// ---------------------------------------------------------------------------

const char* TerrainLayers::role_name(uint32_t n32)
{
    for (const Named& n : kNames) if (n.n32 == n32) return n.name;
    return nullptr;
}

size_t TerrainLayers::empty_count() const
{
    size_t n = 0;
    for (const TerrainLayer& l : layers_) if (l.empty) n++;
    return n;
}

size_t TerrainLayers::resolved_count() const
{
    size_t n = 0;
    for (const TerrainLayer& l : layers_) if (l.material.resolved) n++;
    return n;
}

size_t TerrainLayers::with_base_color_count() const
{
    size_t n = 0;
    for (const TerrainLayer& l : layers_) if (!l.material.base_color().empty()) n++;
    return n;
}

// Ascending by layer index, which is what a nibble index into it requires.
// layers_ is built in index order, so a forward walk is already ascending and
// sorting it again would only hide a future change that broke that.
std::vector<int> TerrainLayers::linked_list() const
{
    std::vector<int> out;
    for (const TerrainLayer& l : layers_) if (l.link >= 0) out.push_back((int)l.index);
    return out;
}

std::vector<int> TerrainLayers::linked_list(int link_group) const
{
    std::vector<int> out;
    for (const TerrainLayer& l : layers_) if (l.link == link_group) out.push_back((int)l.index);
    return out;
}

std::vector<int> TerrainLayers::link_groups() const
{
    std::vector<int> out;
    for (const TerrainLayer& l : layers_)
        if (l.link >= 0 && std::find(out.begin(), out.end(), l.link) == out.end())
            out.push_back(l.link);
    std::sort(out.begin(), out.end());
    return out;
}

bool TerrainLayers::load(Source& src, const std::string& level, std::string& err)
{
    err.clear();
    layers_.clear();
    lg_name_.clear(); depot_name_.clear(); lc_name_.clear();
    depot_matched_ = false; depots_seen_ = 0; record_offset_ = 0;
    table_id_ = 0; surface_key_ = 0;

    const std::string low = lower(level);

    // --- pick the three resources, ALL of them scoped to this level ---------
    //
    // A ShaderBlockKey is content-addressed and shared between maps, so a depot
    // chosen without a level filter binds another level's material for a key
    // that merely collides. That failure looks like success: every layer
    // resolves, and the ground is painted with a different map's textures. The
    // filter is the only thing standing between those two outcomes.
    std::string depot_any;
    for (const auto& kv : src.res())
    {
        const std::string n = lower(kv.first);
        const uint32_t ty = kv.second.type;
        if (ty == tl::kResLayerGraphs && contains(n, low)) lg_name_ = kv.first;
        else if (ty == tl::kResLayerComb && contains(n, low)) lc_name_ = kv.first;
        else if (ty == tl::kResDepot && contains(n, "layergraph"))
        {
            depots_seen_++;
            if (depot_any.empty()) depot_any = kv.first;
            if (contains(n, low)) depot_name_ = kv.first;
        }
    }
    depot_matched_ = !depot_name_.empty();
    // A single-level mount has nothing else to offer, so a borrowed depot is
    // still better than no palette - but the caller is told which it got.
    if (depot_name_.empty()) depot_name_ = depot_any;

    if (lg_name_.empty() || depot_name_.empty())
    {
        err = "no layer graphs (" + lg_name_ + ") or layergraphs depot ("
            + depot_name_ + ") for " + level;
        return false;
    }

    std::string e;
    std::vector<uint8_t> lg = src.get_res(lg_name_, e);
    if (lg.size() < 20) { err = "layer-graph table too small: " + e; return false; }

    std::vector<uint8_t> draw = src.get_res(depot_name_, e);
    Depot depot;
    if (!depot.parse(draw, e)) { err = "layergraphs depot: " + e; return false; }

    table_id_ = rd<uint64_t>(lg, 0);
    const uint32_t n = rd<uint32_t>(lg, 8);
    if (n == 0 || n > 4096)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "implausible layer count %u", n);
        err = m;
        return false;
    }

    // --- where the 32-byte records start ------------------------------------
    //
    // SOLVED, NOT SEARCHED. The plugin hunted this offset by testing every
    // 4-byte position until all N keys resolved in the depot; on an install
    // where they never all do there is no early break at all, so it grinds the
    // whole buffer at N dictionary lookups per offset - measured at 2 m 34 s on
    // a user's main thread, and the palette was unusable at the end of it.
    //
    // The whole table is fixed-shape once N is known. Measured on mp_aftermath
    // (N=40), mp_dumbo (47) and mp_tungsten (33), all three byte-identical in
    // shape:
    //
    //   +0    u64  table id
    //   +8    u32  N
    //   +12   N x u32   per-layer result-format codes, 1 on every layer
    //   +12+4N  u32 1, u32 0x12, u32 N, u32 0      <- the 16-byte marker
    //   +28+4N  N x 32-byte records
    //   end-20  u32 0xFF6, u64 0, u64 MD5("") in digest order
    //
    // So the offset is 28 + 4N, and BOTH ends of the table say so independently.
    // Both are checked and both must agree: a build that moves one of them is
    // caught here rather than one node into the join, where it would present as
    // a palette of plausible wrong textures.
    //
    // NOTE vs the hub's write-up, which has the trailing marker as "u32 N again,
    // u32 0" (8 bytes) and the footer as 24 bytes with an extra u32 0. On disk
    // the marker is 16 bytes - the {1, 0x12} pair precedes the N - and the
    // footer is 20. The hub's field ORDER is right; the two lengths are not.
    static const uint8_t kMd5Empty[8] = { 0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04 };
    bool pinned = false;
    {
        const size_t marker = 12 + 4ull * n;
        const size_t rec = marker + 16;
        const bool header_ok =
            rec + 32ull * n + 20 <= lg.size() &&
            rd<uint32_t>(lg, marker) == 1u &&
            rd<uint32_t>(lg, marker + 4) == 0x12u &&
            rd<uint32_t>(lg, marker + 8) == n &&
            rd<uint32_t>(lg, marker + 12) == 0u;
        const bool size_ok = lg.size() == rec + 32ull * n + 20;
        const size_t f = rec + 32ull * n;
        const bool footer_ok =
            f + 20 <= lg.size() &&
            rd<uint32_t>(lg, f) == 0xFF6u &&
            rd<uint64_t>(lg, f + 4) == 0ull &&
            std::memcmp(lg.data() + f + 12, kMd5Empty, 8) == 0;
        if (header_ok && size_ok && footer_ok) { record_offset_ = rec; pinned = true; }
    }
    if (!pinned)
    {
        // FALLBACK, kept because a future build could move the footer: the
        // plugin's rule, that the right offset is the one where every key
        // resolves. Two passes - filter on the FIRST record, then test the
        // survivors in full - so the cost of failing is a single lookup per
        // offset instead of N of them. Pass 1's filter is a superset of the
        // answer, not a heuristic: if all N keys resolve at an offset then its
        // first key does too, so no possible winner is discarded.
        std::vector<size_t> cands;
        const size_t last = lg.size() >= 32ull * n ? lg.size() - 32ull * n : 0;
        for (size_t off = 12; off <= last; off += 4)
            if (depot.has_key(rd<uint64_t>(lg, off + 20))) cands.push_back(off);
        size_t best = 0, best_hits = 0;
        for (size_t off : cands)
        {
            size_t hit = 0;
            for (uint32_t i = 0; i < n; i++)
                if (depot.has_key(rd<uint64_t>(lg, off + (size_t)i * 32 + 20))) hit++;
            if (hit > best_hits) { best_hits = hit; best = off; }
            if (hit == n) break;
        }
        if (best_hits != n)
        {
            char m[288];
            std::snprintf(m, sizeof(m),
                "layer-graph record table could not be pinned: neither the 16-byte "
                "marker at %zu nor the 0xFF6 footer sits where %u layers put it, and "
                "the best key fit was %zu of %u over %zu candidate offsets (needs "
                "all). Usually a game version whose table layout differs, not missing "
                "files - which is a different fix.",
                (size_t)(12 + 4ull * n), n, best_hits, n, cands.size());
            err = m;
            return false;
        }
        record_offset_ = best;
    }

    // --- the layer-combination table: Link, and the surface key -------------
    //
    // Located by its own invariant - it must end exactly at EOF - which makes
    // the offset a FUNCTION of the entry count rather than something to hunt
    // for. Counting down reproduces the plugin's choice (smallest offset, so
    // largest n) exactly.
    std::vector<int> link_of;
    if (!lc_name_.empty())
    {
        std::vector<uint8_t> lc = src.get_res(lc_name_, e);
        for (int cnt = 511; cnt >= 1; cnt--)
        {
            const ptrdiff_t off = (ptrdiff_t)lc.size() - 12 - (ptrdiff_t)cnt * 5;
            if (off < 0) continue;
            if ((int)rd<uint32_t>(lc, (size_t)off + 8) != cnt) continue;
            surface_key_ = rd<uint64_t>(lc, (size_t)off);
            link_of.reserve((size_t)cnt);
            for (int i = 0; i < cnt; i++)
                link_of.push_back((int)rd<uint32_t>(lc, (size_t)off + 12 + (size_t)i * 5 + 1));
            break;
        }
    }

    // --- decode, and join each key through the depot -------------------------
    layers_.reserve(n);
    for (uint32_t i = 0; i < n; i++)
    {
        const size_t o = record_offset_ + (size_t)i * 32;
        TerrainLayer l;
        l.index = i;
        // Three mask words and a trailing one. They are per-layer bit fields
        // (0xFF, 0xFFF, 0x1FFF and friends turn up across a palette); nothing
        // downstream needs them decoded, so they are handed over as they lie
        // rather than given invented names.
        l.masks[0] = rd<uint32_t>(lg, o);
        l.masks[1] = rd<uint32_t>(lg, o + 4);
        l.masks[2] = rd<uint32_t>(lg, o + 8);
        l.content_hash = rd_digest64(lg, o + 12);
        l.shader_block_key = rd<uint64_t>(lg, o + 20);
        l.tail = rd<uint32_t>(lg, o + 28);
        l.empty = (l.content_hash == tl::kEmptyContentHash);
        if (i < link_of.size())
        {
            const int lk = link_of[i];
            l.link = (lk == -1 || (uint32_t)lk == 0xFFFFFFFFu) ? -1 : lk;
        }
        if (depot.has_key(l.shader_block_key))
            fill_material(depot.textures_for(l.shader_block_key, draw), l.material);
        layers_.push_back(std::move(l));
    }
    return true;
}

}  // namespace bf6
