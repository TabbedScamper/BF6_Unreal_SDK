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

#define BF6_ABI_VERSION 3

/* ------------------------------------------------------------------ session */
typedef struct bf6_ctx bf6_ctx;

/* Runtime contract check for dynamically loaded consumers. A binding must
 * compare this result with BF6_ABI_VERSION before resolving or calling any
 * struct-bearing API; this catches a current DLL paired with a stale copied
 * header, which otherwise compiles and can corrupt memory without an error. */
BF6_API int bf6_abi_version(void);

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
    /* PER-VERTEX BONE / PART INDEX, or NULL.
     *
     * The BoneIndices element (vertex usage 2). What it MEANS depends on the
     * mesh kind, and reading it without knowing which is a real error:
     *   Skinned (weapons)  - a slot in this section's bone palette, which
     *                        resolves through `bone_list` to a skeleton bone.
     *                        The bolt, ejection cover, charging handle and
     *                        trigger are bones, not separate meshes, so a
     *                        consumer that ignores this draws them wherever
     *                        their bone-local origin happens to fall.
     *   Rigid/Composite    - a destruction part index, a different and
     *                        differently sized index space entirely.
     * Decoded all along; it simply never crossed this ABI, the same way uv1
     * did not. */
    const uint16_t* bones;
    /* This section's bone palette: bone_list[i] is the SKELETON bone id for
     * palette slot i. NULL on a mesh that carries no palette. */
    const uint16_t* bone_list;
    int32_t         bone_list_count;
    /* Authored MeshSubsetCategory_TransparentDecal membership. Per section,
     * never inferred from an asset or material name. */
    int32_t         is_decal;
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

/* Armory-specialized form of the same read. It resolves material FILE guids
 * through the bounded runtime armory index, avoiding a scan of unrelated level
 * partitions. Geometry and bundle scoping are otherwise identical. */
BF6_API bf6_mesh* bf6_read_armory_mesh_scoped(bf6_ctx*, const char* res_name, int lod,
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
    /* The depot record binds the terrain virtual-texture colour slot
     * 0x89D3AD5E and has no usable albedo of its own. These are the authored
     * M_TerrainBlend surfaces that receive terrain decals. */
    int32_t                terrain_decal_receiver; /* 0/1 */
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
    /* The 33-cubed VisualEnvironment grading LUT. Appended so the values of
     * every previously published format remain ABI-stable. */
    BF6_FMT_RGB10A2,
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

/* Same exact texture resource and authored mip chain, capped for an on-screen
 * preview. For streamed textures this deliberately reads the embedded mip tail
 * rather than isolated mip0, which makes the cap effective. Context-owned. */
BF6_API const bf6_texture* bf6_texture_at_max_dim(bf6_ctx*, int texture_id,
                                                  int max_dimension);

/* The live resource name behind a texture id returned by any read path.
 * Context-owned, stable until bf6_close. This is metadata, not an exported
 * intermediate: consumers use it to select material behaviour from the
 * current install (for example road paint versus track wear) without shipping
 * a per-patch id table. */
BF6_API const char* bf6_texture_name_at(bf6_ctx*, int texture_id);

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

/* ------------------------------------------------------- local light placements
 *
 * THE MAP'S ARTIFICIAL LIGHT: every lamp, spotlight, ceiling fixture and
 * emissive panel it places. bf6_level_lighting is the sun and the sky; this is
 * everything else, and on an urban map it is thousands of fixtures. A level
 * imported with a sun and none of these is lit at midday whatever its preset
 * says, because ALL of its artificial light is here.
 *
 * These are found by walking the level graph, not by reading its lighting
 * partitions: sampling lay_art_*_lighting.ebx and _layers_content/lighting.ebx
 * directly returns zero light entities, because almost every light sits inside
 * a placed fixture prefab. The call therefore does its own traversal and takes
 * tens of seconds on a big map, cached per level like the placement walk.
 *
 * Only bf6_open plus a mount is needed - bf6_open_level's placement walk does
 * NOT have to have run. */
typedef enum {
    BF6_LIGHT_SPHERE = 0,   /* PbrSphereLight: a point light with a real radius */
    BF6_LIGHT_SPOT   = 1,   /* PbrSpotLight                                     */
    BF6_LIGHT_TUBE   = 2,   /* PbrTubeLight: a line or capsule emitter          */
    BF6_LIGHT_RECT   = 3,   /* PbrRectangularLight: an area emitter             */
    BF6_LIGHT_OTHER  = 4    /* a declared light class carrying no light fields   */
} bf6_light_type;

/* LightUnitType, the engine's own two-member enum. The two are DIFFERENT
 * PHYSICAL QUANTITIES and cannot share a conversion constant: a consumer must
 * branch on `unit` before scaling anything. Measured on mp_dumbo, 11,634 of
 * 11,640 lights are LuminousPower, so lumens is the case that matters. */
typedef enum {
    BF6_LIGHT_UNIT_LUMINOUS_POWER = 0,  /* lumens (lm), total emitted flux      */
    BF6_LIGHT_UNIT_LUMINANCE      = 1   /* candela per square metre (nits)      */
} bf6_light_unit;

/* QualityScalableEnabled: the LOWEST graphics preset at which the feature is
 * on. NOT a boolean - reading it as one turns "Ultra only" into "always". */
typedef enum {
    BF6_QSE_LOW = 0, BF6_QSE_MEDIUM = 1, BF6_QSE_HIGH = 2,
    BF6_QSE_ULTRA = 3, BF6_QSE_DISABLED = 4
} bf6_quality_scalable;

typedef struct {
    int32_t type;           /* bf6_light_type                                  */

    /* World transform in the GAME's space, 3x4 ROW-MAJOR: rows 0..2 are the
     * basis right/up/forward and row 3 is the translation in metres. Identical
     * in shape and order to bf6_instance.xform, so a consumer that already
     * places props places these the same way.
     *
     * A SPOT SHINES ALONG MINUS ROW 2 (minus forward). Measured off fixtures
     * whose real aim is not in question - recessed ceiling downlights and
     * street lamps 7 to 10 m up their poles all carry forward = (0, +1, 0) -
     * and across mp_dumbo's 4,470 spots, minus-forward points 3,688 of them
     * downwards while plus-forward points 36. Get the sign wrong and every
     * cone in the map aims at the ceiling it is set into.
     *
     * THE BASIS ROWS CARRY THE HOLDER'S SCALE and are not unit vectors -
     * measured scales of 1.2 and 1.5 are ordinary. Two consequences: normalise
     * row 2 before using it as a direction, and an AREA light's world size is
     * its authored rect_height / tube_width TIMES that scale. A rect light
     * authored 1 m square inside a prefab scaled 1.5 is 1.5 m square in the
     * world. */
    float   xform[12];

    float   color[3];       /* LINEAR, as authored. 96% are non-white          */
    float   intensity;      /* in `unit`; RAW, spanning eight orders of magnitude */
    int32_t unit;           /* bf6_light_unit                                  */
    float   dimmer;         /* authored multiplier on intensity, usually 1     */

    float   attenuation_radius;  /* metres, the light's reach                  */
    float   attenuation_offset;  /* metres, an inner offset on the falloff     */

    /* Shape. Which of these is authored depends on `type`; the rest are 0.
     * BOTH ANGLES ARE THE FULL CONE IN DEGREES, not half-angles and not
     * radians: mp_dumbo's spots run 40 to 179 degrees, and halving is the
     * consumer's job if its renderer wants a half-angle. */
    float   inner_angle;    /* spot                                            */
    float   outer_angle;    /* spot, and rect when its shape is a frustum      */
    float   shape_radius;   /* metres: sphere SphereRadius / spot DiscRadius /
                             * tube TubeRadius. The size of the EMITTER, which
                             * is what softens a shadow - not the reach.       */
    float   tube_width;     /* tube, metres end to end                         */
    int32_t is_capsule;     /* tube                                            */
    float   rect_height;    /* rect, metres                                    */
    float   rect_aspect;    /* rect. THERE IS NO Width FIELD on the class:
                             * the width is height * aspect.                   */
    int32_t rect_shape;     /* RectangularLightShape: 0 Rect, 1 Frustum,
                             * 2 OrthoFrustum                                  */

    int32_t cast_shadows_enable;  /* bool                                      */
    int32_t cast_shadows;         /* bf6_quality_scalable                      */
    int32_t cast_volumetric;      /* bf6_quality_scalable                      */
    float   volumetric_scattering;
    int32_t affect_diffuse;       /* bool                                      */
    int32_t affect_specular;      /* bool                                      */
    int32_t affect_radiosity;     /* bool                                      */
    int32_t emissive_shape_enable;/* bool: the emitter draws its own shape     */

    /* Goniometry. ies_profile is the partition name of an IesProfileAsset, or
     * NULL; its resource is a 128x128 equal-solid-angle candela grid carrying
     * absolute peak-candela and lumen scalars. */
    const char* ies_profile;
    float       ies_multiplier;
    int32_t     ies_as_mask;
    const char* texture;    /* rect only: the atlas texture it projects, or NULL */

    float   cull_distance;  /* metres                                          */
    float   fade_distance;  /* metres                                          */

    const char* source;     /* the partition the light was found in            */
    uint32_t    flags;      /* the entity's Flags word. MEANING UNDECODED      */
    int32_t     from_component; /* 1 = placed by its own spatial component     */

    /* All const char* above point into storage owned by the context and stay
     * valid until the next bf6_level_lights for a DIFFERENT level. */
} bf6_light;

/* Counters, so a bad read is visible rather than plausible. A light count of 0
 * or of tens of thousands is a failure, not a result; `placed_at_holder` is the
 * one that catches the classic bug, where lights come out in the right number
 * and all sit on their holders' origins. */
typedef struct {
    int32_t total;
    int32_t sphere, spot, tube, rect, other;
    int32_t placed_by_component;    /* transform came from the spatial component */
    int32_t placed_by_own_transform;/* the entity's own Transform, non-identity   */
    /* Neither: the entity's Transform is identity and no component claimed it,
     * so it sits exactly on its holder. NOT a failure on its own - a light
     * authored straight into a subworld legitimately sits on the holder placed
     * for it, and shipped maps run 1% (dumbo) to 16% (subsurface). It IS the
     * failure when it is most of the map and those lights pile at the world
     * origin, which is the component join having broken. */
    int32_t placed_at_holder;
    int32_t components;             /* placement components seen                  */
    int32_t comp_unlinked;          /* component whose Light pointer went nowhere */
    int32_t comp_excluded;          /* authored Excluded, dropped                 */
    int32_t partitions;             /* partitions the traversal opened            */
    int32_t unresolved_types;       /* types the schema could not describe        */
    /* The OLD join, measured beside the real one. Earlier readers follow field
     * 0x11F57ECA as "an int that holds a pointer"; under the multiplayer schema
     * it is a genuine four-byte Enum(PBRAnalyticLightShape) at a different
     * offset, and it only ever resolved because those readers load the
     * SINGLE-PLAYER executable, whose table puts 0x11F57ECA at the offset where
     * the MP data keeps the real pointer. Expect resolved to be small and agree
     * to be 0 on a correct MP-schema read. */
    int32_t comp_legacy_resolved, comp_legacy_agree;
} bf6_light_stats;

/* Every light on the level. Same convention as bf6_level_instances: returns the
 * count, fills out[] to out_max, so a caller sizes its buffer by calling once
 * with out=NULL. `stats` may be NULL. Returns 0 with a message in err on
 * failure - and 0 lights on a map that has fixtures IS a failure. */
BF6_API int bf6_level_lights(bf6_ctx*, const char* level,
                             bf6_light* out, int out_max,
                             bf6_light_stats* stats, char* err, int err_len);

/* -------------------------------------- local VisualEnvironment trigger zones
 * A VisualEnvironment preset does not carry its own bounds. These are the
 * exact shapes connected from AreaProximityEntityData.Geometry in the owning
 * blueprint, transformed through the placed level graph. Only zones whose
 * property/event graph reaches a concrete VisualEnvironmentReferenceObjectData
 * preset are returned. Channel-routed zones are counted in `omitted_no_preset`
 * and deliberately omitted until their channel-to-preset join is decoded. */
enum bf6_lighting_zone_kind {
    BF6_LIGHTING_ZONE_OBB = 0,
    BF6_LIGHTING_ZONE_POLYGON = 1
};

typedef struct {
    int32_t kind;              /* bf6_lighting_zone_kind */
    float   xform[12];         /* world transform, same row layout as instances */
    float   half_extents[3];   /* OBB local half extents; zero for polygon */
    /* Polygon vertices as point_count triples, local to xform. Context-owned,
     * valid until this function is called for another level or bf6_close. */
    const float* points;
    int32_t point_count;
    float   height;            /* polygon extrusion along local +Y */
    float   fade_distance;     /* authored AreaProximity ProximityDistance */
    const char* preset;        /* exact imported VE partition */
    const char* source;        /* owning blueprint partition */
    int32_t proximity_instance;
    int32_t shape_instance;
} bf6_lighting_zone;

typedef struct {
    int32_t total, obb, polygon;
    int32_t partitions, instances, proximity;
    int32_t shape_links;
    int32_t non_geometry_links;
    int32_t non_shape_geometry_links;
    int32_t target_obb, target_polygon, target_other;
    int32_t joined_preset, omitted_no_preset;
    int32_t parse_fail, missing, cycles, malformed_shape, unresolved_types;
    /* Negative control: source instance rotated by one while target is held
     * fixed. A real join should dominate this fabricated pairing. */
    int32_t rotated_control_hits;
} bf6_lighting_zone_stats;

/* Count/fill convention. Mounts the level and reflection schema on demand.
 * Returns 0 with err set on failure; stats may be NULL. */
BF6_API int bf6_level_lighting_zones(bf6_ctx*, const char* level,
                                     bf6_lighting_zone* out, int out_max,
                                     bf6_lighting_zone_stats* stats,
                                     char* err, int err_len);

/* ------------------------------------------------------------------ effects */
/* One GPU-exposed parameter. `type` is 0 Float, 1 Vec2, 2 Vec3, 3 Vec4, 4 Bool,
 * 5 Int, and IT IS THE READ RULE: only the first width(type) components of v[]
 * are meaningful, and a Bool or Int carries its value in `ivalue`, never in the
 * floats. Reading all four back gives a Float parameter as (180, 1, 1, 1) where
 * only 180 was written.
 *
 * `cb_offset` is the parameter's float offset in the emitter constant buffer,
 * and it tiles exactly: cb[k+1] == cb[k] + width(type[k]) held for 31,039 of
 * 31,039 consecutive pairs across every shipped emitter template. That is what
 * lets a consumer lay the buffer out for all 2,524 observed PropertyIds without
 * knowing what any of them are called. A LAYER'S OVERRIDE TABLE CARRIES
 * cb_offset == 0 THROUGHOUT - it patches a value and leaves the layout to the
 * template - so the merged table here keeps the template's offset. */
typedef struct {
    uint32_t pid;          /* djb2-xor of the parameter name                   */
    int32_t  type;
    int32_t  cb_offset;
    float    v[4];
    int32_t  ivalue;       /* the value when type is Bool or Int               */
} bf6_fx_param;

/* One placed EMITTER LAYER. Not one effect: a layer is the unit that renders -
 * its own sheet, its own spawn rate, its own parameters, its own offset - and
 * 1,368 distinct effects on 14 retail levels expand to 3,983 layers. Merging
 * them per effect throws away exactly the variation that makes an explosion
 * read as an explosion (BaseSize 100/200/35 and Drag 0.5/0.0/0.7/0.01 on ONE
 * effect) and leaves a generic puff. */
typedef struct {
    const char* effect;         /* fx_* leaf name                              */
    const char* effect_path;    /* its partition path, no .ebx                 */
    int32_t     placements;     /* how many times the level places it          */
    float       effect_cull_distance;
    int32_t     effect_max_instances;

    int32_t     layer;          /* EBX instance index inside the effect        */
    const char* graph;          /* eg_* emitter template path                  */
    /* billboard_globalsorting | volumedecal | sparks | debris_mesh | distortion
     * | creature | lensflare | ribbon | other. From the graph path alone, so it
     * is independent of lighting_model and the two can be cross-tabulated. */
    const char* family;
    float       local[12];      /* the layer's offset inside the effect        */

    /* THE SHEET. NULL for the families that draw no flipbook, which is 1,406 of
     * 3,983 shipped layers and is a classification, not a miss. */
    const char* atlas;          /* AtlasTextureAsset partition, or NULL        */
    const char* atlas_res;      /* the RES the rid resolved to, or NULL        */
    const char* atlas_chunk;    /* its pixel chunk guid, raw hex, or NULL      */
    /* 1 when the sheet came from the LAYER'S own binding and 0 when it came
     * from the template's GlobalSorting. Measured over 3,983 layers: 2,322
     * override, 255 template - a reader that implements only the template path
     * resolves 255 of 2,577 and calls the rest textureless. */
    int32_t     atlas_from_override;
    int32_t     atlas_cols;     /* AUTHORED, never from the filename           */
    int32_t     atlas_frames;   /* total frames; t_..._7x144_d authors 49       */
    int32_t     atlas_left_right;
    uint64_t    atlas_rid;      /* the ResourceId the RES is found BY, not name */
    int32_t     atlas_width, atlas_height, atlas_mips;

    /* Authored per layer, in the layer's own parameter table. NOT in the draw
     * config: 267 distinct draw-config signatures over 565 templates and not
     * one of their 21 booleans separates Emissive from GnomonLit. -1 means the
     * layer authors none. */
    int32_t lighting_model;     /* 0 Emissive 1 VertexLit 2 GnomonLit          */
    int32_t alignment;          /* 0 Screen 1 Directional 2 ScreenStretch
                                 * 3 FullRotation 4 Emitter 5 Up               */

    const char* spawn_mode;     /* "SpawnModeContinuous" | "SpawnModeBurst"    */
    int32_t has_spawn_rate;     /* burst has no such field AT ALL              */
    float   spawn_rate;         /* particles per second                        */
    int32_t particle_max;
    float   particle_life, emitter_life;
    float   gpu_cull_distance;  /* the REAL cull distance                      */
    /* Every float above whose engine type is QualityScalableFloat is its LOW
     * member. Four of the five are identical across all four quality tiers on
     * all 374 shipped templates; max_spawn_distance is not - 39 of 374 author
     * Low 40 / Medium 60 / High 80 / Ultra 100. */
    float   min_spawn_distance, max_spawn_distance;
    float   preroll_time;
    int32_t draw_layer, draw_pass, sort_mode;
    int32_t particle_type;      /* 0 Quad, 1 Ribbon, -1 no global sorting      */

    /* The template's defaults with this layer's overrides applied on top.
     * Points into storage owned by the context. */
    int32_t             param_count;
    const bf6_fx_param* params;

    /* Every const char* above points into storage owned by the context and
     * stays valid until the next bf6_level_fx for a DIFFERENT level. */
} bf6_fx_layer;

/* Counters, so a bad read is visible rather than plausible. atlas_resolved
 * BELOW layers is normal and expected; atlas_resolved == atlas_template is the
 * signature of a reader that skipped the override slot. */
typedef struct {
    int32_t placements, distinct_effects, effect_failed;
    int32_t layers, graph_resolved;
    int32_t atlas_resolved, atlas_override, atlas_template;
    int32_t grid_resolved, rid_present, res_resolved, chunk_named;
    int32_t lighting_model, alignment;
} bf6_fx_stats;

/* Every emitter layer the level places, one row each. Requires bf6_open_level
 * for this level first: the effect placements come from that walk, and they
 * have to, because it is what stops at the destruction branch and at
 * StaticModelGroup members. A hand-rolled descent that skips those returned
 * 14,576 placements on mp_dumbo against the correct 1,010.
 *
 * Same convention as bf6_level_instances: returns the count and fills out[] to
 * out_max, so a caller sizes its buffer by calling once with out=NULL. `stats`
 * may be NULL. Returns 0 with a message in err on failure. */
BF6_API int bf6_level_fx(bf6_ctx*, const char* level,
                         bf6_fx_layer* out, int out_max,
                         bf6_fx_stats* stats, char* err, int err_len);

/* Where one effect is placed. Fills out_xf with count*12 floats (rows right,
 * up, forward, translation) and returns the count. */
BF6_API int bf6_fx_placements(bf6_ctx*, const char* level, const char* effect,
                              float* out_xf, int out_max);

/* The sheet's mip 0 for one row of bf6_level_fx, as BCn blocks ready for
 * upload - nothing is decompressed. The bytes are owned by the core and stay
 * valid until the next call. Returns 1 on success, 0 with a reason in err. */
BF6_API int bf6_fx_atlas_mip0(bf6_ctx*, const char* level, int layer_index,
                              const uint8_t** out_data, int32_t* out_size,
                              char* err, int err_len);

/* One flipbook frame's UV rect (u0, v0, u1, v1). USE THIS rather than computing
 * a pixel rect: six- and seven-column sheets do not divide a power-of-two
 * texture, so the cell is fractional (341.333, 170.667, 146.286 px on four of
 * one level's fourteen sheets) and flooring to pixels accumulates drift and
 * shears the last column. On a LeftRightTiles sheet the LEFT half is the
 * sprite; the right half is the alternate tile UseRightTile selects, and the
 * six-way basis GnomonLit reads. */
BF6_API void bf6_fx_frame_uv(const bf6_fx_layer*, int frame, float* out_uv);


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
/* Absolute-Y water surface heightfield from streaming-tree block 2.  This is
 * the large spatial offset sampled by the water vertex shader; it is distinct
 * from the small ocean FFT displacement.  Reads the mounted game at runtime. */
BF6_API bf6_terrain* bf6_read_water_heightfield(bf6_ctx*, const char* level);

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
    /* APPENDED, NEVER REORDERED. bf6_level_decals writes at the DLL's own
     * sizeof(bf6_decal), so a header and a DLL that disagree write every row
     * at the wrong stride and produce plausible garbage rather than an error.
     * Deploy the two together. */

    /* AMBIENT OCCLUSION. Its slot was declared and never read. */
    int32_t      ao;

    /* THE AUTHORED COLOUR, and it means two DIFFERENT things.
     *
     * With a colour sheet bound it is a MULTIPLIER over that sheet: 40.6% of
     * 12,941 such values exceed 1.0 and the largest is 61.1. With no sheet it
     * is the colour ITSELF, absolute: only 1.2% of 4,568 exceed 1.0, the
     * median is 0.691, and the commonest triples are road yellow
     * (0.911, 0.542, 0.042), park green and concretes. Switch on whether
     * `albedo` is bound, never on the value. */
    float        tint[3];
    int32_t      has_tint;
    /* A second authored colour, only ever present when there is no sheet. */
    float        tint2[3];
    int32_t      has_tint2;

    /* WHICH CHANNEL OF A PACKED MASK is this record's coverage, 0..3, or -1.
     *
     * The sheets are atlases: one texture holds three or four painted road
     * words or arrows and each record picks one. Evidence that this is the
     * selector rather than a coincidence: it is present on 6,334 colourless
     * records and 0 coloured ones; its cardinality per sheet is capped at 4
     * while the sheets carry 5 to 851 records each, where competing scalars on
     * the same records reach 8, 11, 19 and 28; it varies WITHIN one material
     * class and sheet pair on 34 of 49 pairs, which kills "the material
     * selects it"; and every one of the 13 sheets using 3 or 4 values is named
     * `_rgb`, while no single-mask `_op` sheet reaches 3. A reader that always
     * takes red gets roughly one record in three right. */
    int32_t      mask_channel;

    /* The record's decal-asset slot: a BLEND PRESET, not art. On MP_Badlands
     * the colour / no-colour split is uniform per slot with zero mixing.
     * NOTE it is not a terrain layer index where the slot is non-empty, which
     * on MP_Badlands is all 628 of them. */
    int32_t      asset_slot;
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
 * Units: the executable's initial-spectrum builder divides wind_angle by 360,
 * so it is degrees. wind_speed is a normalised authoring scalar, NOT m/s.
 *
 * RETRACTED: this comment used to give that scalar's scale as "0.01 calm, 0.07
 * windy, 0.30+ the D-Day sea". Those numbers were read through the SINGLE
 * PLAYER executable's reflection schema, which lays this class out differently
 * from the multiplayer one, so they are values from neighbouring fields. Under
 * the MP schema the class default is 0.5 and MP_Isolated authors about 0.9, so
 * the real scale is roughly an order of magnitude higher and 0.5 is the
 * middle of it rather than a gale. See the exe-schema finding; anything
 * calibrated against the old numbers is wrong by that factor.
 *
 * The distribution is
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

/* Complete ocean-cascade input. Unlike bf6_water_sim this preserves every
 * value consumed by the game's CPU H0 builder, including the Hermite tangent
 * pairs and the selected PC resolution. source_index is the original EBX
 * instance index; returned rows are in renderer cascade order (largest tile
 * first), up to the game's four-cascade limit. */
typedef struct {
    int32_t source_index;
    int32_t enabled;
    int32_t resolution;       /* PlatformScalableInt.Default: Win64/PC route */
    float   wind_angle_degrees;
    float   wind_speed;
    float   choppiness;
    float   tile_dimension;
    float   min_wavelength;
    float   large_wave_reduction;
    float   wave_amplitude;
    float   wave_thickness;
    int32_t foam_enable;
    float   foam_threshold;
    float   foam_max;
    float   foam_half_life;
    int32_t physics_simulation_enabled;
    int32_t force_simple_plane_collision;
    int32_t visual_cpu_simulation_enabled;
    int32_t dist_count;       /* 5, 9 or 13 */
    float   dist_x[13];
    float   dist_y[13];
    float   dist_tangent_out[12];
    float   dist_tangent_in[12];
    float   dist_clamp_min;
    float   dist_clamp_max;
} bf6_water_sim_v2;

/* Requires bf6_open_level. Returns the total enabled cascade count; writes at
 * most out_max rows. A null output/count-only call is supported. */
BF6_API int bf6_level_water_sims(bf6_ctx*, const char* level,
                                  bf6_water_sim_v2* out, int out_max);

/* Water-lab route: mount only the named level archives, load the executable
 * type schema, and inspect only that level's water schematic partitions. It
 * deliberately does NOT run bf6_open_level's placement/object-graph walk.
 * The returned rows and count/fill contract are identical to
 * bf6_level_water_sims, so callers can compare the isolated and full routes
 * byte-for-byte as a control. */
BF6_API int bf6_level_water_sims_isolated(bf6_ctx*, const char* level,
                                           bf6_water_sim_v2* out, int out_max);

/* Rebuild the CPU-created complex H0 texture used by the shipped ocean FFT.
 * out_rg contains resolution*resolution float2 values. Returns the required
 * float count on success (also for a null/count-only call), or 0 on invalid
 * input. The seed and random walk are the executable's deterministic route. */
BF6_API int bf6_water_spectrum_h0(const bf6_water_sim_v2* sim,
                                  float* out_rg, int out_float_count);

/* ---------------------------------------------------------- water, part 2
 *
 * The full RENDER description of a level's water: the geometry above plus
 * the decoded material, enough to drive a conventional renderer without
 * reading the game again.
 *
 * ABSENT IS NOT ZERO. Zero is a legal authored value for nearly everything
 * here, so every float that is not a coordinate carries a NEGATIVE sentinel
 * when the level does not author it, and every texture id is -1. That is not
 * defensive style: two of the four ocean levels run a different material
 * graph and carry none of the named foam slots, so a consumer reading zero
 * would switch their foam off rather than fall back.
 */
typedef struct {
    uint64_t state_key;
    int32_t  variant;                  /* 1 ocean, 0 foam, 2 region-banded
                                         (undecoded; colours absent)        */

    float center[2];                   /* world X, Z of the plane centre   */
    float size[2];                     /* metres, FULL width               */
    float height;                      /* world Y                          */
    float query_box_half_extent[2];    /* metres, half width; redundant    */

    int32_t attenuation_type;          /* 0 None 1 ShoreDepth 2 CoarseMask
                                         3 CoarseAndDetailMask            */
    int32_t shore_fade_valid;          /* 1 only when attenuation_type==1   */
    int32_t shore_enable;
    float   shore_depth_m;             /* metres; t = saturate(depth/this)  */
    float   shore_blend[4];            /* cubic (a,b,c,d): dot4(t^3,t^2,t,1) */
    float   additional_water_depth_m;  /* metres, added to the depth sample */
    float   wave_amplitude_scale;      /* dimensionless                     */
    int32_t visible;
    int32_t terrain_vt_access;
    /* THE ONLY READABLE WATER TYPE. There is no ocean/river/pool enum; this
      bool is the river flag, and it agrees 14 of 14 with whether the level's
      shader permutation declares Serac:WaterRiverFlowSampler. */
    int32_t is_river;

    /* COLOUR. base_colour is the AUTHORED value and on the ocean family it is
      a per-metre transmission, NOT a surface colour: it is what the water
      reaches after absorption_distance_m metres. surface_colour and
      extinction are that value converted, so a renderer never needs the
      formula. transmission over L metres is exp(-extinction * L). */
    float base_colour[3];              /* linear RGB, authored              */
    float deep_colour[3];              /* linear RGB, foam family only      */
    float surface_colour[3];           /* linear RGB, DERIVED               */
    float extinction[3];               /* per metre, DERIVED                */
    float absorption_distance_m;       /* metres, ocean family              */
    float depth_ramp_m;                /* metres, foam family               */
    int32_t overlay_count;             /* 0..4 extra lerp-stack colours     */
    float overlay_colour[4][3];        /* linear RGB, in application order;
                                         their weights are RUNTIME terms and
                                         cannot be evaluated offline       */

    float detail_fade_start_m, detail_fade_end_m;
    float foam_threshold;              /* 0..1                              */
    float shore_foam_suppression;      /* 0..1                              */
    float foam_contrast_divisor;
    float cascade_foam_weight[4];      /* cascade 0..3                      */
    float smoothness_zero_foam, smoothness_full_foam;
    float smoothness_bias, smoothness_near_multiplier;
    float foam_sheet_normal_strength, micro_sheet_normal_strength;
    float noise_uv_scale;              /* 1/metres                          */
    float reflectance_low, reflectance_high, reflectance_bias;

    /* for bf6_texture_at, or -1 */
    int32_t detail_normal, foam_normal, foam_rgb, noise, perlin;
    int32_t contact_foam, foam_rgb2;

    /* Extended-water draw graph parameters. These are appended so older ABI
       consumers keep their layout. They are read from the current level's
       ShaderBlockDepot on every call; negative means this graph does not
       author the parameter. MP_Isolated uses separate scales for the micro
       and foam normal sheets -- collapsing them to one scale is visibly
       wrong -- plus an authored micro-sheet animation rate. */
    float micro_sheet_uv_scale;         /* cycles / metre                  */
    float foam_sheet_uv_scale;          /* cycles / metre                  */
    float micro_sheet_flow_speed;       /* graph time multiplier           */
    float foam_composite_low;           /* final coverage range remap low  */
    float foam_composite_high;          /* final coverage range remap high */
    float contact_world_divisor_m;      /* contact UV = world/divisor-.5   */
    float contact_remap_low;
    float contact_gain;
    /* MP_Isolated's broad moving crest sheet.  These three values are depot
       constants selected by the current pixel permutation, not fitted
       preview controls.  The three motion coefficients which combine them
       are literals in the current shipped permutation. */
    float broad_pattern_world_mul;
    float broad_pattern_world_scale;
    float broad_pattern_floor;
    /* Exact current extended-water material cbuffer, registers 0..21.  The
       values are assembled from the mounted depot by Name32, never loaded
       from a staged dump.  Version is zero unless every value used by the
       current MP_Isolated graph and all five textures are present AND the
       live ShaderStateDatabase selects the pixel program this translation was
       decoded from. */
    uint32_t extended_graph_version;
    float extended_cb1[22][4];
    /* Raw .NET-layout GUID bytes of pass 0's selected pixel program. */
    uint8_t selected_pass0_pixel_guid[16];
    /* Current selected vertex shader's cascade-0 overlap transform. Appended
       for ABI compatibility. Version is zero unless the live entity carries
       every source field and pass 0 selects the shader whose arithmetic was
       decoded. params=(sin,cos,shear_x,shear_y), params2=(1/A,1/B,A,B). */
    uint32_t cascade_overlap_version;
    int32_t cascade_overlap_enabled;
    float cascade_overlap_params[4];
    float cascade_overlap_params2[4];
    float cascade_overlap_height_scale;
    uint8_t selected_pass0_vertex_guid[16];

    /* Active VisualEnvironment OceanComponentData, joined at runtime through
       the level root's import list.  These are the inputs consumed by the
       deferred water composite; they do not come from the surface material
       above.  Version is zero unless the active preset and every required
       source field were resolved from the mounted game. */
    uint32_t ocean_component_version;
    char ocean_preset[128];
    int32_t ocean_preset_candidates;
    int32_t ocean_enable;
    int32_t simplified_distortion;
    int32_t foam_enable;
    float composite_ior;
    float opacity_ramp_m;
    float foam_depth_ramp_m;
    float scatter_phase_g;
    float transmission_colour[3];
    float scatter_shadow_influence;
    float foam_tint[3];
    float foam_smoothness;
    float foam_roughness;              /* derived as 1-FoamSmoothness */
    float authored_ocean_albedo[3];
    float authored_albedo_distance_m;
} bf6_water_render;

/* Requires bf6_open_level for this level first. Same convention as
  bf6_level_instances: returns the count, fills out[] to out_max. */
BF6_API int bf6_level_water_render(bf6_ctx*, const char* level,
                                   bf6_water_render* out, int out_max);

/* The terrain utility raster bound by the selected water vertex shader as the
 * CoarseMask source. The atlas is the game's R8 pages in persistent-record
 * order, byte-for-byte; indirection is the packed uint32 table built by the
 * game's runtime loader. Pointers are owned by the context and remain valid
 * until the next mask read or bf6_close. No exported intermediate is used. */
typedef struct bf6_water_mask {
    uint32_t version;                 /* 1 = current exact utility-raster path */
    uint32_t tile_side;               /* stored page side, including border   */
    uint32_t interior_side;           /* tile_side - 2*border - 1             */
    uint32_t border;
    uint32_t page_count;
    uint32_t indirection_side;        /* 1 << max_level                       */
    float bounds_min[2];              /* world X,Z metres                     */
    float bounds_max[2];
    float coverage_side_rcp;          /* 1 / (bounds_max.x-bounds_min.x)       */
    float border_fraction;            /* border / (tile_side-1)                */
    const uint8_t* atlas_r8;          /* page_count*tile_side*tile_side bytes  */
    const uint32_t* indirection_u32;  /* indirection_side squared packed cells */
} bf6_water_mask;

/* Requires the level to be mounted. Returns 1 and fills out on success. */
BF6_API int bf6_level_water_mask(bf6_ctx*, const char* level,
                                 bf6_water_mask* out, char* err, int err_cap);


/* --------------------------------------------------------------- lighting */
/* THE LEVEL'S OWN LIGHTING, out of its active VisualEnvironment preset.
 *
 * A VE is the map's whole authored environment: sun, sky, atmosphere, two cloud
 * layers, cloud shadows, fog, exposure, bloom, colour grading, white balance,
 * ambient occlusion, global illumination and the shadow cascades - about 800
 * fields over 27 components. This is the subset that lights a scene, plus the
 * texture references that go with it.
 *
 * WHICH PRESET. A level ships several - interior, dark alley, construction
 * site, thermal - and the level ROOT partition names the outdoor one by
 * importing it. mp_dumbo's lighting/ folder holds 19 partitions and the root
 * imports exactly two of them: the active VE and `thermal`. So:
 *
 *   active VE = the non-thermal ve_* partition, under the level's OWN
 *               directory, that the level root imports
 *
 * It is a selector, not a dependency dump. `preset` says which one answered and
 * `preset_candidates` says how many matched, so a level that ever grows a
 * second outdoor preset shows up as a number rather than as a silent choice.
 *
 * ABSENT IS NOT ZERO. Zero is a legal authored value nearly everywhere here -
 * MP_Subsurface really does ship SunIntensity 0.001, an underground map with
 * the sun off - so a component the preset does not carry cannot be recognised
 * by its values. `components` is the bitmask of the ones that were there, and
 * every field belonging to a component whose bit is clear is meaningless.
 *
 * Strings are FIXED ARRAYS inside the struct rather than pointers into the
 * context: the whole record is one value the caller owns, with no lifetime to
 * track and nothing to free.
 */
typedef enum {
    BF6_VE_SUN           = 1 << 0,
    BF6_VE_SKY           = 1 << 1,
    BF6_VE_FOG           = 1 << 2,
    BF6_VE_EXPOSURE      = 1 << 3,
    BF6_VE_GRADING       = 1 << 4,
    BF6_VE_WHITE_BALANCE = 1 << 5,
    BF6_VE_AO            = 1 << 6,
    BF6_VE_GI            = 1 << 7,
    BF6_VE_SUN_SHADOW    = 1 << 8
} bf6_ve_component;

typedef struct {
    char    preset[128];        /* the preset's leaf name                    */
    char    preset_path[256];   /* the partition it was read from            */
    int32_t preset_candidates;  /* outdoor presets the level root selected   */
    uint32_t components;        /* bf6_ve_component bits actually present    */
    /* A level blends several VEs. This is the chosen preset's own blend
     * weight, 1.0 on every outdoor preset checked, and the number that
     * separates the environment from a thin layer stacked on top of it
     * (mp_granite's `nolut` preset sits at 0.15 with 2 components). */
    float   visibility;
    int32_t component_count;    /* components the preset's entity declares   */

    /* ---- sun --------------------------------------------------------------
     * SUN ANGLES ARE A COMPASS BEARING PLUS AN ELEVATION, both in DEGREES.
     * sun_rotation_x is the bearing: 0 points along the game's +Z and turns
     * toward +X. sun_rotation_y is height above the horizon. A unit vector
     * TOWARD the sun is therefore
     *
     *     (sin(az)*cos(el), sin(el), cos(az)*cos(el))
     *
     * and NOT the maths convention (cos az, sin az), which is both mirrored
     * and 90 degrees out - two errors that cancel often enough to look nearly
     * right. Nothing shipped can settle this on its own: the sky panoramas
     * contain no sun disc (the engine draws it), and the maptiles are flat lit.
     * It was settled against the running game on two dense city maps.
     *
     * sun_intensity is REAL ILLUMINANCE IN LUX - 120000 for full midday, 45860
     * for a low golden-hour sun, 0.001 on an underground map. A renderer with
     * no physical light units has to map it, and that mapping is a calibration
     * the consumer owns; this reports the game's number.
     *
     * sun_color is a linear TINT and the magnitude lives in sun_intensity, so
     * red is 1.0 on almost every map. It is NOT normalised, though: MP_Plaza
     * ships (1.06066, 0.5249, 0.1534) and clamping that to 1 quietly changes
     * the colour. Use it as authored. */
    float   sun_rotation_x;
    float   sun_rotation_y;
    float   sun_color[3];
    float   sun_intensity;
    float   sun_angular_radius;        /* degrees, the disc's half-angle      */
    float   sun_specular_scale;
    float   sun_shadow_view_distance;  /* metres, highest quality level       */
    /* Cloud shadows. speed is (0,0) on some maps and (-4,-8) or (2,2) on
     * others: static on some, moving on others, which is exactly the kind of
     * thing generalising one map's lighting to the fleet gets wrong. */
    float   cloud_shadow_size;         /* world metres per tile               */
    float   cloud_shadow_coverage;
    float   cloud_shadow_exponent;
    float   cloud_shadow_speed[2];
    float   cloud_shadow_translation[2];
    int32_t cloud_radiosity;           /* 0/1 */

    /* ---- sky -------------------------------------------------------------
     * THERE IS NO ZENITH / HORIZON / GROUND COLOUR HERE, because the VE does
     * not author one. Those three are a SAMPLE of the sky gradient texture,
     * and a reader that invented them would be reporting its own sampling
     * choices as game data. sky_gradient_res and sky_gradient_texture hand
     * over the gradient itself (a 256x128 BC6H strip) so a consumer can sample
     * it and say how. */
    int32_t sky_type;                  /* 0 = panoramic                       */
    /* Panoramas ship NORMALISED (measured mean 0.057 to 1.345 across the
     * fleet) and this carries the magnitude. Read it; do not measure the
     * texture. */
    float   sky_luminance_scale;
    float   sky_panoramic_rotation;    /* TURNS, not degrees                  */
    float   sky_panoramic_tile_factor;
    int32_t sky_draw_sun_disc;         /* 0/1, true on every shipped map      */
    float   sun_disc_size;
    float   sun_disc_scale;            /* the SKY's SunScale, ~300000         */
    /* RayleighScatteringCoefficient is a VEC3, not the scalar its name
     * implies. The per-channel ratio (6e-06, 1.4e-05, 3.3e-05) is why the sky
     * is blue; collapsing it to one number throws the colour away. */
    float   rayleigh[3];
    float   rayleigh_scale;
    float   mie_coefficient;
    float   mie_g;
    int32_t use_aerial_perspective;    /* 0/1 */
    float   aerial_perspective_scale;
    float   aerial_perspective_intensity;
    float   earth_radius;              /* thousands of km, as authored        */
    float   atmosphere_radius;
    float   height_fog_color_add[3];   /* HDR radiance, the sky's own term    */
    float   cloud1_altitude;           /* metres                              */
    float   cloud1_tile_factor;
    float   cloud1_rotation;           /* degrees                             */
    float   cloud1_speed;
    float   cloud1_alpha_mul;
    float   cloud1_color[3];

    /* ---- fog --------------------------------------------------------------
     * fog_color is HDR RADIANCE, not a 0..1 colour: (1385, 2132, 3072) is a
     * sky blue at magnitude 3072. Normalise by the peak channel to get a hue
     * and keep the magnitude separately; clamping it turns every map's fog
     * white. Fog is the most map-varying system in the VE - fog_color alone
     * takes 14 distinct values across 22 maps, and fog_height_enable is
     * genuinely false on some. */
    int32_t fog_height_enable;         /* 0/1 */
    int32_t fog_color_enable;
    int32_t fog_gradient_enable;
    float   fog_color[3];
    float   fog_dist_start, fog_dist_end;      /* metres */
    float   fog_color_start, fog_color_end;    /* metres */
    float   fog_height_start, fog_height_end;  /* metres */
    float   fog_altitude;              /* world Y the height fog sits at      */
    float   fog_depth;
    float   fog_visibility_range;
    int32_t volumetrics_enable;        /* participating media, 0/1            */
    float   sun_scatter_intensity;
    float   local_light_scatter_intensity;

    /* ---- exposure + bloom -------------------------------------------------
     * EXPOSURE IS AUTOMATIC on the shipped maps (auto_exposure is 1), so `ev`
     * is the starting point of a runtime metering loop and not a fixed value a
     * renderer can apply. ev_max clamps it. A consumer that wants a fixed
     * exposure has to choose one; the game does not ship it. */
    int32_t auto_exposure;             /* 0/1 */
    float   ev;
    float   ev_max;
    float   exposure_compensation;
    float   bloom_scale[3];
    int32_t bloom_method;

    /* ---- colour grading + white balance ----------------------------------- */
    int32_t grading_enable;            /* 0/1 */
    float   grade_brightness[3];
    float   grade_contrast[3];
    float   grade_saturation[3];
    float   grade_hue;                 /* degrees */
    float   white_temperature;         /* kelvin */
    float   white_tint;

    /* ---- ambient occlusion ------------------------------------------------
     * ao_affects_outdoor_light is FALSE on the maps checked: ambient occlusion
     * does not darken sun-lit surfaces in this game. A renderer that lets its
     * SSAO touch direct light is not matching it. */
    int32_t ao_affects_outdoor_light;  /* 0/1 */
    int32_t ao_affects_local_light;
    float   ssao_max_distance_inner, ssao_max_distance_outer;
    float   hbao_radius, hbao_contrast;
    float   dynamic_ao_factor;

    /* ---- global illumination (the Enlighten sky box) ---------------------- */
    float   gi_terrain_color[3];
    float   gi_sky_color[3];
    float   gi_ground_color[3];
    float   gi_sun_color[3];
    float   gi_backlight_rotation_x;   /* degrees, = sun bearing + 180        */
    float   gi_backlight_rotation_y;
    float   gi_bounce_scale;
    float   gi_sun_scale;              /* the GI's own SunScale, ~1.5         */

    /* ---- the textures the preset binds ------------------------------------
     * Resource names, empty when the preset binds nothing there, plus the
     * texture ids for bf6_texture_at where the mount carries the resource
     * (-1 otherwise). Skies and light probes are BC6H.
     *
     * These come out of the payload as raw import pointers. The type tables
     * describe these particular fields with a NULL type, so an ordinary schema
     * read reports them absent - which is why an earlier reader concluded some
     * maps bound no sky at all. */
    int32_t has_panorama;              /* 0/1, panorama_res is non-empty      */
    char    panorama_res[192];
    int32_t panorama_texture;
    char    panorama_alpha_res[192];
    char    sky_gradient_res[192];
    int32_t sky_gradient_texture;
    char    flow_mask_res[192];
    char    cloud_layer1_res[192];
    char    cloud_shadow_res[192];
    int32_t cloud_shadow_texture;
    char    secondary_cloud_shadow_res[192];
    char    grading_lut_res[192];      /* directly consumable as a colour LUT */
    char    lens_dirt_res[192];

    /* Of the named fields this call looks for, how many the preset carried.
     * A number rather than a bool because a preset legitimately omits whole
     * components, and a caller wants to see that as a ratio rather than as a
     * failure. */
    int32_t fields_found;
    int32_t fields_expected;

    /* Version 1 appended sky/cloud fields. They are kept at the tail so every
     * previously published member retains its ABI offset. The values are read
     * from the active preset in the current install; no fleet table is a
     * runtime input. */
    uint32_t sky_cloud_extension_version;
    float sky_panoramic_uv_min[2];
    float sky_panoramic_uv_max[2];
    float sky_flow_distance;
    float sky_flow_direction;           /* degrees */
    float sky_flow_period;              /* seconds */
    float sky_flow_height_mask_scale;
    float sky_flow_height_mask_bias;
    float secondary_cloud_shadow_size;
    float secondary_cloud_shadow_coverage;
    float secondary_cloud_shadow_exponent;
    float secondary_cloud_shadow_speed[2];
    float secondary_cloud_shadow_translation[2];
    int32_t cloud_shadow_addressing_mode;
    int32_t secondary_cloud_shadow_addressing_mode;
    int32_t cloud_shadow_is_top_down;
    int32_t secondary_cloud_shadow_is_top_down;
    float cloud_shadow_start_fade;
    float cloud_shadows_fade_distance;
    int32_t cloud_shadow_height_fade_enable;
    float cloud_shadow_start_height_fade;
    float cloud_shadows_height_fade_distance;
    int32_t secondary_cloud_shadow_texture;
    int32_t panorama_alpha_texture;
    int32_t flow_mask_texture;
    int32_t cloud_layer1_texture;
    int32_t grading_lut_texture;
    int32_t lens_dirt_texture;
} bf6_ve_lighting;

/* Decode `level`'s active VisualEnvironment. Returns 1 on success.
 *
 * Mounts on demand, so it works on a context where bf6_open_level failed at the
 * placement walk (or was never called). */
BF6_API int bf6_level_lighting(bf6_ctx*, const char* level,
                               bf6_ve_lighting* out, char* err, int err_len);

/* Every partition the active preset imports, resolved to a name: its level's
 * own textures, a few shared/global ones, and the enum types two of its fields
 * are declared with. This is how a map's real texture set is found without
 * guessing at a `t_<map>_panoramicsky_*` naming convention - Tungsten's set is
 * a five-element lens-flare rig and a colour-grading LUT, which no convention
 * would have surfaced.
 *
 * Returns the TOTAL count and fills out[] to out_max. Strings point into the
 * context and stay valid until the next bf6_level_lighting_imports call. */
BF6_API int bf6_level_lighting_imports(bf6_ctx*, const char* level,
                                       const char** out, int out_max);

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
    int32_t colour_map;
    /* Let a texel no textured layer reached take the aerial colour map rather
     * than the game's magenta. A renderer wants this on; a diagnostic does
     * not. */
    int32_t fallback_colour_map;     /* 0/1, the per-layer Overlay blend */
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
    /* How strongly this layer takes the aerial colour map, as a Photoshop
     * Overlay. 1 when the level does not author it - the field is optional
     * and its absence means "fully", not "not at all". */
    float       overlay;

    /* THE EVALUATOR CONSTANTS. `weight` in the coverage raster is the game's
     * raw MASK, not final coverage; the ComputeLayer kernel turns one into the
     * other per pixel using each layer's own height:
     *
     *   coverage = saturate(mask + (hiRef - loRef) * height_blend)
     *
     * with both height references pulled toward the running composite by
     * pow(maskRamp, mask_ramp_exp), evaluated in ASCENDING layer order.
     * Normalising the masks instead makes every texel a four-way average and
     * no material ever reads. The height itself is the BLUE channel of
     * `normal_res`. */
    float       base_height;
    float       displace_range;
    float       mask_ramp_exp;
    float       height_blend;
    float       coord_scale[2];
    float       uv_offset[2];
    /* Authored auxiliary `_op` sheet used by the generated evaluator to gate
     * the stored paint mask. Appended so every earlier field keeps its ABI
     * offset; NULL/empty means the multiplicative identity. */
    const char* coverage_res;
} bf6_ground_material;

typedef struct {
    int32_t        size;
    float          lo[2];           /* world XZ of the low corner, metres   */
    float          hi[2];
    /* size*size*slot_count each, owned by the context. idx indexes MATERIALS, not raw
     * layer ids, and 255 means "no layer here"; w is that slot's weight,
     * weight-sorted with the first zero ending the list. Sample idx with
     * POINT filtering - a bilinear read of an index is a different index. */
    const uint8_t* idx;
    const uint8_t* weight;
    /* THE AERIAL COLOUR MAP over the same rectangle, size*size*3 sRGB bytes,
     * or NULL when the level ships none. Every layer Overlay-blends this by
     * its own `overlay` strength. It is what carries a map's real palette:
     * the sheets on their own are stock studio colour, so a renderer that
     * skips this draws ground that is grey or plainly the wrong hue. */
    const uint8_t* colour;
    const bf6_ground_material* materials;
    int32_t        material_count;
    float          empty_fraction;
    /* Number of compact evaluator slots per texel. Appended for ABI hygiene;
     * zero from an older DLL means the historical width of four. */
    int32_t        slot_count;
} bf6_ground_coverage;

/* Requires bf6_open_level. size 0 -> 2048. Returns 1 on success. */
BF6_API int bf6_ground_coverage_get(bf6_ctx*, const char* level, int size,
                                    bf6_ground_coverage* out,
                                    char* err, int err_len);

/* One material sheet, DECODED and resampled to a square of `size`, so a
 * renderer can put every ground layer into one texture array.
 *
 * bf6_texture_at hands back COMPRESSED blocks at whatever size the asset
 * ships, which is right for binding a sheet on its own and useless for an
 * array: an array needs one size and one format for every slice. This does
 * the decode and the box-resample so a caller does not need a BCn decoder of
 * its own.
 *
 * Writes size*size*4 bytes of RGBA8 into `out`, which the CALLER owns and
 * sizes. Returns 1 on success. */
BF6_API int bf6_layer_sheet(bf6_ctx*, const char* res_name, int size,
                            uint8_t* out, char* err, int err_len);

/* ----------------------------------------------------------- raw asset access
 *
 * WHY A RAW DOOR EXISTS AT ALL. Everything above hands back DECODED things -
 * a mesh, a texture, a light row - because that is what an engine binding
 * wants. A pipeline that reproduces the game's own asset tree on disk wants
 * the opposite: the bytes exactly as the mount produces them, so a decoder
 * written elsewhere (a Python EBX reader, an existing converter) can read them
 * without this library having to grow that decoder's opinions.
 *
 * These are the mount's own three tables, unfiltered: EBX partitions,
 * resources, and chunks. Nothing here parses anything except bf6_res_chunks,
 * which exists only because the mapping resource -> chunk is not recorded in
 * the mount and has to come out of the resource's own header.
 */

/* MOUNT THE REST OF THE INSTALL.
 *
 * bf6_open mounts <game>/Data/Win32/*.toc only. That is the fast catalogue and
 * it is not the whole install: a large part of the shipped content arrives in
 * the packages under Update/, and anything that reads assets by NAME rather
 * than by placement will find those names simply absent. Measured on a retail
 * install, a weapon folder that holds 250 assets holds 4 of them in the
 * Data/Win32 mount - every per-attachment record is in the Update packages.
 *
 * include_levels additionally mounts every LEVEL's archives, which is a much
 * bigger and slower thing; pass 0 unless level content is actually wanted.
 * First mount wins on a name collision, so calling this after bf6_open leaves
 * everything bf6_open already resolved exactly as it was.
 *
 * Returns 1 on success, 0 with a reason in err. */
BF6_API int bf6_mount_all(bf6_ctx*, int include_levels, char* err, int err_len);

/* One row of the mount's name tables. `name` points into the ctx and is valid
 * until bf6_close. `type` is the resource type id (0 for EBX), `size` the
 * decompressed byte length the bundle recorded. */
typedef struct {
    const char* name;
    uint32_t    type;
    uint32_t    size;
} bf6_asset;

/* Every EBX partition / resource whose name CONTAINS `search` (NULL or "" =
 * all). Returns the TOTAL match count and writes up to out_max rows, so the
 * usual call is once with out NULL to size and once to fill. Order is the
 * mount's hash order and is not stable across runs; sort if you need it. */
BF6_API int bf6_list_ebx(bf6_ctx*, const char* search, bf6_asset* out, int out_max);
BF6_API int bf6_list_res(bf6_ctx*, const char* search, bf6_asset* out, int out_max);

/* Which chunks the mount carries, by 32-char lowercase guid hex. Writes
 * out_max * 33 bytes (32 hex + NUL each) into `out`. Returns the total. */
BF6_API int bf6_list_chunks(bf6_ctx*, char* out, int out_max);

/* PARTITION GUID -> ASSET NAME.
 *
 * An EBX partition refers to another one by GUID, never by name, so anything
 * that follows a reference out of a partition needs this map to know what it
 * landed on. It is not in the mount's tables: it comes from the EFIX header of
 * every partition, so building it is a read of every partition header in the
 * mount and it is cached after the first call.
 *
 * Both strings point into the ctx and live until bf6_close. Returns the TOTAL
 * count; writes up to out_max rows. */
typedef struct {
    const char* guid;   /* 8-4-4-4-12 lowercase hex with dashes */
    const char* name;   /* the asset name with a .ebx suffix    */
} bf6_partition;

BF6_API int bf6_partition_index(bf6_ctx*, bf6_partition* out, int out_max);

/* The runtime-read subset needed by the native armory. Same lifetime and
 * count/fill contract as bf6_partition_index. */
BF6_API int bf6_armory_partition_index(bf6_ctx*, bf6_partition* out, int out_max);

typedef enum {
    BF6_RAW_EBX = 0,
    BF6_RAW_RES = 1,
    BF6_RAW_CHUNK = 2        /* `name` is the guid hex, either spelling */
} bf6_raw_kind;

/* The decompressed bytes of one asset. Returns the byte length, or -1 when the
 * name is not in the mount / the read failed.
 *
 * The bytes live in a ONE-SLOT buffer on the context and are valid only until
 * the next bf6_read_raw on that context - a copy-out door, not a cache. A
 * two-call size-then-fill protocol would decompress twice, which over a
 * hundred thousand assets is the whole cost of the operation; a handle per read
 * would make the caller free a hundred thousand times. Neither is worth it for
 * a caller that is going to memcpy the bytes and write them to a file.
 *
 * NOT thread safe against itself for that reason: one reader per context. */
BF6_API int64_t bf6_read_raw(bf6_ctx*, int kind, const char* name,
                             const uint8_t** out_data);

/* What a chunk is TO the resource that named it. A caller extracting to disk
 * wants this: the streamed chunks are the high-resolution mip0 sheets and they
 * are the overwhelming majority of the bytes, so "everything except those" is
 * a real and useful choice rather than a corrupt subset. */
typedef enum {
    BF6_CHUNK_EMBEDDED = 0,  /* the texture's own chain, mip0 or the tail mips */
    BF6_CHUNK_STREAMED = 1,  /* mip0 on its own, present only when the header's
                              * stream bit is set - the high-resolution sheet  */
    BF6_CHUNK_LOD      = 2   /* a MeshSet LOD's vertex and index bytes         */
} bf6_chunk_role;

/* The chunk guids one RESOURCE references, which is the only part of the
 * mapping that is not in the mount's tables. A MeshSet names one chunk per LOD;
 * a texture names an embedded chunk and, when its header's stream bit is set, a
 * separate mip0 chunk.
 *
 * Writes out_max * 33 bytes (32 hex + NUL each) into `out`, and, when
 * out_roles is non-NULL, out_max ints of bf6_chunk_role beside them. Returns
 * the total found, or 0 when the resource references none.
 *
 * A guid the mount does not carry is NOT returned, and neither is an all-zero
 * one: both mean "no chunk here", and reporting them would make every caller
 * re-derive that. */
BF6_API int bf6_res_chunks(bf6_ctx*, const char* res_name, char* out, int out_max,
                           int* out_roles);

/* IS THIS INSTALL'S TYPE SCHEMA READABLE?
 *
 * An EA App install ships the executable DRM-wrapped: the reflection sections
 * read as ciphertext, so every type lookup returns nothing and every EBX walk
 * comes back EMPTY WITH NO ERROR. A tool that does not check this produces a
 * plausible, entirely empty result. Meshes, textures and the archive name
 * tables are unaffected - they never touch the executable.
 *
 * Measures Shannon entropy over the first mebibyte of the executable's
 * `typeinfo` section. Returns 1 when the schema reads, 0 when it does not, and
 * -1 when the executable could not be opened at all. `bits` (may be NULL)
 * receives the measurement so a caller can print it rather than assert it. */
BF6_API int bf6_typeinfo_readable(const char* exe_path, double* bits);

/* One EBX partition, read THROUGH THE REFLECTION, formatted as a readable
 * tree. Returns the byte length the text needs (including the terminator), or
 * -1 with a reason written into `out`.
 *
 * For exploring a record whose layout you do not yet know. Reading a field by
 * FIXED OFFSET is the alternative and it is a trap: attachment cost sits at
 * 0x80 in an attachment partition, while 0x80 in a slot-definition partition
 * is the middle of that partition's own name string, and both reads return a
 * plausible integer. The schema already knows the difference.
 *
 * max_depth <= 0 means 3, which is deep enough to see an array of structs. */
BF6_API int64_t bf6_ebx_dump(bf6_ctx*, const char* name, int max_depth,
                             char* out, int out_len);

/* BF6's installed reflection does not ship field names, only hashes. This ABI
 * is retained for compatibility and returns NULL in the runtime library.
 * Research tools may join hashes against the guarded field-name oracle, but a
 * runtime reader must carry its proven field hashes and read path directly. */
BF6_API const char* bf6_field_name(uint32_t name_hash);

/* ------------------------------------------------------------------ armory */
/* One weapon-customization slot, as the GAME defines it.
 *
 * There are 43 of these, not the 11 three-letter codes that appear in
 * attachment FILENAMES. `mzl` is an abbreviation in a filename; MuzzleDevice
 * is a slot. A consumer that works from the abbreviations cannot see the
 * slots that have no filename form, including camo and charm slots. */
typedef struct {
    uint32_t    id;        /* authored, NOT a hash of the name - it must be read */
    const char* name;      /* owned by the ctx, valid until bf6_close            */
    int32_t     is_player_facing; /* IsPlayerFacing, 0/1; authored field          */
    int32_t     flag2;     /* 0/1; meaning not yet established                   */
} bf6_armory_slot;

/* Every slot the install defines. Returns the TOTAL count and fills up to
 * out_max rows, so the usual call is once with out NULL to size and once to
 * fill. Sorted by partition name, so the order is stable between runs - the
 * mount's own order is a hash order and is not. -1 if the type schema cannot
 * be loaded (an EA App install, whose type table is encrypted). */
BF6_API int bf6_armory_slots(bf6_ctx*, bf6_armory_slot* out, int out_max);

/* The armory screen's 12 attachment categories, IN THE ORDER THE GAME SHOWS
 * THEM. Read from the screen's own config asset, not tabulated here, and not
 * the navigation order - a sibling asset carries the same labels in a
 * different permutation for input indexing. Strings are owned by the ctx. */
BF6_API int bf6_armory_categories(bf6_ctx*, const char** out, int out_max);

/* The three honestly reproducible BASE numbers in the armory stat block.
 * The four bars are attribute-delegate outputs and are intentionally absent
 * until those delegate expressions are decoded. */
typedef struct {
    int32_t damage;
    int32_t rate_of_fire;
    int32_t magazine;
} bf6_weapon_base_stats;

/* Read <weapon>_wb plus its ProjectileData import at runtime. Returns 1 when
 * all three values resolve, 0 for an unsupported/non-weapon roster entry. */
BF6_API int bf6_base_weapon_stats(bf6_ctx*, const char* weapon_class,
                                  const char* weapon, bf6_weapon_base_stats* out);

typedef struct bf6_weapon_ui_info {
    const char* name;
    const char* description;
    const char* class_label;
    const char* factory_label;
    const char* traits[3];
    int32_t trait_count;
} bf6_weapon_ui_info;

/* US-English strings and weapon UI metadata, decoded from the mounted
 * FsUITextDatabase/UIWeaponAbilityMetadata at runtime. Returned strings are
 * context-owned; weapon_ui_info pointers last until the next such call. */
BF6_API const char* bf6_localized_string(bf6_ctx*, uint32_t string_id);
BF6_API int bf6_weapon_ui_info_read(bf6_ctx*, const char* weapon,
                                    bf6_weapon_ui_info* out);

/* -------------------------------------------------------------------- rime */
/* One axis of a UI element's box. Anchors are fractions of the parent, offsets
 * are authored pixels on a 1920x1080 canvas (a consumer scales by nothing).
 * offset_end is an INWARD inset: on all 14,077 point-anchored axes in the
 * front end, offset_start == -offset_end exactly, which only makes sense if
 * both edges land on one line and the box is self-sized from Width/Height and
 * placed by the pivot. */
typedef struct {
    float anchor_start, anchor_end;
    float offset_start, offset_end;
    float pivot, weight;
} bf6_rime_axis;

/* One UI element, read from the SHIPPED schema.
 *
 * Rime partitions carry an older revision of these types than the executable
 * does, so this is deliberately NOT read through the reflection - doing that
 * returns plausible garbage with no error. */
typedef struct {
    const char*   name;          /* owned by the ctx, valid until bf6_close  */
    uint32_t      name_hash;     /* djb2-xor of the lowercased name          */
    bf6_rime_axis h, v;
    float         width, height; /* used when an axis is point-anchored      */
    float         alpha;
    int32_t       visible, fit_w, fit_h;
    int32_t       is_widget_ref; /* a reference to another partition's tree  */
    int32_t       instance;      /* index within the partition               */
} bf6_rime_element;

/* Every named UI element in one partition. Returns the TOTAL count and fills
 * up to out_max, so size then fill. -1 when the partition is not in the mount
 * or the type schema cannot load. */
BF6_API int bf6_rime_elements(bf6_ctx*, const char* partition,
                              bf6_rime_element* out, int out_max);

/* A live, recursively expanded Rime screen tree.  Unlike bf6_rime_elements,
 * this follows the authored Elements arrays and widget-reference imports, so
 * depth describes the real parent/child order used by the layout pass.  All
 * text is stored inline: rows remain valid independently of later calls.
 *
 * `kind` is one of BF6_RIME_* below.  A node with kind UNKNOWN is retained as
 * an explicit gap, but no fixed offsets are read from it unless its concrete
 * (type GUID, shipped signature) pair is in the compiled gate. */
enum {
    BF6_RIME_UNKNOWN = 0,
    BF6_RIME_WIDGET_REFERENCE,
    BF6_RIME_CONTAINER,
    BF6_RIME_STACK_CONTAINER,
    BF6_RIME_LABEL,
    BF6_RIME_LAYER,
    BF6_RIME_REPEAT_SHAPE,
    BF6_RIME_VECTOR_SHAPE,
    BF6_RIME_FILL,
    BF6_RIME_SVG,
    BF6_RIME_TEXTURE,
    BF6_RIME_LINE,
    BF6_RIME_MOVIE
};

typedef struct {
    char          partition[256];   /* partition that owns this instance       */
    char          reference[256];   /* widget target, empty if not a reference */
    char          type_guid[37];
    char          type_name[64];
    char          name[128];
    uint32_t      type_signature;   /* signature shipped in this partition     */
    uint32_t      name_hash;
    bf6_rime_axis h, v;
    float         width, height, alpha;
    float         pad_l, pad_t, pad_r, pad_b;
    float         item_spacing;
    int32_t       kind, depth, parent, instance;
    int32_t       visible, fit_w, fit_h;
    int32_t       stack_orientation; /* -1 except on a stack                    */
} bf6_rime_node;

typedef struct {
    int32_t nodes;
    int32_t gated_nodes;       /* concrete types accepted by signature gate */
    int32_t unknown_types;     /* retained without speculative field reads   */
    int32_t unresolved_refs;
    int32_t cycles;
    int32_t depth_limited;
    int32_t duplicate_guid_refs;    /* refs whose GUID names >1 partition    */
    int32_t instance_disambiguated_refs; /* import InstanceGuid selected one */
    int32_t name_disambiguated_refs;/* duplicate resolved by authored Name  */
    int32_t identity_collision_refs;/* same full identity; mount-first wins */
    int32_t equivalent_alias_refs;  /* byte-identical aliases; mount first  */
    int32_t ambiguous_refs;         /* duplicate still ambiguous; not read  */
} bf6_rime_tree_stats;

/* Read one screen directly from the mounted install.  max_ref_depth <= 0 uses
 * six, matching the verified menuweaponscreen oracle.  Returns the total row
 * count, -1 when the root partition cannot be read.  The function caches the
 * last root on the context, so the count/fill pair performs one traversal. */
BF6_API int bf6_rime_tree(bf6_ctx*, const char* root_partition,
                          int max_ref_depth, bf6_rime_node* out, int out_max,
                          bf6_rime_tree_stats* stats);

/* ------------------------------------------------------------------- bones */
/* One bone's skinning matrix, 3x4 row-major: right, up, forward, trans -
 * the same convention a placement transform uses. */
typedef struct { float m[12]; } bf6_bone_xform;

/* The weapon's skinning palette.
 *
 * A WEAPON MESH IS SKINNED, AND IGNORING THAT IS VISIBLE. The bolt, the
 * ejection cover, the charging handle and a mounted optic are all placed by
 * this palette - there is no separate mount path. A Wep_Bolt1 part reproduces
 * the game's own bounding box 96.4% of the time when skinned and 0.0% as
 * authored, median error 89.8 mm, which is what "the part is floating off the
 * receiver" looks like.
 *
 * Apply it per vertex:
 *     v_render = skin[ section.bones[v] ] * v_authored
 * The bone palette is the 66-entry identity list on 1,944 of 1,948 weapon
 * meshes, so the vertex index IS the bone id in practice; the four exceptions
 * are cloth sims.
 *
 * md_partition is the weapon's model definition (its parent-local bone
 * overrides). Pass NULL for the bare shared bind pose. Returns the bone count
 * and fills up to out_max, or -1 if the skeleton cannot be read. */
BF6_API int bf6_weapon_skin(bf6_ctx*, const char* md_partition,
                            bf6_bone_xform* out, int out_max);

/* The shader bundle that owns a weapon part's material, or 0 if none matches.
 *
 * Pass the result to bf6_read_mesh_scoped. A part's material lives in
 * dpf_<weapon>_<part>_<hash>_bundle_1p, NOT in the bundle its mesh shipped in,
 * and resolving against the latter binds nothing - which renders black. */
BF6_API int bf6_part_bundle(bf6_ctx*, const char* weapon, const char* part,
                            char* out, int out_len);

/* A texture id for a standalone texture RESOURCE, or -1 if not in the mount.
 *
 * bf6_texture_at only takes ids from material bindings, which cannot reach UI
 * art: an icon atlas page is bound by nothing. Register it here, then read it
 * with bf6_texture_at as usual - same cache, same decode-once rule. */
BF6_API int bf6_texture_id_by_name(bf6_ctx*, const char* res_name);

/* One sprite in a layered icon atlas - a weapon card layer, or an attachment's
 * outline icon. Card and icon are the same data. */
typedef struct {
    const char* name;        /* the sprite's source path; owned by the ctx    */
    uint32_t    name_hash;   /* joins to a card layer's sprite_hash          */
    int32_t     page;        /* -> <atlas>_atlas<page>                        */
    float       uv[4];       /* u0, v0, u1, v1 within the page                */
    float       size[2];     /* pixel size                                    */
    float       offset[2];   /* slot in the packed sheet (not card placement) */
    float       placement[2];/* authored base placement on the 512x256 canvas */
    float       canvas[2];   /* the canvas it is placed on, {512, 256}        */
} bf6_icon_sprite;

/* Every sprite in one layered icon atlas partition, e.g.
 * common/ui/assets/images/hardware/generated/layerediconsatlases/<w>_layerediconatlas
 * Returns the total and fills up to out_max. Sprites are 2-channel: line art
 * in R, fill in G - alpha is NOT coverage and sampling it gives a solid box. */
BF6_API int bf6_icon_atlas(bf6_ctx*, const char* partition,
                           bf6_icon_sprite* out, int out_max);

/* Where one sprite goes ON THE CARD.
 *
 * NOT the same as the sprite's Offset in the atlas - that is its slot in the
 * packed sheet, and composing with it stacks every layer in a column. Card
 * placement is authored in hiao_<weapon>.ebx and can be fractional/negative. */
typedef struct {
    uint32_t sprite_hash;   /* joins to bf6_icon_sprite.name_hash */
    float    offset[2];
} bf6_card_layer;

BF6_API int bf6_card_layers(bf6_ctx*, const char* hiao_partition,
                            bf6_card_layer* out, int out_max);

/* One drawable part of a weapon: its mesh, and the bundle that owns it.
 *
 * The bundle is not a convenience - it is the MATERIAL SCOPE. Pass it to
 * bf6_read_mesh_scoped, or the part resolves against the bundle its mesh
 * shipped in, binds nothing, and renders black. */
typedef struct {
    const char* mesh;     /* resource name for bf6_read_mesh                */
    const char* bundle;   /* placing bundle for bf6_read_mesh_scoped        */
    /* Runtime-authored record Binding[0] translation plus the slot-group
     * socket translation. Add after skinning. Zero when the part has no
     * gameplay-bone binding (receiver, most magazines, charms). */
    float       attach_offset[3];
    int32_t     has_attach_offset;
} bf6_weapon_part;

typedef struct {
    const char* slot;       /* shipped UI slot code: scp, rgt, btm...        */
    const char* attachment; /* token from attachment_<weapon>_<slot>_<token> */
} bf6_weapon_fit;

/* Every part mesh a weapon's model definition can draw, with its bundle.
 *
 * A DefinitionMesh does not name its mesh; it names a blueprint BUNDLE, and
 * the bundle names the mesh. One bundle can list 1-3 meshes, which is where an
 * optic's lens and a receiver's charm holder come from - draw them all. */
BF6_API int bf6_weapon_parts(bf6_ctx*, const char* md_partition,
                             bf6_weapon_part* out, int out_max);

/* The model-definition default presentation, one authored MeshSlot.Default member per
 * slot.  Unlike bf6_weapon_parts (the complete mutually-exclusive graph), this
 * is directly drawable as one weapon.  Mesh and bundle strings are context
 * owned and valid until the next weapon-parts call on that context. */
BF6_API int bf6_weapon_default_parts(bf6_ctx*, const char* md_partition,
                                     bf6_weapon_part* out, int out_max);

/* A zero-optional-attachment presentation with fitted slots selected from the
 * live model-definition graph. Structural defaults (base, barrel, magazine,
 * iron sights and rail covers) remain. The token join is bounded to the selected slot's
 * own groups and requires an exact normalized substring; an unknown/fake
 * token therefore leaves the authored default unchanged instead of choosing
 * a plausible wrong part. */
BF6_API int bf6_weapon_configured_parts(bf6_ctx*, const char* md_partition,
                                        const bf6_weapon_fit* fits, int fit_count,
                                        bf6_weapon_part* out, int out_max);

/* ---------------------------------------------------------- ground scatter */
/* One record from the level's MeshScatteringDatabase RES (0x2AB067B5).
 * This is the exact shipped CATALOGUE: which mesh, its visibility horizon and
 * dissolve ratio. The retail resource does not contain per-instance world
 * positions; consumers must label any generated placement as reconstruction. */
typedef struct {
    const char* name;              /* authored blueprint path                  */
    const char* mesh_res;          /* resolved MeshSet RES, context-owned      */
    float       view_distance;     /* metres                                   */
    float       dissolve_ratio;    /* fade fraction of the view distance       */
    int32_t     point_count;       /* opaque authored points; not placements   */
} bf6_scatter_entry;

/* Returns the exact record count, 0 when the level carries no database, or -1
 * on a malformed resource. A variable-length parse is accepted only when it
 * consumes the payload exactly. Strings remain valid until the next call. */
BF6_API int bf6_level_scatter(bf6_ctx*, const char* level,
                              bf6_scatter_entry* out, int out_max,
                              char* err, int err_len);

/* -------------------------------------------------------------------- memory */
/* Free anything this API returned (bf6_mesh*, bf6_terrain*, ...). The bf6_ctx*
 * itself is freed by bf6_close(), not this. */
BF6_API void bf6_free(bf6_ctx*, void* handle);

#ifdef __cplusplus
}
#endif
#endif /* BF6_CORE_H */
