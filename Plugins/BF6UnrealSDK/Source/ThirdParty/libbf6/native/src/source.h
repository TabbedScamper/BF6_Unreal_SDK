/* libbf6 internal - the mount (module 2, final piece).
 *
 * Ties the mount together: a locator (cas_path), one or more parsed TOCs, and
 * for each bundle the segments-vs-payload zip that fills the res/ebx tables.
 * get_res / get_ebx then resolve a name to its CAS location and read+decompress
 * it. First mount wins on a name collision, matching bf6_source.gd.
 */
#ifndef LIBBF6_SOURCE_H
#define LIBBF6_SOURCE_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "caslocator.h"
#include "toc.h"

namespace bf6 {

struct ResEntry { CasLoc loc; uint32_t dsize = 0; uint32_t type = 0; uint64_t rid = 0; };
struct EbxEntry { CasLoc loc; uint32_t dsize = 0; };

class Source {
public:
    bool open(const std::string& game_dir, std::string& err);   // build the locator
    bool mount_toc(const std::string& toc_path, std::string& err);

    // Add one EBX from its proven TOC + bundle owner without importing every
    // unrelated name in that archive into the active mount/index.
    bool mount_ebx_owner(const std::string& toc_path,
                         const std::string& bundle_name,
                         const std::string& ebx_name,
                         std::string& err);

    // Called from inside the long loops. See the ABI's note: it can be called
    // from several threads at once and must not touch a UI. False asks to stop.
    using Progress = std::function<bool(const char*, int, int)>;
    void set_progress(Progress p) { progress_ = std::move(p); }

    // ---- mounting a LEVEL, not just the shared archives ----
    //
    // Non-level archives are always mounted. Level archives are normally just
    // the one asked for, which is right for READING a level: its terrain, its
    // placements, its lighting.
    //
    // It is the wrong answer for the objects a PLAYER can place. Measured on
    // mp_dumbo, one level's mount carries pf_portal_ prefabs for 1,609 of the
    // SDK's 10,883 placeables and another 235 reachable by folder name; the
    // remaining 9,039 are not in that mount in any form, because a prefab lives
    // in the bundles of the levels that use the object. all_levels mounts every
    // level so the whole catalogue resolves. It is not the default, because a
    // level read does not need it and it is not free.
    bool mount_level(const std::string& level, bool all_levels, std::string& err);

    // Focused front-end/armory mount. Discovers the current install's shared
    // TOCs at runtime, then mounts only the authored UI, weapons, characters,
    // vehicles, global, main-menu and English-localization families. This is
    // a path-class filter over the installed files, never a staged asset list.
    bool mount_frontend(std::string& err);

    // The .toc paths under the install, IN MOUNT ORDER. See the ordering law in
    // the .cpp: first mount wins, so shared archives go first, then the level
    // being read, then every other level.
    std::vector<std::string> find_tocs(const std::string& level, bool all_levels) const;

    // The level ids the install actually carries, from directory names only.
    std::vector<std::string> available_levels() const;

    // Partition guid -> "<name>.ebx", for resolving EBX imports to real names.
    // Every partition's EFIX header is read to build it, which is the whole
    // cost; the result is cached on this Source. First name wins, so the result
    // is stable across runs rather than dependent on iteration order.
    const std::map<std::string, std::string>& partition_index();

    // The armory follows imports only into authored hardware, gameplay and UI
    // families.  Building this runtime index reads that bounded name space
    // instead of every level partition; it is an optimization of the same
    // EFIX read path, not a staged/exported cache.
    const std::map<std::string, std::string>& armory_partition_index();

    // Some authored UI aliases deliberately share a partition GUID.  A
    // one-value index cannot represent that fact.  Callers that have an
    // authored discriminator (for example a Rime widget-reference Name) use
    // this live candidate set and must decline unresolved ambiguity.
    const std::map<std::string, std::vector<std::string>>& armory_partition_candidates();

    // WHICH BUNDLE A RESOURCE CAME IN, which is how a mesh finds its depot.
    //
    // A shader state key is only unique within a scope, and the scope is the
    // BUNDLE, not the directory: depots are named
    // <bundle asset path>_win32_shaderstate/shaderblockdepot_<n>. Nothing else
    // in the mount records this, so it has to be captured while the bundles are
    // being read.
    const std::string& bundle_of(const std::string& res_name) const;

    // Which bundle an EBX partition came in. This is the one that matters for
    // materials: the depot rule is THE PLACING BUNDLE - the bundle whose
    // placement pulled a mesh in - not the bundle the mesh resource happens to
    // live in. Measured over 79,000 placed instances on a retail level, the
    // placing bundle's own depot carries every one of its section keys.
    const std::string& bundle_of_ebx(const std::string& ebx_name) const;

    // The depot covering a bundle, or empty. Handles the one-token difference
    // that makes this fail silently: a TOC spells a bundle "win32/game/..."
    // while a depot spells it "game/...".
    //
    // On a miss it widens to the bundle's ANCESTORS and never to a sibling. A
    // state key is unique only within a scope, so a sibling that happens to
    // hold the key binds a material that merely collides - a confidently wrong
    // texture, which is worse than an untextured surface because nothing about
    // it looks broken.
    // Every bundle that has a depot. Exposed so a caller can find the bundle
    // that OWNS a part when the part's own resource bundle carries no
    // material - see bf6_part_bundle. Deliberately a caller's decision:
    // depot_for_bundle will not widen to a sibling by itself, because a
    // sibling holding the same key binds a confidently wrong texture.
    const std::map<std::string, std::string>& depots_by_bundle() const
    { return depot_by_bundle_; }

    std::string depot_for_bundle(const std::string& bundle) const;
    std::string depot_for_res(const std::string& res_name) const;

    static bool        is_level_toc(const std::string& path);
    static std::string mount_key(const std::string& path);

    std::vector<uint8_t> get_res(const std::string& name, std::string& err);
    std::vector<uint8_t> get_ebx(const std::string& name, std::string& err);
    // Loose chunk or bundle chunk, by guid hex (either spelling - see get_chunk).
    std::vector<uint8_t> get_chunk(const std::string& guid_hex, std::string& err);

    const std::string& game_dir() const { return game_; }
    size_t res_count() const { return res_.size(); }
    size_t ebx_count() const { return ebx_.size(); }
    // Catalogue membership without reading or decompressing the resource.
    // Registration paths use this when payload bytes are consumed later by a
    // dedicated decoder.
    bool has_res(const std::string& name) const
    { return res_.find(name) != res_.end(); }
    const std::unordered_map<std::string, ResEntry>& res() const { return res_; }

    /* RES ENTRIES SEEN BEFORE DEDUP, per type. res_ keys by NAME and the first
     * bundle wins, so res_.size() is a DISTINCT-NAME count. A resource shared
     * by many bundles is listed once per bundle in the payloads and counted
     * once here per listing. Exposed to settle what data/res_types.tsv counts:
     * its totals are ~18x res_.size() overall and vary 2x-76x per type, which
     * distinct names cannot explain but listings can. */
    const std::map<uint32_t, uint64_t>& res_entries_by_type() const { return res_entry_type_; }
    uint64_t res_entries_total() const { return res_entries_total_; }
    const std::unordered_map<std::string, EbxEntry>& ebx() const { return ebx_; }
    // The chunk tables, for a caller that enumerates rather than asking for one
    // guid it already knows. Two of them because they ARE two: a loose chunk is
    // in the TOC's own chunk list, a bundle chunk is a segment of a bundle, and
    // get_chunk looks in both.
    const std::map<std::string, CasLoc>& loose_chunks() const { return chunks_; }
    const std::map<std::string, CasLoc>& bundle_chunks() const { return chunk_seg_; }
    // Is this guid in either chunk map, WITHOUT reading it. A resource names
    // its chunk in one of two byte orders and the only way to know which is to
    // look; doing that with get_chunk would decompress a megabyte to answer a
    // yes/no question.
    bool has_chunk(const std::string& guid_hex) const;

private:
    std::string game_;
    CasLocator  loc_;
    std::unordered_map<std::string, ResEntry> res_;
    std::map<uint32_t, uint64_t> res_entry_type_;
    uint64_t res_entries_total_ = 0;
    std::unordered_map<std::string, EbxEntry> ebx_;
    std::unordered_set<std::string>           mounted_tocs_;
    // Full-content fingerprints of the exact live TOCs accepted into this
    // mount. They key the disposable armory index cache, so a patched install
    // cannot silently consume an index produced from older archive metadata.
    std::map<std::string, std::pair<uint64_t, uint64_t>> mounted_toc_hashes_;
    std::map<std::string, CasLoc>             chunks_;    // loose-chunk guid -> loc
    std::map<std::string, CasLoc>             chunk_seg_; // bundle-chunk guid -> loc
    Progress                                  progress_;
    std::unordered_map<std::string, std::string> res_bundle_;  // res -> bundle
    std::unordered_map<std::string, std::string> ebx_bundle_;  // ebx -> bundle
    std::map<std::string, std::string>        depot_by_bundle_; // bundle -> depot res
    std::map<std::string, std::string>        pidx_;      // partition guid -> name.ebx
    bool                                      pidx_built_ = false;
    std::map<std::string, std::string>        armory_pidx_;
    std::map<std::string, std::vector<std::string>> armory_pidx_candidates_;
    bool                                      armory_pidx_built_ = false;

    std::vector<uint8_t> read_seg(const CasLoc& seg, bool allow_raw, std::string& err);
};

}  // namespace bf6
#endif
