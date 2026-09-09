/* libbf6 internal - CAS block reader (module 2, lower half).
 *
 * Ported from bf6_cas.gd. Reads one CAS reference: a run of blocks in a .cas
 * file at a byte offset, each block an 8-byte big-endian header followed by its
 * (usually Oodle-compressed) payload. The mount resolves a resource name to
 * (path, offset, size); this turns that into the decompressed bytes.
 *
 * Not part of the public C ABI - the source/mount layer calls it.
 */
#ifndef LIBBF6_CAS_H
#define LIBBF6_CAS_H

#include <cstdint>
#include <string>
#include <vector>

namespace bf6 {

// Read the reference at `path`+`offset`, `size` bytes on disk, and return the
// decompressed payload. Empty vector on failure with `err` set. `allow_raw`
// returns the bytes verbatim when the block guard is absent (non-block data).
// oodle_open() must have been called for the install first.
std::vector<uint8_t> cas_read(const std::string& path, int64_t offset,
                              int64_t size, bool allow_raw, std::string& err);

}  // namespace bf6
#endif
