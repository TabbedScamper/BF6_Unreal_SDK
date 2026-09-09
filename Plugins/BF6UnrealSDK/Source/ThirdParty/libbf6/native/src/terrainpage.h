/* One complete, live CPU-side input set for a camera-relative DXIL page. */
#ifndef LIBBF6_TERRAINPAGE_H
#define LIBBF6_TERRAINPAGE_H

#include <string>
#include <vector>

#include "groundsplat.h"
#include "terrainmask.h"
#include "terrainshader.h"
#include "terraintextures.h"
#include "terrain.h"

namespace bf6 {

class Source;

struct TerrainStaticBinding {
    uint32_t descriptor = 0;             // common descriptor; shader register = 21+d
    TerrainBindlessTexture texture;
};

struct TerrainStaticUnresolved {
    uint32_t descriptor = 0;
    std::string file_guid;
    std::string resource;
    int32_t source_format = 0;
    int32_t source_width = 0;
    int32_t source_height = 0;
    int32_t source_slices = 0;
    std::string error;
};

struct TerrainPageInputs {
    GroundCoverage coverage;
    TerrainMaskAtlas masks;
    TerrainWindow height;
    TerrainShaderProgram shader;
    TerrainBindlessTable bindless;
    std::vector<TerrainStaticBinding> statics;
    // A page can be inspected even when a specialist common resource has a
    // still-undecoded RenderFormat. It is not COMPLETE until this is empty.
    std::vector<TerrainStaticUnresolved> unresolved_statics;
    std::vector<uint8_t> rows;
    TerrainRowBuildStats row_stats;
};

struct TerrainPageOpts {
    float rect_min[2] = {0.f,0.f};
    float rect_size = 64.f;
    int size = 64;
    int texture_max_dim = 512;            // 0 = authored top mip
};

bool prepare_terrain_page(Source& src, const std::string& level,
                          const TerrainPageOpts& opts,
                          TerrainPageInputs& out, std::string& err);

} // namespace bf6
#endif
