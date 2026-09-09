/* EA LAYER3 -> standard MPEG frames.
 *
 * An EALayer3 frame holds ONE MPEG Layer III granule with the MPEG header and
 * bit reservoir stripped. To decode it you rebuild a standard frame: synthesise
 * the header, write the side info from the stored fields, and concatenate the
 * granules' main data. MPEG1 needs TWO EA frames per MPEG frame.
 *
 * "Free format" (bitrate index 0) is used deliberately, per vgmstream: it makes
 * every frame large enough that no bit reservoir is needed, which is what lets
 * granules be rebuilt independently.
 *
 * Algorithm from vgmstream's mpeg_custom_utils_ealayer3.c, used as the
 * specification. The OUTPUT is verifiable independently: a rebuilt stream is
 * ordinary MP3, so ffmpeg decodes it or it does not. */

struct Eal3Gran {
    uint32_t version_index, sample_rate_index, channel_mode, mode_extension;
    int      version, channels, sample_rate, mpeg1;
    uint32_t granule_index;
    uint32_t scfsi[2];
    uint32_t main_data_size[2], others_1[2], others_2[2];
    size_t   data_offset_b;          /* bit offset of main data within the frame */
    uint32_t common_size;            /* 0 = PCM-only frame, nothing to rebuild   */
    uint32_t pcm_samples;            /* EaLayer32Pcm: samples appended to this frame */
    const uint8_t* base;
};

struct BitW {
    std::vector<uint8_t>* buf;
    size_t bit;
    void put(int w, uint32_t v) {
        for (int i = w - 1; i >= 0; i--) {
            const size_t byte = bit >> 3;
            if (byte >= buf->size()) buf->resize(byte + 1, 0);
            if ((v >> i) & 1) (*buf)[byte] |= (uint8_t)(0x80 >> (bit & 7));
            bit++;
        }
    }
};

static uint32_t eal3_get(const uint8_t* p, size_t& bit, int w)
{
    uint32_t v = 0;
    for (int i = 0; i < w; i++) { v = (v << 1) | ((p[bit >> 3] >> (7 - (bit & 7))) & 1); bit++; }
    return v;
}

/* Parse one EA frame at `base` (its own bit 0). Returns bytes consumed, 0 on error. */
static uint32_t eal3_parse(const uint8_t* base, size_t avail, Eal3Gran& g)
{
    static const int version_table[4]  = { 3, -1, 2, 1 };
    static const int channel_table[4]  = { 2, 2, 2, 1 };
    static const int rate_table[4][4]  = {
        { 11025, 12000,  8000, -1 }, { -1, -1, -1, -1 },
        { 22050, 24000, 16000, -1 }, { 44100, 48000, 32000, -1 },
    };
    if (avail < 2) return 0;
    std::memset(&g, 0, sizeof(g));
    g.base = base;
    size_t bit = 0;
    const uint32_t extended = eal3_get(base, bit, 1);
    /* stereo flag */            eal3_get(base, bit, 1);
    /* reserved   */             eal3_get(base, bit, 2);
    const uint32_t frame_size =  eal3_get(base, bit, 12);
    if (frame_size < 2 || frame_size > avail) return 0;

    uint32_t v2_common_size = 1;   /* non-extended frames always carry a common header */
    if (extended) {
        eal3_get(base, bit, 2);                       /* offset_mode    */
        eal3_get(base, bit, 10);                      /* offset_samples */
        g.pcm_samples = eal3_get(base, bit, 10);
        v2_common_size = eal3_get(base, bit, 10);
    }
    if (!v2_common_size) { g.common_size = 0; return frame_size; }  /* PCM-only */

    const size_t start_b = bit;
    g.version_index     = eal3_get(base, bit, 2);
    g.sample_rate_index = eal3_get(base, bit, 2);
    g.channel_mode      = eal3_get(base, bit, 2);
    g.mode_extension    = eal3_get(base, bit, 2);
    if (!g.version_index && !g.sample_rate_index && !g.channel_mode && !g.mode_extension) return 0;

    g.version     = version_table[g.version_index];
    g.channels    = channel_table[g.channel_mode];
    g.sample_rate = rate_table[g.version_index][g.sample_rate_index];
    g.mpeg1       = (g.version == 1);
    if (g.version == -1 || g.sample_rate == -1 || g.channels > 2) return 0;

    const int others_2_bits = g.mpeg1 ? (47 - 32) : (51 - 32);
    g.granule_index = eal3_get(base, bit, 1);

    if (g.mpeg1 && g.granule_index == 1)
        for (int i = 0; i < g.channels; i++) g.scfsi[i] = eal3_get(base, bit, 4);

    for (int i = 0; i < g.channels; i++) {
        g.main_data_size[i] = eal3_get(base, bit, 12);
        g.others_1[i]       = eal3_get(base, bit, 32);
        g.others_2[i]       = eal3_get(base, bit, others_2_bits);
    }

    g.data_offset_b = bit;
    size_t base_size_b = bit - start_b, data_size_b = 0;
    for (int i = 0; i < g.channels; i++) data_size_b += g.main_data_size[i];
    size_t pad = 0;
    if ((base_size_b + data_size_b) % 8) pad = 8 - ((base_size_b + data_size_b) % 8);
    g.common_size = (uint32_t)((base_size_b + data_size_b + pad) / 8);
    if ((g.data_offset_b + data_size_b + 7) / 8 > frame_size) return 0;
    return frame_size;
}

/* Rebuild one standard MPEG frame. g1 may be null for MPEG2. */
static bool eal3_rebuild(const Eal3Gran& g0, const Eal3Gran* g1, std::vector<uint8_t>& out)
{
    if (!g0.common_size) return false;
    if (g0.mpeg1 && (!g1 || !g1->common_size || g0.granule_index == g1->granule_index)) return false;

    /* Free format (index 0) needs no bit reservoir but ffmpeg rejects it
     * ("unspecified frame size"), so use the max standard bitrate instead -
     * 320 kbps MPEG1 / 160 kbps MPEG2, index 0x0E - and report a frame that
     * will not fit rather than emitting a corrupt one. */
    const int bitrate_index = 0x0E;
    const int expected = g0.mpeg1 ? (int)(144l * 320 * 1000l / g0.sample_rate)
                                  : (int)( 72l * 160 * 1000l / g0.sample_rate);
    out.assign((size_t)expected, 0);
    BitW os{ &out, 0 };

    os.put(11, 0x7FF);                 /* sync              */
    os.put(2,  g0.version_index);
    os.put(2,  0x01);                  /* layer III         */
    os.put(1,  1);                     /* no CRC            */
    os.put(4,  bitrate_index);         /* 0x0E = 320k MPEG1 / 160k MPEG2 */
    os.put(2,  g0.sample_rate_index);
    os.put(1,  0);                     /* padding           */
    os.put(1,  0);                     /* private           */
    os.put(2,  g0.channel_mode);
    os.put(2,  g0.mode_extension);
    os.put(1,  1);                     /* copyright         */
    os.put(1,  1);                     /* original          */
    os.put(2,  0);                     /* emphasis          */

    if (g0.mpeg1) {
        os.put(9, 0);                                      /* main_data_begin */
        os.put(g0.channels == 1 ? 5 : 3, 0);               /* private bits    */
        for (int i = 0; i < g1->channels; i++) os.put(4, g1->scfsi[i]);
        for (int i = 0; i < g0.channels; i++) {
            os.put(12, g0.main_data_size[i]);
            os.put(32, g0.others_1[i]);
            os.put(47 - 32, g0.others_2[i]);
        }
        for (int i = 0; i < g1->channels; i++) {
            os.put(12, g1->main_data_size[i]);
            os.put(32, g1->others_1[i]);
            os.put(47 - 32, g1->others_2[i]);
        }
        size_t rb = g0.data_offset_b;
        for (int i = 0; i < g0.channels; i++)
            for (uint32_t j = 0; j < g0.main_data_size[i]; j++) os.put(1, eal3_get(g0.base, rb, 1));
        rb = g1->data_offset_b;
        for (int i = 0; i < g1->channels; i++)
            for (uint32_t j = 0; j < g1->main_data_size[i]; j++) os.put(1, eal3_get(g1->base, rb, 1));
    } else {
        os.put(8, 0);
        os.put(g0.channels == 1 ? 1 : 2, 0);
        for (int i = 0; i < g0.channels; i++) {
            os.put(12, g0.main_data_size[i]);
            os.put(32, g0.others_1[i]);
            os.put(51 - 32, g0.others_2[i]);
        }
        size_t rb = g0.data_offset_b;
        for (int i = 0; i < g0.channels; i++)
            for (uint32_t j = 0; j < g0.main_data_size[i]; j++) os.put(1, eal3_get(g0.base, rb, 1));
    }

    if (os.bit % 8) os.put((int)(8 - (os.bit % 8)), 0);
    if ((size_t)(os.bit / 8) > (size_t)expected) return false;   /* would need a bit reservoir */
    out.resize((size_t)expected, 0);
    return true;
}
