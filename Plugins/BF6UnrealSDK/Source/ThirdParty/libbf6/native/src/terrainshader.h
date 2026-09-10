/* Live terrain-compositor program and binding schema.
 *
 * This is the read path behind the DXIL terrain page baker.  Nothing here is
 * generated or keyed to one game build: it follows the mounted level's shader
 * state database to the compute permutation, its shared data, all three
 * BindingSets, and the CompiledBytecode resource named by that permutation.
 */
#ifndef LIBBF6_TERRAINSHADER_H
#define LIBBF6_TERRAINSHADER_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

class Source;

struct TerrainShaderDecl {
    uint64_t param_hash = 0;
    uint32_t type_hash = 0;
    uint16_t flags = 0;
    uint16_t name_hi = 0;
    uint32_t destination = 0;
    uint16_t meta = 0;

    uint32_t name32() const
    {
        return ((uint32_t)name_hi << 16) |
               (uint32_t)((param_hash >> 48) & 0xffffu);
    }
};

struct TerrainShaderBindingRecord {
    uint32_t destination_span = 0;
    std::vector<TerrainShaderDecl> declarations;
};

// Common and variant binding schemas for one identified raster program.
bool load_raster_bindings(Source& src, uint64_t permutation,
    std::vector<TerrainShaderBindingRecord>& records, std::string& err);

struct TerrainShaderProgram {
    uint64_t permutation_id = 0;
    uint64_t shared_data_id = 0;
    uint64_t common_binding_set = 0;
    uint64_t variant_binding_set = 0;
    uint64_t layer_binding_set = 0;
    std::string bytecode_guid;
    std::string bytecode_resource;
    std::vector<uint8_t> bytecode;       // bare DXBC/DXIL container
    std::vector<TerrainShaderBindingRecord> common_records;
    std::vector<TerrainShaderBindingRecord> variant_records;
    std::vector<TerrainShaderBindingRecord> layer_records;

    uint32_t layer_row_stride() const;
};

// Ubershader zero is the ComputeLayer evaluator used by the terrain page
// compositor on the measured retail levels.  The program and GUID are still
// read from this level's current mounted data rather than assumed.
bool load_terrain_shader(Source& src, const std::string& level,
                         TerrainShaderProgram& out, std::string& err,
                         int ubershader = 0);

class TerrainLayers;

// Materializes the live ShaderLayerInfos rows.  Texture values are remapped to
// the caller's descriptor heap; every inline value is copied from the mounted
// layer depot by name32.  Defaults are reported because they are evaluator
// inputs too, not silently treated as authored data.
struct TerrainRowBuildStats {
    uint32_t rows = 0;
    uint32_t authored_values = 0;
    uint32_t defaulted_values = 0;
    uint32_t bound_textures = 0;
    uint32_t missing_textures = 0;
};

bool build_terrain_layer_rows(const TerrainShaderProgram& program,
                              const TerrainLayers& layers,
                              const std::map<std::string, uint32_t>& descriptors,
                              std::vector<uint8_t>& rows,
                              TerrainRowBuildStats& stats,
                              std::string& err);

} // namespace bf6
#endif
