/* The compositor's statically bound texture table. See terrainstatic.h for the
 * chain, the layer join and how far the join has been measured to hold. */
#include "terrainstatic.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include "depot.h"
#include "source.h"
#include "terrainjoin.h"
#include "terrainshader.h"

namespace bf6 {

namespace {

uint32_t rd32(const std::vector<uint8_t>& d, size_t o)
{ uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; }
uint16_t rd16(const std::vector<uint8_t>& d, size_t o)
{ uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2); return v; }
uint64_t rd64(const std::vector<uint8_t>& d, size_t o)
{ uint64_t v = 0; if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8); return v; }

// The compositor program's serialized id.
const uint64_t kTerrainProgram = 0x9F11D96B0FDF4773ull;

std::string lower(std::string s)
{ for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::string leaf_no_ebx(const std::string& asset)
{
    std::string a = asset;
    if (a.size() > 4 && a.compare(a.size() - 4, 4, ".ebx") == 0) a.resize(a.size() - 4);
    const size_t sl = a.find_last_of('/');
    return sl == std::string::npos ? a : a.substr(sl + 1);
}

struct RoleSuffix { const char* suf; TerrainTexRole role; };
const RoleSuffix kRoles[] = {
    { "_cv",  TerrainTexRole::BaseColor },
    { "_nhs", TerrainTexRole::NormalHeight },
    { "_noh", TerrainTexRole::NormalHeight },
    { "_ao",  TerrainTexRole::AmbientOcclusion },
    { "_op",  TerrainTexRole::Opacity },
    { "_cs",  TerrainTexRole::Smoothness },
    { "_mxx", TerrainTexRole::Mixed },
};

void split_role(const std::string& leaf, std::string& stem, TerrainTexRole& role)
{
    for (const RoleSuffix& r : kRoles)
    {
        const size_t n = std::strlen(r.suf);
        if (leaf.size() > n && leaf.compare(leaf.size() - n, n, r.suf) == 0)
        { stem = leaf.substr(0, leaf.size() - n); role = r.role; return; }
    }
    stem = leaf;
    role = TerrainTexRole::Unknown;
}

// The compositor's OWN sheets - the noise, gradients, break-up masks and the
// interior/crater sheets that many layer bodies share. These are named rather
// than positional because their position moves: on aftermath and dumbo they are
// descriptors 0..12, on isolated 0..15, and isolated also parks a lone granite
// gradient in the MIDDLE of the material stack. A group made only of these
// belongs to no layer.
const char* const kUtilityPrefix[] = {
    "t_perlinnoise",
    "t_gradientgranite",
    "t_ind_breakupmask",
    "t_gen_breakupmask",
    "t_tilingnoise",
    "t_scatternoises",
    "ta_interior_01",
    "hardsurface_exterior",
    "crater_",
    "t_wum_asphaltedge_01",
    "t_wum_imperfections_mask",
};

bool is_utility(const std::string& leaf)
{
    for (const char* p : kUtilityPrefix)
        if (leaf.compare(0, std::strlen(p), p) == 0) return true;
    return false;
}

}  // namespace

const char* terrain_tex_role_name(TerrainTexRole r)
{
    switch (r)
    {
    case TerrainTexRole::BaseColor:        return "colour";
    case TerrainTexRole::NormalHeight:     return "normal+height";
    case TerrainTexRole::AmbientOcclusion: return "occlusion";
    case TerrainTexRole::Opacity:          return "opacity";
    case TerrainTexRole::Smoothness:       return "smoothness";
    case TerrainTexRole::Mixed:            return "mixed";
    default:                               return "?";
    }
}

const std::string& TerrainStaticGroup::stem() const
{
    static const std::string empty;
    return tex.empty() ? empty : tex.front().stem;
}

bool TerrainStaticTable::load(Source& src, const std::string& level,
                              std::string& err, int ubershader)
{
    tex_.clear();
    groups_.clear();
    layer_join_.clear();
    join_exact_ = false;
    register_base_ = -1;
    register_base_hits_ = 0;
    register_base_runner_up_hits_ = 0;
    attributed_samples_ = 0;
    unattributed_samples_ = 0;
    bytecode_guid_.clear();
    const std::string lvl = lower(level);

    // ---- 1. the compositor program's common BindingSet ---------------------
    std::string dbname;
    for (const auto& kv : src.res())
    {
        const std::string n = lower(kv.first);
        if (n.find("shaderstate_db") != std::string::npos &&
            n.find(lvl) != std::string::npos) { dbname = kv.first; break; }
    }
    if (dbname.empty()) { err = "no shaderstate db for " + level; return false; }
    std::vector<uint8_t> db = src.get_res(dbname, err);
    if (db.size() < 12) { err = "shaderstate db: " + err; return false; }

    const uint32_t program_count = rd32(db, 0);
    const uint64_t program_ptr   = rd64(db, 4);
    uint64_t perm_id = 0;
    for (uint32_t s = 0; s < program_count; s++)
    {
        const size_t rec = (size_t)program_ptr + (size_t)s * 0xA8;
        if (rec + 0xA8 > db.size()) break;
        bool hit = false;
        for (size_t o = 0; o + 8 <= 0xA8; o += 4)
            if (rd64(db, rec + o) == kTerrainProgram) { hit = true; break; }
        if (!hit) continue;
        const uint32_t pc = rd32(db, rec + 0x60), po = rd32(db, rec + 0x64);
        if ((uint32_t)ubershader < pc) perm_id = rd64(db, po + 8ull * (uint32_t)ubershader);
        break;
    }
    if (!perm_id) { err = "no terrain compositor permutation"; return false; }

    char nm[192];
    std::snprintf(nm, sizeof(nm), "expressionshader/permutation%llu",
                  (unsigned long long)perm_id);
    std::vector<uint8_t> pr = src.get_res(nm, err);
    // 36 bytes is the COMPUTE permutation form; a raster one is longer and has
    // no common set at +0x18, so reading it would silently give a raster table.
    if (pr.size() != 36) { err = "permutation is not the compute form"; return false; }

    std::snprintf(nm, sizeof(nm), "expressionshader/permutationshareddata/%llu",
                  (unsigned long long)rd64(pr, 0));
    std::vector<uint8_t> sd = src.get_res(nm, err);
    if (sd.size() < 0x20) { err = "no permutation shared data"; return false; }
    binding_set_ = rd64(sd, 0x18);

    std::snprintf(nm, sizeof(nm), "expressionshader/bindingset/%llu",
                  (unsigned long long)binding_set_);
    std::vector<uint8_t> bs = src.get_res(nm, err);
    if (bs.empty()) { err = "no common bindingset"; return false; }

    // descriptor index -> Name32. The destination is an INDEX here, not a byte
    // offset: it steps by 1.
    std::map<uint32_t, uint32_t> desc_to_name;
    {
        const uint32_t nrec = rd32(bs, 0);
        const uint64_t rp = rd64(bs, 4);
        for (uint32_t r = 0; r < nrec; r++)
        {
            const size_t rec = (size_t)rp + (size_t)r * 0x38;
            if (rec + 0x38 > bs.size()) break;
            const uint16_t nd = rd16(bs, rec + 0x10);
            const uint64_t dp = rd64(bs, rec + 0x20), sp = rd64(bs, rec + 0x28);
            for (uint16_t i = 0; i < nd; i++)
            {
                const size_t d0 = (size_t)dp + i * 16, s0 = (size_t)sp + i * 8;
                if (d0 + 16 > bs.size() || s0 + 8 > bs.size()) break;
                desc_to_name[rd32(bs, s0)] =
                    Depot::make_name32(rd64(bs, d0), rd16(bs, d0 + 14));
            }
        }
    }
    declared_ = desc_to_name.size();
    if (desc_to_name.empty()) { err = "common bindingset declares nothing"; return false; }

    // ---- 2. the depot record carrying those Name32s ------------------------
    //
    // No guesswork needed and none taken: walk every record of the level's
    // depots and keep whichever carries the most of these names. The compositor
    // binds its OWN sheets, so one record carries all of them and nothing else
    // comes close (76 of 76 on dumbo, 61 of 61 on aftermath).
    std::set<uint32_t> want;
    for (const auto& kv : desc_to_name) want.insert(kv.second);
    if (getenv("BF6_TS_DEBUG")) {
        std::string w; int n=0;
        for (uint32_t v : want) { char b[16]; snprintf(b,sizeof b,"%08x ",v); w+=b; if(++n>=6) break; }
        fprintf(stderr, "  [ts] want(%zu) first6: %s\n", want.size(), w.c_str());
    }

    size_t best_hits = 0;
    std::vector<uint8_t> best_data;
    for (const auto& kv : src.res())
    {
        const std::string n = lower(kv.first);
        if (n.find("shaderblockdepot") == std::string::npos) continue;
        if (n.find(lvl) == std::string::npos) continue;
        std::string e;
        std::vector<uint8_t> d = src.get_res(kv.first, e);
        if (d.empty()) continue;
        Depot dep;
        if (!dep.parse(d, e)) continue;
        if (getenv("BF6_TS_DEBUG")) {
            uint64_t h = 1469598103934665603ull;
            for (uint8_t b : d) { h ^= b; h *= 1099511628211ull; }
            fprintf(stderr, "  [ts] candidate res=%s bytes=%zu records=%zu fnv=%016llx\n",
                    kv.first.c_str(), d.size(), dep.record_count(), (unsigned long long)h);
        }
        size_t dbg_ok = 0, dbg_fail = 0, dbg_params = 0;
        for (size_t r = 0; r < dep.record_count(); r++)
        {
            std::vector<DepotParam> ps;
            std::string pe;
            if (!dep.params(r, d, ps, pe)) { dbg_fail++; continue; }
            dbg_ok++; dbg_params += ps.size();
            size_t hits = 0;
            for (const DepotParam& q : ps) if (want.count(q.name32)) hits++;
            if (hits > best_hits)
            { best_hits = hits; depot_res_ = kv.first; depot_rec_ = r; }
            // DO NOT take `d` here. This used to be `best_data.swap(d)`, which
            // emptied the very buffer the loop below still reads from: the
            // FIRST record that improved the best killed the scan for the rest
            // of that depot, every later params() failing with "blob outside
            // the file". The winner then depended on the order src.res()
            // happened to enumerate candidates, which changes with what is
            // mounted - mp_tungsten scored 57/57 through a bare level mount and
            // 1/57 through the C API, off byte-identical input. Re-fetch the
            // winner after the scan instead.
        }
        if (getenv("BF6_TS_DEBUG") && dep.record_count() > 1000)
            fprintf(stderr, "  [ts] BIG depot recs=%zu parsed_ok=%zu failed=%zu params=%zu\n",
                    dep.record_count(), dbg_ok, dbg_fail, dbg_params);
    }
    if (getenv("BF6_TS_DEBUG"))
        fprintf(stderr, "  [ts] lvl=%s depots_scanned_best_hits=%zu depot=%s rec=%zu want=%zu\n",
                lvl.c_str(), best_hits, depot_res_.c_str(), depot_rec_, want.size());
    if (best_hits == 0) { err = "no depot record carries the compositor's names"; return false; }

    std::map<uint32_t, std::string> name_to_guid;
    {
        Depot dep;
        std::string e;
        std::string fe;
        best_data = src.get_res(depot_res_, fe);
        dep.parse(best_data, e);
        std::vector<DepotParam> ps;
        dep.params(depot_rec_, best_data, ps, e);
        for (const DepotParam& q : ps)
            if (!q.refs.empty()) name_to_guid[q.name32] = q.refs[0].second;
    }

    // ---- 3. descriptor -> texture ------------------------------------------
    const std::map<std::string, std::string>& pidx = src.partition_index();
    for (const auto& kv : desc_to_name)
    {
        auto g = name_to_guid.find(kv.second);
        if (g == name_to_guid.end()) continue;
        TerrainStaticTexture t;
        t.descriptor = kv.first;
        t.name32 = kv.second;
        t.file_guid = g->second;
        auto a = pidx.find(g->second);
        t.asset = a == pidx.end() ? std::string() : leaf_no_ebx(a->second);
        split_role(t.asset, t.stem, t.role);
        tex_[kv.first] = t;
    }
    if (tex_.empty()) { err = "the depot record named no textures"; return false; }

    // ---- 4. the utility prologue -------------------------------------------
    // Walk UP from descriptor 0 while the sheet is one of the compositor's own.
    prologue_top_ = -1;
    for (auto it = tex_.begin(); it != tex_.end(); ++it)
    {
        if (!is_utility(it->second.asset)) break;
        prologue_top_ = (int)it->first;
    }

    // ---- 5. group the table, descending ------------------------------------
    //
    // A base colour opens a group and takes at most the two descriptors below it
    // (the normal-height and the occlusion/opacity map). That is the shipped
    // allocation: consecutive ao / nhs / cv triples, occasionally with a hole
    // where a slot went unbound. A run with no colour in it - the lone grunge
    // smoothness sheet a modifier layer samples, the three-plane default terrain
    // sheet - is still a group, because the evaluator still spends a layer on it.
    {
        std::vector<TerrainStaticGroup> gs;
        TerrainStaticGroup cur;
        int take = 0;
        for (auto it = tex_.rbegin(); it != tex_.rend(); ++it)
        {
            const TerrainStaticTexture& t = it->second;
            const bool cv = t.role == TerrainTexRole::BaseColor;
            const bool close_run = take == 0 && !cur.tex.empty() && cur.base_color >= 0;
            if (cv || close_run)
            {
                if (!cur.tex.empty()) gs.push_back(cur);
                cur = TerrainStaticGroup();
                take = cv ? 2 : 0;
            }
            else if (take > 0) take--;
            if (cur.tex.empty()) cur.top = (int)t.descriptor;
            const int at = (int)cur.tex.size();
            cur.tex.push_back(t);
            if (t.role == TerrainTexRole::BaseColor && cur.base_color < 0) cur.base_color = at;
            else if (t.role == TerrainTexRole::NormalHeight && cur.normal_height < 0) cur.normal_height = at;
            else if (cur.third < 0 &&
                     (t.role == TerrainTexRole::AmbientOcclusion ||
                      t.role == TerrainTexRole::Opacity ||
                      t.role == TerrainTexRole::Smoothness)) cur.third = at;
        }
        if (!cur.tex.empty()) gs.push_back(cur);

        for (TerrainStaticGroup& g : gs)
        {
            g.utility = true;
            for (const TerrainStaticTexture& t : g.tex)
                if (!is_utility(t.asset)) { g.utility = false; break; }
            // Nothing at or below the prologue is ever handed to a layer.
            if (g.top <= prologue_top_) g.utility = true;
            if (!g.utility) groups_.push_back(g);
        }
    }
    // ---- 6. the layer join, recovered from this install's live DXIL --------
    TerrainShaderProgram shader;
    TerrainDxilJoin dxil;
    if (!load_terrain_shader(src, level, shader, err, ubershader)) return false;
    if (!recover_terrain_dxil_join(src.game_dir(), shader.bytecode, dxil, err)) return false;
    bytecode_guid_ = shader.bytecode_guid;
    attributed_samples_ = dxil.attributed_samples;
    unattributed_samples_ = dxil.unattributed_samples;

    // Derive the common BindingSet's register base. Every declared descriptor
    // must land on a statically declared Texture2D SRV. A unique candidate is
    // required: ambiguity is an unresolved read, never permission to assume 21.
    std::vector<int> candidates;
    int base_best_hits = -1;
    for (int base = 0; base <= 255; ++base) {
        int hits = 0;
        for (const auto& kv : desc_to_name)
            if (dxil.static_texture_registers.count(base + (int)kv.first)) hits++;
        if (hits > base_best_hits) {
            register_base_runner_up_hits_ = base_best_hits;
            base_best_hits = hits;
            candidates.assign(1, base);
        } else if (hits == base_best_hits) {
            candidates.push_back(base);
        } else if (hits > register_base_runner_up_hits_) {
            register_base_runner_up_hits_ = hits;
        }
    }
    register_base_hits_ = base_best_hits;
    if (base_best_hits <= 0 || candidates.size() != 1 ||
        base_best_hits <= register_base_runner_up_hits_) {
        int dlo = 0, dhi = -1, rlo = 0, rhi = -1;
        if (!desc_to_name.empty()) {
            dlo = (int)desc_to_name.begin()->first;
            dhi = (int)desc_to_name.rbegin()->first;
        }
        if (!dxil.static_texture_registers.empty()) {
            rlo = *dxil.static_texture_registers.begin();
            rhi = *dxil.static_texture_registers.rbegin();
        }
        std::string regs;
        for (int r : dxil.static_texture_registers) {
            if (!regs.empty()) regs += ',';
            regs += std::to_string(r);
        }
        err = "live terrain register-base control is " +
              std::string(candidates.empty() ? "empty" : "ambiguous") +
              " (" + std::to_string(candidates.size()) +
              " candidates, best/runner " + std::to_string(base_best_hits) + "/" +
              std::to_string(register_base_runner_up_hits_) + "; " +
              std::to_string(desc_to_name.size()) + " descriptors " +
              std::to_string(dlo) + ".." + std::to_string(dhi) + ", " +
              std::to_string(dxil.static_texture_registers.size()) +
              " Texture2D registers " + std::to_string(rlo) + ".." +
              std::to_string(rhi) + " [" + regs + "])";
        return false;
    }
    register_base_ = candidates.front();

    for (const auto& layer : dxil.registers_by_layer) {
        Triple t;
        bool has_common_descriptor = false;
        for (int reg : layer.second) {
            const int d = reg - register_base_;
            if (d < 0 || !desc_to_name.count((uint32_t)d)) continue;
            has_common_descriptor = true;
            auto tx = tex_.find((uint32_t)d);
            if (tx == tex_.end()) continue; // descriptor is exact; asset name unresolved
            switch (tx->second.role) {
            case TerrainTexRole::BaseColor:        if (t.cv < 0) t.cv = d; break;
            case TerrainTexRole::NormalHeight:     if (t.nh < 0) t.nh = d; break;
            case TerrainTexRole::AmbientOcclusion:
            case TerrainTexRole::Opacity:
            case TerrainTexRole::Smoothness:       if (t.third < 0) t.third = d; break;
            default: break;
            }
        }
        if (has_common_descriptor) layer_join_[layer.first] = t;
    }
    if (layer_join_.empty()) {
        err = "live terrain evaluator sampled no common BindingSet descriptors";
        return false;
    }
    join_exact_ = true;
    return true;
}

bool TerrainStaticTable::layer_descriptors(int layer, int& cv, int& nh, int& third) const
{
    cv = nh = third = -1;
    const auto it = layer_join_.find(layer);
    if (it == layer_join_.end()) return false;
    cv = it->second.cv; nh = it->second.nh; third = it->second.third;
    return true;
}

}  // namespace bf6
