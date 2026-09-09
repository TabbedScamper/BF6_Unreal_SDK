/* libbf6 internal - the terrain GROUND-MATERIAL COMPOSITE.
 *
 * WHAT THIS IS FOR. `terrain.h` gives the shape of the land, `splat.h` gives
 * which layers cover each point of it, and `terrainlayers.h` gives what each
 * layer is made of. None of the three produces a PIXEL. This does: it runs the
 * game's own ComputeLayer evaluator over a world window and hands a renderer a
 * finished RGBA8 albedo raster, so terrain can be drawn with its real materials
 * instead of a flat grey.
 *
 * WHERE THE MATH COMES FROM. Not from guessing what looks right. Every line of
 * the blend in the .cpp is transcribed from
 * BF6_Frostbite_Research/findings/terrain-computelayer-evaluator-decoded.md,
 * which is a full DXIL disassembly of MP_Aftermath's two terrain evaluator
 * kernels. The load-bearing parts, all reproduced:
 *
 *   coverage = saturate( mask + (hiRef - loRef) * heightBlendStrength )
 *   with the two height references pulled toward the running composite by
 *   pow(maskRamp, maskRampExponent), and mask >= 1 / mask <= 0 short-circuiting
 *   to 1 / 0. Then acc = lerp(acc, value, coverage) in ASCENDING layer order,
 *   quantised through binary16 after each layer while coverage stays float.
 *   The colour map is a literal Photoshop Overlay lerped by the layer's overlay
 *   strength. The output is sRGB-encoded with the exact IEC 61966-2-1 curve.
 *
 * THE TILING SLOT. A layer samples its sheet in WORLD SPACE at
 * `world / metres_per_repeat`, and that scale is the single thing that makes
 * ground read as ground rather than as a stretched photograph. It comes from
 * name32 0xFA13C5B0 via TerrainLayerMaterial::metres_per_repeat(). It is NOT
 * the displacement-range slot (0x5707A992); the shipped Godot plugin reads the
 * displacement slot as tiling and every layer it draws is therefore tiled at
 * roughly a tenth of its authored rate. Anything ported from that plugin
 * carries the same bug and should be corrected against this, not the reverse.
 *
 * BOTH TEXTURE ROUTES ARE NOW DECODED. A terrain layer's textures reach the
 * shader two ways: a bindless descriptor index in its ShaderLayerInfos row
 * (which the layer-graph depot fills, and which terrainlayers.h decodes), and
 * STATIC binding from the compute permutation's CommonBindingSet - 26 of the 40
 * layer bodies on Aftermath and 34 of 47 on Dumbo, where they are the road
 * surfaces. `terrainstatic.h` decodes the second route and recovers its
 * descriptor-to-layer join from the current level's live evaluator DXIL. This
 * file uses it, under TerrainBakeOpts::static_fallback, for any layer the depot
 * left without a colour. A layer neither route reaches is still SKIPPED, never
 * faked, and the texels it leaves are still counted in `texels_untouched`.
 *
 * NO ENGINE HEADERS, no exceptions, no image library. The BCn decoder is here
 * because the library genuinely has none: texture.h hands back compressed
 * blocks on purpose (an engine uploads them directly) and splat.cpp's private
 * BC4 page codec only ever meets 2,592-byte weight pages.
 */
#ifndef LIBBF6_TERRAINCOMPOSITE_H
#define LIBBF6_TERRAINCOMPOSITE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "splat.h"   // Splat and SplatChunkDir, for paint_colour_map

namespace bf6 {

class Source;

// ---------------------------------------------------------------------------
// BCn -> RGBA8. The library has no other one; see the note above.
//
// `dxgi` is the code TextureImage::dxgi carries. Handles BC1 (71/72), BC3
// (77/78), BC4 (80), BC5 (83) and BC7 (98/99). sRGB codes are decoded as the
// bytes lie - the curve is NOT undone here, because the caller has to know
// which of its inputs are colour and which are data, and a decoder that
// linearises normals is worse than one that linearises nothing.
//
// Rows are top-down, 4 bytes per texel, `w*h*4` bytes out. False with `err` set
// when the format is unsupported or the block payload is short for w/h.
// ---------------------------------------------------------------------------
// THE AERIAL COLOUR MAP, painted into a world window.
//
// Block 1 carries what the artists shot from above: one BC-compressed tile per
// quadtree key, 132 square with a two-texel apron, coarse keys covering the
// whole map and finer ones refining it. The ground materials are only half the
// colour - this is what puts a map's actual palette on them, and without it
// every layer draws at its stock studio colour.
//
// Painted coarse-first so a finer tile overwrites a coarser one, exactly as the
// weight pages composite. Writes size*size*3 sRGB bytes and leaves `rgb` EMPTY
// when the level ships no colour map, which is a real case and not an error.
// A per-tile decode failure is appended to `failures` if one is given and does
// not stop the paint.
//
// Lives here rather than in Splat because it needs the block decoder, and it is
// shared: the flattened bake folds it into each layer, and the per-pixel path
// hands it to a shader to do the same thing live.
bool paint_colour_map(const Splat& sp, const SplatChunkDir& dir,
                      const std::function<std::vector<uint8_t>(const std::string&)>& fetch,
                      const float lo[2], const float hi[2], int size,
                      std::vector<uint8_t>& rgb,
                      std::vector<std::string>* failures);

bool bcn_to_rgba8(const uint8_t* blocks, size_t nbytes, int w, int h, int dxgi,
                  std::vector<uint8_t>& rgba, std::string& err);

// ---------------------------------------------------------------------------
// What to bake.
// ---------------------------------------------------------------------------
struct TerrainBakeOpts {
    // The world window, in the same XZ metres the splat's root bounds use.
    // rect_size <= 0 bakes the whole map footprint. A window is the point of
    // the exercise: the weight pages are far denser than any whole-map raster
    // can afford, so a window recovers real data instead of upsampling.
    float rect_min[2] = {0, 0};
    float rect_size   = 0.f;

    int   size        = 1024;   // output texels per side
    // Resolution of the intermediate SplatCoverage. 0 follows `size`. Kept
    // separate because coverage and colour want different densities: coverage
    // comes from 66x66 pages and saturates, colour does not.
    int   splat_size  = 0;
    // Largest mip taken from each layer sheet. 0 -> 512. This is the memory
    // knob: a bake touching 13 layers at 2048 is 13 x 16 MB of RGBA.
    int   texture_dim = 0;

    // Three-tap stochastic tiling, the shader's own repetition breakup
    // (26 of 40 layer bodies use it). Off gives a single tap, which is visibly
    // more repetitive but otherwise the same colours.
    bool  stochastic  = true;
    // Apply the block-1 colour raster as the Overlay blend. Layers that author
    // no overlay strength are unaffected either way.
    bool  colour_map  = true;
    // Round every accumulator through binary16 after each layer, as the LDS
    // stores do. Off is faster and drifts from the game by <1/256.
    bool  quantise_f16 = true;
    bool  want_normal = true;

    // What a texel no textured layer reached comes out as. The shader's own
    // initial value is magenta (1, 0, 1) and that is kept, because a bake with
    // a hole in it should look like a bake with a hole in it.
    float fallback[3] = {1.f, 0.f, 1.f};

    // ...unless this is on and the level ships a colour map, in which case an
    // untouched texel takes the AERIAL PHOTOGRAPH of that spot instead.
    //
    // Magenta is right for a diagnostic and wrong for a viewport: on an urban
    // map two thirds of the ground can go untextured, because a layer whose
    // sheets are bound by register is only recoverable as far as the static
    // table's join reaches. The aerial map is real shipped data covering the
    // whole map, and where nothing else resolves it is by far the closest
    // answer available - it is literally a photograph of the ground being
    // drawn. Off by default so the library keeps telling the truth about its
    // own holes; a renderer turns it on.
    bool  fallback_colour_map = false;

    // Promote the FIRST layer that has a sheet to full coverage, for the colour
    // accumulator only (the height accumulators keep its real coverage).
    //
    // Why this exists, and why it defaults on. In the game the bottom of every
    // stack is a layer that always has a texture, so the magenta initial value
    // is always overwritten. Here the bottom of the stack is frequently one of
    // the statically-bound layers this library cannot resolve, and the layers
    // above it arrive at PARTIAL coverage - so `lerp(magenta, colour, 0.3)`
    // leaves two thirds magenta and the result is a pink haze across ground
    // that is not actually uncovered. Promoting the first real layer removes
    // the haze without inventing a colour: it says "whatever is lowest in this
    // stack that I can see, treat as the ground". Turn it OFF to see the raw
    // extent of the gap; `texels_untouched` counts genuine holes either way and
    // is unaffected by this switch.
    bool  prime_first_layer = true;
    // Metres per repeat for a layer that authors no tiling.
    float default_metres_per_repeat = 4.0f;
    int   threads = 0;          // 0 = hardware concurrency

    // Fall back to the STATICALLY BOUND texture table (terrainstatic.h) for a
    // layer the layer-graph depot gave no base colour. This is what puts an
    // asphalt street under an urban map; without it mp_dumbo's entire road grid
    // bakes as the fallback colour.
    //
    // ON BY DEFAULT. The descriptor table and its layer join are read from the
    // mounted game; if live DXIL disassembly or the register-base control fails,
    // the layer stays an honest hole. Turn this off for a bindless-only control;
    // `texels_untouched` counts genuine holes either way.
    bool  static_fallback = true;
};

// Per-layer accounting, so a wrong palette shows up as a named layer rather
// than as bad pixels.
struct TerrainBakeLayer {
    int         layer = 0;
    bool        painted = false;      // owns weight pages in block 1
    bool        base = false;         // appears in the block-7 base field
    bool        has_sheet = false;    // the depot bound a base colour
    bool        decoded = false;      // and it decoded to pixels
    std::string asset;                // base colour asset name, for reports
    int         width = 0, height = 0, dxgi = 0;
    float       metres_per_repeat = 0.f;
    bool        tiling_authored = false;
    uint64_t    texels = 0;           // texels where this layer had any coverage
    std::string failure;              // why it did not decode
};

struct TerrainBake {
    // ---- placement: everything a renderer needs to put this on the ground ---
    int   size = 0;
    float lo[2] = {0, 0};             // world XZ of the raster's low corner
    float hi[2] = {0, 0};
    float metres_per_texel = 0.f;

    // RGBA8, top-down, size*size*4. sRGB-ENCODED (IEC 61966-2-1), alpha from
    // the base-colour sheets' own alpha accumulator.
    std::vector<uint8_t> albedo;
    // RGBA8, size*size*4, empty when want_normal is false. RGB is the layer
    // (tangent-space) normal as 0.5*n + 0.5; A is the composite displacement
    // height remapped to 0..255 over -1..+1 metres.
    //
    // NOT the final world normal. The shader finishes by building a terrain
    // normal from the heightfield, combining it with this by Reoriented Normal
    // Mapping and rotating into world space. A renderer already has the
    // heightfield (terrain.h) and its own tangent frame, so doing that half
    // here would only be a worse version of what it does anyway.
    std::vector<uint8_t> normal;

    // ---- what actually happened -------------------------------------------
    int      layers_in_palette = 0;
    int      layers_present = 0;      // reached the raster at all
    int      layers_with_sheet = 0;   // ...and had a base colour bound
    int      layers_decoded = 0;      // ...and it decoded
    uint64_t texels_untouched = 0;    // no textured layer covered them
    double   mean_rgb[3] = {0, 0, 0}; // over the sRGB bytes, 0..255
    bool     colour_map_used = false;
    int      colour_tiles = 0;        // colour-map tiles decoded for the window

    // ---- HOW MIXED THE GROUND IS -------------------------------------------
    //
    // The complaint a chroma score cannot see is "the ground looks like several
    // textures averaged together". These measure that directly.
    //
    // `stack_hist[k]` counts texels whose evaluated stack held k textured
    // layers (k > 8 folded onto 8). `mix_dominant` is the mean, over texels
    // that drew anything, of the largest EFFECTIVE weight - the share of the
    // final colour one layer actually owns after the evaluator, which for an
    // ascending lerp chain is w_k = c_k * prod_{j>k}(1 - c_j). `mask_dominant`
    // is the same statistic on the RAW normalised mask, so the pair says how
    // much decisiveness the evaluator adds over a naive weight blend.
    // From the intermediate SplatCoverage: how many (texel, layer) paints the
    // four-slot merge rejected, and the average mask lost per coverage texel.
    uint64_t splat_evictions = 0;
    double   splat_evicted_mask = 0.0;

    uint64_t stack_hist[9] = {};
    double   mix_dominant = 0.0;
    double   mask_dominant = 0.0;
    double   mix_participation = 0.0;   // mean 1/sum(w^2): effective layer count

    std::vector<TerrainBakeLayer> layers;
    std::vector<std::string> failures; // texture decode failures, one line each
};

class TerrainComposite {
public:
    // `src` must already have the level mounted (Source::mount_level). Returns
    // false only when the chain cannot be opened at all - a level whose layers
    // partly fail to resolve still bakes, and says so in the report, because a
    // partial ground is usable and a silent empty one is not.
    static bool bake(Source& src, const std::string& level,
                     const TerrainBakeOpts& opt, TerrainBake& out,
                     std::string& err);
};

}  // namespace bf6

#endif
