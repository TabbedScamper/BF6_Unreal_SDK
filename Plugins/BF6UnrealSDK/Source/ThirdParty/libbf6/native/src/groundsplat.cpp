#include "groundsplat.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "splat.h"
#include "terrainlayers.h"
#include "terrainstatic.h"
#include "terraincomposite.h"   // paint_colour_map

namespace bf6 {

bool ground_coverage(Source& src, const std::string& level,
                     const GroundCoverageOpts& request,
                     GroundCoverage& out, std::string& err)
{
    out = GroundCoverage();
    int size = request.size;
    if (size <= 0) size = 2048;

    // ---- the streaming tree, and block 1 out of it -------------------------
    std::string lvl = level;
    for (char& c : lvl) c = (char)tolower((unsigned char)c);
    std::string tree;
    for (const auto& kv : src.res()) {
        std::string n = kv.first;
        for (char& c : n) c = (char)tolower((unsigned char)c);
        if (n.find("streamingtree") != std::string::npos &&
            n.find(lvl) != std::string::npos) { tree = kv.first; break; }
    }
    if (tree.empty()) { err = "no streaming tree for " + level; return false; }

    std::vector<uint8_t> res = src.get_res(tree, err);
    if (res.empty()) { err = "streaming tree unreadable"; return false; }

    std::vector<uint8_t> b1;
    if (!Splat::find_block(res, 1, b1, err)) return false;

    Splat sp;
    if (!sp.parse(b1, err)) return false;

    SplatChunkDir dir;
    if (!Splat::read_chunk_dir(res, dir, err)) return false;
    if (!sp.detect_layout(dir, err)) return false;

    auto fetch = [&src](const std::string& g) {
        std::string e;
        return src.get_chunk(g, e);
    };

    SplatCoverage cov;
    SplatCompositeOpts opt;
    // MP_Isolated saturates four slots on every land texel and discards 9.06
    // units of authored mask per texel. Eight retains the practical evaluator
    // stack while staying compact enough for a live 4096-square texture set.
    opt.max_slots = std::clamp(request.max_slots, 1, 16);
    // An explicit window is the camera-relative fast path. With no window,
    // Splat::composite uses the root bounds read from the mounted game's block
    // 1. Do not substitute the Portal editor overlay box here: that box comes
    // from an SDK JSON file, is not a game-runtime read path, and is known not
    // to equal the playable/combat volume on every level.
    if (request.rect_size > 0.f) {
        opt.rect_min[0] = request.rect_min[0];
        opt.rect_min[1] = request.rect_min[1];
        opt.rect_size = request.rect_size;
    }
    if (!sp.composite(dir, fetch, size, cov, err, opt)) return false;

    // ---- the materials those layer indices refer to ------------------------
    TerrainLayers tl;
    std::string le;
    const bool have_layers = tl.load(src, level, le);
    if (!have_layers && !le.empty()) {
        // Not fatal: coverage without materials still tells a renderer the
        // shape of the ground, and saying so beats failing the whole call.
        err = "layers: " + le;
    }

    // THE MATERIAL UNDER THE PAINT. Block 1 contains the painted masks, but
    // IgnoreMask/base records do not carry pages and therefore can never
    // arrive through `cov`. Block 7 is the TerrainMaterialTree that names the
    // full-coverage surface those masks are evaluated over. This matters most
    // on MP_Isolated: its base is absent from all four block-1 slots on 93.65%
    // of land texels.
    //
    // Keep this non-fatal. A map with no readable block 7 still has the same
    // useful painted coverage this API returned before this read path existed.
    MaterialRaster base;
    bool have_base = false;
    {
        std::vector<uint8_t> b7;
        std::string be;
        if (Splat::find_block(res, 7, b7, be)) {
            MaterialTree mt;
            if (mt.parse(b7, be)) {
                have_base = mt.rasterize(
                    size,
                    [&](float cx, float cz, float w)
                    { return sp.base_list_at(cx, cz, w); },
                    sp.full_list(),
                    have_layers ? tl.linked_list() : std::vector<int>(),
                    sp.global_base_list(), base, be);
            }
        }
    }

    // The static table hands back an asset LEAF name; the resource wants the
    // full path, so it is looked up in the mount rather than assumed.
    auto res_for_name = [&src](const std::string& leaf) -> std::string {
        if (leaf.empty()) return std::string();
        for (const auto& kv : src.res()) {
            const size_t sl = kv.first.find_last_of('/');
            const std::string tail = sl == std::string::npos ? kv.first : kv.first.substr(sl + 1);
            if (tail == leaf) return kv.first;
        }
        return std::string();
    };

    // guid -> resource name, the same walk every other consumer does
    const std::map<std::string, std::string>& gi = src.partition_index();
    auto res_for_guid = [&gi](const std::string& g) -> std::string {
        if (g.empty()) return std::string();
        auto it = gi.find(g);
        if (it == gi.end()) return std::string();
        std::string n = it->second;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ebx") == 0) n.resize(n.size() - 4);
        return n;
    };

    // THE STATIC HALF OF THE TEXTURES.
    //
    // Only some layers name their sheet through the layer-graph depot. The
    // rest are bound statically out of the compositor's own BindingSet, and
    // on an urban map those are the road surfaces - a coverage list built
    // from the bindless path alone hands a renderer thirty materials of
    // which four have textures. Same join the bake uses.
    TerrainStaticTable stab;
    bool have_static = false;
    {
        std::string se;
        have_static = stab.load(src, level, se) && stab.join_exact();
        if (getenv("BF6_GS_DEBUG"))
            fprintf(stderr, "  [gs] have_static=%d resolved=%d declared=%d err='%s'\n",
                    (int)have_static, stab.resolved(), stab.declared(), se.c_str());
    }

    // WHICH LAYERS ARE MODIFIERS, not surfaces.
    //
    // A modifier layer has no colour of its own: its evaluator body multiplies
    // whatever is already accumulated, of the form
    //     lerp(acc, acc * grunge, coverage)
    // writing only base colour, smoothness and class - never normal, height or
    // alpha. MP_Aftermath's layer 23 is one, a global grunge pass whose splat
    // mask is 1.0 over 100% of the raster BY DESIGN.
    //
    // That is fatal to a top-N-layers-per-texel coverage: the modifier wins
    // slot 0 on 99.8% of texels, has no sheet, and the renderer falls back to
    // the aerial photograph for the whole map. The layer that should be
    // underneath is layer 0, the base field over 95.3% of it.
    //
    // So modifiers are dropped from the SURFACE selection. They are not
    // rendered as a tint yet, which is a real loss of grunge, but drawing the
    // ground and losing its dirt beats drawing a photograph of the ground.
    std::set<int> modifier_layers;
    // The live evaluator names modifiers directly: an attributed case with no
    // sampled base-colour descriptor supplies no colour.
    if (have_layers && have_static && stab.resolved() == stab.declared()) {
        for (size_t i = 0; i < tl.layers().size(); i++) {
            if (tl.layers()[i].empty) continue;
            if (!tl.layers()[i].material.base_color().empty()) continue;
            int tcv = -1, tnh = -1, tthird = -1;
            if (stab.layer_descriptors((int)i, tcv, tnh, tthird) && tcv < 0)
                modifier_layers.insert((int)i);
        }
    }

    // COMPACT THE LAYER SPACE. The coverage carries raw layer indices, which
    // run to 47 on some maps while a handful ever appear. A renderer binds one
    // sheet per material, so handing it 47 slots to fill with 13 textures
    // wastes most of an array; the indices are remapped to the layers that
    // actually reach the raster, in ascending order.
    bool base_layers[256] = {};
    if (have_base)
        for (uint8_t bl : base.layer) if (bl != 255) base_layers[bl] = true;

    std::map<int, int> layer_to_slot;
    for (int L = 0; L < 256; L++) {
        if (cov.layer_texels[L] == 0 && !base_layers[L]) continue;
        if (modifier_layers.count(L)) continue;      // see modifier_layers above
        // AN EMPTY PALETTE SLOT IS NOT A SURFACE EITHER.
        //
        // A layer index can be present in the splat with real coverage while
        // the palette has no layer there at all. mp_isolated's L53 is the
        // clearest case: absent between L52 and L54, no row in the evaluator
        // bytecode, and it wins the paint on 60.6% of the map. Elected as a
        // surface it resolves to nothing and the renderer falls back to the
        // aerial photograph for most of the level.
        //
        // Thirteen levels do this. On some of them the empty slot is WORSE
        // than a hole, because the depot is content-deduplicated and hands
        // back a plausible neighbouring texture instead of nothing, which is
        // silently wrong rather than visibly missing.
        if (have_layers && L < (int)tl.layers().size() && tl.layers()[(size_t)L].empty)
            continue;
        const int slot = (int)out.materials.size();
        layer_to_slot[L] = slot;

        GroundMaterial m;
        m.layer = L;
        if (have_layers && L < (int)tl.layers().size()) {
            const TerrainLayer& lay = tl.layers()[(size_t)L];
            // base_color() hands back the texture's FILE GUID, not a resource
            // name - the depot stores guids and the partition index is what
            // turns one into something get_res can open. Handing the guid
            // straight to a renderer gives it a name that resolves to nothing
            // and looks like an unbound layer.
            m.albedo_res = res_for_guid(lay.material.base_color());
            m.normal_res = res_for_guid(lay.material.normal_height());
            // The single-material auxiliary slot is overloaded between the
            // flat default AO placeholder and authored coverage maps. Preserve
            // only the latter here: the generated evaluator multiplies masks
            // by these `_op` sheets (MP_Isolated L29/L30 are the visible case),
            // while treating default AO as coverage would erase ordinary
            // surfaces. This is read from the mounted depot record at runtime.
            auto op = lay.material.other_textures.find(tl::kTexDefault);
            if (op != lay.material.other_textures.end()) {
                std::string candidate = res_for_guid(op->second);
                std::string low = candidate;
                for (char& c : low) c = (char)tolower((unsigned char)c);
                const size_t slash = low.find_last_of('/');
                const std::string leaf = slash == std::string::npos ? low : low.substr(slash + 1);
                if (leaf.size() >= 3 && leaf.compare(leaf.size() - 3, 3, "_op") == 0)
                    m.coverage_res = std::move(candidate);
            }
            // The current install's evaluator bytecode is the only static join.
            //
            // Which layer consumes which texture group is decided by the
            // compositor's compiled bytecode and by nothing in the shipped
            // data. The former ordinal walk painted the wrong sheet over 98.8%
            // of mp_abbasid, 45.6% of mp_isolated and 16.8% of mp_badlands.
            //
            // The live CFG is the answer: evaluator case N is layer N and the
            // descriptor-register base is derived from this shader's metadata.
            if (m.albedo_res.empty()) {
                int tcv = -1, tnh = -1, tthird = -1;
                if (have_static && stab.layer_descriptors(L, tcv, tnh, tthird)) {
                    auto by_descriptor = [&stab](int d) -> std::string {
                        if (d < 0) return std::string();
                        for (const TerrainStaticGroup& g : stab.groups())
                            for (const TerrainStaticTexture& tx : g.tex)
                                if ((int)tx.descriptor == d) return tx.asset;
                        return std::string();
                    };
                    const std::string cv_asset = by_descriptor(tcv);
                    if (!cv_asset.empty()) m.albedo_res = res_for_name(cv_asset);
                    if (getenv("BF6_GS_DEBUG"))
                        fprintf(stderr, "  [gs] L%-3d tcv=%-4d asset='%s' res='%s'\n",
                                L, tcv, cv_asset.c_str(), m.albedo_res.c_str());
                    if (m.normal_res.empty()) {
                        const std::string nh_asset = by_descriptor(tnh);
                        if (!nh_asset.empty()) m.normal_res = res_for_name(nh_asset);
                    }
                }
            }
            m.metres_per_repeat = lay.material.metres_per_repeat(4.f);
            if (lay.material.uv_rotation_deg_set)
                m.uv_rotation_deg = lay.material.uv_rotation_deg;
            if (lay.material.tint_set) {
                m.tint[0] = lay.material.tint[0];
                m.tint[1] = lay.material.tint[1];
                m.tint[2] = lay.material.tint[2];
            }
            m.overlay = lay.material.overlay_strength_set
                      ? lay.material.overlay_strength : 1.f;

            // The evaluator's own constants, same reads the bake makes.
            m.base_height    = lay.material.base_height;
            m.displace_range = lay.material.displace_range;
            m.mask_ramp_exp  = lay.material.mask_ramp_exp_set &&
                               lay.material.mask_ramp_exp > 0.f
                             ? lay.material.mask_ramp_exp : 1.f;
            m.height_blend   = lay.material.height_blend;
            m.coord_scale[0] = lay.material.coord_scale_set &&
                               lay.material.coord_scale[0] != 0.f
                             ? lay.material.coord_scale[0] : 1.f;
            m.coord_scale[1] = lay.material.coord_scale_set &&
                               lay.material.coord_scale[1] != 0.f
                             ? lay.material.coord_scale[1] : 1.f;
            m.uv_offset[0]   = lay.material.uv_offset[0];
            m.uv_offset[1]   = lay.material.uv_offset[1];
        }
        out.materials.push_back(m);
        if (out.materials.size() >= 250) break;   // 255 is the "none" marker
    }

    out.size = cov.size;
    out.slots = cov.slots;
    out.lo[0] = cov.lo[0]; out.lo[1] = cov.lo[1];
    out.hi[0] = cov.hi[0]; out.hi[1] = cov.hi[1];
    // Counted BELOW, not carried over. A texel can be non-empty in the raster
    // and still have nothing left once modifier layers are dropped, so the
    // raster's own count would understate it - and adding to it would count
    // the same texel twice.
    out.empty_texels = 0;

    // The aerial photograph over the same window. Not fatal when absent: some
    // levels ship no colour map, and the sheets still draw.
    paint_colour_map(sp, dir, fetch, cov.lo, cov.hi, cov.size, out.colour, nullptr);
    out.idx.assign((size_t)cov.size * cov.size * (size_t)out.slots, 255);
    out.w.assign((size_t)cov.size * cov.size * (size_t)out.slots, 0);

    for (size_t i = 0; i < (size_t)cov.size * cov.size; i++) {
        // THE EVALUATION STACK, compacted into the existing four-channel ABI:
        // block-7 base first at full mask, followed by the three strongest
        // nonduplicate block-1 layers in ascending evaluator order. With no
        // base, all four block-1 layers remain available.
        //
        // The base must stay first even when its raw layer index is higher
        // than a paint layer: the game's evaluator establishes the substrate
        // first, then walks the paint list in ascending layer order. Sorting
        // all four together is a convincing but wrong approximation.
        int w_out = 0;
        int base_slot = -1;
        if (have_base && base.size > 0) {
            const int x = (int)(i % (size_t)cov.size);
            const int z = (int)(i / (size_t)cov.size);
            const float wx = cov.lo[0] + ((float)x + 0.5f) / (float)cov.size
                           * (cov.hi[0] - cov.lo[0]);
            const float wz = cov.lo[1] + ((float)z + 0.5f) / (float)cov.size
                           * (cov.hi[1] - cov.lo[1]);
            const float base_span_x = base.hi[0] - base.lo[0];
            const float base_span_z = base.hi[1] - base.lo[1];
            if (base_span_x > 0.f && base_span_z > 0.f) {
                const int bx = std::clamp((int)((wx - base.lo[0]) / base_span_x * base.size),
                                          0, base.size - 1);
                const int bz = std::clamp((int)((wz - base.lo[1]) / base_span_z * base.size),
                                          0, base.size - 1);
                const uint8_t raw = base.layer[(size_t)bz * base.size + bx];
                auto bit = layer_to_slot.find(raw);
                if (raw != 255 && bit != layer_to_slot.end()) {
                    base_slot = bit->second;
                    out.idx[i * out.slots] = (uint8_t)base_slot;
                    out.w[i * out.slots] = 255;
                    w_out = 1;
                }
            }
        }

        std::pair<int, uint8_t> detail[16];
        int detail_n = 0;
        for (int s = 0; s < cov.slots; s++) {
            const uint8_t weight = cov.w[i * cov.slots + s];
            if (weight == 0) break;                 // weight-sorted, first zero ends it
            auto it = layer_to_slot.find(cov.idx[i * cov.slots + s]);
            if (it == layer_to_slot.end()) continue;
            if (it->second == base_slot) continue;  // base already owns full coverage
            detail[detail_n++] = {it->second, weight};
        }
        std::sort(detail, detail + detail_n,
                  [](const std::pair<int, uint8_t>& a,
                     const std::pair<int, uint8_t>& b) { return a.first < b.first; });
        for (int d = 0; d < detail_n && w_out < out.slots; d++, w_out++) {
            out.idx[i * out.slots + w_out] = (uint8_t)detail[d].first;
            out.w[i * out.slots + w_out] = detail[d].second;
        }
        if (w_out == 0) out.empty_texels++;
    }
    return true;
}

bool ground_coverage(Source& src, const std::string& level, int size,
                     GroundCoverage& out, std::string& err)
{
    GroundCoverageOpts opts;
    opts.size = size;
    return ground_coverage(src, level, opts, out, err);
}

}  // namespace bf6
