#include "destruction.h"

#include "ebx.h"
#include "source.h"
#include "types.h"

#include <map>
#include <vector>

namespace bf6 {
namespace {

// The three fields the rule needs, by name hash.
const uint32_t F_PHYSICS_PART_INFOS = 0x5B95359C;
const uint32_t F_HEALTH_STATE       = 0x97C633FB;
const uint32_t F_PART_COMPONENT     = 0x0723904B;

const uint8_t MESHTYPE_SKINNED = 1;

// Answers cached per mesh resource: a map places the same prop thousands of
// times and the prop's partition would otherwise be parsed once per placement.
std::map<std::string, std::set<uint16_t> > g_cache;

int64_t as_int(const EbxValue* v)
{
    if (!v) return -1;
    switch (v->kind)
    {
    case EbxValue::Kind::Int:  return v->i;
    case EbxValue::Kind::Uint: return (int64_t)v->u;
    case EbxValue::Kind::Bool: return v->b ? 1 : 0;
    default: return -1;
    }
}

}  // namespace

std::set<uint16_t> destruction_hidden_parts(Source& src, TypeDb& types,
                                            const std::string& mesh_res)
{
    std::map<std::string, std::set<uint16_t> >::const_iterator c = g_cache.find(mesh_res);
    if (c != g_cache.end()) return c->second;

    std::set<uint16_t> out;

    // `X_mesh` -> `X`. A mesh whose name does not carry the suffix IS its own
    // prop, which is the same fallback the resource resolver uses in the other
    // direction.
    std::string prop = mesh_res;
    if (prop.size() > 5 && prop.compare(prop.size() - 5, 5, "_mesh") == 0)
        prop.resize(prop.size() - 5);

    std::string err;
    std::vector<uint8_t> raw = src.get_ebx(prop + ".ebx", err);
    if (raw.empty()) raw = src.get_ebx(prop, err);
    if (raw.empty()) { g_cache[mesh_res] = out; return out; }

    Ebx e(types);
    if (!e.parse(std::move(raw), err)) { g_cache[mesh_res] = out; return out; }

    const std::vector<uint32_t> want(1, F_PHYSICS_PART_INFOS);
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        EbxValue inst = e.read_instance(i, &want);
        const EbxValue* tbl = inst.field(F_PHYSICS_PART_INFOS);
        if (!tbl || tbl->kind != EbxValue::Kind::Array || tbl->items.empty()) continue;

        // Pass one: which components have an INTACT row. Without this the rule
        // would cull every damaged-looking part, including the ones whose
        // damaged look is the authored look and which have no intact twin.
        std::set<int64_t> intact;
        for (size_t r = 0; r < tbl->items.size(); r++)
        {
            const EbxValue& row = tbl->items[r];
            if (row.kind != EbxValue::Kind::Struct) continue;
            if (as_int(row.field(F_HEALTH_STATE)) == 0)
                intact.insert(as_int(row.field(F_PART_COMPONENT)));
        }

        // Pass two: a damaged row whose component also has an intact one is a
        // twin, and the twin is what the game hides until the piece breaks.
        //
        // THE ROW INDEX IS THE PART INDEX. It is what the per-vertex
        // BoneIndices element stores, so the two line up positionally and
        // neither is a lookup into the other.
        for (size_t r = 0; r < tbl->items.size(); r++)
        {
            const EbxValue& row = tbl->items[r];
            if (row.kind != EbxValue::Kind::Struct) continue;
            const int64_t hs = as_int(row.field(F_HEALTH_STATE));
            if (hs != 0 && hs != -1 && intact.count(as_int(row.field(F_PART_COMPONENT))))
                out.insert((uint16_t)r);
        }
        break;   // one table per prop
    }

    g_cache[mesh_res] = out;
    return out;
}

}  // namespace bf6
