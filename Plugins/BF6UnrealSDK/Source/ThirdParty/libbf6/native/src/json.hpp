/* json.hpp - a tiny, self-contained JSON DOM parser.
 *
 * The only thing in libbf6 that is not the game's binary formats: the SDK ships
 * its placeable catalogue as two plain JSON files (level_info.json,
 * asset_types.json). This reads them. Deliberately minimal - objects, arrays,
 * strings, numbers, bool, null, and the common string escapes - which is all the
 * SDK's machine-generated JSON uses. Not a general-purpose library.
 */
#ifndef BF6_JSON_HPP
#define BF6_JSON_HPP

#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace bf6json {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<Value>                          arr;
    std::vector<std::pair<std::string, Value>>  obj;

    bool is_obj() const { return type == Obj; }
    bool is_arr() const { return type == Arr; }
    bool is_str() const { return type == Str; }

    // Object member lookup; null if absent or not an object.
    const Value* find(const char* key) const {
        if (type != Obj) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const std::string& as_str(const std::string& dflt) const {
        return type == Str ? str : dflt;
    }
    int as_int(int dflt) const { return type == Num ? (int)num : dflt; }
};

class Parser {
public:
    Parser(const char* data, size_t len) : p_(data), end_(data + len) {}

    bool parse(Value& out, std::string& err) {
        skip_ws();
        if (!value(out)) { err = err_.empty() ? "parse error" : err_; return false; }
        return true;
    }

private:
    const char* p_;
    const char* end_;
    std::string err_;

    void skip_ws() {
        while (p_ < end_) {
            char c = *p_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p_++;
            else break;
        }
    }

    bool value(Value& v) {
        skip_ws();
        if (p_ >= end_) { err_ = "unexpected end"; return false; }
        char c = *p_;
        switch (c) {
            case '{': return object(v);
            case '[': return array(v);
            case '"': { v.type = Value::Str; return string(v.str); }
            case 't': case 'f': return boolean(v);
            case 'n': return null(v);
            default:  return number(v);
        }
    }

    bool object(Value& v) {
        v.type = Value::Obj;
        p_++;                       // '{'
        skip_ws();
        if (p_ < end_ && *p_ == '}') { p_++; return true; }
        for (;;) {
            skip_ws();
            if (p_ >= end_ || *p_ != '"') { err_ = "expected key"; return false; }
            std::string key;
            if (!string(key)) return false;
            skip_ws();
            if (p_ >= end_ || *p_ != ':') { err_ = "expected ':'"; return false; }
            p_++;
            Value child;
            if (!value(child)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            skip_ws();
            if (p_ >= end_) { err_ = "unterminated object"; return false; }
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == '}') { p_++; return true; }
            err_ = "expected ',' or '}'"; return false;
        }
    }

    bool array(Value& v) {
        v.type = Value::Arr;
        p_++;                       // '['
        skip_ws();
        if (p_ < end_ && *p_ == ']') { p_++; return true; }
        for (;;) {
            Value child;
            if (!value(child)) return false;
            v.arr.push_back(std::move(child));
            skip_ws();
            if (p_ >= end_) { err_ = "unterminated array"; return false; }
            if (*p_ == ',') { p_++; continue; }
            if (*p_ == ']') { p_++; return true; }
            err_ = "expected ',' or ']'"; return false;
        }
    }

    bool string(std::string& s) {
        p_++;                       // opening quote
        while (p_ < end_) {
            char c = *p_++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p_ >= end_) break;
                char e = *p_++;
                switch (e) {
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    case '/':  s += '/';  break;
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case 'r':  s += '\r'; break;
                    case 'b':  s += '\b'; break;
                    case 'f':  s += '\f'; break;
                    case 'u': {             // \uXXXX -> UTF-8 (BMP only)
                        if (end_ - p_ < 4) { err_ = "bad \\u"; return false; }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = *p_++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            else { err_ = "bad hex"; return false; }
                        }
                        if (cp < 0x80) s += (char)cp;
                        else if (cp < 0x800) {
                            s += (char)(0xC0 | (cp >> 6));
                            s += (char)(0x80 | (cp & 0x3F));
                        } else {
                            s += (char)(0xE0 | (cp >> 12));
                            s += (char)(0x80 | ((cp >> 6) & 0x3F));
                            s += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: s += e; break;
                }
            } else {
                s += c;
            }
        }
        err_ = "unterminated string";
        return false;
    }

    bool boolean(Value& v) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "true", 4) == 0) {
            v.type = Value::Bool; v.b = true;  p_ += 4; return true;
        }
        if (end_ - p_ >= 5 && std::strncmp(p_, "false", 5) == 0) {
            v.type = Value::Bool; v.b = false; p_ += 5; return true;
        }
        err_ = "bad literal"; return false;
    }

    bool null(Value& v) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "null", 4) == 0) {
            v.type = Value::Null; p_ += 4; return true;
        }
        err_ = "bad literal"; return false;
    }

    bool number(Value& v) {
        const char* start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) p_++;
        while (p_ < end_) {
            char c = *p_;
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
                c == '+' || c == '-') p_++;
            else break;
        }
        if (p_ == start) { err_ = "bad number"; return false; }
        v.type = Value::Num;
        v.num = std::strtod(start, nullptr);
        return true;
    }
};

inline Value parse(const char* data, size_t len, std::string& err) {
    Value root;
    Parser(data, len).parse(root, err);
    return root;
}

}  // namespace bf6json

#endif  // BF6_JSON_HPP
