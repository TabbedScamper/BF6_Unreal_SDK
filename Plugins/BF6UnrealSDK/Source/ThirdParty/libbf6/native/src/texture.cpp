#include "texture.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

namespace bf6 {
namespace {

constexpr size_t kHdrMin = 56;

template <typename T>
T rd(const std::vector<uint8_t>& d, size_t off)
{
    T v{};
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

// BF6 texture format -> DXGI. The reference implementation's table, not a
// second opinion: a second opinion is how two decoders drift apart.
//
// TWO DEPARTURES, both measured, both the same class of bug - two DXGI codes
// differing only in interpretation, collapsed in a lookup, silent.
//
// BC6H has an unsigned flavour (95) and a signed one (96), and the reference
// maps BOTH game codes 64 and 65 to 96. Reading an unsigned BC6H as signed
// makes the leading magnitude bit a sign: the same blocks of a sky panorama
// come out min -65379 / mean 8328 / 14% negative as signed, against min 0.129 /
// mean 0.69 / 0% negative as unsigned. A sky has no negative radiance.
//
// And game codes 60 and 61 both mapped to DXGI 78 (BC3_UNORM_SRGB), while the
// BC7 pair below them is correctly split into linear and sRGB. Decoded
// directly, a flat tangent-space normal under code 60 reads R and G mean 0.502,
// which is LINEAR; an sRGB-encoded flat normal would store about 0.735. So 60
// is the BC3_UNORM twin, and 87 textures on one map were being flagged sRGB
// when they are linear.
//
// 65 stays on 96: nothing measured uses it, and if a signed BC6H exists in this
// game that is its code.
const std::map<int, int> kFmtToDxgi = {
    {6, 61}, {18, 28}, {27, 24}, {29, 28}, {31, 56}, {40, 10}, {54, 71}, {55, 72}, {56, 74},
    {57, 75}, {58, 75}, {59, 77}, {60, 77}, {61, 78}, {62, 80}, {63, 83},
    {64, 95}, {65, 96}, {66, 98}, {67, 99},
};

}  // namespace

int Texture::block_bytes(int dxgi)
{
    // DELIBERATELY NOT a copy of the reference's bpp1(), which answers 8 for
    // BC2 and BC3 as well as BC1 and BC4. Those are 16-byte formats and 8 is
    // wrong for them. It has never mattered there because the only callers are
    // reachable when the header-exact path fails, and that path matched 4,000
    // of 4,000 sampled textures. Copying a known-wrong constant to preserve
    // bug-for-bug parity would be the wrong trade.
    return (dxgi == 71 || dxgi == 72 || dxgi == 80) ? 8 : 16;
}

bool Texture::is_srgb(int dxgi)
{
    return dxgi == 72 || dxgi == 75 || dxgi == 78 || dxgi == 99;
}

int Texture::level_size(int w, int h, int dxgi)
{
    // The RenderFormat enum and DXGI join are already recovered in
    // formats/TEXTURES.md. These are ordinary texels, not BC blocks. Treating
    // them as 4x4 blocks happened to give R8 the right byte count by accident
    // (16 bytes per fake block), but under-counted RGBA8/RGBA16 and the R16
    // crater utility needed by the terrain evaluator.
    int bytes_per_pixel = 0;
    switch (dxgi)
    {
    case 61: bytes_per_pixel = 1; break;  // R8_UNORM
    case 56: bytes_per_pixel = 2; break;  // R16_UNORM
    case 28: bytes_per_pixel = 4; break;  // R8G8B8A8_UNORM
    case 24: bytes_per_pixel = 4; break;  // R10G10B10A2_UNORM
    case 10: bytes_per_pixel = 8; break;  // R16G16B16A16_FLOAT
    default: break;
    }
    if (bytes_per_pixel) return std::max(1, w) * std::max(1, h) * bytes_per_pixel;
    return std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * block_bytes(dxgi);
}

int Texture::chain_size(int w, int h, int dxgi)
{
    int t = 0, cw = w, ch = h;
    for (;;)
    {
        t += level_size(cw, ch, dxgi);
        if (cw <= 1 && ch <= 1) break;
        cw = std::max(1, cw / 2);
        ch = std::max(1, ch / 2);
    }
    return t;
}

bool Texture::read_header(const std::vector<uint8_t>& d, TextureHeader& out)
{
    if (d.size() < kHdrMin) return false;
    out = TextureHeader();
    out.format = rd<int32_t>(d, 12);
    auto it = kFmtToDxgi.find(out.format);
    out.dxgi = it == kFmtToDxgi.end() ? 0 : it->second;

    // WIDTH IS AT 22 AND HEIGHT AT 24, which is the opposite of what the
    // Python reference's variable names have always said. Its output was still
    // right, by two mistakes cancelling: it read w and h the wrong way round
    // and then wrote them into the DDS header's dwHeight and dwWidth slots in
    // that same order, un-swapping them. A port that follows the NAMES comes
    // out transposed on every non-square texture and identical on the square
    // ones. Settled by pixels, not by reading code: a BCn texture decoded at
    // transposed dimensions is not a rotated image, because the 4x4 blocks are
    // then read in the wrong order and the result is spatially incoherent.
    // Sixty real non-square textures decoded both ways voted 60-0 for this.
    out.width  = rd<uint16_t>(d, 22);
    out.height = rd<uint16_t>(d, 24);
    out.slices = std::max<int32_t>(1, rd<uint16_t>(d, 28));
    out.mipcount = d[30];
    out.streamflag = d[21];

    if (d.size() >= 120 && out.mipcount > 0)
        for (int i = 0; i < std::min(15, out.mipcount); i++)
            out.mip_sizes.push_back(rd<uint32_t>(d, 56 + 4 * (size_t)i));

    char t[33];
    auto hex16 = [&](size_t off) -> std::string
    {
        if (off + 16 > d.size()) return std::string();
        for (int i = 0; i < 16; i++) std::snprintf(t + i * 2, 3, "%02x", d[off + (size_t)i]);
        return std::string(t, 32);
    };
    out.embedded = hex16(40);
    out.streamed = d.size() >= 180 ? hex16(164) : std::string();
    return true;
}

const char* Texture::which_chunk(const TextureHeader& h)
{
    if ((h.streamflag & 0x10) != 0 && !h.streamed.empty()) return "streamed";
    return "embedded";
}

bool Texture::dims_for(const TextureHeader& h, const std::vector<uint8_t>& pix,
                       int max_dim, TextureImage& out)
{
    const int w = h.width, ht = h.height, dxgi = h.dxgi;
    const int slices = h.slices, mipcount = h.mipcount;

    out.dxgi = dxgi;
    out.srgb = is_srgb(dxgi);
    out.slices = slices;

    // The streamed chunk is mip0 ON ITS OWN, so it matches no suffix-sum of the
    // mip table. Stated up front rather than left to the walk below, which does
    // reach the same answer but by coincidence of ordering.
    if (!h.mip_sizes.empty() && pix.size() == (size_t)h.mip_sizes[0] * (size_t)slices)
    {
        // The streamed chunk is mip0 ON ITS OWN, so this really is one level.
        // A consumer that needs a chain for this texture has to build one.
        out.width = w; out.height = ht; out.blocks = pix; out.mip_count = 1;
        return true;
    }

    if (mipcount > 0 && (int)h.mip_sizes.size() == mipcount)
    {
        bool all_set = true;
        for (uint32_t s : h.mip_sizes) if (s == 0) { all_set = false; break; }
        if (all_set)
        {
            for (int fm = 0; fm < mipcount; fm++)
            {
                size_t tail = 0;
                for (int i = fm; i < mipcount; i++) tail += h.mip_sizes[(size_t)i];
                if (tail * (size_t)slices != pix.size()) continue;

                // Walk down until the level fits the cap. Never past the last:
                // a texture whose smallest mip still exceeds the cap gets that
                // smallest mip, which is the best this chunk can offer.
                int lvl = fm;
                if (max_dim > 0)
                    while (lvl + 1 < mipcount &&
                           (std::max(1, w >> lvl) > max_dim || std::max(1, ht >> lvl) > max_dim))
                        lvl++;

                size_t off = 0;
                for (int i = fm; i < lvl; i++) off += (size_t)h.mip_sizes[(size_t)i] * (size_t)slices;
                if (off >= pix.size()) return false;

                out.width  = std::max(1, w  >> lvl);
                out.height = std::max(1, ht >> lvl);
                // EVERYTHING FROM HERE DOWN, not just this level. The chain is
                // contiguous and largest-first, so the tail is the mip chain.
                out.blocks.assign(pix.begin() + (ptrdiff_t)off, pix.end());
                out.mip_count = mipcount - lvl;
                return true;
            }
        }
    }

    // THE FALLBACKS BELOW ASSUME A BLOCK FORMAT and are wrong for the handful
    // of textures that are not one. Kept deliberately in step with the
    // reference: changing it here would put the two readers out of step on
    // exactly the cases neither handles well. The header-exact path above
    // matched 4,000 of 4,000 sampled textures, so this is near-dead code.
    const int cw[4] = { w, w / 2, w / 4, w * 2 };
    const int chh[4] = { ht, ht / 2, ht / 4, ht * 2 };
    for (int i = 0; i < 4; i++)
        if (cw[i] > 0 && chh[i] > 0 &&
            (size_t)chain_size(cw[i], chh[i], dxgi) * (size_t)slices == pix.size())
        { out.width = cw[i]; out.height = chh[i]; out.blocks = pix; return true; }
    for (int i = 0; i < 4; i++)
        if (cw[i] > 0 && chh[i] > 0 &&
            (size_t)level_size(cw[i], chh[i], dxgi) * (size_t)slices == pix.size())
        { out.width = cw[i]; out.height = chh[i]; out.blocks = pix; return true; }

    int bw = 0, bh = 0;
    for (int i = 0; i < 4; i++)
        if (cw[i] > 0 && chh[i] > 0 &&
            (size_t)level_size(cw[i], chh[i], dxgi) * (size_t)slices <= pix.size())
            if (cw[i] * chh[i] > bw * bh) { bw = cw[i]; bh = chh[i]; }
    if (bw > 0) { out.width = bw; out.height = bh; out.blocks = pix; return true; }

    out.width = (h.streamflag & 0x10) ? w / 2 : w;
    out.height = (h.streamflag & 0x10) ? ht / 2 : ht;
    out.blocks = pix;
    return out.width > 0 && out.height > 0;
}

bool Texture::decode(const std::vector<uint8_t>& res, const FetchChunk& fetch,
                     TextureImage& out, int max_dim, std::string& err)
{
    err.clear();
    TextureHeader h;
    if (!read_header(res, h)) { err = "too short to be a TextureResource"; return false; }
    if (h.dxgi == 0)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "texture format %d has no DXGI mapping", h.format);
        err = m;
        return false;
    }

    const std::string want = std::strcmp(which_chunk(h), "streamed") == 0 ? h.streamed : h.embedded;
    if (want.empty()) { err = "no chunk guid in the header"; return false; }

    // READING THE WHOLE CHUNK MATTERS MORE HERE THAN ANYWHERE ELSE. A
    // mip-chained texture read through its bundle segment comes back as the
    // resident sub-range only, measured 4x and 64x short, which is exactly the
    // shape of a texture that decodes to a plausible but WRONG size.
    std::vector<uint8_t> pix = fetch(want);
    if (pix.empty() && want != h.embedded) pix = fetch(h.embedded);
    if (pix.empty()) { err = "neither chunk could be read"; return false; }

    if (!dims_for(h, pix, max_dim, out)) { err = "could not size the texture"; return false; }

    // Refuse rather than render bytes as if there were more of them: a short
    // buffer drawn at full dimensions is garbage that looks like a texture.
    // Only the TOP level has to be present in full; the tail is a bonus and a
    // short one simply means fewer mips.
    const size_t need = (size_t)level_size(out.width, out.height, out.dxgi) * (size_t)out.slices;
    if (out.blocks.size() < need)
    {
        char m[160];
        std::snprintf(m, sizeof(m), "%dx%d needs %zu bytes, chunk gave %zu",
                      out.width, out.height, need, out.blocks.size());
        err = m;
        return false;
    }
    return true;
}

bool Texture::decode_capped(const std::vector<uint8_t>& res, const FetchChunk& fetch,
                            TextureImage& out, int max_dim, std::string& err)
{
    err.clear();
    TextureHeader h;
    if (!read_header(res, h)) { err = "too short to be a TextureResource"; return false; }
    if (h.dxgi == 0)
    {
        char m[96];
        std::snprintf(m, sizeof(m), "texture format %d has no DXGI mapping", h.format);
        err = m;
        return false;
    }

    const bool streamedMip0 = (h.streamflag & 0x10) != 0 && !h.streamed.empty();
    const bool wantsTail = max_dim > 0 && streamedMip0 && !h.embedded.empty() &&
                           (h.width > max_dim || h.height > max_dim);
    const std::string want = wantsTail ? h.embedded :
        (std::strcmp(which_chunk(h), "streamed") == 0 ? h.streamed : h.embedded);
    if (want.empty()) { err = "no chunk guid in the header"; return false; }
    std::vector<uint8_t> pix = fetch(want);
    if (pix.empty() && want != h.embedded) pix = fetch(h.embedded);
    if (pix.empty()) { err = "neither chunk could be read"; return false; }
    if (!dims_for(h, pix, max_dim, out)) { err = "could not size the texture"; return false; }
    const size_t need = (size_t)level_size(out.width, out.height, out.dxgi) * (size_t)out.slices;
    if (out.blocks.size() < need) { err = "chosen mip payload is short"; return false; }
    return true;
}

}  // namespace bf6
