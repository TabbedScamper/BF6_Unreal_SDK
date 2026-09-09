/* Installed UI-options provider data.
 *
 * options_sectionscreen deliberately serializes empty DataLists.  The game
 * populates them from UIOptionsCategory assets and the referenced gameplay
 * option assets at runtime.  Keep this decoder on the typed EBX boundary: a
 * diagnostic text dump is evidence for humans, never a runtime input. */

namespace {

constexpr uint32_t kOptionsId = 0x17D2E994u;
constexpr uint32_t kOptionsLabelSid = 0x54007DDCu;
constexpr uint32_t kOptionsSubcategories = 0xA5CB2ED7u;
constexpr uint32_t kOptionsItems = 0x482DE2AAu;
constexpr uint32_t kOptionsDbd = 0x1191B52Cu;
constexpr uint32_t kOptionsAsset = 0xCA15F0C3u;
constexpr uint32_t kOptionsDebugName = 0x4E117A39u;
constexpr uint32_t kOptionsRowLabelSid = 0x6ADD2124u;
constexpr uint32_t kOptionsSubcategoryId = 0x221CF9EEu;
constexpr uint32_t kOptionsPath = 0x0C59FA06u;

constexpr uint32_t kOptionEnumValues = 0x2C95D7B7u;
constexpr uint32_t kOptionEnumLabelId = 0xA1C61366u;
constexpr uint32_t kOptionEnumDefault = 0x97EA1523u;
constexpr uint32_t kOptionMin = 0x8EE32A15u;
constexpr uint32_t kOptionMax = 0xE2253F2Bu;
constexpr uint32_t kOptionDefault = 0x8CF424E7u;
constexpr uint32_t kOptionStep = 0x4276044Bu;
constexpr uint32_t kOptionLabelId = 0x2A760FECu;
constexpr uint32_t kOptionDescriptionId = 0x860816A9u;

std::string bf6__option_path(const bf6::EbxValue* value)
{
    if (!value || value->kind != bf6::EbxValue::Kind::ImportRef) return {};
    std::string path = value->import_path.empty() ? value->s
                                                   : value->import_path;
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ebx") == 0)
        path.resize(path.size() - 4);
    return path;
}

bool bf6__option_number(const bf6::EbxValue* value, double& out)
{
    if (!value) return false;
    if (value->kind == bf6::EbxValue::Kind::Real) { out = value->f; return true; }
    if (value->kind == bf6::EbxValue::Kind::Int) { out = (double)value->i; return true; }
    if (value->kind == bf6::EbxValue::Kind::Uint) { out = (double)value->u; return true; }
    return false;
}

uint32_t bf6__option_u32(const bf6::EbxValue* value)
{
    if (!value) return 0;
    if (value->kind == bf6::EbxValue::Kind::Uint) return (uint32_t)value->u;
    if (value->kind == bf6::EbxValue::Kind::Int && value->i >= 0)
        return (uint32_t)value->i;
    return 0;
}

std::string bf6__option_string(const bf6::EbxValue* value)
{
    return value && value->kind == bf6::EbxValue::Kind::Str ? value->s
                                                             : std::string{};
}

std::string bf6__option_localized_sid(bf6_ctx* c, uint32_t sid)
{
    const char* text = sid ? bf6_localized_string(c, sid) : nullptr;
    return text ? text : "";
}

std::string bf6__option_localized_id(bf6_ctx* c, const std::string& id)
{
    const char* text = id.empty() ? nullptr
                                  : bf6_localized_string_by_id(c, id.c_str());
    return text ? text : "";
}

bool bf6__option_ebx(bf6_ctx* c, const std::string& path, bf6::Ebx& out)
{
    std::string error;
    std::vector<uint8_t> raw = c->src.get_ebx(path + ".ebx", error);
    if (raw.empty()) raw = c->src.get_ebx(path, error);
    if (raw.empty()) return false;
    out.set_guid_index(&c->src.armory_partition_index());
    return out.parse(std::move(raw), error);
}

bf6_ui_option_kind bf6__option_kind(const std::string& dbd)
{
    if (dbd.find("uioptionsgrouptitledatabinding") != std::string::npos)
        return BF6_UI_OPTION_GROUP_TITLE;
    if (dbd.find("uioptionssubcategorynavigationdatabinding") != std::string::npos)
        return BF6_UI_OPTION_SUBCATEGORY;
    if (dbd.find("uioptionsnewwindowdatabinding") != std::string::npos)
        return BF6_UI_OPTION_NEW_WINDOW;
    if (dbd.find("uioptionsthresholdfloat") != std::string::npos)
        return BF6_UI_OPTION_THRESHOLD_FLOAT;
    if (dbd.find("uioptionsfloatdatabinding") != std::string::npos ||
        dbd.find("uioptionsalternativefloatdatabinding") != std::string::npos)
        return BF6_UI_OPTION_FLOAT;
    if (dbd.find("uioptionsbooldatabinding") != std::string::npos)
        return BF6_UI_OPTION_BOOL;
    if (dbd.find("uioptionsenumdatabinding") != std::string::npos)
        return BF6_UI_OPTION_ENUM;
    if (dbd.find("uioptionscolordatabinding") != std::string::npos)
        return BF6_UI_OPTION_COLOR;
    return BF6_UI_OPTION_UNKNOWN;
}

} // namespace

int bf6_ui_option_subcategories(bf6_ctx* c, const char* category_partition,
                                bf6_ui_option_category* out, int out_max)
{
    if (!c || !category_partition || !*category_partition || out_max < 0)
        return -1;
    std::string typeError;
    if (!ensure_types(c, typeError)) return -1;

    bf6::Ebx category(*c->types);
    if (!bf6__option_ebx(c, category_partition, category) ||
        category.instance_count() == 0)
        return -1;
    const bf6::EbxValue root = category.read_instance(0);
    const bf6::EbxValue* refs = root.field(kOptionsSubcategories);
    if (!refs || refs->kind != bf6::EbxValue::Kind::Array) return -1;

    int total = 0;
    for (const bf6::EbxValue& ref : refs->items)
    {
        const std::string path = bf6__option_path(&ref);
        if (path.empty()) return -1;
        bf6::Ebx subcategory(*c->types);
        if (!bf6__option_ebx(c, path, subcategory) ||
            subcategory.instance_count() == 0)
            return -1;
        const bf6::EbxValue subRoot = subcategory.read_instance(0);
        if (out && total < out_max)
        {
            bf6_ui_option_category& row = out[total];
            row = bf6_ui_option_category{};
            row.ordinal = total;
            row.label_sid = bf6__option_u32(subRoot.field(kOptionsLabelSid));
            put_str(row.id, sizeof(row.id),
                    bf6__option_string(subRoot.field(kOptionsId)));
            put_str(row.partition, sizeof(row.partition), path);
            put_str(row.label, sizeof(row.label),
                    bf6__option_localized_sid(c, row.label_sid));
        }
        ++total;
    }
    return total;
}

int bf6_ui_option_rows(bf6_ctx* c, const char* subcategory_partition,
                       bf6_ui_option_row* out, int out_max)
{
    if (!c || !subcategory_partition || !*subcategory_partition || out_max < 0)
        return -1;
    std::string typeError;
    if (!ensure_types(c, typeError)) return -1;

    bf6::Ebx category(*c->types);
    if (!bf6__option_ebx(c, subcategory_partition, category) ||
        category.instance_count() == 0)
        return -1;
    const bf6::EbxValue root = category.read_instance(0);
    const bf6::EbxValue* refs = root.field(kOptionsItems);
    if (!refs || refs->kind != bf6::EbxValue::Kind::Array) return -1;

    int total = 0;
    for (const bf6::EbxValue& ref : refs->items)
    {
        if (ref.kind != bf6::EbxValue::Kind::InstanceRef || ref.instance < 0 ||
            (size_t)ref.instance >= category.instance_count())
            return -1;
        const bf6::EbxValue item = category.read_instance((size_t)ref.instance);
        bf6_ui_option_row row{};
        row.ordinal = total;
        row.authored_default_index = -1;
        row.authored_default_bool = -1;
        row.databinding[0] = row.option_asset[0] = 0;
        const std::string dbd = bf6__option_path(item.field(kOptionsDbd));
        const std::string optionPath = bf6__option_path(item.field(kOptionsAsset));
        row.kind = bf6__option_kind(dbd);
        put_str(row.databinding, sizeof(row.databinding), dbd);
        put_str(row.option_asset, sizeof(row.option_asset), optionPath);

        row.label_sid = bf6__option_u32(item.field(kOptionsLabelSid));
        if (!row.label_sid)
            row.label_sid = bf6__option_u32(item.field(kOptionsRowLabelSid));
        std::string id = bf6__option_string(item.field(kOptionsSubcategoryId));
        if (id.empty()) id = bf6__option_string(item.field(kOptionsDebugName));
        std::string label = bf6__option_localized_sid(c, row.label_sid);

        if (!optionPath.empty())
        {
            bf6::Ebx option(*c->types);
            if (!bf6__option_ebx(c, optionPath, option) ||
                option.instance_count() == 0)
            {
                std::fprintf(stderr, "ui options: unreadable option asset %s\n",
                             optionPath.c_str());
                return -1;
            }
            const bf6::EbxValue optionRoot = option.read_instance(0);
            const std::string debug =
                bf6__option_string(optionRoot.field(kOptionsDebugName));
            if (!debug.empty()) id = debug;
            const std::string labelId =
                bf6__option_string(optionRoot.field(kOptionLabelId));
            const std::string descriptionId =
                bf6__option_string(optionRoot.field(kOptionDescriptionId));
            const std::string optionLabel =
                bf6__option_localized_id(c, labelId);
            if (!optionLabel.empty()) label = optionLabel;
            put_str(row.description, sizeof(row.description),
                    bf6__option_localized_id(c, descriptionId));

            const bf6::EbxValue* defaultValue = optionRoot.field(kOptionDefault);
            if (defaultValue && defaultValue->kind == bf6::EbxValue::Kind::Bool)
                row.authored_default_bool = defaultValue->b ? 1 : 0;
            double number = 0.0;
            if (bf6__option_number(defaultValue, number))
                row.authored_default_value = (float)number;
            if (bf6__option_number(optionRoot.field(kOptionMin), number))
                row.min_value = (float)number;
            if (bf6__option_number(optionRoot.field(kOptionMax), number))
                row.max_value = (float)number;
            if (bf6__option_number(optionRoot.field(kOptionStep), number))
                row.step = (float)number;

            const bf6::EbxValue* values = optionRoot.field(kOptionEnumValues);
            if (values && values->kind == bf6::EbxValue::Kind::Array)
            {
                row.enum_count = (int32_t)(std::min)(
                    values->items.size(), (size_t)BF6_UI_OPTION_ENUM_MAX);
                for (int index = 0; index < row.enum_count; ++index)
                {
                    const bf6::EbxValue& enumValue = values->items[(size_t)index];
                    const std::string enumId =
                        bf6__option_string(enumValue.field(kOptionEnumLabelId));
                    const std::string enumLabel =
                        bf6__option_localized_id(c, enumId);
                    put_str(row.enum_labels[index], sizeof(row.enum_labels[index]),
                            enumLabel.empty() ? enumId : enumLabel);
                    const bf6::EbxValue* isDefault =
                        enumValue.field(kOptionEnumDefault);
                    if (isDefault && isDefault->kind == bf6::EbxValue::Kind::Bool &&
                        isDefault->b)
                        row.authored_default_index = index;
                }
                if (row.authored_default_index >= 0)
                    put_str(row.value_label, sizeof(row.value_label),
                            row.enum_labels[row.authored_default_index]);
            }
            else if (row.kind == BF6_UI_OPTION_BOOL &&
                     row.authored_default_bool >= 0)
            {
                const char* boolId = row.authored_default_bool
                    ? "ID_M_OPTIONS_ON" : "ID_M_OPTIONS_OFF";
                std::string boolLabel = bf6__option_localized_id(c, boolId);
                put_str(row.value_label, sizeof(row.value_label),
                        boolLabel.empty() ? (row.authored_default_bool ? "ON" : "OFF")
                                          : boolLabel);
            }
            else if (row.kind == BF6_UI_OPTION_FLOAT ||
                     row.kind == BF6_UI_OPTION_THRESHOLD_FLOAT)
            {
                char value[64]{};
                std::snprintf(value, sizeof(value), "%g", row.authored_default_value);
                put_str(row.value_label, sizeof(row.value_label), value);
            }
        }

        put_str(row.id, sizeof(row.id), id);
        put_str(row.label, sizeof(row.label), label.empty() ? id : label);
        if (out && total < out_max) out[total] = row;
        ++total;
    }
    return total;
}
