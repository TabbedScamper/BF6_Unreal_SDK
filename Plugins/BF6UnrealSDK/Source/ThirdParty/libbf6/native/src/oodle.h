/* libbf6 internal - Oodle decompression.
 *
 * Module 1 of the port, lifted from native/src/bf6_oodle.cpp with the Godot glue
 * removed. This is NOT part of the public C ABI; the cas module calls it.
 *
 * We never ship Oodle. oo2core_9_win64.dll is installed as part of Battlefield 6
 * and loaded from the user's own install at runtime - the same thin-wrapper
 * arrangement the Godot plugin already uses. Windows only, which BF6 is.
 */
#ifndef LIBBF6_OODLE_H
#define LIBBF6_OODLE_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace bf6 {

// Load the game's oo2core from game_dir. Idempotent; safe to call repeatedly.
bool oodle_open(const std::string& game_dir);

// Decompress exactly dst_len bytes from src into dst. Returns true only on a
// full-length result - a short decode yields false and leaves dst untrusted.
bool oodle_decompress(const uint8_t* src, size_t src_len,
                      uint8_t* dst, size_t dst_len);

// Last failure reason, for surfacing through the ctx error string.
const std::string& oodle_error();

}  // namespace bf6
#endif
