/* Live bindless texture table for terrain ShaderLayerInfos rows. */
#ifndef LIBBF6_TERRAINTEXTURES_H
#define LIBBF6_TERRAINTEXTURES_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "texture.h"

namespace bf6 {

class Source;
class TerrainLayers;

struct TerrainBindlessTexture {
    uint32_t descriptor = 0;
    std::string file_guid;
    std::string resource;
    int32_t source_format = 0;           // Frostbite RenderFormat, even on refusal
    int32_t source_width = 0;
    int32_t source_height = 0;
    int32_t source_slices = 0;
    TextureImage image;                 // authored BCn blocks/mips, not RGBA cache
};

struct TerrainBindlessTable {
    // Descriptor zero is reserved for the caller's neutral/dummy texture.
    std::map<std::string,uint32_t> descriptor_by_guid;
    std::vector<TerrainBindlessTexture> textures;
    uint32_t declarations = 0;
    uint32_t unique_guids = 0;
};

bool load_terrain_texture(Source& src, const std::string& file_guid,
                          int max_dim, TerrainBindlessTexture& out,
                          std::string& err);

// Empty active_layers means the full palette. max_dim=0 retains mip0; a
// positive cap selects an authored mip for bounded diagnostic dispatches.
bool load_terrain_bindless(Source& src, const TerrainLayers& layers,
                           const std::set<int>& active_layers, int max_dim,
                           TerrainBindlessTable& out, std::string& err);

} // namespace bf6
#endif
