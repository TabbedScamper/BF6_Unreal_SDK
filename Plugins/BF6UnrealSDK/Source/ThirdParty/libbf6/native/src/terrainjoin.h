/* libbf6 internal - recover the terrain layer/static-texture join from the
 * mounted game's ComputeLayer DXIL.
 *
 * This deliberately returns shader registers, not pre-solved material names.
 * TerrainStaticTable joins those registers to the current level's live common
 * BindingSet.  No generated TSV or compiled per-level table is a runtime
 * input.
 */
#ifndef LIBBF6_TERRAINJOIN_H
#define LIBBF6_TERRAINJOIN_H

#include <map>
#include <set>
#include <string>
#include <vector>

namespace bf6 {

struct TerrainDxilJoin {
    // Evaluator case N is terrain layer N. Values are sampled Texture2D SRV
    // registers in register space zero.
    std::map<int, std::set<int>> registers_by_layer;
    // All statically declared Texture2D SRV registers in space zero.  The
    // caller uses this set to derive the common BindingSet's register base.
    std::set<int> static_texture_registers;
    int attributed_samples = 0;
    int unattributed_samples = 0;
};

// Uses dxcompiler.dll from game_dir to disassemble `bytecode` in memory, then
// attributes texture samples through the evaluator's top-level switch CFG.
bool recover_terrain_dxil_join(const std::string& game_dir,
                               const std::vector<unsigned char>& bytecode,
                               TerrainDxilJoin& out, std::string& err);

} // namespace bf6

#endif
