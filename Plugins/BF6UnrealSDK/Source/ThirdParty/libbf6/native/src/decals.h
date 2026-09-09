/* libbf6 internal - TerrainDecals (module 11): roads and street markings.
 *
 * RES type 0x15E1F32E, one per level: the pre-tessellated compiled output of
 * the TerrainFillDecalData / TerrainQuadDecalData authoring instances. This is
 * what makes a road read as a road. Without it the street surface is still
 * drawn - it IS the terrain heightfield, block-7 paints it asphalt - but with
 * no markings, no mud, no wear and no kerb blending, which is what "the roads
 * are giant empty spaces" describes.
 *
 * THE CONTAINER
 *
 *   +0x00  u32   1
 *   +0x10  u32   slotCount        (guard: 0 < n <= 4096)
 *   +0x14  slotCount x Guid       sparse decal-asset slot table, empty = zeros
 *          u32   recordCount
 *          recordCount x record   variable size, two framings - see below
 *          RES-level parameter block
 *          vertex buffer          vertexCount x 32 B, records index by byte offset
 *          u32 4, u32 0           the VB-end landmark, which is how the VB is found
 *
 * TWO RECORD FRAMINGS SHIP and the container does not say which. The
 * FIXED-HEADER family leads each record with a 16-byte id and holds every
 * geometry field at a constant offset with the property stream last; the
 * PROPS-FIRST family is the layout TERRAIN.md 10.2 describes, whose tail has
 * to be found by scanning. The fixed walk is tried first because it is cheap
 * and self-checking: on a props-first map its FirstIndex chain collapses
 * immediately and the walk rejects itself.
 *
 * NO Y ON THE VERTICES. A decal carries world X and Z only and is draped on
 * the heightfield by the consumer. The record's AABB Y is the band its
 * authored geometry occupied and is worth exactly one thing - telling an
 * ELEVATED record (a rooftop court, a deck) from a street-level one. Clamping
 * every record into its band is a trap: where the rebuilt ground sits lower
 * than the ground the decal was compiled against, the clamp cannot follow the
 * terrain down and the marking hangs in the air.
 */
#ifndef LIBBF6_DECALS_H
#define LIBBF6_DECALS_H

#include <cstdint>
#include <string>
#include <vector>

namespace bf6 {

// One decoded vertex. Stride 8 floats, matching the reference's layout so the
// two readers can be diffed field for field.
struct DecalVertex {
    float x = 0.f, z = 0.f;     // world; there is no y
    float u = 0.f, v = 0.f;     // u across the ribbon 0..1, v arc length / Tiling0
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;   // alpha is the mud/dirt edge fade
};

struct DecalProp {
    uint64_t name = 0;
    enum class Kind : uint8_t { Texture, Float, Vec3, Vec2, Int, Bool }
                 kind = Kind::Texture;
    std::string  guid;               // Texture: the 16 raw bytes, dashed
    std::vector<float> values;       // Float / Vec3 / Vec2, flat
    // INTS ARE NOT FLOATS HERE. A stored 2 read as a float prints 2.8e-45,
    // which looks like a rounding problem rather than a type error.
    std::vector<int32_t> ints;       // Int / Bool
};

struct DecalRecord {
    uint32_t first_index = 0, tri_count = 0;
    float    tiling0 = 0.f, tiling1 = 0.f;
    float    aabb_min[3] = { 0, 0, 0 };
    float    aabb_max[3] = { 0, 0, 0 };
    uint32_t asset_slot = 0;
    uint32_t vb_off = 0, vb_size = 0;
    uint32_t prop_size = 0;
    std::vector<DecalProp> props;
};

class Decals {
public:
    bool parse(std::vector<uint8_t> bytes, std::string& err);

    const std::vector<DecalRecord>& records() const { return records_; }
    // Slot index -> the decal asset's instance guid, dashed, or "" for an
    // empty slot. A record with no slot is POSITIONAL rather than broken: the
    // AssetSlot is then a terrain layer-graph layer index and the surface is
    // that layer's own material, so a consumer should give it a neutral
    // material rather than drop it.
    const std::vector<std::string>& slots() const { return slots_; }

    // One record's vertices, decoded out of the shared buffer.
    std::vector<DecalVertex> vertices(const DecalRecord& r) const;

    // A planar record stores u = world X and v = world Z verbatim rather than
    // authoring u across and v along. Measured: all such records carry
    // Tiling0 == Tiling1 == 10. A consumer tiles those by the tilings instead.
    static bool is_planar(const std::vector<DecalVertex>& vs);

    // Reporting, and every one of these is a way the parse can be wrong while
    // still producing plausible numbers.
    const char* framing() const { return framing_; }
    bool     chain_ok()      const { return chain_ok_; }
    bool     anchor_ok()     const { return anchor_ok_; }
    bool     vb_from_anchor()const { return vb_from_anchor_; }
    int      truncated_at()  const { return truncated_at_; }
    uint32_t declared()      const { return declared_; }
    uint64_t triangles()     const { return total_tris_; }
    size_t   vb_start()      const { return vb_start_; }
    size_t   vb_size()       const { return vb_size_; }

    // Find the level's decal resource BY RES TYPE, not by name. The name is
    // "<level>/<level>_terraindecals" on every map sampled, but the type is
    // what the format guarantees and a map spelling its name differently would
    // otherwise silently ship no roads at all.
    static const uint32_t kResType = 0x15E1F32E;

private:
    int64_t parse_fixed(size_t start);
    bool    read_record(size_t p, int64_t expect_first, DecalRecord& out, size_t& next);
    int64_t find_anchor(size_t prop_end, int64_t expect_first) const;
    int64_t find_vb_anchor(size_t span, size_t stream_end) const;
    void    read_props(size_t p, size_t end, std::vector<DecalProp>& out) const;

    std::vector<uint8_t>     d_;
    std::vector<std::string> slots_;
    std::vector<DecalRecord> records_;
    uint32_t declared_ = 0;
    uint64_t total_tris_ = 0;
    size_t   vb_start_ = 0, vb_size_ = 0;
    const char* framing_ = "";
    bool  chain_ok_ = true, anchor_ok_ = false, vb_from_anchor_ = false;
    int   truncated_at_ = -1;
};

}  // namespace bf6

#endif
