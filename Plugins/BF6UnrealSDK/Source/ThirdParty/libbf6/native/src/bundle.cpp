#include "bundle.h"

#include <cstdio>

namespace bf6 {

static const uint32_t SALT           = 0x7065636E;  // "pecn"
static const uint32_t MAGIC_STANDARD = 0xED1CEDB8;
static const uint32_t MAGIC_KELVIN   = 0xC3889333;
static const uint32_t MAGIC_ENCRYPTED= 0xC3E5D5C3;
static const uint8_t  F_FNVFILE      = 0x80;

static uint32_t be32(const uint8_t* d, size_t at) {
    return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) |
           (uint32_t(d[at + 2]) << 8) | uint32_t(d[at + 3]);
}
static uint16_t be16(const uint8_t* d, size_t at) {
    return (uint16_t)((uint32_t(d[at]) << 8) | uint32_t(d[at + 1]));
}
static uint32_t le32(const uint8_t* d, size_t at) {
    return uint32_t(d[at]) | (uint32_t(d[at + 1]) << 8) |
           (uint32_t(d[at + 2]) << 16) | (uint32_t(d[at + 3]) << 24);
}

std::vector<CasLoc> read_segments(const uint8_t* body, size_t body_len,
                                  size_t blob_off, std::string& err) {
    std::vector<CasLoc> segs;
    if (blob_off + 32 > body_len) { err = "segments blob past the toc body"; return segs; }
    uint32_t flag_off  = be32(body, blob_off + 8);
    uint32_t flag_size = be32(body, blob_off + 12);
    uint32_t data_off  = be32(body, blob_off + 16);
    size_t pos = blob_off + 32;

    // DataCount is conditional (5.5): implicit when data begins right after the
    // 32-byte header, else a 9th u32 carries it.
    uint32_t count;
    if (blob_off + data_off == pos) {
        count = flag_size;
    } else {
        count = be32(body, pos);
        pos += 4;
    }

    size_t p = blob_off + data_off;
    int64_t last_id = -1;
    int last_cas = -1;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t fl = (i < flag_size) ? body[blob_off + flag_off + i] : 0;
        if (fl != 0) {
            if (fl & F_FNVFILE) {
                last_id  = be32(body, p + 2);
                last_cas = be16(body, p + 6);
                p += 8;
            } else {
                last_id  = body[p + 2];
                last_cas = body[p + 3];
                p += 4;
            }
        } else if (last_id < 0) {
            err = "segment reuses a CasFileId before one was set";
            return std::vector<CasLoc>();
        }
        CasLoc s;
        s.chunk_id = (uint32_t)last_id;
        s.cas_ix   = (uint16_t)last_cas;
        s.off      = be32(body, p);
        s.size     = be32(body, p + 4);
        p += 8;
        segs.push_back(s);
    }
    return segs;
}

// Payload uses a per-blob endianness decided by the magic. These read helpers
// close over it.
namespace {
struct Reader {
    const uint8_t* d;
    size_t len;
    bool little;
    uint32_t u32(size_t at) const {
        return little ? le32(d, at) : be32(d, at);
    }
    int32_t s32(size_t at) const { return (int32_t)u32(at); }
    uint64_t u64(size_t at) const {
        if (little) {
            uint64_t v = 0;
            for (int k = 0; k < 8; k++) v |= (uint64_t)d[at + k] << (8 * k);
            return v;
        }
        return ((uint64_t)be32(d, at) << 32) | (uint64_t)be32(d, at + 4);
    }
};
}  // namespace

bool Payload::parse(const uint8_t* d, size_t len, std::string& err) {
    if (len < 32) { err = "payload too short"; return false; }

    // 6.1: XOR-salted magic; endianness undeclared. Try big-endian, else little.
    uint32_t magic;
    bool little;
    uint32_t be = be32(d, 4) ^ SALT;
    if (be == MAGIC_STANDARD || be == MAGIC_KELVIN || be == MAGIC_ENCRYPTED) {
        magic = be; little = false;
    } else {
        uint32_t le = le32(d, 4) ^ SALT;
        if (le != MAGIC_STANDARD && le != MAGIC_KELVIN && le != MAGIC_ENCRYPTED) {
            err = "bundle magic is not a known value";
            return false;
        }
        magic = le; little = true;
    }
    if (magic == MAGIC_ENCRYPTED) { err = "encrypted bundle; no decryption defined"; return false; }

    Reader r{d, len, little};
    int32_t total_count = r.s32(8);
    int32_t ebx_count   = r.s32(12);
    int32_t res_count   = r.s32(16);
    int32_t chunk_count = r.s32(20);
    // Every declared offset is relative to byte 4 (just after TotalSize).
    size_t strings_off  = (size_t)(r.s32(24) + 4);

    auto str_at = [&](uint32_t name_off) -> std::string {
        size_t at = strings_off + name_off;
        if (at >= len) return std::string();
        size_t end = at;
        while (end < len && d[end] != 0) end++;
        return std::string((const char*)d + at, end - at);
    };

    // 6.1: Standard's header is 0x24 and carries TotalCount x 20-byte SHA-1s.
    size_t pos = (magic == MAGIC_STANDARD) ? 0x24 : 0x20;
    if (magic == MAGIC_STANDARD) pos += (size_t)20 * total_count;

    for (int i = 0; i < ebx_count; i++) {
        ebx.emplace_back(str_at(r.u32(pos)), r.u32(pos + 4));
        pos += 8;
    }

    // 6.3 RES: four parallel arrays back to back.
    size_t base    = pos;
    size_t n       = (size_t)res_count;
    size_t types_at= base + n * 8;
    size_t meta_at = types_at + n * 4;
    size_t rid_at  = meta_at + n * 0x10;
    for (size_t i = 0; i < n; i++) {
        PayloadRes pr;
        pr.name = str_at(r.u32(base + i * 8));
        pr.size = r.u32(base + i * 8 + 4);
        pr.type = r.u32(types_at + i * 4);
        pr.rid  = r.u64(rid_at + i * 8);
        res.push_back(std::move(pr));
    }
    pos = rid_at + n * 8;

    static const char* H = "0123456789abcdef";
    for (int i = 0; i < chunk_count; i++) {
        std::string id;
        id.reserve(32);
        for (int k = 0; k < 16; k++) { id += H[d[pos + k] >> 4]; id += H[d[pos + k] & 0xF]; }
        chunk_id.push_back(std::move(id));
        pos += 24;
    }
    return true;
}

}  // namespace bf6
