/* WEATHER: the global weather-state variable database.
 *
 * There is NO authored storm type in this engine - the stems Storm, Disaster,
 * Tornado, Sandstorm, Wander and Twister return zero of the 4,376 authored
 * data types. The WeatherSequencer state machine exists as 10 reflected types
 * and ships ONE instance across 28 levels. What ships is a single global asset,
 * `globals/weather/weatherstatevariabledatabase`, holding the variables every
 * other system reads:
 *
 *     70642eb8  WeatherSequencerVariableSet   Name + VariableCategories[]
 *     c367e1b1  WSFloatVariable  0x40
 *        0x820EC965 DebugName   0xC124C7D8 NameHash
 *        0x42C8B257 DefaultValue
 *        0x46040F97 FadeLowThreshold   0x49DAFD7B FadeHighThreshold
 *
 * So a storm is a VARIABLE - `Global_Storm_Active` - not an entity that
 * wanders, and the fade thresholds are the transition window.
 *
 * The NameHash is djb2-exact of the DebugName, verified 21 of 21 here (see
 * bf6_name_hash). A stored 0 would mean "not baked"; none are. */

static const uint32_t kWsDebugName = 0x820ec965u;
static const uint32_t kWsNameHash  = 0xc124c7d8u;
static const uint32_t kWsDefault   = 0x42c8b257u;
static const uint32_t kWsFadeLow   = 0x46040f97u;
static const uint32_t kWsFadeHigh  = 0x49dafd7bu;

/* Mixed-endian on disk. */
static const uint8_t kWsFloatVarType[16] = {
    0xb1,0xe1,0x67,0xc3, 0x1b,0xeb, 0x26,0x9c, 0x5b,0xa3,0x8f,0xe7,0x65,0xf3,0x77,0xff
};
static const uint8_t kWsBoolVarType[16] = {
    0x45,0x9b,0xe3,0x1e, 0x96,0x0d, 0xe5,0xda, 0x28,0x3c,0xa4,0x97,0x07,0x5c,0xe2,0xcf
};
static const uint8_t kWsIntVarType[16] = {
    0x35,0x4a,0xd7,0x21, 0x08,0x6a, 0x5a,0xa2, 0x2d,0x55,0x04,0x26,0x8b,0x74,0x63,0x24
};

/* A 32-bit HASH MUST NOT GO THROUGH A FLOAT. bf6__spawn_num returns float, and
 * float32 has 24 bits of mantissa: NameHash 0xA75E6E5F round-trips to
 * 0xA75E6E00, silently losing the low byte. The first version of this reader
 * did exactly that and reported 0 of 15 hashes matching djb2-exact - which
 * looked like the hash finding being wrong rather than the reader being wrong.
 * Read the raw integer. */
static uint32_t bf6__ws_u32(const bf6::EbxValue* v)
{
    if (!v) return 0;
    if (v->kind == bf6::EbxValue::Kind::Uint) return (uint32_t)v->u;
    if (v->kind == bf6::EbxValue::Kind::Int)  return (uint32_t)v->i;
    return 0;
}

struct WeatherHandle {
    bf6_weather                    s{};
    std::vector<bf6_weather_var>   vars;
    std::vector<std::string>       names;
    std::vector<const char*>       nameptr;
};

bf6_weather* bf6_weather_read(bf6_ctx* c, const char* name)
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

    WeatherHandle* h = new WeatherHandle();
    h->names.reserve(64);

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid tg = e.instance_type(i);
        const bool isf = memcmp(tg.data(), kWsFloatVarType, 16) == 0;
        const bool isb = memcmp(tg.data(), kWsBoolVarType,  16) == 0;
        const bool isi = memcmp(tg.data(), kWsIntVarType,   16) == 0;
        if (!isf && !isb && !isi) continue;
        bf6::EbxValue v = e.read_instance(i);
        bf6_weather_var w{};
        w.name_index = -1;
        if (const bf6::EbxValue* f = v.field(kWsDebugName))
            if (f->kind == bf6::EbxValue::Kind::Str && !f->s.empty()) {
                h->names.push_back(f->s);
                w.name_index = (int32_t)h->names.size() - 1;
            }
        w.name_hash = bf6__ws_u32(v.field(kWsNameHash));
        w.kind = isf ? 0u : (isb ? 1u : 2u);
        if (const bf6::EbxValue* f = v.field(kWsDefault))   w.default_value = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kWsFadeLow))   w.fade_low = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kWsFadeHigh))  w.fade_high = bf6__spawn_num(*f);
        h->vars.push_back(w);
    }

    h->s.count = (int32_t)h->vars.size();
    h->s.vars  = h->vars.empty() ? nullptr : h->vars.data();
    h->s.name_count = (int32_t)h->names.size();
    h->nameptr.clear();
    for (const std::string& s : h->names) h->nameptr.push_back(s.c_str());
    h->s.names = h->nameptr.empty() ? nullptr : h->nameptr.data();
    c->handles[&h->s] = bf6_ctx::HK_WEATHER;
    return &h->s;
}

void bf6__weather_delete(void* p) { delete (WeatherHandle*)p; }
