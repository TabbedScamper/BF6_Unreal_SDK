/* libbf6 internal - the parts a destructible prop HIDES at spawn.
 *
 * A destructible prop's damaged look is built INTO its intact mesh: the
 * deflated tyre, the cracked windscreen, the crushed panel are all present in
 * the same geometry, tagged per vertex with a part index, and hidden by the
 * game until that piece actually breaks. Nothing separate is placed, so there
 * is no placement to filter out - a reader that draws the mesh as it lies
 * draws every parked car with its own wreck inside it, which reads as
 * overlapping, stretched and corrupt bodywork rather than as a missing filter.
 *
 * THE RULE (DESTRUCTION.md 4.3), and the "and" is the whole of it:
 *
 *     a part is hidden IFF HealthStateIndex != 0
 *     AND an intact (state 0) twin exists for the same PartComponentIndex
 *
 * Culling every non-zero state instead removes legitimate authored geometry:
 * plenty of props have parts whose damaged look IS the intended look, and
 * those have no state-0 twin.
 *
 * THE TABLE IS ON THE PROP, NOT THE MESH. `X_mesh` is owned by `X` in the same
 * folder, and the table is one field on one of that prop's instances.
 *
 * SKINNED MESHES ARE EXEMPT. On a playable vehicle the per-vertex BoneIndices
 * value is a SKELETON BONE - a different and differently sized index space -
 * and indexing the part table with a bone id would cull arbitrary pieces of
 * every aircraft.
 */
#ifndef LIBBF6_DESTRUCTION_H
#define LIBBF6_DESTRUCTION_H

#include <cstdint>
#include <set>
#include <string>

namespace bf6 {

class Source;
class TypeDb;

// The part indices this prop hides, or empty. `mesh_res` is the mesh resource
// name; the prop is that name with the "_mesh" suffix removed.
std::set<uint16_t> destruction_hidden_parts(Source& src, TypeDb& types,
                                            const std::string& mesh_res);

}  // namespace bf6

#endif
