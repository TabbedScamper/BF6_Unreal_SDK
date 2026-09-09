/* Raw-installed DiceExpression graph inspection over the stable C ABI. */

int bf6_expression_inspect(bf6_ctx* c, const char* res_name,
                           bf6_expression_stats* out,
                           char* err, int err_len)
{
    auto fail = [&](const std::string& message) {
        if (err && err_len > 0)
            std::snprintf(err, (size_t)err_len, "%s", message.c_str());
        return 0;
    };
    if (out) *out = bf6_expression_stats{};
    if (!c || !res_name || !*res_name || !out) return fail("bad arguments");

    const uint8_t* data = nullptr;
    const int64_t bytes = bf6_read_raw(c, BF6_RAW_RES, res_name, &data);
    if (bytes <= 0 || !data) return fail("expression resource is not in the active mount");

    bf6::expression::Graph graph;
    std::string parse_error;
    if (!bf6::expression::parse(data, (size_t)bytes, graph, parse_error))
        return fail(parse_error);

    const bf6::expression::Header& h = graph.header;
    out->payload_bytes = (uint32_t)std::min<int64_t>(bytes, UINT32_MAX);
    out->content_hash = h.content_hash;
    out->instance_header_size = h.instance_header_size;
    out->constant_pool_size = h.constant_pool_size;
    out->slot_file_size = h.slot_file_size;
    out->record_bytes = h.record_dwords * 4u;
    out->pointer_table_entries = h.pointer_table_entries;
    out->external_bindings = h.external_bindings;
    out->type_count = (uint32_t)graph.types.size();
    out->instance_value_group_count = (uint32_t)graph.instance_values.size();
    out->slot_value_group_count = (uint32_t)graph.slot_values.size();
    out->relocation_count = (uint32_t)graph.relocations.size();
    out->fixup_count = (uint32_t)graph.fixups.size();
    out->record_count = (uint32_t)graph.records.size();
    out->discovered_record_bytes =
        (uint32_t)std::min<size_t>(graph.discovered_record_bytes, UINT32_MAX);
    out->exact_record_tiling = graph.exact_record_tiling ? 1 : 0;
    out->instance_buffer_count = h.instance_buffer_count;
    out->register_group_0 = h.register_groups[0];
    out->register_group_1 = h.register_groups[1];
    out->register_group_2 = h.register_groups[2];
    if (err && err_len > 0) err[0] = '\0';
    return 1;
}
