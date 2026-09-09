/* Origin Online Activation (OOA) licence lift, in memory.
 *
 * EA App builds of BF6 wrap the executable and encrypt five PE sections,
 * including `typeinfo` and `fieldinf`. Anything reading the reflection tables
 * therefore opens the file, finds a typeinfo section of the right size, and
 * resolves EVERY type to ZERO FIELDS - it does not error, it returns an empty
 * game. See `encrypted-typeinfo-selected-over-a-readable-build`.
 *
 * The Godot plugin has done this since 2026-08-15 using the USER'S OWN licence
 * file; libbf6 never got the port, which is why it needs BF6_EXE pointed at a
 * research copy. This is that port, so an EA user's own install works with no
 * external file. Authorized Portal-editor work; ported from the GDScript in
 * `bf6_types.gd` (_lift_in_memory / _section_key).
 *
 * NOTHING IS WRITTEN TO DISK and no licence is created, altered or bypassed:
 * the decrypted bytes exist only in this process's buffer, the installed
 * executable is untouched, and the lift simply FAILS on a machine whose own
 * licence is absent. AES-128 is implemented here because libbf6 carries no
 * dependencies and builds for both Godot and Unreal.
 */

namespace {

static const uint8_t kAesSbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t kAesInv[256];
static bool    kAesInvReady = false;

static void aes_init_inv()
{
    if (kAesInvReady) return;
    for (int i = 0; i < 256; i++) kAesInv[kAesSbox[i]] = (uint8_t)i;
    kAesInvReady = true;
}

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) { if (b & 1) p ^= a; a = xtime(a); b >>= 1; }
    return p;
}

static void aes_expand(const uint8_t key[16], uint8_t rk[176])
{
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i-4], rk[i-3], rk[i-2], rk[i-1] };
        if (i % 16 == 0) {
            const uint8_t tmp = t[0];
            t[0] = (uint8_t)(kAesSbox[t[1]] ^ rcon);
            t[1] = kAesSbox[t[2]];
            t[2] = kAesSbox[t[3]];
            t[3] = kAesSbox[tmp];
            rcon = xtime(rcon);
        }
        for (int k = 0; k < 4; k++) rk[i + k] = (uint8_t)(rk[i - 16 + k] ^ t[k]);
    }
}

static void aes_decrypt_block(const uint8_t rk[176], uint8_t s[16])
{
    aes_init_inv();
    for (int k = 0; k < 16; k++) s[k] ^= rk[160 + k];
    for (int round = 9; round >= 0; round--) {
        uint8_t t[16];
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[((c + r) % 4) * 4 + r] = s[c * 4 + r];      /* InvShiftRows */
        for (int k = 0; k < 16; k++) s[k] = kAesInv[t[k]];    /* InvSubBytes  */
        for (int k = 0; k < 16; k++) s[k] ^= rk[round * 16 + k];
        if (round > 0) {                                       /* InvMixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t* p = s + c * 4;
                const uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3, 9));
                p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
                p[2] = (uint8_t)(gmul(a0,13) ^ gmul(a1, 9) ^ gmul(a2,14) ^ gmul(a3,11));
                p[3] = (uint8_t)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2, 9) ^ gmul(a3,14));
            }
        }
    }
}

static bool aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                            uint8_t* buf, size_t n)
{
    if (n % 16) return false;
    uint8_t rk[176];
    aes_expand(key, rk);
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t o = 0; o < n; o += 16) {
        uint8_t cipher[16], block[16];
        std::memcpy(cipher, buf + o, 16);
        std::memcpy(block, cipher, 16);
        aes_decrypt_block(rk, block);
        for (int k = 0; k < 16; k++) buf[o + k] = (uint8_t)(block[k] ^ prev[k]);
        std::memcpy(prev, cipher, 16);
    }
    return true;
}

static bool b64_decode(const std::string& in, std::vector<uint8_t>& out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    uint32_t acc = 0; int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = val(c);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)((acc >> bits) & 0xFF)); }
    }
    return true;
}

}  // namespace

/* The machine's own licence for `content_id` -> its 16-byte section key, or
 * empty. Reads only from the local EA licence directory; if the user has never
 * run the game there is no file and the lift correctly does not happen. */
std::vector<uint8_t> TypeDb::ooa_section_key(const std::string& content_id,
                                             std::string& note)
{
    static const uint8_t kLicKey[16] = {
        65, 50, 114, 45, 208, 130, 239, 176, 220, 100, 87, 197, 118, 104, 202, 9 };
    const uint8_t zero_iv[16] = {0};

    const char* pd = std::getenv("ProgramData");
    const std::string base = std::string(pd ? pd : "C:/ProgramData") +
                             "/Electronic Arts/EA Services/License/";
    const std::string cands[2] = { base + content_id + ".dlf",
                                   base + content_id + "_cached.dlf" };
    for (const std::string& path : cands) {
        FILE* f = fopen_binary_read(path.c_str());
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0x41) { std::fclose(f); continue; }
        std::vector<uint8_t> raw((size_t)n);
        const size_t got = std::fread(raw.data(), 1, (size_t)n, f);
        std::fclose(f);
        if (got != (size_t)n) continue;

        std::vector<uint8_t> body(raw.begin() + 0x41, raw.end());
        body.resize(body.size() - (body.size() % 16));
        if (body.empty()) continue;
        if (!aes_cbc_decrypt(kLicKey, zero_iv, body.data(), body.size())) continue;

        const std::string txt((const char*)body.data(), body.size());
        const std::string tag = "<CipherKey>";
        const size_t a = txt.find(tag);
        if (a == std::string::npos) continue;
        const size_t b = txt.find('<', a + tag.size());
        if (b == std::string::npos) continue;
        std::string b64 = txt.substr(a + tag.size(), b - a - tag.size());
        if (b64.size() > 24) b64 = b64.substr(0, 24);
        std::vector<uint8_t> key;
        if (!b64_decode(b64, key) || key.size() < 16) continue;
        key.resize(16);
        note = "licence " + path;
        return key;
    }
    note = "no licence file for content id '" + content_id + "' under " + base +
           " - run the game once so EA writes it, then reopen";
    return {};
}

/* Decrypt, in this object's buffer only, the sections the .ooa metadata lists.
 * Returns how many were decrypted; 0 means no lift and `note` says why. */
int TypeDb::ooa_lift(std::string& note)
{
    const Section* ooa = nullptr;
    for (const Section& s : sections_) if (s.name == ".ooa") { ooa = &s; break; }
    if (!ooa) { note = "encrypted but there is no .ooa section - cannot lift"; return 0; }
    const size_t o = ooa->raw_off;
    if (o + 0x500 > data_.size()) { note = ".ooa section is truncated"; return 0; }

    /* content id: a UTF-16 string at .ooa + 0x42, ASCII range in practice. */
    std::string cid;
    for (size_t i = 0; i < 0x200; i += 2) {
        const uint8_t lo = data_[o + 0x42 + i], hi = data_[o + 0x42 + i + 1];
        if (!lo && !hi) break;
        if (hi || lo < 0x20 || lo > 0x7e) break;
        cid.push_back((char)lo);
    }
    while (!cid.empty() && (cid.back() == ' ' || cid.back() == '\r' || cid.back() == '\n'))
        cid.pop_back();
    if (cid.empty()) { note = "could not read the .ooa content id"; return 0; }

    std::string keynote;
    const std::vector<uint8_t> key = ooa_section_key(cid, keynote);
    if (key.size() != 16) { note = keynote; return 0; }

    /* block list: count at .ooa+0x4EE, entries from +0x4F0 stride 0x30,
     * section VA = u32 at entry+0 times 0x100. */
    const int count = (int)data_[o + 0x4EE];
    if (count < 1 || count > 10) { note = "unexpected .ooa block count"; return 0; }

    int done = 0;
    for (int i = 0; i < count; i++) {
        const size_t bo = o + 0x4F0 + (size_t)i * 0x30;
        if (bo + 4 > data_.size()) break;
        const uint64_t va = (uint64_t)rd<uint32_t>(data_, bo) * 0x100ull;
        for (const Section& s : sections_) {
            if ((uint64_t)s.va != va) continue;
            const size_t ro = s.raw_off, rs = s.raw_size;
            if (rs == 0 || (rs % 16) || ro < 16 || ro + rs > data_.size()) break;
            uint8_t iv[16];
            std::memcpy(iv, data_.data() + ro - 16, 16);   /* IV precedes the section */
            if (!aes_cbc_decrypt(key.data(), iv, data_.data() + ro, rs)) break;
            /* PADDING QUIRK: a trailing all-0x10 block decrypts to zeros. */
            bool all10 = true;
            for (size_t j = rs - 16; j < rs; j++)
                if (data_[ro + j] != 0x10) { all10 = false; break; }
            if (all10) std::memset(data_.data() + ro + rs - 16, 0, 16);
            done++;
            break;
        }
    }
    if (done == 0) { note = "no listed block matched a PE section"; return 0; }
    note = keynote;
    return done;
}
