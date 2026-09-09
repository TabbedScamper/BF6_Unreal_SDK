int bf6_expression_descriptor_operators(
    const char* exe_path, bf6_expression_operator* out, int out_max,
    char* err, int err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!exe_path || !*exe_path) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "bad executable path");
        return 0;
    }
    std::vector<bf6::expression::DescriptorOperator> rows;
    std::string read_error;
    if (!bf6::expression::read_descriptor_operators(exe_path, rows, read_error)) {
        if (err && err_len > 0)
            std::snprintf(err, (size_t)err_len, "%s", read_error.c_str());
        return 0;
    }
    if (out && out_max > 0) {
        const int n = std::min<int>((int)rows.size(), out_max);
        for (int i = 0; i < n; ++i) {
            out[i].key = rows[(size_t)i].key;
            out[i].flags = rows[(size_t)i].flags;
            out[i].implementation_va = rows[(size_t)i].implementation_va;
            out[i].record_va = rows[(size_t)i].record_va;
        }
    }
    return (int)rows.size();
}

int bf6_expression_resolve_operator_names(
    const char* exe_path, const uint32_t* keys, int key_count,
    bf6_expression_operator_name* out, int out_max,
    char* err, int err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!exe_path || !*exe_path || key_count < 0 || (key_count && !keys)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "bad arguments");
        return 0;
    }
    std::vector<uint32_t> query;
    if (key_count > 0) query.assign(keys, keys + key_count);
    std::vector<bf6::expression::NamedOperator> rows;
    std::string read_error;
    if (!bf6::expression::resolve_named_operators(exe_path, query, rows, read_error)) {
        if (err && err_len > 0)
            std::snprintf(err, (size_t)err_len, "%s", read_error.c_str());
        return 0;
    }
    if (out && out_max > 0) {
        const int n = std::min<int>((int)rows.size(), out_max);
        for (int i = 0; i < n; ++i) {
            out[i] = bf6_expression_operator_name{};
            out[i].key = rows[(size_t)i].key;
            out[i].match_count = rows[(size_t)i].match_count;
            if (rows[(size_t)i].match_count == 1)
                std::snprintf(out[i].name, sizeof(out[i].name), "%s",
                              rows[(size_t)i].name.c_str());
        }
    }
    return (int)rows.size();
}

int bf6_expression_method_operators(
    const char* exe_path, const uint32_t* keys, int key_count,
    bf6_expression_operator* out, int out_max,
    char* err, int err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!exe_path || !*exe_path || key_count < 0 || (key_count && !keys)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "bad arguments");
        return 0;
    }
    std::vector<uint32_t> query;
    if (key_count > 0) query.assign(keys, keys + key_count);
    std::vector<bf6::expression::MethodOperator> rows;
    std::string read_error;
    if (!bf6::expression::read_method_operators(exe_path, query, rows, read_error)) {
        if (err && err_len > 0)
            std::snprintf(err, (size_t)err_len, "%s", read_error.c_str());
        return 0;
    }
    if (out && out_max > 0) {
        const int n = std::min<int>((int)rows.size(), out_max);
        for (int i = 0; i < n; ++i) {
            out[i].key = rows[(size_t)i].key;
            out[i].flags = rows[(size_t)i].flags;
            out[i].implementation_va = rows[(size_t)i].implementation_va;
            out[i].record_va = rows[(size_t)i].record_va;
        }
    }
    return (int)rows.size();
}

int bf6_expression_reflected_operators(
    const char* exe_path, bf6_expression_reflected_operator* out, int out_max,
    char* err, int err_len)
{
    if (err && err_len > 0) err[0] = '\0';
    if (!exe_path || !*exe_path) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "bad executable path");
        return 0;
    }
    std::vector<bf6::expression::ReflectedOperator> rows;
    std::string read_error;
    if (!bf6::expression::read_reflected_operators(exe_path, rows, read_error)) {
        if (err && err_len > 0)
            std::snprintf(err, (size_t)err_len, "%s", read_error.c_str());
        return 0;
    }
    if (out && out_max > 0) {
        const int n = std::min<int>((int)rows.size(), out_max);
        for (int i = 0; i < n; ++i) {
            out[i] = bf6_expression_reflected_operator{};
            out[i].key = rows[(size_t)i].key;
            out[i].parameter_count = rows[(size_t)i].parameter_count;
            out[i].signature = rows[(size_t)i].signature;
            out[i].descriptor_va = rows[(size_t)i].descriptor_va;
            out[i].parameters_va = rows[(size_t)i].parameters_va;
        }
    }
    return (int)rows.size();
}
