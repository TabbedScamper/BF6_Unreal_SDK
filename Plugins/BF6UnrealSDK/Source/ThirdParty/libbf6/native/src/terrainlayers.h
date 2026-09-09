/* libbf6 internal - the terrain LAYER PALETTE and the material tree.
 *
 * Ported from the plugin's bf6_terrainlayers.gd and bf6_materialtree.gd, with
 * the research hub's layout taking precedence where the two disagree (the
 * divergences are called out at the point they bite, below and in the .cpp).
 *
 * WHAT THIS ANSWERS. `terrain.h` gives the SHAPE of the ground. This gives what
 * it is PAINTED WITH: the level's palette of layers, and for each layer the
 * concrete textures and shading constants the compositor feeds its shader.
 *
 * The chain, three resources deep:
 *
 *   <level>_terrain                            RES 0x1CA38E06
 *       the layer-combination table: a surface key and, per layer, its Link
 *       (which crater/debris set it belongs to).
 *   <level>_terrain.layergraphslayergraphs     RES 0xDE540C59
 *       the compiled layer graphs: per layer a ContentHash and a ShaderBlockKey.
 *   <level>_terrain.layergraphs_shaderblockdepot   RES 0x73312045
 *       per ShaderBlockKey, the texture refs and shading constants.
 *
 * TWO PROPERTIES OF THE KEYS THAT DECIDE THE DESIGN.
 *
 * 1. ShaderBlockKeys are CONTENT-ADDRESSED and shared between levels: the same
 *    authored layer carries the same key on every map that uses it. They are
 *    therefore NOT per-level ordinals, and a key is only meaningful inside a
 *    scope. The depot is picked by LEVEL NAME for that reason - a globally
 *    chosen depot binds colliding keys from another map, which produces a
 *    confidently wrong texture rather than a missing one.
 *
 * 2. The depot is content-deduplicated: several layers legitimately resolve to
 *    the SAME record (mp_dumbo has 47 keys over 40 records). Layers sharing a
 *    ContentHash are the same authored material, not a decode error.
 *
 * WHAT THIS MODULE DOES NOT DO, and where it hands off. The compositor also
 * needs streaming-tree blocks 1, 7 and 8 - the coverage tree, the
 * TerrainMaterialTree and the mask tree. Those live in `splat.h`, which owns
 * block 1 and therefore owns the per-node base lists a block-7 pair entry is
 * resolved against; a second block-7 decoder here would be the same bytes read
 * two ways in one library. The one list `splat.h` cannot produce is the LINKED
 * list, because it comes from THIS table's Link field - so this module supplies
 * `linked_list()` and MaterialTree::rasterize consumes it. That is the whole
 * seam between the two.
 *
 * THE ROW MAP, and why nothing here is keyed by field position. The compositor
 * consumes a layer as a fixed-size dword row, and the plugin's older reading
 * named parameters by where they sat. The row STRIDE AND ORDER VARY PER LEVEL:
 * measured with libbf6's own bindingset_test, mp_aftermath and mp_dumbo use a
 * 65-dword row and mp_tungsten a 64-dword row, because tungsten's graph does
 * not bind the breakup texture (0x0B725504) and every field after it shifts
 * down one slot. The NAME32 of a parameter is stable across all three. So the
 * join is by name32 and never by dword index; the dword numbers in the comments
 * below are aftermath's, recorded only so the two views can be lined up.
 */
#ifndef LIBBF6_TERRAINLAYERS_H
#define LIBBF6_TERRAINLAYERS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

class Source;

// ---------------------------------------------------------------------------
// Resource type codes and the name32s the terrain compositor reads.
// ---------------------------------------------------------------------------
namespace tl {

constexpr uint32_t kResLayerGraphs = 0xDE540C59;
constexpr uint32_t kResLayerComb   = 0x1CA38E06;
constexpr uint32_t kResDepot       = 0x73312045;

// The first half of MD5("") in digest byte order. A layer whose ContentHash is
// this hashed NOTHING, which is the palette's way of spelling an unused slot -
// levels declare a fixed-width palette and leave the tail unauthored.
constexpr uint64_t kEmptyContentHash = 0xd41d8cd98f00b204ull;

// Texture slots. Three material SETS exist; a layer that blends two authored
// materials (craters, road edges) populates two of them, which is exactly the
// case that defeats classifying textures by filename suffix.
//                                         aftermath row dword
constexpr uint32_t kTexBaseColorA    = 0x0929399A;   // 15  base colour
constexpr uint32_t kTexNormalHeightA = 0x2E50567A;   // 28  normal + height
constexpr uint32_t kTexThirdA        = 0x09293A41;   // 16  the third map
constexpr uint32_t kTexBaseColorB    = 0x2E5ACDA8;   // 54
constexpr uint32_t kTexNormalHeightB = 0xF9B44F08;   // 24
constexpr uint32_t kTexThirdB        = 0x2E5ACDF3;   // 55
constexpr uint32_t kTexBaseColorC    = 0x2E5B5189;   // 25
constexpr uint32_t kTexNormalHeightC = 0xF9C55709;   // 29
constexpr uint32_t kTexThirdC        = 0x2E5B51D2;   // 26
// Auxiliary maps, kept by name32 rather than promoted to a role.
constexpr uint32_t kTexDefault       = 0x09293810;   // 14
constexpr uint32_t kTexThirdA2       = 0xF860C3BE;   // 51  set A's second third-map
constexpr uint32_t kTexThirdB2       = 0x304613CC;   //  3  set B's second third-map
constexpr uint32_t kTexBreakup       = 0x0B725504;   // 27  absent on tungsten
constexpr uint32_t kTexNcs           = 0xB6C7E795;   // 58
constexpr uint32_t kTexHfd           = 0xAE16A5C0;   // 47
constexpr uint32_t kTexScatterNoise  = 0xEB1B291C;   // (other levels)
constexpr uint32_t kTexSsmDistance   = 0x07A9B250;   // (other levels)

// Scalars.
constexpr uint32_t kCOverlayStrength = 0xE68B2B10;   //  8  float
constexpr uint32_t kCUvOffset        = 0xCBB9A946;   // 48  float2
constexpr uint32_t kCDisplaceRange   = 0x5707A992;   // 50  float
constexpr uint32_t kCUvTiling        = 0xFA13C5B0;   // 52  float
constexpr uint32_t kCUvRotationDeg   = 0x3D27F349;   // 59  float
constexpr uint32_t kCCoordScaleX     = 0x53B7F668;   // 60  float
constexpr uint32_t kCCoordScaleY     = 0x53B7F669;   // 61  float
constexpr uint32_t kCTint            = 0x4FDCF6B1;   // 62  float3
constexpr uint32_t kCBaseHeight      = 0x4C200FE0;   //  2  float
constexpr uint32_t kCMaskRampExp     = 0xF7652FB3;   // 13  float
constexpr uint32_t kCHeightBlend     = 0x2F9990B7;   // 42  float
constexpr uint32_t kCSurfaceClass    = 0xCF3F97E0;   // 45  int / enum

}  // namespace tl

// One material set: the triple a single authored surface binds.
struct TerrainMaterialSet {
    std::string base_color;      // texture FILE guid, empty when unbound
    std::string normal_height;
    std::string third;           // the third map (occlusion-family)
    bool empty() const
    { return base_color.empty() && normal_height.empty() && third.empty(); }
};

// A layer's resolved material: what its ShaderBlockKey found in the depot.
//
// Scalars carry a `_set` companion because ZERO IS A REAL VALUE for most of
// these. A layer that does not author a tint is white, not black; a layer that
// does not author tiling wants the caller's own default, not 0 repeats per
// metre. Defaulting silently is how a palette comes out plausible and wrong.
struct TerrainLayerMaterial {
    // The key found a record in the depot. The AUTHORED identity of that
    // material is the layer's own ContentHash, not a record ordinal: the depot
    // is content-deduplicated, so two layers with one hash share one record and
    // that sharing is the format working, not a collision.
    bool resolved = false;

    TerrainMaterialSet set_a, set_b, set_c;

    // Lossless BindingSet source values.  The friendly fields below collapse
    // graph-template aliases into roles, which is useful to a conventional
    // renderer but insufficient for replaying the shipped evaluator: its row
    // is keyed by the original name32.  Keep both views of the same live depot
    // record so the exact path never has to reverse the normalization.
    std::map<uint32_t, std::string> raw_textures;
    std::map<uint32_t, std::vector<uint8_t>> raw_constants;

    // Everything else the record binds, by name32. Includes the auxiliary maps
    // above and any slot this table has not named - AN UNKNOWN SLOT IS NOT AN
    // ABSENT TEXTURE, and dropping it is how a layer that binds real sheets
    // reads as shader-computed.
    std::map<uint32_t, std::string> other_textures;

    float overlay_strength = 0.f;   bool overlay_strength_set = false;
    float uv_offset[2]     = {0, 0}; bool uv_offset_set = false;
    float displace_range   = 0.f;   bool displace_range_set = false;
    float uv_tiling        = 0.f;   bool uv_tiling_set = false;      // repeats/metre
    float uv_rotation_deg  = 0.f;   bool uv_rotation_deg_set = false;
    float coord_scale[2]   = {0, 0}; bool coord_scale_set = false;
    float tint[3]          = {1, 1, 1}; bool tint_set = false;
    float base_height      = 0.f;   bool base_height_set = false;
    float mask_ramp_exp    = 0.f;   bool mask_ramp_exp_set = false;
    float height_blend     = 0.f;   bool height_blend_set = false;
    int32_t surface_class  = 0;     bool surface_class_set = false;

    // Unrecognised inline constants, as they lie, so a consumer can go further
    // than this table without re-reading the depot.
    std::map<uint32_t, std::vector<uint8_t>> other_constants;

    // The primary colour sheet, preferring set A then B then C, then any named
    // slot that carries one. Empty when the layer is shader-computed.
    const std::string& base_color() const;
    const std::string& normal_height() const;

    // World metres per texture repeat. The constant is repeats per metre, so
    // the shader wants its reciprocal; an unauthored or degenerate value falls
    // back rather than dividing by ~0.
    float metres_per_repeat(float fallback = 4.0f) const;
};

// One entry of the compiled layer-graph table.
struct TerrainLayer {
    uint32_t index = 0;
    uint32_t masks[3] = {0, 0, 0};   // +0, +4, +8 - see the note in the .cpp
    uint32_t tail = 0;               // +28
    uint64_t content_hash = 0;       // +12, half-MD5 in DIGEST byte order
    uint64_t shader_block_key = 0;   // +20
    bool     empty = false;          // content_hash == MD5("")
    int      link = -1;              // crater/debris set, from the 0x1CA38E06 table
    TerrainLayerMaterial material;
};

// ---------------------------------------------------------------------------
// The palette reader.
// ---------------------------------------------------------------------------
class TerrainLayers {
public:
    // Mount `level` on `src` first. Returns false only when the chain cannot be
    // closed at all; a level whose layers partly fail to resolve still loads,
    // because a partial palette is usable and silently returning nothing is not.
    bool load(Source& src, const std::string& level, std::string& err);

    const std::vector<TerrainLayer>& layers() const { return layers_; }
    size_t layer_count() const { return layers_.size(); }

    uint64_t table_id() const { return table_id_; }      // layer-graph table identity
    uint64_t surface_key() const { return surface_key_; }// from the combination table

    const std::string& layergraph_res() const { return lg_name_; }
    const std::string& depot_res() const { return depot_name_; }
    const std::string& layercomb_res() const { return lc_name_; }
    // False means no depot names this level and one was borrowed. A borrowed
    // depot is reported rather than trusted: see the scope note at the top.
    bool depot_matched_level() const { return depot_matched_; }
    size_t depots_seen() const { return depots_seen_; }
    size_t record_offset() const { return record_offset_; }

    size_t empty_count() const;
    size_t resolved_count() const;         // layers whose key found a depot record
    size_t with_base_color_count() const;

    // ---- the palette's contribution to the block-7 compositor --------------
    //
    // A block-7 pair entry of kind 2 indexes the LINKED list: the crater and
    // debris set, which the layer-combination table marks with a Link. Every
    // shipped level measured carries exactly one such set of four consecutive
    // layers (mp_aftermath 35-38, mp_dumbo 42-45, mp_tungsten 29-32), which is
    // why four entries fit a 4-bit index with room to spare.
    //
    // ASCENDING BY LAYER INDEX, because the entry gives a nibble POSITION and a
    // layer inserted or omitted mid-list renames every index after it.
    // splat.h's MaterialTree::rasterize takes this as its `linked` argument;
    // empty is legal and resolves kind 2 against the base list instead.
    std::vector<int> linked_list() const;

    // The distinct Link values, and the members of one of them. A map with more
    // than one linked set would need the caller to choose; none measured does,
    // and finding one is a fact worth surfacing rather than averaging away.
    std::vector<int> link_groups() const;
    std::vector<int> linked_list(int link_group) const;

    // name32 -> a short human name, for reports. Null when unnamed.
    static const char* role_name(uint32_t name32);

private:
    std::vector<TerrainLayer> layers_;
    uint64_t    table_id_ = 0, surface_key_ = 0;
    std::string lg_name_, depot_name_, lc_name_;
    bool        depot_matched_ = false;
    size_t      depots_seen_ = 0, record_offset_ = 0;
};

// ---------------------------------------------------------------------------
// BLOCKS 1, 7 AND 8 ARE NOT HERE. See the hand-off note at the top of this
// file: `splat.h` owns the coverage tree, the TerrainMaterialTree and the mask
// tree, because it owns block 1 and therefore the per-node base lists a block-7
// pair entry has to be resolved against. This module's half of that seam is
// `TerrainLayers::linked_list()`.
// ---------------------------------------------------------------------------

}  // namespace bf6

#endif
