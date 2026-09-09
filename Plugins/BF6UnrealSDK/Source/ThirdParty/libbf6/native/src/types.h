/* libbf6 internal - Frostbite type layouts (module 4).
 *
 * Ported from bf6_types.gd, which is itself a port of typesdk.py. All three
 * read the same thing: the reflection metadata Frostbite leaves in the game
 * executable's `typeinfo` section. Layout per FrostyToolsuite FrostbiteVersion
 * 7 (2023+), type names stripped.
 *
 * WHY THIS EXISTS AT ALL: an EBX payload is not self-describing. An instance is
 * a bare struct, and reading one needs its type's field list - name hashes,
 * offsets, field types. Without this module nothing that walks a level can
 * work, which is why it is the first thing ported after the container layer.
 *
 * SAFE: the executable is read as a file. The running game is never touched.
 *
 * WHY C++ IS THE RIGHT HOME. Resolving one type means finding its 16-byte guid
 * inside a 5.3 MB section, and a level traversal resolves hundreds. The Godot
 * plugin measured 3.2 s per scan in GDScript against 29.6 ms native over the
 * whole 169 MB file, which is why it had to call out to a native find() and
 * why the schema is worth having here for both engines rather than in one.
 *
 * NOT PORTED YET, deliberately:
 *  - the EA App DRM lift (bf6_types.gd _lift_in_memory). An EA install wraps
 *    the executable, so the type sections read as ciphertext and every map
 *    comes back empty while meshes and terrain read perfectly. Detection IS
 *    ported (entropy), so the caller can say so plainly instead of shipping an
 *    empty map. The lift itself follows.
 *  - the generated type database (save_db/open_db), which is how an install
 *    that cannot read its own executable is served. It needs the lift's
 *    decision first, so it comes with it.
 */
#ifndef LIBBF6_TYPES_H
#define LIBBF6_TYPES_H

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

using TypeGuid = std::array<uint8_t, 16>;

struct FieldInfo {
    uint32_t name_hash  = 0;
    uint16_t flags      = 0;
    uint32_t offset     = 0;    // byte offset into the instance payload
    uint64_t type_va    = 0;    // resolve() turns this into a type
    uint8_t  ftype_enum = 0;    // (flags >> 5) & 0x1F
    uint8_t  fcategory  = 0;    // (flags >> 1) & 0x0F
};

struct TypeLayout {
    bool      valid       = false;
    TypeGuid  guid{};
    uint32_t  name_hash   = 0;
    uint16_t  flags       = 0;
    uint16_t  size        = 0;
    uint8_t   align       = 0;
    uint8_t   type_enum   = 0;   // 2 struct, 3 class, 4 array, 8 enum
    uint16_t  field_count = 0;   // as DECLARED; fields may hold fewer (see below)
    uint32_t  signature   = 0;
    uint64_t  super_va    = 0;
    std::vector<FieldInfo> fields;
};

struct ResolvedType {
    bool     valid   = false;
    TypeGuid guid{};
    uint16_t flags   = 0;
    uint8_t  te      = 0;        // type enum
    uint8_t  cat     = 0;        // category
    uint64_t elem_va = 0;        // arrays: the element type
};

// How far a superclass chain is followed before it is treated as a cycle.
constexpr int kMaxSuperDepth = 12;

class TypeDb {
public:
    // Read and parse the executable. False with err set on any failure, and a
    // SHORT READ IS A FAILURE: every lookup is a scan of this buffer, so a
    // partial read does not fail, it silently resolves nothing and the map
    // opens with terrain and textures perfect and no objects at all.
    bool open(const std::string& exe_path, std::string& err);

    // One type's own fields. Cached: every instance read walks its whole
    // superclass chain, so the same handful of types would otherwise be
    // rescanned millions of times over one traversal.
    const TypeLayout& layout(const TypeGuid& guid);

    // Including inherited fields, deduped by name hash. This is what a decoder
    // actually consumes.
    const TypeLayout& layout_full(const TypeGuid& guid);

    // A field's type_va -> what that type is.
    ResolvedType resolve(uint64_t type_va) const;

    // ---- diagnostics, all of which exist because a silent failure here looks
    // like an empty map rather than an error ----
    bool     typeinfo_found() const { return ti_found_; }
    uint64_t typeinfo_size()  const { return ti_size_; }
    uint64_t file_size()      const { return file_size_; }
    int      misses()         const { return n_miss_; }
    int      searches()       const { return n_find_; }
    int      fallbacks()      const { return n_fallback_; }

    // Shannon entropy over the type table, to tell "these ids are genuinely
    // absent" from "this region is encrypted on disk". Structured records with
    // padding measure about 3.4 bits per byte; ciphertext sits near 8.0. The
    // only opaque region in a plain build is the anti-tamper stub at 7.96, so
    // the difference is not subtle. On demand, never at open: a healthy install
    // must not pay for a diagnostic it will never print.
    void entropy(double& out_bits, double& out_zero_pct, size_t sample = 1u << 20) const;
    bool looks_encrypted() const;

    // "C:/.../Battlefield 6" -> the executables worth trying, best first. The SP
    // and MP builds carry DIFFERENT databases, so which one is loaded is a real
    // choice: the wrong one resolves types to the wrong layouts rather than
    // failing outright.
    static std::vector<std::string> exe_candidates(const std::string& game_dir);

    static std::string guid_str(const TypeGuid& g);

    /* THE OOA LIFT. An EA App install ships its reflection sections encrypted,
     * and an encrypted table does not fail to open - it resolves every type to
     * zero fields. open() therefore attempts an in-memory lift using the
     * machine's OWN licence before giving up. Nothing is written to disk and
     * the installed executable is never modified; on a machine with no licence
     * the lift simply does not happen and looks_encrypted() stays true.
     * `lift_note()` says what happened either way. */
    int                lifted()    const { return lifted_; }
    const std::string& lift_note() const { return lift_note_; }

private:
    int64_t  offset_of(uint64_t va) const;   // virtual address -> file offset, or -1
    int64_t  find_guid(const TypeGuid& guid);
    TypeGuid guid_at_typeinfo(uint64_t va) const;
    bool     type_guid_only(uint64_t type_va, TypeGuid& out) const;
    const TypeLayout& layout_full_depth(const TypeGuid& guid, int depth);
    std::vector<uint8_t> ooa_section_key(const std::string& content_id, std::string& note);
    int                  ooa_lift(std::string& note);
    int         lifted_ = 0;
    std::string lift_note_;

    struct Section {
        std::string name;
        uint32_t    va = 0, vsize = 0, raw_off = 0, raw_size = 0;
    };

    std::vector<uint8_t> data_;
    std::vector<Section> sections_;
    uint64_t             image_base_ = 0;
    uint64_t             file_size_  = 0;
    size_t               ti_off_ = 0, ti_end_ = 0;
    bool                 ti_found_ = false;
    uint64_t             ti_size_  = 0;
    int                  n_miss_ = 0, n_find_ = 0, n_fallback_ = 0;

    std::map<TypeGuid, TypeLayout> layout_cache_;
    std::map<TypeGuid, TypeLayout> full_cache_;
    TypeLayout                     empty_;
};

}  // namespace bf6

#endif
