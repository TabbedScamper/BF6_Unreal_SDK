/* Exact EBX field-hash -> ImportRef paths for runtime consumers.
 *
 * This is intentionally a typed value boundary rather than parsing
 * bf6_ebx_dump's diagnostic text. It uses the executable reflection and the
 * mount's partition GUID index on every call; no exported name table enters
 * the route. Nested structures are walked because sound config Wave entries
 * are authored inside an array on the root instance. */

namespace {

void bf6__imports_with_hash(const bf6::EbxValue& value, uint32_t wanted,
                            std::vector<std::string>& out)
{
    using K = bf6::EbxValue::Kind;
    if (value.kind == K::Array)
    {
        for (const auto& item : value.items)
            bf6__imports_with_hash(item, wanted, out);
        return;
    }
    if (value.kind != K::Struct) return;
    for (const auto& entry : value.fields)
    {
        const bf6::EbxValue& child = entry.second;
        if (entry.first == wanted && child.kind == K::ImportRef)
        {
            const std::string& path = child.import_path.empty()
                ? child.s : child.import_path;
            if (!path.empty()) out.push_back(path);
        }
        bf6__imports_with_hash(child, wanted, out);
    }
}

} // namespace

int bf6_ebx_imports_by_field(bf6_ctx* c, const char* name,
                             uint32_t field_hash,
                             char* paths, int path_stride, int out_max)
{
    if (!c || !name || !*name || path_stride < 2 || out_max < 0) return -1;
    std::string type_error;
    if (!ensure_types(c, type_error)) return -1;
    std::string error;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", error);
    if (raw.empty()) raw = c->src.get_ebx(name, error);
    if (raw.empty()) return -1;

    bf6::Ebx ebx(*c->types);
    ebx.set_guid_index(&c->src.partition_index());
    if (!ebx.parse(std::move(raw), error)) return -1;
    std::vector<std::string> matches;
    for (size_t i = 0; i < ebx.instance_count(); ++i)
        bf6__imports_with_hash(ebx.read_instance(i), field_hash, matches);

    if (paths && out_max > 0)
    {
        const int fill = std::min<int>((int)matches.size(), out_max);
        for (int i = 0; i < fill; ++i)
            std::snprintf(paths + (size_t)i * (size_t)path_stride,
                          (size_t)path_stride, "%s", matches[(size_t)i].c_str());
    }
    return (int)matches.size();
}

static int bf6__ebx_instance_imports(bf6_ctx* c, const char* name,
                                     int instance_index,
                                     bf6_ebx_instance_import* out, int out_max,
                                     bool bounded_armory)
{
    if (!c || !name || !*name || instance_index < 0 || out_max < 0)
        return -1;
    std::string typeError;
    if (!ensure_types(c, typeError)) return -1;
    std::string error;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", error);
    if (raw.empty()) raw = c->src.get_ebx(name, error);
    if (raw.empty()) return -1;

    bf6::Ebx ebx(*c->types);
    ebx.set_guid_index(bounded_armory
        ? &c->src.armory_partition_index()
        : &c->src.partition_index());
    if (!ebx.parse(std::move(raw), error) ||
        instance_index >= static_cast<int>(ebx.instance_count()))
        return -1;
    const bf6::EbxValue instance =
        ebx.read_instance(static_cast<size_t>(instance_index));
    std::vector<bf6_ebx_instance_import> rows;
    for (const auto& field : instance.fields)
    {
        std::string partition, instanceGuid, path;
        bool valid = false;
        if (field.second.kind == bf6::EbxValue::Kind::ImportRef)
        {
            path = field.second.import_path.empty()
                ? field.second.s : field.second.import_path;
            valid = !path.empty();
        }
        if (!valid)
            valid = ebx.import_ref_field(instance, field.first,
                                         partition, instanceGuid, path);
        if (!valid || path.empty()) continue;
        bf6_ebx_instance_import row{};
        row.field_hash = field.first;
        std::snprintf(row.partition_guid, sizeof(row.partition_guid),
                      "%s", partition.c_str());
        std::snprintf(row.instance_guid, sizeof(row.instance_guid),
                      "%s", instanceGuid.c_str());
        std::snprintf(row.path, sizeof(row.path), "%s", path.c_str());
        rows.push_back(row);
    }
    if (out && out_max > 0)
        for (int i = 0; i < static_cast<int>(rows.size()) && i < out_max; ++i)
            out[i] = rows[(size_t)i];
    return static_cast<int>(rows.size());
}

int bf6_ebx_instance_imports(bf6_ctx* c, const char* name,
                             int instance_index,
                             bf6_ebx_instance_import* out, int out_max)
{
    return bf6__ebx_instance_imports(
        c, name, instance_index, out, out_max, false);
}

int bf6_armory_ebx_instance_imports(bf6_ctx* c, const char* name,
                                    int instance_index,
                                    bf6_ebx_instance_import* out, int out_max)
{
    return bf6__ebx_instance_imports(
        c, name, instance_index, out, out_max, true);
}
