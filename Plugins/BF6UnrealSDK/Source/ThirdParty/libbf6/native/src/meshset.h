/* libbf6 internal - MeshSet decoder (module 6, geometry).
 *
 * Ported from bf6_meshset.gd. A MeshSet resource describes a mesh's LODs and
 * sections; the vertex/index bytes live in a referenced chunk. This header holds
 * the parse (metadata) half; read_lod (the actual buffers) follows.
 *
 * Independent of EBX and the type schema - it works from the resource bytes
 * alone, which is why one mesh can be decoded without a full level walk.
 */
#ifndef LIBBF6_MESHSET_H
#define LIBBF6_MESHSET_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf6 {

struct MeshDecl {
    std::vector<std::array<uint8_t, 4>> elements;   // usage, fmt, off, stream
    std::vector<std::array<uint8_t, 2>> streams;    // stride, classification
};

struct MeshSection {
    int         index = 0;
    std::string material;
    uint64_t    state_key = 0;
    uint16_t    material_id = 0;
    uint8_t     stride = 0;
    uint32_t    prim_count = 0;
    uint32_t    start_index = 0;
    uint32_t    vertex_offset = 0;
    uint32_t    vertex_count = 0;
    uint8_t     bones_per_vertex = 0;
    uint8_t     category_flags = 0;  // MeshSubsetCategoryFlags bits 0..4
    MeshDecl    decl;      // the declaration describing positions
    MeshDecl    decl0, decl1;
};

struct MeshLod {
    int          index = 0;
    int          section_count = 0;
    std::vector<MeshSection> sections;
    bool         idx32 = false;
    int          index_size = 0;
    int          vertex_size = 0;
    std::array<uint8_t, 16> chunk_id{};
    std::string  name;
    uint32_t     inline_offset = 0;
};

struct MeshSet {
    bool         ok = false;
    std::string  name;
    uint8_t      mesh_type = 0;
    int          lod_count = 0;
    int          section_count = 0;
    int          lod_stride = 0;
    std::vector<MeshLod> lods;
    // THE BONE PALETTE, from the header's bone/part block at 0xAC.
    //
    // Present only when mesh_type != 0 (Rigid skips the block entirely), so an
    // empty palette here means "this mesh declares none", not "we failed".
    // `bone_parts` holds bone_part_count skeleton bone ids; the array on disk is
    // padded up to a 16-byte boundary and the padding is NOT part of the
    // palette - reading the padded length appends zeros, which are a legal bone
    // id and would silently bind stray vertices to the root.
    std::vector<uint16_t> bone_parts;
    int          bone_count = 0;
    size_t       size = 0;
};

MeshSet meshset_parse(const uint8_t* d, size_t len, std::string& err);

// One renderable section's geometry, in the game's own space. Focused set:
// positions, normals, primary UV (TexCoord0) and triangle indices - enough to
// build a mesh. Material/destruction extras follow with the EBX + depot work.
struct MeshGeomSection {
    std::string           material;
    uint64_t              state_key = 0;
    uint16_t              material_id = 0;
    uint8_t               category_flags = 0;
    std::vector<float>    positions;   // xyz * vertex_count
    std::vector<float>    normals;     // xyz * vertex_count, or empty
    // THE PRIMARY UV, already chosen. Which channel that is depends on the
    // material FAMILY, and for car paint the depot names it - see the caller.
    // Consumers should sample this and not think about channels.
    std::vector<float>    uv0;         // uv * vertex_count, or empty
    // EVERY texcoord the section declares, indexed by channel (TC0..TC4), so a
    // caller that knows the rule can pick. Keyed by the declared USAGE rather
    // than by declaration order: the old code kept "the first one declared",
    // which is not TC0 in general and gave no way to ask for TC3 at all.
    std::vector<float>    uv[5];
    // SubMaterialIndex (usage 51), raw byte 0, not a normalized colour.
    std::vector<uint8_t>  layer_lanes;
    std::vector<float>    tangent_sign; // tangent xyz and binormal sign, usage 9
    std::vector<float>    color0;       // authored normalized Color0, not a tint palette
    std::vector<uint32_t> indices;
    // ONE DESTRUCTION PART INDEX PER VERTEX, or empty.
    //
    // The BoneIndices element (usage 2). On a Rigid or Composite destructible
    // this is the per-vertex DESTRUCTION PART; on a Skinned mesh the same
    // element is a skeleton bone, which is a different and differently sized
    // index space, so a caller must know which kind of mesh it has before
    // using this for anything.
    std::vector<uint16_t> parts;

    // PER-VERTEX SKIN BINDING, or empty when the section is not skinned.
    //
    // `influences` is 0, 4 or 8 - the count is decided by whether the section
    // declares the SECOND pair of elements (BoneIndices2 / BoneWeights2), not
    // by a field. Both arrays are influences * vertex_count, lane-aligned:
    // skin_weights[v * influences + k] is the weight for skin_bones[v * influences + k].
    //
    // WEIGHTS SUM ACROSS BOTH ELEMENTS, NOT WITHIN ONE. BoneWeights is UByte4N
    // and reads back as 0..1, but on an 8-influence section the four lanes of
    // BoneWeights alone do NOT sum to 1 - the remaining weight is in
    // BoneWeights2. Reading only the first element silently under-weights every
    // 8-influence vertex, which looks like a subtly collapsed mesh rather than
    // like an error. Here they are concatenated, so the sum is over all
    // `influences` lanes.
    //
    // `skin_bones` are SKELETON BONE IDS, not palette slots. The bone/part list
    // in the header is not a skinning palette - it holds 1 to 7 entries on a
    // character while these indices reach 210 - so remapping through it is
    // wrong and would run off the end.
    std::vector<uint16_t> skin_bones;
    std::vector<float>    skin_weights;
    int                   influences = 0;
};

// Decode one LOD's sections. `chunk` is the LOD's [vertex buffer][index buffer].
std::vector<MeshGeomSection> meshset_read_lod(const MeshSet& ms, int lod,
                                              const uint8_t* chunk, size_t chunk_len,
                                              std::string& err);

// A LOD WITH NO CHUNK IS NOT A LOD WITH NO GEOMETRY.
//
// When MeshSetLod.ChunkId is all zeros the geometry is stored INLINE in the
// MeshSet resource, laid out [vertex][index] exactly as a chunk would be.
// `inline_offset` (LOD+0x84) addresses it relative to a data base at the TAIL
// of the resource, not to its start, and the base is recoverable as
//
//   base = len - max over lods of (inline_offset + vertex_size + index_size)
//
// Measured on smokegrenade_projectile_mesh (215,536 bytes, 4 LODs): the maximum
// end is 210,400, giving base 5,136, and 5,136 + 194,432 + 13,568 + 2,400 lands
// exactly on the end of the file. The per-LOD offsets are the running sum of the
// preceding buffers rounded up to 8, so they are ascending but not contiguous
// to the byte - which is why the base is taken from the MAXIMUM end rather than
// from a running total.
//
// Only gadget, projectile and weapon meshes ship this way (9 of 10,571 mesh
// resources in one level's mount, every one under common/hardware), so a reader
// that skips it loses those and nothing else.
//
// Returns false when the LOD has a real chunk id, when the sizes are absent, or
// when the span would leave the resource. `out` points INTO `d`.
bool meshset_inline_lod(const MeshSet& ms, int lod, const uint8_t* d, size_t len,
                        const uint8_t** out, size_t* out_len);

}  // namespace bf6
#endif
