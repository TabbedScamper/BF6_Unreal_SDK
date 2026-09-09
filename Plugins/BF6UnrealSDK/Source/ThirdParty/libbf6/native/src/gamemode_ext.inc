/* GAME MODE LAYOUTS: the gameplay entities each mode of a level authors, with
 * world transforms, read from the mode subworlds under _layers_gameplay/.
 *
 * WHY ITS OWN WALK. The placement walk (walk.cpp) starts at the level root and
 * follows the graph down; the gameplay subworlds hang off gameplay.ebx behind
 * SubWorldInclusionSettings and are gated, so the default walk never enters
 * them, and if it did it would fold every mode's layout on top of each other.
 * Walking each partition under `<level>/_layers_gameplay/` as a root of its
 * own is what reaches them - 2,701 entities across the modes on MP_Aftermath
 * against 0 from the level walk (measured by the GDScript miner this ports).
 *
 * WHAT IS COLLECTED IS DECIDED BY TYPE GUID, at the top level of each
 * partition, where the instance index is known and the partition's
 * LinkConnections can be joined to it. Nested Objects children are descended
 * for their transforms and references but never collected: a Blueprint's
 * Objects are pointers to top-level instances, so collecting there as well
 * would return every entity twice.
 *
 * GUIDS ARE GENERATED FROM THEIR DASHED TEXT, never typed as byte arrays. The
 * spawn reader records what a hand-typed array cost: two typos produced a
 * clean zero on every partition while every other check read green. The
 * dashed spelling sits beside each so it can be checked against the type
 * tables by eye. */

namespace bf6gm {

using bf6::Ebx;
using bf6::EbxValue;
using bf6::Mat34;
using bf6::TypeGuid;
using bf6::Vec3;

constexpr uint32_t F_OBJECTS      = 0x5A6115B2;
constexpr uint32_t F_DATAREFS     = 0xCDA4739B;
constexpr uint32_t F_COMPONENTS   = 0x5E00055A;
constexpr uint32_t F_BUNDLENAME   = 0x095E926D;
constexpr uint32_t F_BP_TRANSFORM = 0x7B554EF5;
constexpr uint32_t F_BLUEPRINT    = 0x6B831B88;
constexpr uint32_t F_EXCLUDED     = 0x4CD269A8;
constexpr uint32_t F_SMG_MEMBERS  = 0xA43BD992;

constexpr uint32_t F_LINKS        = 0xF7791E41;
constexpr uint32_t F_SOURCE       = 0x4676BA2C;
constexpr uint32_t F_TARGET       = 0x645B319C;
constexpr uint32_t F_SOURCE_FIELD = 0xC41FB636;
constexpr uint32_t F_TARGET_FIELD = 0x2D7314C9;

constexpr uint32_t F_TRANSFORM    = 0xD6351EDE;   /* LinearTransform on an entity */
constexpr uint32_t F_HALF_EXTENTS = 0x0D497F25;
constexpr uint32_t F_POINTS       = 0xEDFC6DF6;
constexpr uint32_t F_HEIGHT       = 0x28CEA299;
constexpr uint32_t F_TEAM         = 0x2ADBF2A3;
constexpr uint32_t F_ENABLED      = 0xF97D7309;
constexpr uint32_t F_FLAGS        = 0x5645E663;

constexpr uint32_t F_LT_RIGHT   = 0xC478CC3B;
constexpr uint32_t F_LT_UP      = 0xBF151EF9;
constexpr uint32_t F_LT_FORWARD = 0x695D12A4;
constexpr uint32_t F_LT_TRANS   = 0xBC4B07B4;
constexpr uint32_t K_VEC_X = 956422932, K_VEC_Y = 1123815262, K_VEC_Z = 849976220;

const char* K_LT = "06ce1d10-9a4e-fc64-2a9f-a9d482576ffa";

struct Wanted { const char* guid; int kind; const char* name; bool own_transform; };

/* The collected set. own_transform says whether the type declares a
 * LinearTransform of its own that composes onto the holder's; a volume does
 * not, and takes its holder's transform outright. */
const Wanted kWanted[] = {
    { "f7fbc419-e145-394f-7086-b81c1935e8ab", BF6_GM_SPAWN,         "AlternateSpawnEntityData",        true  },
    { "9fc7ba2d-7564-b0a6-8a9c-61f3fd93e55d", BF6_GM_VOLUME,        "VolumeVectorShapeData",           false },
    { "c8e55f62-8409-c039-a6bb-fbd11cb03739", BF6_GM_OBB,           "OBBData",                         true  },
    { "e36c4110-716c-d05f-7615-8f7b8a5d620b", BF6_GM_COMBAT,        "CombatAreaEntityData",            true  },
    { "8cba5d25-59fe-fa86-1c2a-8140e224d7da", BF6_GM_CAPTURE,       "CapturePointEntityData",          true  },
    { "25644ead-48bc-2015-5b75-849aa6e3d64f", BF6_GM_SECTOR,        "SectorEntityData",                true  },
    { "e529dbe3-1ef0-5632-37a6-82f9a3ac003f", BF6_GM_OBJECTIVE,     "ObjectiveData",                   true  },
    { "2196fb53-09d9-f276-1f73-103bd485f31a", BF6_GM_VEHICLE_SPAWN, "VehicleSpawnReferenceObjectData", true  },
    { "14db3a4e-8490-af9f-6d0a-98c72ca6c23e", BF6_GM_SOLDIER_SPAWN, "SoldierSpawnReferenceObjectData", true  },
    /* THE GEM SLOT (identified 2026-09-02 by its company: x80 in breakthrough0
     * beside imports gem_vehiclespawner / gem_stationaryspawner /
     * gem_vehicleresupplystation, x35 in the gauntlet layout whose only
     * vehicle import is gem_vehiclespawner, present in every vehicle-ful
     * mode). Its fields cannot be named through the tables, so they are read
     * BY SHAPE below, and its template link is a raw word at byte 132. */
    { "ee2bf4d0-c3fd-b131-1c29-78b6dd42672e", BF6_GM_GEM,           "gem instance (ee2bf4d0)",         true  },
};

/* The gem's parameter struct: field 5389A5F9 wraps a struct whose sole int is
 * the value word (1,7,11,...,31 on mp_isolated). Kept for the record; it is
 * NOT a vehicle type. */
constexpr uint32_t F_GEM_PARAM = 0x5389A5F9;
constexpr int      kGemSize = 160;
constexpr int      kGemLinkAt = 132;

/* WHAT OWNS A SHAPE. A polygon or box on a mode layer is usually the
 * geometry of some entity that links to it - a camera trigger, an area
 * proximity, a combat area - and only the unowned ones are the bare capture
 * zones and sectors a consumer wants. Ownership is read off the partition's
 * LinkConnections, walked backwards from the shape through the schematic
 * plumbing (channels, interface descriptors, surrounding-geometry) to the
 * first entity that is not plumbing. Names for the ones the type tables
 * know; the rest are reported by guid, which is still a fact. */
struct Named { const char* guid; const char* name; int kind; };
const Named kOwnerNames[] = {
    { "e36c4110-716c-d05f-7615-8f7b8a5d620b", "CombatAreaEntityData",          BF6_GM_COMBAT  },
    { "81a2a59c-9859-685c-d809-2c3825f18b3e", "CombatAreaTriggerEntityData",   BF6_GM_COMBAT  },
    { "8cba5d25-59fe-fa86-1c2a-8140e224d7da", "CapturePointEntityData",        BF6_GM_CAPTURE },
    { "25644ead-48bc-2015-5b75-849aa6e3d64f", "SectorEntityData",              BF6_GM_SECTOR  },
    { "4fc0d302-617a-0ef8-c026-b62e680214d3", "CameraEnterAreaTriggerEntityData", -1 },
    { "b4a11959-380a-707f-04d8-9d30554ca8fc", "AreaProximityEntityData",       -1 },
    { "1f425b27-0a92-e1c1-91de-0aeef4c36ef4", "TriggerVolumeEntityData",       -1 },
    { "949a114a-d523-a285-0884-8f9755d7e943", "AreaTriggerEntityData",         -1 },
    { "fd8c9a8f-3b00-82bd-c2a5-53d5700d6c84", "TransformQueryEntityData",      -1 },
    { "0daa4aa3-52a2-fbb6-f90b-afaafde2ad6e", "DiceSoundEntityData",           -1 },
};
const char* kPlumbing[] = {
    "f2e1dfd3-7b56-e6f1-2976-0bf6658c162e",   /* InterfaceDescriptorData      */
    "8c1c1b96-11b3-2b54-71e6-a7733c0f3f7a",   /* SchematicChannelEntityData   */
    "3cd5e8e1-7504-e8f9-9787-471f2a0de8ff",   /* SurroundingGeometryEntityData */
};

const char* kDestructionBranch[] = {
    "464ab605-1fb8-3d21-202f-f3feb43dffb9", "591bcc82-5380-7cbd-543e-faabbc873c09",
    "ef0c40ad-226b-f6fe-04dd-7b26c26350a2", "96e64a4c-c7e1-644c-dedb-499e51d965c2",
    "570f0b4f-fcd0-d977-580e-d409f464c6f1", "e788769d-af38-c1e6-d413-10cffb99e5f8",
    "643b508d-7f3e-af14-3022-033b9ced8d79", "b3120863-e2a0-5eb4-4523-a3f4dc814e20",
    "a0e4f0a2-5007-928a-20de-1f613062529c", "0f86bea6-d792-51ab-8d91-b724ea871296",
};

/* Subworlds under the prefix that are not a playable mode. telemetry_* is
 * analytics volumes and would draw a second, wrong set of zones. */
const char* kSkipLayer[] = { "telemetry", "_ai", "_narrative", "generated/" };
const char* kSkipSubworld[] = { "_autotests", "_tools", "marketing", "glacierflow/" };

constexpr int kMaxDepth = 24;
constexpr int kMaxOtherTypes = 64;
constexpr int kMaxSpawnerPaths = 32;

std::string lower(std::string s)
{
    for (char& c : s) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
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
bool starts_with(const std::string& s, const std::string& t)
{
    return s.size() >= t.size() && s.compare(0, t.size(), t) == 0;
}
std::string strip_ebx(std::string n)
{
    if (ends_with(lower(n), ".ebx")) n.resize(n.size() - 4);
    return n;
}

TypeGuid raw_guid(const std::string& dashed)
{
    TypeGuid g{}; std::string h;
    for (char c : dashed) if (c != '-') h += c;
    if (h.size() != 32) return g;
    auto byte = [&](size_t i) { return (uint8_t)std::stoul(h.substr(i * 2, 2), nullptr, 16); };
    g[0] = byte(3); g[1] = byte(2); g[2] = byte(1); g[3] = byte(0);
    g[4] = byte(5); g[5] = byte(4); g[6] = byte(7); g[7] = byte(6);
    for (int i = 8; i < 16; i++) g[(size_t)i] = byte((size_t)i);
    return g;
}

float as_f(const EbxValue* v)
{
    if (!v) return 0.f;
    if (v->kind == EbxValue::Kind::Real) return (float)v->f;
    if (v->kind == EbxValue::Kind::Int)  return (float)v->i;
    if (v->kind == EbxValue::Kind::Uint) return (float)v->u;
    return 0.f;
}
int32_t as_i(const EbxValue* v)
{
    if (!v) return 0;
    if (v->kind == EbxValue::Kind::Int)  return (int32_t)v->i;
    if (v->kind == EbxValue::Kind::Uint) return (int32_t)v->u;
    if (v->kind == EbxValue::Kind::Real) return (int32_t)v->f;
    if (v->kind == EbxValue::Kind::Bool) return v->b ? 1 : 0;
    return 0;
}
bool as_b(const EbxValue* v, bool dflt)
{
    if (!v) return dflt;
    if (v->kind == EbxValue::Kind::Bool) return v->b;
    if (v->kind == EbxValue::Kind::Uint) return v->u != 0;
    if (v->kind == EbxValue::Kind::Int)  return v->i != 0;
    return dflt;
}
Vec3 vec_of(const EbxValue* d)
{
    Vec3 out;
    if (!d || d->kind != EbxValue::Kind::Struct) return out;
    out.x = as_f(d->field(K_VEC_X));
    out.y = as_f(d->field(K_VEC_Y));
    out.z = as_f(d->field(K_VEC_Z));
    return out;
}
bool is_lt(const EbxValue* v, const TypeGuid& lt)
{
    return v && v->kind == EbxValue::Kind::Struct && v->guid == lt;
}
Mat34 lt_to_mat(const EbxValue* t)
{
    static const uint32_t m[4] = { F_LT_RIGHT, F_LT_UP, F_LT_FORWARD, F_LT_TRANS };
    Mat34 out = bf6::mat_identity();
    if (!t || t->kind != EbxValue::Kind::Struct) return out;
    Mat34 b;
    for (int i = 0; i < 4; i++) { const EbxValue* v = t->field(m[i]); if (!v) return out; b.m[i] = vec_of(v); }
    return b;
}
int ref_index(const EbxValue* v)
{
    return v && v->kind == EbxValue::Kind::InstanceRef ? v->instance : -1;
}

/* "conquest/conquest0" -> "conquest"; "winter_domination" -> itself.
 *
 * THREE SPELLINGS OF ONE MODE were measured on MP_Isolated and all fold here:
 * "carrierstrike0_schematic" is carrierstrike0's schematic partition, so the
 * key is cut at the first digit run that is followed by an underscore;
 * "mp_koth0" sits flat under the prefix while "koth/" is the folder of the
 * same mode, so a leading "mp_" goes; and a trailing digit is a variant of
 * the same subworld, not a separate mode. */
std::string mode_of(const std::string& tail)
{
    std::string first = tail.substr(0, tail.find('/'));
    for (size_t i = 0; i + 1 < first.size(); i++) {
        if (!std::isdigit((unsigned char)first[i])) continue;
        size_t j = i;
        while (j < first.size() && std::isdigit((unsigned char)first[j])) j++;
        if (j < first.size() && first[j] == '_' && i > 0) { first.resize(i); break; }
    }
    while (first.size() > 1 && std::isdigit((unsigned char)first.back())) first.pop_back();
    if (starts_with(first, "mp_") && first.size() > 3) first = first.substr(3);
    return first;
}
bool is_mode(const std::string& m)
{
    /* "shared_cq_esc" and "sharedassets_smallgamemode_lay" (MP_Dumbo) are the
     * prop layers several modes share, gated in world.ebx, not a mode */
    return !(m.empty() || m == "gameplay" || ends_with(m, "_global")
             || starts_with(m, "gmpf_") || starts_with(m, "pf_") || starts_with(m, "shared"));
}

struct Entity {
    std::string mode, layer, partition, type_name, blueprint;
    int         kind = 0;
    int         instance = -1;
    Mat34       xf = bf6::mat_identity();
    bool        own_transform = false;
    int32_t     team = 0;
    bool        enabled = true;
    uint32_t    flags = 0;
    std::vector<Vec3> points;
    float       height = 0.f;
    Vec3        half_extents;
    std::vector<int32_t>  links;
    std::vector<uint32_t> link_fields;
    std::string owner_type;     /* name or dashed guid of the entity this shape belongs to */
    int         owner_kind = -1;
    bool        is_gem = false;
    std::string gem_link;       /* gems: the byte-132 word's partition leaf, "" none, "?" out of range */
    int32_t     gem_value = -1;
};

struct Stats {
    int layers = 0, layers_skipped = 0;
    int partitions = 0, instances = 0, parse_fail = 0, missing = 0, cycles = 0, unresolved = 0;
    int links = 0, links_dropped = 0, fake_hits = 0;
    int gems = 0, gems_linked = 0;
    std::map<TypeGuid, int> other_types;
    std::map<std::string, int> dropped_link_types;   /* "src->tgt" guid pair -> count */
    std::vector<std::string> spawner_paths;
};

class GmWalk {
public:
    GmWalk(bf6::Source& s, bf6::TypeDb& t) : src(s), types(t), lt(raw_guid(K_LT))
    {
        for (const Wanted& w : kWanted) wanted[raw_guid(w.guid)] = &w;
        for (const Named& n : kOwnerNames) owner_names[raw_guid(n.guid)] = &n;
        for (const char* p : kPlumbing) plumbing.insert(raw_guid(p));
        for (const char* d : kDestructionBranch) destruction.insert(d);
    }

    bool run(const std::string& level, std::string& err);

    std::vector<Entity> ents;
    Stats st;

private:
    void build_catalog();
    std::string resolve_name(const std::string&) const;
    bool type_matters(Ebx&, size_t);
    void walk_ref(const std::string&, const Mat34&, std::set<std::string>, int);
    void visit(const EbxValue&, const Mat34&, const std::string&, const std::set<std::string>&, int);
    void collect_partition(Ebx&, const Mat34&, const std::string&);

    bf6::Source& src;
    bf6::TypeDb& types;
    TypeGuid lt{};
    std::map<TypeGuid, const Wanted*> wanted;
    std::map<TypeGuid, const Named*>  owner_names;
    std::set<TypeGuid>                plumbing;
    std::set<std::string> destruction;
    std::unordered_map<std::string, std::string> by_name;
    std::map<std::string, std::string> gi;
    std::map<TypeGuid, bool> matters;
    std::set<std::string> done;      /* partitions already collected, across roots */
    std::string cur_mode, cur_layer, prefix;
};

void GmWalk::build_catalog()
{
    for (const auto& kv : src.ebx()) {
        const std::string low = lower(kv.first), ref = kv.first + ".ebx";
        by_name.emplace(low, ref);
        by_name.emplace(leaf_of(low), ref);
    }
    gi = src.partition_index();
}

std::string GmWalk::resolve_name(const std::string& name) const
{
    if (name.empty()) return {};
    const std::string n = lower(name);
    for (const std::string& k : { n, leaf_of(n), n + ".ebx" }) {
        auto it = by_name.find(k);
        if (it != by_name.end()) return it->second;
    }
    return {};
}

bool GmWalk::type_matters(Ebx& e, size_t i)
{
    const TypeGuid g = e.instance_type(i);
    auto it = matters.find(g);
    if (it != matters.end()) return it->second;
    const bf6::TypeLayout& lay = types.layout_full(g);
    bool yes = false;
    if (!lay.valid) { st.unresolved++; yes = true; }
    else {
        bool smg = false;
        for (const bf6::FieldInfo& f : lay.fields) {
            if (f.name_hash == F_SMG_MEMBERS) { smg = true; break; }
            for (uint32_t h : { F_BP_TRANSFORM, F_EXCLUDED, F_BUNDLENAME, F_BLUEPRINT, F_OBJECTS, F_DATAREFS, F_COMPONENTS })
                if (f.name_hash == h) { yes = true; break; }
        }
        if (smg) yes = false;     /* a StaticModelGroup is meshes, not gameplay */
    }
    matters[g] = yes;
    return yes;
}

/* One partition's top-level instances: collect the wanted ones with the
 * partition's inherited transform, then join the partition's LinkConnections
 * between them. */
void GmWalk::collect_partition(Ebx& e, const Mat34& parent, const std::string& ref)
{
    const std::string part = strip_ebx(ref);
    if (!done.insert(lower(part)).second) return;

    std::map<int, size_t> by_instance;    /* instance index -> ents index */
    const size_t first = ents.size();
    const std::vector<uint32_t> want{ F_TRANSFORM, F_HALF_EXTENTS, F_POINTS, F_HEIGHT, F_TEAM, F_ENABLED, F_FLAGS };

    for (size_t i = 0; i < e.instance_count(); i++) {
        const TypeGuid g = e.instance_type(i);
        auto w = wanted.find(g);
        if (w == wanted.end()) {
            /* the census is of the LAYER partitions themselves, not of every
             * prefab they place: a carrier's material variations are not
             * gameplay and would bury the one BFTeamEntityData that is */
            if (starts_with(lower(part), prefix)) st.other_types[g]++;
            continue;
        }

        const bool gem = w->second->kind == BF6_GM_GEM;
        /* a gem's fields have no names in the tables, so EVERY field is read
         * and identified by shape; 7 fields in 160 bytes, so it is cheap */
        const EbxValue v = e.read_instance(i, gem ? nullptr : &want);
        Entity en;
        en.mode = cur_mode; en.layer = cur_layer; en.partition = part;
        en.type_name = w->second->name; en.kind = w->second->kind; en.instance = (int)i;
        en.xf = parent;
        if (w->second->own_transform) {
            const EbxValue* t = v.field(F_TRANSFORM);
            if (!is_lt(t, lt) && gem)
                for (const auto& f : v.fields) if (is_lt(&f.second, lt)) { t = &f.second; break; }
            if (is_lt(t, lt)) { en.xf = bf6::mat_mul(parent, lt_to_mat(t)); en.own_transform = true; }
        }
        en.team    = as_i(v.field(F_TEAM));
        en.enabled = as_b(v.field(F_ENABLED), true);
        if (const EbxValue* f = v.field(F_FLAGS)) en.flags = (uint32_t)as_i(f);
        if (gem) {
            en.is_gem = true;
            /* Team: the one int in the team range (the key word is huge, the
             * value word sits inside its own struct); Godot's _int_of rule */
            if (!v.field(F_TEAM))
                for (const auto& f : v.fields) {
                    if (f.second.kind != EbxValue::Kind::Int && f.second.kind != EbxValue::Kind::Uint) continue;
                    if (f.first == F_FLAGS) continue;
                    const int32_t iv = as_i(&f.second);
                    if (iv > 0 && iv < 128) { en.team = iv; break; }
                }
            if (const EbxValue* pr = v.field(F_GEM_PARAM))
                if (pr->kind == EbxValue::Kind::Struct)
                    for (const auto& f : pr->fields)
                        if (f.second.kind == EbxValue::Kind::Int || f.second.kind == EbxValue::Kind::Uint) { en.gem_value = as_i(&f.second); break; }
            /* THE LINK WORD, raw. The walk hands the reference back as a null
             * typed pointer, so the bytes are read: 0xFFFFFFFF is "none", else
             * an index into the partition's import table whose partition
             * half resolves through the guid index to a name. */
            const int64_t base = e.payload() + (int64_t)e.instance_offset(i);
            const std::vector<uint8_t>& d = e.raw();
            en.gem_link = "?";
            if (base >= 0 && base + kGemSize <= (int64_t)d.size()) {
                uint32_t refw = 0;
                std::memcpy(&refw, d.data() + base + kGemLinkAt, 4);
                if (refw == 0xFFFFFFFFu) en.gem_link.clear();
                else if ((size_t)refw < e.import_count()) {
                    auto it = gi.find(e.import_at((size_t)refw).partition);
                    en.gem_link = it != gi.end() ? leaf_of(strip_ebx(it->second)) : std::string("?");
                }
            }
            st.gems++;
            if (!en.gem_link.empty() && en.gem_link != "?") st.gems_linked++;
        }
        if (en.kind == BF6_GM_VOLUME) {
            if (const EbxValue* p = v.field(F_POINTS))
                if (p->kind == EbxValue::Kind::Array)
                    for (const EbxValue& el : p->items)
                        if (el.kind == EbxValue::Kind::Struct) en.points.push_back(vec_of(&el));
            en.height = as_f(v.field(F_HEIGHT));
            /* a polygon with fewer than three points is not a shape; kept out
             * rather than handed to a consumer as a zero-area zone */
            if (en.points.size() < 3) continue;
        }
        if (en.kind == BF6_GM_OBB) en.half_extents = vec_of(v.field(F_HALF_EXTENTS));
        by_instance[(int)i] = ents.size();
        ents.push_back(std::move(en));
    }
    if (ents.size() == first) return;

    /* LinkConnections live on whatever instance declares Links - the blueprint
     * root, usually - as Source/Target instance refs. Only edges whose both
     * ends were collected above are kept; the rest are counted, because an
     * edge to something this reader does not name is still information. */
    std::map<int, std::vector<int>> pred;   /* target -> sources, every edge */
    const std::vector<uint32_t> lwant{ F_LINKS };
    for (size_t i = 0; i < e.instance_count(); i++) {
        const bf6::TypeLayout& lay = types.layout_full(e.instance_type(i));
        bool has = false;
        if (lay.valid) for (const bf6::FieldInfo& f : lay.fields) if (f.name_hash == F_LINKS) { has = true; break; }
        if (!has) continue;
        const EbxValue root = e.read_instance(i, &lwant);
        const EbxValue* a = root.field(F_LINKS);
        if (!a || a->kind != EbxValue::Kind::Array) continue;
        for (const EbxValue& x : a->items) {
            if (x.kind != EbxValue::Kind::Struct) continue;
            const int s = ref_index(x.field(F_SOURCE)), t = ref_index(x.field(F_TARGET));
            if (s < 0 || t < 0) continue;
            pred[t].push_back(s);
            auto si = by_instance.find(s), ti = by_instance.find(t);
            if (si == by_instance.end() || ti == by_instance.end()) {
                st.links_dropped++;
                /* what the uncollected end IS, so the next reader knows what
                 * a combat area actually links to */
                if ((size_t)s < e.instance_count() && (size_t)t < e.instance_count())
                    st.dropped_link_types[bf6::TypeDb::guid_str(e.instance_type((size_t)s)) + "->" +
                                          bf6::TypeDb::guid_str(e.instance_type((size_t)t))]++;
                continue;
            }
            const uint32_t sf = (uint32_t)as_i(x.field(F_SOURCE_FIELD));
            ents[si->second].links.push_back((int32_t)ti->second);
            ents[si->second].link_fields.push_back(sf);
            st.links++;
        }
    }

    /* Owners: from each collected shape, back along the edges through the
     * plumbing to the first entity that is not plumbing, three hops at most.
     * A shape nobody links to stays unowned, which is the interesting case. */
    for (size_t k = first; k < ents.size(); k++) {
        Entity& en = ents[k];
        if (en.kind != BF6_GM_VOLUME && en.kind != BF6_GM_OBB) continue;
        std::set<int> seen;
        std::vector<int> frontier{ en.instance };
        for (int hop = 0; hop < 3 && !frontier.empty() && en.owner_type.empty(); hop++) {
            std::vector<int> next;
            for (int t : frontier) {
                auto it = pred.find(t);
                if (it == pred.end()) continue;
                for (int s : it->second) {
                    if (!seen.insert(s).second || (size_t)s >= e.instance_count()) continue;
                    const TypeGuid g = e.instance_type((size_t)s);
                    if (plumbing.count(g)) { next.push_back(s); continue; }
                    auto nm = owner_names.find(g);
                    if (nm != owner_names.end()) { en.owner_type = nm->second->name; en.owner_kind = nm->second->kind; }
                    else {
                        auto w = wanted.find(g);
                        if (w != wanted.end()) { en.owner_type = w->second->name; en.owner_kind = w->second->kind; }
                        else en.owner_type = bf6::TypeDb::guid_str(g);
                    }
                    break;
                }
                if (!en.owner_type.empty()) break;
            }
            frontier.swap(next);
        }
    }
}

void GmWalk::visit(const EbxValue& inst, const Mat34& parent, const std::string& ref,
                   const std::set<std::string>& guard, int depth)
{
    if (destruction.count(bf6::TypeDb::guid_str(inst.guid))) return;
    const EbxValue* local = inst.field(F_BP_TRANSFORM);
    const Mat34 world = is_lt(local, lt) ? bf6::mat_mul(parent, lt_to_mat(local)) : parent;
    if (as_b(inst.field(F_EXCLUDED), false)) return;

    if (const EbxValue* bn = inst.field(F_BUNDLENAME))
        if (bn->kind == EbxValue::Kind::Str && !bn->s.empty()) {
            const std::string l = lower(bn->s);
            for (const char* s : kSkipSubworld) if (l.find(s) != std::string::npos) return;
            const std::string sub = resolve_name(bn->s);
            if (!sub.empty()) walk_ref(sub, world, guard, depth + 1);
            return;
        }

    if (const EbxValue* bp = inst.field(F_BLUEPRINT))
        if (bp->kind == EbxValue::Kind::ImportRef && !bp->import_path.empty()
            && bp->import_path != "<not indexed>") {
            const std::string l = lower(bp->import_path);
            /* THE VEHICLE SPAWNER IS A GEM, NOT AN ENTITY. A placement of the
             * shared vehiclespawner gem is where a vehicle spawns; its inside
             * is logic, so it is recorded at the composed transform and not
             * descended. Every other "spawner" path is listed for the caller,
             * because a loot or emplacement spawner is worth knowing about and
             * silently walking through it would lose that. */
            if (l.find("vehiclespawner") != std::string::npos
                || leaf_of(l).rfind("gmpf_portal_veh_", 0) == 0) {
                Entity en;
                en.mode = cur_mode; en.layer = cur_layer; en.partition = strip_ebx(ref);
                en.type_name = "vehiclespawner gem placement"; en.kind = BF6_GM_VEHICLE_SPAWN;
                en.blueprint = strip_ebx(bp->import_path);
                en.xf = world; en.own_transform = is_lt(local, lt);
                ents.push_back(std::move(en));
                return;
            }
            if (l.find("spawner") != std::string::npos
                && (int)st.spawner_paths.size() < kMaxSpawnerPaths) {
                const std::string p = strip_ebx(bp->import_path);
                if (std::find(st.spawner_paths.begin(), st.spawner_paths.end(), p) == st.spawner_paths.end())
                    st.spawner_paths.push_back(p);
            }
            walk_ref(bp->import_path, world, guard, depth + 1);
            return;
        }

    for (uint32_t h : { F_OBJECTS, F_DATAREFS, F_COMPONENTS })
        if (const EbxValue* a = inst.field(h))
            if (a->kind == EbxValue::Kind::Array)
                for (const EbxValue& c : a->items)
                    if (c.kind == EbxValue::Kind::Struct) visit(c, world, ref, guard, depth);
}

void GmWalk::walk_ref(const std::string& ref, const Mat34& parent, std::set<std::string> guard, int depth)
{
    if (depth > kMaxDepth || ref.empty()) return;
    const std::string key = lower(ref);
    if (guard.count(key)) { st.cycles++; return; }
    std::string err;
    std::vector<uint8_t> raw = src.get_ebx(strip_ebx(ref), err);
    if (raw.empty()) { st.missing++; return; }
    Ebx e(types);
    e.set_guid_index(&gi);
    if (!e.parse(std::move(raw), err)) { st.parse_fail++; return; }
    guard.insert(key);
    st.partitions++;

    collect_partition(e, parent, ref);

    static const std::vector<uint32_t> want{
        F_BP_TRANSFORM, F_EXCLUDED, F_BUNDLENAME, F_BLUEPRINT, F_OBJECTS, F_DATAREFS, F_COMPONENTS };
    for (size_t i = 0; i < e.instance_count(); i++) {
        st.instances++;
        if (!type_matters(e, i)) continue;
        const EbxValue d = e.read_instance(i, &want);
        if (d.kind == EbxValue::Kind::Struct) visit(d, parent, ref, guard, depth);
    }
}

/* The level's partition directory, from any partition the mount has under
 * /levels/<level>/, and every root under its _layers_gameplay/. */
static std::string level_dir_of(bf6::Source& src, const std::string& level)
{
    const std::string want = "/levels/" + lower(level) + "/";
    for (const auto& kv : src.ebx()) {
        const std::string s = lower(kv.first);
        const size_t at = s.find(want);
        if (at != std::string::npos) return kv.first.substr(0, at + want.size() - 1);
    }
    return {};
}

static std::vector<std::string> roots_of(bf6::Source& src, const std::string& level, int* skipped)
{
    std::vector<std::string> out;
    const std::string dir = level_dir_of(src, level);
    if (dir.empty()) return out;
    const std::string pre = lower(dir) + "/_layers_gameplay/";
    for (const auto& kv : src.ebx()) {
        const std::string s = lower(kv.first);
        if (!starts_with(s, pre)) continue;
        const std::string tail = s.substr(pre.size());
        bool skip = false;
        for (const char* bad : kSkipLayer) if (tail.find(bad) != std::string::npos) { skip = true; break; }
        if (!skip && !is_mode(mode_of(tail))) skip = true;
        if (skip) { if (skipped) (*skipped)++; continue; }
        out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool GmWalk::run(const std::string& level, std::string& err)
{
    err.clear();
    ents.clear();
    st = Stats();
    build_catalog();

    const std::string dir = level_dir_of(src, level);
    if (dir.empty()) { err = "no partition under /levels/" + level + "/ is mounted"; return false; }
    const std::string pre = lower(dir) + "/_layers_gameplay/";
    prefix = pre;
    const std::vector<std::string> roots = roots_of(src, level, &st.layers_skipped);
    if (roots.empty()) { err = "no gameplay layers under " + dir + "/_layers_gameplay/"; return false; }

    for (const std::string& r : roots) {
        cur_layer = r;
        cur_mode  = mode_of(lower(r).substr(pre.size()));
        st.layers++;
        /* NEGATIVE CONTROL: a root with its last character changed must not
         * resolve. If it does, the name index is matching loosely and every
         * count above it is suspect. */
        {
            std::string fake = r;
            fake.back() = fake.back() == 'z' ? 'y' : 'z';
            if (!resolve_name(fake).empty()) st.fake_hits++;
        }
        walk_ref(r + ".ebx", bf6::mat_identity(), {}, 0);
    }
    return true;
}

}  // namespace bf6gm

/* ---- the C surface ------------------------------------------------------- */

static void bf6__gm_publish(bf6_ctx* c, const std::vector<bf6gm::Entity>& ents, const bf6gm::Stats& st)
{
    c->gm_rows.clear(); c->gm_points.clear(); c->gm_links.clear(); c->gm_link_fields.clear();
    c->gm_strings.clear(); c->gm_other_types.clear(); c->gm_other_ptrs.clear();
    c->gm_spawner_paths.clear(); c->gm_spawner_ptrs.clear();
    c->gm_stats = bf6_gm_stats{};

    auto intern = [c](const std::string& s) -> const char* {
        if (s.empty()) return nullptr;
        auto it = c->gm_strings.find(s);
        if (it == c->gm_strings.end()) it = c->gm_strings.insert(s).first;
        return it->c_str();
    };

    const size_t n = ents.size();
    c->gm_rows.resize(n); c->gm_points.resize(n); c->gm_links.resize(n); c->gm_link_fields.resize(n);
    std::set<std::string> modes;
    for (size_t i = 0; i < n; i++) {
        const bf6gm::Entity& e = ents[i];
        bf6_gm_entity& r = c->gm_rows[i];
        r = bf6_gm_entity{};
        r.mode = intern(e.mode); r.layer = intern(e.layer); r.partition = intern(e.partition);
        r.type_name = intern(e.type_name); r.blueprint = intern(e.blueprint);
        r.kind = e.kind; r.instance = e.instance;
        for (int k = 0; k < 4; k++) {
            r.xform[k * 3 + 0] = e.xf.m[k].x; r.xform[k * 3 + 1] = e.xf.m[k].y; r.xform[k * 3 + 2] = e.xf.m[k].z;
        }
        r.has_own_transform = e.own_transform ? 1u : 0u;
        r.team = e.team; r.enabled = e.enabled ? 1u : 0u; r.flags = e.flags;
        std::vector<float>& p = c->gm_points[i];
        for (const bf6::Vec3& v : e.points) { p.push_back(v.x); p.push_back(v.y); p.push_back(v.z); }
        r.points = p.empty() ? nullptr : p.data();
        r.point_count = (int32_t)e.points.size();
        r.height = e.height;
        r.half_extents[0] = e.half_extents.x; r.half_extents[1] = e.half_extents.y; r.half_extents[2] = e.half_extents.z;
        c->gm_links[i] = e.links; c->gm_link_fields[i] = e.link_fields;
        r.links = c->gm_links[i].empty() ? nullptr : c->gm_links[i].data();
        r.link_fields = c->gm_link_fields[i].empty() ? nullptr : c->gm_link_fields[i].data();
        r.link_count = (int32_t)e.links.size();
        r.owner_type = intern(e.owner_type);
        r.owner_kind = e.owner_kind;
        /* "" must survive as a non-NULL pointer: NULL means "not a gem" */
        r.gem_link  = e.is_gem ? (e.gem_link.empty() ? "" : intern(e.gem_link)) : nullptr;
        r.gem_value = e.is_gem ? e.gem_value : -1;
        modes.insert(e.mode);
        switch (e.kind) {
            case BF6_GM_SPAWN:         c->gm_stats.spawns++; break;
            case BF6_GM_VOLUME:        c->gm_stats.volumes++; break;
            case BF6_GM_OBB:           c->gm_stats.obbs++; break;
            case BF6_GM_COMBAT:        c->gm_stats.combat++; break;
            case BF6_GM_CAPTURE:       c->gm_stats.capture++; break;
            case BF6_GM_SECTOR:        c->gm_stats.sector++; break;
            case BF6_GM_OBJECTIVE:     c->gm_stats.objective++; break;
            case BF6_GM_VEHICLE_SPAWN: c->gm_stats.vehicle_spawns++; break;
            case BF6_GM_SOLDIER_SPAWN: c->gm_stats.soldier_spawns++; break;
            default: break;
        }
    }
    bf6_gm_stats& s = c->gm_stats;
    s.gems = st.gems; s.gems_linked = st.gems_linked;
    s.layers = st.layers; s.layers_skipped = st.layers_skipped; s.modes = (int32_t)modes.size();
    s.partitions = st.partitions; s.instances = st.instances; s.parse_fail = st.parse_fail;
    s.missing = st.missing; s.cycles = st.cycles; s.unresolved_types = st.unresolved;
    s.links = st.links; s.links_dropped = st.links_dropped; s.control_fake_root_hits = st.fake_hits;

    std::vector<std::pair<int, bf6::TypeGuid>> other;
    for (const auto& kv : st.other_types) other.push_back({ kv.second, kv.first });
    std::sort(other.begin(), other.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < other.size() && (int)i < bf6gm::kMaxOtherTypes; i++)
        c->gm_other_types.push_back(bf6::TypeDb::guid_str(other[i].second) + " " + std::to_string(other[i].first));
    for (const std::string& t : c->gm_other_types) c->gm_other_ptrs.push_back(t.c_str());
    s.other_type_count = (int32_t)c->gm_other_ptrs.size();
    s.other_types = c->gm_other_ptrs.empty() ? nullptr : c->gm_other_ptrs.data();

    c->gm_spawner_paths = st.spawner_paths;
    for (const std::string& t : c->gm_spawner_paths) c->gm_spawner_ptrs.push_back(t.c_str());
    s.spawner_path_count = (int32_t)c->gm_spawner_ptrs.size();
    s.spawner_paths = c->gm_spawner_ptrs.empty() ? nullptr : c->gm_spawner_ptrs.data();

    std::vector<std::pair<int, std::string>> dl;
    for (const auto& kv : st.dropped_link_types) dl.push_back({ kv.second, kv.first });
    std::sort(dl.begin(), dl.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    c->gm_dropped_types.clear(); c->gm_dropped_ptrs.clear();
    for (size_t i = 0; i < dl.size() && i < 16; i++)
        c->gm_dropped_types.push_back(dl[i].second + " " + std::to_string(dl[i].first));
    for (const std::string& t : c->gm_dropped_types) c->gm_dropped_ptrs.push_back(t.c_str());
    s.dropped_link_type_count = (int32_t)c->gm_dropped_ptrs.size();
    s.dropped_link_types = c->gm_dropped_ptrs.empty() ? nullptr : c->gm_dropped_ptrs.data();
}

int bf6_level_gamemodes(bf6_ctx* c, const char* level,
                        bf6_gm_entity* out, int out_max,
                        bf6_gm_stats* stats, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level) return fail("bad arguments");

    if (c->gm_level != level) {
        std::string e;
        if (!ensure_mounted(c, level, e)) return fail(e);
        if (!ensure_types(c, e)) return fail(e);
        c->report("reading the game modes", 0, 0);
        bf6gm::GmWalk w(c->src, *c->types);
        if (!w.run(level, e)) return fail(e);
        bf6__gm_publish(c, w.ents, w.st);
        c->gm_level = level;
    }
    if (stats) *stats = c->gm_stats;
    const int n = (int)c->gm_rows.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = c->gm_rows[(size_t)i];
    if (n == 0) return fail("the gameplay layers hold no collected entity");
    return n;
}

int bf6_level_gamemode_layers(bf6_ctx* c, const char* level,
                              bf6_asset* out, int out_max,
                              char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    std::string e;
    if (!ensure_mounted(c, level, e)) return fail(e);
    const std::vector<std::string> roots = bf6gm::roots_of(c->src, level, nullptr);
    if (roots.empty()) return fail("no gameplay layers for " + std::string(level));
    c->gm_layer_names = roots;
    const int n = (int)roots.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) {
            out[i].name = c->gm_layer_names[(size_t)i].c_str();
            out[i].type = 0;
            auto it = c->src.ebx().find(c->gm_layer_names[(size_t)i]);
            out[i].size = it != c->src.ebx().end() ? it->second.dsize : 0;
        }
    return n;
}

/* ============================================================================
 * THE LAYOUT: one mode's rows classified by the laws the Godot fork settled on
 * real maps (highpoly_gmmine.gd _junk_src / _classify / _name_objects /
 * _mark_ground, 2026-09-01..04). The rows above stay a faithful read of the
 * layers; this is the reading of them, kept in the library so the standalone
 * test and every consumer classify the same way.
 *
 * Caches live in file statics keyed by context rather than on bf6_ctx, which
 * is not this file's to grow. One context per process is the norm.
 * ========================================================================== */
namespace bf6gmlay {

using bf6::Vec3;

constexpr float kCaptureMaxArea   = 1500.f;
constexpr float kBigFlagMaxArea   = 10000.f;
constexpr int   kBigFlagMinSpawns = 4;
constexpr float kBigFlagRadius    = 45.f;
constexpr int   kSanityMaxFlags   = 16;
constexpr float kSpawnClaimRadius = 30.f;
constexpr float kFlagMergeM       = 15.f;    /* two capture polygons this close are one objective */
constexpr float kBeachBand        = 3.f;     /* the game pulls boats onto the sand */
constexpr float kLandClearance    = 1.5f;
constexpr float kGridStep         = 15.f;
constexpr float kGridMargin       = 250.f;
constexpr int   kGridMax          = 260;
constexpr float kNoHeight         = -1e9f;

/* Prefab-referenced and analytics partitions never carry a mode's gameplay.
 * The roots filter already refuses pf_* subworlds, but these arrive as prefab
 * REFERENCES from inside a mode's own root, so the rule is applied to the
 * partition each entity actually came out of. */
bool junk_partition(const std::string& leaf_lower)
{
    return bf6gm::starts_with(leaf_lower, "pf_") || bf6gm::starts_with(leaf_lower, "gmpf_")
        || leaf_lower.find("telemetry") != std::string::npos;
}

bool flag_family(const std::string& mode)
{
    return mode.find("conquest") != std::string::npos || mode.find("carrierstrike") != std::string::npos
        || mode.find("escalation") != std::string::npos;
}

struct Ground {
    std::string level;
    bool tried = false, ok = false, water_tried = false;
    int  size = 0;
    std::vector<uint16_t> h;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    float yscale = 0.f;
    float water_y = kNoHeight;
};
static std::map<bf6_ctx*, Ground> g_ground;

static Ground& ground_for(bf6_ctx* c, const std::string& level)
{
    Ground& g = g_ground[c];
    if (g.level != level) { g = Ground(); g.level = level; }
    if (!g.tried) {
        g.tried = true;
        std::string e;
        if (ensure_mounted(c, level.c_str(), e)) {
            bf6_terrain* t = bf6_read_terrain(c, level.c_str());
            if (t && t->width > 1 && t->heights) {
                g.ok = true;
                g.size = t->width;
                g.h.assign(t->heights, t->heights + (size_t)t->width * (size_t)t->height);
                for (int i = 0; i < 3; i++) { g.lo[i] = t->world_min[i]; g.hi[i] = t->world_max[i]; }
                /* y = u16 / 65536 * height scale, straight from the tree's own
                 * header; the AABB span is the fallback when it is missing */
                g.yscale = t->height_scale > 0.f ? t->height_scale / 65536.f
                         : std::max(0.001f, g.hi[1] - g.lo[1]) / 65535.f;
            }
            if (t) bf6_free(c, t);
        }
    }
    if (!g.water_tried) {
        g.water_tried = true;
        bf6_water w{};
        if (bf6_level_water(c, level.c_str(), &w, 1) > 0) g.water_y = w.height;
    }
    return g;
}

static float height_at(const Ground& g, float x, float z)
{
    if (!g.ok || g.size < 2) return kNoHeight;
    const float sx = g.hi[0] - g.lo[0], sz = g.hi[2] - g.lo[2];
    if (sx <= 0.f || sz <= 0.f) return kNoHeight;
    const float fx = (x - g.lo[0]) / sx * (float)(g.size - 1);
    const float fz = (z - g.lo[2]) / sz * (float)(g.size - 1);
    if (fx < 0.f || fz < 0.f || fx > (float)(g.size - 1) || fz > (float)(g.size - 1)) return kNoHeight;
    const int ix = std::min((int)fx, g.size - 2), iz = std::min((int)fz, g.size - 2);
    const float tx = fx - (float)ix, tz = fz - (float)iz;
    /* ABSOLUTE, not AABB-relative: u16 * scale is the world Y, exactly as the
     * High Poly terrain build reads it. Adding the AABB floor put Golmud's
     * ground 80 m and Eastwood's 175 m above every spawn (measured 2026-09-05). */
    auto H = [&](int xx, int zz) { return (float)g.h[(size_t)zz * (size_t)g.size + (size_t)xx] * g.yscale; };
    const float a = H(ix, iz) + (H(ix + 1, iz) - H(ix, iz)) * tx;
    const float b = H(ix, iz + 1) + (H(ix + 1, iz + 1) - H(ix, iz + 1)) * tx;
    return a + (b - a) * tz;
}

struct Obj {
    int   entity = -1;
    int   kind = 0;            /* bf6_gm_kind, for the dedup key */
    int   role = BF6_GMR_DROPPED;
    int   pending = 0;         /* 1 = a polygon still to be classified, 2 = a bare combat reference */
    std::string label;
    int   flag = -1;
    int   team = 0;
    float area = 0.f;
    Vec3  centre;
    Vec3  origin;
    std::vector<float> wpts;
    float height = 0.f;
    bool  stationary = false, by_data = false, has_ground = false, water = false, has_land = false;
    float ground_y = 0.f;
    Vec3  land;
    const char* gem_link = nullptr;
    int   gem_value = -1;
    std::string extra;         /* dedup: footprint */
};

struct Layout {
    std::string level, mode;
    std::vector<Obj>           objs;
    std::vector<bf6_gm_object> rows;
    std::set<std::string>      strings;
    std::vector<float>         grid;
    bf6_gm_layout              st{};
};
static std::map<bf6_ctx*, Layout> g_layout;

static Vec3 apply(const float* m, const Vec3& p)
{
    Vec3 o;
    o.x = m[0] * p.x + m[3] * p.y + m[6] * p.z + m[9];
    o.y = m[1] * p.x + m[4] * p.y + m[7] * p.z + m[10];
    o.z = m[2] * p.x + m[5] * p.y + m[8] * p.z + m[11];
    return o;
}

static float area_local_xz(const float* p, int n)
{
    if (n < 3) return 0.f;
    double a = 0.0;
    for (int i = 0; i < n; i++) {
        const float* c = p + i * 3; const float* d = p + ((i + 1) % n) * 3;
        a += (double)c[0] * d[2] - (double)d[0] * c[2];
    }
    return (float)(std::fabs(a) * 0.5);
}

static bool in_poly_xz(const std::vector<float>& w, float x, float z)
{
    const int n = (int)w.size() / 3;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const float xi = w[i * 3], zi = w[i * 3 + 2], xj = w[j * 3], zj = w[j * 3 + 2];
        if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) inside = !inside;
    }
    return inside;
}

static std::string letter(int i)
{
    return i < 26 ? std::string(1, (char)('A' + i)) : std::to_string(i + 1);
}

static void build(bf6_ctx* c, const std::string& level, const std::string& mode, Layout& L)
{
    L = Layout();
    L.level = level; L.mode = mode;
    bf6_gm_layout& st = L.st;
    st.water_y = kNoHeight;

    std::vector<Obj> objs;
    for (size_t i = 0; i < c->gm_rows.size(); i++) {
        const bf6_gm_entity& r = c->gm_rows[i];
        if (!r.mode || mode != r.mode) continue;
        const std::string part = r.partition ? bf6gm::lower(r.partition) : std::string();
        if (junk_partition(bf6gm::leaf_of(part))) { st.dropped_junk++; continue; }

        Obj o;
        o.entity = (int)i; o.kind = r.kind; o.team = r.team;
        o.origin = Vec3{ r.xform[9], r.xform[10], r.xform[11] };
        o.centre = o.origin;
        switch (r.kind) {
            case BF6_GM_SPAWN: case BF6_GM_SOLDIER_SPAWN:
                o.role = BF6_GMR_SPAWN; break;
            case BF6_GM_VOLUME: {
                if (r.point_count < 3) { st.dropped_other++; continue; }
                if (r.owner_type && r.owner_kind != BF6_GM_COMBAT) { st.dropped_owned++; continue; }
                Vec3 cs{};
                for (int k = 0; k < r.point_count; k++) {
                    const Vec3 w = apply(r.xform, Vec3{ r.points[k * 3], r.points[k * 3 + 1], r.points[k * 3 + 2] });
                    o.wpts.push_back(w.x); o.wpts.push_back(w.y); o.wpts.push_back(w.z);
                    cs.x += w.x; cs.y += w.y; cs.z += w.z;
                }
                const float inv = 1.f / (float)r.point_count;
                o.centre = Vec3{ cs.x * inv, cs.y * inv, cs.z * inv };
                o.area = area_local_xz(r.points, r.point_count);
                o.height = r.height;
                o.extra = std::to_string((int)o.area);
                if (r.owner_type) { o.role = BF6_GMR_COMBAT; o.by_data = true; }
                else o.pending = 1;
                break;
            }
            case BF6_GM_OBB:
                if (part.find("/_layers_gameplay/") == std::string::npos) { st.dropped_prop_box++; continue; }
                if (r.owner_type) { st.dropped_owned++; continue; }
                o.role = BF6_GMR_OBB;
                o.extra = std::to_string((int)(r.half_extents[0] * 2)) + "x" + std::to_string((int)(r.half_extents[1] * 2))
                        + "x" + std::to_string((int)(r.half_extents[2] * 2));
                break;
            case BF6_GM_COMBAT:
                o.pending = 2; break;
            case BF6_GM_OBJECTIVE:
                o.role = BF6_GMR_MCOM; break;
            case BF6_GM_VEHICLE_SPAWN:
                o.role = BF6_GMR_VEHICLE; o.gem_link = "vehiclespawner blueprint"; break;
            case BF6_GM_GEM: {
                const std::string link = r.gem_link ? r.gem_link : "";
                o.gem_link = r.gem_link ? r.gem_link : "";
                o.gem_value = r.gem_value;
                if (bf6gm::starts_with(link, "gem_vehiclespawner") || bf6gm::starts_with(link, "gem_stationaryspawner")) {
                    o.role = BF6_GMR_VEHICLE; o.stationary = bf6gm::starts_with(link, "gem_stationaryspawner");
                }
                else if (bf6gm::starts_with(link, "gem_vehicleresupplystation")) o.role = BF6_GMR_RESUPPLY;
                else if (bf6gm::starts_with(link, "gem_objective_mcom"))        o.role = BF6_GMR_MCOM;
                else if (bf6gm::starts_with(link, "gem_bomb_pickup"))           o.role = BF6_GMR_BOMB;
                else if (bf6gm::starts_with(link, "gem_specialcombatarea"))     o.role = BF6_GMR_SPECIALAREA;
                else if (link.find("vectorshapeasset") != std::string::npos)    o.role = BF6_GMR_SLOT;
                else if (link.empty())                                          o.role = BF6_GMR_UNLINKED;
                else { st.dropped_gem_other++; continue; }   /* areas, HQ/flag/insertion logic */
                break;
            }
            default:
                st.dropped_other++; continue;
        }
        objs.push_back(std::move(o));
    }

    /* DUPLICATES. A mode folded from several numbered partitions can ship the
     * same object from each of them (GolmudRailway's conquest carried every
     * sector twice). Same kind, same world position, same footprint, same
     * team = one object. */
    {
        std::set<std::string> seen;
        std::vector<Obj> uniq;
        for (Obj& o : objs) {
            char key[160];
            std::snprintf(key, sizeof(key), "%d|%d|%d,%d,%d|%s|%d", o.kind, o.pending,
                          (int)std::lround(o.centre.x), (int)std::lround(o.centre.y), (int)std::lround(o.centre.z),
                          o.extra.c_str(), o.team);
            if (!seen.insert(key).second) { st.dropped_dup++; continue; }
            uniq.push_back(std::move(o));
        }
        objs.swap(uniq);
    }

    /* A CombatAreaEntityData is a real statement that a combat area exists
     * here, so the volume nearest one is that area. The bare reference never
     * reaches the consumer. */
    {
        std::vector<Obj> kept;
        std::set<size_t> claimed;
        for (size_t i = 0; i < objs.size(); i++) if (objs[i].pending == 2) {
            size_t best = SIZE_MAX; double bd = 1e300;
            for (size_t j = 0; j < objs.size(); j++) {
                if (objs[j].pending != 1 || claimed.count(j)) continue;
                const double dx = objs[j].centre.x - objs[i].centre.x, dy = objs[j].centre.y - objs[i].centre.y, dz = objs[j].centre.z - objs[i].centre.z;
                const double d = dx * dx + dy * dy + dz * dz;
                if (d < bd) { bd = d; best = j; }
            }
            if (best != SIZE_MAX) { claimed.insert(best); objs[best].pending = 0; objs[best].role = BF6_GMR_COMBAT; }
        }
        for (Obj& o : objs) if (o.pending != 2) kept.push_back(std::move(o));
        objs.swap(kept);
    }

    /* SIZE, then the spawn cluster. On conquest the capture zones are 268-562
     * m2 and the next polygon up is 3,604. Carrier-deck flags are bigger than
     * the band (503-7,069 m2 on MP_Isolated) and every one has 5-9 spawns
     * within 45 m of its centroid, while sectors and boundaries have spread
     * deploys or none - so in the conquest family a bigger polygon with that
     * cluster is a flag. Not breakthrough: its small sectors hold whole deploy
     * clusters and would all promote. */
    std::vector<Vec3> spawn_at;
    for (const Obj& o : objs) if (o.role == BF6_GMR_SPAWN) spawn_at.push_back(o.origin);
    const bool fam = flag_family(mode);
    int caps = 0;
    for (Obj& o : objs) {
        if (o.pending != 1) continue;
        o.pending = 0;
        int near = 0;
        if (fam && o.area > kCaptureMaxArea && o.area <= kBigFlagMaxArea)
            for (const Vec3& s : spawn_at) {
                const float dx = s.x - o.centre.x, dz = s.z - o.centre.z;
                if (dx * dx + dz * dz <= kBigFlagRadius * kBigFlagRadius) near++;
            }
        if (o.area <= kCaptureMaxArea) o.role = BF6_GMR_CAPTURE;
        else if (near >= kBigFlagMinSpawns) { o.role = BF6_GMR_CAPTURE; st.big_flag_rescued++; }
        else o.role = BF6_GMR_ZONE;
        if (o.role == BF6_GMR_CAPTURE) caps++;
    }
    /* ONE OBJECTIVE AUTHORED TWICE. The layers write some objectives as a PAIR
     * of polygons a few metres apart, and a consumer that builds both puts two
     * capture points on one flag. Golmud Conquest is the clean proof: the layer
     * holds twelve, the game plays seven, and the twelve are five pairs
     * 5.3-14.0 m apart plus two singles. THE SMALLER POLYGON IS THE OBJECTIVE -
     * the pair is the authored capture shape plus the 4-point bounding box
     * around it, and keeping the bigger one makes every capture area square and
     * oversized. Area decides; the point count only breaks a tie. The count of
     * polygons the layer authored is captures + dropped_twin, so a reading can
     * still be compared against a raw mine. */
    {
        std::vector<size_t> keep_idx;
        std::vector<char> dropped(objs.size(), 0);
        for (size_t i = 0; i < objs.size(); i++) {
            if (objs[i].role != BF6_GMR_CAPTURE) continue;
            size_t twin = SIZE_MAX;
            for (size_t k : keep_idx) {
                const float dx = objs[k].centre.x - objs[i].centre.x;
                const float dz = objs[k].centre.z - objs[i].centre.z;
                if (dx * dx + dz * dz <= kFlagMergeM * kFlagMergeM) { twin = k; break; }
            }
            if (twin == SIZE_MAX) { keep_idx.push_back(i); continue; }
            /* TWO OBJECTIVES CAN GENUINELY BE THIS CLOSE. MP_Contaminated's
             * Breakthrough plays a pair 2.3 m apart as two capture points, and
             * folding those loses one of the game's ten objectives. What tells
             * the two cases apart is in the data: a double-authored objective
             * is a shape plus the BOX around it, so the bigger polygon is 1.8x
             * to 5.7x the smaller (measured on all five of Golmud Conquest's
             * pairs), while two real objectives are two capture areas of much
             * the same size. Under 1.5x, both are kept and the build check
             * ("two capture points 2.3 m apart") puts it in front of the
             * creator instead of deciding for them. */
            {
                const float big = std::max(objs[i].area, objs[twin].area);
                const float small = std::min(objs[i].area, objs[twin].area);
                if (small > 0.f && big < small * 1.5f) { keep_idx.push_back(i); continue; }
            }
            const bool i_is_better = objs[i].area != objs[twin].area
                ? objs[i].area < objs[twin].area
                : objs[i].wpts.size() > objs[twin].wpts.size();
            if (i_is_better) {
                dropped[twin] = 1;
                for (size_t& k : keep_idx) if (k == twin) k = i;
            } else {
                dropped[i] = 1;
            }
            st.dropped_twin++;
            caps--;
        }
        if (st.dropped_twin > 0) {
            std::vector<Obj> kept;
            kept.reserve(objs.size());
            for (size_t i = 0; i < objs.size(); i++) if (!dropped[i]) kept.push_back(std::move(objs[i]));
            objs.swap(kept);
        }
    }

    if (caps > kSanityMaxFlags) {
        /* no Battlefield mode has this many flags: the claim is wrong
         * somewhere, so nothing is called a flag and every polygon ships as a
         * zone to be promoted by hand */
        st.sanity_capped = 1;
        st.big_flag_rescued = 0;
        for (Obj& o : objs) if (o.role == BF6_GMR_CAPTURE && !o.by_data) o.role = BF6_GMR_ZONE;
    }

    /* NAMES. Flags lettered by position, not walk order, so the letters hold
     * across runs; a spawn takes the letter of the flag it is AT, and only
     * that - nearest-unconditionally handed one flag 36 of 137 spawns. */
    std::vector<size_t> flags;
    for (size_t i = 0; i < objs.size(); i++) if (objs[i].role == BF6_GMR_CAPTURE) flags.push_back(i);
    std::sort(flags.begin(), flags.end(), [&](size_t a, size_t b) {
        const Vec3& pa = objs[a].centre; const Vec3& pb = objs[b].centre;
        return std::fabs(pa.x - pb.x) > 1e-3f ? pa.x < pb.x : pa.z < pb.z;
    });
    for (size_t k = 0; k < flags.size(); k++) { objs[flags[k]].label = "Flag " + letter((int)k); objs[flags[k]].flag = (int)k; }
    std::map<int, int> counters;
    for (Obj& o : objs) {
        if (o.role == BF6_GMR_CAPTURE) continue;
        if (o.role == BF6_GMR_SPAWN) {
            int nf = -1; double bd = 1e300;
            for (size_t k = 0; k < flags.size(); k++) {
                const Vec3& fc = objs[flags[k]].centre;
                const double dx = fc.x - o.origin.x, dy = fc.y - o.origin.y, dz = fc.z - o.origin.z;
                const double d = dx * dx + dy * dy + dz * dz;
                if (d < bd) { bd = d; nf = (int)k; }
            }
            if (nf >= 0 && bd <= (double)kSpawnClaimRadius * kSpawnClaimRadius) { o.flag = nf; o.label = objs[flags[(size_t)nf]].label + " Spawn"; }
            else o.label = "Spawn Point";
            continue;
        }
        const char* base = "Object";
        switch (o.role) {
            case BF6_GMR_ZONE: base = "Zone"; break;           case BF6_GMR_COMBAT: base = "Combat Area"; break;
            case BF6_GMR_OBB: base = "Box"; break;             case BF6_GMR_VEHICLE: base = o.stationary ? "Emplacement" : "Vehicle"; break;
            case BF6_GMR_RESUPPLY: base = "Resupply"; break;   case BF6_GMR_MCOM: base = "MCOM"; break;
            case BF6_GMR_BOMB: base = "Bomb"; break;           case BF6_GMR_SPECIALAREA: base = "Special Area"; break;
            case BF6_GMR_SLOT: base = "Slot"; break;           case BF6_GMR_UNLINKED: base = "Unlinked Gem"; break;
            default: break;
        }
        o.label = std::string(base) + " " + std::to_string(++counters[o.role]);
    }

    /* THE GROUND. The slot's number is not its vehicle, but the terrain under
     * it is unambiguous: below the water it is a boat slot. A zone's landing
     * spot is the nearest point to its centroid that is inside the polygon
     * and on land, with the real height - an HQ dropped on a centroid over
     * the sea had players parachuting in. And a 15 m grid over the mode, so a
     * builder can put anything on the ground. */
    const Ground& g = ground_for(c, level);
    st.has_terrain = g.ok ? 1 : 0;
    st.water_y = g.water_y;
    const bool has_water = g.water_y > kNoHeight * 0.5f;
    float lo_x = 1e30f, lo_z = 1e30f, hi_x = -1e30f, hi_z = -1e30f;
    for (Obj& o : objs) {
        lo_x = std::min(lo_x, o.origin.x); hi_x = std::max(hi_x, o.origin.x);
        lo_z = std::min(lo_z, o.origin.z); hi_z = std::max(hi_z, o.origin.z);
        if (!g.ok) continue;
        switch (o.role) {
            case BF6_GMR_VEHICLE: case BF6_GMR_SLOT: case BF6_GMR_RESUPPLY: case BF6_GMR_UNLINKED:
            case BF6_GMR_MCOM: case BF6_GMR_BOMB: {
                const float gh = height_at(g, o.origin.x, o.origin.z);
                if (gh > kNoHeight * 0.5f) {
                    o.has_ground = true;
                    o.ground_y = std::round(gh * 10.f) / 10.f;
                    o.water = has_water && gh < g.water_y + kBeachBand;
                }
                break;
            }
            case BF6_GMR_ZONE: case BF6_GMR_CAPTURE: {
                static const float radii[] = { 0.f, 8.f, 16.f, 28.f, 44.f, 64.f, 90.f, 125.f, 170.f, 230.f, 300.f };
                for (float r : radii) {
                    const int steps = r == 0.f ? 1 : 12;
                    for (int i = 0; i < steps && !o.has_land; i++) {
                        const double a = 6.283185307179586 * i / steps;
                        const float x = o.centre.x + (float)std::cos(a) * r, z = o.centre.z + (float)std::sin(a) * r;
                        if (!in_poly_xz(o.wpts, x, z)) continue;
                        const float gh = height_at(g, x, z);
                        if (gh <= kNoHeight * 0.5f) continue;
                        if (!has_water || gh >= g.water_y + kLandClearance) { o.has_land = true; o.land = Vec3{ x, gh, z }; }
                    }
                    if (o.has_land) break;
                }
                break;
            }
            default: break;
        }
    }
    if (g.ok && !objs.empty()) {
        st.grid_step = kGridStep;
        st.grid_x0 = lo_x - kGridMargin; st.grid_z0 = lo_z - kGridMargin;
        st.grid_nx = std::min((int)((hi_x - lo_x + kGridMargin * 2.f) / kGridStep) + 2, kGridMax);
        st.grid_nz = std::min((int)((hi_z - lo_z + kGridMargin * 2.f) / kGridStep) + 2, kGridMax);
        L.grid.resize((size_t)st.grid_nx * (size_t)st.grid_nz);
        for (int iz = 0; iz < st.grid_nz; iz++)
            for (int ix = 0; ix < st.grid_nx; ix++) {
                const float gh = height_at(g, st.grid_x0 + ix * kGridStep, st.grid_z0 + iz * kGridStep);
                L.grid[(size_t)iz * st.grid_nx + ix] = gh > kNoHeight * 0.5f ? std::round(gh * 10.f) / 10.f : kNoHeight;
            }
        st.grid_h = L.grid.data();
    }

    /* publish */
    auto intern = [&L](const std::string& s) -> const char* {
        auto it = L.strings.find(s);
        if (it == L.strings.end()) it = L.strings.insert(s).first;
        return it->c_str();
    };
    L.objs = std::move(objs);
    L.rows.resize(L.objs.size());
    for (size_t i = 0; i < L.objs.size(); i++) {
        const Obj& o = L.objs[i];
        bf6_gm_object& r = L.rows[i];
        r = bf6_gm_object{};
        r.entity = o.entity; r.role = o.role; r.label = intern(o.label); r.flag = o.flag; r.team = o.team;
        r.area_m2 = o.area;
        r.centre[0] = o.centre.x; r.centre[1] = o.centre.y; r.centre[2] = o.centre.z;
        r.world_points = o.wpts.empty() ? nullptr : o.wpts.data();
        r.point_count = (int32_t)(o.wpts.size() / 3);
        r.height = o.height;
        r.stationary = o.stationary ? 1u : 0u; r.by_data = o.by_data ? 1u : 0u;
        r.has_ground = o.has_ground ? 1u : 0u; r.water = o.water ? 1u : 0u; r.ground_y = o.ground_y;
        r.has_land = o.has_land ? 1u : 0u; r.land[0] = o.land.x; r.land[1] = o.land.y; r.land[2] = o.land.z;
        r.gem_link = o.gem_link ? intern(o.gem_link) : nullptr;
        r.gem_value = o.gem_value;
        st.objects++;
        switch (o.role) {
            case BF6_GMR_SPAWN: st.spawns++; break;        case BF6_GMR_CAPTURE: st.captures++; break;
            case BF6_GMR_ZONE: st.zones++; break;          case BF6_GMR_COMBAT: st.combat++; break;
            case BF6_GMR_OBB: st.obbs++; break;            case BF6_GMR_VEHICLE: st.vehicles++; break;
            case BF6_GMR_RESUPPLY: st.resupply++; break;   case BF6_GMR_MCOM: st.mcoms++; break;
            case BF6_GMR_BOMB: st.bombs++; break;          case BF6_GMR_SPECIALAREA: st.specialareas++; break;
            case BF6_GMR_SLOT: st.slots++; break;          case BF6_GMR_UNLINKED: st.unlinked++; break;
            default: break;
        }
        if (o.gem_link) { st.gems++; if (!*o.gem_link) st.gems_unlinked++; }
    }
}

}  // namespace bf6gmlay

int bf6_level_gamemode_layout(bf6_ctx* c, const char* level, const char* mode,
                              bf6_gm_object* out, int out_max,
                              bf6_gm_layout* stats, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level || !mode || !*mode) return fail("bad arguments");
    if (c->gm_level != level) {
        const int n = bf6_level_gamemodes(c, level, nullptr, 0, nullptr, err, err_len);
        if (n <= 0) return 0;
    }
    bf6gmlay::Layout& L = bf6gmlay::g_layout[c];
    if (L.level != level || L.mode != mode) bf6gmlay::build(c, level, mode, L);
    if (stats) *stats = L.st;
    const int n = (int)L.rows.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = L.rows[(size_t)i];
    if (n == 0) return fail(std::string("mode ") + mode + " has no gameplay object on the layers of " + level);
    return n;
}

int bf6_level_gamemode_ground(bf6_ctx* c, const char* level,
                              const float* xz, int count, float* out_y,
                              float* out_water_y, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    const bf6gmlay::Ground& g = bf6gmlay::ground_for(c, level);
    if (out_water_y) *out_water_y = g.water_y;
    if (!g.ok) return fail(std::string("no heightfield for ") + level);
    if (!xz || !out_y || count <= 0) return 0;
    for (int i = 0; i < count; i++) out_y[i] = bf6gmlay::height_at(g, xz[i * 2], xz[i * 2 + 1]);
    return count;
}
