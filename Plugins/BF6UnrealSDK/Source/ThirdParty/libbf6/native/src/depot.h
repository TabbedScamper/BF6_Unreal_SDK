/* libbf6 internal - ShaderBlockDepot (module 8): which textures geometry binds.
 *
 * Ported from bf6_depot.gd. THIS IS THE LAST LINK. The walk says where a prop
 * goes, the meshset gives its geometry, the texture module turns a resource
 * into blocks - and this says WHICH textures. Without it the reader produces
 * untextured maps.
 *
 * The container is NOT the one Frosty documents for older Frostbite: there is
 * no 16-byte resMeta, no {offset,type} entry table, no five-type resource tree.
 *
 *   +0    u64 header_a  (= 16: the key table starts here)
 *   +8    u64 header_b  (= 0)
 *   +16   KEY TABLE   N x {u64 key; u64 recPtr}, recPtr an ABSOLUTE offset of a
 *         24-byte record. The table's length is NOT stored: it runs to the
 *         first record's data offset, because the key table and the blob area
 *         are back to back.
 *   +D    BLOB AREA   contiguous, content-deduplicated parameter blobs
 *   +R    RECORDS     M x {u64 dataOffset; u64 contentHash; u64 size}, ascending
 *                     and exactly tiling the blob area. That contiguity IS the
 *                     terminator: there is no count.
 *
 * THE JOIN, which is the part nobody had: a MeshSet section stores its 64-bit
 * shader state key inline at +0x130, and the record for that key holds the
 * section's textures. It is NOT the mesh variation database's shader guid or
 * id: those name the shader ASSET and appear nowhere in a depot.
 */
#ifndef LIBBF6_DEPOT_H
#define LIBBF6_DEPOT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

struct DepotParam {
    uint64_t param_hash = 0;
    uint32_t type_hash = 0;
    uint16_t flags = 0;
    uint16_t name_hi = 0;
    uint32_t name32 = 0;        // the FULL 32 bits; see the note in the .cpp
    uint32_t count = 0;
    // Texture entries: pairs of (root-instance guid, FILE guid). The FILE guid
    // is the one that resolves through the partition index to an asset name.
    std::vector<std::pair<std::string, std::string>> refs;
    std::vector<uint8_t> raw;   // inline scalars, as they lie
};

struct DepotRecord {
    uint64_t offset = 0, content_hash = 0, size = 0;
};

// What one piece of geometry binds: slot name -> texture FILE guid, plus the
// inline constants by name hash.
struct MaterialBinding {
    // Keyed by the slot's NAME32 rather than by a display name. A consumer maps
    // hashes to its own slots, and a name would mean going back through a table
    // to recover the number it was derived from.
    std::map<uint32_t, std::string> textures;         // name32 -> file guid
    std::map<uint32_t, std::vector<uint8_t>> constants;
    bool valid = false;

    // For reporting only.
    static const char* display_name(uint32_t name32);
};

class Depot {
public:
    bool parse(const std::vector<uint8_t>& d, std::string& err);

    // The blobs are parsed lazily: a depot holds thousands and a map touches a
    // fraction of them.
    bool params(size_t record, const std::vector<uint8_t>& d,
                std::vector<DepotParam>& out, std::string& err);

    MaterialBinding textures_for(uint64_t state_key, const std::vector<uint8_t>& d);

    size_t record_count() const { return records_.size(); }
    size_t key_count() const { return key_to_record_.size(); }
    bool   has_key(uint64_t k) const { return key_to_record_.count(k) != 0; }

    static const char* slot_name(uint32_t name32);
    static uint32_t    make_name32(uint64_t param_hash, uint16_t name_hi);

private:
    std::vector<DepotRecord>     records_;
    std::map<uint64_t, size_t>   key_to_record_;
    std::map<size_t, std::vector<DepotParam>> cache_;
};

}  // namespace bf6

#endif
