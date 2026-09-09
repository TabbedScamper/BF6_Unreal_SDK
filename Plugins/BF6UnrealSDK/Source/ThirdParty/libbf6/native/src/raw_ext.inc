/* raw_ext.inc - the mount's own name tables and bytes, over the C ABI.
 *
 * NEW FILE. A translation-unit fragment, not a standalone .cpp: it uses
 * bf6_ctx, which only exists inside bf6_core.cpp. Wire it with ONE line in
 * src/bf6_core.cpp, immediately BEFORE that file's last line, so the entry
 * points get C linkage:
 *
 *     #include "raw_ext.inc"
 *     }  // extern "C"          <- the existing last line, unchanged
 *
 * WHY IT EXISTS. Everything else in this ABI hands back DECODED things,
 * because that is what an engine binding wants. A pipeline that reproduces the
 * game's asset tree on disk wants the opposite: the bytes as the mount
 * produces them, so a decoder written elsewhere can read them without this
 * library growing that decoder's opinions. That is the whole difference
 * between "render this mesh" and "extract this install".
 *
 * Nothing here parses anything, with one exception: bf6_res_chunks, which has
 * to, because the mapping resource -> chunk is the one edge that the mount's
 * tables do not record. It lives in the resource's own header.
 */

/* ------------------------------------------------------------------- mount */

int bf6_mount_all(bf6_ctx* c, int include_levels, char* err, int err_len)
{
    if (!c) return 0;
    /* bf6_open mounts only <game>/Data/Win32/*.toc, which is the fast
     * level-independent catalogue and is NOT the whole install. Measured on a
     * retail install, the per-weapon attachment records - the files whose
     * NAMES are the weapon/attachment compatibility table - are not in that
     * set at all: a weapon folder that has 250 entries in a full extraction
     * has 4 there. They arrive with the archives under Update/, which this
     * walks for.
     *
     * include_levels also pulls every level's own archives. That is a
     * different and much larger thing, and the previewer does not need it,
     * so it is a parameter rather than the default. */
    std::string e;
    if (!c->src.mount_level(std::string(), include_levels != 0, e)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", e.c_str());
        return 0;
    }
    return 1;
}

int bf6_mount_frontend(bf6_ctx* c, char* err, int err_len)
{
    if (!c) return 0;
    std::string e;
    if (!c->src.mount_frontend(e)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", e.c_str());
        return 0;
    }
    return 1;
}

int bf6_mount_level_archives(bf6_ctx* c, const char* level,
                             char* err, int err_len)
{
    if (!c || !level || !*level) return 0;
    std::string e;
    if (!c->src.mount_level(level, false, e)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", e.c_str());
        return 0;
    }
    return 1;
}

int bf6_mount_ebx_owner(bf6_ctx* c, const char* toc_path,
                        const char* bundle_name, const char* ebx_name,
                        char* err, int err_len)
{
    if (!c || !toc_path || !bundle_name || !ebx_name) return 0;
    std::string e;
    if (!c->src.mount_ebx_owner(toc_path, bundle_name, ebx_name, e)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", e.c_str());
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------- name tables */

static int bf6__list_map_ebx(bf6_ctx* c, const char* search,
                             bf6_asset* out, int out_max)
{
    const std::string q = search ? search : "";
    int total = 0, written = 0;
    for (const auto& kv : c->src.ebx()) {
        if (!q.empty() && kv.first.find(q) == std::string::npos) continue;
        if (out && written < out_max) {
            /* The key's storage belongs to the Source's own table and outlives
             * every call; handing back a pointer into it is what lets a caller
             * enumerate 95,000 names without this library allocating any. */
            out[written].name = kv.first.c_str();
            out[written].type = 0;
            out[written].size = kv.second.dsize;
            written++;
        }
        total++;
    }
    return total;
}

int bf6_list_ebx(bf6_ctx* c, const char* search, bf6_asset* out, int out_max)
{
    if (!c) return 0;
    return bf6__list_map_ebx(c, search, out, out_max);
}

int bf6_list_res(bf6_ctx* c, const char* search, bf6_asset* out, int out_max)
{
    if (!c) return 0;
    const std::string q = search ? search : "";
    int total = 0, written = 0;
    for (const auto& kv : c->src.res()) {
        if (!q.empty() && kv.first.find(q) == std::string::npos) continue;
        if (out && written < out_max) {
            out[written].name = kv.first.c_str();
            out[written].type = kv.second.type;
            out[written].size = kv.second.dsize;
            written++;
        }
        total++;
    }
    return total;
}

int bf6_list_chunks(bf6_ctx* c, char* out, int out_max)
{
    if (!c) return 0;
    int total = 0, written = 0;
    /* Both tables, and a guid that is in both is ONE chunk, not two: get_chunk
     * looks in the loose table first, so the loose spelling is the one a caller
     * would get back and the one reported here. */
    std::set<std::string> seen;
    for (const auto& kv : c->src.loose_chunks())  seen.insert(kv.first);
    for (const auto& kv : c->src.bundle_chunks()) seen.insert(kv.first);
    for (const std::string& g : seen) {
        if (out && written < out_max) {
            std::snprintf(out + (size_t)written * 33, 33, "%s", g.c_str());
            written++;
        }
        total++;
    }
    return total;
}

int bf6_partition_index(bf6_ctx* c, bf6_partition* out, int out_max)
{
    if (!c) return 0;
    /* Built on first call and cached on the Source: it reads the EFIX header
     * of EVERY mounted partition, which is the whole cost, and a caller that
     * sizes then fills would otherwise pay it twice. */
    const std::map<std::string, std::string>& idx = c->src.partition_index();
    int written = 0;
    for (const auto& kv : idx) {
        if (out && written < out_max) {
            out[written].guid = kv.first.c_str();
            out[written].name = kv.second.c_str();
            written++;
        }
    }
    return (int)idx.size();
}

int bf6_armory_partition_index(bf6_ctx* c, bf6_partition* out, int out_max)
{
    if (!c) return 0;
    const std::map<std::string, std::string>& idx = c->src.armory_partition_index();
    int written = 0;
    for (const auto& kv : idx) {
        if (out && written < out_max) {
            out[written].guid = kv.first.c_str();
            out[written].name = kv.second.c_str();
            written++;
        }
    }
    return (int)idx.size();
}

/* ------------------------------------------------------------------- bytes */

int64_t bf6_read_raw(bf6_ctx* c, int kind, const char* name,
                     const uint8_t** out_data)
{
    if (out_data) *out_data = nullptr;
    if (!c || !name) return -1;
    std::string err;
    std::vector<uint8_t> d;
    switch (kind) {
    case BF6_RAW_EBX:   d = c->src.get_ebx(name, err);   break;
    case BF6_RAW_RES:   d = c->src.get_res(name, err);   break;
    case BF6_RAW_CHUNK: d = c->src.get_chunk(name, err); break;
    default: return -1;
    }
    /* An empty read is a FAILED read here, not a zero-byte asset: the mount
     * has no zero-length entries, and reporting 0 would make a caller write an
     * empty file and call the extraction complete. */
    if (d.empty()) return -1;
    c->raw_buf.swap(d);
    if (out_data) *out_data = c->raw_buf.data();
    return (int64_t)c->raw_buf.size();
}

/* --------------------------------------------------- resource -> its chunks */

static const char kZeroGuid[] = "00000000000000000000000000000000";

/* A chunk guid is spelled one of TWO ways depending on the resource that names
 * it, and there is no rule that says which - bf6_read_mesh discovered this the
 * same way, by trying the forward bytes and then the reversed ones. Return the
 * spelling that is actually in the mount, so a caller never has to know the
 * question exists. Empty when neither resolves. */
static std::string bf6__resolve_guid(bf6_ctx* c, const std::array<uint8_t, 16>& b)
{
    static const char H[] = "0123456789abcdef";
    std::string fwd, rev;
    for (int i = 0; i < 16; i++)  { fwd += H[b[(size_t)i] >> 4]; fwd += H[b[(size_t)i] & 0xF]; }
    for (int i = 15; i >= 0; i--) { rev += H[b[(size_t)i] >> 4]; rev += H[b[(size_t)i] & 0xF]; }
    if (fwd == kZeroGuid) return std::string();
    if (c->src.has_chunk(fwd)) return fwd;
    if (c->src.has_chunk(rev)) return rev;
    return std::string();
}

static std::string bf6__resolve_guid_hex(bf6_ctx* c, const std::string& hex)
{
    if (hex.size() != 32 || hex == kZeroGuid) return std::string();
    std::array<uint8_t, 16> b{};
    for (int i = 0; i < 16; i++) {
        auto nib = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        const int hi = nib(hex[(size_t)i * 2]), lo = nib(hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::string();
        b[(size_t)i] = (uint8_t)((hi << 4) | lo);
    }
    return bf6__resolve_guid(c, b);
}

/* The bundle's own resource-type ids. WHICH PARSER TO USE IS RECORDED, NOT
 * GUESSED: Texture::read_header accepts a MeshSet's first bytes happily and
 * hands back two guids of nonsense, so "try the texture reader and fall back"
 * silently mis-reads every mesh in the mount. Measured over the 83,696
 * resources of a shared mount that also exist in a reference extraction, these
 * two ids partition Texture and MeshSet with no exceptions at all. */
#define BF6_RESTYPE_TEXTURE  0x6bde20bau
#define BF6_RESTYPE_MESHSET  0x49b156d4u

int bf6_res_chunks(bf6_ctx* c, const char* res_name, char* out, int out_max,
                   int* out_roles)
{
    if (!c || !res_name) return 0;
    const auto it = c->src.res().find(res_name);
    if (it == c->src.res().end()) return 0;
    const uint32_t rtype = it->second.type;
    if (rtype != BF6_RESTYPE_TEXTURE && rtype != BF6_RESTYPE_MESHSET) return 0;

    std::string err;
    std::vector<uint8_t> d = c->src.get_res(res_name, err);
    if (d.empty()) return 0;

    std::vector<std::pair<std::string, int> > guids;

    if (rtype == BF6_RESTYPE_TEXTURE) {
        bf6::TextureHeader th;
        if (bf6::Texture::read_header(d, th)) {
            /* Both slots are read unconditionally by the header reader, so a
             * sheet with no streamed mip0 still reports one - as sixteen zero
             * bytes. Resolving filters that, and any guid the mount lacks. */
            const std::string ge = bf6__resolve_guid_hex(c, th.embedded);
            if (!ge.empty()) guids.push_back(std::make_pair(ge, BF6_CHUNK_EMBEDDED));
            const std::string gs = bf6__resolve_guid_hex(c, th.streamed);
            if (!gs.empty()) guids.push_back(std::make_pair(gs, BF6_CHUNK_STREAMED));
        }
    } else {
        bf6::MeshSet ms = bf6::meshset_parse(d.data(), d.size(), err);
        if (ms.ok) {
            for (const bf6::MeshLod& l : ms.lods) {
                /* An all-zero id is a LOD whose bytes are inline in the
                 * resource, not a chunk that is missing. bf6__resolve_guid
                 * drops those as well as unresolvable ones. */
                const std::string g = bf6__resolve_guid(c, l.chunk_id);
                if (!g.empty()) guids.push_back(std::make_pair(g, BF6_CHUNK_LOD));
            }
        }
    }

    /* One LOD chunk can back several LODs; the caller is going to write files
     * named by guid, so duplicates would be duplicate work. The FIRST role
     * wins, which keeps an embedded chunk labelled embedded even if a later
     * slot names it again. */
    std::vector<std::pair<std::string, int> > uniq;
    for (const auto& g : guids) {
        bool dup = false;
        for (const auto& u : uniq) if (u.first == g.first) { dup = true; break; }
        if (!dup) uniq.push_back(g);
    }

    int written = 0;
    for (const auto& g : uniq) {
        if (written < out_max) {
            if (out) std::snprintf(out + (size_t)written * 33, 33, "%s", g.first.c_str());
            if (out_roles) out_roles[written] = g.second;
            written++;
        }
    }
    return (int)uniq.size();
}

/* ------------------------------------------ is the type schema readable here */

int bf6_typeinfo_readable(const char* exe_path, double* bits_out)
{
    if (bits_out) *bits_out = 0.0;
    if (!exe_path || !*exe_path) return -1;

    /* Read only the header plus enough of the file to cover the section we
     * want. The executable is ~190 MB and this runs on a first-run check, so
     * it seeks rather than slurps. */
    FILE* f = bf6::fopen_binary_read(exe_path);
    if (!f) return -1;

    unsigned char hdr[4096];
    const size_t got = std::fread(hdr, 1, sizeof(hdr), f);
    if (got < 1024) { std::fclose(f); return -1; }

    auto u16 = [&](size_t o) { return (uint32_t)hdr[o] | ((uint32_t)hdr[o + 1] << 8); };
    auto u32 = [&](size_t o) {
        return (uint32_t)hdr[o] | ((uint32_t)hdr[o + 1] << 8) |
               ((uint32_t)hdr[o + 2] << 16) | ((uint32_t)hdr[o + 3] << 24);
    };

    const uint32_t pe = u32(0x3c);
    if (pe + 24 > got || hdr[pe] != 'P' || hdr[pe + 1] != 'E') { std::fclose(f); return -1; }
    const uint32_t nsec  = u16(pe + 6);
    const uint32_t optsz = u16(pe + 20);
    const uint32_t so    = pe + 24 + optsz;

    uint32_t ti_off = 0, ti_size = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        const size_t o = so + i * 40u;
        if (o + 40 > got) break;
        char nm[9] = {0};
        std::memcpy(nm, hdr + o, 8);
        if (std::strncmp(nm, "typeinfo", 8) != 0) continue;
        ti_size = u32(o + 16);      /* SizeOfRawData    */
        ti_off  = u32(o + 20);      /* PointerToRawData */
        break;
    }
    /* No typeinfo section at all is not the same failure as an encrypted one,
     * but it has the same consequence for a caller, so it reports the same
     * "not readable" rather than an error it would have to special-case. */
    if (!ti_size) { std::fclose(f); return 0; }

    const size_t want = std::min<size_t>(ti_size, 1u << 20);
    std::vector<unsigned char> buf(want);
    if (std::fseek(f, (long)ti_off, SEEK_SET) != 0 ||
        std::fread(buf.data(), 1, want, f) != want) {
        std::fclose(f);
        return -1;
    }
    std::fclose(f);

    uint64_t counts[256] = {0};
    for (unsigned char b : buf) counts[b]++;
    double bits = 0.0;
    for (int i = 0; i < 256; i++)
        if (counts[i]) {
            const double p = (double)counts[i] / (double)want;
            bits -= p * std::log2(p);
        }
    if (bits_out) *bits_out = bits;

    /* The same threshold TypeDb::looks_encrypted uses, on the same bytes, so
     * the two cannot disagree. Measured on the retail builds: a readable
     * typeinfo section is 3.77 bits/byte and a DRM-wrapped one is 8.00, so the
     * gap this sits in is not a close call. */
    return bits > 7.0 ? 0 : 1;
}

/* Resolve a ResourceRef id to the RES it names.
 *
 * An EBX ResourceRef is a 64-bit rid, NOT a name, and a reader that only looks
 * assets up by name simply stops at one - which is how a reference can look
 * like a dead end when the resource is present and streamable. The res index
 * already carries the rid on every entry, so this is a lookup rather than a
 * search; it is exposed because "the reference does not resolve" and "this
 * reader cannot resolve references" are very different conclusions and were
 * being confused. */
int bf6_res_by_rid(bf6_ctx* c, uint64_t rid, char* out, int out_len)
{
    if (!c || !out || out_len <= 0) return 0;
    out[0] = 0;
    if (!rid) return 0;
    for (const auto& kv : c->src.res())
        if (kv.second.rid == rid) {
            std::snprintf(out, (size_t)out_len, "%s", kv.first.c_str());
            return 1;
        }
    return 0;
}
