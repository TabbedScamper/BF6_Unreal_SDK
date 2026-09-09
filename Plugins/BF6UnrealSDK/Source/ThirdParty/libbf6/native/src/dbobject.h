/* libbf6 internal - DICE DbObject parser (module 2, mount).
 *
 * Ported from bf6_container.gd. A self-describing binary object tree (like a
 * binary JSON) used by layout.toc and every SuperBundle *.toc. Each top-level
 * container may carry a 556-byte DICE header, which is stripped.
 */
#ifndef LIBBF6_DBOBJECT_H
#define LIBBF6_DBOBJECT_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

struct DbValue {
    enum Type {
        Null, Bool, Int, Long, Float, Double, String, Guid, Sha1,
        Matrix44, Vector4, Blob, Object, Array, Timestamp, Timespan
    } type = Null;

    bool     b = false;                       // Bool
    int64_t  i = 0;                           // Int / Long / Timestamp / Timespan
    double   d = 0;                           // Float / Double
    std::string s;                            // String; hex for Guid / Sha1
    std::vector<uint8_t> blob;                // Blob / Matrix44 / Vector4
    std::map<std::string, DbValue> obj;       // Object
    std::vector<DbValue> arr;                 // Array

    const DbValue* find(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

// Parse a top-level DbObject; strips the DICE header if present. On failure the
// result is type Null and `err` is set.
DbValue read_dbobject(const uint8_t* d, size_t len, std::string& err);

}  // namespace bf6
#endif
