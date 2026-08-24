/* libbf6 - engine-neutral Battlefield 6 decode core.
 *
 * The contract between the decode (which reads and understands the game's own
 * files) and an engine binding (Godot, Unreal) that turns the result into scene
 * objects. Nothing engine-specific crosses this line: vectors are plain floats,
 * buffers are pointer+length, strings are UTF-8. The core resolves EVERYTHING -
 * the mount, Oodle, the DRM lift, material and variation resolution, the
 * placement walk - and hands back baked data. The binding only uploads it,
 * converts coordinates, and parents nodes.
 *
 * A C ABI on purpose: a Godot GDExtension and an Unreal module can both call it
 * without either knowing the other exists, and it stays stable while the C++
 * inside churns.
 *
 * Memory: every pointer this API returns is owned by the core and freed with
 * bf6_free() on the handle the returning call gave you. Do not free members
 * individually. Handles are valid until freed or until bf6_close().
 */
#ifndef BF6_CORE_H
#define BF6_CORE_H

#include <stdint.h>

/* Windows only exports DLL symbols that are explicitly marked. The shared build
 * defines BF6_BUILDING_DLL and every public function is tagged for export; a
 * static build (Unreal linking bf6_core_static) leaves it empty. Consumers that
 * load the DLL at runtime (GetProcAddress) don't need the import side, so this
 * stays a plain export tag rather than the usual export/import dance. */
#if defined(_WIN32) && defined(BF6_BUILDING_DLL)
#  define BF6_API __declspec(dllexport)
#else
#  define BF6_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BF6_ABI_VERSION 1

/* ------------------------------------------------------------------ session */
typedef struct bf6_ctx bf6_ctx;

/* Open an install: mount, read the type schema, and OOA-lift the executable in
 * memory if it is DRM-wrapped (EA App). All storefront divergence lives behind
 * this one call. Returns NULL on failure and writes a reason into err.
 * An empty game_dir is the explicit no-install mode: it always succeeds and
 * returns a context with nothing mounted. Mesh reads fail on it, but the
 * placeable catalogue (bf6_load_placeables and friends) works fully. */
BF6_API bf6_ctx* bf6_open(const char* game_dir, char* err, int err_len);
BF6_API void     bf6_close(bf6_ctx*);

/* True if the type schema had to be decrypted (EA install). For diagnostics. */
BF6_API int      bf6_was_lifted(bf6_ctx*);

/* --------------------------------------------------------------- enumerate */
/* The SDK ships its placeable-object catalogue as JSON (level_info.json +
 * asset_types.json under FbExportData/). Point this at that directory once after
 * bf6_open; it populates the level list and the placeable catalogue below.
 * Returns the number of placeables loaded, 0 on failure (reason in err). Until
 * called, bf6_level_count and bf6_list_placeables report empty. */
BF6_API int bf6_load_placeables(bf6_ctx*, const char* fbexport_dir,
                                char* err, int err_len);

BF6_API int         bf6_level_count(bf6_ctx*);
BF6_API const char* bf6_level_name(bf6_ctx*, int index);      /* e.g. "MP_Badlands" */

/* One SDK-placeable object. Strings point into the ctx and live until close. */
typedef struct {
    const char* type;          /* the placeable's name, e.g. "AAGun_01"     */
    const char* directory;     /* UI category, e.g. "Generic/Common/Props"  */
    const char* mesh;          /* the 'mesh' constant - a resource stem      */
    int32_t     physics_cost;
    int32_t     universal;     /* 1 if allowed on every level (no restriction) */
} bf6_placeable;

/* List the SDK placeables available on `level` - the objects allowed on that
 * level PLUS the universal ones - exactly the Portal object library's per-level
 * set. level NULL/"" lists every placeable. `search` filters by type substring
 * (NULL/"" = all). Returns the TOTAL match count; writes up to out_max. */
BF6_API int bf6_list_placeables(bf6_ctx*, const char* level, const char* search,
                                bf6_placeable* out, int out_max);

/* One editable field the SDK exposes on a placeable (from its properties[]). */
typedef struct {
    const char* name;       /* "CameraFOV", "Team", "ObjId"                   */
    const char* type;       /* "float","int","bool","string","vector",       */
                            /* "selection", or an object-link type like       */
                            /* "PolygonVolume" / "Array[SpawnPoint]"          */
    const char* def;        /* default value as a string, may be ""           */
    const char* selections; /* "selection" enum options, newline-joined; ""   */
                            /* when the field is not an enum                  */
} bf6_prop;

/* List the editable properties of placeable `type` (e.g. a CapturePoint has ~34;
 * a plain prop has just "ObjId"). Returns the TOTAL count; writes up to out_max.
 * Strings point into the ctx and live until close. */
BF6_API int bf6_placeable_props(bf6_ctx*, const char* type,
                                bf6_prop* out, int out_max);

typedef struct {
    const char* res_name;      /* the asset id, e.g. common/.../foo_mesh   */
    const char* category;      /* grouping for a browser, may be ""        */
} bf6_cat_entry;

/* Fill out[] with up to out_max catalogue entries matching `search` (NULL or ""
 * = everything). Returns the number written. */
BF6_API int bf6_catalogue(bf6_ctx*, const char* search,
                  bf6_cat_entry* out, int out_max);

/* ---------------------------------------------------------------- geometry */
typedef struct {
    const float*    positions;   /* xyz * vertex_count                      */
    const float*    normals;     /* xyz * vertex_count, or NULL             */
    const float*    tangents;    /* xyzw * vertex_count, or NULL            */
    const float*    uv0;         /* uv * vertex_count, primary channel      */
    const float*    uv1;         /* uv * vertex_count, secondary, or NULL   */
    const uint32_t* colors;      /* rgba8 * vertex_count, or NULL           */
    const uint32_t* indices;
    int32_t         vertex_count;
    int32_t         index_count;
    int32_t         material;    /* index into bf6_mesh.materials           */
} bf6_section;

typedef struct bf6_material_desc bf6_material_desc;   /* below */

typedef struct {
    const bf6_section*       sections;
    int32_t                  section_count;
    const bf6_material_desc*  materials;
    int32_t                  material_count;
    float                    aabb_min[3];
    float                    aabb_max[3];
} bf6_mesh;

/* Build one asset's geometry. lod 0 = full detail. NULL if unreadable.
 *
 * Materials resolve against the bundle the mesh RESOURCE lives in, which is the
 * right answer for reading a mesh on its own - a browser, a preview - because
 * there is no placement above it to ask. */
BF6_API bf6_mesh* bf6_read_mesh(bf6_ctx*, const char* res_name, int lod);

/* The same, for a mesh that is being PLACED.
 *
 * placing_bundle is the bundle whose placement pulled this mesh in (a walk row
 * carries it). That is the exact scope for a material: a shader state key is
 * unique only within a bundle, so resolving against any other one can bind a
 * material that merely collides - which looks correct and is not.
 *
 * variation is the ObjectVariation asset path from the placement, or NULL. It
 * derives a second key (state key + djb2 of the path, a genuine 64-bit add),
 * and the base key is used when that derived key is absent.
 *
 * Passed rather than held on the context on purpose: the scope belongs to the
 * INSTANCE, and hidden state would make two placements of one mesh race. */
BF6_API bf6_mesh* bf6_read_mesh_scoped(bf6_ctx*, const char* res_name, int lod,
                              const char* placing_bundle, const char* variation);

/* DOES THIS VARIATION CHANGE ANYTHING FOR THIS MESH? 1 when any of the mesh's
 * section keys derives a resolving record in the placing bundle's depot, else
 * 0. The question exists because splitting instance groups by variation is
 * not free - every extra group is another component and another mesh build -
 * so a renderer splits only where the split is EARNED, which is the rule the
 * reference plugin measured its way to. Cached on the context. */
BF6_API int bf6_variation_live(bf6_ctx*, const char* res_name,
                               const char* placing_bundle, const char* variation);

/* ---------------------------------------------------------------- material */
typedef enum {
    BF6_TEX_ALBEDO = 0,
    BF6_TEX_NORMAL,
    BF6_TEX_MRO,          /* metallic / roughness / occlusion, packed       */
    BF6_TEX_EMISSIVE,
    BF6_TEX_MASK,
    BF6_TEX_SLOT_COUNT
} bf6_tex_slot;

typedef struct {
    bf6_tex_slot slot;
    int32_t      texture;      /* index for bf6_texture_at(), or -1          */
} bf6_tex_binding;

struct bf6_material_desc {
    const bf6_tex_binding* textures;
    int32_t                texture_count;
    float                  base_color[4];
    float                  emissive[3];
    float                  roughness;
    float                  metallic;
    int32_t                two_sided;   /* 0/1 */
    /* The shader's OWN alpha-test switch, not a guess from the textures. A
     * depot record carries a bool that says whether it cuts out; a prop with no
     * cutout of its own is sometimes handed another prop's mask in the alpha
     * slot, and cutting by that shreds the surface. Checked before any test of
     * the mask's content. */
    int32_t                alpha_test;  /* 0/1 */
    /* Classified from the bindings, not from names: glass is the
     * destruction-glass-volume slot or the glass-tint palette. */
    int32_t                translucent; /* 0/1 */
    /* Vegetation carries its cutout in its OWN base colour's alpha (the "_cu"
     * sheet), not in a separate mask. Everything else that cuts out ships a
     * single-channel "_a" sheet, because a normal "_cs" base colour's alpha is
     * SMOOTHNESS. A consumer has to mask from a different place for the two,
     * so the record says which. */
    int32_t                alpha_from_albedo; /* 0/1 */
    /* The far-LOD impostor's "_nsm" packing on the NORMAL binding, from the
     * recovered Aftermath M_Vista pixel shader: RG is the tangent normal
     * (reconstruct Z), B is WETNESS response and A is SMOOTHNESS. It is not a
     * metallic map, and roughness comes from 1 - A. When this is set, light
     * the surface with specular INDEPENDENT of albedo, the way the game's
     * separate G-buffer outputs do - a baseColor * (diffuse + specular)
     * composition makes the sheet's authentic dark texels impossible to
     * light and reads as corruption. */
    int32_t                normal_is_nsm; /* 0/1 */
};

/* ---------------------------------------------------------------- textures */
typedef enum {
    BF6_FMT_RGBA8 = 0,
    BF6_FMT_BC1,   BF6_FMT_BC3,   BF6_FMT_BC4,
    BF6_FMT_BC5,   BF6_FMT_BC7,
    /* Added rather than reusing a slot: skies and light probes are BC6H, and a
     * binding that quietly reported them as BC7 would upload garbage. */
    BF6_FMT_BC6H_U, BF6_FMT_BC6H_S,
    BF6_FMT_R8,     BF6_FMT_RGBA16F,
    BF6_FMT_UNKNOWN = 255
} bf6_fmt;

typedef struct {
    int32_t        width;
    int32_t        height;
    int32_t        mip_count;
    bf6_fmt        format;
    const uint8_t* data;       /* all mips, tightly packed, GPU-ready        */
    int32_t        data_len;
    int32_t        srgb;       /* 0/1 */
} bf6_texture;

/* Textures are handed across COMPRESSED (BCn) so the engine uploads them as-is.
 * texture_id comes from a material's bf6_tex_binding. Owned by the ctx. */
BF6_API const bf6_texture* bf6_texture_at(bf6_ctx*, int texture_id);

/* --------------------------------------------------------------- placements */
typedef struct {
    const char* res_name;      /* the mesh to instance                       */
    float       xform[12];     /* 3x4 row-major: basis columns then origin,  */
                               /* in the GAME's space - the binding converts */
    int32_t     material_scope;/* pre-resolved variation key, for caching    */
    /* The bundle whose placement pulled this mesh in, and the ObjectVariation
     * path if it has one. Pass BOTH to bf6_read_mesh_scoped: they are what
     * makes a material resolve in the right scope, and they belong to the
     * INSTANCE rather than to the mesh. Owned by the ctx, valid until the next
     * bf6_open_level. */
    const char* placing_bundle;
    const char* variation;
} bf6_instance;

/* ---------------------------------------------------------------- progress */
/* Called from inside the long calls so a caller can show something moving.
 * `stage` is a short label ("mounting", "indexing partitions", "walking");
 * done/total are that stage's own counts, and total may be 0 when it is not
 * known yet.
 *
 * CALLED FROM WHATEVER THREAD IS DOING THE WORK, including several at once
 * during indexing, so an implementation must be safe to call concurrently and
 * must NOT touch a UI directly. Store the numbers and let the UI thread read
 * them. Return 0 to ask the operation to stop.
 */
typedef int (*bf6_progress_fn)(void* user, const char* stage, int done, int total);

BF6_API void bf6_set_progress(bf6_ctx*, bf6_progress_fn, void* user);

/* Mount a level's archives and read the type schema, which every placement
 * call needs. all_levels also mounts every OTHER level, which is what makes the
 * whole placeable catalogue resolvable and is not free. Returns 0 on success,
 * with a message in err.
 *
 * Expensive and cached on the context: mounting is a few seconds and indexing
 * every partition's guid is a few more. Call it once per level. */
BF6_API int bf6_open_level(bf6_ctx*, const char* level, const char* exe_path,
                   int all_levels, char* err, int err_len);

/* Every placement in a level. Returns the count; if it exceeds out_max, out[]
 * is filled to out_max and the return value tells you to call again bigger.
 * bf6_open_level must have been called for this level first.
 *
 * res_name points into storage owned by the context and stays valid until the
 * next bf6_open_level. */
BF6_API int bf6_level_instances(bf6_ctx*, const char* level,
                        bf6_instance* out, int out_max);

/* ------------------------------------------------------------------- lights */
typedef enum { BF6_LIGHT_POINT = 0, BF6_LIGHT_SPOT, BF6_LIGHT_LINE } bf6_light_type;

typedef struct {
    bf6_light_type type;
    float          xform[12];
    float          color[3];
    float          intensity;
    float          range;
    float          spot_inner;   /* radians, 0 for non-spot */
    float          spot_outer;
    const char*    ies_profile;  /* goniometric ref, or NULL */
} bf6_light;

BF6_API int bf6_level_lights(bf6_ctx*, const char* level,
                     bf6_light* out, int out_max);

/* ------------------------------------------------------------------ terrain */
typedef struct {
    int32_t         width;         /* samples per side, from the tree itself */
    int32_t         height;
    const uint16_t* heights;       /* row-major, width*height samples */
    float           world_min[3];  /* the AABB the grid spans, GAME space */
    float           world_max[3];
    float           height_scale;  /* the header's height scale             */
    int32_t         splat_texture; /* index for bf6_texture_at(), or -1     */
    int32_t         color_texture;
} bf6_terrain;

BF6_API bf6_terrain* bf6_read_terrain(bf6_ctx*, const char* level);

/* ------------------------------------------------------------ terraindecals */
/* Roads and street markings.
 *
 * The street SURFACE is the terrain heightfield - block 7 paints it asphalt -
 * so a reader without this still draws ground where a road is. What it does not
 * draw is any of what makes a road read as one: lane markings, mud, wear, tyre
 * tracks, kerb blending. That is what "the roads are giant empty spaces"
 * describes.
 *
 * A decal vertex carries world X and Z and NO Y: decals are draped on the
 * heightfield by the consumer, which must sample the terrain at (x, z).
 *
 * THE AABB IS A BAND, NOT A SURFACE. Its Y range is where the authored
 * geometry sat, and it is worth exactly one thing: telling an ELEVATED record
 * (a rooftop court, a loading deck) from a street-level one. Clamping every
 * record into its band is a trap - where the rebuilt ground sits lower than
 * the ground the decal was compiled against, a clamp cannot follow the terrain
 * down and the marking hangs in the air. Clamp only where the authored floor
 * stands well above the ground you actually built under that record. */
typedef struct {
    /* x, z, u, v, r, g, b, a per vertex - stride 8 floats. The list is NON
     * INDEXED: vertex_count is exactly tri_count * 3, in triangle order. */
    const float* verts;
    int32_t      vertex_count;
    float        aabb_min[3];
    float        aabb_max[3];
    /* World metres per tile. A planar fill stores u = world X and v = world Z
     * verbatim and tiles by these; everything else authors u across the ribbon
     * (0..1 over tiling1) and v as arc length along it, already divided by
     * tiling0. */
    float        tiling0, tiling1;
    int32_t      planar;        /* 0/1 */
    /* Texture ids for bf6_texture_at, or -1. THE MARKINGS LIVE IN opacity -
     * a lane stripe is coverage, not colour, and a consumer that binds only
     * the base colour draws the asphalt and none of the paint. */
    int32_t      albedo, opacity, normal;
} bf6_decal;

/* Every decal record in a level. Same convention as bf6_level_instances:
 * returns the count, fills out[] to out_max. The vert pointers are owned by the
 * context and stay valid until the next bf6_open_level. */
BF6_API int bf6_level_decals(bf6_ctx*, const char* level, bf6_decal* out, int out_max);

/* ------------------------------------------------------------------- water */
/* The level's water surfaces, from its own water entities: flat planes at an
 * absolute height, with the colours the level's depot record authors for them.
 *
 * A consumer draws each as a horizontal plane of size[0] x size[1] metres
 * centred at (center[0], height, center[1]) in the GAME's coordinates. Where
 * the record carried no colour, shallow[0] is negative and a preset is the
 * honest fallback. is_ocean says which shader family the level binds - the
 * ocean variant authors ONE colour and the darker "deep" is absent by design
 * (a consumer derives depth by darkening, and writing the same colour into
 * both would flatten the gradient while still looking like mined data). */
typedef struct {
    float   center[2];     /* world X, Z of the plane's centre               */
    float   size[2];       /* metres                                        */
    float   height;        /* world Y of the surface                        */
    float   shallow[3];    /* linear; [0] < 0 when the record had no colour */
    float   deep[3];       /* linear; [0] < 0 when absent (always on ocean) */
    int32_t is_ocean;      /* 0/1                                           */
    /* THE SHEETS THE GAME'S OWN WATER BINDS, for bf6_texture_at, or -1.
     * Colour alone gives a flat pane; these are what make it read as water:
     * detail_normal is the micro-ripple normal the surface is covered in,
     * foam_normal and foam_rgb are the foam sheets (R patches, G bubbles,
     * B crest streaks), and noise/perlin drive the break-up. They come off
     * the same depot record as the colours. */
    int32_t detail_normal;
    int32_t foam_normal;
    int32_t foam_rgb;
    int32_t noise;
    int32_t perlin;
} bf6_water;

/* Requires bf6_open_level for this level first (the scan needs the mounted
 * archives, the type schema and the walk's root). Same convention as
 * bf6_level_instances: returns the count, fills out[] to out_max. */
BF6_API int bf6_level_water(bf6_ctx*, const char* level, bf6_water* out, int out_max);

/* The ocean simulation's INPUTS - the authored sea state. The wave field
 * itself is a runtime GPU simulation and nothing on disk holds it; these are
 * the numbers that drive it, read from WaterOceanSimulationEntityData in the
 * level's schematic partitions (the flagged instance wins, else the first -
 * tungsten's only instance is NOT flagged, so "flagged only" loses the most
 * oceanic map in the game).
 *
 * Units, as far as the corpus settles them: wind_angle is radians with an
 * UNRESOLVED axis convention; wind_speed is a normalised authoring scalar
 * (0.01 calm, 0.07 windy, 0.30+ the D-Day sea), NOT m/s. The distribution is
 * wave ENERGY BY DIRECTION: control points over x 0..1 = a full turn around
 * wind_angle, y = relative energy. */
typedef struct {
    float   wind_angle;
    float   wind_speed;
    float   choppiness;
    float   tile_dimension;
    float   min_wavelength;
    float   large_wave_reduction;
    float   wave_thickness;
    int32_t foam_enable;
    float   foam_threshold;
    float   foam_max;
    int32_t enabled;          /* 1 = the flagged instance; 0 = first fallback */
    int32_t dist_count;       /* 0, 5, 9 or 13 control points */
    float   dist_x[13];       /* ascending, last implicitly 1.0              */
    float   dist_y[13];
} bf6_water_sim;

/* Requires bf6_open_level. Returns 1 and fills out when the level has a sim
 * entity, else 0. */
BF6_API int bf6_level_water_sim(bf6_ctx*, const char* level, bf6_water_sim* out);

/* ------------------------------------------------------------ ground bake */
/* The terrain's real ground materials, composited the way the game's own
 * ComputeLayer evaluator does it: per-texel layer coverage from the splat,
 * each layer's sheet sampled in world space at its authored tiling, blended
 * in ascending layer order, with the colour map applied as an Overlay.
 *
 * This is a BAKE, not a live shader. A renderer drapes the result over the
 * heightfield and gets ground that matches the game rather than flat grey.
 *
 * The window is in world XZ metres. rect_size <= 0 bakes the whole footprint,
 * which on a 4 km map means a metre per texel at 4096 - the pages are far
 * denser than that, so a window is how real detail is recovered. */
typedef struct {
    float   rect_min[2];
    float   rect_size;      /* <= 0 = the whole map */
    int32_t size;           /* output texels per side, 0 -> 1024 */
    int32_t want_normal;    /* 0/1 */
    int32_t stochastic;     /* 0/1, the shader's own repetition breakup */
    int32_t colour_map;     /* 0/1, the per-layer Overlay blend */
} bf6_terrain_bake_opts;

typedef struct {
    int32_t        size;
    float          lo[2];   /* world XZ of the low corner */
    float          hi[2];
    float          metres_per_texel;
    /* RGBA8, size*size*4, sRGB-ENCODED. Owned by the context, valid until
     * the next bake or bf6_close. */
    const uint8_t* albedo;
    /* RGBA8 or NULL. RGB is the tangent-space layer normal as 0.5n+0.5; A is
     * composite height over -1..+1 m. This is NOT the final world normal -
     * the game finishes by combining it with the heightfield's own normal,
     * which a renderer holding terrain.h data can do better itself. */
    const uint8_t* normal;
    int32_t        layers_used;
    int32_t        layers_textured;
    float          fallback_fraction;   /* texels no textured layer reached */
} bf6_terrain_bake;

/* Requires bf6_open_level. Returns 1 on success. */
BF6_API int bf6_bake_terrain(bf6_ctx*, const char* level,
                             const bf6_terrain_bake_opts* opts,
                             bf6_terrain_bake* out, char* err, int err_len);

/* ---------------------------------------------- ground coverage (per pixel) */
/* The ground as WEIGHTS plus a material list, for a renderer that blends per
 * pixel rather than consuming a flattened bake.
 *
 * bf6_bake_terrain flattens the ground into one albedo raster, which is the
 * right answer for a thumbnail and the wrong one for a viewport: a whole-map
 * raster lands at two to four metres a texel while the ground materials
 * themselves repeat every one to seven metres, so flattening averages every
 * material away before the renderer sees it and the result reads as a
 * low-resolution photograph of ground.
 *
 * This carries the coverage instead - which varies slowly and rasterises
 * happily at a couple of metres - and leaves the materials to be sampled per
 * pixel at their own tiling. The detail then comes from the sheets at full
 * resolution and only the mixing weights are baked. */
typedef struct {
    int32_t     layer;              /* the layer index this came from       */
    const char* albedo_res;         /* texture resource, "" when unbound    */
    const char* normal_res;
    float       metres_per_repeat;  /* world metres per texture repeat      */
    float       uv_rotation_deg;
    float       tint[3];
} bf6_ground_material;

typedef struct {
    int32_t        size;
    float          lo[2];           /* world XZ of the low corner, metres   */
    float          hi[2];
    /* size*size*4 each, owned by the context. idx indexes MATERIALS, not raw
     * layer ids, and 255 means "no layer here"; w is that slot's weight,
     * weight-sorted with the first zero ending the list. Sample idx with
     * POINT filtering - a bilinear read of an index is a different index. */
    const uint8_t* idx;
    const uint8_t* weight;
    const bf6_ground_material* materials;
    int32_t        material_count;
    float          empty_fraction;
} bf6_ground_coverage;

/* Requires bf6_open_level. size 0 -> 2048. Returns 1 on success. */
BF6_API int bf6_ground_coverage_get(bf6_ctx*, const char* level, int size,
                                    bf6_ground_coverage* out,
                                    char* err, int err_len);

/* -------------------------------------------------------------------- memory */
/* Free anything this API returned (bf6_mesh*, bf6_terrain*, ...). The bf6_ctx*
 * itself is freed by bf6_close(), not this. */
BF6_API void bf6_free(bf6_ctx*, void* handle);

#ifdef __cplusplus
}
#endif
#endif /* BF6_CORE_H */
