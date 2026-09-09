/* The level's MeshScatteringDatabase over the C ABI.
 *
 * This file is included by bf6_core.cpp because it uses the opaque bf6_ctx.
 * Public linkage comes from bf6_core.h. The variable-length exact-end invariant
 * are recovered in findings/meshscatteringdatabase-layout.md.
 */

int bf6_level_scatter(bf6_ctx* c, const char* level,
                      bf6_scatter_entry* out, int out_max,
                      char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) {
            std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        }
        return -1;
    };
    if (!c || !level || !*level) return fail("no level");
    std::string me;
    if (!ensure_mounted(c, level, me)) return fail(me);

    auto lower = [](std::string s) {
        for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
        return s;
    };
    const std::string want = lower(level);
    std::string db;
    for (const auto& kv : c->src.res()) {
        if (kv.second.type != 0x2AB067B5u) continue;
        if (lower(kv.first).find(want) != std::string::npos) {
            db = kv.first;
            break;
        }
    }
    // No widening to another level's sole database. A fake id is the control
    // for precisely that confidently-wrong fallback.
    if (db.empty()) {
        c->scatter_rows.clear();
        c->scatter_names.clear();
        c->scatter_meshes.clear();
        return 0;
    }

    std::vector<uint8_t> d = c->src.get_res(db, me);
    if (d.size() < 0x28) return fail("scatter database shorter than header");
    auto u32 = [&d](size_t p) {
        uint32_t v = 0;
        if (p + 4 <= d.size()) std::memcpy(&v, d.data() + p, 4);
        return v;
    };
    auto f32 = [&d](size_t p) {
        float v = 0.f;
        if (p + 4 <= d.size()) std::memcpy(&v, d.data() + p, 4);
        return v;
    };
    const uint32_t count = u32(0);
    if (count > 100000) return fail("scatter record count outside guard range");

    struct Row { std::string name, mesh; float dist = 0.f, ratio = 0.f; int points = 0; };
    std::vector<Row> rows;
    rows.reserve(count);
    size_t p = 0x28;
    for (uint32_t i = 0; i < count; i++) {
        const size_t start = p;
        while (p < d.size() && d[p] != 0) p++;
        if (p >= d.size()) return fail("scatter record has unterminated name");
        std::string name((const char*)d.data() + start, p - start);
        p++;
        constexpr size_t kHead = 45, kPoint = 16, kTail = 24;
        if (p + kHead > d.size()) return fail("scatter fixed record runs past payload");
        const float dist = f32(p + 33);
        const float ratio = f32(p + 37);
        const uint32_t points = u32(p + 41);
        const size_t tail = p + kHead + (size_t)points * kPoint;
        if (tail > d.size() || tail + kTail > d.size())
            return fail("scatter point array runs past payload");
        p = tail + kTail;

        std::string stem = lower(name);
        if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".ebx") == 0)
            stem.resize(stem.size() - 4);
        std::string mesh;
        const std::string candidates[2] = {stem + "_mesh", stem};
        for (const std::string& cand : candidates) {
            if (c->src.res().find(cand) != c->src.res().end()) { mesh = cand; break; }
        }
        rows.push_back({name, mesh, dist, ratio, (int)points});
    }
    if (p != d.size()) {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "scatter parse ended at %zu of %zu bytes", p, d.size());
        return fail(msg);
    }

    c->scatter_names.clear(); c->scatter_meshes.clear(); c->scatter_rows.clear();
    c->scatter_names.reserve(rows.size()); c->scatter_meshes.reserve(rows.size());
    c->scatter_rows.resize(rows.size());
    for (const Row& r : rows) {
        c->scatter_names.push_back(r.name);
        c->scatter_meshes.push_back(r.mesh);
    }
    for (size_t i = 0; i < rows.size(); i++) {
        bf6_scatter_entry& dst = c->scatter_rows[i];
        dst = bf6_scatter_entry{};
        dst.name = c->scatter_names[i].c_str();
        dst.mesh_res = c->scatter_meshes[i].c_str();
        dst.view_distance = rows[i].dist;
        dst.dissolve_ratio = rows[i].ratio;
        dst.point_count = rows[i].points;
    }
    if (out && out_max > 0)
        for (int i = 0; i < (int)c->scatter_rows.size() && i < out_max; i++)
            out[i] = c->scatter_rows[(size_t)i];
    return (int)c->scatter_rows.size();
}
