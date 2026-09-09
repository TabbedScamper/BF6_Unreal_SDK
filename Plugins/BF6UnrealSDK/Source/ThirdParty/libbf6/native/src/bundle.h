/* libbf6 internal - bundle segments + payload index (module 2, mount).
 *
 * Ported from bf6_bundle.gd. read_segments turns a bundle's blob (inside the toc
 * body) into per-asset CAS locations; Payload parses the metadata blob (segment
 * 0) into the bundle's ebx / res / chunk name index. The mount zips the two.
 */
#ifndef LIBBF6_BUNDLE_H
#define LIBBF6_BUNDLE_H

#include <cstdint>
#include <string>
#include <vector>

#include "toc.h"   // CasLoc

namespace bf6 {

// 5.5: the per-bundle blob -> one CAS location per segment. Segment 0 is the
// metadata payload; segments 1.. are the assets in ebx-then-res-then-chunk order.
std::vector<CasLoc> read_segments(const uint8_t* body, size_t body_len,
                                  size_t blob_off, std::string& err);

struct PayloadRes {
    std::string name;
    uint32_t    size = 0;
    uint32_t    type = 0;
    uint64_t    rid = 0;
};

// 6: a bundle's asset index. Names only (with sizes/type/rid for res); the CAS
// locations come from read_segments, matched positionally.
struct Payload {
    std::vector<std::pair<std::string, uint32_t>> ebx;   // name, size
    std::vector<PayloadRes>                       res;
    std::vector<std::string>                      chunk_id;  // 16-byte LE guid, hex

    bool parse(const uint8_t* d, size_t len, std::string& err);
};

}  // namespace bf6
#endif
