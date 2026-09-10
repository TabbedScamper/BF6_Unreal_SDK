#include "source.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>

#include "bundle.h"
#include "cas.h"
#include "oodle.h"
#include "stdio_compat.h"

namespace bf6 {

namespace {

// Two independent full-content 64-bit fingerprints. This is an invalidation
// identity, not an authenticity primitive: the cache remains untrusted and
// every decoded path is checked against the current live mount before use.
struct ContentId {
    uint64_t a = 1469598103934665603ull;
    uint64_t b = 0x9e3779b97f4a7c15ull;
    void add(const void* ptr, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(ptr);
        for (size_t i = 0; i < size; ++i) {
            a = (a ^ p[i]) * 1099511628211ull;
            b ^= (uint64_t)p[i] + 0x9e3779b97f4a7c15ull + (b << 6) + (b >> 2);
        }
    }
    void add(const std::string& s) { add(s.data(), s.size()); }
};

static std::pair<uint64_t, uint64_t> content_id(const void* p, size_t n) {
    ContentId h; h.add(p, n); return {h.a, h.b};
}

static bool file_content_id(const std::filesystem::path& path, ContentId& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<char> buf(1 << 20); // heap: a Windows thread stack is only 1 MiB
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        const std::streamsize n = f.gcount();
        if (n > 0) out.add(buf.data(), (size_t)n);
    }
    return f.eof();
}

static bool armory_candidate(const std::string& name) {
    return name.find("common/hardware") != std::string::npos ||
           name.find("common/characters") != std::string::npos ||
           // GlacierFlow's authored hangar is not self-contained: its single
           // StaticModelGroup imports the shell, floor and set dressing from
           // the shared common/environment family.  Without those partition
           // GUIDs the member records decode correctly but every MeshAsset is
           // left as <not indexed>, silently dropping all 3,215 placements.
           // This remains a bounded front-end index over mounted shared TOCs;
           // it does not pull in any playable-level partition family.
           name.find("common/environment") != std::string::npos ||
           // Front-end actor presentation walks this one authored scene for
           // camera anchors, studio lights and nearby hangar geometry.  Keep
           // the complete scene family in the bounded index so those walks do
           // not fall back to indexing every playable-level partition.
           name.find("game/glacierflow/flow_mainmenu") != std::string::npos ||
           name.find("game/glacierflow/common") != std::string::npos ||
           // Hardware shader sheets are direct material dependencies of the
           // vehicle meshes above (for example the Abrams emissive/CA sheet).
           // Keep this narrower than all common/shaders so the bounded index
           // does not regress into the global every-partition scan.
           name.find("common/shaders/textures/hardware") != std::string::npos ||
           // Character materials also bind shared shader-owned sheets outside
           // common/characters.  The eye record, for example, puts its iris
           // mask, inner normal and fake reflection under this exact root.
           // Keeping the texture/character subtree bounded avoids the global
           // partition scan while making the complete live eye record usable.
           name.find("common/shaders/textures/character") != std::string::npos ||
           name.find("common/gameplay") != std::string::npos ||
           name.find("common/ui") != std::string::npos ||
           name.find("common/gamesetup/options") != std::string::npos ||
           name.find("common/gamesetup/gameconfigurations") != std::string::npos ||
           name.find("weapon") != std::string::npos ||
           name.find("projectile") != std::string::npos;
}

static bool valid_guid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    }
    return true;
}

static void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((uint8_t)(v >> (i * 8)));
}
static void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back((uint8_t)(v >> (i * 8)));
}
static bool get_u32(const std::vector<uint8_t>& d, size_t& p, size_t end, uint32_t& v) {
    if (p + 4 > end) return false; v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)d[p++] << (i * 8);
    return true;
}
static bool get_u64(const std::vector<uint8_t>& d, size_t& p, size_t end, uint64_t& v) {
    if (p + 8 > end) return false; v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)d[p++] << (i * 8);
    return true;
}
static std::string env_value(const char* name) {
#ifdef _MSC_VER
    char* raw = nullptr; size_t size = 0;
    if (_dupenv_s(&raw, &size, name) != 0 || !raw) return std::string();
    const std::string value(raw); std::free(raw); return value;
#else
    const char* raw = std::getenv(name); return raw ? raw : "";
#endif
}
static bool cache_disabled() {
    const std::string v = env_value("BF6_DISABLE_ARMORY_INDEX_CACHE");
    return !v.empty() && v != "0";
}
static std::filesystem::path cache_dir() {
    const std::string override_dir = env_value("BF6_ARMORY_INDEX_CACHE_DIR");
    if (!override_dir.empty()) return override_dir;
    const std::string local = env_value("LOCALAPPDATA");
    if (!local.empty()) return std::filesystem::path(local) / "BF6HighPoly" / "Cache";
    return std::filesystem::temp_directory_path() / "BF6HighPoly" / "Cache";
}

} // namespace

static std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = fopen_binary_read(path.c_str());
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) { out.resize((size_t)n); out.resize(std::fread(out.data(), 1, (size_t)n, f)); }
    std::fclose(f);
    return out;
}

bool Source::open(const std::string& game_dir, std::string& err) {
    game_ = game_dir;
    // The decompressor must be loaded before any CAS read - without it every
    // block decode silently returns empty.
    if (!oodle_open(game_dir)) { err = oodle_error(); return false; }
    return loc_.open(game_dir, err);
}

std::vector<uint8_t> Source::read_seg(const CasLoc& seg, bool allow_raw, std::string& err) {
    std::string path = loc_.cas_path(seg.chunk_id, seg.cas_ix);
    if (path.empty()) {
        err = "no cas file for install chunk";
        return std::vector<uint8_t>();
    }
    return cas_read(path, seg.off, seg.size, allow_raw, err);
}

bool Source::mount_toc(const std::string& toc_path, std::string& err) {
    // bf6_open and later targeted mounts share the same non-level TOCs. A
    // second parse cannot add anything under the first-wins law, so remember
    // successful paths and make repeated mount requests free.
    if (mounted_tocs_.find(toc_path) != mounted_tocs_.end()) return true;
    std::vector<uint8_t> raw = read_file(toc_path);
    if (raw.empty()) { err = "cannot read " + toc_path; return false; }

    // THE PARTITION INDEX IS BUILT FROM THE MOUNT, so a mount that grows after
    // it was built leaves it answering for a smaller world than exists. It only
    // ever MISSES - a guid it does not know reads as "no such partition" -
    // which is the worst shape a staleness bug can take: mounting a second
    // level and asking for its lighting returned "the level root imports no
    // outdoor preset" for a preset that is plainly imported. Marked stale here
    // and rebuilt on the next request, so the cost is only paid by a caller
    // that actually asks after mounting more.
    const size_t before = ebx_.size();


    Toc toc;
    if (!toc.parse(raw.data(), raw.size(), err)) return false;

    // Loose-chunk map, resolved while the toc body is resident.
    for (const TocChunk& c : toc.chunks) {
        if (!chunks_.count(c.guid)) chunks_[c.guid] = toc.chunk_location(c);
    }

    const std::vector<uint8_t>& body = toc.body();
    for (const TocBundle& b : toc.bundles) {
        std::string e2;
        std::vector<CasLoc> segs = read_segments(body.data(), body.size(),
                                                 (size_t)b.offset, e2);
        if (segs.empty()) continue;
        std::vector<uint8_t> meta = read_seg(segs[0], true, e2);
        if (meta.empty()) continue;
        Payload pay;
        if (!pay.parse(meta.data(), meta.size(), e2)) continue;

        // Positional: entry i of ebx-then-res is segment i+1 (segment 0 is meta).
        size_t si = 1;
        size_t nseg = segs.size();
        for (const auto& e : pay.ebx) {
            if (si < nseg && !ebx_.count(e.first)) {
                EbxEntry ee; ee.loc = segs[si]; ee.dsize = e.second;
                ebx_[e.first] = ee;
                ebx_bundle_[e.first] = b.name;
            }
            si++;
        }
        for (const PayloadRes& r : pay.res) {
            /* COUNT THE LISTING, not the survivor. This runs before the dedup
             * test on purpose - it is the number data/res_types.tsv appears to
             * hold, and the whole point is to compare the two. */
            res_entry_type_[r.type]++;
            res_entries_total_++;
            if (si < nseg && !res_.count(r.name)) {
                ResEntry re; re.loc = segs[si]; re.dsize = r.size;
                re.type = r.type; re.rid = r.rid;
                res_[r.name] = re;
                res_bundle_[r.name] = b.name;

                // A depot names the bundle it covers, so the index is built
                // from the name rather than from a second pass.
                const size_t sh = r.name.find("_win32_shaderstate/shaderblockdepot_");
                if (sh != std::string::npos)
                    depot_by_bundle_.emplace(r.name.substr(0, sh), r.name);
            }
            si++;
        }
        // Bundle chunks come after res, and mesh vertex data lives in one.
        for (const std::string& cid : pay.chunk_id) {
            if (si < nseg && !chunk_seg_.count(cid)) chunk_seg_[cid] = segs[si];
            si++;
        }
    }
    if (ebx_.size() != before) {
        pidx_built_ = false; pidx_.clear();
        light_names_built_ = false; light_names_.clear();
        armory_pidx_built_ = false; armory_pidx_.clear(); armory_pidx_candidates_.clear();
    }
    mounted_toc_hashes_[mount_key(toc_path)] = content_id(raw.data(), raw.size());
    mounted_tocs_.insert(toc_path);
    return true;
}

bool Source::mount_ebx_owner(const std::string& toc_path,
                             const std::string& bundle_name,
                             const std::string& ebx_name,
                             std::string& err)
{
    if (ebx_.find(ebx_name) != ebx_.end()) return true;
    const std::filesystem::path requested(toc_path);
    const std::string full = requested.is_absolute()
        ? requested.string()
        : (std::filesystem::path(game_) / requested).string();
    std::vector<uint8_t> raw = read_file(full);
    if (raw.empty()) { err = "cannot read " + full; return false; }

    Toc toc;
    if (!toc.parse(raw.data(), raw.size(), err)) return false;
    const std::vector<uint8_t>& body = toc.body();
    for (const TocBundle& b : toc.bundles)
    {
        if (b.name != bundle_name) continue;
        std::string e2;
        const std::vector<CasLoc> segs = read_segments(
            body.data(), body.size(), (size_t)b.offset, e2);
        if (segs.empty()) { err = e2; return false; }
        std::vector<uint8_t> meta = read_seg(segs[0], true, e2);
        if (meta.empty()) { err = e2; return false; }
        Payload pay;
        if (!pay.parse(meta.data(), meta.size(), e2)) { err = e2; return false; }
        size_t si = 1;
        for (const auto& e : pay.ebx)
        {
            if (e.first == ebx_name && si < segs.size())
            {
                EbxEntry ee; ee.loc = segs[si]; ee.dsize = e.second;
                ebx_[e.first] = ee;
                ebx_bundle_[e.first] = b.name;
                pidx_built_ = false; pidx_.clear();
                light_names_built_ = false; light_names_.clear();
                armory_pidx_built_ = false; armory_pidx_.clear(); armory_pidx_candidates_.clear();
                mounted_toc_hashes_[mount_key(full)] = content_id(raw.data(), raw.size());
                return true;
            }
            ++si;
        }
        err = "bundle does not carry EBX " + ebx_name;
        return false;
    }
    err = "TOC does not carry bundle " + bundle_name;
    return false;
}

std::vector<uint8_t> Source::get_chunk(const std::string& guid_hex, std::string& err) {
    std::string g = guid_hex;
    for (char& ch : g) if (ch >= 'A' && ch <= 'Z') ch += 32;   // lower
    auto it = chunks_.find(g);
    if (it != chunks_.end()) return read_seg(it->second, false, err);
    auto it2 = chunk_seg_.find(g);
    if (it2 != chunk_seg_.end()) return read_seg(it2->second, false, err);
    err = "chunk " + g.substr(0, 16) + " is in no chunk map";
    return std::vector<uint8_t>();
}

bool Source::has_chunk(const std::string& guid_hex) const {
    std::string g = guid_hex;
    for (char& ch : g) if (ch >= 'A' && ch <= 'Z') ch += 32;
    return chunks_.find(g) != chunks_.end() || chunk_seg_.find(g) != chunk_seg_.end();
}

std::vector<uint8_t> Source::get_res(const std::string& name, std::string& err) {
    auto it = res_.find(name);
    if (it == res_.end()) { err = "no res named " + name; return std::vector<uint8_t>(); }
    std::vector<uint8_t> d = read_seg(it->second.loc, false, err);
    if (d.size() != it->second.dsize) {
        char m[96];
        std::snprintf(m, sizeof(m), "res declared %u bytes, got %zu",
                      it->second.dsize, d.size());
        err = m;
        return std::vector<uint8_t>();
    }
    return d;
}

std::vector<uint8_t> Source::get_ebx(const std::string& name, std::string& err) {
    auto it = ebx_.find(name);
    if (it == ebx_.end()) { err = "no ebx named " + name; return std::vector<uint8_t>(); }
    std::vector<uint8_t> d = read_seg(it->second.loc, false, err);
    if (d.size() != it->second.dsize) { err = "ebx size mismatch"; return std::vector<uint8_t>(); }
    return d;
}

const std::string& Source::bundle_of(const std::string& res_name) const
{
    static const std::string kEmpty;
    auto it = res_bundle_.find(res_name);
    return it == res_bundle_.end() ? kEmpty : it->second;
}

const std::string& Source::bundle_of_ebx(const std::string& ebx_name) const
{
    static const std::string kEmpty;
    auto it = ebx_bundle_.find(ebx_name);
    return it == ebx_bundle_.end() ? kEmpty : it->second;
}

std::string Source::depot_for_bundle(const std::string& bundle) const
{
    if (bundle.empty()) return std::string();

    // A TOC SPELLS A BUNDLE "win32/game/..." AND A DEPOT SPELLS IT "game/...".
    // Depot resources are named after the bundle's ASSET path, which carries no
    // platform prefix. One token apart, and the lookup misses every time
    // without saying so.
    std::string b = bundle;
    if (b.rfind("win32/", 0) == 0) b = b.substr(6);

    auto hit = depot_by_bundle_.find(b);
    if (hit != depot_by_bundle_.end()) return hit->second;

    // ANCESTORS ONLY. Bundle paths nest by directory and a container's own
    // bundle repeats its directory name, so ".../mp_dumbo/sub_art_10_oob"
    // widens to ".../mp_dumbo/mp_dumbo". Never sideways: see the header.
    for (;;)
    {
        const size_t slash = b.find_last_of('/');
        if (slash == std::string::npos) break;
        b = b.substr(0, slash);
        const size_t leaf = b.find_last_of('/');
        const std::string cand = leaf == std::string::npos
            ? b + "/" + b : b + "/" + b.substr(leaf + 1);
        hit = depot_by_bundle_.find(cand);
        if (hit != depot_by_bundle_.end()) return hit->second;
        hit = depot_by_bundle_.find(b);
        if (hit != depot_by_bundle_.end()) return hit->second;
    }
    return std::string();
}

std::string Source::depot_for_res(const std::string& res_name) const
{
    return depot_for_bundle(bundle_of(res_name));
}

// ---------------------------------------------------------------------------
// Partition index
// ---------------------------------------------------------------------------

// A partition's own GUID, out of its EFIX fixup.
//
// Deliberately NOT done by handing the bytes to the deserializer: this is a
// header read of every partition in the mount, and the guid sits in a known
// place. The formatting MUST match the one the EBX reader produces, because
// this index is looked up with the keys that reader hands out: .NET mixed
// endian, first three groups little-endian and the last eight bytes as they lie.
static std::string efix_guid(const std::vector<uint8_t>& raw)
{
    if (raw.size() < 12 || std::memcmp(raw.data(), "RIFF", 4) != 0) return std::string();
    size_t o = 12;
    while (o + 8 <= raw.size())
    {
        uint32_t sz = 0;
        std::memcpy(&sz, raw.data() + o + 4, 4);
        if (std::memcmp(raw.data() + o, "EFIX", 4) == 0)
        {
            const size_t s = o + 8;
            if (s + 16 > raw.size()) return std::string();
            char buf[40];
            const uint8_t* g = raw.data() + s;
            std::snprintf(buf, sizeof(buf),
                "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                (unsigned)(g[0] | (g[1] << 8) | (g[2] << 16) | ((unsigned)g[3] << 24)),
                (unsigned)(g[4] | (g[5] << 8)), (unsigned)(g[6] | (g[7] << 8)),
                g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
            return buf;
        }
        o += 8 + (size_t)sz;
        if (o % 2 == 1) o++;
    }
    return std::string();
}

const std::unordered_map<std::string, std::string>& Source::light_name_index()
{
    if (light_names_built_) return light_names_;
    for (const auto& kv : ebx_) {
        std::string low = kv.first;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const std::string ref = kv.first + ".ebx";
        light_names_.emplace(low, ref);
        const size_t slash = low.find_last_of('/');
        light_names_.emplace(slash == std::string::npos ? low : low.substr(slash + 1), ref);
    }
    light_names_built_ = true;
    return light_names_;
}

const std::map<std::string, std::string>& Source::partition_index()
{
    if (pidx_built_) return pidx_;
    pidx_built_ = true;

    // CAS LOCALITY ORDER, not name order. Every partition in the mount is read
    // to get one 16-byte header, so the reads want to run down each archive in
    // the order the blocks lie rather than jumping the disk per name.
    std::vector<const std::pair<const std::string, EbxEntry>*> order;
    order.reserve(ebx_.size());
    for (const auto& kv : ebx_) order.push_back(&kv);
    std::sort(order.begin(), order.end(),
        [](const std::pair<const std::string, EbxEntry>* a,
           const std::pair<const std::string, EbxEntry>* b)
        {
            if (a->second.loc.chunk_id != b->second.loc.chunk_id)
                return a->second.loc.chunk_id < b->second.loc.chunk_id;
            if (a->second.loc.cas_ix != b->second.loc.cas_ix)
                return a->second.loc.cas_ix < b->second.loc.cas_ix;
            if (a->second.loc.off != b->second.loc.off)
                return a->second.loc.off < b->second.loc.off;
            // SAME BYTES UNDER TWO NAMES. An asset shipped at two paths shares
            // one partition guid, so "first name wins" is decided by whatever
            // order the sort happened to leave them in - which is not an order
            // at all when the locations are equal. Broken by name so this index
            // is at least the same on every run. Note the Godot plugin has no
            // such tie-break, so its pick depends on dictionary order and the
            // two readers can legitimately name the same partition differently.
            return a->first < b->first;
        });

    // READ IN PARALLEL, PUBLISH IN ORDER.
    //
    // This reads every partition in the mount for one 16-byte header, which is
    // the single biggest cost of opening a level - about 19 seconds on mp_dumbo
    // against 4 for the mount and 2.5 for the walk. It is also embarrassingly
    // parallel: each read is independent, and the only shared thing is the
    // result.
    //
    // The GUIDS ARE COLLECTED INTO A SLOT PER PARTITION and folded in afterwards
    // in the original order, NOT inserted from the workers. "First name wins"
    // is the rule that makes this index stable across runs, and a map written
    // from several threads would resolve ties by whichever thread got there
    // first - which is no rule at all.
    const size_t n = order.size();
    std::vector<std::string> found(n);
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = std::max<size_t>(1, std::min<size_t>(hw ? hw : 4, 16));

    std::atomic<size_t> next{0};
    auto worker = [&]()
    {
        std::string e;
        for (;;)
        {
            const size_t i = next.fetch_add(1);
            if (i >= n) return;
            // Every few hundred, from whichever worker got there. The callback
            // is documented as concurrent for exactly this.
            if (progress_ && (i & 511) == 0 && !progress_("indexing partitions", (int)i, (int)n))
                return;
            // Safe to run concurrently: cas_path only reads the locator, and
            // cas_read opens its own handle. Nothing here touches Source state.
            std::vector<uint8_t> bytes = read_seg(order[i]->second.loc, false, e);
            if (!bytes.empty()) found[i] = efix_guid(bytes);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t i = 0; i < workers; i++) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();

    for (size_t i = 0; i < n; i++)
        if (!found[i].empty()) pidx_.emplace(found[i], order[i]->first + ".ebx");
    return pidx_;
}

const std::map<std::string, std::string>& Source::armory_partition_index()
{
    if (armory_pidx_built_) return armory_pidx_;
    armory_pidx_built_ = true;

    // This is intentionally a broad runtime selector. The full-index control
    // resolves 931/931 model-definition mesh imports for all 185 roster rows
    // inside these families (shuffled/non-family control: 0 outside). Keeping
    // whole authored roots also covers gadgets and future names that do not
    // happen to contain the word "weapon".
    std::vector<const std::pair<const std::string, EbxEntry>*> order;
    order.reserve(ebx_.size() / 4);
    for (const auto& kv : ebx_) if (armory_candidate(kv.first)) order.push_back(&kv);
    std::sort(order.begin(), order.end(),
        [](const std::pair<const std::string, EbxEntry>* a,
           const std::pair<const std::string, EbxEntry>* b)
        {
            if (a->second.loc.chunk_id != b->second.loc.chunk_id)
                return a->second.loc.chunk_id < b->second.loc.chunk_id;
            if (a->second.loc.cas_ix != b->second.loc.cas_ix)
                return a->second.loc.cas_ix < b->second.loc.cas_ix;
            if (a->second.loc.off != b->second.loc.off)
                return a->second.loc.off < b->second.loc.off;
            return a->first < b->first;
        });

    // A cache is useful only when its identity was computed from the current
    // executable and every raw TOC in this exact mount. It stores no asset
    // payloads and is never authoritative: every path is revalidated against
    // ebx_ and the live family selector before it can be published.
    ContentId install;
    static const char schema[] =
        "bf6-armory-partition-index-v8-character-shader-textures";
    install.add(schema, sizeof(schema) - 1);
    const std::filesystem::path exe = std::filesystem::path(game_) / "bf6.exe";
    const bool have_exe = file_content_id(exe, install);
    for (const auto& kv : mounted_toc_hashes_) {
        install.add(kv.first);
        install.add(&kv.second.first, sizeof(kv.second.first));
        install.add(&kv.second.second, sizeof(kv.second.second));
    }
    const size_t candidate_count = order.size();
    install.add(&candidate_count, sizeof(candidate_count));
    std::ostringstream key;
    key << std::hex << std::setfill('0') << std::setw(16) << install.a
        << std::setw(16) << install.b;
    const std::filesystem::path cache = cache_dir() / ("armory-pidx-" + key.str() + ".bin");

    if (!cache_disabled() && have_exe && !mounted_toc_hashes_.empty()) {
        const std::vector<uint8_t> raw = read_file(cache.string());
        const size_t trailer = 16;
        size_t p = 0;
        bool ok = raw.size() >= 8 + 4 + 16 + 4 + trailer && raw.size() <= 32u * 1024u * 1024u;
        static const char magic[8] = {'B','F','6','A','P','I','D','X'};
        if (ok && std::memcmp(raw.data(), magic, 8) != 0) ok = false;
        p = 8;
        uint32_t version = 0, count = 0; uint64_t ia = 0, ib = 0, ca = 0, cb = 0;
        if (ok) ok = get_u32(raw, p, raw.size() - trailer, version) && version == 2 &&
                     get_u64(raw, p, raw.size() - trailer, ia) &&
                     get_u64(raw, p, raw.size() - trailer, ib) &&
                     ia == install.a && ib == install.b &&
                     get_u32(raw, p, raw.size() - trailer, count) && count <= order.size();
        if (ok) {
            size_t q = raw.size() - trailer;
            ok = get_u64(raw, q, raw.size(), ca) && get_u64(raw, q, raw.size(), cb);
            const auto actual = content_id(raw.data(), raw.size() - trailer);
            ok = ok && ca == actual.first && cb == actual.second;
        }
        std::map<std::string, std::string> first;
        std::map<std::string, std::vector<std::string>> all;
        for (uint32_t i = 0; ok && i < count; ++i) {
            uint32_t gl = 0, pl = 0;
            ok = get_u32(raw, p, raw.size() - trailer, gl) && gl == 36 && p + gl <= raw.size() - trailer;
            std::string guid;
            if (ok) { guid.assign((const char*)raw.data() + p, gl); p += gl; }
            ok = ok && get_u32(raw, p, raw.size() - trailer, pl) && pl > 4 && pl <= 4096 && p + pl <= raw.size() - trailer;
            std::string path;
            if (ok) { path.assign((const char*)raw.data() + p, pl); p += pl; }
            if (ok) {
                const bool suffix = path.size() > 4 && path.compare(path.size() - 4, 4, ".ebx") == 0;
                const std::string base = suffix ? path.substr(0, path.size() - 4) : std::string();
                ok = valid_guid(guid) && suffix && armory_candidate(base) && ebx_.find(base) != ebx_.end();
                if (ok) { first.emplace(guid, path); all[guid].push_back(path); }
            }
        }
        ok = ok && p == raw.size() - trailer && !first.empty();
        if (ok) {
            armory_pidx_.swap(first); armory_pidx_candidates_.swap(all);
            std::fprintf(stderr, "bf6 armory index cache hit: %zu candidates (%s)\n",
                         (size_t)count, key.str().c_str());
            return armory_pidx_;
        }
        if (!raw.empty()) std::fprintf(stderr, "bf6 armory index cache rejected; rebuilding from current install\n");
    }

    const size_t n = order.size();
    std::vector<std::string> found(n);
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t workers = std::max<size_t>(1, std::min<size_t>(hw ? hw : 4, 16));
    std::atomic<size_t> next{0};
    auto worker = [&]()
    {
        std::string e;
        for (;;)
        {
            const size_t i = next.fetch_add(1);
            if (i >= n) return;
            if (progress_ && (i & 511) == 0 &&
                !progress_("indexing armory partitions", (int)i, (int)n)) return;
            std::vector<uint8_t> bytes = read_seg(order[i]->second.loc, false, e);
            if (!bytes.empty()) found[i] = efix_guid(bytes);
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t i = 0; i < workers; i++) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    for (size_t i = 0; i < n; i++)
        if (!found[i].empty())
        {
            const std::string path = order[i]->first + ".ebx";
            armory_pidx_.emplace(found[i], path);
            armory_pidx_candidates_[found[i]].push_back(path);
        }

    if (!cache_disabled() && have_exe && !mounted_toc_hashes_.empty() && !armory_pidx_.empty()) {
        std::vector<uint8_t> raw;
        static const char magic[8] = {'B','F','6','A','P','I','D','X'};
        raw.insert(raw.end(), magic, magic + 8); put_u32(raw, 2);
        put_u64(raw, install.a); put_u64(raw, install.b);
        uint32_t count = 0;
        for (const auto& kv : armory_pidx_candidates_) count += (uint32_t)kv.second.size();
        put_u32(raw, count);
        for (const auto& kv : armory_pidx_candidates_) for (const std::string& path : kv.second) {
            put_u32(raw, (uint32_t)kv.first.size()); raw.insert(raw.end(), kv.first.begin(), kv.first.end());
            put_u32(raw, (uint32_t)path.size()); raw.insert(raw.end(), path.begin(), path.end());
        }
        const auto sum = content_id(raw.data(), raw.size()); put_u64(raw, sum.first); put_u64(raw, sum.second);
        std::error_code ec; std::filesystem::create_directories(cache.parent_path(), ec);
        const auto nonce = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
        const std::filesystem::path tmp = cache.string() + "." + std::to_string(nonce) + ".tmp";
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) { f.write((const char*)raw.data(), (std::streamsize)raw.size()); f.close();
            if (f) { std::filesystem::rename(tmp, cache, ec); if (ec && std::filesystem::exists(cache, ec)) std::filesystem::remove(tmp, ec); }
        }
    }
    return armory_pidx_;
}

const std::map<std::string, std::vector<std::string>>& Source::armory_partition_candidates()
{
    armory_partition_index();
    return armory_pidx_candidates_;
}

// ---------------------------------------------------------------------------
// Finding and mounting a level's archives
// ---------------------------------------------------------------------------

namespace {

std::string lower_slash(const std::string& s)
{
    std::string o = s;
    for (char& c : o)
    {
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }
    return o;
}

// The SDK names a scene by display name while the game files the level under an
// mp_ id (Portal_Sand -> levels/mp_portal_sand), so both spellings match.
std::vector<std::string> level_dirs(const std::string& level)
{
    std::string l = lower_slash(level);
    std::vector<std::string> out{ "/levels/" + l + "/" };
    if (l.rfind("mp_", 0) != 0) out.push_back("/levels/mp_" + l + "/");
    return out;
}

bool in_level_dir(const std::string& path, const std::vector<std::string>& dirs)
{
    const std::string p = lower_slash(path);
    for (const std::string& d : dirs)
        if (p.find(d) != std::string::npos) return true;
    return false;
}

}  // namespace

bool Source::is_level_toc(const std::string& path)
{
    return lower_slash(path).find("/levels/") != std::string::npos;
}

std::string Source::mount_key(const std::string& path)
{
    const std::string low = lower_slash(path);
    return (low.find("/update/") != std::string::npos ? "1" : "0") + low;
}

std::vector<std::string> Source::available_levels() const
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(game_, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        if (!it->is_directory(ec)) continue;
        const std::string here = lower_slash(it->path().string());
        if (here.size() >= 7 && here.compare(here.size() - 7, 7, "/levels") == 0)
        {
            std::error_code e2;
            for (const auto& sub : fs::directory_iterator(it->path(), e2))
                if (sub.is_directory(e2)) out.push_back(lower_slash(sub.path().filename().string()));
            it.disable_recursion_pending();   // the level dirs need no descent
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// THE ORDER IS THE CORRECTNESS, not a tidy-up. Mounting is FIRST WINS: the
// sweep keeps the first entry it sees for a name and skips the rest.
//
// Shared archives therefore go first, so a level cannot displace a global it
// depends on. Among LEVELS the same rule reads backwards from how it sounds:
// the level being read has to come FIRST, or every other level outranks it for
// any name they share, and levels share names freely - the shader-state depots,
// the terrain resources, the section keys. Sorted purely by path, mp_dumbo
// lands wherever the alphabet puts it and the level you are reading resolves
// against another level's data.
//
// So: shared archives, then this level, then everything else purely to make its
// objects reachable.
std::vector<std::string> Source::find_tocs(const std::string& level, bool all_levels) const
{
    namespace fs = std::filesystem;
    std::vector<std::string> shared, lvl;
    const std::vector<std::string> want = level_dirs(level);

    std::error_code ec;
    for (fs::recursive_directory_iterator it(game_, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec))
    {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string p = it->path().string();
        if (p.size() < 4 || lower_slash(p).compare(p.size() - 4, 4, ".toc") != 0) continue;
        if (is_level_toc(p))
        {
            if (all_levels || (!level.empty() && in_level_dir(p, want))) lvl.push_back(p);
        }
        else shared.push_back(p);
    }

    auto by_key = [](const std::string& a, const std::string& b)
    { return mount_key(a) < mount_key(b); };
    std::sort(shared.begin(), shared.end(), by_key);
    std::sort(lvl.begin(), lvl.end(), by_key);

    if (all_levels && !level.empty())
    {
        std::vector<std::string> mine, others;
        for (const std::string& p : lvl)
            (in_level_dir(p, want) ? mine : others).push_back(p);
        lvl = mine;
        lvl.insert(lvl.end(), others.begin(), others.end());
    }

    std::vector<std::string> out = shared;
    out.insert(out.end(), lvl.begin(), lvl.end());
    return out;
}

bool Source::mount_level(const std::string& level, bool all_levels, std::string& err)
{
    const std::vector<std::string> tocs = find_tocs(level, all_levels);
    if (tocs.empty()) { err = "no .toc found under " + game_; return false; }
    size_t mounted = 0;
    int done = 0;
    for (const std::string& t : tocs)
    {
        if (progress_ && !progress_("mounting the level's archives", done++, (int)tocs.size()))
        { err = "cancelled"; return false; }
        std::string e;
        if (mount_toc(t, e)) mounted++;
        // A toc that will not mount is not fatal on its own: the install
        // carries archives this reader has no business in. An empty mount is.
    }
    if (mounted == 0) { err = "no .toc mounted"; return false; }
    if (!level.empty())
    {
        bool any_level_toc = false;
        for (const std::string& t : tocs)
            if (is_level_toc(t) && in_level_dir(t, level_dirs(level))) { any_level_toc = true; break; }
        if (!any_level_toc)
        {
            err = "no archives for level '" + level + "'";
            return false;
        }
    }
    return true;
}

bool Source::mount_frontend(std::string& err)
{
    const std::vector<std::string> all = find_tocs(std::string(), false);
    if (all.empty()) { err = "no shared .toc found under " + game_; return false; }

    auto ends = [](const std::string& value, const char* suffix) {
        const size_t n = std::strlen(suffix);
        return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
    };
    std::vector<std::string> selected;
    for (const std::string& toc : all)
    {
        const std::string low = lower_slash(toc);
        const bool contentFamily =
            ends(low, "/characters.toc") || ends(low, "/globals.toc") ||
            ends(low, "/ui.toc") || ends(low, "/vehicles.toc") ||
            ends(low, "/weapons.toc");
        const bool mainMenu =
            low.find("/game/glacierflow/flow_mainmenu/") != std::string::npos;
        const bool englishText = ends(low, "/loc/en.toc");
        if (contentFamily || mainMenu || englishText) selected.push_back(toc);
    }
    if (selected.empty()) { err = "no front-end archive families found"; return false; }

    int mounted = 0, done = 0;
    for (const std::string& toc : selected)
    {
        if (progress_ && !progress_("mounting front-end archives", done++,
                                    (int)selected.size()))
        { err = "cancelled"; return false; }
        std::string one;
        if (mount_toc(toc, one)) ++mounted;
    }
    if (!mounted) { err = "no front-end archive mounted"; return false; }
    return true;
}

}  // namespace bf6
