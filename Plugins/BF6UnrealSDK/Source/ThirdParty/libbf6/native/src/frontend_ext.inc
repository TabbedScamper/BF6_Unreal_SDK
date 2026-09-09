/* Front-end runtime anchors resolved from the installed prefab's own link
 * graph. Public definitions inherit C linkage from bf6_core.h. */

namespace {

constexpr uint32_t kFrontendLinkConnections = 0xF7791E41u;
constexpr uint32_t kFrontendLinkSource       = 0x4676BA2Cu;
constexpr uint32_t kFrontendLinkTarget       = 0x645B319Cu;
constexpr uint32_t kFrontendLinkSourceField  = 0xC41FB636u;
constexpr uint32_t kFrontendLinkTargetField  = 0x2D7314C9u;
constexpr uint32_t kFrontendTransform        = 0xD6351EDEu;
constexpr uint32_t kFrontendLtRight          = 0xC478CC3Bu;
constexpr uint32_t kFrontendLtUp             = 0xBF151EF9u;
constexpr uint32_t kFrontendLtForward        = 0x695D12A4u;
constexpr uint32_t kFrontendLtTranslation    = 0xBC4B07B4u;
constexpr uint32_t kFrontendVecX             = 956422932u;
constexpr uint32_t kFrontendVecY             = 1123815262u;
constexpr uint32_t kFrontendVecZ             = 849976220u;

uint32_t bf6__frontend_name_hash(const char* value)
{
    uint32_t hash = 5381u;
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(value); *p; ++p)
        hash = (hash * 33u) ^ static_cast<uint32_t>(*p);
    return hash;
}

int bf6__frontend_ref(const bf6::EbxValue* value)
{
    return value && value->kind == bf6::EbxValue::Kind::InstanceRef
        ? value->instance : -1;
}

bool bf6__frontend_u32(const bf6::EbxValue* value, uint32_t& out)
{
    if (!value) return false;
    if (value->kind == bf6::EbxValue::Kind::Uint) {
        out = static_cast<uint32_t>(value->u); return true;
    }
    if (value->kind == bf6::EbxValue::Kind::Int) {
        out = static_cast<uint32_t>(value->i); return true;
    }
    return false;
}

bool bf6__frontend_float(const bf6::EbxValue* value, float& out)
{
    if (!value) return false;
    if (value->kind == bf6::EbxValue::Kind::Real) {
        out = static_cast<float>(value->f); return std::isfinite(out);
    }
    return false;
}

bool bf6__frontend_vec3(const bf6::EbxValue* value, float out[3])
{
    return value && value->kind == bf6::EbxValue::Kind::Struct &&
        bf6__frontend_float(value->field(kFrontendVecX), out[0]) &&
        bf6__frontend_float(value->field(kFrontendVecY), out[1]) &&
        bf6__frontend_float(value->field(kFrontendVecZ), out[2]);
}

bool bf6__frontend_transform_value(const bf6::EbxValue& instance,
                                    float out[12])
{
    const bf6::EbxValue* transform = instance.field(kFrontendTransform);
    if (!transform || transform->kind != bf6::EbxValue::Kind::Struct)
        return false;
    const uint32_t fields[4] = {
        kFrontendLtRight, kFrontendLtUp, kFrontendLtForward,
        kFrontendLtTranslation
    };
    for (int column = 0; column < 4; ++column)
        if (!bf6__frontend_vec3(transform->field(fields[column]),
                                out + column * 3))
            return false;
    return true;
}

} // namespace

int bf6_frontend_named_transform_read(
    bf6_ctx* c, const char* prefab, const char* property_name,
    bf6_frontend_named_transform* out, char* err, int err_len)
{
    auto fail = [&](const std::string& message) {
        if (out) *out = bf6_frontend_named_transform{};
        if (err && err_len > 0)
            std::snprintf(err, static_cast<size_t>(err_len), "%s",
                          message.c_str());
        return 0;
    };
    if (!c || !prefab || !*prefab || !property_name || !*property_name || !out)
        return fail("bad arguments");

    std::string parse_error;
    if (!ensure_types(c, parse_error)) return fail(parse_error);
    std::vector<uint8_t> raw = c->src.get_ebx(prefab, parse_error);
    if (raw.empty())
        return fail(parse_error.empty() ? "front-end prefab not mounted" :
                                          parse_error);

    bf6::Ebx ebx(*c->types);
    ebx.set_guid_index(&c->src.partition_index());
    if (!ebx.parse(std::move(raw), parse_error)) return fail(parse_error);

    const uint32_t wanted = bf6__frontend_name_hash(property_name);
    int matches = 0;
    int controller = -1;
    int transform_instance = -1;
    float transform[12] = {};
    for (size_t instance_index = 0;
         instance_index < ebx.instance_count(); ++instance_index)
    {
        const bf6::EbxValue root = ebx.read_instance(instance_index);
        const bf6::EbxValue* links = root.field(kFrontendLinkConnections);
        if (!links || links->kind != bf6::EbxValue::Kind::Array) continue;
        for (const bf6::EbxValue& link : links->items)
        {
            uint32_t source_field = 0, target_field = 0;
            const int source = bf6__frontend_ref(link.field(kFrontendLinkSource));
            const int target = bf6__frontend_ref(link.field(kFrontendLinkTarget));
            const bool source_named =
                bf6__frontend_u32(link.field(kFrontendLinkSourceField),
                                  source_field) && source_field == wanted;
            const bool target_named =
                bf6__frontend_u32(link.field(kFrontendLinkTargetField),
                                  target_field) && target_field == wanted;
            if (source < 0 || target < 0 || source_named == target_named)
                continue;

            const int candidate = source_named ? target : source;
            if (candidate < 0 ||
                candidate >= static_cast<int>(ebx.instance_count()))
                continue;
            const bf6::EbxValue endpoint =
                ebx.read_instance(static_cast<size_t>(candidate));
            float candidate_transform[12] = {};
            if (!bf6__frontend_transform_value(endpoint,
                                                candidate_transform))
                continue;
            ++matches;
            controller = source_named ? source : target;
            transform_instance = candidate;
            std::memcpy(transform, candidate_transform, sizeof(transform));
        }
    }

    if (matches != 1)
        return fail(matches == 0 ? "named transform link not found" :
                                   "named transform link is ambiguous");
    *out = bf6_frontend_named_transform{};
    std::memcpy(out->transform, transform, sizeof(transform));
    out->controller_instance = controller;
    out->transform_instance = transform_instance;
    out->matching_links = matches;
    if (err && err_len > 0) err[0] = '\0';
    return 1;
}
