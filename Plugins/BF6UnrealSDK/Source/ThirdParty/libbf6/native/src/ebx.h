/* libbf6 internal - RIFF-EBX container and value deserializer (module 5).
 *
 * Ported from bf6_ebx.gd, which ported ebx.py and ebx_deser.py together
 * because one is unusable without the other. Layout per
 * FrostyToolsuite/FrostySdk/IO/RiffEbx (FrostbiteVersion >= 2021).
 *
 * An EBX partition is RIFF with three chunks: EBXD (the instance data), EFIX
 * (the fixup table: type guids, instance offsets, imports) and EBXX. The data
 * is a flat block of bare structs and NOTHING IN IT says what any instance is.
 * The EFIX names each instance's type; the executable's type database (module
 * 4) says what fields that type has and where. All three are needed before a
 * single value can be read, which is why this arrives after the types.
 *
 * Several comments below record bugs that cost real placements, kept because
 * every one of them produced PLAUSIBLE output rather than an error, and a port
 * is exactly where they get silently reintroduced.
 */
#ifndef LIBBF6_EBX_H
#define LIBBF6_EBX_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "types.h"

namespace bf6 {

struct EbxValue;

// One decoded field value. A tagged union rather than a map of strings: a
// placement walk decodes hundreds of thousands of instances, and every string
// built here is one the caller usually never reads.
struct EbxValue {
    enum class Kind : uint8_t {
        Null, Bool, Int, Uint, Real, Str, Guid, ResRef,
        Struct, Array, InstanceRef, ImportRef, Unknown
    };

    Kind        kind = Kind::Null;
    bool        b    = false;
    int64_t     i    = 0;
    uint64_t    u    = 0;      // also ResRef's id, and Unknown's raw u32
    double      f    = 0.0;
    std::string s;             // Str, and ImportRef's partition guid
    TypeGuid    guid{};        // Guid's value, Struct's type
    uint8_t     te   = 0;      // Unknown: which type enum was not handled

    // Absolute byte position of a decoded Struct in data_.  This is internal
    // provenance, not a serialized value.  It lets a caller that has proved a
    // particular null-typed field is a PointerRef recover that one field at
    // its authored offset without teaching the general decoder to reinterpret
    // every unknown field as a reference.
    int64_t     source_pos = -1;

    int32_t     instance = -1; // InstanceRef: the target index, -1 if unresolved
    std::string import_path;   // ImportRef: resolved through the guid index
    std::string import_instance; // ImportRef: instance half of the EFIX import

    std::vector<EbxValue> items;                              // Array
    std::vector<std::pair<uint32_t, EbxValue>> fields;        // Struct, in layout order

    const EbxValue* field(uint32_t name_hash) const
    {
        for (const auto& kv : fields)
            if (kv.first == name_hash) return &kv.second;
        return nullptr;
    }
};

// Element stride by type enum, for arrays. Kept beside the scalar cases in
// decode() deliberately: an array element and a lone field of the same type
// must be read the same way, and once were not.
uint32_t ebx_elem_size(uint8_t te);

constexpr int      kEbxMaxDepth = 6;
constexpr uint32_t kEbxMaxArray = 100000;

class Ebx {
public:
    struct OpaqueArrayDescriptor {
        uint32_t offset = 0;
        uint32_t count = 0;
        uint32_t hash = 0;
        uint16_t flags = 0;
        uint16_t class_ref = 0xFFFF;
    };
    // The type reader is BORROWED, never owned. It holds a 176 MB executable
    // and a layout cache, and every partition in a level walk has to share one.
    explicit Ebx(TypeDb& types) : types_(types) {}

    // Optional: import partition guid -> the ebx name it refers to, so an
    // ImportRef can carry a readable path instead of a bare guid.
    void set_guid_index(const std::map<std::string, std::string>* idx) { guid_index_ = idx; }

    bool parse(std::vector<uint8_t> bytes, std::string& err);

    size_t             instance_count()  const { return instance_offsets_.size(); }
    size_t             exported_count()  const { return exported_instance_count_; }
    const std::string& partition_guid()  const { return partition_guid_; }

    // The instance GUID for instance i, or "" when it has none. It is the 16
    // bytes immediately BEFORE the instance's image start, and only the first
    // ExportedInstanceCount instances carry one: internal instances have no
    // identity and are not addressable from another partition. The network
    // registry names placements by (partition, instance), so this is what makes
    // a placement matchable against it.
    std::string instance_guid(size_t i) const;
    TypeGuid    instance_type(size_t i) const;
    /* The layout signature written into this partition for instance i's
     * concrete type.  This is not the executable reflection signature.  Rime
     * needs the distinction because its shipped records deliberately use an
     * older schema and a wrong offset still decodes as plausible data. */
    uint32_t    instance_signature(size_t i) const;

    // Decode instance i. With `want` non-empty, only those TOP-LEVEL field name
    // hashes are decoded and everything else on the instance is skipped.
    //
    // The filter is deliberately top-level only. A kept field still decodes in
    // full, nested structs and arrays included, which is what a walk relies on.
    // Filtering at depth would break those and save nothing extra, because the
    // nested work only happens under a field somebody asked for.
    EbxValue read_instance(size_t idx, const std::vector<uint32_t>* want = nullptr);

    // Raw payload access, for the handful of records whose fields are read at
    // FIXED offsets rather than through the schema. The water entity is the
    // case in hand: its transform sits at stable offsets while the schema'd
    // fields around its state key shift between the two shader variants, so
    // the reference reader takes the transform raw and the key deserialized.
    const std::vector<uint8_t>& raw() const { return data_; }
    int64_t  payload() const { return payload_; }
    uint32_t instance_offset(size_t i) const { return instance_offsets_[i]; }
    std::vector<OpaqueArrayDescriptor> opaque_array_descriptors() const;
    /* Read an opaque EBXX array from a caller-proven serialized field offset.
     * This bypasses executable reflection only for that one field; the EBXX
     * descriptor remains authoritative for count and bounds. */
    bool opaque_array_at(int64_t absolute_field_pos, uint32_t element_stride,
                         std::vector<uint8_t>& bytes,
                         OpaqueArrayDescriptor* descriptor = nullptr) const;

    // A FIELD THE TYPE TABLES CALL AN INTEGER AND THE BYTES CALL A POINTER.
    // For a handful of fields the reflection says int32 where the payload holds
    // an ordinary internal PointerRef. A light entity's spatial component points
    // at its light this way, and without following it nothing joins the two.
    // Separate from read_instance on purpose: the type says int, and a reader
    // that reinterpreted every int as a pointer would resolve garbage on the
    // ones that really are numbers.
    int32_t int_pointer(size_t idx, uint32_t name_hash);

    // A REFERENCE THE REFLECTION CANNOT DESCRIBE. Sibling to int_pointer, and
    // the opposite failure: for the asset-reference fields on a
    // VisualEnvironment - PanoramicTexture, SkyGradientTexture,
    // CloudShadowTexture, HdrColorGradingLut - the type table hands back a null
    // type_va, so decode() resolves nothing and read_instance reports the field
    // as Null. The PAYLOAD is a perfectly ordinary import pointer: an odd i64
    // whose top bits are the import index, exactly what pointer_ref reads on
    // every field the reflection DOES describe.
    //
    // Left out of read_instance on purpose. A null type_va means the width and
    // meaning of those bytes are unknown, and reading every such field as a
    // pointer would invent references on fields that hold something else. Here
    // the caller names the field and takes responsibility for it.
    //
    // Returns false when the field is absent, zero, or out of range. On success
    // `partition_guid` is the PARTITION half of the import record - the half a
    // cross-partition lookup keys on - and `path` is that guid resolved through
    // the guid index, or empty when no index was set or it does not know it.
    bool import_ref(size_t idx, uint32_t name_hash,
                    std::string& partition_guid, std::string& path);

    // Decode one PointerRef at a caller-proven absolute payload position.
    // This is intentionally narrower than making the reflection reader guess
    // about null-typed fields.  Rime's shipped armory config uses its older
    // 16-byte row layout, so its category reference is only trustworthy at the
    // raw row offset established from the partition itself.
    bool import_ref_at(int64_t absolute_pos,
                       std::string& partition_guid, std::string& path);

    // Decode a caller-proven PointerRef field on any already-decoded struct.
    // Unlike import_ref(), this also works for nested array rows and returns
    // both GUID halves needed for an exact identity join.
    bool import_ref_field(const EbxValue& owner, uint32_t name_hash,
                          std::string& partition_guid,
                          std::string& instance_guid,
                          std::string& path);

    // THE IMPORT TABLE, both halves.
    //
    // An EFIX Imports[] record is PartitionGuid(16) + InstanceGuid(16), and a
    // cross-partition lookup keys on the PARTITION half. Comparing the instance
    // half instead answers a different question and still appears to work,
    // because the two are equal on a minority of records (48 of 400 in the
    // mp_dumbo root); it then fails silently on the other 88%. Both are exposed
    // so a caller cannot pick one by accident.
    //
    // The list is also a SELECTOR, not a dependency dump: a level root imports
    // only the assets it actually turns on. That is how the active
    // VisualEnvironment preset is identified - see velighting.cpp.
    struct Import { std::string partition, instance; };
    size_t              import_count() const { return imports_.size(); }
    const Import&       import_at(size_t i) const { return imports_[i]; }

    // Counters, so the `want` filter's reach can be judged rather than assumed.
    static uint64_t n_inst, n_top, n_nested, n_arr_elem;
    static void     reset_counts();

private:
    struct ExtendedDescriptor {
        uint32_t offset = 0;
        uint32_t count = 0;
        uint32_t hash = 0;
        uint16_t flags = 0;
        uint16_t class_ref = 0xFFFF;
    };

    const TypeLayout& layout(const TypeGuid& g);
    EbxValue  read_struct(const TypeGuid& g, int64_t base, int depth);
    EbxValue  read_struct_only(const TypeGuid& g, int64_t base, int depth,
                               const std::vector<uint32_t>& want);
    EbxValue  decode(int64_t pos, uint64_t type_va, int depth);
    EbxValue  decode_boxed(int64_t pos, int depth);
    EbxValue  decode_extended(int64_t pos, const ExtendedDescriptor& desc,
                              int depth);
    EbxValue  pointer_ref(int64_t pos);
    EbxValue  read_array(int64_t pos, uint64_t elem_va, int depth);
    EbxValue  scalar(int64_t p, uint8_t te) const;
    std::string cstring(int64_t pos) const;

    TypeDb& types_;
    const std::map<std::string, std::string>* guid_index_ = nullptr;

    std::vector<uint8_t> data_;
    int64_t              payload_ = 0;
    std::string          partition_guid_;
    std::vector<TypeGuid>  type_guids_;
    std::vector<uint32_t>  type_signatures_;
    size_t                 exported_instance_count_ = 0;
    std::vector<uint32_t>  instance_offsets_;
    std::vector<uint64_t>  resource_refs_;
    std::vector<Import>    imports_;
    std::map<uint32_t, ExtendedDescriptor> array_descriptors_;
    std::map<uint32_t, ExtendedDescriptor> boxed_descriptors_;

    std::map<uint32_t, size_t> inst_map_;    // payload offset -> instance index
    std::vector<int32_t>       inst_type_;   // instance index -> type_guids_ slot, -1 none
};

}  // namespace bf6

#endif
