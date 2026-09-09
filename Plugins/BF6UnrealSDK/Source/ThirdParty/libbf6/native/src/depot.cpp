#include "depot.h"

#include <cstdio>
#include <cstring>

namespace bf6 {
namespace {

constexpr uint32_t TH_TEXTURE = 0xcc84d53d;

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t off)
{
    T v{};
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

// Inline scalar sizes by type hash. The type hash IS a Frostbite name hash, but
// this game changed the hash function, so most of these are known by size and
// behaviour rather than by name.
//
// AN UNKNOWN TYPE HASH IS A HARD ERROR, not a skip: the entries are packed with
// no alignment, so guessing a size does not lose one parameter, it
// desynchronises everything after it.
int scalar_size(uint32_t th)
{
    switch (th)
    {
    case 0x14a0b1c1: case 0xa1c34f4a: case 0x34791132: case 0x56b8d054:
    case 0x80f40524: case 0xf682b971: case 0xbb4b3d1c: case 0x8182bee2:
    case 0x6f07e870: case 0x56403cba: case 0x68b06dfb: case 0xa08d014b:
        return 4;
    // QualityLevel is inline WITHOUT the usual flags bit, which is why the form
    // is dispatched on the TYPE hash and never on the flags.
    case 0x48ecee3e: return 4;
    case 0x39ab6941: return 8;    // Vec2
    case 0x25f81af1: return 12;   // float3
    case 0xdef2e1a5: return 16;   // float4
    case 0x26b52646: return 1;    // bool / u8
    default: return -1;
    }
}

// THE SAME SPELLING THE PARTITION INDEX USES, which is the whole point of
// producing it: these guids are looked up there to get an asset name. .NET
// mixed endian - first three groups little-endian, the last eight bytes as they
// lie. Raw hex would be a perfectly reasonable-looking string that resolves
// nothing at all, and would fail silently on every texture.
std::string guid_str16(const std::vector<uint8_t>& d, size_t o)
{
    const uint8_t* g = d.data() + o;
    char t[40];
    std::snprintf(t, sizeof(t),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        (unsigned)(g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24)),
        (unsigned)(g[4] | (g[5] << 8)), (unsigned)(g[6] | (g[7] << 8)),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return t;
}

struct Slot { uint32_t n32; const char* name; };

// THE FULL 32 BITS MATTER: BaseColor and BaseColorVeg share the top half
// 0x54BB, and Emissive and Alpha share 0xD405, so comparing the high half alone
// silently confuses them.
const Slot kSlots[] = {
    { 0x54bbcd30, "basecolor" },        { 0x54bbcd36, "basecolor_veg" },
    { 0xec35a74c, "normal" },           { 0xec35a9e2, "normal_vt" },
    { 0xd405b0e5, "emissive" },         { 0xd405b0e1, "alpha" },
    { 0x851a1207, "tilebreaker" },      { 0x54bbcd22, "wrap" },
    { 0x21f3f4e1, "facade" },           { 0xa7e2f2fb, "facade_alpha" },
    { 0xc8c9370a, "backdrop_facade" },  { 0x62dfb21a, "backdrop_roof" },
    { 0x31ebaba9, "backdrop_palette" }, { 0xbb245590, "glass_volume" },
    { 0xa11011b8, "carpaint_flakes" },  { 0x407055fd, "emissive_lit" },
    { 0x48b69b15, "smoke_ca" },         { 0xba5b7844, "smoke_noise" },
    { 0xd0182167, "smoke_noise2" },     { 0x0127e78c, "smoke_ramp" },
    { 0x42ec27b9, "smoke_edge" },       { 0x87180b38, "decal_ca" },
    { 0x6a19658a, "decal_nrm" },

    // NAMED FROM THE DATA, 2026-08-22, by resolving what each slot is bound to
    // across MP_Battery's whole mount and counting asset-name suffixes
    // (test/slots_test.cpp). The method validated itself on the known slot:
    // 0x54bbcd30 came back "cs" on 99.1% of 7,074 bindings.
    //
    // These two are the per-asset maps that were being dropped. The table above
    // named a "normal" at 0xec35a74c which NEVER APPEARS in any depot; the slot
    // the game actually uses is 0xec35a757, in the same 0xec35 family.
    // THERE IS MORE THAN ONE NORMAL SLOT: the hash differs by SHADER FAMILY, and
    // a sample drawn from one family names only that family's. The first pass
    // here saw weapons and props and named 0xec35a757; running the whole chain
    // on character meshes then reported zero normals, because characters use
    // 0xec35a68c. Both are normals and both are needed.
    { 0xec35a757, "normal" },        // 5,415 uses, 93.2% "_nmt"  - props, weapons
    { 0xec35a68c, "normal" },        // 1,198 uses, 100%  "_nx"   - characters
    { 0xec35a697, "normal_face" },   //   534 uses, 100%  "_nx"   - facerig
    // AND ONE MEMBER OF THE SAME FAMILY IS NOT A NORMAL AT ALL. A blanket
    // "0xec35* is a normal" rule would bind a subsurface-scattering map as a
    // normal on every face in the game, which is exactly the kind of plausible
    // wrongness this table exists to prevent.
    { 0xec35b353, "subsurface" },    //   499 uses, 100%  "_sssrtm"
    { 0xb1a29a3c, "occl_rough" },    // 4,998 uses, 99.2% "_wo"

    // Global shader inputs rather than a prop's own maps: shared weathering and
    // detail layers, bound by nearly every material to the same few textures.
    // Named so they can be recognised and skipped, not because a prop needs
    // them resolved per-asset.
    { 0x5075fa43, "detail_ncs" },    // ta_weapondetailmaps_ncs
    { 0x70ceae93, "tiling_dust" },   // t_veh_tilingdust_a
    { 0xc79fa238, "rain_nm" },       // t_vehiclerain_drops_1k_nm
    { 0x6ab5f4f2, "rain_streaks" },  // t_vehiclerain_streaks
    { 0x13a62eca, "mud_nch" },       // t_hardwaremud_03_nch
    { 0x9d012723, "splatter_nca" },  // t_weathering_splatter_nca
    { 0x3d90fc7b, "snow_sparkle" },  // t_snowsparke_rgb
    { 0xd8236463, "blackbody_ramp" },// t_blackbodyramps_01_m
};

}  // namespace

uint32_t Depot::make_name32(uint64_t param_hash, uint16_t name_hi)
{
    return (uint32_t)(((uint32_t)name_hi << 16) | (uint32_t)((param_hash >> 48) & 0xFFFF));
}

const char* Depot::slot_name(uint32_t n32)
{
    for (const Slot& s : kSlots) if (s.n32 == n32) return s.name;
    return nullptr;
}

bool Depot::parse(const std::vector<uint8_t>& d, std::string& err)
{
    err.clear();
    records_.clear();
    key_to_record_.clear();
    cache_.clear();

    if (d.size() < 32) { err = "too small to be a depot"; return false; }
    const uint64_t a = rd<uint64_t>(d, 0);
    if (a != 16)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "unexpected header (key table at %llu, expected 16)",
                      (unsigned long long)a);
        err = m;
        return false;
    }

    // THE KEY TABLE'S LENGTH IS NOT STORED. It is recovered by dereferencing the
    // first pair's record pointer: that record's dataOffset is where the blob
    // area starts, which is exactly where the key table ends.
    if (24 + 8 > d.size()) { err = "truncated key table"; return false; }
    const uint64_t ptr0 = rd<uint64_t>(d, 24);
    if (ptr0 + 8 > d.size()) { err = "first record pointer is outside the file"; return false; }
    const uint64_t data_start = rd<uint64_t>(d, (size_t)ptr0);
    if (data_start <= 16 || data_start >= d.size() || (data_start - 16) % 16 != 0)
    { err = "bad key-table extent"; return false; }

    const size_t npairs = (size_t)((data_start - 16) / 16);
    std::vector<uint64_t> keys(npairs), ptrs(npairs);
    uint64_t rec_base = ~0ull;
    for (size_t i = 0; i < npairs; i++)
    {
        const size_t o = 16 + 16 * i;
        if (o + 16 > d.size()) { err = "key table overruns the file"; return false; }
        keys[i] = rd<uint64_t>(d, o);
        ptrs[i] = rd<uint64_t>(d, o + 8);
        if (ptrs[i] < rec_base) rec_base = ptrs[i];
    }

    // The record array tiles the blob area exactly: each record's dataOffset is
    // the previous one's end. That contiguity IS the terminator - there is no
    // count - so a record that does not continue the tiling ends the array.
    uint64_t rp = rec_base, expect = data_start;
    while (rp + 24 <= d.size())
    {
        DepotRecord r;
        r.offset       = rd<uint64_t>(d, (size_t)rp);
        r.content_hash = rd<uint64_t>(d, (size_t)rp + 8);
        r.size         = rd<uint64_t>(d, (size_t)rp + 16);
        if (r.offset != expect || r.offset + r.size > rec_base) break;
        records_.push_back(r);
        expect = r.offset + r.size;
        rp += 24;
    }
    if (records_.empty()) { err = "no records tile the blob area"; return false; }

    const DepotRecord& last = records_.back();
    const int64_t gap = (int64_t)rec_base - (int64_t)(last.offset + last.size);
    if (gap < 0 || gap > 16)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "record array does not tile the blob area (gap %lld)",
                      (long long)gap);
        err = m;
        return false;
    }

    for (size_t i = 0; i < npairs; i++)
    {
        if (ptrs[i] < rec_base || (ptrs[i] - rec_base) % 24 != 0) continue;
        const size_t ri = (size_t)((ptrs[i] - rec_base) / 24);
        if (ri < records_.size()) key_to_record_[keys[i]] = ri;
    }
    return true;
}

// One blob: u32 count, then `count` packed entries with NO alignment.
//
//   u64 parameterHash, u32 typeHash, u16 flags, u16 nameHashHi
//   texture (typeHash == TH_TEXTURE): u8 n, then n x 32 bytes of
//       {root-instance guid A, FILE guid B}. B resolves through the partition
//       index to an asset name.
//   inline: u32 n, then n == 1 ? scalar : 16*(n-1) + scalar
//       (cbuffer array packing - 16-byte element stride, last element unpadded)
bool Depot::params(size_t record, const std::vector<uint8_t>& d,
                   std::vector<DepotParam>& out, std::string& err)
{
    out.clear();
    err.clear();
    if (record >= records_.size()) { err = "no such record"; return false; }

    auto hit = cache_.find(record);
    if (hit != cache_.end()) { out = hit->second; return true; }

    const DepotRecord& rec = records_[record];
    const size_t off = (size_t)rec.offset, end = off + (size_t)rec.size;
    if (end > d.size() || rec.size < 4) { err = "blob outside the file"; return false; }

    const uint32_t cnt = rd<uint32_t>(d, off);
    size_t pos = off + 4;
    for (uint32_t i = 0; i < cnt; i++)
    {
        if (pos + 17 > end) { err = "header overrun"; return false; }
        DepotParam p;
        p.param_hash = rd<uint64_t>(d, pos);
        p.type_hash  = rd<uint32_t>(d, pos + 8);
        p.flags      = rd<uint16_t>(d, pos + 12);
        p.name_hi    = rd<uint16_t>(d, pos + 14);
        p.name32     = make_name32(p.param_hash, p.name_hi);
        pos += 16;

        if (p.type_hash != TH_TEXTURE)
        {
            const uint32_t n = rd<uint32_t>(d, pos);
            pos += 4;
            const int s = scalar_size(p.type_hash);
            if (s < 0)
            {
                char m[96];
                std::snprintf(m, sizeof(m), "unknown inline type hash %08x at entry %u",
                              p.type_hash, i);
                err = m;
                return false;
            }
            const size_t vlen = n <= 1 ? (size_t)s : 16 * (size_t)(n - 1) + (size_t)s;
            if (pos + vlen > end) { err = "value overrun"; return false; }
            p.count = n;
            p.raw.assign(d.begin() + (ptrdiff_t)pos, d.begin() + (ptrdiff_t)(pos + vlen));
            pos += vlen;
        }
        else
        {
            const uint8_t n = d[pos++];
            for (uint8_t k = 0; k < n; k++)
            {
                if (pos + 32 > end) { err = "texture ref overrun"; return false; }
                p.refs.emplace_back(guid_str16(d, pos), guid_str16(d, pos + 16));
                pos += 32;
            }
            p.count = n;
        }
        out.push_back(std::move(p));
    }

    if (pos != end)
    {
        // The blob must be consumed EXACTLY. Anything else means the entry
        // stream desynchronised, and the params read so far are not trustworthy
        // even though they look plausible.
        char m[96];
        std::snprintf(m, sizeof(m), "end mismatch: consumed %zu of %llu",
                      pos - off, (unsigned long long)rec.size);
        err = m;
        out.clear();
        return false;
    }
    cache_[record] = out;
    return true;
}

MaterialBinding Depot::textures_for(uint64_t state_key, const std::vector<uint8_t>& d)
{
    MaterialBinding out;
    auto it = key_to_record_.find(state_key);
    if (it == key_to_record_.end()) return out;

    std::vector<DepotParam> ps;
    std::string err;
    if (!params(it->second, d, ps, err)) return out;

    for (const DepotParam& p : ps)
    {
        if (p.type_hash == TH_TEXTURE)
        {
            if (p.refs.empty()) continue;
            out.textures[p.name32] = p.refs[0].second;   // the FILE guid
        }
        else out.constants[p.name32] = p.raw;
    }
    out.valid = true;
    return out;
}

const char* MaterialBinding::display_name(uint32_t name32)
{
    const char* n = Depot::slot_name(name32);
    return n ? n : "(unnamed)";
}

}  // namespace bf6
