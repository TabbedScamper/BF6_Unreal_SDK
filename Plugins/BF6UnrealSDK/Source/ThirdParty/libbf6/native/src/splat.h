/* libbf6 internal - the terrain SPLAT (streaming-tree block 1) and the
 * MATERIAL TREE (block 7), plus the mask tree (block 8) that shares block 7's
 * grammar.
 *
 * terrain.h/.cpp reads block 0, the shape of the land. This reads what the land
 * is MADE OF, and that answer comes out of two different trees that have to be
 * read together:
 *
 *   BLOCK 1, the layer coverage tree. A quadtree of nodes, each carrying a list
 *   of RECORDS. A record is (layer index, world rect, flags); most records own
 *   one stored 66x66 weight PAGE saying how much of that layer covers each
 *   point of the rect. Records whose bit 8 is set (IgnoreMask) have no page and
 *   full coverage over their rect - those are the map's BASE layers.
 *
 *   BLOCK 7, the TerrainMaterialTree. A second quadtree whose leaves are
 *   nibble-RLE rasters of 4-bit values, each value indexing a table of
 *   MATERIAL PAIRS. A pair entry names one of three ordered layer lists and two
 *   nibble indices into it. This is what says which base layer covers a texel,
 *   and on a city map it is nearly all of what the ground looks like: on
 *   mp_dumbo 30 layers are painted (2 of them textured) against 16 base layers
 *   (11 textured), so a consumer that reads only the weight pages gets a ground
 *   made of two textures.
 *
 * Ported from addons/highpoly_toggle/bf6_splat.gd and bf6_materialtree.gd,
 * which are themselves BF6_Frostbite_Research/formats/TERRAIN.md sections 5,
 * 7 and 8. The GDScript ships and works; where the research hub has since
 * corrected it, the hub wins and the divergence is called out at the site.
 * The four standing corrections, all noted again where they bite:
 *
 *   - block 7 is the TerrainMaterialTree and block 8 a MASK tree. Neither is
 *     "crater denial", which is a consumer of block 8, not its meaning.
 *   - block 2 is Density / DetailDisplacement and appears on 8k maps only. The
 *     GDScript calls it a water heightfield. Its NAME does not change any
 *     offset arithmetic, but the "which maps have it" does: it is a map-size
 *     property, not a rivers property.
 *   - a chunk is a sequence of PLANES consumed in ascending index, not the
 *     fixed [block0][pages][block2][tiles] the GDScript comments assert. Plane
 *     2 is optional BEFORE the colour plane and plane 5 optional AFTER it, and
 *     a paired pack is plane-major with the four children in reversed order
 *     [3,2,1,0]. See the note on Layout below for what that changes.
 *   - layer 0 is a real layer. It is never skipped when resolving a base
 *     material, and nothing here special-cases it.
 *
 * No engine headers, no exceptions. The BC4 page codec is decoded here rather
 * than handed to an image library, because 2,592-byte pages are the only BCn
 * this module meets and a 40-line decoder beats a dependency.
 */
#ifndef LIBBF6_SPLAT_H
#define LIBBF6_SPLAT_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

// ---------------------------------------------------------------------------
// The chunk directory that trails the typed blocks in the streaming-tree
// resource.
//
// terrain.cpp parses the identical structure into its own private DirEntry.
// This is a deliberate duplicate and not an oversight: Terrain::DirEntry and
// Terrain::read_chunk_directory are private, terrain.h is not mine to edit, and
// the alternative - reaching into another module's internals - is worse than
// twenty lines that both sides can be diffed against. If the two ever disagree,
// terrain.cpp is the original and this is the copy.
// ---------------------------------------------------------------------------
struct SplatDirEntry {
    uint32_t    primary_size = 0, paired_size = 0;
    std::string primary, paired;
};
using SplatChunkDir = std::map<uint64_t, SplatDirEntry>;

// One coverage record inside a block-1 node. 33 bytes on disk.
struct SplatRecord {
    uint16_t layer = 0;
    uint16_t id    = 0;
    float    lo[2] = {0, 0};       // world X, Z
    float    hi[2] = {0, 0};
    uint16_t flags = 0;
    // Index into this node's stored page list, or -1 for an IgnoreMask record:
    // no stored page, full coverage of its rect, and a BASE-layer candidate.
    int      page = -1;
};

struct SplatNode {
    uint64_t key   = 0;            // quadtree key, root 3, child (key<<4)|i
    int      depth = 0;
    float    lo[2] = {0, 0};
    float    hi[2] = {0, 0};
    std::vector<SplatRecord> records;
    int      pages = 0;            // stored pages, == count of records with page >= 0
};

// ---------------------------------------------------------------------------
// WHAT COMES OUT: per-texel layer coverage, as the TOP FOUR layers.
//
// The obvious shape - one full-resolution coverage raster per layer - is what
// the pages themselves are, and it is unaffordable at map scale: mp_dumbo has
// 47 layers and rasterises at 4096, so per-layer rasters are 47 x 16.7 M =
// 786 MB for data that is zero almost everywhere. A texel is covered by a
// handful of layers at most (it is a blend of ground materials, not a mixture
// of forty-seven), so the strongest few can be stored densely. Four is the
// historical/default width; callers that reproduce a saturated map's complete
// evaluator stack can request more through SplatCompositeOpts::max_slots.
//
//   idx[i*slots+s]  layer index of slot s at texel i
//   w  [i*slots+s]  its weight, 0..255. A ZERO WEIGHT MEANS THE SLOT IS EMPTY, and
//               slots fill from 0 upward, so the first zero ends the list.
// Slots are kept sorted by descending weight, so slot 0 is the winner.
//
// Weights are NOT normalised and do not sum to 255: they are the stored page
// values, and a texel can legitimately be covered 255 by two layers at once
// (the engine's own blend resolves that with the layer order). Anything that
// wants a partition of unity has to normalise, and should say so.
// ---------------------------------------------------------------------------
struct SplatCoverage {
    int      size = 0;                     // texels per side
    int      slots = 4;                    // stored layers per texel
    float    lo[2] = {0, 0};               // world XZ the raster spans
    float    hi[2] = {0, 0};
    std::vector<uint8_t> idx;              // size*size*slots
    std::vector<uint8_t> w;                // size*size*slots

    int      pages_painted = 0;            // decoded pages that landed in the raster
    uint64_t empty_texels  = 0;            // texels with no layer at all
    uint64_t layer_texels[256] = {};       // texels where the layer holds ANY slot
    int      layer_count = 0;              // layers with a nonzero count

    // WHAT THE FIXED-WIDTH MERGE THREW AWAY, so the simplification can be priced
    // rather than assumed. `slot_evictions` counts (texel, layer) paints that
    // found all requested slots already held; `evicted_weight` sums the masks that
    // lost, in 0..1, so dividing by the texel count gives the average mask
    // dropped per texel. The game keeps a mask per layer and walks the whole
    // work list, so anything large here is a real divergence from it.
    uint64_t slot_evictions = 0;
    double   evicted_weight = 0.0;

    // COUNTED ACROSS ALL STORED SLOTS, not just the winner. "which layer wins
    // here" and "which layers appear at all" are different questions and the
    // consumer wants the second: it decides which layers get a texture slice.
    // On mp_dumbo the difference was 2 slices against 12 - grass, sand and
    // cobblestone are almost never the single strongest layer at a texel.
};

// Options for a composite. Defaults give the whole-footprint bake.
struct SplatCompositeOpts {
    // Rasterise only this world window instead of the whole footprint. The
    // pages are denser than any whole-map raster can afford, so a window
    // recovers real data rather than upsampling. rect_size <= 0 means "whole".
    float rect_min[2] = {0, 0};
    float rect_size   = 0.f;
    // Skip records whose world rect is wider than this (<= 0 disables). Paired
    // with a pre-seeded output it is what makes a camera-following window cheap:
    // the far raster already holds every coarse record, so the window merges
    // only the pages finer than the far raster could keep.
    float max_span    = 0.f;
    // Bilinear-sample the pages instead of nearest. The 1-texel apron makes this
    // seamless without touching the neighbouring record - the apron texels ARE
    // the neighbour's, byte for byte. Worth it only where the raster is denser
    // than the pages; the whole-map bake stays nearest, since matching density
    // would buy nothing but blur.
    bool  smooth      = false;
    // Paint on top of whatever `out` already holds instead of clearing it. The
    // caller supplies the seed (typically an upsample of the far raster) - there
    // is nothing this module can do with it that the caller cannot.
    bool  seeded      = false;
    // Strongest layer masks retained per texel. Four preserves the historical
    // ABI and memory cost. MP_Isolated saturates four on every land texel and
    // loses 9.06 mask units/texel, so its live evaluator requests eight.
    int   max_slots   = 4;
    // 0 = hardware concurrency. 1 = serial. Bands are horizontal strips and each
    // band owns its bytes outright, so nothing is shared and the answer is
    // identical to the serial one: every band walks the same record list in the
    // same order and a texel belongs to exactly one band.
    int   threads     = 0;
};

// ---------------------------------------------------------------------------
// Where a colour tile lives, WITHOUT decoding it.
//
// The colour map is a BC7 (BC1 on one map) aerial photograph trailing the
// weight pages in the same chunks, and it is genuinely part of this walk: the
// page offsets and the tile offsets come out of the SAME residual
// decomposition, so a module that skipped the tiles would have to do the
// decomposition anyway and then throw half the answer away. What is NOT here is
// a BC7 decoder - that is texture.cpp's job and an engine's job, and shipping
// one here to satisfy a bookkeeping API would be the tail wagging the dog.
//
// KNOWN SHORTFALL, ported deliberately rather than fixed. This enumerates tiles
// the way the shipping GDScript does - by decomposing each chunk's residual -
// and it reproduces that reference exactly (mp_aftermath: 240 slices, the same
// 240 the plugin's own session logs report). The research hub describes a LATER
// extractor that instead takes block 4's `coverage=1` set as the authoritative
// colour ownership list, admits colour-only primaries, and accounts for the
// optional planes around the colour plane; it reaches 416/416 keys on Aftermath
// and 1,104/1,104 on Dumbo where this reaches 240 and 336. Closing that gap
// needs block 4 decoded, which is a separate module. It does NOT affect the
// weight pages: every stored page resolves on all maps tested, which is the
// part the splat depends on.
// ---------------------------------------------------------------------------
struct ColorSlice {
    uint64_t    key    = 0;        // quadtree key the tile covers
    std::string chunk;             // guid hex to fetch
    size_t      offset = 0;        // byte offset of the tile inside that chunk
    size_t      bytes  = 0;        // tile size; side and format are on the Splat
};

class Splat {
public:
    using FetchChunk = std::function<std::vector<uint8_t>(const std::string&)>;
    // (center x, center z, node width) -> that block-1 node's ascending
    // no-page layer list. Splat::base_list_at is the implementation; it is a
    // callback so block 7 can be rasterised against a list from anywhere.
    using BaseListAt = std::function<std::vector<int>(float, float, float)>;

    // ---- shared readers for the streaming-tree resource --------------------
    // Both duplicate a private terrain.cpp helper; see the note on
    // SplatDirEntry for why. `type` 0 heights, 1 splat, 2 density, 7 material
    // tree, 8 mask tree.
    static bool find_block(const std::vector<uint8_t>& res, int type,
                           std::vector<uint8_t>& out, std::string& err);
    static bool read_chunk_dir(const std::vector<uint8_t>& res, SplatChunkDir& out,
                               std::string& err);

    // §5.1: the metadata quadtree. Specified BYTE-EXACT and enforced as such -
    // a variable-size record stream that desynchronises still yields plausible
    // nodes whose layer indices belong to somebody else.
    bool parse(const std::vector<uint8_t>& block1, std::string& err);

    // Which page codec this map uses and where its pages start in each chunk.
    // Neither is stored anywhere; both are inferred. Must run before composite.
    bool detect_layout(const SplatChunkDir& dir, std::string& err);

    bool composite(const SplatChunkDir& dir, const FetchChunk& fetch, int size,
                   SplatCoverage& out, std::string& err,
                   const SplatCompositeOpts& opt = SplatCompositeOpts()) const;

    // Every colour tile the layout resolved, primary chunks then paired ones.
    // Empty when the map ships no colour raster we recognise, which is legal.
    std::vector<ColorSlice> color_slices(const SplatChunkDir& dir,
                                         const FetchChunk& fetch) const;

    // ---- the three ordered lists a block-7 pair entry indexes into (§8) ----
    // The ORDER is load-bearing - a pair entry gives nibble indices, so a layer
    // inserted or omitted mid-list renames every index after it. All three are
    // ascending by layer index, which is what §8 specifies for all three.
    std::vector<int> full_list() const;          // every layer block 1 mentions
    std::vector<int> global_base_list() const;   // every no-page layer, map-wide
    std::vector<int> base_list_at(float cx, float cz, float width) const;

    // painted[layer] / base[layer] -> record counts. A painted layer owns a
    // weight page; a base layer is IgnoreMask, full coverage over its rect.
    void layer_usage(std::map<int, int>& painted, std::map<int, int>& base) const;

    // ---- bookkeeping a consumer needs to address the data ------------------
    int    page_size() const { return page_size_; }
    int    page_side() const;                    // always 66; the codecs agree
    int    tile_bytes() const { return tile_bytes_; }
    int    tile_side()  const { return tile_side_; }
    bool   tile_is_bc1() const { return tile_bytes_ == 8712; }
    bool   no_colour()  const { return no_colour_; }
    int    layer_slot_count() const { return layer_slot_count_; }
    size_t node_count() const { return nodes_.size(); }
    int    declared_nodes()   const { return declared_nodes_; }
    int    declared_records() const { return declared_records_; }
    size_t stored_pages() const;
    const std::vector<SplatNode>& nodes() const { return nodes_; }
    const float* root_min() const { return root_min_; }
    const float* root_max() const { return root_max_; }
    // Forward byte offset of a node's first weight page inside its primary
    // chunk, or -1 when the chunk size does not decompose. See the .cpp: this is
    // FORWARD from the chunk head and never backward from the end.
    int    pages_offset(uint32_t primary_size, int pages) const;

    // A stored page -> 66*66 weights. Public because it is the one piece a
    // caller might want to run on bytes it obtained some other way.
    static bool decode_page(const uint8_t* raw, int size, uint8_t* out66);

    static int  depth_of(uint64_t key);
    static void bounds_of(uint64_t key, const float root_lo[2], const float root_hi[2],
                          float lo[2], float hi[2]);

private:
    bool read_node(const std::vector<uint8_t>& b, size_t& p, uint64_t key, int depth,
                   std::string& err);
    int  trailer_bytes(uint32_t primary_size, int pages) const;

    float  root_min_[2] = {0, 0}, root_max_[2] = {0, 0};
    int    layer_slot_count_ = 0, declared_nodes_ = 0, declared_records_ = 0;
    std::vector<SplatNode>      nodes_;
    std::map<uint64_t, size_t>  by_key_;          // key -> index into nodes_

    int  page_size_  = 0;
    int  tile_bytes_ = 0, tile_side_ = 0;
    bool no_colour_  = false;
    int  tail_const_ = -1;                        // model B constant, or -1

    // residual (primary_size - pages*page_size) -> derived offsets. Mutable
    // because an unseen residual is decomposed on demand and remembered; that is
    // a cache, not state, and it keeps the query methods const.
    mutable std::map<int, int> trailer_;          // residual -> trailer bytes
    mutable std::map<int, int> mip_;              // residual -> tile offset inside a mip pair
    mutable std::map<int, int> pfx_;              // residual -> forward page offset
};

// ---------------------------------------------------------------------------
// BLOCK 7, and the block-8 mask that shares its grammar.
// ---------------------------------------------------------------------------
struct MaterialNode {
    uint64_t key   = 0;
    int      depth = 0;
    std::vector<uint8_t> rows;     // dim*dim decoded 4-bit values
};

struct MaterialRaster {
    int      size = 0;
    float    lo[2] = {0, 0}, hi[2] = {0, 0};
    // The RAW pair-table index per texel, 0..15, or 255 where no block-7 node
    // covered the texel. This is the thing the format actually stores and it is
    // exposed separately from the resolved layer on purpose: resolution depends
    // on layer lists that come from OUTSIDE this module (the palette's linked
    // list), so a caller with a better list can re-resolve without re-walking.
    std::vector<uint8_t> pair;
    // The resolved base layer per texel, 255 where nothing resolved.
    std::vector<uint8_t> layer;
    uint64_t pair_hist[16] = {};
    uint64_t unset = 0;            // texels no node covered
};

class MaterialTree {
public:
    // Block 7: node stream at +0x24, pair-table footer at the end.
    bool parse(const std::vector<uint8_t>& block7, std::string& err);
    // Block 8: same node grammar, stream starts at +0x28, NO footer.
    //
    // The hub is explicit that this is a MASK tree, and that the "crater denial"
    // reading the GDScript comment carries is backwards: crater-denial markers
    // are a heavy CONSUMER of the region (77-100% of them sit inside it), which
    // is how the two got confused. It is a hard-ground descriptor. It is not a
    // render mask and must never be used to cut ground geometry - ray-casts
    // showed the engine draws its terrain inside the region like anywhere else.
    bool parse_mask(const std::vector<uint8_t>& block8, std::string& err);

    // 1 = terrain present, 0 = suppressed (bit 0 of the texel). Block 8 only.
    std::vector<uint8_t> hole_raster(int size) const;

    // Coarse-first, so finer nodes overwrite. `linked` may be empty: a map with
    // no linked layers resolves kind 2 against the base list, which is §8's own
    // stated fallback.
    bool rasterize(int size, const Splat::BaseListAt& base_at,
                   const std::vector<int>& full, const std::vector<int>& linked,
                   const std::vector<int>& global_base, MaterialRaster& out,
                   std::string& err) const;

    // §8 pair-entry field accessors, exposed because a consumer studying an
    // unfamiliar map wants to look at the table directly.
    static int  entry_list_kind(uint32_t e) { return (int)((e >> 16) & 0xF); }
    static int  entry_primary  (uint32_t e) { return (int)((e >> 8)  & 0xF); }
    static int  entry_secondary(uint32_t e) { return (int)((e >> 12) & 0xF); }
    static bool entry_is_framed(uint32_t e)
    { return (e & 0xFFu) == 0x80u && ((e >> 24) & 0xFFu) == 0x06u; }

    int  dim()        const { return dim_; }
    int  levels()     const { return levels_; }
    int  declared_nodes() const { return node_count_; }
    uint32_t background() const { return background_; }
    const std::vector<uint32_t>&     pairs() const { return pairs_; }
    const std::vector<MaterialNode>& nodes() const { return nodes_; }
    const float* world_min() const { return world_min_; }
    const float* world_max() const { return world_max_; }

private:
    bool read_node(uint64_t key, int depth, std::string& err);
    // -> layer index, or -1. `lists` is [full, node base, linked].
    int  resolve(int texel, const std::vector<int>* lists[3]) const;

    const std::vector<uint8_t>* d_ = nullptr;
    size_t   p_ = 0;
    int      dim_ = 0, levels_ = 0, node_count_ = 0;
    float    world_min_[2] = {0, 0}, world_max_[2] = {0, 0};
    uint32_t background_ = 0;
    std::vector<uint32_t>     pairs_;
    std::vector<MaterialNode> nodes_;
};

}  // namespace bf6

#endif
