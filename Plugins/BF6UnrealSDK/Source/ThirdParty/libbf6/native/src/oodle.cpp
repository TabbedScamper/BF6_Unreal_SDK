#include "oodle.h"

#include <windows.h>

namespace bf6 {

typedef intptr_t (*OodleLZ_Decompress_t)(
    const void* src, intptr_t srcLen, void* dst, intptr_t dstLen,
    int fuzz, int crc, int verbose,
    void* dstBase, intptr_t dstSize, void* cb, void* cbCtx,
    void* scratch, intptr_t scratchSize, int threadPhase);

static HMODULE g_oodle = nullptr;
static OodleLZ_Decompress_t g_decompress = nullptr;
static std::string g_error;

bool oodle_open(const std::string& game_dir) {
    if (g_decompress) return true;
    std::string dll = game_dir;
    if (!dll.empty() && dll.back() != '\\' && dll.back() != '/') dll += "\\";
    dll += "oo2core_9_win64.dll";
    g_oodle = LoadLibraryA(dll.c_str());
    if (!g_oodle) {
        g_error = "cannot load " + dll +
                  " (it ships with Battlefield 6; this never bundles a copy)";
        return false;
    }
    g_decompress = reinterpret_cast<OodleLZ_Decompress_t>(
        GetProcAddress(g_oodle, "OodleLZ_Decompress"));
    if (!g_decompress) {
        g_error = "OodleLZ_Decompress missing from " + dll;
        FreeLibrary(g_oodle);
        g_oodle = nullptr;
        return false;
    }
    g_error.clear();
    return true;
}

bool oodle_decompress(const uint8_t* src, size_t src_len,
                      uint8_t* dst, size_t dst_len) {
    if (!g_decompress || !src || !dst) return false;
    // Same arguments the Godot reader uses: fuzzSafe=1, checkCRC=0,
    // verbosity=0, threadPhase=3 (Unthreaded). threadPhase MUST be 3 - asking
    // for a threaded phase makes two readers disagree on the same bytes.
    intptr_t got = g_decompress(src, (intptr_t)src_len, dst, (intptr_t)dst_len,
                                1, 0, 0, nullptr, 0, nullptr, nullptr,
                                nullptr, 0, 3);
    return got == (intptr_t)dst_len;
}

const std::string& oodle_error() { return g_error; }

}  // namespace bf6
