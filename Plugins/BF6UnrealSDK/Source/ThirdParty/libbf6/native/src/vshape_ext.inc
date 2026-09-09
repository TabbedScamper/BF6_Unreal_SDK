/* VECTOR SHAPES AND SPLINES: authored control-point geometry on a level.
 *
 * Two types share one 6-field head - Flags, Points[], Realm, Tension, IsClosed,
 * AllowRoll - and `VolumeVectorShapeData` adds a Height:
 *
 *     VolumeVectorShapeData  9fc7ba2d-7564-b0a6-8a9c-61f3fd93e55d  size 0x40
 *     CustomSplineData       064f7746-fbfa-993d-995d-56f3bd6f2800  size 0x38
 *
 *     0x018  0x5645E663  Flags       UInt32
 *     0x020  0xEDFC6DF6  Points      Array of Vec3
 *     0x028  0xDFD68748  Realm       Enum
 *     0x02C  0xEA66800B  Tension     Float32
 *     0x030  0xCE6A13A0  IsClosed    Bool
 *     0x031  0x69050471  AllowRoll   Bool
 *     0x038  0x28CEA299  Height      Float32   (volume only)
 *
 * Despite the shared head the two are used for DISJOINT purposes in shipped
 * data, and a consumer should treat them as different things:
 *
 *   VOLUME  - a CLOSED, exactly PLANAR polygon (52 of 52 measured have a Y
 *             span of 0.000), extruded vertically by Height. IsClosed is true
 *             on all, Tension is 0 on all.
 *   CUSTOM  - an OPEN 3D path (8 of 9 are non-planar, up to 2.57 of Y span).
 *             IsClosed is false on all, and Tension is where the authored
 *             values actually live (0.5 on 3 of 9).
 *
 * TWO THINGS A CONSUMER WILL GET WRONG:
 *
 *   1. CLOSURE IS IMPLICIT. The first point is NOT repeated as the last -
 *      0 of 61 shapes do that. Closing the loop is the reader's job.
 *   2. WINDING IS NOT NORMALISED. 32 CW against 20 CCW in XZ. Anything that
 *      needs an orientation must compute the signed area rather than assume
 *      one, which is a real difference from combat-area volumes elsewhere in
 *      this engine that DO require a fixed winding. */

static const uint32_t kVShapeFlags   = 0x5645e663u;
static const uint32_t kVShapePoints  = 0xedfc6df6u;  /* Array of Vec3 */
static const uint32_t kVShapeRealm   = 0xdfd68748u;
static const uint32_t kVShapeTension = 0xea66800bu;
static const uint32_t kVShapeClosed  = 0xce6a13a0u;
static const uint32_t kVShapeRoll    = 0x69050471u;
static const uint32_t kVShapeHeight  = 0x28cea299u;  /* volume only */

/* Mixed-endian on disk: first three groups byte-reversed, last eight as
 * written. Both were confirmed against raw instance bytes before use, because a
 * wrong arrangement here matches nothing and reports an empty level. */
static const uint8_t kVolumeType[16] = {
    0x2d,0xba,0xc7,0x9f, 0x64,0x75, 0xa6,0xb0, 0x8a,0x9c,0x61,0xf3,0xfd,0x93,0xe5,0x5d
};
static const uint8_t kSplineType[16] = {
    0x46,0x77,0x4f,0x06, 0xfa,0xfb, 0x3d,0x99, 0x99,0x5d,0x56,0xf3,0xbd,0x6f,0x28,0x00
};

struct VShapeHandle {
    bf6_vector_shapes             s{};
    std::vector<bf6_vector_shape> shapes;
    std::vector<float>            pts;     /* all points, 3 floats each */
    std::vector<size_t>           offset;  /* per shape, index into pts   */
};

bf6_vector_shapes* bf6_vector_shapes_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;

    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(name, err);
    if (raw.empty()) return nullptr;

    bf6::Ebx e(*c->types);
    if (!e.parse(std::move(raw), err)) return nullptr;

    VShapeHandle* h = new VShapeHandle();

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid g = e.instance_type(i);
        const bool vol = memcmp(g.data(), kVolumeType, 16) == 0;
        const bool spl = memcmp(g.data(), kSplineType, 16) == 0;
        if (!vol && !spl) continue;

        bf6::EbxValue v = e.read_instance(i);

        bf6_vector_shape sh{};
        sh.is_volume = vol ? 1u : 0u;
        if (const bf6::EbxValue* f = v.field(kVShapeFlags))   sh.flags   = (uint32_t)f->u;
        if (const bf6::EbxValue* f = v.field(kVShapeRealm))   sh.realm   = (int32_t)bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kVShapeTension)) sh.tension = bf6__spawn_num(*f);
        if (const bf6::EbxValue* f = v.field(kVShapeHeight))  sh.height  = bf6__spawn_num(*f);
        sh.is_closed  = bf6__spawn_bool(v.field(kVShapeClosed));
        sh.allow_roll = bf6__spawn_bool(v.field(kVShapeRoll));

        /* Offsets now, pointers after the vector has stopped growing - taking
         * a pointer into `pts` here would dangle on the next reallocation. */
        h->offset.push_back(h->pts.size());
        int32_t n = 0;
        if (const bf6::EbxValue* p = v.field(kVShapePoints)) {
            if (p->kind == bf6::EbxValue::Kind::Array) {
                for (const bf6::EbxValue& el : p->items) {
                    if (el.kind != bf6::EbxValue::Kind::Struct || el.fields.size() < 3) continue;
                    for (int k = 0; k < 3; k++)
                        h->pts.push_back(bf6__spawn_num(el.fields[(size_t)k].second));
                    n++;
                }
            }
        }
        sh.point_count = n;
        h->shapes.push_back(sh);
    }

    for (size_t k = 0; k < h->shapes.size(); k++)
        h->shapes[k].points = h->pts.empty() ? nullptr : h->pts.data() + h->offset[k];

    h->s.count  = (int32_t)h->shapes.size();
    h->s.shapes = h->shapes.empty() ? nullptr : h->shapes.data();
    c->handles[&h->s] = bf6_ctx::HK_VSHAPES;
    return &h->s;
}

void bf6__vshapes_delete(void* p) { delete (VShapeHandle*)p; }
