#include "cas.h"

#include <map>

#include <cstdio>

#include "oodle.h"
#include "stdio_compat.h"

namespace bf6 {

// The CAS block header, ported field-for-field from bf6_cas.gd:
//   d0 (BE) : flags = d0>>24,        dsize = d0 & 0x00FFFFFF
//   d1 (BE) : codec = d1>>24, guard = (d1>>20)&0xF, csize = d1 & 0x000FFFFF
static const uint8_t  GUARD      = 7;
static const uint8_t  CODEC_NONE = 0x00;
static const int      MAX_BLOCKS = 4096;

static bool is_oodle_codec(uint8_t c) {
    return c == 0x11 || c == 0x15 || c == 0x17 || c == 0x19;
}

static uint32_t be32(const uint8_t* b, size_t at) {
    return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) |
           (uint32_t(b[at + 2]) << 8) | uint32_t(b[at + 3]);
}

struct BlockHdr {
    uint8_t  flags;
    uint32_t dsize;
    uint8_t  codec;
    uint8_t  guard;
    uint32_t csize;
};

static BlockHdr block_header(const uint8_t* b, size_t pos) {
    uint32_t d0 = be32(b, pos);
    uint32_t d1 = be32(b, pos + 4);
    BlockHdr h;
    h.flags = uint8_t((d0 >> 24) & 0xFF);
    h.dsize = d0 & 0x00FFFFFF;
    h.codec = uint8_t((d1 >> 24) & 0xFF);
    h.guard = uint8_t((d1 >> 20) & 0x0F);
    h.csize = d1 & 0x000FFFFF;
    return h;
}

// Decode one block's payload of `csize` bytes at buf+at into `out` (appended).
static bool decode_one(const uint8_t* buf, size_t at, uint32_t csize,
                       uint32_t dsize, uint8_t codec,
                       std::vector<uint8_t>& out, std::string& err) {
    if (codec == CODEC_NONE) {
        if (csize != dsize) { err = "uncompressed block size mismatch"; return false; }
        out.insert(out.end(), buf + at, buf + at + csize);
        return true;
    }
    if (!is_oodle_codec(codec)) {
        char m[64];
        std::snprintf(m, sizeof(m), "codec 0x%02X is not present in BF6 data", codec);
        err = m;
        return false;
    }
    size_t base = out.size();
    out.resize(base + dsize);
    if (!oodle_decompress(buf + at, csize, out.data() + base, dsize)) {
        err = "oodle decompress failed / short";
        out.resize(base);
        return false;
    }
    return true;
}

namespace {

// OPEN EACH ARCHIVE ONCE PER THREAD, not once per read.
//
// This used to fopen and fclose around every read, which is fine for a handful
// and ruinous for the partition index: that walks every partition in the mount
// for one 16-byte header, so mp_dumbo alone paid 228,818 file opens. Threading
// the index only took it from 19 seconds to 14 precisely because the cost was
// syscalls rather than work.
//
// THREAD_LOCAL RATHER THAN SHARED, deliberately. A FILE* carries one position,
// so a shared handle would need a lock around the seek and the read together,
// which serialises exactly the thing being parallelised. A handle per thread per
// archive costs a few dozen opens in total and needs no lock at all.
//
// Handles close when the thread ends, which for the index's short-lived workers
// is immediately after.
struct CasHandles
{
    std::map<std::string, FILE*> open;
    ~CasHandles()
    {
        for (auto& kv : open) if (kv.second) std::fclose(kv.second);
    }
};

FILE* cas_handle(const std::string& path)
{
    static thread_local CasHandles cache;
    auto it = cache.open.find(path);
    if (it != cache.open.end()) return it->second;
    FILE* f = fopen_binary_read(path.c_str());
    cache.open.emplace(path, f);   // a null is cached too: a missing archive
    return f;                      // should not be reopened once per read either
}

}  // namespace

std::vector<uint8_t> cas_read(const std::string& path, int64_t offset,
                              int64_t size, bool allow_raw, std::string& err) {
    std::vector<uint8_t> empty;
    FILE* f = cas_handle(path);
    if (!f) { err = "cannot open " + path; return empty; }
    _fseeki64(f, offset, SEEK_SET);
    std::vector<uint8_t> buf((size_t)size);
    size_t got = std::fread(buf.data(), 1, (size_t)size, f);
    buf.resize(got);
    if (got < 8) { err = "CAS reference shorter than a block header"; return empty; }

    BlockHdr h = block_header(buf.data(), 0);
    if (h.guard != GUARD) {
        if (allow_raw) return buf;
        err = "guard nibble != 7";
        return empty;
    }

    std::vector<uint8_t> out;
    // Single block: the compressed size accounts for the whole reference.
    if (h.csize == (uint32_t)(size - 8)) {
        if (!decode_one(buf.data(), 8, h.csize, h.dsize, h.codec, out, err))
            return empty;
        return out;
    }

    // Multi-block span: walk blocks until the guard breaks or the buffer ends.
    size_t pos = 0;
    int nparts = 0;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (pos + 8 > buf.size()) break;
        BlockHdr h2 = block_header(buf.data(), pos);
        if (h2.guard != GUARD || pos + 8 + h2.csize > buf.size()) break;
        if (!decode_one(buf.data(), pos + 8, h2.csize, h2.dsize, h2.codec, out, err))
            return empty;
        nparts++;
        pos += 8 + h2.csize;
    }
    if (nparts < 2) { err = "not a multi-block span"; return empty; }
    return out;
}

}  // namespace bf6
