/* TELEMETRY SCORING ENUMS: what each game mode reports about scoring.
 *
 * `common/telemetry/gamemodescoringsettings/<mode>_scoringtelemetryenum` is an
 * enum whose MEMBERS name the scoring metrics that mode emits - Conquest ships
 * current_tickets, kill_tickets and majority_bleed.
 *
 * A member instance (type 469a734c-7247-12c4-b34d-3f2436954754) carries:
 *
 *     0x4A8FC6B0  ordinal
 *     0x0C59FA06  name
 *     0xC124C7D8  a 32-bit id
 *     0x96E1B4F3  back-reference to the enum root
 *     0x8CF424E7  value
 *
 * THE 32-BIT ID IS NOT AN FNV HASH OF THE NAME. Both FNV-1 and FNV-1a with the
 * engine's usual 0x811c9dc5 seed were scored against three members and matched
 * 0 of 3, so the id is left raw rather than guessed at.
 *
 * TWO MEMBER TYPES SHARE THIS SHAPE. Scoring enums use
 * 469a734c-7247-12c4-b34d-3f2436954754 with an integer ordinal at 0x4A8FC6B0;
 * the round-reason enums under common/telemetry/round/ use
 * ccdf7446-8133-14d9-15b1-f0411409ae48, which has a STRING at 0x6A1E3529 in
 * that slot instead. Requiring the ordinal therefore rejected every
 * round-reason member - the reader selects on name, id and value, which both
 * families carry, and falls back to `value` when no ordinal is present.
 *
 * Members are stored in arbitrary instance order - Conquest's arrive 1, 0, 2 -
 * so a consumer wanting enum order must sort by `value`. This reader does not
 * sort; it reports what the file holds. */

static const uint32_t kTelOrdinal = 0x4a8fc6b0u;
static const uint32_t kTelName    = 0x0c59fa06u;
static const uint32_t kTelId      = 0xc124c7d8u;
static const uint32_t kTelValue   = 0x8cf424e7u;

struct TelemetryEnumHandle {
    bf6_telemetry_enum                e{};
    std::vector<bf6_telemetry_member> members;
    std::vector<std::string>          names;
    std::string                       asset;
};

static int32_t bf6__tel_int(const bf6::EbxValue* v)
{
    if (!v) return 0;
    if (v->kind == bf6::EbxValue::Kind::Int)  return (int32_t)v->i;
    if (v->kind == bf6::EbxValue::Kind::Uint) return (int32_t)v->u;
    if (v->kind == bf6::EbxValue::Kind::Real) return (int32_t)v->f;
    return 0;
}

bf6_telemetry_enum* bf6_telemetry_enum_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(name, err);
    if (raw.empty()) return nullptr;

    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return nullptr;

    TelemetryEnumHandle* h = new TelemetryEnumHandle();
    h->asset = name;

    /* Select on CONTENT: a member carries a name, an id and a value. The root
     * instance has a name too, so requiring the id and value fields is what
     * separates members from it. */
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::EbxValue v = e.read_instance(i);
        const bf6::EbxValue* nm = v.field(kTelName);
        const bf6::EbxValue* id = v.field(kTelId);
        const bf6::EbxValue* va = v.field(kTelValue);
        if (!nm || !id || !va) continue;   /* ordinal is OPTIONAL - see note */
        if (nm->kind != bf6::EbxValue::Kind::Str || nm->s.empty()) continue;
        h->names.push_back(nm->s);
    }
    h->members.resize(h->names.size());
    size_t k = 0;
    for (size_t i = 0; i < e.instance_count() && k < h->members.size(); i++)
    {
        bf6::EbxValue v = e.read_instance(i);
        const bf6::EbxValue* nm = v.field(kTelName);
        const bf6::EbxValue* id = v.field(kTelId);
        const bf6::EbxValue* va = v.field(kTelValue);
        const bf6::EbxValue* od = v.field(kTelOrdinal);
        if (!nm || !id || !va) continue;   /* ordinal is OPTIONAL - see note */
        if (nm->kind != bf6::EbxValue::Kind::Str || nm->s.empty()) continue;
        h->members[k].value   = bf6__tel_int(va);
        h->members[k].ordinal = od ? bf6__tel_int(od) : bf6__tel_int(va);
        h->members[k].id      = (uint32_t)bf6__tel_int(id);
        h->members[k].name    = h->names[k].c_str();
        k++;
    }

    h->e.count   = (int32_t)h->members.size();
    h->e.members = h->members.empty() ? nullptr : h->members.data();
    h->e.name    = h->asset.c_str();
    c->handles[&h->e] = bf6_ctx::HK_TELEMETRY;
    return &h->e;
}

void bf6__telemetry_delete(void* p) { delete (TelemetryEnumHandle*)p; }
