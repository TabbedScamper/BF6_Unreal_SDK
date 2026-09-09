/* Camera-relative mask atlas and ordered ComputeLayer work lists. */
#ifndef LIBBF6_TERRAINMASK_H
#define LIBBF6_TERRAINMASK_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

struct GroundCoverage;

struct TerrainMaskTransform {
    float scale_x = 0.f, scale_z = 0.f;
    float bias_x = 0.f, bias_z = 0.f;
};

struct TerrainMaskAtlas {
    int source_size = 0;
    int atlas_size = 0;
    int grid = 8;
    std::vector<uint8_t> r8;
    std::map<int, int> layer_panel;             // raw layer -> atlas panel
    std::map<int, TerrainMaskTransform> transform;

    // One list per 8x8 output tile. Lists are ascending evaluator order;
    // packed_work stores each list reversed because ComputeLayer consumes it
    // from the end. Offsets/counts are kept separate until the exact t13 head
    // encoding is validated by dispatch rather than guessed.
    int tiles_per_side = 0;
    std::vector<uint32_t> list_offset;
    std::vector<uint32_t> list_count;
    std::vector<uint32_t> tile_descriptor;      // pixel X | (pixel Z << 16)
    std::vector<uint32_t> packed_head;          // count low8 | offset high24
    std::vector<uint32_t> packed_work;          // two dwords per entry
};

bool build_terrain_mask_atlas(const GroundCoverage& coverage,
                              TerrainMaskAtlas& out, std::string& err);

} // namespace bf6
#endif
