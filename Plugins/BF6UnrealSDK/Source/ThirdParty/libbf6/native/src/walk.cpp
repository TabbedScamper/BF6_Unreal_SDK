#include "walk.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bf6 {
namespace {

// Field name hashes the traversal reads. Named the way the GDScript names them
// so the two can be diffed by eye.
constexpr uint32_t F_OBJECTS             = 0x5A6115B2;
constexpr uint32_t F_DATAREFS            = 0xCDA4739B;
constexpr uint32_t F_SUBWORLD_COMPONENTS = 0x5E00055A;
constexpr uint32_t F_BUNDLENAME          = 0x095E926D;
constexpr uint32_t F_BP_TRANSFORM        = 0x7B554EF5;
constexpr uint32_t F_BLUEPRINT           = 0x6B831B88;
constexpr uint32_t F_EXCLUDED            = 0x4CD269A8;
constexpr uint32_t F_SMG_MEMBERS         = 0xA43BD992;
constexpr uint32_t F_SMG_XFORMS          = 0xEBF4D386;
constexpr uint32_t F_SMG_BLUEPRINT       = 0xB608BEEE;
constexpr uint32_t F_SMG_MESHASSET       = 0x1B4A547C;
constexpr uint32_t F_SMG_ENABLED         = 0xFB410D18;
constexpr uint32_t F_SMG_VISIBLE         = 0x8F8D9F78;
constexpr uint32_t F_SMG_OBJVAR          = 0xE2FFCC45;
constexpr uint32_t F_BP_OBJVAR           = 0x4B8B8E51;
constexpr uint32_t F_TRANSFORM           = 0xD6351EDE;

// LinearTransform members, and the Vec3 components inside each.
constexpr uint32_t F_LT_RIGHT   = 0xC478CC3B;
constexpr uint32_t F_LT_UP      = 0xBF151EF9;
constexpr uint32_t F_LT_FORWARD = 0x695D12A4;
constexpr uint32_t F_LT_TRANS   = 0xBC4B07B4;
constexpr uint32_t K_VEC_X = 956422932, K_VEC_Y = 1123815262, K_VEC_Z = 849976220;

const char* kLtGuid = "06ce1d10-9a4e-fc64-2a9f-a9d482576ffa";

// EVERY field visit() looks at. A type declaring none of them provably cannot
// affect the walk, which is what makes the skip an equivalence rather than a
// heuristic. This list and visit() are kept in step BY HAND; nothing can check
// it automatically, so a field dropped here silently loses geometry.
const uint32_t kWalkFields[] = {
    F_SMG_MEMBERS, F_SMG_XFORMS, F_SMG_BLUEPRINT, F_SMG_MESHASSET,
    F_SMG_ENABLED, F_SMG_VISIBLE, F_SMG_OBJVAR,
    F_BP_TRANSFORM, F_BP_OBJVAR, F_EXCLUDED, F_BUNDLENAME, F_BLUEPRINT,
    F_OBJECTS, F_DATAREFS, F_SUBWORLD_COMPONENTS
};

// Types whose subtree is an alternate reality: a destroyed state, or the effect
// of becoming one. Walking in emits wreckage as scenery AND suppresses the leaf
// row that would have placed the intact prop.
// We used to walk straight through them, and it cost geometry twice over.
// lf_com_streetlight_02 is the clean example: its only Blueprint references are
// four destruction effects and one transition spawn (the wreck). So the walk
// emitted the destroyed twin and the break effects as scenery - and because
// those rows made the target look productive, the leaf fallback that would have
// emitted the intact PREFAB never fired. The 1,406-triangle street light was
// never placed at all, 395 times.
const char* kDestructionBranch[] = {
    "464ab605-1fb8-3d21-202f-f3feb43dffb9",   // DestructionEffectReferenceObjectData
    "591bcc82-5380-7cbd-543e-faabbc873c09",   // ...Template
    "ef0c40ad-226b-f6fe-04dd-7b26c26350a2",   // DestructionTransitionSpawnReferenceObjectData
    "96e64a4c-c7e1-644c-dedb-499e51d965c2",   // ...Template
    "570f0b4f-fcd0-d977-580e-d409f464c6f1",   // DestructionProcess
    "e788769d-af38-c1e6-d413-10cffb99e5f8",   // DestructionProcessData
    "643b508d-7f3e-af14-3022-033b9ced8d79",   // Class_0a6b45b4
    "b3120863-e2a0-5eb4-4523-a3f4dc814e20",   // Class_a8cf23c0
    "a0e4f0a2-5007-928a-20de-1f613062529c",   // EdgeModelsBaseData
    "0f86bea6-d792-51ab-8d91-b724ea871296",   // EdgeModelsData
};

// Subworld paths that are not part of a playable map.
const char* kSkipSubworld[] = { "_autotests", "_tools", "marketing" };

constexpr int kMaxDepth = 24;

std::string lower(std::string s)
{
    for (char& c : s)
    {
        if (c == '\\') c = '/';
        else c = (char)std::tolower((unsigned char)c);
    }
    return s;
}

std::string leaf_of(const std::string& s)
{
    const size_t i = s.find_last_of('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}

bool ends_with(const std::string& s, const std::string& t)
{
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

bool contains(const std::string& s, const std::string& t)
{
    return s.find(t) != std::string::npos;
}

TypeGuid raw_guid(const std::string& dashed)
{
    // The inverse of TypeDb::guid_str, so a guid written the way the rest of
    // this file writes them can be compared against raw type bytes without
    // formatting a string per instance.
    TypeGuid g{};
    std::string h;
    for (char c : dashed) if (c != '-') h += c;
    if (h.size() != 32) return g;
    auto byte = [&](size_t i) { return (uint8_t)std::stoul(h.substr(i * 2, 2), nullptr, 16); };
    g[0] = byte(3); g[1] = byte(2); g[2] = byte(1); g[3] = byte(0);
    g[4] = byte(5); g[5] = byte(4);
    g[6] = byte(7); g[7] = byte(6);
    for (int i = 8; i < 16; i++) g[(size_t)i] = byte((size_t)i);
    return g;
}

const TypeGuid& lt_guid()
{
    static const TypeGuid g = raw_guid(kLtGuid);
    return g;
}

bool is_lt(const EbxValue* v)
{
    return v && v->kind == EbxValue::Kind::Struct && v->guid == lt_guid();
}

Vec3 vec_of(const EbxValue* d)
{
    Vec3 out;
    if (!d || d->kind != EbxValue::Kind::Struct) return out;
    auto get = [&](uint32_t h) -> float
    {
        const EbxValue* f = d->field(h);
        if (!f) return 0.0f;
        if (f->kind == EbxValue::Kind::Real) return (float)f->f;
        if (f->kind == EbxValue::Kind::Int)  return (float)f->i;
        if (f->kind == EbxValue::Kind::Uint) return (float)f->u;
        return 0.0f;
    };
    out.x = get(K_VEC_X);
    out.y = get(K_VEC_Y);
    out.z = get(K_VEC_Z);
    return out;
}

Mat34 lt_to_mat(const EbxValue* t)
{
    static const uint32_t members[4] = { F_LT_RIGHT, F_LT_UP, F_LT_FORWARD, F_LT_TRANS };
    Mat34 out = mat_identity();
    if (!t || t->kind != EbxValue::Kind::Struct) return out;
    Mat34 built;
    for (int i = 0; i < 4; i++)
    {
        const EbxValue* m = t->field(members[i]);
        if (!m) return out;                 // an incomplete transform is identity
        built.m[i] = vec_of(m);
    }
    return built;
}

// A pointer ref's resolved path, or "" when it points nowhere useful.
std::string ref_path(const EbxValue* v)
{
    if (!v || v->kind != EbxValue::Kind::ImportRef) return "";
    if (v->import_path.empty() || v->import_path == "<not indexed>") return "";
    return v->import_path;
}

// THE PACKED-BOOL UNPACK, and why an array of `true` read as garbage.
//
// InstanceEnabled and Visible are FLAT BYTE arrays, but the reflection carries
// no element type for an array of a primitive: the field's flags say
// category=Array/type=Array for every array alike and elemVA is 0. With nothing
// to dispatch on, the deserializer falls back to a 4-byte stride, so element i
// is read from elem + i*4 and the read runs four times past the end.
//
// The damage was quiet rather than loud. On mp_dumbo, Visible came back as
// [16843009, ...] - 0x01010101, four `true` bytes packed into one int -
// followed by whatever bytes followed the array, and 5,762 SMG instances were
// judged not-visible from that garbage and dropped. Building facades were among
// them, which is how rooms ended up with their windows and fittings but no
// walls. The count in the array header IS correct, so the first ceil(n/4)
// values hold all n real bytes. Unpack them back.
std::vector<bool> flat_bools(const EbxValue* arr)
{
    std::vector<bool> out;
    if (!arr || arr->kind != EbxValue::Kind::Array || arr->items.empty()) return out;

    bool all_bool = true;
    for (const EbxValue& v : arr->items)
        if (v.kind != EbxValue::Kind::Bool) { all_bool = false; break; }
    if (all_bool)
    {
        for (const EbxValue& v : arr->items) out.push_back(v.b);
        return out;
    }

    const size_t n = arr->items.size();
    for (size_t i = 0; i < (n + 3) / 4; i++)
    {
        const EbxValue& v = arr->items[i];
        if (v.kind != EbxValue::Kind::Uint && v.kind != EbxValue::Kind::Int)
        {
            // Not the packed form; leave it alone rather than invent bits.
            out.clear();
            for (const EbxValue& w : arr->items) out.push_back(w.kind == EbxValue::Kind::Bool ? w.b : true);
            return out;
        }
        const uint64_t raw = v.kind == EbxValue::Kind::Uint ? v.u : (uint64_t)v.i;
        for (int k = 0; k < 4; k++) out.push_back(((raw >> (8 * k)) & 0xFF) != 0);
    }
    out.resize(n);
    return out;
}

// Missing entries default to VISIBLE.
bool visible_at(const std::vector<bool>& enabled, const std::vector<bool>& visible, size_t i)
{
    if (i < enabled.size() && !enabled[i]) return false;
    if (i < visible.size() && !visible[i]) return false;
    return true;
}

}  // namespace

Mat34 mat_identity()
{
    Mat34 m;
    m.m[0] = { 1, 0, 0 };
    m.m[1] = { 0, 1, 0 };
    m.m[2] = { 0, 0, 1 };
    m.m[3] = { 0, 0, 0 };
    return m;
}

// a * b, column-basis: rows 0..2 are the basis, row 3 the translation.
//
// ACCUMULATED IN DOUBLE, STORED AS FLOAT, which is not a style choice. GDScript
// has one float type and it is a double, so reading a Vector3's component
// widens it and the whole multiply-add runs in double before the result is
// stored back into a float32 Vector3. Doing the arithmetic in float here
// instead disagreed with the plugin on the sixth significant digit of 175 of
// mp_dumbo's 48,126 rows: same placements, different last bit. Composition runs
// several levels deep on a prefab inside a subworld, so the error compounds.
Mat34 mat_mul(const Mat34& a, const Mat34& b)
{
    // The basis rows and the translation row are written out separately rather
    // than sharing a lambda with a "+ 0.0 when not translating" term. Adding a
    // positive zero turns a NEGATIVE zero result positive, and the plugin
    // records -0 where the arithmetic produced one: 52 of Dumbo's rows differed
    // by nothing but the sign of a zero until this was split.
    Mat34 o;
    for (int i = 0; i < 3; i++)
    {
        const double x = b.m[i].x, y = b.m[i].y, z = b.m[i].z;
        o.m[i].x = (float)(x * a.m[0].x + y * a.m[1].x + z * a.m[2].x);
        o.m[i].y = (float)(x * a.m[0].y + y * a.m[1].y + z * a.m[2].y);
        o.m[i].z = (float)(x * a.m[0].z + y * a.m[1].z + z * a.m[2].z);
    }
    {
        const double x = b.m[3].x, y = b.m[3].y, z = b.m[3].z;
        o.m[3].x = (float)(x * a.m[0].x + y * a.m[1].x + z * a.m[2].x + a.m[3].x);
        o.m[3].y = (float)(x * a.m[0].y + y * a.m[1].y + z * a.m[2].y + a.m[3].y);
        o.m[3].z = (float)(x * a.m[0].z + y * a.m[1].z + z * a.m[2].z + a.m[3].z);
    }
    return o;
}

void Walk::build_catalog(bool bounded_frontend)
{
    bounded_frontend_ = bounded_frontend;
    by_name_.clear();
    scope_index.clear();
    // A depot is named after the bundle ASSET whose subtree it covers.  Use
    // those exact asset paths as graph-scope transitions: entering such a
    // partition changes material scope, while entering an ordinary prefab
    // must keep the enclosing layer/subworld scope.  The latter distinction
    // is what prevents a placed prop from resolving against the bundle that
    // merely stores its prefab EBX.
    for (const auto& kv : src_.depots_by_bundle())
    {
        std::string asset = lower(kv.first);
        if (asset.rfind("win32/", 0) == 0) asset.erase(0, 6);
        if (src_.ebx().find(asset) != src_.ebx().end())
            scope_index.emplace(asset, kv.first);
    }
    // Exact front-end asset walks always start from a full mounted path, and
    // their BundleName references are full paths as well. Source::ebx is
    // already that exact lower-case name index; duplicating every name plus a
    // leaf alias into a second hash map dominated 101-row actor walks. Bare
    // playable-level lookup retains the legacy alias catalogue.
    if (!bounded_frontend)
        for (const auto& kv : src_.ebx())
        {
            const std::string low = lower(kv.first);
            const std::string ref = kv.first + ".ebx";
            by_name_.emplace(low, ref);
            by_name_.emplace(leaf_of(low), ref);
        }
    gi_ = bounded_frontend ? &src_.armory_partition_index()
                           : &src_.partition_index();
}

std::string Walk::resolve_name(const std::string& name) const
{
    if (name.empty()) return "";
    std::string n = lower(name);
    if (ends_with(n, ".ebx")) n.resize(n.size() - 4);
    const auto exact = src_.ebx().find(n);
    if (exact != src_.ebx().end()) return exact->first + ".ebx";
    auto it = by_name_.find(n);
    if (it != by_name_.end()) return it->second;
    it = by_name_.find(leaf_of(n));
    if (it != by_name_.end()) return it->second;
    it = by_name_.find(n + ".ebx");
    if (it != by_name_.end()) return it->second;
    return "";
}

bool Walk::type_matters(Ebx& e, size_t i)
{
    const TypeGuid tb = e.instance_type(i);
    bool all_zero = true;
    for (uint8_t b : tb) if (b) { all_zero = false; break; }
    if (all_zero) return false;                 // no type, nothing to decode

    auto it = matters_.find(tb);
    if (it != matters_.end()) return it->second;

    // Resolved ONCE per type across the whole map, not once per instance: a few
    // thousand types against a quarter of a million instances.
    const TypeLayout& lay = types_.layout_full(tb);
    bool yes = false;
    if (!lay.valid)
    {
        // AN UNKNOWN TYPE IS NOT A BORING ONE, and this is where a user's map
        // became empty. The skip is sound only as an equivalence: a type that
        // DECLARES none of the walk fields provably cannot affect the walk. An
        // empty layout is not that statement. It means the type database could
        // not describe this type at all, and reading "I do not know" as "no"
        // means every instance is provably-skippable, the walk descends into
        // nothing, and the map comes back with zero objects while the terrain
        // and textures are perfect. It fails open instead.
        n_unresolved++;
        yes = true;
    }
    else
    {
        for (const FieldInfo& f : lay.fields)
        {
            for (uint32_t w : kWalkFields)
                if (f.name_hash == w) { yes = true; break; }
            if (yes) break;
        }
    }
    matters_[tb] = yes;
    return yes;
}

void Walk::emit_smg(const EbxValue& inst, const Mat34& parent, const std::string& src_ref)
{
    const EbxValue* members = inst.field(F_SMG_MEMBERS);
    if (!members || members->kind != EbxValue::Kind::Array) return;

    for (const EbxValue& md : members->items)
    {
        if (md.kind != EbxValue::Kind::Struct) continue;

        const EbxValue* bp = md.field(F_SMG_BLUEPRINT);
        if (!bp || ref_path(bp).empty()) bp = md.field(F_SMG_MESHASSET);
        const std::string path = ref_path(bp);
        if (path.empty()) { n_smg_unresolved++; continue; }

        const EbxValue* xf = md.field(F_SMG_XFORMS);
        const std::vector<bool> enabled = flat_bools(md.field(F_SMG_ENABLED));
        const std::vector<bool> visible = flat_bools(md.field(F_SMG_VISIBLE));
        // PER-INSTANCE ObjectVariation. On Dumbo 158 of 230 vehicle members
        // carry one and it varies WITHIN a member, so the mesh a placement
        // wants is (name, variation), not name alone.
        const EbxValue* ovar = md.field(F_SMG_OBJVAR);
        auto var_of = [&](size_t i) -> std::string
        {
            // A truncated array means trailing defaults, i.e. no variation.
            if (!ovar || ovar->kind != EbxValue::Kind::Array || i >= ovar->items.size()) return "";
            return ref_path(&ovar->items[i]);
        };

        if (!xf || xf->kind != EbxValue::Kind::Array || xf->items.empty())
        {
            // no instance transforms: one row at the bare parent, if visible
            if (visible_at(enabled, visible, 0))
                rows_.push_back({ path, parent, src_ref, "smg0", var_of(0),
                                  scope_, scope_.empty() ? bundle_ : scope_ });
            continue;
        }
        for (size_t i = 0; i < xf->items.size(); i++)
        {
            if (!is_lt(&xf->items[i])) continue;
            if (!visible_at(enabled, visible, i)) { n_smg_hidden++; continue; }
            rows_.push_back({ path, mat_mul(parent, lt_to_mat(&xf->items[i])),
                              src_ref, "smg", var_of(i), scope_,
                              scope_.empty() ? bundle_ : scope_ });
        }
    }
}

void Walk::visit(const EbxValue& inst, const Mat34& parent, const std::string& ref,
                 const std::set<std::string>& guard, int depth)
{
    // Destruction state branch: stop. Everything below is an alternate or an
    // effect, and emitting it both ships wreckage as scenery AND suppresses the
    // leaf row that would have placed the intact prop.
    {
        const std::string t = TypeDb::guid_str(inst.guid);
        for (const char* d : kDestructionBranch)
            if (t == d) { n_destruction++; return; }
    }

    // A StaticModelGroup anywhere, including directly in the level root, which
    // is where backdrop and vista instancing lives.
    if (const EbxValue* mem = inst.field(F_SMG_MEMBERS))
        if (mem->kind == EbxValue::Kind::Array)
        {
            emit_smg(inst, parent, ref);
            n_smg++;
            return;
        }

    const EbxValue* local = inst.field(F_BP_TRANSFORM);
    const Mat34 world = is_lt(local) ? mat_mul(parent, lt_to_mat(local)) : parent;

    if (const EbxValue* ex = inst.field(F_EXCLUDED))
        if ((ex->kind == EbxValue::Kind::Bool && ex->b) ||
            (ex->kind == EbxValue::Kind::Uint && ex->u) ||
            (ex->kind == EbxValue::Kind::Int && ex->i))
        { n_excluded++; return; }

    // Subworld reference: the BundleName string names the subworld asset. This
    // is the one point where the graph depends on the catalogue rather than on
    // PointerRefs.
    if (const EbxValue* bn = inst.field(F_BUNDLENAME))
        if (bn->kind == EbxValue::Kind::Str && !bn->s.empty())
        {
            const std::string low = lower(bn->s);
            for (const char* s : kSkipSubworld)
                if (contains(low, s)) { n_subworld_skipped++; return; }
            if (!include_frontend_ && contains(low, "glacierflow/"))
            { n_subworld_skipped++; return; }
            const std::string sub = resolve_name(bn->s);
            if (!sub.empty()) { n_subworld++; walk_ref(sub, world, guard, depth + 1); }
            else n_subworld_unresolved++;
            return;
        }

    // A placement pointing at another partition: recurse with the composed
    // transform.
    const std::string tgt = ref_path(inst.field(F_BLUEPRINT));
    const std::string variation = ref_path(inst.field(F_BP_OBJVAR));
    if (!tgt.empty())
    {
        if (ends_with(lower(tgt), ".ebx"))
        {
            // RECURSE, BUT DO NOT LET A TARGET THAT YIELDS NOTHING VANISH.
            // Only SMG members ever emitted a row, so a placement pointing at a
            // blueprint whose geometry is a plain mesh asset was walked and
            // silently forgotten: on Dumbo, 1,381 of 1,407 target partitions
            // produced zero rows across 26,537 visits, and among them are
            // nongroupable_autogen building parts, debris piles and street
            // lights - real geometry, individually placed rather than
            // instanced. Emit those as leaf rows and let mesh resolution
            // decide; an FX graph simply fails to resolve and is reported as a
            // drop, which is visible rather than silent.
            const size_t before = rows_.size();
            walk_ref(tgt, world, guard, depth + 1);
            // Ordinary blueprint placements carry ObjectVariation too, not
            // only StaticModelGroup rows. Keep more-specific child overrides.
            for (size_t row = before; row < rows_.size(); ++row)
                if (rows_[row].var.empty()) rows_[row].var = variation;
            if (rows_.size() == before)
            {
                rows_.push_back({ tgt, world, ref, "leaf", variation, scope_,
                                  scope_.empty() ? bundle_ : scope_ });
                n_leaf++;
            }
        }
        else rows_.push_back({ tgt, world, ref, "ref", variation, scope_,
                               scope_.empty() ? bundle_ : scope_ });
        return;
    }

    for (uint32_t f : { F_OBJECTS, F_DATAREFS, F_SUBWORLD_COMPONENTS })
        if (const EbxValue* arr = inst.field(f))
            if (arr->kind == EbxValue::Kind::Array)
                for (const EbxValue& child : arr->items)
                    if (child.kind == EbxValue::Kind::Struct)
                        visit(child, world, ref, guard, depth);
}

void Walk::walk_ref(const std::string& ref, const Mat34& parent,
                    std::set<std::string> guard, int depth)
{
    if (depth > kMaxDepth || ref.empty()) return;
    const std::string key = lower(ref);
    if (guard.count(key)) { n_cycles++; return; }

    std::string name = ref;
    if (ends_with(lower(name), ".ebx")) name.resize(name.size() - 4);
    std::string err;
    std::vector<uint8_t> data = src_.get_ebx(name, err);
    if (data.empty()) { n_missing++; return; }

    Ebx e(types_);
    e.set_guid_index(gi_);
    if (!e.parse(std::move(data), err)) { n_parse_fail++; return; }

    guard.insert(key);
    n_partitions++;

    // THE SCOPE A PLACEMENT INHERITS, tracked down the recursion.
    //
    // A section's shader state key is looked up in a ShaderBlockDepot, and a
    // key is only unique within a scope. The obvious reading, the depot beside
    // the partition holding the placement, is wrong and measurably so: a prop's
    // src is the PREFAB it sits in, somewhere under props, while its depot
    // belongs to the SUBWORLD that mounted the prefab. Those have no path
    // relationship at all. On Dumbo, matching by directory ancestry resolved
    // 54.7% of sections, and 213 of the 214 absent keys turned up in another
    // subworld's depot of the same level - the one that actually placed them.
    // The bundle this partition came in. Everything emitted below inherits it,
    // because a placement's material resolves against the bundle that placed
    // it. Restored on the way out for the same reason scope is.
    const std::string prev_bundle = bundle_;
    {
        std::string bare = name;
        const std::string& b = src_.bundle_of_ebx(bare);
        if (!b.empty()) bundle_ = b;
    }

    const std::string prev_scope = scope_;
    if (!scope_index.empty())
    {
        std::string bare = key;
        if (ends_with(bare, ".ebx")) bare.resize(bare.size() - 4);
        auto it = scope_index.find(bare);
        if (it != scope_index.end()) scope_ = it->second;
    }

    const std::vector<uint32_t> want(std::begin(kWalkFields), std::end(kWalkFields));
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        n_instances++;
        // THE ONLY PLACE THIS WALK CAN REPORT FROM. The traversal is recursive
        // and has no outer loop to count, but every instance the level contains
        // passes through here. Reported by placements FOUND against instances
        // seen, because there is no honest denominator: nothing knows how many
        // instances a level has until the walk has ended.
        if (progress_ && (n_instances & 8191) == 0)
            progress_("walking the level", (int)rows_.size(), (int)n_instances);

        // CAN THIS INSTANCE POSSIBLY MATTER? visit() reads exactly the walk
        // fields, so an instance whose type declares none of them is a provable
        // no-op. Decoding it means building every field of every nested struct
        // and throwing it away, and this is where the walk's time is: reading
        // and decompressing 11,748 partitions costs 1.6 s and parsing their
        // headers 0.6 s, against ~102 s decoding 268,587 instances.
        if (!type_matters(e, i)) { n_skipped++; continue; }
        const EbxValue inst = e.read_instance(i, &want);
        if (inst.kind != EbxValue::Kind::Struct) continue;
        visit(inst, parent, ref, guard, depth);
    }

    // Restored on the way out: a sibling branch must not inherit the scope a
    // subworld set for its own subtree.
    scope_ = prev_scope;
    bundle_ = prev_bundle;
}

bool Walk::run(const std::string& level_rel, std::string& err)
{
    err.clear();
    rows_.clear();
    n_instances = n_skipped = n_unresolved = 0;
    n_partitions = n_cycles = n_missing = n_parse_fail = 0;
    n_smg = n_smg_hidden = n_smg_unresolved = 0;
    n_leaf = n_excluded = n_destruction = 0;
    n_subworld = n_subworld_skipped = n_subworld_unresolved = 0;

    std::string rel = lower(level_rel);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();
    const std::string leaf = leaf_of(rel);

    std::string start = resolve_name(rel + "/" + leaf);
    if (start.empty()) start = resolve_name(rel);
    if (start.empty()) start = resolve_name(leaf);
    if (start.empty() && !contains(rel, "/"))
    {
        // A BARE LEVEL NAME. The catalogue knows which studio owns a level, so
        // a caller should not have to. Anchored on '/levels/<leaf>/<leaf>'
        // rather than a substring: a loose match would happily pick a
        // neighbouring level whose name merely CONTAINS this one.
        std::vector<std::string> tails{ "/levels/" + leaf + "/" + leaf };
        if (leaf.rfind("mp_", 0) != 0)
            tails.push_back("/levels/mp_" + leaf + "/mp_" + leaf);
        for (const std::string& tail : tails)
        {
            std::vector<std::string> hits;
            for (const auto& kv : by_name_)
                if (ends_with(kv.first, tail)) hits.push_back(kv.first);
            if (hits.empty()) continue;
            std::sort(hits.begin(), hits.end());
            start = by_name_[hits[0]];
            break;
        }
    }
    if (start.empty())
    {
        err = "could not resolve the level root for " + level_rel;
        return false;
    }
    root = start;
    walk_ref(start, mat_identity(), {}, 0);
    return !rows_.empty();
}

}  // namespace bf6

