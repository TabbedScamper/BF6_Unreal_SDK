/* libbf6 internal - textures (module 7).
 *
 * Ported from bf6_texture.gd. A TextureResource is a small header plus one or
 * two chunks; the blocks come out still compressed, because every engine worth
 * binding to uploads BCn directly and decompressing to RGBA in order to
 * re-compress on upload is work nobody needs.
 *
 *   header (the .Texture res)   format, w/h, mip count, per-mip byte sizes
 *   guid at +40                 the embedded chunk
 *   guid at +164                the streamed high-resolution chunk
 *
 * WHICH ONE HOLDS MIP0 IS A HEADER BIT, NOT A GUESS. Measured over 4,000
 * texture resources the split is exact:
 *
 *   bit 0x10 of byte 21 SET     embedded = the chain MINUS mip0 (tail mips)
 *                               streamed = mip0, exactly, on its own
 *   bit 0x10 CLEAR              embedded = the whole chain from mip0
 *                               streamed = absent
 *
 * An older decoder could not use that rule and reached for the streamed chunk
 * only when the result came back under 512 px: a size heuristic standing in for
 * a header bit.
 */
#ifndef LIBBF6_TEXTURE_H
#define LIBBF6_TEXTURE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bf6 {

struct TextureHeader {
    int32_t  format = 0;        // the game's own code
    int32_t  dxgi = 0;          // translated
    int32_t  width = 0, height = 0;
    int32_t  slices = 1;
    int32_t  mipcount = 0;
    uint8_t  streamflag = 0;    // bit 0x10: mip0 is NOT embedded
    std::vector<uint32_t> mip_sizes;
    std::string embedded, streamed;   // chunk guids, raw hex
};

struct TextureImage {
    int32_t  width = 0, height = 0;
    int32_t  dxgi = 0;
    bool     srgb = false;
    int32_t  slices = 1;
    // THE WHOLE CHAIN from `width`x`height` down, not one level. A chunk is
    // front-ordered largest-first with no padding, so the remainder after the
    // chosen level is already a valid chain and only needs to be handed over
    // rather than sliced away.
    //
    // It matters most for MASKS: foliage drawn with a hard alpha test against a
    // mask with no mip chain speckles per frame and crawls with the camera -
    // the "lacy, moth-eaten" look - and it is INVARIANT to mask resolution, so
    // capping the size never fixes it and the mask gets blamed instead.
    int32_t  mip_count = 1;
    std::vector<uint8_t> blocks;      // BCn as it lies, ready for upload
};

class Texture {
public:
    using FetchChunk = std::function<std::vector<uint8_t>(const std::string&)>;

    static bool read_header(const std::vector<uint8_t>& res, TextureHeader& out);

    // Which chunk holds mip0. The header says so; nothing is guessed.
    static const char* which_chunk(const TextureHeader& h);

    // max_dim > 0 asks for the largest level in the chosen chunk that fits the
    // cap, rather than that chunk's top level.
    static bool decode(const std::vector<uint8_t>& res, const FetchChunk& fetch,
                       TextureImage& out, int max_dim, std::string& err);

    // Viewer-oriented variant: when mip0 is isolated in the streamed chunk,
    // read the authored embedded tail instead so max_dim can actually select
    // a smaller authored mip. The ordinary decoder deliberately keeps its
    // historical chunk-selection contract.
    static bool decode_capped(const std::vector<uint8_t>& res, const FetchChunk& fetch,
                              TextureImage& out, int max_dim, std::string& err);

    static int  block_bytes(int dxgi);
    static bool is_srgb(int dxgi);

private:
    static bool dims_for(const TextureHeader& h, const std::vector<uint8_t>& pix,
                         int max_dim, TextureImage& out);
    static int  level_size(int w, int h, int dxgi);
    static int  chain_size(int w, int h, int dxgi);
};

}  // namespace bf6

#endif
