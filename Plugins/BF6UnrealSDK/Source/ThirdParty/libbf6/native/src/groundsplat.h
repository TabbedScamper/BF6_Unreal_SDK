/* libbf6 internal - the ground as WEIGHTS plus a material list, for renderers
 * that blend per pixel instead of consuming a flattened bake.
 *
 * WHY THIS EXISTS ALONGSIDE terraincomposite.
 *
 * terraincomposite flattens the ground into one albedo raster. That is the
 * right shape for an offline look, an atlas or a thumbnail, and the wrong
 * shape for a viewport: a whole-map raster lands at two to four metres a
 * texel, while the layer materials themselves repeat every one to seven
 * metres. Flattening at that density averages each material away before the
 * renderer ever sees it, and the result reads as a low-resolution photograph
 * of ground rather than as ground.
 *
 * The game does not flatten either. It composites into virtual-texture pages
 * at whatever density the camera needs, which is a streaming system no
 * consumer of this library is going to reimplement. The practical equivalent
 * is what every terrain renderer does: carry the COVERAGE, which varies
 * slowly and rasterises happily at a couple of metres, and sample the
 * MATERIALS per pixel at their own tiling, blending by that coverage. The
 * detail then comes from the material sheets at full resolution and only the
 * mixing weights are baked.
 *
 * So this hands a binding two things:
 *   - a coverage raster: per texel, a compact ordered layer stack
 *   - the material list those indices refer to, with each layer's sheet and
 *     the tiling it is authored at
 */
#ifndef LIBBF6_GROUNDSPLAT_H
#define LIBBF6_GROUNDSPLAT_H

#include <cstdint>
#include <string>
#include <vector>

#include "source.h"

namespace bf6 {

// One entry of the material list a coverage index refers to.
struct GroundMaterial {
    int32_t     layer = 0;             // the layer index this came from
    std::string albedo_res;            // texture resource, empty when unbound
    std::string normal_res;
    // Authored auxiliary opacity/coverage sheet. The generated evaluator may
    // multiply the stored terrain mask by this texture before height blending;
    // omitting it turns sparse rock/debris breakup into an opaque paint tile.
    std::string coverage_res;
    float       metres_per_repeat = 4.f;
    float       uv_rotation_deg = 0.f;
    float       tint[3] = {1.f, 1.f, 1.f};
    // How strongly this layer takes the aerial colour map, as a Photoshop
    // Overlay. Defaults to FULL: the authored field is optional and its
    // absence means "yes", which a default of zero silently turns into a map
    // painted in stock studio colours.
    float       overlay = 1.f;

    // THE EVALUATOR CONSTANTS. The weights in the raster are the game's raw
    // MASK, not coverage: the ComputeLayer kernel turns one into the other per
    // pixel, using each layer's own height. Skipping that step and normalising
    // the masks is what turns ground into a four-way average of everything
    // near it - measured on MP_Dumbo, the dominant layer carries only 33% of
    // the mask, so nothing ever wins outright and no material reads.
    //
    //   coverage = saturate(mask + (hiRef - loRef) * height_blend)
    //
    // with the two height references pulled toward the running composite by
    // pow(maskRamp, mask_ramp_exp), evaluated in ASCENDING layer order. A
    // consumer that samples the height sheet can run exactly this.
    float       base_height = 0.f;
    float       displace_range = 0.f;
    float       mask_ramp_exp = 1.f;
    float       height_blend = 0.f;
    float       coord_scale[2] = {1.f, 1.f};
    float       uv_offset[2] = {0.f, 0.f};
};

struct GroundCoverage {
    int   size = 0;
    int   slots = 8;
    float lo[2] = {0, 0};              // world XZ of the low corner, metres
    float hi[2] = {0, 0};

    // size*size*slots each. idx[i*slots+s] indexes MATERIALS (not raw layer ids), so a
    // consumer can bind exactly the sheets it needs; 255 means "no layer".
    // w[i*slots+s] is that slot's mask, 0..255, in EVALUATION order: the block-7
    // base at full mask first when one resolves, then the retained block-1
    // paint layers in ascending raw-layer order. The first zero ends the list.
    std::vector<uint8_t> idx;
    std::vector<uint8_t> w;

    std::vector<GroundMaterial> materials;

    // THE AERIAL COLOUR MAP over the same rectangle, size*size*3 sRGB bytes,
    // or empty when the level ships none. Each layer Overlay-blends this by
    // its own `overlay` strength, which is what carries a map's real palette;
    // the sheets alone are studio colour and land grey or wrongly tinted.
    std::vector<uint8_t> colour;

    uint64_t empty_texels = 0;
};

// Window controls used by camera-relative consumers. A non-positive rect_size
// uses the live splat root read from the mounted game. There is deliberately no
// built-in Portal SDK overlay box: it is not a game-runtime source and it is
// not the combat volume on every map.
struct GroundCoverageOpts {
    int   size = 2048;
    int   max_slots = 8;
    float rect_min[2] = {0.f, 0.f};
    float rect_size = 0.f;             // <= 0: the live level splat footprint
};

bool ground_coverage(Source& src, const std::string& level,
                     const GroundCoverageOpts& opts,
                     GroundCoverage& out, std::string& err);

// Builds the coverage and the material list for a level. `src` must already
// have the level mounted.
bool ground_coverage(Source& src, const std::string& level, int size,
                     GroundCoverage& out, std::string& err);

}  // namespace bf6
#endif
