#ifndef LIBBF6_STDIO_COMPAT_H
#define LIBBF6_STDIO_COMPAT_H

#include <cstdio>

namespace bf6 {

/* MSVC deprecates std::fopen even for read-only binary access. Keep the
 * portability branch in one place instead of suppressing C4996 globally; a
 * warning in format-reading code should remain actionable. */
inline FILE* fopen_binary_read(const char* path)
{
    if (!path || !*path) return nullptr;
#if defined(_MSC_VER)
    FILE* file = nullptr;
    return fopen_s(&file, path, "rb") == 0 ? file : nullptr;
#else
    return std::fopen(path, "rb");
#endif
}

} // namespace bf6

#endif
