/* libbf6 internal - the terrain compositor's STATICALLY BOUND texture table.
 *
 * WHY THIS EXISTS. A terrain layer's textures reach the ComputeLayer evaluator
 * two ways. One is BINDLESS: the layer's ShaderLayerInfos row carries a
 * descriptor index the layer-graph depot fills, and `terrainlayers.h` decodes
 * that. The other is STATIC: the compositor's compute permutation binds the
 * sheets directly and the generated evaluator samples them by register. Only
 * the first was decoded anywhere in this library, so on mp_dumbo 34 of 47 layer
 * bodies - the whole street grid of Manhattan Bridge - arrived with no colour.
 *
 * THE CHAIN, transcribed from
 * BF6_Frostbite_Research/findings/terrain-static-texture-table-resolved.md:
 *
 *   level shaderstate_db -> compositor program 0x9F11D96B0FDF4773
 *     -> +0x60/+0x64 indexed by UberShaderIndex -> permutation id
 *     -> expressionshader/permutation<id>          (36-byte compute form)
 *     -> +0x00 -> expressionshader/permutationshareddata/<id>
 *     -> +0x18 -> CommonBindingSetId
 *     -> expressionshader/bindingset/<id>
 *          destination = DESCRIPTOR INDEX (steps by 1), paired with a Name32
 *     -> the level's ShaderBlockDepot: the ONE record carrying those Name32s
 *     -> texture FILE guid -> asset name via Source::partition_index()
 *
 * Note the destination semantic. In the ShaderLayerInfos set the destinations
 * are byte offsets into a row and step by 4; here they are descriptor indices
 * and step by 1 (`shaderlayerinfos-row-map-read-from-disk`).
 *
 * THE HARD PART: WHICH LAYER OWNS WHICH TRIPLE.
 *
 * The descriptor table names textures, not layers. The join therefore comes
 * from the mounted level's evaluator bytecode on every run: BF6's own
 * dxcompiler.dll disassembles the in-memory DXIL, a CFG walk attributes each
 * Texture2D sample to the top-level switch case, and case N is layer N. The
 * common BindingSet register base is derived from the live resource metadata;
 * no per-level table, exported disassembly or ordinal guess is consumed.
 */
#ifndef LIBBF6_TERRAINSTATIC_H
#define LIBBF6_TERRAINSTATIC_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

class Source;

// The role a sheet plays, read off the asset name's suffix - the same trick
// that named the prop depot slots. An unsuffixed sheet is Unknown, never
// guessed at, because a normal map promoted to base colour is worse than none.
enum class TerrainTexRole {
    Unknown,
    BaseColor,        // _cv
    NormalHeight,     // _nhs, _noh
    AmbientOcclusion, // _ao
    Opacity,          // _op
    Smoothness,       // _cs
    Mixed             // _mxx
};

const char* terrain_tex_role_name(TerrainTexRole r);

struct TerrainStaticTexture {
    uint32_t       descriptor = 0;
    uint32_t       name32 = 0;
    std::string    file_guid;   // resolves through Source::partition_index()
    std::string    asset;       // leaf asset name, ".ebx" stripped
    std::string    stem;        // asset leaf with the role suffix removed
    TerrainTexRole role = TerrainTexRole::Unknown;
};

// One material's worth of consecutive descriptors, in DESCENDING order.
struct TerrainStaticGroup {
    std::vector<TerrainStaticTexture> tex;
    int  base_color = -1;      // index into tex, -1 when the group binds none
    int  normal_height = -1;
    int  third = -1;           // the ao / op / cs map
    bool utility = false;      // every member is a compositor-owned shared sheet
    int  top = 0;              // highest descriptor in the group
    const std::string& stem() const;
};

class TerrainStaticTable {
public:
    // `src` must already have the level mounted. False only when the chain
    // cannot be opened; a table that resolves partly still loads and says so.
    bool load(Source& src, const std::string& level, std::string& err,
              int ubershader = 0);

    // descriptor index -> texture, for every declared descriptor that resolved.
    const std::map<uint32_t, TerrainStaticTexture>& textures() const { return tex_; }
    // Material groups, DESCENDING (groups()[0] is the top of the table).
    // Utility-only groups and everything inside the prologue are already gone:
    // this is the allocation list a layer walk consumes.
    const std::vector<TerrainStaticGroup>& groups() const { return groups_; }

    // ---- the exact live layer join -----------------------------------------
    // Returns true when the mounted evaluator has an attributed case for this
    // layer. -1 is a real answer for a role the case does not sample.
    bool layer_descriptors(int layer, int& cv, int& nh, int& third) const;
    bool join_exact() const { return join_exact_; }
    int register_base() const { return register_base_; }
    int register_base_hits() const { return register_base_hits_; }
    int register_base_runner_up_hits() const { return register_base_runner_up_hits_; }
    int attributed_samples() const { return attributed_samples_; }
    int unattributed_samples() const { return unattributed_samples_; }
    const std::string& bytecode_guid() const { return bytecode_guid_; }

    // ---- provenance, so a wrong table shows up as a named resource ---------
    uint64_t     binding_set() const { return binding_set_; }
    const std::string& depot_res() const { return depot_res_; }
    size_t       depot_record() const { return depot_rec_; }
    size_t       declared() const { return declared_; }   // descriptors declared
    size_t       resolved() const { return tex_.size(); } // ...that named a texture
    int          prologue_top() const { return prologue_top_; }

private:
    std::map<uint32_t, TerrainStaticTexture> tex_;
    std::vector<TerrainStaticGroup>          groups_;
    uint64_t    binding_set_ = 0;
    std::string depot_res_;
    size_t      depot_rec_ = 0, declared_ = 0;
    int         prologue_top_ = -1;
    struct Triple { int cv = -1, nh = -1, third = -1; };
    std::map<int, Triple> layer_join_;
    bool        join_exact_ = false;
    int         register_base_ = -1;
    int         register_base_hits_ = 0;
    int         register_base_runner_up_hits_ = 0;
    int         attributed_samples_ = 0;
    int         unattributed_samples_ = 0;
    std::string bytecode_guid_;
};

}  // namespace bf6

#endif
