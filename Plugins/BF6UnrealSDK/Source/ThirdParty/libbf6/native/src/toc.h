/* libbf6 internal - SuperBundle TOC parser (module 2, mount).
 *
 * Ported from bf6_toc.gd. A .toc names the bundles and loose chunks of a
 * superbundle. Bundle names may be Huffman-compressed. chunk_location() gives
 * the CAS location of a loose chunk's full data.
 */
#ifndef LIBBF6_TOC_H
#define LIBBF6_TOC_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bf6 {

struct TocBundle {
    std::string name;
    int64_t     offset = 0;
    uint32_t    size = 0;
    bool        inl = false;        // stored inline in the toc
    bool        in_memory = false;
};

struct TocChunk {
    std::string guid;               // 16-byte guid, reversed, hex
    uint8_t     flags = 0;
    uint32_t    data_pos = 0;
};

struct CasLoc {                     // [install_chunk_id, cas_index, off, size]
    uint32_t chunk_id = 0;
    uint16_t cas_ix = 0;
    uint32_t off = 0;
    uint32_t size = 0;
};

class Toc {
public:
    bool parse(const uint8_t* raw, size_t len, std::string& err);

    std::vector<TocBundle> bundles;
    std::vector<TocChunk>  chunks;

    // Full-data CAS location of a loose chunk (5.4).
    CasLoc chunk_location(const TocChunk& ch) const;

    // The stripped TOC body - a bundle's segments blob lives inside it.
    const std::vector<uint8_t>& body() const { return body_; }

private:
    std::vector<uint8_t> body_;
    uint32_t bundle_value_off_ = 0, chunk_value_off_ = 0;
    uint32_t names_off_ = 0, data_off_ = 0;
    uint32_t bundle_count_ = 0, chunk_count_ = 0;
    bool     compressed_names_ = false;
    uint32_t name_count_ = 0, huff_nodes_ = 0, huff_off_ = 0;
    std::map<uint32_t, std::string> names_;   // start bit offset -> string

    bool huffman_names(std::string& err);
    std::string name_at(uint32_t off) const;
    void read_bundles(std::string& err);
    void read_chunks();
};

}  // namespace bf6
#endif
