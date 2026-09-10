#include "terrainshader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "source.h"
#include "terrainlayers.h"

namespace bf6 {
namespace {

template <typename T> T rd(const std::vector<uint8_t>& d, size_t o)
{
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

std::string lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string guid_net(const std::vector<uint8_t>& d, size_t o)
{
    if (o + 16 > d.size()) return std::string();
    char b[64];
    std::snprintf(b, sizeof(b),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        d[o+3], d[o+2], d[o+1], d[o+0], d[o+5], d[o+4], d[o+7], d[o+6],
        d[o+8], d[o+9], d[o+10], d[o+11], d[o+12], d[o+13], d[o+14], d[o+15]);
    return b;
}

bool bindings(Source& src, uint64_t id,
              std::vector<TerrainShaderBindingRecord>& out, std::string& err)
{
    out.clear();
    if (!id) return true;
    char n[128];
    std::snprintf(n, sizeof(n), "expressionshader/bindingset/%llu",
                  (unsigned long long)id);
    std::vector<uint8_t> d = src.get_res(n, err);
    if (d.empty()) { err = "no BindingSet resource " + std::string(n); return false; }
    const uint32_t count = rd<uint32_t>(d, 0);
    const uint64_t ptr = rd<uint64_t>(d, 4);
    if (!count || count > 4096 || ptr >= d.size())
    { err = "invalid BindingSet record table in " + std::string(n); return false; }

    for (uint32_t r = 0; r < count; ++r) {
        const size_t at = (size_t)ptr + (size_t)r * 0x38;
        if (at + 0x38 > d.size())
        { err = "truncated BindingSet record in " + std::string(n); return false; }
        TerrainShaderBindingRecord rec;
        rec.destination_span = rd<uint32_t>(d, at + 0x08);
        const uint16_t nd = rd<uint16_t>(d, at + 0x10);
        const uint64_t dp = rd<uint64_t>(d, at + 0x20);
        const uint64_t sp = rd<uint64_t>(d, at + 0x28);
        rec.declarations.reserve(nd);
        for (uint16_t i = 0; i < nd; ++i) {
            const size_t da = (size_t)dp + (size_t)i * 16;
            const size_t sa = (size_t)sp + (size_t)i * 8;
            if (da + 16 > d.size() || sa + 8 > d.size())
            { err = "truncated BindingSet declaration in " + std::string(n); return false; }
            TerrainShaderDecl x;
            x.param_hash = rd<uint64_t>(d, da);
            x.type_hash = rd<uint32_t>(d, da + 8);
            x.flags = rd<uint16_t>(d, da + 12);
            x.name_hi = rd<uint16_t>(d, da + 14);
            x.destination = rd<uint32_t>(d, sa);
            x.meta = rd<uint16_t>(d, sa + 6);
            rec.declarations.push_back(x);
        }
        out.push_back(std::move(rec));
    }
    return true;
}

} // namespace

bool load_raster_bindings(Source& src, uint64_t permutation,
    std::vector<TerrainShaderBindingRecord>& records, std::string& err)
{
    records.clear();
    const auto pr = src.get_res("expressionshader/permutation" + std::to_string(permutation), err);
    if (pr.size() != 68) { err = "raster permutation is absent or has no pixel stage"; return false; }
    const auto shared = src.get_res("expressionshader/permutationshareddata/" +
        std::to_string(rd<uint64_t>(pr, 0)), err);
    if (shared.size() < 0x48) return false;
    for (size_t at : {size_t(0x18), size_t(0x20)}) {
        std::vector<TerrainShaderBindingRecord> batch;
        if (!bindings(src, rd<uint64_t>(shared, at), batch, err)) return false;
        records.insert(records.end(), batch.begin(), batch.end());
    }
    return true;
}

uint32_t TerrainShaderProgram::layer_row_stride() const
{
    uint32_t best = 0;
    for (const TerrainShaderBindingRecord& r : layer_records)
        best = std::max(best, r.destination_span);
    return best;
}

bool load_terrain_shader(Source& src, const std::string& level,
                         TerrainShaderProgram& out, std::string& err,
                         int ubershader)
{
    out = TerrainShaderProgram();
    err.clear();
    const std::string want = lower(level);
    std::string dbname;
    for (const auto& kv : src.res()) {
        const std::string n = lower(kv.first);
        if (n.find("shaderstate_db") != std::string::npos &&
            n.find(want) != std::string::npos) { dbname = kv.first; break; }
    }
    if (dbname.empty()) { err = "no shaderstate database for " + level; return false; }
    std::vector<uint8_t> db = src.get_res(dbname, err);
    if (db.size() < 12) { err = "terrain shaderstate database is unreadable"; return false; }

    const uint32_t programs = rd<uint32_t>(db, 0);
    const uint64_t pp = rd<uint64_t>(db, 4);
    constexpr uint64_t kTerrainProgram = 0x9F11D96B0FDF4773ull;
    for (uint32_t i = 0; i < programs; ++i) {
        const size_t at = (size_t)pp + (size_t)i * 0xA8;
        if (at + 0xA8 > db.size()) break;
        bool terrain = false;
        for (size_t o = 0; o + 8 <= 0xA8; o += 4)
            if (rd<uint64_t>(db, at + o) == kTerrainProgram) { terrain = true; break; }
        if (!terrain) continue;
        const uint32_t count = rd<uint32_t>(db, at + 0x60);
        const uint32_t poff = rd<uint32_t>(db, at + 0x64);
        if (ubershader < 0 || (uint32_t)ubershader >= count ||
            (size_t)poff + ((size_t)ubershader + 1) * 8 > db.size())
        { err = "terrain ubershader index is outside the live permutation list"; return false; }
        out.permutation_id = rd<uint64_t>(db, (size_t)poff + (size_t)ubershader * 8);
        break;
    }
    if (!out.permutation_id) { err = "terrain compositor program not found"; return false; }

    char pn[128];
    std::snprintf(pn, sizeof(pn), "expressionshader/permutation%llu",
                  (unsigned long long)out.permutation_id);
    std::vector<uint8_t> pr = src.get_res(pn, err);
    if (pr.size() != 36) { err = "terrain permutation is not the 36-byte compute form"; return false; }
    out.shared_data_id = rd<uint64_t>(pr, 0);
    out.bytecode_guid = guid_net(pr, 0x10);

    char sn[128];
    std::snprintf(sn, sizeof(sn), "expressionshader/permutationshareddata/%llu",
                  (unsigned long long)out.shared_data_id);
    std::vector<uint8_t> shared = src.get_res(sn, err);
    if (shared.size() < 0x30) { err = "terrain permutation shared data is truncated"; return false; }
    out.common_binding_set = rd<uint64_t>(shared, 0x18);
    out.variant_binding_set = rd<uint64_t>(shared, 0x20);
    out.layer_binding_set = rd<uint64_t>(shared, 0x28);
    if (!bindings(src, out.common_binding_set, out.common_records, err) ||
        !bindings(src, out.variant_binding_set, out.variant_records, err) ||
        !bindings(src, out.layer_binding_set, out.layer_records, err)) return false;

    const std::string needle = lower(out.bytecode_guid);
    std::vector<uint8_t> wrapped;
    for (const auto& kv : src.res()) {
        const std::string n = lower(kv.first);
        if (n.find("bytecode") == std::string::npos || n.find(needle) == std::string::npos)
            continue;
        wrapped = src.get_res(kv.first, err);
        if (!wrapped.empty()) { out.bytecode_resource = kv.first; break; }
    }
    if (wrapped.empty()) { err = "live terrain CompiledBytecode " + out.bytecode_guid + " not found"; return false; }
    size_t dxbc = std::string::npos;
    for (size_t o = 0; o + 4 <= wrapped.size() && o < 8192; ++o)
        if (!std::memcmp(wrapped.data() + o, "DXBC", 4)) { dxbc = o; break; }
    if (dxbc == std::string::npos || dxbc + 0x20 > wrapped.size())
    { err = "live terrain CompiledBytecode has no DXBC/DXIL container"; return false; }
    const uint32_t bytes = rd<uint32_t>(wrapped, dxbc + 0x18);
    if (bytes < 0x20 || dxbc + bytes > wrapped.size())
    { err = "live terrain DXBC/DXIL container size is invalid"; return false; }
    out.bytecode.assign(wrapped.begin() + dxbc, wrapped.begin() + dxbc + bytes);
    return true;
}

bool build_terrain_layer_rows(const TerrainShaderProgram& program,
                              const TerrainLayers& layers,
                              const std::map<std::string, uint32_t>& descriptors,
                              std::vector<uint8_t>& rows,
                              TerrainRowBuildStats& stats,
                              std::string& err)
{
    rows.clear(); stats = TerrainRowBuildStats(); err.clear();
    const uint32_t stride = program.layer_row_stride();
    if (!stride || (stride & 3u)) { err = "invalid live terrain layer-row stride"; return false; }
    const TerrainShaderBindingRecord* schema = nullptr;
    for (const TerrainShaderBindingRecord& r : program.layer_records)
        if (!schema || r.destination_span > schema->destination_span) schema = &r;
    if (!schema) { err = "live terrain LayerInfo BindingSet has no records"; return false; }

    uint32_t nrows = 0;
    for (const TerrainLayer& l : layers.layers()) nrows = std::max(nrows, l.index + 1);
    if (!nrows) { err = "terrain palette contains no layer rows"; return false; }
    rows.assign((size_t)nrows * stride, 0);
    stats.rows = nrows;

    auto default_bytes = [](uint32_t name, uint32_t type, uint8_t out[16], size_t& n) -> bool {
        std::memset(out, 0, 16); n = 4;
        float one = 1.f;
        switch (name) {
        case tl::kCOverlayStrength:
        case tl::kCMaskRampExp:
        case tl::kCUvTiling:
        case tl::kCCoordScaleX:
        case tl::kCCoordScaleY:
            std::memcpy(out, &one, 4); return true;
        case tl::kCTint:
            std::memcpy(out + 0, &one, 4); std::memcpy(out + 4, &one, 4);
            std::memcpy(out + 8, &one, 4); n = 12; return true;
        default:
            // Zero is the neutral value for the remaining decoded scalar
            // fields. Unknown declarations are left zero but are not counted
            // as a known default: their true schema default remains open.
            if (name == tl::kCUvOffset || name == tl::kCDisplaceRange ||
                name == tl::kCUvRotationDeg || name == tl::kCBaseHeight ||
                name == tl::kCHeightBlend || name == tl::kCSurfaceClass) {
                n = type == 0x39ab6941 ? 8 : 4;
                return true;
            }
            return false;
        }
    };

    for (const TerrainLayer& layer : layers.layers()) {
        if (layer.index >= nrows) continue;
        uint8_t* dst = rows.data() + (size_t)layer.index * stride;
        for (const TerrainShaderDecl& decl : schema->declarations) {
            if (decl.destination >= stride) continue;
            const uint32_t name = decl.name32();
            if (decl.type_hash == 0xcc84d53d) {
                auto ti = layer.material.raw_textures.find(name);
                if (ti == layer.material.raw_textures.end()) continue;
                auto di = descriptors.find(ti->second);
                if (di == descriptors.end()) { stats.missing_textures++; continue; }
                std::memcpy(dst + decl.destination, &di->second, 4);
                stats.bound_textures++;
                continue;
            }
            auto ci = layer.material.raw_constants.find(name);
            if (ci != layer.material.raw_constants.end()) {
                const size_t n = std::min<size_t>(ci->second.size(), stride - decl.destination);
                if (n) std::memcpy(dst + decl.destination, ci->second.data(), n);
                stats.authored_values++;
                continue;
            }
            uint8_t value[16]; size_t n = 0;
            if (default_bytes(name, decl.type_hash, value, n)) {
                n = std::min<size_t>(n, stride - decl.destination);
                std::memcpy(dst + decl.destination, value, n);
                stats.defaulted_values++;
            }
        }
    }
    return true;
}

} // namespace bf6
