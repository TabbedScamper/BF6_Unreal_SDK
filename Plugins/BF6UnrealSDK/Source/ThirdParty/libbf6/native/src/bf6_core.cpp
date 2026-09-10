/* libbf6 - public C ABI.
 *
 * Module 2 is done, so bf6_open now opens a real install: it builds a Source,
 * mounts the shared SuperBundle TOCs, and hands back a context whose catalogue
 * can be listed. Geometry (bf6_read_mesh) waits on the EBX + meshset modules and
 * is still stubbed.
 */
#include "bf6_core.h"

#include <cstdio>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <set>
#include <cstring>
#include <exception>
#include <new>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "installed_sound_wave.h"
#include "source.h"
#include "meshset.h"
#include "depot.h"
#include "decals.h"
#include "destruction.h"
#include "terrain.h"
#include "terrainshader.h"
#include "terraincomposite.h"
#include "stdio_compat.h"
#include "groundsplat.h"
#include "splat.h"
#include "texture.h"
#include "walk.h"
#include "placeables.h"
#include "velighting.h"
#include "levellights.h"
#include "lightingzones.h"
#include "fx.h"
#include "expression_graph.h"
#include "expression_registry.h"

namespace fs = std::filesystem;

struct bf6_rime_binding_cache {
    std::vector<bf6_rime_connection> connections;
    std::vector<bf6_rime_event_connection> event_connections;
    std::vector<bf6_rime_conditional_float> conditional_floats;
    std::vector<bf6_rime_float_interpolator> float_interpolators;
    std::vector<bf6_rime_conditional_property> conditional_properties;
    std::vector<bf6_rime_logic_operation> logic_operations;
    std::vector<bf6_rime_property_status> property_statuses;
    std::vector<bf6_rime_math_instruction> math_instructions;
    std::vector<bf6_rime_math_operation> math_operations;
    std::vector<bf6_rime_rounding> roundings;
    std::vector<bf6_rime_property_cast> property_casts;
    std::vector<bf6_rime_compare_float> compare_floats;
    std::vector<bf6_rime_array_element> array_elements;
    std::vector<bf6_rime_logic_reference> logic_references;
    std::vector<int32_t> interface_descriptors;
    std::vector<bf6_rime_interface_field> interface_fields;
    std::vector<bf6_rime_interface_struct_type> interface_struct_types;
    std::map<int32_t, uint32_t> string_entities;
    std::vector<bf6_rime_string_argument> string_arguments;
};

struct bf6_ctx {
    bf6::Source      src;
    std::map<int, std::vector<bf6::TerrainShaderBindingRecord>> surface_schemas;
    // Backing store for the slot names bf6_armory_slots hands out as
    // char*. Owned by the context, like every other string in this ABI.
    std::vector<std::string> armory_slot_names;
    std::vector<std::string> armory_category_names;
    std::vector<std::string> rime_names;
    std::vector<bf6_rime_node> rime_tree_rows;
    std::string                rime_tree_key;
    bf6_rime_tree_stats        rime_tree_stats{};
    // Property connections and localized-string entities belong to the same
    // partition. Keep their decoded rows together so the C count/fill calls
    // and subsequent per-source lookups do not decompress and parse that EBX
    // once per connection.
    std::string                         rime_bindings_key;
    std::vector<bf6_rime_connection>    rime_binding_connections;
    std::vector<bf6_rime_event_connection> rime_binding_event_connections;
    std::vector<bf6_rime_conditional_float> rime_binding_conditional_floats;
    std::vector<bf6_rime_float_interpolator> rime_binding_float_interpolators;
    std::vector<bf6_rime_conditional_property> rime_binding_conditional_properties;
    std::vector<bf6_rime_logic_operation> rime_binding_logic_operations;
    std::vector<bf6_rime_property_status> rime_binding_property_statuses;
    std::vector<bf6_rime_math_instruction> rime_binding_math_instructions;
    std::vector<bf6_rime_math_operation> rime_binding_math_operations;
    std::vector<bf6_rime_rounding> rime_binding_roundings;
    std::vector<bf6_rime_property_cast> rime_binding_property_casts;
    std::vector<bf6_rime_compare_float> rime_binding_compare_floats;
    std::vector<bf6_rime_array_element> rime_binding_array_elements;
    std::vector<bf6_rime_logic_reference> rime_binding_logic_references;
    std::vector<int32_t>                    rime_binding_interface_descriptors;
    std::vector<bf6_rime_interface_field>    rime_binding_interface_fields;
    std::vector<bf6_rime_interface_struct_type>
                                                rime_binding_interface_struct_types;
    std::map<int32_t, uint32_t>         rime_binding_string_entities;
    std::vector<bf6_rime_string_argument> rime_binding_string_arguments;
    // Rime screens repeatedly share the same widget partitions.  A one-entry
    // cache made every partition loaded after the first evict the previous
    // one, so returning to Weapons/Home reparsed the complete graph family.
    // These rows are immutable projections of mounted EBX and are safe to
    // retain for the lifetime of the context.
    std::map<std::string, bf6_rime_binding_cache> rime_bindings_by_partition;
    // The front end's colour palette, read once. `tried` is kept apart from
    // `empty` so a failed read is not retried once per element.
    std::vector<bf6_rime_color>   rime_palette;
    std::map<std::string, size_t> rime_palette_by_id;
    bool                          rime_palette_tried = false;
    // The authored text styles, and the raw sfnt of the last font asked for.
    // The font blob is held so the ABI can hand out a pointer-free size/fill
    // pair without re-reading the archive between the two calls.
    std::vector<bf6_rime_font_style> rime_font_styles;
    std::map<std::string, size_t>    rime_font_by_name;
    bool                             rime_fonts_tried = false;
    std::string                      rime_font_blob_name;
    std::vector<uint8_t>             rime_font_blob;
    // Inline [img] mappings followed from the installed text-markup config.
    std::vector<bf6_rime_markup_svg> rime_markup_svgs;
    bool                             rime_markup_svgs_tried = false;

    // Photon offline visual bundle.  The rows are copied values; the chunk is
    // retained only long enough to validate ranges and file signatures.  A
    // consumer obtains its own one-slot view with BF6_RAW_CHUNK.
    bool                             photon_assets_tried = false;
    bf6_photon_bundle_info           photon_bundle_info{};
    std::vector<bf6_photon_asset>    photon_assets;
    std::vector<std::string> icon_names;
    std::vector<std::string> part_meshes, part_bundles;
    std::vector<std::string> factory_fit_slots, factory_fit_tokens;
    std::vector<bf6_weapon_fit> factory_fit_rows;
    std::vector<float>       part_attach_offsets; // 3 floats per returned part
    std::vector<int32_t>     part_has_attach_offset;
    std::map<uint32_t, std::string> loc_en;
    bool loc_en_tried = false;
    std::vector<std::string> weapon_ui_strings;
    std::string weapon_packages_key;
    std::vector<bf6_weapon_package_row> weapon_packages;
    std::string weapon_package_configs_key;
    std::vector<bf6_weapon_package_config> weapon_package_configs;
    // The attachment catalogue for the last weapon asked about.
    std::vector<bf6_attachment_catalogue_row> catalogue;
    std::vector<bf6_weapon_name_row> weapon_names;
    bool gadget_ui_metadata_tried = false;
    std::vector<bf6_gadget_ui_metadata> gadget_ui_metadata;
    bool melee_ui_metadata_tried = false;
    std::vector<bf6_melee_ui_metadata> melee_ui_metadata;
    bool vehicle_archetypes_tried = false;
    std::vector<bf6_vehicle_archetype> vehicle_archetypes;
    bool vehicle_loadout_presets_tried = false;
    std::vector<bf6_vehicle_loadout_preset> vehicle_loadout_presets;
    bf6::PlaceableDB pdb;
    int lifted = 0;

    // The level currently mounted and walked, with the type schema it needed.
    // Held on the context because mounting and indexing cost seconds: a caller
    // that asked for placements twice should pay once.
    std::unique_ptr<bf6::TypeDb> types;
    std::unique_ptr<bf6::Walk>   walk;
    std::string                  walked_level;
    // The level whose archives are MOUNTED, which is a weaker thing than
    // walked and the only thing the ground and water readers need. They are
    // kept apart on purpose: on an EA App install the walk cannot run at all
    // until the executable is lifted, and the terrain has no business being
    // unreadable because of that.
    std::string                  mounted_level;

    // WHAT EACH HANDED-OUT POINTER ACTUALLY IS.
    //
    // bf6_free takes a void* and used to cast it to MeshHandle* unconditionally,
    // which was fine while a mesh was the only thing this API handed out. The
    // moment a second handle type existed, freeing one ran the WRONG
    // destructor over it and released whatever the other type happened to have
    // at those offsets - an access violation inside the free, nowhere near the
    // call that was actually wrong.
    //
    // The ABI hands out bare pointers by design, so the type has to be
    // remembered here. Registered on the way out, looked up on the way back.
    enum HandleKind { HK_MESH = 1, HK_TERRAIN = 2, HK_SKELETON = 3,
                      HK_ADJACENCY = 4, HK_HAIRBIND = 5,
                      HK_RENDERBONES = 6, HK_ANIMRELOC = 7, HK_ANIMCLIP = 8, HK_PSD = 9, HK_PSDMAP = 10, HK_SWARM = 11, HK_TELEMETRY = 12, HK_SPAWNS = 13, HK_VSHAPES = 14, HK_FOOTPRINTS = 15, HK_AUTOPAINT = 16, HK_ECSSYSTEM = 17, HK_DEBRIS = 18, HK_WIND = 19, HK_WEATHER = 20, HK_WAVESEL = 21, HK_BEHAVIORTREE = 22, HK_NETREGISTRY = 23, HK_UNLOCKS = 24, HK_GEM = 25, HK_SCHEMATIC = 26, HK_PHYSICS = 27, HK_OCCLUDER = 28, HK_PMVOLUME = 29, HK_LIGHTPROBE = 30 };
    std::map<void*, int> handles;

    // The last coverage, and the C view of its material list. Same lifetime
    // rule as the bake: the context owns them so a caller frees nothing.
    std::unique_ptr<bf6::GroundCoverage> ground;
    std::vector<bf6_ground_material>     ground_mats;

    // Exact MeshScatteringDatabase catalogue and its C-string backing store.
    std::vector<std::string>             scatter_names, scatter_meshes;
    std::vector<bf6_scatter_entry>       scatter_rows;

    // The last ground bake, so its buffers outlive the call that made it and
    // a second bake replaces the first rather than leaking it.
    std::unique_ptr<bf6::TerrainBake> bake;

    // The live utility raster used by CoarseMask water attenuation. The C ABI
    // returns views into these buffers, matching the ownership convention used
    // by textures and ground coverage. A subsequent mask read replaces them.
    std::vector<uint8_t>              water_mask_atlas;
    std::vector<uint32_t>             water_mask_indirection;

    // bf6_variation_live answers, keyed res|bundle|variation. The question is
    // asked once per distinct triple while a level's groups are being formed,
    // and each answer costs a meshset parse.
    std::map<std::string, int> var_live;

    // ---- terraindecals ---------------------------------------------------
    // Parsed once per level and kept, for the same reason the walk is: the
    // container is megabytes and a caller commonly asks for a count before it
    // asks for the rows.
    std::unique_ptr<bf6::Decals>     decals;
    std::string                      decal_level;
    std::vector<std::vector<float> > decal_verts;
    std::vector<bf6_decal>           decal_rows;

    // ---- lighting ---------------------------------------------------------
    // The last decoded VisualEnvironment, kept only so its import list has
    // somewhere to live for bf6_level_lighting_imports. The lighting record
    // itself is copied out by value.
    bf6::VeLighting          ve;
    std::string              ve_level;
    std::vector<const char*> ve_import_ptrs;

    // ---- local lights ------------------------------------------------------
    // The traversal is tens of seconds on a big map and a caller commonly asks
    // for a count before it asks for the rows, so both the decoded lights and
    // the C view of them are kept. The C view holds pointers into the C++
    // strings, which is why the vector they live in has to be owned here.
    std::vector<bf6::LevelLight> lights;
    std::vector<bf6_light>       light_rows;
    bf6::LightStats              light_stats;
    std::string                  lights_level;

    // ---- local VisualEnvironment zones -----------------------------------
    std::vector<bf6::LightingZone> lighting_zones;
    std::vector<bf6_lighting_zone> lighting_zone_rows;
    std::vector<std::vector<float>> lighting_zone_points;
    bf6::LightingZoneStats         lighting_zone_stats;
    std::string                    lighting_zones_level;

    // ---- game mode layouts (gamemode_ext.inc) ---------------------------
    // Cached per level like the lighting zones: the walk is seconds and a
    // caller asks for a count before it asks for the rows.
    std::string                        gm_level;
    std::vector<bf6_gm_entity>         gm_rows;
    std::vector<std::vector<float>>    gm_points;
    std::vector<std::vector<int32_t>>  gm_links;
    std::vector<std::vector<uint32_t>> gm_link_fields;
    std::set<std::string>              gm_strings;      // interned; set nodes never move
    std::vector<std::string>           gm_other_types;
    std::vector<const char*>           gm_other_ptrs;
    std::vector<std::string>           gm_spawner_paths;
    std::vector<const char*>           gm_spawner_ptrs;
    std::vector<std::string>           gm_layer_names;
    std::vector<std::string>           gm_dropped_types;
    std::vector<const char*>           gm_dropped_ptrs;
    bf6_gm_stats                       gm_stats{};

    bf6_progress_fn progress = nullptr;
    void*           progress_user = nullptr;

    // ---- raw asset door ---------------------------------------------------
    // ONE slot, reused by every bf6_read_raw. See the ABI note: the caller is
    // copying the bytes straight out, so a handle per read would make it free
    // a hundred thousand times and a size-then-fill protocol would decompress
    // everything twice. Owned here so it dies with the context.
    std::vector<uint8_t> raw_buf;

    // ---- materials -------------------------------------------------------
    // Depots are parsed lazily and kept: a level touches a few hundred of the
    // thousands in a mount, and each is independent, which is what makes the
    // bundle a natural unit here.
    struct DepotHold { bf6::Depot depot; std::vector<uint8_t> bytes; };
    std::map<std::string, DepotHold> depots;

    // One entry per distinct texture RESOURCE, decoded on demand. Deduplicated
    // by name because the same texture is bound by many materials, and decoding
    // a 4K BC7 sheet once per binding would be minutes of nothing.
    struct TexHold {
        std::string      res;
        bf6::TextureImage img;
        bf6_texture      abi{};
        bool             tried = false, ok = false;
    };
    std::vector<TexHold>          textures;
    std::map<std::string, int32_t> tex_by_res;

    struct CappedTexHold {
        bf6::TextureImage img;
        bf6_texture abi{};
        bool tried = false, ok = false;
    };
    std::map<uint64_t, CappedTexHold> capped_textures; // (id,max-dimension)

    // FORGET EVERY DECODED TEXTURE, KEEPING THE IDS.
    //
    // A texture id is the index of its resource NAME, so ids stay valid across
    // a level change and callers may hold them. The DECODE does not: it was
    // produced against the mount that was open at the time, and a level change
    // remounts. Leaving `tried` set meant the first map's pixels were served
    // for the second map's resources - one map's scenery appearing on another,
    // and a decode running against archives that had moved underneath it.
    //
    // So the names and their ids survive and only the pixels are dropped.
    void forget_texture_decodes() {
        capped_textures.clear();
        for (TexHold& h : textures) {
            h.img = bf6::TextureImage();
            h.abi = bf6_texture{};
            h.tried = false;
            h.ok = false;
        }
    }

    bf6::Depot* depot_named(const std::string& name, const std::vector<uint8_t>** out_bytes) {
        if (name.empty()) return nullptr;
        auto it = depots.find(name);
        if (it == depots.end()) {
            std::string e;
            DepotHold h;
            h.bytes = src.get_res(name, e);
            if (h.bytes.empty() || !h.depot.parse(h.bytes, e)) {
                depots.emplace(name, DepotHold{});   // remember the failure too
                return nullptr;
            }
            it = depots.emplace(name, std::move(h)).first;
        }
        if (it->second.bytes.empty()) return nullptr;
        *out_bytes = &it->second.bytes;
        return &it->second.depot;
    }

    int32_t texture_id(const std::string& res) {
        auto it = tex_by_res.find(res);
        if (it != tex_by_res.end()) return it->second;
        const int32_t id = (int32_t)textures.size();
        TexHold h;
        h.res = res;
        textures.push_back(std::move(h));
        tex_by_res.emplace(res, id);
        return id;
    }

    // Fanned out to the pieces that take time. Returns false when the caller
    // asked to stop.
    bool report(const char* stage, int done, int total) {
        if (!progress) return true;
        return progress(progress_user, stage, done, total) != 0;
    }
};

// Backing store for a returned bf6_mesh. `mesh` is the first member so a
// bf6_mesh* handed out can be cast back for bf6_free.
// Backing store for a returned bf6_terrain.
struct TerrainHandle {
    bf6_terrain           t{};
    std::vector<uint16_t> heights;
};

// WHICH ENGINE SLOT A DEPOT SLOT FEEDS.
//
// From the research hub's SHADERS.md table plus the slots named here from the
// bindings themselves. Two things in it are easy to get wrong and expensive:
//
//  - a "_cs" base colour's ALPHA IS SMOOTHNESS, not opacity. The real opacity
//    twin is a separate slot (0xD405B0E1) that shares its high half, which is
//    why the full 32 bits always have to be compared.
//  - the NAMED emissive slot (0xD405B0E5) is bound by almost nothing and binds
//    black when it is; the glow that actually lights things is 0x407055FD.
// WHICH TEXCOORD IS THE PRIMARY UV, per material family.
//
// It is NOT a property of the geometry, and it must not be guessed. A previous
// attempt picked whichever channel "fits inside 0..1" and shipped; it was wrong
// and had to be retracted, because fitting 0..1 is how you RECOGNISE AN UNWRAP,
// and on architecture that channel is the lightmap/AO unwrap - buildings drew
// one brick the size of a facade.
//
//   *Unique*   sections : always TC0 (the prop's own atlas is authored there)
//   *CarPaint* sections : the DEPOT names it. Bool const 0x4F5F0664 on the
//                         section's record: 1 = TC1, 0 or absent = TC3.
//                         Every geometric fit provably fails here - one car's
//                         TC3 v spans -2.12..0.99 because unliveried pieces are
//                         parked outside the sheet.
//   everything else     : TC0
static int primary_uv_channel(const std::string& material, const bf6::MaterialBinding& mb)
{
    // DATA-DRIVEN, NEVER BY NAME, and never on one signal.
    //
    // The material names in this content do not contain "carpaint" at all, so a
    // name test quietly matches nothing and reports a clean zero - which reads
    // as agreement rather than as a detector that never fired.
    //
    // And the flakes slot ALONE is not enough either: a portable fuel canister
    // and a machine gun bind it, and forcing those onto TC3 would break props
    // that are correct on TC0. The full rule is flakes bound AND no base colour
    // texture AND no tile-paint palettes.
    (void)material;
    const bool carpaint =
        mb.textures.count(0xA11011B8) &&
        !mb.textures.count(0x54BBCD30) &&
        !mb.constants.count(0xF1CEE56D) && !mb.constants.count(0xF1CEE56E);

    if (carpaint) {
        auto it = mb.constants.find(0x4F5F0664);
        const bool tc1 = it != mb.constants.end() && !it->second.empty() && it->second[0] != 0;
        return tc1 ? 1 : 3;
    }
    return 0;   // Unique and everything else
}

static bf6_fmt fmt_of(int dxgi)
{
    switch (dxgi) {
    case 71: case 72: return BF6_FMT_BC1;
    case 74: case 75: case 77: case 78: return BF6_FMT_BC3;
    case 80: return BF6_FMT_BC4;
    case 83: return BF6_FMT_BC5;
    case 98: case 99: return BF6_FMT_BC7;
    case 95: return BF6_FMT_BC6H_U;
    case 96: return BF6_FMT_BC6H_S;
    case 28: return BF6_FMT_RGBA8;
    case 61: return BF6_FMT_R8;
    case 10: return BF6_FMT_RGBA16F;
    case 24: return BF6_FMT_RGB10A2;
    default: return BF6_FMT_UNKNOWN;
    }
}

// FAR-LOD IMPOSTORS PUT THEIR ALBEDO SOMEWHERE ELSE.
//
// An M_Vista / M_LastLod / M_Projection record binds NOTHING at the nominal
// base colour 0x54BBCD30 and puts its baked atlas at 0x54BBCD22 - the slot that
// carries a vehicle LIVERY on an ordinary material. The three share a
// 0x54BBCD** prefix and look like variants of one authored parameter under
// different shader families.
//
// So this cannot be a blanket mapping: making 0x54BBCD22 an albedo everywhere
// would paint a car with its livery sheet as the base colour. It is a
// FALLBACK, taken only when neither real base-colour slot is bound - which is
// exactly the condition the impostor records satisfy and ordinary ones do not.
//
// Measured on MP_Battery: 96 drawn sections across 768 placed instances bind
// 0x54BBCD22 with no base colour, and they are the out-of-bounds skyline.
// ---------------------------------------------------------------- colour
//
// A DEPOT RECORD CAN CARRY THE COLOUR ITSELF, and for whole families of prop
// that is the ONLY place the colour exists. Car paint is the clearest case: a
// carpaint record binds a flakes normal and NO base-colour texture at all, so
// a reader that only ever looks for a texture finds nothing, falls through to
// white, and every car on the map is white.
//
// Three sources, in priority order. They are exclusive in the data, but the
// order is stated rather than assumed because a record that carried two would
// otherwise resolve differently depending on map iteration order.
//
//   1. car paint      the body colour, and its smoothness
//   2. tile paint     the per-zone palette these architectural kits use
//   3. an albedo tint a MULTIPLIER over the sheet, which is where the
//                     "everything looks washed out / too dark" class of
//                     mismatch lives
//
// All of these end up in base_color, which a consumer multiplies its albedo
// sheet by. That works for both kinds at once: where there IS no sheet the
// consumer multiplies white and gets the colour, and where there is one it
// gets the tint. One field, no branch on the consumer's side.

// A float3 out of a constant blob at a byte offset, or false if it is short.
static bool const_c3(const bf6::MaterialBinding& mb, uint32_t hash, size_t off, float out[3])
{
    auto it = mb.constants.find(hash);
    if (it == mb.constants.end()) return false;
    const std::vector<uint8_t>& b = it->second;
    if (b.size() < off + 12) return false;
    std::memcpy(&out[0], b.data() + off + 0, 4);
    std::memcpy(&out[1], b.data() + off + 4, 4);
    std::memcpy(&out[2], b.data() + off + 8, 4);
    return true;
}

static bool const_f1(const bf6::MaterialBinding& mb, uint32_t hash, float& out)
{
    auto it = mb.constants.find(hash);
    if (it == mb.constants.end() || it->second.size() < 4) return false;
    std::memcpy(&out, it->second.data(), 4);
    return true;
}

// The tints are authored around a neutral, and the neutral means "no tint".
// 0.004 is the tolerance the reference uses; the doubling below is because the
// per-family tints are stored at half scale so that 0.5 is identity.
static const float kTintEps = 0.004f;

static bool near3(const float c[3], float v)
{
    return std::fabs(c[0] - v) < kTintEps &&
           std::fabs(c[1] - v) < kTintEps &&
           std::fabs(c[2] - v) < kTintEps;
}

static void resolve_colour(const bf6::MaterialBinding& mb, bf6_material_desc& md)
{
    const bool tilepaint = mb.constants.count(0xF1CEE56D) != 0;

    // 1. CAR PAINT. The full conjunction, for the same reason the UV rule uses
    //    it: the flakes slot alone is also bound by a fuel canister and a
    //    machine gun, and painting those in body colour would be worse than
    //    leaving them alone.
    if (mb.textures.count(0xA11011B8) &&
        !mb.textures.count(0x54BBCD30) && !mb.textures.count(0x54BBCD36) &&
        !tilepaint)
    {
        float body[3];
        if (const_c3(mb, 0xDD0512FA, 0, body))
        {
            md.base_color[0] = body[0];
            md.base_color[1] = body[1];
            md.base_color[2] = body[2];
            float smooth = 0.5f;
            const_f1(mb, 0xFE9EDB18, smooth);
            if (smooth < 0.f) smooth = 0.f;
            if (smooth > 1.f) smooth = 1.f;
            md.roughness = 1.f - smooth;
            return;
        }
    }

    // 2. TILE PAINT, at zone 0.
    //
    // Which of the eight zones a vertex takes is a PER-VERTEX selector, so the
    // honest answer needs the mesh, and a section can legitimately span two
    // zones. Entry 0 is the body colour on every record measured, which makes
    // it right by construction here rather than right by luck - but it is
    // still a floor, not the full rule, and a section that spans zones takes
    // one colour where the game takes two.
    if (tilepaint)
    {
        float a[3];
        if (const_c3(mb, 0xF1CEE56D, 0, a))
        {
            md.base_color[0] = a[0];
            md.base_color[1] = a[1];
            md.base_color[2] = a[2];
            return;
        }
    }

    // 3. AN ALBEDO TINT, which multiplies whatever sheet is bound.
    float t[3];
    if (const_c3(mb, 0x8A369BB2, 0, t) && !near3(t, 1.f))
    {
        md.base_color[0] = t[0]; md.base_color[1] = t[1]; md.base_color[2] = t[2];
        return;
    }
    // 0x686A1072 is NOT an albedo tint, and is deliberately not read here.
    //
    // It was, at half scale, and the reading produced base colours the game
    // never draws: on mp_isolated 2,010 of 11,130 records with a real colour
    // sheet carry a non-neutral value, 600 of them below 0.25 (lockers at
    // (0.001, 0.156, 0.469), metal racks (0.003, 0.042, 0.275), plastic
    // barrels (0.011, 0.148, 0.541), a weapon-bench storage box at a flat
    // 0.102 over an orange sheet) and 718 above 0.55 (a window at 1.75). A
    // float3 spanning three and a half decades whose hue is always a
    // near-zero red with a rising blue is three packed scalars, not a
    // colour. The container measurement below already said as much: the
    // same fixed blue on every variant, and never the variant's colour.
    //
    // The architecture layer tint is its own family (0 of 7,460 records
    // carry both), documented with a per-vertex lane, and stays as it was.
    if (const_c3(mb, 0x888A432A, 0, t))
    {
        if (!near3(t, 0.5f) && !near3(t, 0.4995f))
        {
            md.base_color[0] = t[0] * 2.f;
            md.base_color[1] = t[1] * 2.f;
            md.base_color[2] = t[2] * 2.f;
            if (!near3(md.base_color, 1.f)) return;
            md.base_color[0] = md.base_color[1] = md.base_color[2] = 1.f;
        }
    }
    // The eight-entry colour table, used ONLY where every entry agrees.
    //
    // Which entry a vertex takes is again per-vertex, so with no selector in
    // hand the only safe reading is a table that says the same thing whichever
    // entry is chosen. Taking entry 0 regardless would be a guess, and on a
    // table authored with eight different props in it, a badly wrong one.
    auto tab = mb.constants.find(0xC2BB295A);
    if (tab != mb.constants.end() && tab->second.size() >= 124)
    {
        float e0[3];
        if (const_c3(mb, 0xC2BB295A, 0, e0))
        {
            bool uniform = true;
            for (int k = 1; k < 8 && uniform; k++)
            {
                float ek[3];
                if (!const_c3(mb, 0xC2BB295A, (size_t)(16 * k), ek)) { uniform = false; break; }
                for (int j = 0; j < 3; j++)
                    if (std::fabs(ek[j] - e0[j]) > 1e-5f) { uniform = false; break; }
            }
            if (uniform && !near3(e0, 0.5f) && !near3(e0, 0.4995f))
            {
                float c[3] = { e0[0] * 2.f, e0[1] * 2.f, e0[2] * 2.f };
                if (!near3(c, 1.f))
                {
                    md.base_color[0] = c[0];
                    md.base_color[1] = c[1];
                    md.base_color[2] = c[2];
                }
            }
        }
    }
}

// Opaque architecture palettes use SubMaterialIndex, not one colour for the
// whole section. Preserve the existing C ABI: linear RGBA8 vertex multipliers
// and a shared scale in base_color retain values above one without clipping.
// Glass transmission and tile-paint blending are different shader paths.
static std::vector<uint32_t> resolve_layer_colours(const bf6::MaterialBinding& mb,
    const bf6::MeshGeomSection& section, bf6_material_desc& md)
{
    if (section.layer_lanes.empty() || md.translucent || mb.constants.count(0xF1CEE56D)) return {};
    if (mb.textures.count(0xA11011B8) && !mb.textures.count(0x54BBCD30) &&
        !mb.textures.count(0x54BBCD36) && mb.constants.count(0xDD0512FA)) return {};
    float paint[3];
    if (const_c3(mb, 0x8A369BB2, 0, paint) && !near3(paint, 1.f)) return {};
    float palette[8][3]{};
    int count = 0;
    const uint32_t trio[3] = {0x888A432A, 0x888A432B, 0x888A4328};
    bool trio_complete = true, trio_tinted = false;
    for (int k = 0; k < 3; ++k) {
        if (!const_c3(mb, trio[k], 0, palette[k])) { trio_complete = false; break; }
        trio_tinted |= !near3(palette[k], .5f);
    }
    if (!trio_complete && trio_tinted) return {};
    if (trio_complete && trio_tinted) count = 3;
    else {
        for (int k = 0; k < 8; ++k) {
            if (!const_c3(mb, 0xC2BB295A, (size_t)k * 16, palette[k])) return {};
        }
        count = 8;
    }
    for (int k = 0; k < count; ++k) {
        const bool neutral = near3(palette[k], .5f);
        for (float& c : palette[k]) {
            if (!std::isfinite(c) || c < 0.f) return {};
            c = neutral ? 1.f : 2.f * c;
        }
    }
    // Only used entries matter. In particular, an unused blue palette entry
    // must not recolour geometry that exclusively selects the white entry.
    const int first = std::min((int)section.layer_lanes[0], count - 1);
    float scale = 1.f;
    bool uniform = true;
    for (uint8_t lane : section.layer_lanes) {
        const int k = std::min((int)lane, count - 1);
        for (int j = 0; j < 3; ++j) {
            scale = std::max(scale, palette[k][j]);
            uniform &= std::fabs(palette[k][j] - palette[first][j]) < 1e-6f;
        }
    }
    if (uniform) {
        std::copy(palette[first], palette[first] + 3, md.base_color);
        return {};
    }
    std::vector<uint32_t> colours;
    colours.reserve(section.layer_lanes.size());
    for (uint8_t lane : section.layer_lanes) {
        const int k = std::min((int)lane, count - 1);
        uint32_t rgba = 0xff000000u;
        for (int j = 0; j < 3; ++j)
            rgba |= (uint32_t)std::lround(palette[k][j] / scale * 255.f) << (j * 8);
        colours.push_back(rgba);
    }
    md.base_color[0] = md.base_color[1] = md.base_color[2] = scale;
    return colours;
}

// AND CAR PAINT IS THE COUNTER-EXAMPLE THAT ALMOST BROKE THIS.
//
// "Binds the wrap slot and no base colour" is NOT enough. A carpaint record
// satisfies it exactly: it has no base-colour texture (its colour is a
// constant) and it does bind 0x54BBCD22 - with a DEFAULT sheet,
// common/shaders/textures/default/t_base_ca, standing in for a livery it does
// not have. Measured on MP_Battery, 87 carpaint sections match the naive rule,
// and taking it would have textured every car with a blank default and thrown
// away the body colour that was sitting in the record all along.
//
// So the flakes slot excludes it, the same way it identifies car paint
// everywhere else in this file.
static bool wrap_is_the_albedo(const bf6::MaterialBinding& mb)
{
    return mb.textures.count(0x54BBCD22) &&
           !mb.textures.count(0x54BBCD30) &&
           !mb.textures.count(0x54BBCD36) &&
           !mb.textures.count(0xA11011B8);
}

// WHICH SLOT ON THIS RECORD IS THE BASE COLOUR - as a chain, not a set.
//
// There is no single base-colour hash. The one everybody knows, 0x54BBCD30,
// covers most props, and 0x54BBCD36 covers vegetation, but whole families bind
// their colour somewhere else entirely and bind NOTHING at either: facades put
// it at 0x21F3F4E1, trim sheets at 0xEA026FC7, tile-breaker kits at 0xA4415059,
// far-LOD impostors at 0x54BBCD22.
//
// Read off the game rather than guessed. Censusing every slot on the depots
// that MP_Battery's placed props actually resolve to, and counting what asset
// suffix each is bound to, these come back 91-100% "_cs" on per-asset paths -
// which is what a base colour looks like and what a shared weathering sheet
// does not.
//
// STRICTLY A FALLBACK CHAIN, in this order, and only the first one present is
// taken. That matters: several of these are OVERLAY layers on a record that
// also has a real base colour, and promoting an overlay over the sheet beneath
// it would repaint a correct prop. Taken only where the alternative is drawing
// untextured, it can only be an improvement on nothing.
// `usable` answers "does this slot resolve to a sheet that is not a
// placeholder". THE TEST HAS TO BE INSIDE THE WALK, not applied to its answer.
// Applied afterwards, the chain has already stopped at the first slot that is
// PRESENT, and a record whose 0x54BBCD30 holds
// common/shaders/textures/default/t_base_cs while a real facade sheet sits at
// 0x21F3F4E1 draws untextured with its own colour one entry further down the
// list. Measured on four levels: 27 southern-Europe facade sections exactly
// like that. Passing no predicate keeps the old first-present behaviour, which
// is what a caller with no resolver in hand can do.
static uint32_t albedo_slot_of(const bf6::MaterialBinding& mb,
                               const std::function<bool(uint32_t)>& usable
                                   = std::function<bool(uint32_t)>())
{
    static const uint32_t chain[] = {
        0x54BBCD30,   // the ordinary base colour
        0x54BBCD36,   // vegetation
        0x21F3F4E1,   // facade            4,697 instances, 91.2% "_cs"
        0xEA026FC7,   // trim              5,129 instances, 96.8% "_cs"
        0xA4415059,   // tile-breaker      3,679 instances, 100%  "_cs"
        // ASPHALT, which is why the roads read as empty space. The broken
        // asphalt kit is what dresses a road surface and its edges, and it
        // binds its colour at its own two hashes and nothing at 0x54BBCD30 -
        // so every piece of it drew untextured. 1,965 placed instances on
        // MP_Battery between them.
        0x691A5E17,   // asphalt ridge     1,079 instances, 100%  "_cs"
        0x691BEAB4,   // asphalt cracked     886 instances, 100%  "_cs"
        0x39DA140E,   // plaster           1,048 instances, 68.7% "_cs"
        0xA17E658F,   // cable / wire steel  159 instances, 100%  "_cs"
        0x365B13EF,   // backdrop terrain     29 instances, 100%  "_cs"
        0x1C5FA3EE,   // backdrop hulls       10 instances, 100%  "_cs"
        // FROM THE FLEET CENSUS, which dumped the unbound albedo slots rather
        // than counting them. Of 34,582 sections with no base colour, most are
        // not misses at all: 17,118 glass and 9,010 car paint carry their
        // colour as a record CONSTANT, and 3,275 are shader-computed. That
        // leaves 4,430 genuine misses, and these five slots are two thirds of
        // them. Sizes here are sections, not instances, so they are far below
        // the detail layers warned about below and cannot win everywhere.
        0x87180B38,   // prop decal "_ca"   3,152 sections over 26 levels
        0x31EBABA9,   // backdrop            ~440 sections
        0xC8C9370A,   // backdrop            ~440 sections
        0x62DFB21A,   // backdrop            ~440 sections
        0xC670A912,   // light fixture "_ca"   258 sections
        0x2BD6CCCD,   // authored billboard advertisement sheet
    };
    // DELIBERATELY NOT IN THE CHAIN, though they census as 97-100% "_cs" on
    // per-asset paths and look exactly like the entries above:
    //
    //   0x002E8ADD  moss detail        41,279 instances
    //   0x05FFAEDA  moss detail normal
    //   0x5D2D90F3  rock detail         2,172 instances
    //   0x2776F9F6  rock detail
    //
    // These are DETAIL layers - a tiling sheet blended over whatever is
    // underneath, present on nearly every record in the level. Promoting one
    // to a base colour is the single worst thing this chain could do: it is on
    // more sections than any real albedo, so it would win almost everywhere
    // and paint the whole map in moss. That has happened before here, which is
    // why is_detail_layer exists.
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++)
        if (mb.textures.count(chain[i]) && (!usable || usable(chain[i])))
            return chain[i];
    // The impostor sheet last, and only under its own guard - see below.
    if (wrap_is_the_albedo(mb) && (!usable || usable(0x54BBCD22))) return 0x54BBCD22;
    return 0;
}

static bf6_tex_slot slot_for(uint32_t n32, bool& out_known, uint32_t albedo_slot = 0)
{
    out_known = true;
    if (albedo_slot && n32 == albedo_slot) return BF6_TEX_ALBEDO;
    switch (n32) {
    case 0x54BBCD30: case 0x54BBCD36:                 return BF6_TEX_ALBEDO;
    // The impostor's paired "_nsm": RG is the tangent normal, B is wetness
    // response and A is smoothness. Listed by its own hash rather than by a
    // "0xEC35 is a normal" rule, which would also catch a subsurface map.
    case 0xEC35AA10:                                  return BF6_TEX_NORMAL;
    // More normals, from the same census: each is 77-97% bound to an "_nv" /
    // "_nvt" / "_nts" / "_nma" asset sheet on a per-asset path. Listed one by
    // one for the reason the table has always given - one member of this
    // family is a subsurface map, so "0xEC35 is a normal" is not a rule.
    case 0xEC35AA69:                                  return BF6_TEX_NORMAL;
    case 0xEC35A742:                                  return BF6_TEX_NORMAL;
    case 0x2A507435: case 0x2C6B47EB:                 return BF6_TEX_NORMAL;
    // From the same census: an "_nms" sheet on 1,492 sections that was being
    // dropped as unknown rather than bound as a normal.
    case 0x6A19658A:                                  return BF6_TEX_NORMAL;
    // Normals: one hash per SHADER FAMILY, not one globally.
    case 0xEC35A74C: case 0xEC35A9E2: case 0xEC35A757:
    case 0xEC35A68C: case 0xEC35A697:                 return BF6_TEX_NORMAL;
    // Weapon/prop `_wo`, not metallic/roughness/occlusion.  The channel
    // census identifies R=edge/wear and A=paint mask; G/B remain unknown.
    // Calling this MRO made the native viewer use wear as metalness, an
    // unidentified field as roughness, and a sparse field as AO, turning the
    // M4A1 black and marbled.  `_cs` alpha is the verified smoothness source.
    case 0xB1A29A3C:                                  return BF6_TEX_WO;
    case 0x407055FD: case 0xD405B0E5:                 return BF6_TEX_EMISSIVE;
    case 0xD405B0E1:                                  return BF6_TEX_MASK;
    default: break;
    }
    out_known = false;
    return BF6_TEX_ALBEDO;
}

// THE DETAIL-LAYER TRAP. This slot sits on nearly every record and its texture
// is named "..._cs", so any fallback that picks "something albedo-looking"
// takes it - and it painted every glass pane and a whole skyline moss-green
// before it was excluded upstream. Never treat it as a base colour.
static bool is_detail_layer(uint32_t n32)
{
    return n32 == 0x002E8ADD || n32 == 0x05FFAEDA;
}

// djb2 over the lowercased path, for the variation key. THE XOR VARIANT,
// MASKED TO 32 BITS: 5381 / *33 / ^c per the hub's HASHING_AND_IDS.md and the
// pipeline's own djb2_lower. This function first shipped as the classic ADD
// variant accumulating 64 bits, and every derived key it produced resolved
// nothing - which never errored, because a missing variation key falls back
// to the base record. That silent fallback is what "the liveries are not
// coming in" looks like from the outside.
static uint64_t djb2_lower(const std::string& s)
{
    uint32_t h = 5381;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h * 33u) ^ c;
    }
    return (uint64_t)h;
}

// A variation reuses a DERIVED key: sectionStateKey + djb2(variation path), as
// a genuine 64-bit add. The low dword's sum CARRIES into the high dword, so
// adding the halves independently is wrong on exactly the cases that matter.
static uint64_t variation_key(uint64_t state_key, const std::string& variation)
{
    if (variation.empty()) return state_key;
    // The hash is over the asset path with ".ebx" DROPPED and forward slashes,
    // exactly as the reference computes it - hashing the raw reference string
    // misses every entry, silently, thanks to the base-key fallback.
    std::string p = variation;
    for (char& ch : p) if (ch == 0x5C) ch = '/';
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".ebx") == 0) p.resize(p.size() - 4);
    if (p.empty()) return state_key;
    return state_key + djb2_lower(p);
}

/* Defined in skeleton_ext.inc, which is included below bf6_free. The
 * deleter is forward-declared rather than the struct: deleting an
 * incomplete type is undefined, and the compiler cannot warn about it
 * reliably. */
void bf6__skeleton_delete(void*);
void bf6__adjacency_delete(void*);
void bf6__hairbind_delete(void*);
void bf6__swarm_delete(void*);
void bf6__telemetry_delete(void*);
void bf6__spawns_delete(void*);
void bf6__vshapes_delete(void*);
void bf6__footprints_delete(void*);
void bf6__autopaint_delete(void*);
void bf6__ecs_system_delete(void*);
void bf6__debris_delete(void*);
void bf6__wind_delete(void*);
void bf6__weather_delete(void*);
void bf6__wavesel_delete(void*);
void bf6__behaviortree_delete(void*);
void bf6__netregistry_delete(void*);
void bf6__unlocks_delete(void*);
void bf6__gem_delete(void*);
void bf6__schematic_delete(void*);
void bf6__physics_delete(void*);
void bf6__occluder_delete(void*);
void bf6__pmvolume_delete(void*);
void bf6__lightprobe_delete(void*);
void bf6__renderbones_delete(void*);
void bf6__animreloc_delete(void*);
void bf6__animclip_delete(void*);
void bf6__psd_delete(void*);
void bf6__psdmap_delete(void*);

struct MaterialScope {
    bf6::Depot* depot = nullptr;
    const std::vector<uint8_t>* bytes = nullptr;
};

// Rendering and variation grouping must consult the identical bounded scope chain.
static std::vector<MaterialScope> material_scopes(bf6_ctx* c, const std::string& res_name,
    const std::string& placing, const std::string& fallback = {})
{
    std::vector<MaterialScope> scopes;
    auto add_scope = [&](const std::string& bundle) {
        if (bundle.empty()) return;
        const std::string depotName = c->src.depot_for_bundle(bundle);
        if (depotName.empty()) return;
        const std::vector<uint8_t>* bytes = nullptr;
        bf6::Depot* depot = c->depot_named(depotName, &bytes);
        if (!depot || !bytes) return;
        for (const MaterialScope& existing : scopes)
            if (existing.depot == depot) return;
        scopes.push_back({ depot, bytes });
    };
    add_scope(placing);
    // A nested front-end content bundle is a material DELTA, not a closed
    // world.  Its unresolved section keys continue in the depots owned by its
    // graph ancestors.  `depot_for_bundle` deliberately returns the nearest
    // depot, so collect the remaining exact ancestor owners here and merge
    // them below with first-wins semantics.  Never search siblings: a state
    // key is only unique inside one scope chain.
    //
    // Explicit layered armory reads already provide their ordered base scope;
    // inserting an inferred ancestor between the override and that base could
    // mask a legitimate base slot, so this continuation is only for the
    // ordinary one-scope placement path.
    if (!placing.empty() && fallback.empty())
    {
        std::string parent = placing;
        std::replace(parent.begin(), parent.end(), '\\', '/');
        std::transform(parent.begin(), parent.end(), parent.begin(),
            [](unsigned char ch) { return (char)std::tolower(ch); });
        if (parent.rfind("win32/", 0) == 0) parent.erase(0, 6);
        const auto& owners = c->src.depots_by_bundle();
        for (;;)
        {
            const size_t slash = parent.find_last_of('/');
            if (slash == std::string::npos) break;
            parent.resize(slash);
            const size_t parentSlash = parent.find_last_of('/');
            const std::string repeated = parent + "/" +
                (parentSlash == std::string::npos
                    ? parent : parent.substr(parentSlash + 1));
            if (owners.find(repeated) != owners.end()) add_scope(repeated);
            if (owners.find(parent) != owners.end()) add_scope(parent);
        }
    }
    add_scope(fallback);
    // Placement depots can contain only a material delta (or unrelated
    // records). Complete it from this mesh's own authored scope, preserving
    // placement-first precedence. Never search arbitrary sibling depots.
    {
        const std::string& twin = c->src.bundle_of_ebx(res_name);
        if (!twin.empty()) add_scope(twin);
    }
    if (scopes.empty())
    {
        const std::string depotName = c->src.depot_for_res(res_name);
        if (!depotName.empty())
        {
            const std::vector<uint8_t>* bytes = nullptr;
            bf6::Depot* depot = c->depot_named(depotName, &bytes);
            if (depot && bytes) scopes.push_back({ depot, bytes });
        }
    }

    return scopes;
}

struct MeshHandle {
    bf6_mesh                            mesh{};
    std::vector<bf6_section>            sections;
    std::vector<bf6_material_desc>      materials;
    std::vector<std::vector<bf6_tex_binding>> bindings;   // one array per material
    std::vector<std::vector<bf6_shader_tex_binding>> shader_bindings;
    std::vector<std::vector<float>>     pos, nrm, uv;
    // TexCoord1, kept alongside the primary. Its own array because the
    // primary can be REPOINTED later (car paint) and the secondary must not
    // follow it.
    std::vector<std::vector<float>>     uv1;
    // Per-vertex bone/part indices, one array per section. Same story as uv1:
    // meshset decodes them into MeshGeomSection::parts and the ABI dropped
    // them one line before any consumer could see them.
    std::vector<std::vector<uint16_t>>  bones;
    // The full skin binding, one array per section: 4 or 8 influences per
    // vertex with matching weights. `bones` above keeps only a single lane and
    // cannot express a vertex that belongs to two joints, which is every vertex
    // near a character's elbow.
    std::vector<std::vector<uint16_t>>  skin_b;
    std::vector<std::vector<float>>     skin_w;
    std::vector<int>                    skin_n;
    std::vector<std::vector<uint32_t>>  idx;
    std::vector<std::vector<uint32_t>>  colours;
    struct Surface {
        bf6_surface_desc desc{};
        std::vector<float> uv[5], color0, tangents;
        std::vector<uint8_t> lanes;
    };
    std::vector<Surface> surfaces;
    std::vector<uint16_t>               palette;   // mesh-wide bone palette
};

static void fill_surface_profile(bf6_ctx* c, const bf6::MaterialBinding& mb,
    const std::vector<bf6_shader_tex_binding>& textures, const bf6_material_desc& generic, bf6_surface_desc& out)
{
    bool has_albedo = false;
    for (int i = 0; i < generic.texture_count; ++i)
        has_albedo |= generic.textures[i].slot == BF6_TEX_ALBEDO && generic.textures[i].texture >= 0;
    // Roof materials share these utility constants with the procedural facade.
    // A usable ordinary albedo keeps them on their authored textured path.
    const bool whitebox = !has_albedo && mb.constants.count(0x3C4777D3) && mb.constants.count(0xCF3B7A2F) &&
        mb.constants.count(0x27EBB6BB) && mb.constants.count(0x2077DB06);
    const bool backdrop = mb.textures.count(0x31EBABA9) && mb.textures.count(0xA141CF1C) &&
        mb.textures.count(0x62DFB21A) && mb.textures.count(0xC8C9370A) && mb.constants.count(0xECABAAD6);
    out.profile = backdrop ? 2 : whitebox ? 1 : 0;
    if (!out.profile) return;
    const uint32_t slots[2][4] = {{0x5DEB688E,0x416B0E23,0xBA5B7844,0},
        {0x31EBABA9,0xA141CF1C,0x62DFB21A,0xC8C9370A}};
    bool ready = true;
    for (int i=0;i<(backdrop?4:3);++i) {
        out.textures[i] = -1;
        for (const auto& t : textures) if(t.name32==slots[out.profile-1][i]) out.textures[i]=t.texture;
        ready &= out.textures[i]>=0;
    }
    auto found = c->surface_schemas.find(out.profile);
    if (found == c->surface_schemas.end() || found->second.empty()) {
        std::vector<bf6::TerrainShaderBindingRecord> rows;
        std::string err;
        // Identified program ids, not database-local slot numbers. Require
        // the installed program and its current binding schema to exist.
        bf6::load_raster_bindings(c->src, backdrop ? 0xE0AF1530A069027Full : 0xAF450EB8DC72BE15ull, rows, err);
        // A subsequent map mount can make a previously absent program available.
        c->surface_schemas[out.profile] = std::move(rows);
        found = c->surface_schemas.find(out.profile);
    }
    bool material = false;
    const uint32_t minimum = backdrop ? 196 : 24, maximum = backdrop ? 208 : 32;
    for (const auto& row : found->second) {
        if (row.destination_span < minimum || row.destination_span > maximum) continue;
        float candidate[52]{};
        bool complete = !row.declarations.empty();
        int copied = 0;
        for (const auto& d : row.declarations) {
            const auto value = mb.constants.find(d.name32());
            if(value==mb.constants.end() || value->second.empty() ||
                d.destination + value->second.size() > row.destination_span) {complete=false;break;}
            std::memcpy(reinterpret_cast<uint8_t*>(candidate)+d.destination,
                value->second.data(),value->second.size());
            ++copied;
        }
        if (complete && copied) { std::memcpy(out.material,candidate,sizeof(candidate)); material=true; break; }
    }
    out.complete = ready && material;
}

extern "C" {

int bf6_abi_version(void) { return BF6_ABI_VERSION; }

bf6_ctx* bf6_open(const char* game_dir, char* err, int err_len) {
    // Empty game_dir is the explicit no-install mode: a context with nothing
    // mounted. Mesh reads will fail, but the placeable catalogue (which only
    // needs bf6_load_placeables' SDK data) works fully.
    if (!game_dir || !*game_dir) {
        return new bf6_ctx();
    }
    bf6_ctx* c = new bf6_ctx();
    std::string e;
    if (!c->src.open(game_dir, e)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", e.c_str());
        delete c;
        return nullptr;
    }
    // Mount the shared SuperBundle TOCs at Data/Win32/*.toc. This is the fast,
    // level-independent catalogue; per-level mounting lands with find_tocs later.
    std::string dir = std::string(game_dir) + "/Data/Win32";
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (const auto& f : fs::directory_iterator(dir, ec)) {
            if (f.path().extension() == ".toc") {
                std::string me;
                c->src.mount_toc(f.path().string(), me);
            }
        }
    }
    if (c->src.res_count() == 0) {
        if (err && err_len > 0)
            std::snprintf(err, (size_t)err_len, "opened the install but mounted no resources");
        delete c;
        return nullptr;
    }
    return c;
}

void bf6__uisound_forget(bf6_ctx*);
void bf6_close(bf6_ctx* c) { bf6__uisound_forget(c); delete c; }

int bf6_was_lifted(bf6_ctx* c) { return c ? c->lifted : 0; }

int bf6_load_placeables(bf6_ctx* c, const char* fbexport_dir, char* err, int err_len) {
    if (!c || !fbexport_dir) return 0;
    std::string e;
    if (!c->pdb.load(fbexport_dir, e)) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", e.c_str());
        return 0;
    }
    return (int)c->pdb.items().size();
}

int bf6_level_count(bf6_ctx* c) { return c ? (int)c->pdb.levels().size() : 0; }

const char* bf6_level_name(bf6_ctx* c, int index) {
    if (!c) return "";
    const auto& lv = c->pdb.levels();
    if (index < 0 || index >= (int)lv.size()) return "";
    return lv[index].c_str();
}

// Case-insensitive substring test (search is expected lowercase-friendly enough;
// we lower both sides so "dumbo" matches "Dumbo").
static bool ci_contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto lower = [](char c){ return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        size_t j = 0;
        for (; j < needle.size(); j++)
            if (lower(hay[i + j]) != lower(needle[j])) break;
        if (j == needle.size()) return true;
    }
    return false;
}

int bf6_list_placeables(bf6_ctx* c, const char* level, const char* search,
                        bf6_placeable* out, int out_max) {
    if (!c) return 0;
    std::string lvl = level ? level : "";
    std::string q   = search ? search : "";
    int total = 0, written = 0;
    for (const auto& p : c->pdb.items()) {
        if (!bf6::PlaceableDB::allowed_on(p, lvl)) continue;
        if (!q.empty() && !ci_contains(p.type, q)) continue;
        if (out && written < out_max) {
            out[written].type         = p.type.c_str();
            out[written].directory    = p.directory.c_str();
            out[written].mesh         = p.mesh.c_str();
            out[written].physics_cost = p.physics_cost;
            out[written].universal    = p.universal ? 1 : 0;
            written++;
        }
        total++;
    }
    return total;
}

int bf6_placeable_props(bf6_ctx* c, const char* type, bf6_prop* out, int out_max) {
    if (!c || !type) return 0;
    const std::string t = type;
    for (const auto& p : c->pdb.items()) {
        if (p.type != t) continue;
        const int total = (int)p.props.size();
        for (int i = 0; i < total && out && i < out_max; i++) {
            out[i].name = p.props[i].name.c_str();
            out[i].type = p.props[i].type.c_str();
            out[i].def  = p.props[i].def.c_str();
            out[i].selections = p.props[i].selections.c_str();
        }
        return total;
    }
    return 0;
}

// Count matches; write up to out_max. Returns the TOTAL match count, so a caller
// passing out_max=0 learns the catalogue size. res_name points into the ctx.
int bf6_catalogue(bf6_ctx* c, const char* search, bf6_cat_entry* out, int out_max) {
    if (!c) return 0;
    std::string q = search ? search : "";
    int total = 0, written = 0;
    for (const auto& kv : c->src.res()) {
        if (!q.empty() && kv.first.find(q) == std::string::npos) continue;
        if (written < out_max && out) {
            out[written].res_name = kv.first.c_str();
            out[written].category = "";
            written++;
        }
        total++;
    }
    return total;
}

static bf6_mesh* bf6__read_mesh_scoped(bf6_ctx* c, const char* res_name, int lod,
                                      const char* placing_bundle,
                                      const char* fallback_bundle,
                                      const char* variation,
                                      bool armory_index) {
    if (!c || !res_name) return nullptr;
    const std::string placing = placing_bundle ? placing_bundle : "";
    const std::string fallback = fallback_bundle ? fallback_bundle : "";
    const std::string variant = variation ? variation : "";
    std::string err;
    std::vector<uint8_t> d = c->src.get_res(res_name, err);
    if (d.empty()) return nullptr;
    bf6::MeshSet ms = bf6::meshset_parse(d.data(), d.size(), err);
    if (!ms.ok || ms.lods.empty()) return nullptr;
    if (lod < 0 || lod >= (int)ms.lods.size()) lod = 0;

    // The vertex/index bytes live in the LOD's chunk (try both guid spellings).
    static const char* H = "0123456789abcdef";
    const auto& cid = ms.lods[lod].chunk_id;
    std::string fwd, rev;
    for (int i = 0; i < 16; i++) { fwd += H[cid[i] >> 4]; fwd += H[cid[i] & 0xF]; }
    for (int i = 15; i >= 0; i--) { rev += H[cid[i] >> 4]; rev += H[cid[i] & 0xF]; }
    std::vector<uint8_t> chunk = c->src.get_chunk(fwd, err);
    if (chunk.empty()) chunk = c->src.get_chunk(rev, err);

    // NO CHUNK IS NOT ALWAYS A MISSING CHUNK. A LOD whose ChunkId is all zeros
    // stores its geometry INSIDE the MeshSet, and returning null for those loses
    // every gadget and projectile that ships that way - 9 of the 10,571 mesh
    // resources in one level's mount, all under common/hardware. See
    // meshset_inline_lod for the base recovery and its measurement.
    const uint8_t* geom = chunk.data();
    size_t geom_len = chunk.size();
    if (chunk.empty() &&
        !bf6::meshset_inline_lod(ms, lod, d.data(), d.size(), &geom, &geom_len))
        return nullptr;

    auto secs = bf6::meshset_read_lod(ms, lod, geom, geom_len, err);
    if (secs.empty()) return nullptr;

    // ---- DROP THE PARTS THE GAME HIDES AT SPAWN ---------------------------
    //
    // A destructible prop carries its own damaged state inside its intact
    // mesh - the cracked windscreen, the crushed panel, the deflated tyre -
    // tagged per vertex and hidden until the piece breaks. Left in, every
    // parked car is drawn with its own wreck interpenetrating it, which is not
    // subtle: it reads as bodywork that is stretched, doubled and corrupt, and
    // it looks like a geometry bug rather than a missing filter.
    //
    // Skinned meshes are exempt: there the same per-vertex element is a
    // SKELETON BONE, a different and differently sized index space, and
    // indexing the part table with a bone id would cull arbitrary pieces of
    // every aircraft.
    // The part table lives in the prop EBX, so this needs the type schema.
    // A context opened without one can still read geometry, and silently
    // skipping the filter is better than refusing the mesh.
    //
    // THE PART INDEX IS CHECKED FIRST, and that ordering is the whole cost of
    // this feature. Asking the question needs the prop's EBX partition read and
    // parsed, and a map places about 1,450 distinct assets - so doing it for
    // every one of them would put seconds onto a load that is already the thing
    // people complain about. A mesh with no per-vertex part index cannot be
    // filtered whatever its table says, and that test is free: the index was
    // already decoded with the geometry.
    // Every section's state key by material name, taken BEFORE the shadow
    // filter below: a visual section's variation can resolve through its
    // _ZOnly twin's key (reference: _candidate_keys, SHADERS.md 5.2), and the
    // twin is exactly what the filter is about to remove.
    std::map<std::string, uint64_t> twin_keys;
    // Read keys from the cheap section headers, including skipped pass twins.
    // Their vertex buffers do not need to be decoded to resolve materials.
    for (const auto& g : ms.lods[lod].sections) twin_keys.emplace(g.material, g.state_key);

    // Pass membership is authoritative. A backdrop's visible opaque material
    // can be named M_Shadow; discarding it by name removes the whole building.
    // Suppress dedicated depth/shadow twins only when no visual pass uses them.
    {
        auto shadow_only = [](const bf6::MeshGeomSection& g)
        {
            return !(g.category_flags & 7) && (g.category_flags & 24);
        };
        secs.erase(std::remove_if(secs.begin(), secs.end(), shadow_only), secs.end());
        if (secs.empty()) return nullptr;
    }

    bool has_parts = false;
    for (const auto& g : secs)
        if (!g.parts.empty() && g.parts.size() == g.positions.size() / 3) { has_parts = true; break; }

    if (has_parts && ms.mesh_type != 1 && c->types) {
        const std::set<uint16_t> hidden =
            bf6::destruction_hidden_parts(c->src, *c->types, res_name);
        if (!hidden.empty()) {
            for (auto& g : secs) {
                if (g.parts.size() != g.positions.size() / 3) continue;
                std::vector<uint32_t> keep;
                keep.reserve(g.indices.size());
                // PER TRIANGLE, ON ITS FIRST VERTEX. A triangle spans one part
                // in practice, and requiring all three to be visible would also
                // drop the seam triangles between a hidden part and a visible
                // one - which punches a hole in the intact body instead of
                // removing an overlay.
                for (size_t k = 0; k + 2 < g.indices.size(); k += 3) {
                    const uint32_t v0 = g.indices[k];
                    if (v0 < g.parts.size() && hidden.count(g.parts[v0])) continue;
                    keep.push_back(g.indices[k]);
                    keep.push_back(g.indices[k + 1]);
                    keep.push_back(g.indices[k + 2]);
                }
                // The VERTICES are left alone. Only the triangles referencing
                // them are gone, so nothing has to be renumbered - a hidden
                // part's vertices simply become unreferenced.
                g.indices.swap(keep);
            }
            // A section can lose every triangle it had; an empty one would
            // become a zero-index draw downstream.
            secs.erase(std::remove_if(secs.begin(), secs.end(),
                       [](const bf6::MeshGeomSection& g) { return g.indices.empty(); }),
                       secs.end());
            if (secs.empty()) return nullptr;
        }
    }

    MeshHandle* mh = new MeshHandle();
    mh->palette = ms.bone_parts;
    const size_t n = secs.size();
    mh->pos.reserve(n); mh->nrm.reserve(n); mh->uv.reserve(n); mh->idx.reserve(n);
    mh->uv1.reserve(n);
    mh->bones.reserve(n);
    mh->skin_b.reserve(n); mh->skin_w.reserve(n); mh->skin_n.reserve(n);
    for (auto& s : secs) {
        mh->pos.push_back(std::move(s.positions));
        mh->nrm.push_back(std::move(s.normals));
        mh->uv.push_back(std::move(s.uv0));
        // COPIED, not moved: the car-paint override below still reads
        // s.uv[ch], and moving channel 1 out would hand it an empty vector
        // for ch == 1. A weapon mesh's second channel is ~8 bytes a vertex.
        mh->uv1.push_back(s.uv[1]);
        mh->bones.push_back(std::move(s.parts));
        mh->skin_b.push_back(std::move(s.skin_bones));
        mh->skin_w.push_back(std::move(s.skin_weights));
        mh->skin_n.push_back(s.influences);
        mh->idx.push_back(std::move(s.indices));
    }
    mh->sections.resize(n);
    mh->colours.resize(n);
    mh->surfaces.resize(n);
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < n; i++) {
        bf6_section& sec = mh->sections[i];
        sec = bf6_section{};
        sec.positions    = mh->pos[i].data();
        sec.vertex_count = (int32_t)(mh->pos[i].size() / 3);
        sec.normals      = mh->nrm[i].empty() ? nullptr : mh->nrm[i].data();
        sec.uv0          = mh->uv[i].empty()  ? nullptr : mh->uv[i].data();
        sec.uv1          = mh->uv1[i].empty() ? nullptr : mh->uv1[i].data();
        auto& surface = mh->surfaces[i];
        surface.desc.struct_size = sizeof(bf6_surface_desc);
        std::fill(std::begin(surface.desc.textures),std::end(surface.desc.textures),-1);
        for(int ch=0;ch<5;++ch) {
            surface.uv[ch] = secs[i].uv[ch];
            surface.desc.uv[ch] = surface.uv[ch].empty() ? nullptr : surface.uv[ch].data();
        }
        surface.color0 = std::move(secs[i].color0);
        surface.lanes = secs[i].layer_lanes;
        surface.tangents = std::move(secs[i].tangent_sign);
        surface.desc.color0 = surface.color0.empty() ? nullptr : surface.color0.data();
        surface.desc.submaterial = surface.lanes.empty() ? nullptr : surface.lanes.data();
        sec.tangents = surface.tangents.empty() ? nullptr : surface.tangents.data();
        sec.bones        = mh->bones[i].empty() ? nullptr : mh->bones[i].data();
        // The palette is not decoded yet - see meshset.cpp. Reported as absent
        // rather than as an empty-but-present list, so a consumer can tell the
        // difference between "no bones" and "bones we cannot resolve".
        // ONE PALETTE PER MESH, not per section: the bone/part block lives in
        // the MeshSet header, so every section is handed the same list rather
        // than a private copy. Reported as absent when the mesh declares none.
        sec.bone_list       = mh->palette.empty() ? nullptr : mh->palette.data();
        sec.bone_list_count = (int32_t)mh->palette.size();
        sec.skin_bones      = mh->skin_b[i].empty() ? nullptr : mh->skin_b[i].data();
        sec.skin_weights    = mh->skin_w[i].empty() ? nullptr : mh->skin_w[i].data();
        sec.skin_influences = mh->skin_b[i].empty() ? 0 : mh->skin_n[i];
        sec.is_decal        = (secs[i].category_flags & (1u << 2)) ? 1 : 0;
        sec.state_key       = secs[i].state_key;
        sec.indices      = mh->idx[i].data();
        sec.index_count  = (int32_t)mh->idx[i].size();
        sec.material     = (int32_t)i;
        for (size_t v = 0; v + 2 < mh->pos[i].size(); v += 3)
            for (int k = 0; k < 3; k++) {
                float x = mh->pos[i][v + k];
                if (x < lo[k]) lo[k] = x;
                if (x > hi[k]) hi[k] = x;
            }
    }
    mh->mesh.sections       = mh->sections.data();
    mh->mesh.section_count  = (int32_t)n;
    mh->mesh.mesh_type      = (int32_t)ms.mesh_type;
    mh->mesh.bone_count     = (int32_t)ms.bone_count;
    mh->mesh.lod_count      = (int32_t)ms.lod_count;

    // ---- materials, one per section --------------------------------------
    //
    // The join: the section's state key, looked up in the depot co-located with
    // the bundle that PLACED this mesh. The placing bundle is the exact rule,
    // and a caller who knows it passes it in; reading a mesh on its own - a
    // browser, a preview - has no placement above it, so the bundle the
    // resource lives in is the fallback. Widening on a miss goes to ancestors
    // only, never to a sibling: a key is unique within a scope, so a sibling
    // holding it binds a material that merely COLLIDES, which looks fine and is
    // wrong.
    // WITH NO PLACEMENT ABOVE IT, THE MESH'S OWN EBX TWIN NAMES THE SCOPE.
    //
    // depot_for_res asks the bundle the mesh RESOURCE shipped in, which is an
    // art bundle. A weapon part's material does not live there: it lives in a
    // per-part shader bundle, dpf_<weapon>_<part>_<hash>_bundle_1p. A state
    // key is unique only within a bundle, so asking the wrong one resolves
    // nothing and the part draws BLACK - measured on m4a1, where the magazine
    // and both side panels bound 0 textures this way and bind 2 and 3 when
    // scoped correctly.
    //
    // Every mesh resource ships an .ebx twin under the same name, and
    // bundle_of_ebx is the documented rule for materials. So try that first
    // and keep the resource's own bundle as the fallback, which is right for
    // meshes that genuinely carry their material beside them.
    const auto scopes = material_scopes(c, res_name, placing, fallback);

    mh->materials.resize((size_t)n);
    mh->bindings.resize((size_t)n);
    mh->shader_bindings.resize((size_t)n);
    for (size_t i = 0; i < (size_t)n; i++) {
        bf6_material_desc& md = mh->materials[i];
        md.base_color[0] = md.base_color[1] = md.base_color[2] = md.base_color[3] = 1.f;
        md.emissive[0] = md.emissive[1] = md.emissive[2] = 0.f;
        md.roughness = 0.5f;
        md.metallic  = 0.f;
        md.two_sided = 0;
        md.alpha_test = 0;
        md.translucent = 0;
        md.alpha_from_albedo = 0;
        md.normal_is_nsm = 0;
        md.terrain_decal_receiver = 0;
        // Absent is reported as count 0 with the NEUTRAL in the slots, never
        // as zeros: a consumer that multiplied the framebuffer by an
        // unauthored tint would black out the surface instead of leaving it
        // clear, and the two cases are indistinguishable from the values alone.
        md.glass_tint_count = 0;
        for (int gi = 0; gi < 8; gi++) {
            md.glass_tint_palette[gi * 4 + 0] = 0.79138f;
            md.glass_tint_palette[gi * 4 + 1] = 0.85638f;
            md.glass_tint_palette[gi * 4 + 2] = 0.89001f;
            md.glass_tint_palette[gi * 4 + 3] = 1.f;
        }
        for (int gi = 0; gi < 4; gi++)
            md.glass_tint[gi] = md.glass_tint_palette[gi];
        md.textures = nullptr;
        md.texture_count = 0;
        md.shader_textures = nullptr;
        md.shader_texture_count = 0;
        if (scopes.empty()) continue;

        // THE CANDIDATE KEYS, in the reference's order (SHADERS.md 5.2):
        // the section's own variation key, the _ZOnly twin's variation key,
        // then the base key - and the records MERGED, first-wins per slot,
        // never taken first-hit. A variant record often carries only the
        // DELTA - a livery overlay, a colour table - while the base record
        // carries the texture set, so stopping at the first resolving record
        // loses whichever half it did not hold. First-hit is exactly why
        // liveries and paints never arrived.
        uint64_t cands[3];
        int nc = 0;
        const uint64_t base_key = secs[i].state_key;
        if (!variant.empty()) {
            cands[nc++] = variation_key(base_key, variant);
            auto tw = twin_keys.find(secs[i].material + "_ZOnly");
            if (tw != twin_keys.end() && tw->second != base_key)
                cands[nc++] = variation_key(tw->second, variant);
        }
        cands[nc++] = base_key;

        bf6::MaterialBinding mb;
        // Scope priority is stronger than candidate priority: every authored
        // override/delta is consumed before the exact DPF base fills missing
        // slots. Reversing these loops lets a base candidate mask a skin delta.
        for (const MaterialScope& scope : scopes) {
            for (int ci = 0; ci < nc; ci++) {
                bool dup = false;
                for (int cj = 0; cj < ci; cj++) if (cands[cj] == cands[ci]) dup = true;
                if (dup) continue;
                bf6::MaterialBinding one =
                    scope.depot->textures_for(cands[ci], *scope.bytes);
                if (!one.valid) continue;
                mb.valid = true;
                // emplace, not assignment: higher-priority scope/candidate wins.
                for (const auto& kv : one.textures)
                    mb.textures.emplace(kv.first, kv.second);
                for (const auto& kv : one.constants)
                    mb.constants.emplace(kv.first, kv.second);
            }
        }
        if (!mb.valid) continue;

        // ---- what KIND of surface this is, from the record ----------------
        //
        // THE SHADER'S OWN SWITCH, checked before anything about the mask's
        // content. Measured over every record carrying it on one level: 182 of
        // 193 with a real cutout sheet set it, and all 11 that did not were
        // foreign-filler pairings - a prop with no cutout of its own handed
        // another prop's sheet, which the game never samples. Cutting by such a
        // filler shreds the surface: a van's silhouette punched through a
        // wheel's UVs and read as "the tyres look damaged".
        {
            auto g = mb.constants.find(0x77D10576);
            if (g != mb.constants.end() && !g->second.empty() && g->second[0] != 0)
                md.alpha_test = 1;
        }
        // VEGETATION CUTS OUT FROM THE PAIRED "_a" SHEET, NOT FROM "_cu" ALPHA.
        //
        // The spec tables annotate the veg base colour as carrying coverage in
        // its alpha. It does not, and this was implemented that way here first:
        // measured over 708 vegetation shader states on one level, the "_cu"
        // alpha is near-CONSTANT per sheet and the constant differs between
        // sheets - 128 on one, 254..255 on another, 0..1 on an agave. A channel
        // uniformly ~0 on one plant and uniformly ~1 on another is not carrying
        // coverage for either, and masking by it gives blobs.
        //
        // The mask is the R channel of the "_a" texture in 0xD405B0E1, which is
        // the same slot everything else cuts out with. The reason the wrong
        // reading is easy to hold: a one-channel BC4 decompresses to R with
        // A = 1, so a consumer that reads the MASK's alpha sees a constant 1,
        // concludes there is no coverage there, and goes looking in the base
        // colour. That constant is the decoder's invention, not the asset's.
        if (mb.textures.count(0xD405B0E1)) {
            // The shader's own gate still wins when it is present; absent, a
            // real mask being bound is what decides.
            auto g = mb.constants.find(0x77D10576);
            const bool gated = g != mb.constants.end() && !g->second.empty();
            if (!gated) md.alpha_test = 1;
        }

        // Glass, data-driven: the destruction glass volume slot bound, or the
        // glass tint palette present. Never by name.
        if (mb.textures.count(0xBB245590) || mb.constants.count(0xA0106346))
            md.translucent = 1;

        // THE PALETTE'S PAYLOAD, not just its presence.
        //
        // 124 bytes: eight float4 entries whose LAST w is elided, which is the
        // whole reason the buffer is not 128. Measured over every material
        // record reachable on mp_badlands under "vehicle": 62 carriers, all
        // 124 bytes, 13 distinct payloads, and 1,484 records in the same
        // depots with the constant absent - so an absence here is measured
        // rather than a lookup that failed.
        //
        // Entry 0 is the material's own tint; unused slots carry the neutral.
        // Read as per-channel transmission: M_Glass_Red authors
        // (1.0, 0.0017, 0.0), and the AH-64E's M_Glass_Green authors red in
        // slot 0 and green in slot 1 - a navigation-lamp palette, which is
        // what makes this a palette and not a colour.
        {
            auto gt = mb.constants.find(0xA0106346);
            if (gt != mb.constants.end() && gt->second.size() >= 124)
            {
                const std::vector<uint8_t>& gb = gt->second;
                for (int gi = 0; gi < 8; gi++)
                {
                    for (int gc = 0; gc < 3; gc++)
                    {
                        const size_t off = (size_t)gi * 16 + (size_t)gc * 4;
                        if (off + 4 > gb.size()) break;
                        std::memcpy(&md.glass_tint_palette[gi * 4 + gc],
                                    gb.data() + off, 4);
                    }
                    md.glass_tint_palette[gi * 4 + 3] = 1.f;
                }
                for (int gi = 0; gi < 4; gi++)
                    md.glass_tint[gi] = md.glass_tint_palette[gi];
                md.glass_tint_count = 8;
            }
        }

        // THE PRIMARY UV, now that the record is in hand. Car paint is the only
        // family that moves, and only the depot can say where to.
        {
            const int ch = primary_uv_channel(secs[i].material, mb);
            if (ch != 0 && !secs[i].uv[ch].empty()) {
                secs[i].uv0 = secs[i].uv[ch];
                // The section's uv0 pointer was taken before this, so it has to
                // be repointed or the change never reaches the caller.
                mh->uv[i] = secs[i].uv0;
                mh->sections[i].uv0 = mh->uv[i].data();
            }
        }

        resolve_colour(mb, md);
        mh->colours[i] = resolve_layer_colours(mb, secs[i], md);
        mh->sections[i].colors = mh->colours[i].empty() ? nullptr : mh->colours[i].data();
        // A PLACEHOLDER IS NOT AN ALBEDO, AND THE CHAIN HAS TO KNOW THAT WHILE
        // IT IS WALKING. The same guard is applied again in the binding loop
        // below - that one stops a placeholder reaching the caller, this one
        // stops the chain giving up on a record that has a real sheet further
        // down its own fallback list.
        auto slot_is_usable = [&](uint32_t n32) {
            auto t = mb.textures.find(n32);
            if (t == mb.textures.end()) return false;
            const auto& pidx = armory_index ? c->src.armory_partition_index()
                                             : c->src.partition_index();
            auto ait = pidx.find(t->second);
            if (ait == pidx.end()) return false;
            const std::string& nm = ait->second;
            return nm.find("/textures/default/") == std::string::npos &&
                   nm.find("/textures/debug/") == std::string::npos;
        };
        const uint32_t albedo_slot = albedo_slot_of(mb, slot_is_usable);
        const bool wrap_albedo = albedo_slot == 0x54BBCD22;
        // The verified terrain-decal receiver set: a surface sampling the
        // terrain colour VT and carrying no real albedo of its own. Preserve
        // this identity before generic slot mapping discards the authored
        // parameter name.
        md.terrain_decal_receiver =
            mb.textures.count(0x89D3AD5E) && albedo_slot == 0 ? 1 : 0;

        // AND AN IMPOSTOR IS NOT ALPHA TESTED, however binary its sheet looks.
        //
        // These atlases are ~100% binary-shaped in alpha, which invites a
        // cutout rule. The recovered pixel shader for Aftermath M_Vista reads
        // RGB only and contains no discard: the alpha is packing metadata, not
        // coverage. Inventing a test from the shape removed most of a valid
        // 3,636-triangle shell and read as "the LOD is corrupt".
        if (wrap_albedo) md.alpha_test = 0;

        for (const auto& kv : mb.textures) {
            const uint32_t n32 = kv.first;
            // The FILE guid resolves through the partition index to the texture
            // asset's name; the resource is that name without the .ebx.
            const auto& gi = armory_index ? c->src.armory_partition_index()
                                           : c->src.partition_index();
            auto ait = gi.find(kv.second);
            if (ait == gi.end()) continue;
            std::string tres = ait->second;
            if (tres.size() > 4 && tres.compare(tres.size() - 4, 4, ".ebx") == 0)
                tres.resize(tres.size() - 4);

            const int textureId = c->texture_id(tres);
            bf6_shader_tex_binding raw;
            raw.name32 = n32;
            raw.texture = textureId;
            mh->shader_bindings[i].push_back(raw);

            if (is_detail_layer(n32)) continue;
            bool known = false;
            const bf6_tex_slot slot = slot_for(n32, known, albedo_slot);
            if (!known) continue;

            // A PLACEHOLDER IS NOT AN ALBEDO. Plenty of records fill a slot
            // they do not use with common/shaders/textures/default or /debug,
            // and binding one as a base colour paints the prop a flat
            // stand-in - which looks like a texture that loaded, so nobody
            // goes looking for a lookup bug. Only the albedo is guarded:
            // elsewhere a default IS the intended neutral.
            if (slot == BF6_TEX_ALBEDO &&
                (tres.find("/textures/default/") != std::string::npos ||
                 tres.find("/textures/debug/") != std::string::npos))
                continue;

            bf6_tex_binding b;
            b.slot = slot;
            b.texture = textureId;
            mh->bindings[i].push_back(b);

            // The vista "_nsm" is not an ordinary normal map: RG normal,
            // B wetness, A smoothness. The consumer has to unpack it
            // differently, so the record says so - keyed on the slot hash,
            // which is exact, not on the asset's name.
            if (n32 == 0xEC35AA10 && slot == BF6_TEX_NORMAL)
                md.normal_is_nsm = 1;
        }
        md.textures = mh->bindings[i].empty() ? nullptr : mh->bindings[i].data();
        md.texture_count = (int32_t)mh->bindings[i].size();
        md.shader_textures = mh->shader_bindings[i].empty()
            ? nullptr : mh->shader_bindings[i].data();
        md.shader_texture_count = (int32_t)mh->shader_bindings[i].size();
        fill_surface_profile(c,mb,mh->shader_bindings[i],md,mh->surfaces[i].desc);
    }
    mh->mesh.materials      = mh->materials.data();
    mh->mesh.material_count = (int32_t)n;
    for (int k = 0; k < 3; k++) { mh->mesh.aabb_min[k] = lo[k]; mh->mesh.aabb_max[k] = hi[k]; }
    c->handles[&mh->mesh] = bf6_ctx::HK_MESH;
    return &mh->mesh;
}
int bf6_mesh_surface(bf6_ctx* c, const bf6_mesh* mesh, int section, bf6_surface_desc* out)
{
    if(!c || !mesh || !out || out->struct_size!=sizeof(bf6_surface_desc)) return 0;
    const auto owned=c->handles.find(const_cast<bf6_mesh*>(mesh));
    if(owned==c->handles.end() || owned->second!=bf6_ctx::HK_MESH) return 0;
    const auto* handle=reinterpret_cast<const MeshHandle*>(mesh);
    if(section<0 || (size_t)section>=handle->surfaces.size()) return 0;
    *out=handle->surfaces[(size_t)section].desc;
    return 1;
}
bf6_mesh* bf6_read_mesh_scoped(bf6_ctx* c, const char* res_name, int lod,
                               const char* placing_bundle, const char* variation) {
    return bf6__read_mesh_scoped(c, res_name, lod, placing_bundle, nullptr,
                                 variation, false);
}
bf6_mesh* bf6_read_armory_mesh_scoped(bf6_ctx* c, const char* res_name, int lod,
                                      const char* placing_bundle, const char* variation) {
    return bf6__read_mesh_scoped(c, res_name, lod, placing_bundle, nullptr,
                                 variation, true);
}
bf6_mesh* bf6_read_armory_mesh_layered(bf6_ctx* c, const char* res_name, int lod,
                                       const char* base_bundle,
                                       const char* override_bundle,
                                       const char* variation) {
    return bf6__read_mesh_scoped(c, res_name, lod, override_bundle, base_bundle,
                                 variation, true);
}
int bf6_material_scope_exists(bf6_ctx* c, const char* bundle) {
    if (!c || !bundle || !*bundle) return 0;
    return c->src.depot_for_bundle(bundle).empty() ? 0 : 1;
}
int bf6_armory_material_scope_for_mesh(bf6_ctx* c, const char* res_name,
                                       const char* weapon_family,
                                       char* out, int out_len) {
    if (!c || !res_name || !*res_name || !weapon_family || !*weapon_family ||
        !out || out_len <= 0) return 0;
    out[0] = 0;

    std::string err;
    const std::vector<uint8_t> bytes = c->src.get_res(res_name, err);
    if (bytes.empty()) return 0;
    const bf6::MeshSet mesh = bf6::meshset_parse(bytes.data(), bytes.size(), err);
    if (!mesh.ok || mesh.lods.empty()) return 0;

    std::vector<uint64_t> keys;
    for (const bf6::MeshSection& section : mesh.lods[0].sections)
        if (section.state_key &&
            std::find(keys.begin(), keys.end(), section.state_key) == keys.end())
            keys.push_back(section.state_key);
    if (keys.empty()) return 0;

    std::string family = weapon_family;
    std::transform(family.begin(), family.end(), family.begin(),
        [](unsigned char ch) { return (char)std::tolower(ch); });
    const std::string prefix = "dpf_" + family + "_";
    std::string best;
    size_t bestKeyHits = 0, bestTextureSlots = 0, bestConstantSlots = 0;
    for (const auto& owner : c->src.depots_by_bundle())
    {
        std::string bundle = owner.first;
        std::transform(bundle.begin(), bundle.end(), bundle.begin(),
            [](unsigned char ch) { return (char)std::tolower(ch); });
        if (bundle.rfind(prefix, 0) != 0 ||
            bundle.find("_bundle_1p") == std::string::npos)
            continue;
        const std::vector<uint8_t> depotBytes = c->src.get_res(owner.second, err);
        bf6::Depot depot;
        if (depotBytes.empty() || !depot.parse(depotBytes, err)) continue;
        size_t keyHits = 0, textureSlots = 0, constantSlots = 0;
        for (uint64_t key : keys)
        {
            if (!depot.has_key(key)) continue;
            const bf6::MaterialBinding binding = depot.textures_for(key, depotBytes);
            if (!binding.valid) continue;
            ++keyHits;
            textureSlots += binding.textures.size();
            constantSlots += binding.constants.size();
        }
        if (!keyHits) continue;
        if (keyHits > bestKeyHits ||
            (keyHits == bestKeyHits && textureSlots > bestTextureSlots) ||
            (keyHits == bestKeyHits && textureSlots == bestTextureSlots &&
             constantSlots > bestConstantSlots))
        {
            best = owner.first;
            bestKeyHits = keyHits;
            bestTextureSlots = textureSlots;
            bestConstantSlots = constantSlots;
        }
    }
    if (best.empty()) return 0;
    std::snprintf(out, (size_t)out_len, "%s", best.c_str());
    return (int)best.size();
}
bf6_mesh* bf6_read_mesh(bf6_ctx* c, const char* res_name, int lod) {
    return bf6_read_mesh_scoped(c, res_name, lod, nullptr, nullptr);
}

// Decoded on demand and kept: the same texture is bound by many materials, and
// a 4K BC7 sheet decoded once per binding would be minutes of nothing.
const bf6_texture* bf6_texture_at(bf6_ctx* c, int texture_id) {
    if (!c || texture_id < 0 || (size_t)texture_id >= c->textures.size()) return nullptr;
    bf6_ctx::TexHold& h = c->textures[(size_t)texture_id];
    if (!h.tried) {
        h.tried = true;
        std::string e;
        // NOTHING MAY THROW ACROSS THIS BOUNDARY. bf6_texture_at is extern "C"
        // and is called from another module, so an escaping C++ exception does
        // not unwind into the caller - it ends the host process with a stack
        // that stops at this dll. A texture decode allocates, and a mount that
        // has changed underneath a cached entry can produce anything, so this
        // is a real path rather than a theoretical one.
        try {
            std::vector<uint8_t> res = c->src.get_res(h.res, e);
            if (!res.empty()) {
                auto fetch = [c](const std::string& g) {
                    std::string e2;
                    return c->src.get_chunk(g, e2);
                };
                h.ok = bf6::Texture::decode(res, fetch, h.img, 0, e);
            }
        } catch (...) {
            h.ok = false;
        }
        if (h.ok) {
            h.abi.width     = h.img.width;
            h.abi.height    = h.img.height;
            h.abi.mip_count = h.img.mip_count;   // the chain from the chosen level down
            h.abi.format    = fmt_of(h.img.dxgi);
            h.abi.data      = h.img.blocks.data();
            h.abi.data_len  = (int32_t)h.img.blocks.size();
            h.abi.srgb      = h.img.srgb ? 1 : 0;
        }
    }
    return h.ok ? &h.abi : nullptr;
}

const bf6_texture* bf6_texture_at_max_dim(bf6_ctx* c, int texture_id, int max_dim) {
    if (!c || texture_id < 0 || (size_t)texture_id >= c->textures.size() || max_dim <= 0)
        return nullptr;
    const uint64_t key = ((uint64_t)(uint32_t)texture_id << 32) | (uint32_t)max_dim;
    bf6_ctx::CappedTexHold& h = c->capped_textures[key];
    if (!h.tried) {
        h.tried = true;
        std::string e;
        try {
            const std::string resName = c->textures[(size_t)texture_id].res;
            std::vector<uint8_t> res = c->src.get_res(resName, e);
            if (!res.empty()) {
                auto fetch = [c](const std::string& g) {
                    std::string e2;
                    return c->src.get_chunk(g, e2);
                };
                h.ok = bf6::Texture::decode_capped(res, fetch, h.img, max_dim, e);
            }
        } catch (...) { h.ok = false; }
        if (h.ok) {
            h.abi.width = h.img.width; h.abi.height = h.img.height;
            h.abi.mip_count = h.img.mip_count; h.abi.format = fmt_of(h.img.dxgi);
            h.abi.data = h.img.blocks.data(); h.abi.data_len = (int32_t)h.img.blocks.size();
            h.abi.srgb = h.img.srgb ? 1 : 0;
        }
    }
    return h.ok ? &h.abi : nullptr;
}

void bf6_release_texture_payload(bf6_ctx* c, int texture_id, int max_dim) {
    if (!c || texture_id < 0 || max_dim <= 0) return;
    const uint64_t key = ((uint64_t)(uint32_t)texture_id << 32) | (uint32_t)max_dim;
    c->capped_textures.erase(key);
}

const char* bf6_texture_name_at(bf6_ctx* c, int texture_id) {
    if (!c || texture_id < 0 || (size_t)texture_id >= c->textures.size()) return nullptr;
    return c->textures[(size_t)texture_id].res.c_str();
}
void bf6_set_progress(bf6_ctx* c, bf6_progress_fn fn, void* user) {
    if (!c) return;
    c->progress = fn;
    c->progress_user = user;
}

int bf6_open_level(bf6_ctx* c, const char* level, const char* exe_path,
                   int all_levels, char* err, int err_len) {
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) {
            std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        }
        return 1;
    };
    if (!c || !level || !*level) return fail("no level");
    if (c->walked_level == level && c->walk) return 0;   // already open

    std::string e;
    auto tick = [c](const char* stage, int done, int total) {
        return c->report(stage, done, total);
    };
    c->src.set_progress(tick);

    if (!c->src.mount_level(level, all_levels != 0, e)) return fail(e);
    if (c->mounted_level != level) c->forget_texture_decodes();
    c->mounted_level = level;

    c->types.reset(new bf6::TypeDb());
    if (exe_path && *exe_path) {
        if (!c->types->open(exe_path, e)) return fail("type schema: " + e);
    } else {
        // No exe given: try the install's own, MP first because that is the
        // build a Portal level comes from.
        //
        // FIRST THAT OPENS IS NOT FIRST THAT WORKS. An executable whose
        // reflection sections are encrypted opens perfectly well and then
        // resolves every type to zero fields, so breaking on the first
        // successful open pins the schema to an unusable build and never
        // reaches a readable fallback. Keep looking, and only settle for an
        // encrypted one if nothing better exists, so the error below still
        // describes what was actually wrong.
        bool ok = false;
        for (const std::string& cand : bf6::TypeDb::exe_candidates(c->src.game_dir())) {
            if (!c->types->open(cand, e)) continue;
            ok = true;
            if (!c->types->looks_encrypted()) break;
        }
        if (!ok) return fail("no readable executable for the type schema");
    }
    if (c->types->looks_encrypted())
        return fail("this install's type table is encrypted (EA App build); "
                    "placements cannot be read from it yet");

    c->report("reading the object graph", 0, 0);
    c->walk.reset(new bf6::Walk(c->src, *c->types));
    c->walk->set_progress(tick);
    c->walk->build_catalog();
    if (!c->walk->run(level, e)) { c->walk.reset(); return fail(e); }
    c->walked_level = level;
    return 0;
}

static int serve_instance_rows(bf6_ctx* c, bf6_instance* out, int out_max) {
    if (!c || !c->walk) return 0;
    const std::vector<bf6::WalkRow>& rows = c->walk->rows();
    const int n = (int)rows.size();
    for (int i = 0; i < n && i < out_max; i++) {
        const bf6::WalkRow& r = rows[(size_t)i];
        out[i].res_name = r.mesh.c_str();     // owned by the walk, alive until reopen
        // 3x4 row-major: the three basis rows then the origin, in the GAME's
        // space. The binding converts handedness and units, not this.
        for (int k = 0; k < 4; k++) {
            out[i].xform[k * 3 + 0] = r.xf.m[k].x;
            out[i].xform[k * 3 + 1] = r.xf.m[k].y;
            out[i].xform[k * 3 + 2] = r.xf.m[k].z;
        }
        out[i].material_scope = 0;
        // Owned by the walk, alive until the level is reopened, same as the name.
        out[i].placing_bundle = r.bundle.empty() ? nullptr : r.bundle.c_str();
        out[i].variation      = r.var.empty()    ? nullptr : r.var.c_str();
        out[i].source         = r.src.empty()    ? nullptr : r.src.c_str();
    }
    return n;
}

int bf6_level_instances(bf6_ctx* c, const char* level,
                        bf6_instance* out, int out_max) {
    if (!c || !c->walk || !level || c->walked_level != level) return 0;
    return serve_instance_rows(c, out, out_max);
}
/* bf6_level_lights lives beside bf6_level_lighting at the bottom of this file:
 * it needs ensure_mounted and ensure_types, which are declared down there. */
static bf6_terrain* read_terrain_block(bf6_ctx* c, const char* level, bool water_surface) {
    if (!c || !level || !*level) return nullptr;

    // The level's heightfield lives in its streaming-tree resource, which is
    // the one whose name carries both "streamingtree" and the level id. Found
    // by name because that is what the mount gives us: there is no table that
    // says "this level's terrain is here".
    std::string want;
    {
        std::string lvl = level;
        for (char& ch : lvl) ch = (char)std::tolower((unsigned char)ch);
        for (const auto& kv : c->src.res()) {
            std::string n = kv.first;
            for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
            if (n.find("streamingtree") != std::string::npos &&
                n.find(lvl) != std::string::npos) { want = kv.first; break; }
        }
    }
    if (want.empty()) return nullptr;

    std::string err;
    std::vector<uint8_t> res = c->src.get_res(want, err);
    if (res.empty()) return nullptr;

    bf6::Terrain t;
    if (water_surface ? !t.parse_water_surface(res, err) : !t.parse(res, err)) return nullptr;
    t.resolve_external([&](const std::string& guid) {
        std::string e;
        return c->src.get_chunk(guid, e);
    });

    bf6::TerrainGrid g;
    if (!t.composite(g, 0, err)) return nullptr;

    TerrainHandle* th = new TerrainHandle();
    th->heights = std::move(g.heights);
    th->t.width = th->t.height = g.size;
    th->t.heights = th->heights.data();
    for (int i = 0; i < 3; i++) {
        th->t.world_min[i] = g.lo[i];
        th->t.world_max[i] = g.hi[i];
    }
    th->t.height_scale  = g.world_size_y;
    th->t.splat_texture = -1;
    th->t.color_texture = -1;
    c->handles[&th->t] = bf6_ctx::HK_TERRAIN;
    return &th->t;
}

bf6_terrain* bf6_read_terrain(bf6_ctx* c, const char* level) {
    return read_terrain_block(c, level, false);
}

bf6_terrain* bf6_read_water_heightfield(bf6_ctx* c, const char* level) {
    return read_terrain_block(c, level, true);
}

// ------------------------------------------------------------ terraindecals
//
// The property-name hashes the decal material binds its sheets under. These are
// u64 property names in the record's own stream, not depot slot hashes - a
// decal record carries its textures directly rather than through a shader
// state key.
//
// SLOT_OP IS THE ONE THAT MATTERS MOST. A lane stripe, a crosswalk, a stop
// line: they are all COVERAGE, not colour, and a consumer that binds only the
// base colour draws the asphalt and none of the paint - which is a road that
// still reads as empty.
static const uint64_t DECAL_SLOT_CV  = 0x399AC0336ACFE03Cull;   // base colour
static const uint64_t DECAL_SLOT_NHS = 0x567A9BC35CCBB1B2ull;   // normal/height/smooth
static const uint64_t DECAL_SLOT_AO  = 0x3A411B3E209FC9E2ull;   // ambient occlusion
// The authored colour a colourless record paints its mask in. Present on
// 3,365 colourless records and on 0 of 20,308 coloured ones, which is the
// control that says it is the substitute for the sheet rather than a tint
// over it.
static const uint64_t DECAL_TINT     = 0xF6B1A0A50786E60Aull;   // ParamTintColor
static const uint64_t DECAL_TINT2    = 0x4C6D12933B8C3184ull;   // second colour
static const uint64_t DECAL_MASKCHAN = 0x70EDD677D2A6A56Dull;   // Int32, 0..3
static const uint64_t DECAL_SLOT_OP  = 0x3810287D4CE70B49ull;   // coverage / markings

static const char* decal_res_for(bf6_ctx* c, const std::string& level)
{
    // BY RES TYPE, NOT BY NAME. The name is
    // "<level path>/<level>_terraindecals" on every map sampled, but the TYPE
    // is what the format guarantees, and a map that spelled its name
    // differently would otherwise silently ship no roads at all.
    static std::string want;
    want.clear();
    std::string lvl = level, fallback;
    for (char& ch : lvl) ch = (char)std::tolower((unsigned char)ch);
    for (const auto& kv : c->src.res()) {
        if (kv.second.type != (uint32_t)bf6::Decals::kResType) continue;
        std::string n = kv.first;
        for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
        if (n.find(lvl) != std::string::npos) { want = kv.first; return want.c_str(); }
        if (fallback.empty()) fallback = kv.first;
    }
    want = fallback;
    return want.empty() ? nullptr : want.c_str();
}

// ---- water ----------------------------------------------------------------
//
// Ported from highpoly_gamesource.gd (water / _water_partition / _water_look /
// _water_params), which is the reader the shipped Godot plugin renders from.
// Round one carries the PLANES and the COLOURS; the ripple textures and the
// ocean wind simulation stay with the binding that wants them.

static const char* kWaterTypeGuid = "ae0b69fc-2207-d874-8230-fcd467a592cf";
static const uint32_t kWaterStateKeyField = 0x2E15621F;
static const uint32_t kWSlotWaterA     = 0x50b54e74;   // linear float3, brighter
static const uint32_t kWSlotWaterB     = 0xdfcb439c;   // linear float3, darker
static const uint32_t kWSlotOceanColor = 0xeaca953a;   // the ocean variant's one colour
static const uint32_t kWSlotFoamNsh    = 0x60181bbf;   // t_oceanfoam_nsh
static const uint32_t kWSlotDetailNsh  = 0x635b5631;   // t_oceanmicrodetail_nsh
static const uint32_t kWSlotFoamRgb    = 0x3faa6a0b;   // t_waterfoam_rgb
static const uint32_t kWSlotNoise      = 0x18caba16;   // t_oceannoise
static const uint32_t kWSlotPerlin     = 0xf9b82d1e;   // perlin2d

static bool BF6_CountsWater(bf6_ctx* c, const std::string& name)
{
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(name, err);
    if (raw.empty()) return false;
    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return false;
    for (size_t i = 0; i < e.instance_count(); i++)
        if (bf6::TypeDb::guid_str(e.instance_type(i)) == kWaterTypeGuid) return true;
    return false;
}

int bf6_variation_live(bf6_ctx* c, const char* res_name,
                       const char* placing_bundle, const char* variation)
{
    if (!c || !res_name || !variation || !*variation) return 0;
    const std::string res = res_name;
    const std::string place = placing_bundle ? placing_bundle : "";
    const std::string var = variation;

    const std::string ck = res + "|" + place + "|" + var;
    auto it = c->var_live.find(ck);
    if (it != c->var_live.end()) return it->second;
    int& slot = c->var_live[ck];
    slot = 0;

    const auto scopes = material_scopes(c, res, place);
    if (scopes.empty()) return 0;

    std::string err;
    std::vector<uint8_t> mres = c->src.get_res(res, err);
    if (mres.empty()) return 0;
    bf6::MeshSet ms = bf6::meshset_parse(mres.data(), mres.size(), err);
    if (!ms.ok || ms.lods.empty()) return 0;

    for (const bf6::MeshSection& s : ms.lods[0].sections)
    {
        if (!s.state_key) continue;
        const uint64_t vk = variation_key(s.state_key, var);
        if (vk != s.state_key)
            for (const MaterialScope& scope : scopes)
                if (scope.depot->has_key(vk)) { slot = 1; break; }
        if (slot) break;
    }
    return slot;
}

// ---- the ocean simulation entity ------------------------------------------
//
// Ported from highpoly_gamesource.gd water_sim/_sim_in/_sim_row/_sim_curve.
// The field hashes are the hub's (water-material-resolves-through-the-depot,
// ocean-sim-winddistribution-is-a-splinecurve-of-direction).

static const char* kSimTypeGuid = "3ad51130-494f-ee8a-45cd-01103be713ee";
static const uint32_t kSimEnable       = 0x6E8C0B93;
static const uint32_t kSimWindAngle    = 0x2BD08352;
static const uint32_t kSimWindSpeed    = 0x8613EBCA;
static const uint32_t kSimChoppiness   = 0xD488F0CB;
static const uint32_t kSimWindDist     = 0xA1E59641;
static const uint32_t kSimFoamEnable   = 0xF0340815;
static const uint32_t kSimFoamThresh   = 0xFFA1D0E2;
static const uint32_t kSimFoamMax      = 0xF2C13BDD;
static const uint32_t kSimTileDim      = 0x54A5216B;
static const uint32_t kSimMinWavelen   = 0x787474E1;
static const uint32_t kSimLargeWaveRed = 0xC44A1FAF;
static const uint32_t kSimWaveThick    = 0xAA2BBED7;
static const uint32_t kSimWaveAmp      = 0xD395F0B1;
static const uint32_t kSimFoamHalfLife = 0x65FEEA86;
static const uint32_t kSimResolution   = 0x590C8625;
static const uint32_t kSimDefault      = 0x97EA1523;
static const uint32_t kSimPhysics      = 0xBBDB3870;
static const uint32_t kSimForcePlane   = 0x8C10DE28;
static const uint32_t kSimVisualCpu    = 0xA0EA8621;
static const uint32_t kSimCurveType    = 0xEC989148;
static const uint32_t kSimCurveX[3] = { 0xA3F9DFEE, 0xAB145027, 0x4FBB37BF };
static const uint32_t kSimCurveY[4] = { 0x57C358C3, 0xE9D446E7, 0xE4DA513E, 0xF324662A };
static const uint32_t kSimCurveG[6] = { 0x434009FD, 0x2767762B, 0x7378B1F0,
                                         0x36B72C11, 0xB2E9FCA9, 0xEB22B313 };
// Vec4 component field hashes, IN OFFSET ORDER - the warning in the finding:
// read x,y,z,w by offset, never by hash order.
static const uint32_t kSimVec4[4] = { 0x3901DB14, 0x42FC0F5E, 0x32A99B9C, 0x7C8062F2 };

static float BF6_SimF(const bf6::EbxValue& d, uint32_t h, float dflt)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f) return dflt;
    if (f->kind == bf6::EbxValue::Kind::Real) return (float)f->f;
    if (f->kind == bf6::EbxValue::Kind::Int)  return (float)f->i;
    if (f->kind == bf6::EbxValue::Kind::Uint) return (float)f->u;
    return dflt;
}

static bool BF6_SimB(const bf6::EbxValue& d, uint32_t h, bool dflt)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f) return dflt;
    if (f->kind == bf6::EbxValue::Kind::Bool) return f->b;
    if (f->kind == bf6::EbxValue::Kind::Int)  return f->i != 0;
    if (f->kind == bf6::EbxValue::Kind::Uint) return f->u != 0;
    return dflt;
}

static int BF6_SimI(const bf6::EbxValue& d, uint32_t h, int dflt)
{
    const bf6::EbxValue* f = d.field(h);
    if (!f) return dflt;
    if (f->kind == bf6::EbxValue::Kind::Int)  return (int)f->i;
    if (f->kind == bf6::EbxValue::Kind::Uint) return (int)f->u;
    if (f->kind == bf6::EbxValue::Kind::Real) return (int)f->f;
    return dflt;
}

static void BF6_SimRowV2(const bf6::EbxValue& d, int source_index,
                         bf6_water_sim_v2& s)
{
    s = bf6_water_sim_v2{};
    s.source_index                  = source_index;
    s.enabled                       = BF6_SimB(d, kSimEnable, false) ? 1 : 0;
    s.wind_angle_degrees            = BF6_SimF(d, kSimWindAngle, 0.f);
    s.wind_speed                    = BF6_SimF(d, kSimWindSpeed, 0.f);
    s.choppiness                    = BF6_SimF(d, kSimChoppiness, 0.f);
    s.tile_dimension                = BF6_SimF(d, kSimTileDim, 0.f);
    s.min_wavelength                = BF6_SimF(d, kSimMinWavelen, 0.f);
    s.large_wave_reduction          = BF6_SimF(d, kSimLargeWaveRed, 0.f);
    s.wave_amplitude                = BF6_SimF(d, kSimWaveAmp, 0.f);
    s.wave_thickness                = BF6_SimF(d, kSimWaveThick, 0.f);
    s.foam_enable                   = BF6_SimB(d, kSimFoamEnable, true) ? 1 : 0;
    s.foam_threshold                = BF6_SimF(d, kSimFoamThresh, 0.f);
    s.foam_max                      = BF6_SimF(d, kSimFoamMax, 0.f);
    s.foam_half_life                = BF6_SimF(d, kSimFoamHalfLife, 0.f);
    s.physics_simulation_enabled    = BF6_SimB(d, kSimPhysics, false) ? 1 : 0;
    s.force_simple_plane_collision  = BF6_SimB(d, kSimForcePlane, false) ? 1 : 0;
    s.visual_cpu_simulation_enabled = BF6_SimB(d, kSimVisualCpu, false) ? 1 : 0;

    if (const bf6::EbxValue* resolution = d.field(kSimResolution)) {
        if (resolution->kind == bf6::EbxValue::Kind::Struct)
            s.resolution = BF6_SimI(*resolution, kSimDefault, 0);
    }

    const bf6::EbxValue* c = d.field(kSimWindDist);
    if (!c || c->kind != bf6::EbxValue::Kind::Struct) return;
    const int n = BF6_SimI(*c, kSimCurveType, 0);
    if (n != 5 && n != 9 && n != 13) return;
    s.dist_count = n;

    float xs[12] = {0};
    float ys[12] = {0};
    int xi = 0, yi = 0;
    for (uint32_t hash : kSimCurveX) {
        const bf6::EbxValue* value = c->field(hash);
        for (uint32_t channel : kSimVec4)
            xs[xi++] = (value && value->kind == bf6::EbxValue::Kind::Struct)
                ? BF6_SimF(*value, channel, 0.f) : 0.f;
    }
    for (int block = 0; block < 3; ++block) {
        const bf6::EbxValue* value = c->field(kSimCurveY[block]);
        for (uint32_t channel : kSimVec4)
            ys[yi++] = (value && value->kind == bf6::EbxValue::Kind::Struct)
                ? BF6_SimF(*value, channel, 0.f) : 0.f;
    }

    const bf6::EbxValue* limits = c->field(kSimCurveY[3]);
    s.dist_clamp_min = limits && limits->kind == bf6::EbxValue::Kind::Struct
        ? BF6_SimF(*limits, kSimVec4[0], 0.f) : 0.f;
    s.dist_clamp_max = limits && limits->kind == bf6::EbxValue::Kind::Struct
        ? BF6_SimF(*limits, kSimVec4[1], 1.f) : 1.f;

    for (int i = 0; i < n; ++i) {
        s.dist_x[i] = i < n - 1 ? xs[i] : 1.f;
        s.dist_y[i] = i < n - 1 ? ys[i] : s.dist_clamp_min;
    }

    // G0/G1, G2/G3 and G4/G5 are the out/in coefficient pairs for the three
    // four-interval blocks. Their physical reflected offsets are scrambled,
    // but field-by-hash lookup has already restored the logical order here.
    for (int group = 0; group < 3; ++group) {
        const bf6::EbxValue* tangent_out = c->field(kSimCurveG[group * 2]);
        const bf6::EbxValue* tangent_in  = c->field(kSimCurveG[group * 2 + 1]);
        for (int lane = 0; lane < 4; ++lane) {
            const int interval = group * 4 + lane;
            if (interval >= n - 1) break;
            s.dist_tangent_out[interval] =
                tangent_out && tangent_out->kind == bf6::EbxValue::Kind::Struct
                    ? BF6_SimF(*tangent_out, kSimVec4[lane], 0.f) : 0.f;
            s.dist_tangent_in[interval] =
                tangent_in && tangent_in->kind == bf6::EbxValue::Kind::Struct
                    ? BF6_SimF(*tangent_in, kSimVec4[lane], 0.f) : 0.f;
        }
    }
}

// ---- the ground bake ------------------------------------------------------
//
// terraincomposite does the work; this owns the buffers so a caller across the
// ABI never has to free anything, and so a second bake replaces the first
// instead of leaking it.
// Mount on demand for the readers that only need the archives.
//
// bf6_open_level does the mount and then goes on to the type schema and the
// object graph, so a caller that failed there has a MOUNTED context and no way
// to say so. Rather than make every ground call depend on a walk it does not
// use, each one asks for the mount it needs.
static bool ensure_mounted(bf6_ctx* c, const char* level, std::string& err)
{
    if (!c || !level || !*level) { err = "no level"; return false; }
    if (c->mounted_level == level) return true;
    if (!c->src.mount_level(level, false, err)) return false;
    c->mounted_level = level;
    c->forget_texture_decodes();
    return true;
}

static bool ensure_types(bf6_ctx* c, std::string& err);

int bf6_texture_rgba(bf6_ctx* c, int texture_id, uint8_t* out, int64_t capacity)
{
    if (!c || !out || capacity <= 0) return 0;
    const bf6_texture* tx = bf6_texture_at(c, texture_id);
    if (!tx || !tx->data || tx->width <= 0 || tx->height <= 0) return 0;
    const int64_t need = (int64_t)tx->width * tx->height * 4;
    if (need > capacity) return 0;
    if (tx->format == BF6_FMT_RGBA8)
    {
        if (tx->data_len < need) return 0;
        std::memcpy(out, tx->data, (size_t)need);
        return 1;
    }
    int dxgi = 0;
    switch (tx->format) {
    case BF6_FMT_BC1: dxgi=71; break;
    case BF6_FMT_BC3: dxgi=77; break;
    case BF6_FMT_BC4: dxgi=80; break;
    case BF6_FMT_BC5: dxgi=83; break;
    case BF6_FMT_BC7: dxgi=98; break;
    default: return 0;
    }
    std::vector<uint8_t> rgba;
    std::string error;
    if (!bf6::bcn_to_rgba8(tx->data, (size_t)tx->data_len, tx->width, tx->height, dxgi, rgba, error) || (int64_t)rgba.size() != need) return 0;
    std::memcpy(out, rgba.data(), (size_t)need);
    return 1;
}

int bf6_layer_sheet(bf6_ctx* c, const char* res_name, int size,
                    uint8_t* out, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) {
            std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        }
        return 0;
    };
    if (!c || !res_name || !*res_name || !out || size <= 0) return fail("bad arguments");

    // The texture module already caches decodes, so this is the same path a
    // mesh binding takes; only the resampling is new.
    const int id = c->texture_id(res_name);
    const bf6_texture* tx = bf6_texture_at(c, id);
    if (!tx || !tx->data || tx->width <= 0 || tx->height <= 0)
        return fail(std::string("no texture at ") + res_name);

    // The ABI carries a bf6_fmt; the decoder speaks DXGI. sRGB pairs are
    // deliberately NOT selected here - the decode is the same bits either
    // way, and the consumer decides how to interpret them.
    int dxgi = 0;
    switch (tx->format) {
    case BF6_FMT_BC1:   dxgi = 71; break;
    case BF6_FMT_BC3:   dxgi = 77; break;
    case BF6_FMT_BC4:   dxgi = 80; break;
    case BF6_FMT_BC5:   dxgi = 83; break;
    case BF6_FMT_BC7:   dxgi = 98; break;
    case BF6_FMT_RGBA8: dxgi = 28; break;
    default: return fail("unsupported texture format");
    }

    std::vector<uint8_t> rgba;
    std::string e;
    if (tx->format == BF6_FMT_RGBA8) {
        const int64_t need = (int64_t)tx->width * tx->height * 4;
        if ((int64_t)tx->data_len < need) return fail("short rgba payload");
        rgba.assign(tx->data, tx->data + need);
    } else if (!bf6::bcn_to_rgba8(tx->data, (size_t)tx->data_len, tx->width,
                                  tx->height, dxgi, rgba, e)) {
        return fail(e.empty() ? "decode failed" : e);
    }
    if ((int64_t)rgba.size() < (int64_t)tx->width * tx->height * 4)
        return fail("short decode");

    // BOX RESAMPLE, not a point pick. A ground sheet resampled by nearest
    // neighbour keeps its highest frequencies and then aliases against the
    // tiling, which reads as crawling gravel; averaging the block that maps to
    // each output texel is the cheap correct answer.
    for (int y = 0; y < size; y++) {
        const int sy0 = (int)((int64_t)y * tx->height / size);
        const int sy1 = std::max(sy0 + 1, (int)((int64_t)(y + 1) * tx->height / size));
        for (int x = 0; x < size; x++) {
            const int sx0 = (int)((int64_t)x * tx->width / size);
            const int sx1 = std::max(sx0 + 1, (int)((int64_t)(x + 1) * tx->width / size));
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = sy0; sy < sy1 && sy < tx->height; sy++)
                for (int sx = sx0; sx < sx1 && sx < tx->width; sx++) {
                    const uint8_t* s = &rgba[((size_t)sy * tx->width + sx) * 4];
                    acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3];
                    n++;
                }
            uint8_t* d = &out[((size_t)y * size + x) * 4];
            if (n == 0) { d[0] = d[1] = d[2] = 0; d[3] = 255; continue; }
            d[0] = (uint8_t)(acc[0] / n); d[1] = (uint8_t)(acc[1] / n);
            d[2] = (uint8_t)(acc[2] / n); d[3] = (uint8_t)(acc[3] / n);
        }
    }
    return 1;
}

int bf6_ground_coverage_get(bf6_ctx* c, const char* level, int size,
                            bf6_ground_coverage* out, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) {
            std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        }
        return 0;
    };
    if (!c || !level || !*level || !out) return fail("bad arguments");

    std::string e;
    if (!ensure_mounted(c, level, e)) return fail(e);
    c->ground.reset(new (std::nothrow) bf6::GroundCoverage());
    if (!c->ground) return fail("out of memory allocating the coverage");
    bool ok = false;
    try {                                  // see bf6_bake_terrain on why
        ok = bf6::ground_coverage(c->src, level, size, *c->ground, e);
    } catch (const std::bad_alloc&) {
        c->ground.reset();
        return fail("out of memory building the ground coverage - try a smaller size");
    } catch (const std::exception& ex) {
        c->ground.reset();
        return fail(std::string("ground coverage failed: ") + ex.what());
    } catch (...) {
        c->ground.reset();
        return fail("ground coverage failed with an unknown exception");
    }
    if (!ok) {
        c->ground.reset();
        return fail(e.empty() ? "coverage failed" : e);
    }

    // The C view holds pointers into the C++ strings, so those strings have to
    // outlive the call: they do, because the context owns the coverage.
    c->ground_mats.clear();
    c->ground_mats.reserve(c->ground->materials.size());
    for (const bf6::GroundMaterial& m : c->ground->materials) {
        bf6_ground_material g{};
        g.layer = m.layer;
        g.albedo_res = m.albedo_res.c_str();
        g.normal_res = m.normal_res.c_str();
        g.metres_per_repeat = m.metres_per_repeat;
        g.uv_rotation_deg = m.uv_rotation_deg;
        g.tint[0] = m.tint[0]; g.tint[1] = m.tint[1]; g.tint[2] = m.tint[2];
        g.overlay = m.overlay;
        g.base_height = m.base_height;
        g.displace_range = m.displace_range;
        g.mask_ramp_exp = m.mask_ramp_exp;
        g.height_blend = m.height_blend;
        g.coord_scale[0] = m.coord_scale[0];
        g.coord_scale[1] = m.coord_scale[1];
        g.uv_offset[0] = m.uv_offset[0];
        g.uv_offset[1] = m.uv_offset[1];
        g.coverage_res = m.coverage_res.c_str();
        c->ground_mats.push_back(g);
    }

    *out = bf6_ground_coverage{};
    out->size = c->ground->size;
    out->lo[0] = c->ground->lo[0]; out->lo[1] = c->ground->lo[1];
    out->hi[0] = c->ground->hi[0]; out->hi[1] = c->ground->hi[1];
    out->idx = c->ground->idx.empty() ? nullptr : c->ground->idx.data();
    out->weight = c->ground->w.empty() ? nullptr : c->ground->w.data();
    out->colour = c->ground->colour.empty() ? nullptr : c->ground->colour.data();
    out->materials = c->ground_mats.empty() ? nullptr : c->ground_mats.data();
    out->material_count = (int32_t)c->ground_mats.size();
    const double total = (double)c->ground->size * (double)c->ground->size;
    out->empty_fraction = total > 0.0
        ? (float)((double)c->ground->empty_texels / total) : 0.f;
    out->slot_count = c->ground->slots;
    return 1;
}

int bf6_bake_terrain(bf6_ctx* c, const char* level,
                     const bf6_terrain_bake_opts* opts,
                     bf6_terrain_bake* out, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) {
            std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        }
        return 0;
    };
    if (!c || !level || !*level || !out) return fail("bad arguments");
    {
        std::string me;
        if (!ensure_mounted(c, level, me)) return fail(me);
    }

    bf6::TerrainBakeOpts o;
    if (opts) {
        o.rect_min[0] = opts->rect_min[0];
        o.rect_min[1] = opts->rect_min[1];
        o.rect_size   = opts->rect_size;
        if (opts->size > 0) o.size = opts->size;
        o.want_normal = opts->want_normal != 0;
        o.stochastic  = opts->stochastic != 0;
        o.colour_map  = opts->colour_map != 0;
        o.fallback_colour_map = opts->fallback_colour_map != 0;
    }

    // NOTHING MAY THROW ACROSS THIS BOUNDARY.
    //
    // These entry points are extern "C" and are called from another module. A
    // C++ exception that escapes one does not unwind into the caller, it takes
    // the whole process down: an editor sees exception 0xe06d7363 and reports
    // a fatal error with a stack that stops at the dll, which tells nobody
    // anything.
    //
    // A whole-map bake at 4096 allocates two 67 MB rasters plus the colour map
    // and the sheet decodes, so std::bad_alloc is a REAL outcome here rather
    // than a theoretical one, and it deserves a message rather than a crash.
    c->bake.reset(new (std::nothrow) bf6::TerrainBake());
    if (!c->bake) return fail("out of memory allocating the bake");
    std::string e;
    bool ok = false;
    try {
        ok = bf6::TerrainComposite::bake(c->src, level, o, *c->bake, e);
    } catch (const std::bad_alloc&) {
        c->bake.reset();
        return fail("out of memory during the ground bake - try a smaller size");
    } catch (const std::exception& ex) {
        c->bake.reset();
        return fail(std::string("ground bake failed: ") + ex.what());
    } catch (...) {
        c->bake.reset();
        return fail("ground bake failed with an unknown exception");
    }
    if (!ok) {
        c->bake.reset();
        return fail(e.empty() ? "bake failed" : e);
    }

    *out = bf6_terrain_bake{};
    out->size = c->bake->size;
    out->lo[0] = c->bake->lo[0]; out->lo[1] = c->bake->lo[1];
    out->hi[0] = c->bake->hi[0]; out->hi[1] = c->bake->hi[1];
    out->metres_per_texel = c->bake->metres_per_texel;
    out->albedo = c->bake->albedo.empty() ? nullptr : c->bake->albedo.data();
    out->normal = c->bake->normal.empty() ? nullptr : c->bake->normal.data();
    out->layers_used = c->bake->layers_present;
    out->layers_textured = c->bake->layers_decoded;
    const double total = (double)c->bake->size * (double)c->bake->size;
    out->fallback_fraction = total > 0.0
        ? (float)((double)c->bake->texels_untouched / total) : 0.f;
    return 1;
}

static void BF6_CollectWaterSimsInDir(bf6_ctx* c, const std::string& lvl,
                                      std::vector<bf6_water_sim_v2>& rows)
{
    rows.clear();
    if (!c || !c->types || lvl.empty()) return;

    // Same order-not-scope idiom as the water partition: schematics whose name
    // says water first, then default_schematic, then the rest of the level.
    std::vector<std::string> named, rest;
    for (const auto& kv : c->src.ebx()) {
        const std::string& n = kv.first;
        if (n.compare(0, lvl.size(), lvl) != 0) continue;
        if (n.find("schematic") == std::string::npos) continue;
        (n.find("water") != std::string::npos ? named : rest).push_back(n);
    }
    std::sort(named.begin(), named.end());
    std::sort(rest.begin(), rest.end());
    rest.insert(rest.begin(), lvl + "/default_schematic");

    named.insert(named.end(), rest.begin(), rest.end());
    for (const std::string& part : named) {
        std::string err;
        std::vector<uint8_t> raw = c->src.get_ebx(part, err);
        if (raw.empty()) continue;
        bf6::Ebx e(*c->types);
        if (!e.parse(std::move(raw), err)) continue;

        for (size_t i = 0; i < e.instance_count(); i++) {
            if (bf6::TypeDb::guid_str(e.instance_type(i)) != kSimTypeGuid) continue;
            bf6::EbxValue d = e.read_instance(i);
            bf6_water_sim_v2 row;
            BF6_SimRowV2(d, (int)i, row);
            if (row.enabled) rows.push_back(row);
        }
        if (!rows.empty()) break;
    }

    std::stable_sort(rows.begin(), rows.end(),
        [](const bf6_water_sim_v2& a, const bf6_water_sim_v2& b) {
            return a.tile_dimension > b.tile_dimension;
        });
    if (rows.size() > 4) rows.resize(4);
}

static void BF6_CollectWaterSims(bf6_ctx* c, const char* level,
                                 std::vector<bf6_water_sim_v2>& rows)
{
    rows.clear();
    if (!c || !level || !*level || !c->types || !c->walk) return;
    if (c->walked_level != level) return;

    std::string lvl = c->walk->root;
    if (lvl.size() > 4 && lvl.compare(lvl.size() - 4, 4, ".ebx") == 0) lvl.resize(lvl.size() - 4);
    const size_t slash = lvl.find_last_of('/');
    lvl = slash == std::string::npos ? std::string() : lvl.substr(0, slash);
    BF6_CollectWaterSimsInDir(c, lvl, rows);
}

extern "C++" {
static std::string BF6_IsolatedLevelDir(bf6_ctx* c, const char* level)
{
    std::string token = level ? level : "";
    std::replace(token.begin(), token.end(), '\\', '/');
    while (!token.empty() && token.back() == '/') token.pop_back();
    const size_t slash = token.find_last_of('/');
    if (slash != std::string::npos) token = token.substr(slash + 1);
    for (char& ch : token) ch = (char)std::tolower((unsigned char)ch);
    if (token.empty()) return {};

    const std::string marker = "/" + token + "/";
    const std::string schematic = "/default_schematic";
    const std::string fallback = "/default";
    std::string best;
    for (const auto& kv : c->src.ebx()) {
        std::string low = kv.first;
        for (char& ch : low) ch = (char)std::tolower((unsigned char)ch);
        const size_t at = low.find(marker);
        if (at == std::string::npos) continue;

        const bool exact = low.size() >= schematic.size() &&
            low.compare(low.size() - schematic.size(), schematic.size(), schematic) == 0;
        const bool default_part = low.size() >= fallback.size() &&
            low.compare(low.size() - fallback.size(), fallback.size(), fallback) == 0;
        if (exact || default_part) {
            const size_t cut = kv.first.find_last_of('/');
            if (cut != std::string::npos) {
                const std::string dir = kv.first.substr(0, cut);
                if (best.empty() || exact || dir.size() < best.size()) best = dir;
                if (exact) break;
            }
        } else if (best.empty()) {
            // Mount names are authoritative. This fallback truncates the first
            // matching name immediately after /<level>/ if a map omits the
            // conventional default partitions.
            best = kv.first.substr(0, at + marker.size() - 1);
        }
    }
    return best;
}
} // extern "C++"

int bf6_level_water_sims(bf6_ctx* c, const char* level,
                         bf6_water_sim_v2* out, int out_max)
{
    std::vector<bf6_water_sim_v2> rows;
    BF6_CollectWaterSims(c, level, rows);
    if (out && out_max > 0) {
        const int count = std::min<int>((int)rows.size(), out_max);
        for (int i = 0; i < count; ++i) out[i] = rows[(size_t)i];
    }
    return (int)rows.size();
}

int bf6_level_water_sims_isolated(bf6_ctx* c, const char* level,
                                  bf6_water_sim_v2* out, int out_max)
{
    if (!c || !level || !*level) return 0;
    std::string err;
    if (!ensure_mounted(c, level, err) || !ensure_types(c, err)) return 0;
    const std::string lvl = BF6_IsolatedLevelDir(c, level);
    if (lvl.empty()) return 0;

    std::vector<bf6_water_sim_v2> rows;
    BF6_CollectWaterSimsInDir(c, lvl, rows);
    if (out && out_max > 0) {
        const int count = std::min<int>((int)rows.size(), out_max);
        for (int i = 0; i < count; ++i) out[i] = rows[(size_t)i];
    }
    return (int)rows.size();
}

int bf6_level_water_sim(bf6_ctx* c, const char* level, bf6_water_sim* out)
{
    if (!out) return 0;
    bf6_water_sim_v2 full{};
    if (bf6_level_water_sims(c, level, &full, 1) <= 0) return 0;

    *out = bf6_water_sim{};
    out->wind_angle           = full.wind_angle_degrees;
    out->wind_speed           = full.wind_speed;
    out->choppiness           = full.choppiness;
    out->tile_dimension       = full.tile_dimension;
    out->min_wavelength       = full.min_wavelength;
    out->large_wave_reduction = full.large_wave_reduction;
    out->wave_thickness       = full.wave_thickness;
    out->foam_enable          = full.foam_enable;
    out->foam_threshold       = full.foam_threshold;
    out->foam_max             = full.foam_max;
    out->enabled              = full.enabled;
    out->dist_count           = full.dist_count;
    for (int i = 0; i < 13; ++i) {
        out->dist_x[i] = full.dist_x[i];
        out->dist_y[i] = full.dist_y[i];
    }
    return 1;
}

static float BF6_WaterSpline(const bf6_water_sim_v2& sim, float x)
{
    if (sim.dist_count != 5 && sim.dist_count != 9 && sim.dist_count != 13)
        return 0.f;
    int interval = sim.dist_count - 2;
    for (int i = 0; i < sim.dist_count - 1; ++i) {
        if (sim.dist_x[i] <= x && x < sim.dist_x[i + 1]) {
            interval = i;
            break;
        }
    }
    const float x0 = sim.dist_x[interval];
    const float x1 = sim.dist_x[interval + 1];
    float t = x1 - x0 >= 1.0e-5f ? (x - x0) / (x1 - x0) : 0.f;
    t = std::max(0.f, std::min(1.f, t));
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float value =
        (2.f * t3 - 3.f * t2 + 1.f) * sim.dist_y[interval] +
        (t3 - 2.f * t2 + t) * sim.dist_tangent_out[interval] +
        (-2.f * t3 + 3.f * t2) * sim.dist_y[interval + 1] +
        (t3 - t2) * sim.dist_tangent_in[interval];
    return std::max(sim.dist_clamp_min, std::min(sim.dist_clamp_max, value));
}

static uint32_t BF6_WaterRngFold(uint32_t value, uint32_t multiplier)
{
    const int32_t signed_value = (int32_t)value;
    const uint32_t low = (uint32_t)(uint16_t)signed_value * multiplier;
    const int32_t high = (signed_value >> 16) * (int32_t)multiplier +
                         (int32_t)(low >> 16);
    uint32_t folded = ((uint32_t)high & 0x7fffu) << 16;
    folded += (uint32_t)(high >> 15);
    folded += low & 0xffffu;
    folded += 0x80000001u;
    folded += (uint32_t)(((int32_t)folded >> 31) & 0x7fffffff);
    return folded;
}

static uint32_t BF6_WaterRngStep(uint32_t value)
{
    if (value == 0) value = 1;
    value = BF6_WaterRngFold(value, 0x5e30u);
    return BF6_WaterRngFold(value, 0x661fu);
}

static float BF6_WaterRngSigned(uint32_t& state)
{
    state = BF6_WaterRngStep(state);
    const int32_t top24 = ((int32_t)(state | 0x80u) >> 7) + 1;
    const float unit = (float)top24 * 5.9604644775390625e-8f;
    return unit + unit - 1.f;
}

int bf6_water_spectrum_h0(const bf6_water_sim_v2* sim,
                          float* out_rg, int out_float_count)
{
    if (!sim) return 0;
    const int n = sim->resolution;
    if (n < 16 || (n & (n - 1)) != 0 || sim->tile_dimension <= 0.f)
        return 0;
    const int64_t required64 = (int64_t)n * (int64_t)n * 2;
    if (required64 > INT_MAX) return 0;
    const int required = (int)required64;
    if (!out_rg) return required;
    if (out_float_count < required) return 0;

    const float inv_tile = 1.f / sim->tile_dimension;
    const float pi = 3.1415927410125732421875f;
    const float inv_two_pi = 0.15915493667125701904296875f;
    const float inv_g = 0.10204081237316131591796875f;
    const float inv_g2 = 0.010412327013909816741943359375f;
    uint32_t rng = 1;

    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            float random_x, random_y, radius2;
            do {
                random_x = BF6_WaterRngSigned(rng);
                random_y = BF6_WaterRngSigned(rng);
                radius2 = random_x * random_x + random_y * random_y;
            } while (radius2 > 1.f);
            const float normal_scale = std::sqrt((-2.f * std::log(radius2)) / radius2);
            const float normal_x = normal_scale * random_x;
            const float normal_y = normal_scale * random_y;

            const float kx = ((float)(x + x) - (float)n) * -pi * inv_tile;
            const float ky = ((float)(y + y) - (float)n) *  pi * inv_tile;
            const float k2 = kx * kx + ky * ky;
            const float k = std::sqrt(k2);
            float spectrum = 0.f;
            if (k >= 1.0e-4f) {
                float direction = std::atan2(ky, kx) * inv_two_pi + 0.5f -
                                  sim->wind_angle_degrees * (1.f / 360.f);
                if (direction < 0.f) direction += 1.f;
                const float directional_energy = BF6_WaterSpline(*sim, direction);
                const float wind_l = directional_energy * sim->wind_speed * sim->wind_speed;
                if (wind_l * inv_g >= 1.0e-6f) {
                    const float l2k2 = wind_l * wind_l * inv_g2 * k2;
                    float large_wave_factor = 1.f;
                    if (sim->large_wave_reduction > 0.f) {
                        large_wave_factor = sim->tile_dimension * 3.f * k /
                                                sim->large_wave_reduction - 2.f;
                        large_wave_factor = std::max(0.f, std::min(1.f, large_wave_factor));
                    }
                    spectrum = std::exp(-1.f / l2k2) * sim->wave_amplitude /
                               (k2 * k2) * directional_energy *
                               std::exp(-l2k2 * sim->min_wavelength) *
                               large_wave_factor;
                }
            }
            const float h0_scale = std::sqrt(spectrum * 0.5f) * inv_tile;
            const size_t offset = ((size_t)y * (size_t)n + (size_t)x) * 2;
            out_rg[offset] = normal_x * h0_scale;
            out_rg[offset + 1] = normal_y * h0_scale;
        }
    }
    return required;
}

int bf6_level_water(bf6_ctx* c, const char* level, bf6_water* out, int out_max)
{
	if (!c || !level || !*level) return 0;

	// The level directory, off the walk's own root - the narrowest prefix that
    // reaches both the water entity and the depot that materials it. Aftermath
    // declares water in _layers_content/water while the record lives in
    // _layers_content/content, so walking UP from the partition cannot reach
    // it; the level dir does.
	std::string lvl;
	if (c->types && c->walk && c->walked_level == level) {
		lvl = c->walk->root;
		if (lvl.size() > 4 && lvl.compare(lvl.size() - 4, 4, ".ebx") == 0) lvl.resize(lvl.size() - 4);
		const size_t slash = lvl.find_last_of('/');
		lvl = slash == std::string::npos ? std::string() : lvl.substr(0, slash);
	} else {
		// Lightweight water-lab route: mount the named level and locate its
		// authored directory without constructing the placement/object graph.
		std::string mount_err;
		if (!ensure_mounted(c, level, mount_err) || !ensure_types(c, mount_err)) return 0;
		lvl = BF6_IsolatedLevelDir(c, level);
		if (lvl.empty()) return 0;
	}

    // Find the partition that declares the water: the two authored homes
    // first, then anything under the level whose name says water, then the
    // rest of the level. The order is the cost control - the fallback sweep
    // parses partitions until one answers.
    std::string part;
    {
        const std::string cands[] = { lvl + "/default", lvl + "/_layers_content/water",
                                      std::string(level) + "/default" };
        for (const std::string& cand : cands)
            if (c->src.ebx().count(cand) && BF6_CountsWater(c, cand)) { part = cand; break; }
        if (part.empty() && !lvl.empty()) {
            std::vector<std::string> rest;
            for (const auto& kv : c->src.ebx()) {
                const std::string& n = kv.first;
                if (n.compare(0, lvl.size(), lvl) != 0) continue;
                std::string low = n;
                for (char& ch : low) ch = (char)tolower((unsigned char)ch);
                if (low.find("water") != std::string::npos) {
                    if (BF6_CountsWater(c, n)) { part = n; break; }
                } else rest.push_back(n);
            }
            if (part.empty())
                for (const std::string& n : rest)
                    if (BF6_CountsWater(c, n)) { part = n; break; }
        }
    }
    if (part.empty()) return 0;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(part, err);
    if (raw.empty()) return 0;
    bf6::Ebx e(*c->types);
    e.set_guid_index(&c->src.partition_index());
    if (!e.parse(std::move(raw), err)) return 0;

    int total = 0;
    for (size_t i = 0; i < e.instance_count(); i++) {
        if (bf6::TypeDb::guid_str(e.instance_type(i)) != kWaterTypeGuid) continue;

        // The transform, at FIXED offsets - see the accessor note in ebx.h.
        const int64_t base = e.payload() + (int64_t)e.instance_offset(i);
        const std::vector<uint8_t>& d = e.raw();
        if (base < 0 || (size_t)base + 0x60 > d.size()) continue;
        float sx, sz, tx, ty, tz;
        std::memcpy(&sx, d.data() + base + 0x20, 4);
        std::memcpy(&sz, d.data() + base + 0x48, 4);
        std::memcpy(&tx, d.data() + base + 0x50, 4);
        std::memcpy(&ty, d.data() + base + 0x54, 4);
        std::memcpy(&tz, d.data() + base + 0x58, 4);
        if (std::fabs(sx) < 1.f || std::fabs(sz) < 1.f) continue;

        bf6_water w{};
        w.detail_normal = w.foam_normal = w.foam_rgb = w.noise = w.perlin = -1;
        w.center[0] = tx; w.center[1] = tz;
        w.size[0] = std::fabs(sx); w.size[1] = std::fabs(sz);
        w.height = ty;
        w.shallow[0] = w.deep[0] = -1.f;
        w.is_ocean = 0;

        // The look: the state key, resolved in a depot scoped to THIS level.
        // A StateKey is only unique within a scope, so a global search can
        // bind a colliding key from a parallel level - confidently wrong.
        uint64_t key = 0;
        {
            const std::vector<uint32_t> want = { kWaterStateKeyField };
            bf6::EbxValue inst = e.read_instance(i, &want);
            if (const bf6::EbxValue* f = inst.field(kWaterStateKeyField)) {
                if (f->kind == bf6::EbxValue::Kind::Uint) key = f->u;
                else if (f->kind == bf6::EbxValue::Kind::Int) key = (uint64_t)f->i;
            }
        }
        if (key != 0) {
            for (const auto& kv : c->src.res()) {
                const std::string& rn = kv.first;
                if (rn.find("shaderblockdepot") == std::string::npos) continue;
                if (!lvl.empty() && rn.find(lvl) == std::string::npos) continue;
                const std::vector<uint8_t>* dbytes = nullptr;
                bf6::Depot* dep = c->depot_named(rn, &dbytes);
                if (!dep || !dbytes || !dep->has_key(key)) continue;
                bf6::MaterialBinding mb = dep->textures_for(key, *dbytes);
                if (!mb.valid) continue;
                w.is_ocean = (mb.textures.count(kWSlotDetailNsh) ||
                              mb.textures.count(kWSlotFoamNsh)) ? 1 : 0;

                // THE SHEETS. Same resolution the mesh path uses: the slot
                // holds a FILE guid, the partition index turns that into an
                // asset name, and the name without .ebx is the resource.
                {
                    const auto& gi = c->src.partition_index();
                    auto grab = [&](uint32_t slot) -> int32_t {
                        auto sit = mb.textures.find(slot);
                        if (sit == mb.textures.end()) return -1;
                        auto ait = gi.find(sit->second);
                        if (ait == gi.end()) return -1;
                        std::string tres = ait->second;
                        if (tres.size() > 4 && tres.compare(tres.size() - 4, 4, ".ebx") == 0)
                            tres.resize(tres.size() - 4);
                        return c->texture_id(tres);
                    };
                    w.detail_normal = grab(kWSlotDetailNsh);
                    w.foam_normal   = grab(kWSlotFoamNsh);
                    w.foam_rgb      = grab(kWSlotFoamRgb);
                    w.noise         = grab(kWSlotNoise);
                    w.perlin        = grab(kWSlotPerlin);

                    // EVERY slot this record binds, named or not. The five
                    // named ones came from one variant; a level on another
                    // variant binds its detail elsewhere, and a reader that
                    // only looks for the names it knows reports "no textures"
                    // for a surface that is covered in them.
                    bool dump_water_slots = false;
#if defined(_MSC_VER)
                    char* water_slots_env = nullptr;
                    size_t water_slots_len = 0;
                    if (_dupenv_s(&water_slots_env, &water_slots_len,
                                  "BF6_WATER_SLOTS") == 0)
                        dump_water_slots = water_slots_env && *water_slots_env;
                    std::free(water_slots_env);
#else
                    const char* water_slots_env = std::getenv("BF6_WATER_SLOTS");
                    dump_water_slots = water_slots_env && *water_slots_env;
#endif
                    if (dump_water_slots) {
                        for (const auto& kv2 : mb.textures) {
                            auto a2 = gi.find(kv2.second);
                            std::fprintf(stderr, "    water slot %08x -> %s\n",
                                kv2.first, a2 == gi.end() ? "?" : a2->second.c_str());
                        }
                    }
                }
                float c3[3];
                if (w.is_ocean) {
                    // ONE colour; deep stays absent on purpose - the consumer
                    // darkens, and duplicating it would flatten the gradient
                    // while still looking like mined data.
                    if (const_c3(mb, kWSlotOceanColor, 0, c3))
                        { w.shallow[0]=c3[0]; w.shallow[1]=c3[1]; w.shallow[2]=c3[2]; }
                } else {
                    if (const_c3(mb, kWSlotWaterA, 0, c3))
                        { w.shallow[0]=c3[0]; w.shallow[1]=c3[1]; w.shallow[2]=c3[2]; }
                    if (const_c3(mb, kWSlotWaterB, 0, c3))
                        { w.deep[0]=c3[0]; w.deep[1]=c3[1]; w.deep[2]=c3[2]; }
                }
                break;
            }
        }

        if (out && total < out_max) out[total] = w;
        total++;
    }
    return total;
}

int bf6_level_decals(bf6_ctx* c, const char* level, bf6_decal* out, int out_max)
{
    if (!c || !level || !*level) return 0;

    // Parsed once per level and kept, like the walk: the container is a few
    // megabytes and the caller may well ask for a count before asking for the
    // rows.
    if (c->decal_level != level) {
        c->decals.reset();
        c->decal_verts.clear();
        c->decal_rows.clear();
        c->decal_level = level;

        const char* rn = decal_res_for(c, level);
        if (!rn) return 0;
        std::string err;
        std::vector<uint8_t> res = c->src.get_res(rn, err);
        if (res.empty()) return 0;

        std::unique_ptr<bf6::Decals> dc(new bf6::Decals());
        if (!dc->parse(std::move(res), err)) return 0;

        const auto& gi = c->src.partition_index();
        auto tex_of = [&](const bf6::DecalRecord& r, uint64_t name) -> int32_t {
            for (const bf6::DecalProp& p : r.props) {
                if (p.name != name || p.kind != bf6::DecalProp::Kind::Texture) continue;
                auto it = gi.find(p.guid);
                if (it == gi.end()) return -1;
                std::string t = it->second;
                if (t.size() > 4 && t.compare(t.size() - 4, 4, ".ebx") == 0) t.resize(t.size() - 4);
                return c->texture_id(t);
            }
            return -1;
        };

        c->decal_verts.reserve(dc->records().size());
        for (const bf6::DecalRecord& r : dc->records()) {
            std::vector<bf6::DecalVertex> vs = dc->vertices(r);
            // NON-INDEXED, so this is an equality and not a bound. A record
            // whose vertex count disagrees with its triangle count is being
            // read at the wrong offset, and a wrong offset still yields
            // plausible floats - it would render as confetti rather than as
            // nothing. Dropped.
            if (vs.empty() || vs.size() != (size_t)r.tri_count * 3) continue;

            std::vector<float> flat;
            flat.reserve(vs.size() * 8);
            for (const bf6::DecalVertex& v : vs) {
                flat.push_back(v.x); flat.push_back(v.z);
                flat.push_back(v.u); flat.push_back(v.v);
                flat.push_back(v.r); flat.push_back(v.g);
                flat.push_back(v.b); flat.push_back(v.a);
            }

            bf6_decal d{};
            d.vertex_count = (int32_t)vs.size();
            for (int k = 0; k < 3; k++) { d.aabb_min[k] = r.aabb_min[k]; d.aabb_max[k] = r.aabb_max[k]; }
            d.tiling0 = r.tiling0;
            d.tiling1 = r.tiling1;
            d.planar  = bf6::Decals::is_planar(vs) ? 1 : 0;
            d.albedo  = tex_of(r, DECAL_SLOT_CV);
            d.opacity = tex_of(r, DECAL_SLOT_OP);
            d.normal  = tex_of(r, DECAL_SLOT_NHS);
            d.ao           = tex_of(r, DECAL_SLOT_AO);
            d.asset_slot   = (int32_t)r.asset_slot;
            d.mask_channel = -1;
            d.has_tint = d.has_tint2 = 0;
            for (int k = 0; k < 3; k++) { d.tint[k] = -1.f; d.tint2[k] = -1.f; }
            for (const bf6::DecalProp& p : r.props) {
                if (p.kind == bf6::DecalProp::Kind::Vec3 && p.values.size() >= 3) {
                    if (p.name == DECAL_TINT) {
                        for (int k = 0; k < 3; k++) d.tint[k] = p.values[(size_t)k];
                        d.has_tint = 1;
                    } else if (p.name == DECAL_TINT2) {
                        for (int k = 0; k < 3; k++) d.tint2[k] = p.values[(size_t)k];
                        d.has_tint2 = 1;
                    }
                } else if (p.name == DECAL_MASKCHAN
                           && p.kind == bf6::DecalProp::Kind::Int
                           && !p.ints.empty()) {
                    d.mask_channel = p.ints[0];
                }
            }

            c->decal_verts.push_back(std::move(flat));
            c->decal_rows.push_back(d);
        }
        // The vectors are only now stable, so the pointers go in last. Taking
        // them inside the loop would leave every one but the final row dangling
        // after a reallocation, which reads as a decoder that produces garbage
        // for all but the last record.
        for (size_t i = 0; i < c->decal_rows.size(); i++)
            c->decal_rows[i].verts = c->decal_verts[i].data();

        c->decals = std::move(dc);
    }

    const int n = (int)c->decal_rows.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = c->decal_rows[i];
    return n;
}

// ---- the level's VisualEnvironment ----------------------------------------
//
// Mounts AND loads the type schema on demand, for the same reason the ground
// calls mount on demand: the lighting does not need the placement walk, and a
// caller whose walk failed still has a perfectly readable set of archives.
static bool ensure_types(bf6_ctx* c, std::string& err)
{
    if (c->types) return true;
    std::unique_ptr<bf6::TypeDb> t(new bf6::TypeDb());
    // FIRST THAT OPENS IS NOT FIRST THAT WORKS - see the same rule in
    // bf6_mount_all. Returning on the first candidate that opens abandons a
    // readable fallback the moment an encrypted build is listed ahead of it.
    bool opened = false;
    for (const std::string& cand : bf6::TypeDb::exe_candidates(c->src.game_dir())) {
        if (!t->open(cand, err)) continue;
        opened = true;
        if (t->looks_encrypted()) continue;
        c->types = std::move(t);
        return true;
    }
    err = opened ? "this install's type table is encrypted (EA App build)"
                 : "no readable executable for the type schema";
    return false;
}

// Copy a std::string into a fixed field, always terminated. Truncation is
// reported by the caller's own eyes rather than silently: the fields are sized
// well past the longest name in the game (a VE partition path is about 60).
static void put_str(char* dst, size_t cap, const std::string& s)
{
    if (cap == 0) return;
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

int bf6_level_lighting(bf6_ctx* c, const char* level,
                       bf6_ve_lighting* out, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level || !out) return fail("bad arguments");

    std::string e;
    const std::string requested = level;
    /* A VE PRESET IS IDENTIFIED BY ITS LEAF NAME, not by its directory. This
     * test used to look for "/lighting/ve_", which is where a LEVEL keeps its
     * presets - but the gameplay ones do not live there:
     * `common/fx/ve/gameplay/ve_fullscreen_thermal_whot` and the optic thermal
     * views are under common/fx. Those fell through to the LEVEL branch and
     * failed with "no archives for level '<a partition name>'", which reads as
     * the preset being absent rather than as this test being too narrow. A
     * level id never begins "ve_", so widening to the leaf cannot swallow one. */
    const size_t leaf_at = requested.find_last_of('/');
    const std::string leaf = (leaf_at == std::string::npos) ? requested
                                                            : requested.substr(leaf_at + 1);
    const bool explicit_preset = leaf.rfind("ve_", 0) == 0;
    if (!explicit_preset && !ensure_mounted(c, level, e)) return fail(e);
    if (!ensure_types(c, e)) return fail(e);

    bf6::VeLighting v;
    const bool decoded = explicit_preset
        ? bf6::ve_lighting_partition(c->src, *c->types, requested, v, e)
        : bf6::ve_lighting(c->src, *c->types, level, v, e);
    if (!decoded)
        return fail(e.empty() ? "no visual environment" : e);

    c->ve = v;
    c->ve_level = level;
    c->ve_import_ptrs.clear();

    *out = bf6_ve_lighting{};
    put_str(out->preset, sizeof(out->preset), v.preset);
    put_str(out->preset_path, sizeof(out->preset_path), v.preset_path);
    out->preset_candidates = v.preset_candidates;
    out->components = v.components;
    out->visibility = v.visibility;
    out->component_count = v.component_count;

    out->sun_rotation_x = v.sun_rotation_x;
    out->sun_rotation_y = v.sun_rotation_y;
    for (int i = 0; i < 3; i++) out->sun_color[i] = v.sun_color[i];
    out->sun_intensity = v.sun_intensity;
    out->sun_angular_radius = v.sun_angular_radius;
    out->sun_specular_scale = v.sun_specular_scale;
    out->sun_shadow_view_distance = v.sun_shadow_view_distance;
    out->cloud_shadow_size = v.cloud_shadow_size;
    out->cloud_shadow_coverage = v.cloud_shadow_coverage;
    out->cloud_shadow_exponent = v.cloud_shadow_exponent;
    for (int i = 0; i < 2; i++) {
        out->cloud_shadow_speed[i] = v.cloud_shadow_speed[i];
        out->cloud_shadow_translation[i] = v.cloud_shadow_translation[i];
    }
    out->cloud_radiosity = v.cloud_radiosity;

    out->sky_type = v.sky_type;
    out->sky_luminance_scale = v.sky_luminance_scale;
    out->sky_panoramic_rotation = v.sky_panoramic_rotation;
    out->sky_panoramic_tile_factor = v.sky_panoramic_tile_factor;
    out->sky_draw_sun_disc = v.sky_draw_sun_disc;
    out->sun_disc_size = v.sun_disc_size;
    out->sun_disc_scale = v.sun_disc_scale;
    for (int i = 0; i < 3; i++) out->rayleigh[i] = v.rayleigh[i];
    out->rayleigh_scale = v.rayleigh_scale;
    out->mie_coefficient = v.mie_coefficient;
    out->mie_g = v.mie_g;
    out->use_aerial_perspective = v.use_aerial_perspective;
    out->aerial_perspective_scale = v.aerial_perspective_scale;
    out->aerial_perspective_intensity = v.aerial_perspective_intensity;
    out->earth_radius = v.earth_radius;
    out->atmosphere_radius = v.atmosphere_radius;
    for (int i = 0; i < 3; i++) out->height_fog_color_add[i] = v.height_fog_color_add[i];
    out->cloud1_altitude = v.cloud1_altitude;
    out->cloud1_tile_factor = v.cloud1_tile_factor;
    out->cloud1_rotation = v.cloud1_rotation;
    out->cloud1_speed = v.cloud1_speed;
    out->cloud1_alpha_mul = v.cloud1_alpha_mul;
    for (int i = 0; i < 3; i++) out->cloud1_color[i] = v.cloud1_color[i];

    out->fog_height_enable = v.fog_height_enable;
    out->fog_color_enable = v.fog_color_enable;
    out->fog_gradient_enable = v.fog_gradient_enable;
    for (int i = 0; i < 3; i++) out->fog_color[i] = v.fog_color[i];
    out->fog_dist_start = v.fog_dist_start;
    out->fog_dist_end = v.fog_dist_end;
    out->fog_color_start = v.fog_color_start;
    out->fog_color_end = v.fog_color_end;
    out->fog_height_start = v.fog_height_start;
    out->fog_height_end = v.fog_height_end;
    out->fog_altitude = v.fog_altitude;
    out->fog_depth = v.fog_depth;
    out->fog_visibility_range = v.fog_visibility_range;
    out->volumetrics_enable = v.volumetrics_enable;
    out->sun_scatter_intensity = v.sun_scatter_intensity;
    out->local_light_scatter_intensity = v.local_light_scatter_intensity;

    out->auto_exposure = v.auto_exposure;
    out->ev = v.ev;
    out->ev_max = v.ev_max;
    out->exposure_compensation = v.exposure_compensation;
    for (int i = 0; i < 3; i++) out->bloom_scale[i] = v.bloom_scale[i];
    out->bloom_method = v.bloom_method;

    out->grading_enable = v.grading_enable;
    for (int i = 0; i < 3; i++) {
        out->grade_brightness[i] = v.grade_brightness[i];
        out->grade_contrast[i] = v.grade_contrast[i];
        out->grade_saturation[i] = v.grade_saturation[i];
    }
    out->grade_hue = v.grade_hue;
    out->white_temperature = v.white_temperature;
    out->white_tint = v.white_tint;

    out->ao_affects_outdoor_light = v.ao_affects_outdoor_light;
    out->ao_affects_local_light = v.ao_affects_local_light;
    out->ssao_max_distance_inner = v.ssao_max_distance_inner;
    out->ssao_max_distance_outer = v.ssao_max_distance_outer;
    out->hbao_radius = v.hbao_radius;
    out->hbao_contrast = v.hbao_contrast;
    out->dynamic_ao_factor = v.dynamic_ao_factor;

    for (int i = 0; i < 3; i++) {
        out->gi_terrain_color[i] = v.gi_terrain_color[i];
        out->gi_sky_color[i] = v.gi_sky_color[i];
        out->gi_ground_color[i] = v.gi_ground_color[i];
        out->gi_sun_color[i] = v.gi_sun_color[i];
    }
    out->gi_backlight_rotation_x = v.gi_backlight_rotation_x;
    out->gi_backlight_rotation_y = v.gi_backlight_rotation_y;
    out->gi_bounce_scale = v.gi_bounce_scale;
    out->gi_sun_scale = v.gi_sun_scale;

    // A texture id is only offered where the mount actually carries a resource
    // of that name. A VE names an EBX PARTITION, and while the texture resource
    // shares the name on every case checked, asking for one that is not there
    // would hand back an id that decodes to nothing.
    auto tex = [&](const std::string& res) -> int32_t {
        return (!res.empty() && c->src.res().count(res)) ? c->texture_id(res) : -1;
    };
    put_str(out->panorama_res, sizeof(out->panorama_res), v.panorama_res);
    put_str(out->panorama_alpha_res, sizeof(out->panorama_alpha_res), v.panorama_alpha_res);
    put_str(out->sky_gradient_res, sizeof(out->sky_gradient_res), v.sky_gradient_res);
    put_str(out->flow_mask_res, sizeof(out->flow_mask_res), v.flow_mask_res);
    put_str(out->cloud_layer1_res, sizeof(out->cloud_layer1_res), v.cloud_layer1_res);
    put_str(out->cloud_shadow_res, sizeof(out->cloud_shadow_res), v.cloud_shadow_res);
    put_str(out->secondary_cloud_shadow_res, sizeof(out->secondary_cloud_shadow_res),
            v.secondary_cloud_shadow_res);
    put_str(out->grading_lut_res, sizeof(out->grading_lut_res), v.grading_lut_res);
    put_str(out->lens_dirt_res, sizeof(out->lens_dirt_res), v.lens_dirt_res);
    out->has_panorama = v.panorama_res.empty() ? 0 : 1;
    out->panorama_texture = tex(v.panorama_res);
    out->sky_gradient_texture = tex(v.sky_gradient_res);
    out->cloud_shadow_texture = tex(v.cloud_shadow_res);

    out->fields_found = v.fields_found;
    out->fields_expected = v.fields_expected;
    out->sky_cloud_extension_version = 1;
    for (int i = 0; i < 2; i++) {
        out->sky_panoramic_uv_min[i] = v.sky_panoramic_uv_min[i];
        out->sky_panoramic_uv_max[i] = v.sky_panoramic_uv_max[i];
        out->secondary_cloud_shadow_speed[i] = v.secondary_cloud_shadow_speed[i];
        out->secondary_cloud_shadow_translation[i] = v.secondary_cloud_shadow_translation[i];
    }
    out->sky_flow_distance = v.sky_flow_distance;
    out->sky_flow_direction = v.sky_flow_direction;
    out->sky_flow_period = v.sky_flow_period;
    out->sky_flow_height_mask_scale = v.sky_flow_height_mask_scale;
    out->sky_flow_height_mask_bias = v.sky_flow_height_mask_bias;
    out->secondary_cloud_shadow_size = v.secondary_cloud_shadow_size;
    out->secondary_cloud_shadow_coverage = v.secondary_cloud_shadow_coverage;
    out->secondary_cloud_shadow_exponent = v.secondary_cloud_shadow_exponent;
    out->cloud_shadow_addressing_mode = v.cloud_shadow_addressing_mode;
    out->secondary_cloud_shadow_addressing_mode = v.secondary_cloud_shadow_addressing_mode;
    out->cloud_shadow_is_top_down = v.cloud_shadow_is_top_down;
    out->secondary_cloud_shadow_is_top_down = v.secondary_cloud_shadow_is_top_down;
    out->cloud_shadow_start_fade = v.cloud_shadow_start_fade;
    out->cloud_shadows_fade_distance = v.cloud_shadows_fade_distance;
    out->cloud_shadow_height_fade_enable = v.cloud_shadow_height_fade_enable;
    out->cloud_shadow_start_height_fade = v.cloud_shadow_start_height_fade;
    out->cloud_shadows_height_fade_distance = v.cloud_shadows_height_fade_distance;
    out->secondary_cloud_shadow_texture = tex(v.secondary_cloud_shadow_res);
    out->panorama_alpha_texture = tex(v.panorama_alpha_res);
    out->flow_mask_texture = tex(v.flow_mask_res);
    out->cloud_layer1_texture = tex(v.cloud_layer1_res);
    out->grading_lut_texture = tex(v.grading_lut_res);
    out->lens_dirt_texture = tex(v.lens_dirt_res);
    return 1;
}

int bf6_level_lighting_imports(bf6_ctx* c, const char* level,
                               const char** out, int out_max)
{
    if (!c || !level || !*level) return 0;
    if (c->ve_level != level) {
        bf6_ve_lighting tmp;
        if (!bf6_level_lighting(c, level, &tmp, nullptr, 0)) return 0;
    }
    c->ve_import_ptrs.clear();
    c->ve_import_ptrs.reserve(c->ve.imports.size());
    for (const std::string& s : c->ve.imports) c->ve_import_ptrs.push_back(s.c_str());
    const int n = (int)c->ve_import_ptrs.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = c->ve_import_ptrs[(size_t)i];
    return n;
}

// ---- the level's local light placements ------------------------------------
//
// Mounts and loads the type schema on demand for the same reason the VE does:
// the lights need a traversal of their own and not the placement walk, so a
// caller whose walk failed still has a perfectly readable set of archives.
//
// THE EXECUTABLE THIS RESOLVES AGAINST DECIDES WHETHER THE ANSWER IS RIGHT.
// The SP and MP builds ship different reflection schemas for the same classes,
// and the placement component's `Light` pointer sits at a different offset in
// each. Reading MP level data through the SP schema returns wrong values from
// the right bytes and reports no error. ensure_types prefers the MP build.
static void cache_light_rows(bf6_ctx* c, const std::string& key,
                             std::vector<bf6::LevelLight>&& lights,
                             const bf6::LightStats& light_stats)
{
    c->lights = std::move(lights);
    c->light_stats = light_stats;
    c->lights_level = key;
    c->light_rows.clear();

    // Built once and kept, because every const char* in it points into the
    // strings held by c->lights.
    c->light_rows.resize(c->lights.size());
    for (size_t i = 0; i < c->lights.size(); i++) {
        const bf6::LevelLight& L = c->lights[i];
        bf6_light& r = c->light_rows[i];
        r = bf6_light{};
        r.type = L.kind;
        for (int k = 0; k < 4; k++) {
            r.xform[k * 3 + 0] = L.xf.m[k].x;
            r.xform[k * 3 + 1] = L.xf.m[k].y;
            r.xform[k * 3 + 2] = L.xf.m[k].z;
        }
        for (int k = 0; k < 3; k++) r.color[k] = L.color[k];
        r.intensity = L.intensity;
        r.unit = L.unit;
        r.dimmer = L.dimmer;
        r.attenuation_radius = L.attenuation_radius;
        r.attenuation_offset = L.attenuation_offset;
        r.inner_angle = L.inner_angle;
        r.outer_angle = L.outer_angle;
        r.shape_radius = L.shape_radius;
        r.tube_width = L.tube_width;
        r.is_capsule = L.is_capsule;
        r.rect_height = L.rect_height;
        r.rect_aspect = L.rect_aspect;
        r.rect_shape = L.rect_shape;
        r.cast_shadows_enable = L.cast_shadows_enable;
        r.cast_shadows = L.cast_shadows;
        r.cast_volumetric = L.cast_volumetric;
        r.volumetric_scattering = L.volumetric_scattering;
        r.affect_diffuse = L.affect_diffuse;
        r.affect_specular = L.affect_specular;
        r.affect_radiosity = L.affect_radiosity;
        r.emissive_shape_enable = L.emissive_shape_enable;
        r.ies_profile = L.ies_profile.empty() ? nullptr : L.ies_profile.c_str();
        r.ies_multiplier = L.ies_multiplier;
        r.ies_as_mask = L.ies_as_mask;
        r.texture = L.texture.empty() ? nullptr : L.texture.c_str();
        r.cull_distance = L.cull_distance;
        r.fade_distance = L.fade_distance;
        r.source = L.source.empty() ? nullptr : L.source.c_str();
        r.flags = L.flags;
        r.from_component = L.from_component;
    }
}

static int serve_light_rows(bf6_ctx* c, bf6_light* out, int out_max,
                            bf6_light_stats* stats)
{
    if (stats) {
        const bf6::LightStats& s = c->light_stats;
        *stats = bf6_light_stats{};
        stats->total  = (int32_t)c->light_rows.size();
        stats->sphere = (int32_t)s.by_kind[bf6::kLightSphere];
        stats->spot   = (int32_t)s.by_kind[bf6::kLightSpot];
        stats->tube   = (int32_t)s.by_kind[bf6::kLightTube];
        stats->rect   = (int32_t)s.by_kind[bf6::kLightRect];
        stats->other  = (int32_t)s.by_kind[bf6::kLightOther];
        stats->placed_by_component     = (int32_t)s.placed_by_component;
        stats->placed_by_own_transform = (int32_t)s.placed_by_own_transform;
        stats->placed_at_holder        = (int32_t)s.placed_at_holder;
        stats->components     = (int32_t)s.components;
        stats->comp_unlinked  = (int32_t)s.comp_unlinked;
        stats->comp_excluded  = (int32_t)s.comp_excluded;
        stats->partitions     = (int32_t)s.partitions;
        stats->unresolved_types = (int32_t)s.unresolved_types;
        stats->comp_legacy_resolved = (int32_t)s.comp_legacy_resolved;
        stats->comp_legacy_agree    = (int32_t)s.comp_legacy_agree;
    }

    const int n = (int)c->light_rows.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; i++) out[i] = c->light_rows[(size_t)i];
    return n;
}

int bf6_level_lights(bf6_ctx* c, const char* level,
                     bf6_light* out, int out_max,
                     bf6_light_stats* stats, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level) return fail("bad arguments");

    if (c->lights_level != level) {
        std::string e;
        if (!ensure_mounted(c, level, e)) return fail(e);
        if (!ensure_types(c, e)) return fail(e);

        std::vector<bf6::LevelLight> lights;
        bf6::LightStats light_stats;
        if (!bf6::level_lights(c->src, *c->types, level, lights, light_stats, e))
            return fail(e.empty() ? "the light traversal found no level root" : e);
        cache_light_rows(c, level, std::move(lights), light_stats);
    }
    return serve_light_rows(c, out, out_max, stats);
}

int bf6_asset_lights(bf6_ctx* c, const char* asset,
                     bf6_light* out, int out_max,
                     bf6_light_stats* stats, char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !asset || !*asset) return fail("bad arguments");

    const std::string key = std::string("@asset:") + asset;
    if (c->lights_level != key) {
        std::string e;
        if (!ensure_types(c, e)) return fail(e);
        std::vector<bf6::LevelLight> lights;
        bf6::LightStats light_stats;
        const bool frontend = std::string(asset).find(
            "game/glacierflow/flow_mainmenu") == 0;
        if (!bf6::level_lights(c->src, *c->types, asset, lights, light_stats,
                              e, frontend))
            return fail(e.empty() ? "the light traversal could not resolve the asset" : e);
        cache_light_rows(c, key, std::move(lights), light_stats);
    }
    return serve_light_rows(c, out, out_max, stats);
}

int bf6_asset_instances(bf6_ctx* c, const char* asset,
                        bf6_instance* out, int out_max,
                        char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !asset || !*asset) return fail("bad arguments");

    const std::string key = std::string("@asset:") + asset;
    if (!c->walk || c->walked_level != key) {
        std::string e;
        if (!ensure_types(c, e)) return fail(e);

        auto tick = [c](const char* stage, int done, int total) {
            return c->report(stage, done, total);
        };
        const bool frontend = std::string(asset).find(
            "game/glacierflow/flow_mainmenu") == 0;
        const bool reusableAssetCatalog = c->walk &&
            c->walked_level.rfind("@asset:", 0) == 0 &&
            c->walk->catalog_bounded_frontend() == frontend;
        if (!reusableAssetCatalog) {
            c->walk.reset(new bf6::Walk(c->src, *c->types));
            c->walk->build_catalog(frontend);
        }
        c->walk->set_progress(tick);
        c->walk->set_include_frontend(true);
        // run() clears only traversal state; the immutable mounted-name/GUID
        // catalogue is valid for every exact asset in this Source generation.
        // Reuse it when the viewer asks for the customization owner followed
        // by the hangar owner instead of rebuilding the same index twice.
        c->walked_level.clear();
        if (!c->walk->run(asset, e))
            return fail(e.empty() ? "the placement traversal could not resolve the asset" : e);
        c->walked_level = key;
    }
    return serve_instance_rows(c, out, out_max);
}

int bf6_level_lighting_zones(bf6_ctx* c, const char* level,
                             bf6_lighting_zone* out, int out_max,
                             bf6_lighting_zone_stats* stats,
                             char* err, int err_len)
{
    auto fail = [&](const std::string& m) {
        if (err && err_len > 0) std::snprintf(err, (size_t)err_len, "%s", m.c_str());
        return 0;
    };
    if (!c || !level || !*level) return fail("bad arguments");

    if (c->lighting_zones_level != level) {
        std::string e;
        if (!ensure_mounted(c, level, e)) return fail(e);
        if (!ensure_types(c, e)) return fail(e);

        c->lighting_zones.clear();
        c->lighting_zone_rows.clear();
        c->lighting_zone_points.clear();
        c->lighting_zone_stats = bf6::LightingZoneStats();
        if (!bf6::level_lighting_zones(c->src, *c->types, level,
                                      c->lighting_zones,
                                      c->lighting_zone_stats, e))
            return fail(e.empty() ? "the lighting-zone traversal found no level root" : e);
        c->lighting_zones_level = level;

        const size_t n = c->lighting_zones.size();
        c->lighting_zone_rows.resize(n);
        c->lighting_zone_points.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const bf6::LightingZone& z = c->lighting_zones[i];
            bf6_lighting_zone& r = c->lighting_zone_rows[i];
            r = bf6_lighting_zone{};
            r.kind = z.kind == bf6::kZonePolygon
                ? BF6_LIGHTING_ZONE_POLYGON : BF6_LIGHTING_ZONE_OBB;
            for (int k = 0; k < 4; ++k) {
                r.xform[k * 3 + 0] = z.xf.m[k].x;
                r.xform[k * 3 + 1] = z.xf.m[k].y;
                r.xform[k * 3 + 2] = z.xf.m[k].z;
            }
            r.half_extents[0] = z.half_extents.x;
            r.half_extents[1] = z.half_extents.y;
            r.half_extents[2] = z.half_extents.z;
            std::vector<float>& p = c->lighting_zone_points[i];
            p.reserve(z.points.size() * 3);
            for (const bf6::Vec3& v : z.points) {
                p.push_back(v.x); p.push_back(v.y); p.push_back(v.z);
            }
            r.points = p.empty() ? nullptr : p.data();
            r.point_count = (int32_t)z.points.size();
            r.height = z.height;
            r.fade_distance = z.fade_distance;
            r.preset = z.preset.empty() ? nullptr : z.preset.c_str();
            r.source = z.source.empty() ? nullptr : z.source.c_str();
            r.proximity_instance = z.proximity_instance;
            r.shape_instance = z.shape_instance;
        }
    }

    if (stats) {
        const bf6::LightingZoneStats& s = c->lighting_zone_stats;
        *stats = bf6_lighting_zone_stats{};
        stats->total = (int32_t)c->lighting_zone_rows.size();
        for (const bf6_lighting_zone& z : c->lighting_zone_rows)
            z.kind == BF6_LIGHTING_ZONE_POLYGON ? ++stats->polygon : ++stats->obb;
        stats->partitions = (int32_t)s.partitions;
        stats->instances = (int32_t)s.instances;
        stats->proximity = (int32_t)s.proximity;
        stats->shape_links = (int32_t)s.link_edges;
        stats->non_geometry_links = (int32_t)s.non_geometry_links;
        stats->non_shape_geometry_links = (int32_t)s.non_shape_geometry_links;
        stats->target_obb = (int32_t)s.target_obb;
        stats->target_polygon = (int32_t)s.target_polygon;
        stats->target_other = (int32_t)s.target_other;
        stats->joined_preset = (int32_t)s.joined_preset;
        stats->omitted_no_preset = (int32_t)s.omitted_no_preset;
        stats->parse_fail = (int32_t)s.parse_fail;
        stats->missing = (int32_t)s.missing;
        stats->cycles = (int32_t)s.cycles;
        stats->malformed_shape = (int32_t)s.malformed_shape;
        stats->unresolved_types = (int32_t)s.unresolved_types;
        stats->rotated_control_hits = (int32_t)s.rotated_control_hits;
    }

    const int n = (int)c->lighting_zone_rows.size();
    if (out && out_max > 0)
        for (int i = 0; i < n && i < out_max; ++i)
            out[i] = c->lighting_zone_rows[(size_t)i];
    return n;
}

void bf6_free(bf6_ctx* c, void* handle) {
    if (!handle || !c) return;
    auto it = c->handles.find(handle);
    // A pointer this context never handed out is not ours to delete. Better to
    // leak than to run a destructor over memory of unknown type, which is the
    // exact mistake this table exists to stop.
    if (it == c->handles.end()) return;
    const int kind = it->second;
    c->handles.erase(it);
    switch (kind) {
    case bf6_ctx::HK_MESH:    delete reinterpret_cast<MeshHandle*>(handle);    break;
    case bf6_ctx::HK_TERRAIN: delete reinterpret_cast<TerrainHandle*>(handle); break;
    case bf6_ctx::HK_SKELETON: bf6__skeleton_delete(handle); break;
    case bf6_ctx::HK_ADJACENCY: bf6__adjacency_delete(handle); break;
    case bf6_ctx::HK_HAIRBIND: bf6__hairbind_delete(handle); break;
    case bf6_ctx::HK_RENDERBONES: bf6__renderbones_delete(handle); break;
    case bf6_ctx::HK_ANIMRELOC: bf6__animreloc_delete(handle); break;
    case bf6_ctx::HK_ANIMCLIP: bf6__animclip_delete(handle); break;
    case bf6_ctx::HK_PSD: bf6__psd_delete(handle); break;
    case bf6_ctx::HK_PSDMAP: bf6__psdmap_delete(handle); break;
    case bf6_ctx::HK_SWARM: bf6__swarm_delete(handle); break;
    case bf6_ctx::HK_TELEMETRY: bf6__telemetry_delete(handle); break;
    case bf6_ctx::HK_SPAWNS: bf6__spawns_delete(handle); break;
    case bf6_ctx::HK_VSHAPES: bf6__vshapes_delete(handle); break;
    case bf6_ctx::HK_FOOTPRINTS: bf6__footprints_delete(handle); break;
    case bf6_ctx::HK_AUTOPAINT: bf6__autopaint_delete(handle); break;
    case bf6_ctx::HK_ECSSYSTEM: bf6__ecs_system_delete(handle); break;
    case bf6_ctx::HK_DEBRIS: bf6__debris_delete(handle); break;
    case bf6_ctx::HK_WIND: bf6__wind_delete(handle); break;
    case bf6_ctx::HK_WEATHER: bf6__weather_delete(handle); break;
    case bf6_ctx::HK_WAVESEL: bf6__wavesel_delete(handle); break;
    case bf6_ctx::HK_BEHAVIORTREE: bf6__behaviortree_delete(handle); break;
    case bf6_ctx::HK_NETREGISTRY: bf6__netregistry_delete(handle); break;
    case bf6_ctx::HK_UNLOCKS: bf6__unlocks_delete(handle); break;
    case bf6_ctx::HK_GEM: bf6__gem_delete(handle); break;
    case bf6_ctx::HK_SCHEMATIC: bf6__schematic_delete(handle); break;
    case bf6_ctx::HK_PHYSICS: bf6__physics_delete(handle); break;
    case bf6_ctx::HK_OCCLUDER: bf6__occluder_delete(handle); break;
    case bf6_ctx::HK_PMVOLUME: bf6__pmvolume_delete(handle); break;
    case bf6_ctx::HK_LIGHTPROBE: bf6__lightprobe_delete(handle); break;
    default: break;
    }
}

}  // extern "C"


// The water RENDER description. Its own translation unit so the decode
// can grow without this file growing with it. Public definitions inherit C
// linkage from bf6_core.h; private helpers retain ordinary C++ linkage.
#include "water_ext.inc"

// The ocean SEA STATE: the level's authored Beaufort force and the shared
// curve collection that converts it to parameters. Separate from water_ext
// because it reads a different asset and answers a different question.
#include "beaufort_ext.inc"


// The FX decode over the C ABI. Its own translation unit for the same
// reason water_ext.inc is. Public definitions inherit the header's C linkage.
#include "fx_ext.inc"


// The mount's own name tables and raw bytes. Same reason as the two above, and
// Public definitions inherit the header's C linkage.
#include "raw_ext.inc"
#include "expression_ext.inc"
#include "expression_registry_ext.inc"
#include "ebxdump_ext.inc"
#include "ebx_import_ext.inc"
#include "armory_ext.inc" // installed armory and field-upgrade providers
#include "options_ext.inc"
#include "rime_ext.inc"
#include "bones_ext.inc"
#include "skeleton_ext.inc"
#include "chardeform_ext.inc"
#include "anim_ext.inc"
#include "anim_binding_ext.inc"
#include "scatter_ext.inc"
#include "swarm_ext.inc"
#include "telemetry_ext.inc"
#include "spawn_ext.inc"
#include "vshape_ext.inc"
#include "gamemode_ext.inc"
#include "footprint_ext.inc"
#include "autopaint_ext.inc"
#include "debris_ext.inc"
#include "wind_ext.inc"
#include "weather_ext.inc"
#include "wavesel_ext.inc"
#include "gameplay_ext.inc"
#include "physics_ext.inc"
#include "occluder_ext.inc"
#include "pmvolume_ext.inc"
#include "lightprobe_ext.inc"
#include "typecensus_ext.inc"
#include "frontend_ext.inc"

#include "uisound_ext.inc"
