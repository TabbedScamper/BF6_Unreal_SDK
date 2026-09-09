/* SPS SAMPLE DECODING.
 *
 * `newwaveresource-sbr-datasets-link-audio-chunks` closed metadata and codec
 * identification and said plainly that it did NOT close sample decoding. This
 * is that half, for the two codecs that carry the shipped level audio.
 *
 * HEADER (big-endian, at the chunk start)
 *     +0x00 u32  block type high byte, header size low 24 bits
 *     +0x04 u8   codec   0x12 Pcm16Big, 0x14 Xas1, 0x16 EaLayer32Pcm
 *     +0x05 u8   (channels - 1) << 2
 *     +0x06 u16  sample rate
 *     +0x08 u32  sample count, low 24 bits
 *
 * PCM16BIG (0x12) is big-endian interleaved s16. Its size closes exactly:
 * samples * channels * 2 + header == payload, on every block measured.
 *
 * XAS1 (0x14) block GEOMETRY is proven by size arithmetic: a payload is
 * ceil(samples/128) * 76 * channels plus a small preamble, on 39 of 40 blocks
 * measured. So a block is 76 bytes carrying 128 samples per channel, which
 * divides into four 19-byte groups of 32 samples.
 *
 * THE GROUP INTERIOR IS NOW DECODED, after a first guess at it was tested and
 * REJECTED by its own continuity check (step/RMS 1.081, i.e. noise). The real
 * layout came from vgmstream's ea_xas_decoder.c used as a specification; see
 * sps_decode_xas below. */

static const int kXasBlockBytes   = 76;
static const int kXasBlockSamples = 128;
static const int kXasGroupBytes   = 19;
static const int kXasGroupSamples = 32;

static uint32_t sps_be32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t sps_be16(const uint8_t* p){ return (uint16_t)(((uint32_t)p[0]<<8)|p[1]); }
static int16_t  sps_s16be(const uint8_t* p){ return (int16_t)sps_be16(p); }

struct SpsInfo { uint8_t codec; int channels; int rate; uint32_t samples; uint32_t header; };

static bool sps_parse(const uint8_t* b, size_t n, SpsInfo& o)
{
    if (!b || n < 16) return false;
    o.header   = sps_be32(b) & 0x00FFFFFFu;
    o.codec    = b[4];
    o.channels = (b[5] >> 2) + 1;
    o.rate     = sps_be16(b + 6);
    o.samples  = sps_be32(b + 8) & 0x00FFFFFFu;
    if (o.codec != 0x12 && o.codec != 0x14 && o.codec != 0x16) return false;
    if (o.channels < 1 || o.channels > 8) return false;
    if (o.rate < 8000 || o.rate > 48000) return false;
    if (o.samples == 0 || o.samples > 40000000u) return false;
    if (o.header < 8 || o.header > n) return false;
    return true;
}

/* The payload offset the samples start at. The header field is not always the
 * whole preamble, so the size identity is what pins it: for PCM the body is
 * exactly samples*channels*2, so the offset is payload - that. */
static size_t sps_pcm_offset(size_t payload, const SpsInfo& s)
{
    const size_t body = (size_t)s.samples * (size_t)s.channels * 2u;
    return (payload >= body) ? payload - body : (size_t)s.header;
}

static size_t sps_xas_blocks(const SpsInfo& s)
{
    return ((size_t)s.samples + kXasBlockSamples - 1) / kXasBlockSamples;
}

static size_t sps_xas_offset(size_t payload, const SpsInfo& s)
{
    const size_t body = sps_xas_blocks(s) * (size_t)kXasBlockBytes * (size_t)s.channels;
    return (payload >= body) ? payload - body : (size_t)s.header;
}

/* PCM16 big-endian -> interleaved host int16. Exact, no approximation. */
static bool sps_decode_pcm16(const uint8_t* b, size_t n, const SpsInfo& s,
                             std::vector<int16_t>& out)
{
    const size_t off = sps_pcm_offset(n, s);
    const size_t want = (size_t)s.samples * (size_t)s.channels;
    if (off + want * 2 > n) return false;
    out.resize(want);
    for (size_t i = 0; i < want; i++) out[i] = sps_s16be(b + off + i * 2);
    return true;
}

/* XAS1 -> interleaved host int16.
 *
 * Returns the two VERBATIM samples per group as well, because those are the
 * check: they are stored, not predicted, so a reader on the right stride
 * reproduces them and a reader on the wrong one cannot. */
/* XAS1 decode. The layout is NOT what this file previously guessed.
 *
 * A 0x4C block is 4 group HEADERS CONTIGUOUS AT THE FRONT (0x10 bytes), then
 * 15 rows of 4 bytes in which the nibbles are interleaved VERTICALLY across the
 * four groups: byte = frame[0x10 + row*4 + group]. Headers are LITTLE-endian.
 *
 * That is why the earlier hypothesis failed its own continuity test at
 * step/RMS 1.081: it assumed sequential [header][data] groups read big-endian,
 * so it was reading the wrong bytes in the wrong order. The block GEOMETRY
 * derived here from size arithmetic - 76 bytes, 128 samples, four 32-sample
 * groups - was right; the interior was not.
 *
 *     group_header = u32 LE at frame + group*4
 *       coef index = header & 0x0F           (CD-XA K0/K1 pairs)
 *       hist2      = (int16)(header & 0xFFF0)
 *       hist1      = (int16)((header >> 16) & 0xFFF0)
 *       shift      = (header >> 16) & 0x0F
 *     the two hist values ARE the group's first two samples
 *     then 30 nibbles, high nibble first:
 *       s = ((int16)(nib << 12) >> shift) + hist1*coef1 + hist2*coef2, clamped
 *
 * Algorithm from vgmstream's ea_xas_decoder.c, used as the specification. */

static const float kXaCoefs[16][2] = {
    { 0.0f, 0.0f }, { 0.9375f, 0.0f }, { 1.796875f, -0.8125f }, { 1.53125f, -0.859375f },
    { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f },
    { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f },
    { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f },
};

static int16_t sps_clamp16(float v)
{
    if (v >  32767.0f) return  32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

static uint32_t sps_le32(const uint8_t* p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool sps_decode_xas(const uint8_t* b, size_t n, const SpsInfo& s,
                           std::vector<int16_t>& out)
{
    const size_t blocks = sps_xas_blocks(s);
    const size_t off = sps_xas_offset(n, s);
    const size_t body = blocks * (size_t)kXasBlockBytes * (size_t)s.channels;
    if (off + body > n) return false;

    out.assign((size_t)s.samples * (size_t)s.channels, 0);
    for (size_t blk = 0; blk < blocks; blk++) {
        for (int ch = 0; ch < s.channels; ch++) {
            const uint8_t* f = b + off + (blk * (size_t)s.channels + (size_t)ch) * kXasBlockBytes;
            size_t si = blk * (size_t)kXasBlockSamples;   /* sample index within channel */
            for (int g = 0; g < 4; g++) {
                const uint32_t h = sps_le32(f + g * 4);
                const float c1 = kXaCoefs[h & 0x0F][0];
                const float c2 = kXaCoefs[h & 0x0F][1];
                int16_t hist2 = (int16_t)(h & 0xFFF0);
                int16_t hist1 = (int16_t)((h >> 16) & 0xFFF0);
                const int shift = (int)((h >> 16) & 0x0F);

                for (int k = 0; k < kXasGroupSamples; k++) {
                    int16_t smp;
                    if (k == 0)      smp = hist2;
                    else if (k == 1) smp = hist1;
                    else {
                        const int row = (k - 2) >> 1;
                        const uint8_t nb = f[0x10 + row * 4 + g];
                        const int nib = ((k - 2) & 1) ? (nb & 0x0F) : ((nb >> 4) & 0x0F);
                        const float v = (float)((int16_t)(nib << 12) >> shift)
                                      + (float)hist1 * c1 + (float)hist2 * c2;
                        smp = sps_clamp16(v);
                        hist2 = hist1; hist1 = smp;
                    }
                    const size_t idx = si + (size_t)k;
                    if (idx < s.samples) out[idx * (size_t)s.channels + (size_t)ch] = smp;
                }
                si += (size_t)kXasGroupSamples;
            }
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * EA LAYER3 (0x16) CONTAINER, corrected.
 *
 * An earlier pass here derived the outer chain by search and described it as
 * [u8 tag][u8][u16 size]. The bytes were right and the READING was wrong: it is
 * the SPS block chain, `u32` with the TYPE in the high byte and the SIZE in the
 * low 24 bits - exactly the formula the SBR finding already documents for the
 * chunk header. Same bytes, correct interpretation:
 *
 *     0x48 'H'  header block   (the SPS header itself)
 *     0x44 'D'  data block     u8 type + u24 size + u32 SAMPLE COUNT, then frames
 *     0x45 'E'  end block      exactly one, always last
 *
 * The u32 sample count after the data block header was missed entirely first
 * time; it reads 576 - one MPEG granule - on the first block.
 *
 * INSIDE a 'D' block are EALayer3 v2 frames:
 *     bit 0     extended flag
 *     bit 1     stereo flag
 *     bits 2-3  reserved
 *     bits 4-15 FRAME SIZE, 12 bits, whole frame including this header
 *   if extended: 2 offset_mode, 10 offset_samples, 10 pcm_samples, 10 common_size
 *
 * Verified: inner frames tile every data block EXACTLY, 1,073 of 1,073, giving
 * 16,063 frames at a mean of 573.8 samples each - one MPEG granule (576).
 *
 * Layouts from vgmstream's mpeg_custom_utils_ealayer3.c, used as the
 * specification. Sample output still requires rebuilding standard MPEG frames
 * from the granule data, which this does not yet do. */

struct Eal3Frame { uint8_t tag; uint32_t size; uint32_t samples; size_t offset; };

/* Walks the SPS block chain. False unless the blocks tile the payload exactly
 * and the last one is the 0x45 terminator. */
static bool sps_eal3_frames(const uint8_t* b, size_t n, const SpsInfo& s,
                            std::vector<Eal3Frame>& out)
{
    out.clear();
    if (s.codec != 0x16 || s.header >= n) return false;
    size_t pos = s.header;
    while (pos < n) {
        if (pos + 4 > n) return false;
        Eal3Frame f;
        f.tag  = b[pos];
        f.size = ((uint32_t)b[pos+1] << 16) | ((uint32_t)b[pos+2] << 8) | b[pos+3];
        f.offset = pos;
        f.samples = 0;
        if (f.tag == 0x44 && pos + 8 <= n) f.samples = sps_be32(b + pos + 4);
        if (f.size < 4 || pos + f.size > n) return false;
        out.push_back(f);
        pos += f.size;
        if (out.size() > 100000) return false;
    }
    return pos == n && !out.empty() && out.back().tag == 0x45;
}

/* EALayer3 v2 frame header, read at a bit offset inside a 0x44 block body. */
struct Eal3V2Hdr { uint32_t extended, stereo, size; };

static uint32_t sps_bits(const uint8_t* p, size_t bit, int w)
{
    uint32_t v = 0;
    for (int i = 0; i < w; i++) { const size_t q = bit + (size_t)i;
        v = (v << 1) | ((p[q >> 3] >> (7 - (q & 7))) & 1); }
    return v;
}

/* Enumerate the EA frames inside one 0x44 data block. Body starts 8 bytes in:
 * u8 type + u24 size + u32 sample count. */
static bool sps_eal3_block_frames(const uint8_t* b, const Eal3Frame& blk,
                                  std::vector<Eal3V2Hdr>& out)
{
    out.clear();
    if (blk.tag != 0x44 || blk.size < 8) return false;
    size_t pos = blk.offset + 8;
    const size_t end = blk.offset + blk.size;
    while (pos < end) {
        if (pos + 2 > end) return false;
        Eal3V2Hdr h;
        h.extended = sps_bits(b + pos, 0, 1);
        h.stereo   = sps_bits(b + pos, 1, 1);
        h.size     = sps_bits(b + pos, 4, 12);
        if (h.size < 2 || pos + h.size > end) return false;
        out.push_back(h);
        pos += h.size;
        if (out.size() > 5000) return false;
    }
    return pos == end;
}

#include "eal3_rebuild.inc"
