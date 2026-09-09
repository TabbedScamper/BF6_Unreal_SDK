/* fx_ext.inc - particle effects over the C ABI.
 *
 * NEW FILE. It is a translation-unit fragment, not a standalone .cpp: it uses
 * bf6_ctx, which only exists inside bf6_core.cpp. Wire it with ONE line in
 * src/bf6_core.cpp, immediately BEFORE that file's last line - the include has
 * to be INSIDE the extern "C" block so the new entry points get C linkage:
 *
 *     #include "fx_ext.inc"
 *     }  // extern "C"          <- the existing last line, unchanged
 *
 * and add `#include "fx.h"` to the include block at the top of bf6_core.cpp,
 * beside "levellights.h". The declarations in fx_abi_decl.h.txt, beside this
 * file, go into include/bf6_core.h after the bf6_level_lights prototype.
 * Nothing else in either file changes.
 *
 * WHAT A ROW IS. One row per EMITTER LAYER, not per effect. A layer is the unit
 * that renders: its own sheet, its own spawn rate, its own parameters, its own
 * offset inside the effect. 1,368 distinct effects on 14 retail levels expand
 * to 3,983 layers, and merging them into one row per effect throws away exactly
 * the variation that makes an explosion read as an explosion (BaseSize
 * 100/200/35 and Drag 0.5/0.0/0.7/0.01 on ONE effect) and leaves a generic puff.
 *
 * THREE THINGS A CONSUMER OF THIS GETS WRONG WITHOUT BEING TOLD.
 *
 *  1. FRAME RECTS ARE UV, NOT PIXELS. cols is commonly 6 or 7 while the sheet
 *     is a power of two, so a cell is 341.333 or 146.286 px. The frames tile
 *     exactly at u = col/cols; flooring to integer pixels shears the last
 *     column. Four of one level's fourteen sheets are affected. Use
 *     bf6_fx_frame_uv rather than computing a rect.
 *
 *  2. frames IS AUTHORED AND THE FILENAME IS NOT. t_smoke_wispy_light_7x144_d
 *     authors 49 frames, not 144.
 *
 *  3. A LAYER WITH NO SHEET IS NOT A FAILURE. 1,406 of 3,983 shipped layers are
 *     the volume-decal, sparks, mesh-shard, distortion and creature families and
 *     bind no flipbook at all. `family` says which, from the graph path alone.
 */

/* bf6_fx_param IS bf6::FxParam, and the parameter array is handed out by
 * reinterpret_cast rather than copied - a fleet decode carries 1.7 million
 * parameter slots and copying them per row would dominate the call. That is
 * only sound while the two layouts agree, so it is checked here rather than
 * assumed; if either struct gains a field this fails at compile time instead of
 * handing a renderer shifted values. */
static_assert(sizeof(bf6_fx_param) == sizeof(bf6::FxParam), "fx param size");
static_assert(offsetof(bf6_fx_param, pid)       == offsetof(bf6::FxParam, pid),       "pid");
static_assert(offsetof(bf6_fx_param, type)      == offsetof(bf6::FxParam, type),      "type");
static_assert(offsetof(bf6_fx_param, cb_offset) == offsetof(bf6::FxParam, cb_offset), "cb");
static_assert(offsetof(bf6_fx_param, v)         == offsetof(bf6::FxParam, v),         "v");
static_assert(offsetof(bf6_fx_param, ivalue)    == offsetof(bf6::FxParam, ivalue),    "ivalue");

/* Storage for one level's decode, so the const char* a row hands out outlive
 * the call. Same lifetime rule bf6_light states: valid until the next
 * bf6_level_fx for a DIFFERENT level. Kept here rather than on bf6_ctx because
 * this file is included after that struct is closed. */
struct BF6_FxHold {
    std::string                level;
    std::vector<bf6::FxEffect> effects;
    std::vector<bf6_fx_layer>  rows;
    bf6::FxStats               stats;
};
static std::map<bf6_ctx*, BF6_FxHold> g_bf6_fx;

int bf6_level_fx(bf6_ctx* c, const char* level, bf6_fx_layer* out, int out_max,
                 bf6_fx_stats* stats, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level) return fail("bad arguments");
    if (!c->types || !c->walk || c->walked_level != level)
        return fail("call bf6_open_level for this level first");

    BF6_FxHold& h = g_bf6_fx[c];
    if (h.level != level) {
        h.level.clear();
        h.effects.clear();
        h.rows.clear();
        h.stats = bf6::FxStats();

        std::vector<bf6::FxPlacement> plc;
        std::string e;
        // THE WALK IS REUSED, NOT RE-RUN. It is already the exact traversal an
        // effect placement has to come from - it is what stops at the ten
        // destruction-branch types, whose subtrees are full of fx_ references
        // that the level does not place. A hand-rolled descent that skips that
        // returned 14,576 placements on mp_dumbo against the correct 1,010.
        if (!bf6::fx_from_walk(c->src, *c->types, c->walk->rows(), plc, h.effects,
                               h.stats, e))
            return fail(e.empty() ? "the fx decode found nothing" : e);
        h.level = level;

        for (const bf6::FxEffect& fe : h.effects)
            for (const bf6::FxLayer& L : fe.layers) {
                bf6_fx_layer r{};
                r.effect      = fe.name.c_str();
                r.effect_path = fe.path.c_str();
                r.placements  = fe.placements;
                r.effect_cull_distance = fe.cull_distance;
                r.effect_max_instances = fe.max_instances;

                r.layer  = L.instance;
                r.graph  = L.graph.c_str();
                r.family = bf6::fx_family(L.graph);
                for (int k = 0; k < 12; k++) r.local[k] = L.local[k];

                r.atlas       = L.atlas.name.empty() ? nullptr : L.atlas.name.c_str();
                r.atlas_res   = L.atlas.res_name.empty() ? nullptr : L.atlas.res_name.c_str();
                r.atlas_chunk = L.atlas.chunk_guid.empty() ? nullptr : L.atlas.chunk_guid.c_str();
                r.atlas_from_override = L.atlas_from_override ? 1 : 0;
                r.atlas_cols    = L.atlas.cols;
                r.atlas_frames  = L.atlas.frames;
                r.atlas_left_right = L.atlas.left_right ? 1 : 0;
                r.atlas_rid     = L.atlas.rid;
                r.atlas_width   = L.atlas.width;
                r.atlas_height  = L.atlas.height;
                r.atlas_mips    = L.atlas.mips;

                r.lighting_model = L.lighting_model;
                r.alignment      = L.alignment;

                r.spawn_mode   = L.spawn_mode.empty() ? nullptr : L.spawn_mode.c_str();
                r.has_spawn_rate = L.has_spawn_rate ? 1 : 0;
                r.spawn_rate   = L.spawn_rate;
                r.particle_max = L.particle_max;
                r.particle_life = L.particle_life;
                r.emitter_life  = L.emitter_life;
                r.gpu_cull_distance = L.gpu_cull_distance;
                r.min_spawn_distance = L.min_spawn_distance;
                r.max_spawn_distance = L.max_spawn_distance;
                r.preroll_time = L.preroll_time;
                r.draw_layer   = L.draw_layer;
                r.draw_pass    = L.draw_pass;
                r.sort_mode    = L.sort_mode;
                r.particle_type = L.global_sorting_particle_type;

                r.param_count = (int32_t)L.params.size();
                r.params      = L.params.empty() ? nullptr
                                : reinterpret_cast<const bf6_fx_param*>(L.params.data());
                h.rows.push_back(r);
            }
    }

    if (stats) {
        bf6_fx_stats& s = *stats;
        s = bf6_fx_stats{};
        s.placements       = (int32_t)h.stats.placements;
        s.distinct_effects = (int32_t)h.stats.distinct_effects;
        s.effect_failed    = (int32_t)h.stats.effect_failed;
        s.layers           = (int32_t)h.stats.layers;
        s.graph_resolved   = (int32_t)h.stats.graph_resolved;
        s.atlas_resolved   = (int32_t)h.stats.atlas_resolved;
        s.atlas_override   = (int32_t)h.stats.atlas_override;
        s.atlas_template   = (int32_t)h.stats.atlas_template;
        s.grid_resolved    = (int32_t)h.stats.grid_resolved;
        s.rid_present      = (int32_t)h.stats.rid_present;
        s.res_resolved     = (int32_t)h.stats.res_resolved;
        s.chunk_named      = (int32_t)h.stats.chunk_named;
        s.lighting_model   = (int32_t)h.stats.lighting_model;
        s.alignment        = (int32_t)h.stats.alignment;
    }

    const int n = (int)h.rows.size();
    for (int i = 0; i < n && i < out_max; i++) out[i] = h.rows[(size_t)i];
    return n;
}

int bf6_fx_placements(bf6_ctx* c, const char* level, const char* effect,
                      float* out_xf, int out_max)
{
    if (!c || !level || !*level || !effect || !c->walk || c->walked_level != level) return 0;
    std::string want = effect;
    for (char& ch : want) { if (ch == '\\') ch = '/'; else ch = (char)tolower((unsigned char)ch); }
    if (want.size() > 4 && want.compare(want.size() - 4, 4, ".ebx") == 0) want.resize(want.size() - 4);

    int n = 0;
    for (const bf6::WalkRow& r : c->walk->rows()) {
        std::string m = r.mesh;
        for (char& ch : m) { if (ch == '\\') ch = '/'; else ch = (char)tolower((unsigned char)ch); }
        if (m.size() > 4 && m.compare(m.size() - 4, 4, ".ebx") == 0) m.resize(m.size() - 4);
        if (m != want) continue;
        if (out_xf && n < out_max)
            for (int k = 0; k < 4; k++) {
                out_xf[n * 12 + k * 3 + 0] = r.xf.m[k].x;
                out_xf[n * 12 + k * 3 + 1] = r.xf.m[k].y;
                out_xf[n * 12 + k * 3 + 2] = r.xf.m[k].z;
            }
        n++;
    }
    return n;
}

int bf6_fx_atlas_mip0(bf6_ctx* c, const char* level, int layer_index,
                      const uint8_t** out_data, int32_t* out_size,
                      char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !out_data || !out_size) return fail("bad arguments");
    auto it = g_bf6_fx.find(c);
    if (it == g_bf6_fx.end() || it->second.level != level)
        return fail("call bf6_level_fx for this level first");

    // Find the layer by its row index, then its atlas description.
    const BF6_FxHold& h = it->second;
    int i = 0;
    const bf6::FxAtlas* a = nullptr;
    for (const bf6::FxEffect& fe : h.effects) {
        for (const bf6::FxLayer& L : fe.layers) {
            if (i++ != layer_index) continue;
            a = &L.atlas;
            break;
        }
        if (a) break;
    }
    if (!a) return fail("no such layer");
    if (a->chunk_guid.empty()) return fail("that layer binds no sheet");

    // Held so the bytes outlive the call. One sheet at a time is enough: a
    // consumer uploads each and moves on, and 52 sheets cover the whole fleet.
    static std::vector<uint8_t> pixels;
    std::string e;
    if (!bf6::fx_atlas_mip0(c->src, *c->types, *a, pixels, e)) return fail(e);
    *out_data = pixels.data();
    *out_size = (int32_t)pixels.size();
    return 1;
}

void bf6_fx_frame_uv(const bf6_fx_layer* L, int frame, float* out_uv)
{
    if (!out_uv) return;
    out_uv[0] = 0.f; out_uv[1] = 0.f; out_uv[2] = 1.f; out_uv[3] = 1.f;
    if (!L || L->atlas_cols <= 0 || L->atlas_frames <= 0) return;
    const int cols = L->atlas_cols;
    const int rows = (L->atlas_frames + cols - 1) / cols;
    if (rows <= 0) return;
    const int f = frame < 0 ? 0 : (frame >= L->atlas_frames ? L->atlas_frames - 1 : frame);
    const int cx = f % cols, cy = f / cols;
    // IN UV, ALWAYS. A six- or seven-column sheet does not divide a power of
    // two, so the pixel rect is fractional and flooring it shears the last
    // column. The left half is the sprite on a LeftRightTiles sheet; the right
    // half is the alternate tile that UseRightTile selects, and the six-way
    // basis GnomonLit reads.
    const float xdiv = L->atlas_left_right ? (float)(cols * 2) : (float)cols;
    out_uv[0] = (float)cx / xdiv;
    out_uv[1] = (float)cy / (float)rows;
    out_uv[2] = out_uv[0] + 1.0f / xdiv;
    out_uv[3] = out_uv[1] + 1.0f / (float)rows;
}

/* The declarations that go into include/bf6_core.h, after the bf6_level_lights
 * prototype, are in fx_abi_decl.h.txt beside this file. They are kept there
 * rather than quoted in a comment here because a C declaration nested inside a
 * block comment cannot carry its own comments, and every one of these fields
 * needs one. */
