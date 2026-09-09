#include "dbobject.h"

#include <cstdio>
#include <cstring>

namespace bf6 {

// DbType tags (bf6_container.gd 2.2)
enum {
    T_END = 0, T_ARRAY = 1, T_OBJECT = 2, T_NULL = 4, T_BOOL = 6,
    T_STRING = 7, T_INT = 8, T_LONG = 9, T_FLOAT = 11, T_DOUBLE = 12,
    T_TIMESTAMP = 13, T_GUID = 15, T_SHA1 = 16, T_MATRIX44 = 17,
    T_VECTOR4 = 18, T_BLOB = 19, T_ATTACHMENT = 20, T_TIMESPAN = 21
};
static const uint8_t ANONYMOUS = 0x80;
static const uint8_t TYPE_MASK = 0x1F;
static const size_t  DICE_HEADER = 556;

static uint32_t u32le(const uint8_t* d, size_t at) {
    return uint32_t(d[at]) | (uint32_t(d[at + 1]) << 8) |
           (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
}
static int32_t s32le(const uint8_t* d, size_t at) { return (int32_t)u32le(d, at); }
static int64_t s64le(const uint8_t* d, size_t at) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) v |= (uint64_t)d[at + k] << (8 * k);
    return (int64_t)v;
}
static float f32le(const uint8_t* d, size_t at) {
    uint32_t u = u32le(d, at); float f; std::memcpy(&f, &u, 4); return f;
}
static double f64le(const uint8_t* d, size_t at) {
    uint64_t u = 0; for (int k = 0; k < 8; k++) u |= (uint64_t)d[at + k] << (8 * k);
    double f; std::memcpy(&f, &u, 8); return f;
}
static std::string hex(const uint8_t* d, size_t at, size_t n) {
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t k = 0; k < n; k++) { s += H[d[at + k] >> 4]; s += H[d[at + k] & 0xF]; }
    return s;
}

// LEB128 -> {value, new_pos}. ok=false on overrun.
static bool read_varint(const uint8_t* d, size_t len, size_t pos,
                        int64_t& val, size_t& out_pos, std::string& err) {
    val = 0; int shift = 0;
    while (true) {
        if (pos >= len) { err = "varint ran off the end of the buffer"; return false; }
        uint8_t b = d[pos++];
        val |= (int64_t)(b & 0x7F) << shift;
        if ((b & 0x80) == 0) { out_pos = pos; return true; }
        shift += 7;
        if (shift > 63) { err = "varint wider than 64 bits"; return false; }
    }
}

static void read_cstr(const uint8_t* d, size_t len, size_t pos,
                      std::string& out, size_t& out_pos) {
    size_t end = pos;
    while (end < len && d[end] != 0) end++;
    out.assign((const char*)d + pos, end - pos);
    out_pos = (end < len) ? end + 1 : end;
}

// One framed field. Returns ok; sets is_end when it is an End marker, name (if
// present), val, and the new position.
static bool read_field(const uint8_t* d, size_t len, size_t pos,
                       bool& is_end, bool& has_name, std::string& name,
                       DbValue& val, size_t& out_pos, std::string& err) {
    is_end = false; has_name = false; name.clear(); val = DbValue();
    if (pos >= len) { err = "field header past the end of the buffer"; return false; }
    uint8_t tb = d[pos++];
    uint8_t t = tb & TYPE_MASK;
    if ((tb & ANONYMOUS) == 0 && t != T_END) {
        read_cstr(d, len, pos, name, pos);
        has_name = true;
    }
    switch (t) {
        case T_END:
            is_end = true; out_pos = pos; return true;
        case T_OBJECT:
        case T_ARRAY: {
            int64_t size; size_t p2;
            if (!read_varint(d, len, pos, size, p2, err)) return false;
            pos = p2;
            size_t end = pos + (size_t)size;
            if (end > len) { err = "container overruns the buffer"; return false; }
            if (t == T_OBJECT) {
                val.type = DbValue::Object;
                while (pos < end) {
                    bool e2, hn2; std::string n2; DbValue v2; size_t np;
                    if (!read_field(d, len, pos, e2, hn2, n2, v2, np, err)) return false;
                    pos = np;
                    if (e2) break;                 // End
                    if (hn2) val.obj[n2] = v2;      // anonymous fields dropped
                }
            } else {
                val.type = DbValue::Array;
                while (pos < end) {
                    bool e2, hn2; std::string n2; DbValue v2; size_t np;
                    if (!read_field(d, len, pos, e2, hn2, n2, v2, np, err)) return false;
                    pos = np;
                    if (e2 && !hn2) break;
                    val.arr.push_back(v2);
                }
            }
            out_pos = end; return true;
        }
        case T_NULL:  val.type = DbValue::Null; out_pos = pos; return true;
        case T_BOOL:  val.type = DbValue::Bool; val.b = d[pos] != 0; out_pos = pos + 1; return true;
        case T_STRING: {
            int64_t ln; size_t p2;
            if (!read_varint(d, len, pos, ln, p2, err)) return false;
            pos = p2;
            val.type = DbValue::String;
            if (ln > 0) val.s.assign((const char*)d + pos, (size_t)ln - 1);  // strip NUL
            out_pos = pos + (size_t)ln; return true;
        }
        case T_INT:   val.type = DbValue::Int;  val.i = s32le(d, pos); out_pos = pos + 4; return true;
        case T_LONG:
        case T_TIMESTAMP: val.type = DbValue::Long; val.i = s64le(d, pos); out_pos = pos + 8; return true;
        case T_FLOAT: val.type = DbValue::Float; val.d = f32le(d, pos); out_pos = pos + 4; return true;
        case T_DOUBLE: val.type = DbValue::Double; val.d = f64le(d, pos); out_pos = pos + 8; return true;
        case T_GUID:  val.type = DbValue::Guid; val.s = hex(d, pos, 16); out_pos = pos + 16; return true;
        case T_SHA1:
        case T_ATTACHMENT: val.type = DbValue::Sha1; val.s = hex(d, pos, 20); out_pos = pos + 20; return true;
        case T_MATRIX44: val.type = DbValue::Matrix44; val.blob.assign(d + pos, d + pos + 64); out_pos = pos + 64; return true;
        case T_VECTOR4:  val.type = DbValue::Vector4;  val.blob.assign(d + pos, d + pos + 16); out_pos = pos + 16; return true;
        case T_BLOB: {
            int64_t lb; size_t p2;
            if (!read_varint(d, len, pos, lb, p2, err)) return false;
            pos = p2;
            val.type = DbValue::Blob; val.blob.assign(d + pos, d + pos + (size_t)lb);
            out_pos = pos + (size_t)lb; return true;
        }
        case T_TIMESPAN: {
            int64_t raw; size_t p2;
            if (!read_varint(d, len, pos, raw, p2, err)) return false;
            val.type = DbValue::Timespan;
            val.i = (raw >> 1) ^ -(raw & 1);       // zigzag
            out_pos = p2; return true;
        }
    }
    char m[64];
    std::snprintf(m, sizeof(m), "DbType %d is not present in BF6 data", (int)t);
    err = m;
    return false;
}

DbValue read_dbobject(const uint8_t* d, size_t len, std::string& err) {
    // Strip the DICE header (magic 0x00CED100 / 0x01CED100) if present.
    if (len >= 4) {
        uint32_t magic = u32le(d, 0);
        if (magic == 0x00CED100u || magic == 0x01CED100u) {
            d += DICE_HEADER; len -= DICE_HEADER;
        }
    }
    bool is_end, has_name; std::string name; DbValue val; size_t pos;
    if (!read_field(d, len, 0, is_end, has_name, name, val, pos, err))
        return DbValue();
    return val;
}

}  // namespace bf6
