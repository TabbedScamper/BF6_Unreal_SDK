#include "toc.h"

#include <cstdio>

namespace bf6 {

static const uint32_t INLINE    = 1u << 30;
static const uint32_t IN_MEMORY = 1u << 31;
static const uint32_t SIZE_MASK = 0x3FFFFFFF;
static const uint8_t  F_FILE    = 0x01;
static const uint8_t  F_FNVFILE = 0x80;

static uint32_t be32(const uint8_t* d, size_t at) {
    return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) |
           (uint32_t(d[at + 2]) << 8) | uint32_t(d[at + 3]);
}
static uint16_t be16(const uint8_t* d, size_t at) {
    return (uint16_t)((uint32_t(d[at]) << 8) | uint32_t(d[at + 1]));
}
static int64_t be64(const uint8_t* d, size_t at) {
    return ((int64_t)be32(d, at) << 32) | (int64_t)be32(d, at + 4);
}

// A code point -> UTF-8, matching GDScript's char(sym). Names are ASCII in
// practice, but the general case costs nothing.
static void utf8_append(std::string& s, int cp) {
    if (cp < 0x80) { s += (char)cp; }
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

bool Toc::parse(const uint8_t* raw, size_t len, std::string& err) {
    // Strip the DICE header (magic 0x00CED100 / 0x01CED100) if present.
    if (len >= 4) {
        uint32_t magic = uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) |
                         (uint32_t(raw[2]) << 16) | (uint32_t(raw[3]) << 24);
        if (magic == 0x00CED100u || magic == 0x01CED100u) { raw += 556; len -= 556; }
    }
    body_.assign(raw, raw + len);
    if (body_.size() < 48) { err = "too short to hold an SbTocHeader"; return false; }
    const uint8_t* b = body_.data();

    bundle_value_off_ = be32(b, 4);
    bundle_count_     = be32(b, 8);
    chunk_value_off_  = be32(b, 16);
    chunk_count_      = be32(b, 20);
    uint32_t unk1     = be32(b, 24);
    uint32_t unk2     = be32(b, 28);
    names_off_        = be32(b, 32);
    data_off_         = be32(b, 36);
    uint32_t flags    = be32(b, 44);

    compressed_names_ = (flags & 0x04) != 0;

    // UnkOffset1/2 must equal DataOffset (5.1) - reject the unseen otherwise.
    if (unk1 != data_off_ || unk2 != data_off_) {
        char m[96];
        std::snprintf(m, sizeof(m),
            "UnkOffset1/2 (0x%X/0x%X) differ from DataOffset (0x%X)",
            unk1, unk2, data_off_);
        err = m;
        return false;
    }

    if (compressed_names_) {
        if (body_.size() < 60) { err = "CompressedNames set but header is short"; return false; }
        name_count_ = be32(b, 0x30);
        huff_nodes_ = be32(b, 0x34);
        huff_off_   = be32(b, 0x38);
        if (!huffman_names(err)) return false;
    }

    read_bundles(err);
    if (!err.empty()) return false;
    read_chunks();
    return true;
}

// 5.2: i32 BE consumed in pairs; the last pair built is the root. Bits are read
// LSB-first within consecutive 32-bit big-endian words.
bool Toc::huffman_names(std::string& err) {
    // A TOC may set CompressedNames and carry no names (HRES detail packages).
    if (huff_nodes_ == 0 || bundle_count_ == 0) return true;
    if (huff_nodes_ % 2 != 0) {
        char m[48];
        std::snprintf(m, sizeof(m), "HuffmanTreeNodeCount %u is odd", huff_nodes_);
        err = m;
        return false;
    }
    const uint8_t* b = body_.data();
    std::vector<std::pair<int32_t, int32_t>> nodes;
    nodes.reserve(huff_nodes_ / 2);
    for (uint32_t i = 0; i < huff_nodes_; i += 2) {
        int32_t a = (int32_t)be32(b, huff_off_ + i * 4);
        int32_t c = (int32_t)be32(b, huff_off_ + (i + 1) * 4);
        nodes.emplace_back(a, c);
    }
    int root = (int)nodes.size() - 1;
    uint64_t total_bits = (uint64_t)name_count_ * 32;
    uint64_t pos = 0;
    for (uint32_t n = 0; n < bundle_count_; n++) {
        if (pos >= total_bits) break;
        uint32_t start = (uint32_t)pos;
        std::string s;
        while (true) {
            int node = root;
            int sym = 0;
            while (true) {
                uint32_t w = be32(b, names_off_ + (size_t)(pos >> 5) * 4);
                int bit = (w >> (pos & 31)) & 1;
                pos++;
                int32_t v = bit ? nodes[node].second : nodes[node].first;
                if (v < 0) { sym = -v - 1; break; }
                node = v;
            }
            if (sym == 0) break;               // NUL terminates
            utf8_append(s, sym);
        }
        names_[start] = s;
    }
    return true;
}

std::string Toc::name_at(uint32_t off) const {
    if (compressed_names_) {
        auto it = names_.find(off);
        return it == names_.end() ? std::string() : it->second;
    }
    size_t at = names_off_ + off;
    size_t end = at;
    while (end < body_.size() && body_[end] != 0) end++;
    return std::string((const char*)body_.data() + at, end - at);
}

void Toc::read_bundles(std::string& err) {
    const uint8_t* b = body_.data();
    for (uint32_t i = 0; i < bundle_count_; i++) {
        size_t o = bundle_value_off_ + (size_t)i * 16;
        if (o + 16 > body_.size()) { err = "bundle map runs past the end of the toc"; return; }
        uint32_t name_off = be32(b, o);
        uint32_t size = be32(b, o + 4);
        int64_t  offset = be64(b, o + 8);
        if (size == 0xFFFFFFFF && offset == -1) continue;   // removed sentinel
        TocBundle tb;
        tb.name = name_at(name_off);
        tb.offset = offset;
        tb.size = size & SIZE_MASK;
        tb.inl = (size & INLINE) != 0;
        tb.in_memory = (size & IN_MEMORY) != 0;
        bundles.push_back(std::move(tb));
    }
}

void Toc::read_chunks() {
    const uint8_t* b = body_.data();
    for (uint32_t i = 0; i < chunk_count_; i++) {
        size_t o = chunk_value_off_ + (size_t)i * 20;
        if (o + 20 > body_.size()) break;
        uint32_t value = be32(b, o + 16);
        if (value == 0xFFFFFFFF) continue;                  // removed sentinel
        uint8_t fl = (uint8_t)((value >> 24) & 0xFF);
        if ((fl & (F_FILE | F_FNVFILE)) == 0) continue;     // no CAS record
        // 5.4: guid stored reversed.
        TocChunk tc;
        static const char* H = "0123456789abcdef";
        tc.guid.reserve(32);
        for (int k = 15; k >= 0; k--) {
            uint8_t byte = b[o + k];
            tc.guid += H[byte >> 4]; tc.guid += H[byte & 0xF];
        }
        tc.flags = fl;
        tc.data_pos = value & 0x00FFFFFF;
        chunks.push_back(std::move(tc));
    }
}

CasLoc Toc::chunk_location(const TocChunk& ch) const {
    const uint8_t* b = body_.data();
    // DataPosition is in 4-byte units relative to DataOffset.
    size_t pos = data_off_ + (size_t)ch.data_pos * 4;
    CasLoc loc;
    if ((ch.flags & F_FNVFILE) != 0) {
        loc.chunk_id = be32(b, pos + 2);
        loc.cas_ix   = be16(b, pos + 6);
        pos += 8;
    } else {
        loc.chunk_id = b[pos + 2];
        loc.cas_ix   = b[pos + 3];
        pos += 4;
    }
    loc.off  = be32(b, pos);
    loc.size = be32(b, pos + 4);
    return loc;
}

}  // namespace bf6
