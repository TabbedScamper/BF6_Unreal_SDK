/* beaufort_ext.inc - the ocean's SEA STATE, read from the game at runtime.
 *
 * NEW FILE. A translation-unit fragment, not a standalone .cpp: it uses
 * bf6_ctx, which only exists inside bf6_core.cpp. Wired with one line in
 * src/bf6_core.cpp beside the other _ext.inc includes, INSIDE the extern "C"
 * block so the entry point gets C linkage.
 *
 * WHY THIS EXISTS. A level's cascades author a wind (mp_isolated: 0.914,
 * 0.010, 1.000, 1.000) that produces a millimetre sea. The sea actually played
 * is chosen by a BEAUFORT FORCE, and the force is converted to parameters by a
 * shared curve collection. Both are authored data, so a consumer should read
 * them rather than bake a number: this call is what stops
 * "wind = 13.9f" appearing in a caller's source.
 *
 * THE CHAIN, all verified against the shipped data:
 *
 *   <level>/..._schematic
 *     -> an instance of WaterOceanBeaufortMappingEntityData
 *        (e7aa420c-d309-afce-9ffa-3755368b47c3), whose one scalar is the force
 *   common/artsetup/oceancurves/fcc_ocean   (53b622fd-...-605aa497)
 *     -> [0] a collection, field 0x73c94123, ten instance references
 *     -> ten FloatCurve instances (0e26f168-a2df-d74e-807b-19103d97b179)
 *        keys in array 0xedfc6df6, each key seven fields:
 *          0x3901db14 X (the Beaufort force, domain 0..12)
 *          0x42fc0f5e Y (the parameter value)
 *          0x0b47f861 / 0x72439608  in-tangent  (dx, dy)
 *          0x44b6bf62 / 0x6658277c  out-tangent (dx, dy)
 *          0xe6fb9626 an interpolation flag, observed 1 and 2
 *
 * HONESTY, twice.
 *
 * 1. The force field is identified by POSITION, not by name: it is the lone
 *    scalar on a Beaufort mapping entity and it sits inside the curves' own
 *    authored 0..12 domain. The cross-level control could not run - of seven
 *    levels checked only mp_isolated authors a mapping at all - so there is no
 *    second value to compare against. found=1 means "we read the scalar we
 *    believe is the force", not "the field is named Force".
 *
 * 2. Only the WIND curve is identified, and it is identified by its VALUES:
 *    it is the one whose range reaches 40 while every other curve in the
 *    collection tops out at 8 or below, and its thirteen keys reproduce the
 *    meteorological Beaufort table in m/s (force 2 -> 2.45, 6 -> 12.3,
 *    8 -> 18.95, 10 -> 26.45). What the other nine drive is NOT known, so they
 *    are returned evaluated but unnamed, in collection order, for a caller that
 *    later works out the binding.
 */

/* WaterOceanBeaufortMappingEntityData, derived from the name by the module
 * digest rule and confirmed against the instance in the schematic. */
static const char* kBeaufortTypeGuid = "e7aa420c-d309-afce-9ffa-3755368b47c3";
static const char* kFloatCurveGuid   = "0e26f168-a2df-d74e-807b-19103d97b179";

static const uint32_t kBeaufortForce   = 0x68521a40;  /* the lone scalar      */
static const uint32_t kCurveCollection = 0x73c94123;  /* array of curve refs  */
static const uint32_t kCurveKeys       = 0xedfc6df6;  /* array of keys        */
static const uint32_t kKeyX            = 0x3901db14;
static const uint32_t kKeyY            = 0x42fc0f5e;
static const uint32_t kKeyInDx         = 0x0b47f861;
static const uint32_t kKeyInDy         = 0x72439608;
static const uint32_t kKeyOutDx        = 0x44b6bf62;
static const uint32_t kKeyOutDy        = 0x6658277c;
static const uint32_t kKeyType         = 0xe6fb9626;  /* FloatCurveType    */

/* decom-proj/BF6SDK/FloatCurveType.cs: Spline=0, Smooth=1, Linear=2,
 * NearestValue=3. mp_isolated's wind curve is type 2 at EVERY key. */
enum : int32_t {
    kCurveSpline = 0, kCurveSmooth = 1, kCurveLinear = 2, kCurveNearest = 3
};

struct BF6_CurveKey {
    float x = 0.f, y = 0.f;
    float in_dx = 0.f, in_dy = 0.f;
    float out_dx = 0.f, out_dy = 0.f;
    int32_t type = kCurveLinear;
};

/* Read one FloatCurve instance's keys. Returns false when the instance does not
 * carry a key array, which is how a wrong instance index fails loudly. */
static bool BF6_ReadFloatCurve(const bf6::EbxValue& inst,
                               std::vector<BF6_CurveKey>& keys)
{
    keys.clear();
    const bf6::EbxValue* arr = inst.field(kCurveKeys);
    if (!arr || arr->kind != bf6::EbxValue::Kind::Array) return false;
    keys.reserve(arr->items.size());
    for (const bf6::EbxValue& k : arr->items) {
        if (k.kind != bf6::EbxValue::Kind::Struct) continue;
        BF6_CurveKey key;
        key.x      = BF6_SimF(k, kKeyX, 0.f);
        key.y      = BF6_SimF(k, kKeyY, 0.f);
        key.in_dx  = BF6_SimF(k, kKeyInDx, 0.f);
        key.in_dy  = BF6_SimF(k, kKeyInDy, 0.f);
        key.out_dx = BF6_SimF(k, kKeyOutDx, 0.f);
        key.out_dy = BF6_SimF(k, kKeyOutDy, 0.f);
        key.type   = (int32_t)BF6_SimF(k, kKeyType, (float)kCurveLinear);
        keys.push_back(key);
    }
    return !keys.empty();
}

/* Evaluate a FloatCurve, honouring the per-key interpolation mode.
 *
 * WHY THIS IS NOT ALWAYS HERMITE. It was, and that was wrong. The SDK's own
 * FloatCurveType enum reads Spline=0, Smooth=1, LINEAR=2, NearestValue=3, and
 * mp_isolated's wind curve carries type 2 at every one of its thirteen keys.
 *
 * The mistake survived because the test could not see it. The level's force is
 * 6.5, which is the MIDPOINT of the [6,7] interval, and at a midpoint a linear
 * read and a zero-slope Hermite return the same number - both give 13.9. They
 * only separate off-centre: at 6.25 linear gives 13.1 and zero-slope Hermite
 * gives 12.8. So reproducing 13.9 validated neither algorithm, and the shipped
 * value was right by luck while every other force was wrong.
 *
 * Tangents are DIRECTION VECTORS, so a slope is dy/dx of the relevant handle:
 * the left key's out handle and the right key's in handle. The in handle points
 * backwards (dx negative) and dividing keeps the sign correct.
 *
 * HONESTY: Linear and NearestValue are exact here. Spline and Smooth are NOT
 * established - the engine's basis and segment selection for those modes have
 * not been recovered, and an independent reader declines them rather than
 * guessing. They fall through to the Hermite form below, which is a plausible
 * shape and not a verified one. No curve in fcc_ocean uses them so far.
 */
static float BF6_EvalFloatCurve(const std::vector<BF6_CurveKey>& k, float x)
{
    if (k.empty()) return 0.f;
    if (k.size() == 1) return k[0].y;
    if (x <= k.front().x) return k.front().y;
    if (x >= k.back().x)  return k.back().y;

    size_t i = 0;
    for (; i + 1 < k.size(); ++i)
        if (k[i].x <= x && x < k[i + 1].x) break;
    if (i + 1 >= k.size()) return k.back().y;

    const float x0 = k[i].x, x1 = k[i + 1].x;
    const float h = x1 - x0;
    if (h < 1.0e-6f) return k[i + 1].y;
    const float t = (x - x0) / h;

    /* The segment's mode is the LEFT key's: it owns the interval leaving it. */
    switch (k[i].type) {
    case kCurveLinear:
        return k[i].y + (k[i + 1].y - k[i].y) * t;
    case kCurveNearest:
        return t < 0.5f ? k[i].y : k[i + 1].y;
    default:
        break;
    }

    const float m0 = std::fabs(k[i].out_dx) > 1.0e-6f
        ? k[i].out_dy / k[i].out_dx : 0.f;
    const float m1 = std::fabs(k[i + 1].in_dx) > 1.0e-6f
        ? k[i + 1].in_dy / k[i + 1].in_dx : 0.f;

    const float t2 = t * t, t3 = t2 * t;
    return (2.f * t3 - 3.f * t2 + 1.f) * k[i].y
         + (t3 - 2.f * t2 + t) * m0 * h
         + (-2.f * t3 + 3.f * t2) * k[i + 1].y
         + (t3 - t2) * m1 * h;
}

/* The level's authored Beaufort force. Returns false when the level authors no
 * mapping at all, which is the common case: of seven levels checked only
 * mp_isolated carries one. */
static bool BF6_ReadBeaufortForce(bf6_ctx* c, const std::string& lvl, float& force)
{
    if (!c || !c->types || lvl.empty()) return false;
    std::vector<std::string> named, rest;
    for (const auto& kv : c->src.ebx()) {
        const std::string& n = kv.first;
        if (n.compare(0, lvl.size(), lvl) != 0) continue;
        if (n.find("schematic") == std::string::npos) continue;
        (n.find("water") != std::string::npos ? named : rest).push_back(n);
    }
    std::sort(named.begin(), named.end());
    std::sort(rest.begin(), rest.end());
    named.insert(named.end(), rest.begin(), rest.end());

    for (const std::string& part : named) {
        std::string err;
        std::vector<uint8_t> raw = c->src.get_ebx(part, err);
        if (raw.empty()) continue;
        bf6::Ebx e(*c->types);
        if (!e.parse(std::move(raw), err)) continue;
        for (size_t i = 0; i < e.instance_count(); ++i) {
            if (bf6::TypeDb::guid_str(e.instance_type(i)) != kBeaufortTypeGuid)
                continue;
            const bf6::EbxValue d = e.read_instance(i);
            const bf6::EbxValue* f = d.field(kBeaufortForce);
            if (!f || f->kind != bf6::EbxValue::Kind::Real) continue;
            force = (float)f->f;
            return true;
        }
    }
    return false;
}

extern "C" int bf6_level_ocean_sea_state(bf6_ctx* c, const char* level,
                                         bf6_ocean_sea_state* out)
{
    if (!out) return 0;
    *out = bf6_ocean_sea_state{};
    out->wind_curve_index = -1;
    if (!c || !level || !*level || !c->types || !c->walk) return 0;
    if (c->walked_level != level) return 0;

    /* Same level-directory derivation as the cascade reader, so the two calls
     * cannot disagree about which level they are describing. */
    std::string lvl = c->walk->root;
    if (lvl.size() > 4 && lvl.compare(lvl.size() - 4, 4, ".ebx") == 0)
        lvl.resize(lvl.size() - 4);
    const size_t slash = lvl.find_last_of('/');
    lvl = slash == std::string::npos ? std::string() : lvl.substr(0, slash);

    float force = 0.f;
    if (!BF6_ReadBeaufortForce(c, lvl, force)) return 0;
    out->found = 1;
    out->force = force;

    /* The shared curve collection. Located by name: it is a single asset under
     * common/, referenced by partition guid from the schematic. */
    std::string curves_part;
    for (const auto& kv : c->src.ebx()) {
        if (kv.first.find("oceancurves/fcc_ocean") != std::string::npos) {
            curves_part = kv.first;
            break;
        }
    }
    if (curves_part.empty()) return 1;   /* force is still valid on its own */

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(curves_part, err);
    if (raw.empty()) return 1;
    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return 1;

    /* The collection instance carries the ordered references; the order is the
     * collection's, not the instance numbering, and it is preserved here. */
    std::vector<int32_t> order;
    for (size_t i = 0; i < e.instance_count(); ++i) {
        const bf6::EbxValue d = e.read_instance(i);
        const bf6::EbxValue* arr = d.field(kCurveCollection);
        if (!arr || arr->kind != bf6::EbxValue::Kind::Array) continue;
        for (const bf6::EbxValue& ref : arr->items)
            if (ref.kind == bf6::EbxValue::Kind::InstanceRef && ref.instance >= 0)
                order.push_back(ref.instance);
        if (!order.empty()) break;
    }
    if (order.empty()) return 1;

    const int count = (int)std::min<size_t>(order.size(), 16);
    out->curve_count = count;

    float best_max = 0.f;
    for (int slot = 0; slot < count; ++slot) {
        const size_t idx = (size_t)order[(size_t)slot];
        if (idx >= e.instance_count()) continue;
        if (bf6::TypeDb::guid_str(e.instance_type(idx)) != kFloatCurveGuid) continue;
        const bf6::EbxValue inst = e.read_instance(idx);
        std::vector<BF6_CurveKey> keys;
        if (!BF6_ReadFloatCurve(inst, keys)) continue;
        out->curve_value[slot] = BF6_EvalFloatCurve(keys, force);

        /* Identify wind by RANGE, not by slot. Every other curve in this
         * collection tops out at 8 or below; the wind curve reaches 40. */
        float mx = 0.f;
        for (const BF6_CurveKey& k : keys) mx = std::max(mx, k.y);
        if (mx > 20.f && mx > best_max) {
            best_max = mx;
            out->wind_curve_index = slot;
            out->wind_mps = out->curve_value[slot];
        }
    }
    return 1;
}
