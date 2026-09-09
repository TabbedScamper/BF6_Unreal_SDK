/* skeleton_ext.inc - the character rig, read through the reflection.
 *
 * A translation-unit fragment like the others: it needs bf6_ctx and the TypeDb
 * the context owns. Wire it beside them, before the closing `} // extern "C"`.
 *
 * WHY IT EXISTS. bf6_mesh hands back a skin binding - which bones move a
 * vertex and by how much - and that is useless on its own, because a bone index
 * means nothing without the bone. Posing a character needs the rig: the parent
 * chain, the bind pose, and the inverse bind that takes a vertex out of model
 * space into each bone's frame. All three ship in one ordinary EBX instance,
 * so this reads them rather than asking a consumer to re-derive the inverse
 * (which is not the transpose - these transforms carry scale).
 *
 * The three pose arrays are NOT interchangeable and mixing them up produces a
 * character that renders, in a pose that is wrong in a different way for every
 * bone. The identities that pin them down are checked in skeleton_test.
 */

/* The instance's field-name hashes. Retail reflection strips names, so a hash
 * IS the identity here - there is nothing more readable to key on. */
static const uint32_t kSkeBoneNames   = 0x059e9a54u;
static const uint32_t kSkeBoneHashes  = 0xd47aa24eu;
static const uint32_t kSkeHierarchy   = 0xbc1b3e30u;
static const uint32_t kSkeLocalPose   = 0x0b50d1d9u;
static const uint32_t kSkeModelPose   = 0x793003f0u;
static const uint32_t kSkeInversePose = 0xba751ae2u;

/* AN INDEX MAY ARRIVE IN EITHER HALF OF THE VALUE.
 *
 * EbxValue keeps signed and unsigned in separate members, and the parent array
 * reads back as Uint on this type, so the root's -1 arrives as 4294967295.
 * Reading .i alone yields 0 for every bone - and 0 is a legal bone index, so
 * the whole rig would reparent onto bone 0 silently instead of failing. */
static int32_t bf6__ske_int(const bf6::EbxValue* v)
{
    if (!v) return -1;
    int64_t raw = 0;
    if (v->kind == bf6::EbxValue::Kind::Int)       raw = v->i;
    else if (v->kind == bf6::EbxValue::Kind::Uint) raw = (int64_t)v->u;
    else if (v->kind == bf6::EbxValue::Kind::Real) raw = (int64_t)v->f;
    else return -1;
    return (int32_t)(uint32_t)raw;      /* 0xFFFFFFFF -> -1 */
}

/* One LinearTransform: four Vec3 read IN LAYOUT ORDER - right, up, forward,
 * translation - into a 3x4 row-major block. Taken positionally rather than by
 * field hash because the element type is shared with every other transform in
 * the game and its member hashes add nothing a caller can check. */
static bool bf6__ske_xform(const bf6::EbxValue& v, float* m)
{
    if (v.kind != bf6::EbxValue::Kind::Struct || v.fields.size() < 4) return false;
    for (int r = 0; r < 4; r++)
    {
        const bf6::EbxValue& col = v.fields[(size_t)r].second;
        if (col.kind != bf6::EbxValue::Kind::Struct || col.fields.size() < 3) return false;
        for (int c = 0; c < 3; c++)
        {
            const bf6::EbxValue& s = col.fields[(size_t)c].second;
            m[r * 3 + c] = (s.kind == bf6::EbxValue::Kind::Real) ? (float)s.f
                         : (s.kind == bf6::EbxValue::Kind::Int)  ? (float)s.i
                         : (s.kind == bf6::EbxValue::Kind::Uint) ? (float)s.u : 0.0f;
        }
    }
    return true;
}

struct SkeletonHandle {
    bf6_skeleton              ske{};
    std::vector<bf6_bone>     bones;
    std::vector<std::string>  names;
    std::string               self;
};

bf6_skeleton* bf6_skeleton_read(bf6_ctx* c, const char* name)
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

    /* Find the instance that carries the rig. A skeleton partition ships more
     * than one instance, and which index the rig lands on is not fixed, so
     * this selects on CONTENT - the presence of the six parallel arrays -
     * rather than on position. */
    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::EbxValue v = e.read_instance(i);
        const bf6::EbxValue* nm  = v.field(kSkeBoneNames);
        const bf6::EbxValue* hs  = v.field(kSkeBoneHashes);
        const bf6::EbxValue* pa  = v.field(kSkeHierarchy);
        const bf6::EbxValue* lp  = v.field(kSkeLocalPose);
        const bf6::EbxValue* mp  = v.field(kSkeModelPose);
        const bf6::EbxValue* ip  = v.field(kSkeInversePose);
        if (!nm || !pa || !lp || !mp || !ip) continue;
        if (nm->kind != bf6::EbxValue::Kind::Array) continue;

        const size_t n = nm->items.size();
        if (!n) continue;
        /* EVERY ARRAY MUST BE THE BONE COUNT. A short pose array is not a
         * partial rig to be padded - it means this is a different type wearing
         * familiar field hashes, and posing from it would read past the end. */
        if (pa->items.size() != n || lp->items.size() != n ||
            mp->items.size() != n || ip->items.size() != n) continue;

        SkeletonHandle* h = new SkeletonHandle();
        h->names.resize(n);
        h->bones.resize(n);
        for (size_t b = 0; b < n; b++)
        {
            const bf6::EbxValue& s = nm->items[b];
            h->names[b] = (s.kind == bf6::EbxValue::Kind::Str) ? s.s : std::string();
            bf6_bone& out = h->bones[b];
            out = bf6_bone{};
            out.parent    = bf6__ske_int(&pa->items[b]);
            out.name_hash = (hs && hs->items.size() == n)
                          ? (uint32_t)bf6__ske_int(&hs->items[b]) : 0u;
            bf6__ske_xform(lp->items[b], out.local);
            bf6__ske_xform(mp->items[b], out.model);
            bf6__ske_xform(ip->items[b], out.inverse);
        }
        for (size_t b = 0; b < n; b++) h->bones[b].name = h->names[b].c_str();

        const bf6::EbxValue* self = v.field(0x0c59fa06u);
        if (self && self->kind == bf6::EbxValue::Kind::Str) h->self = self->s;

        h->ske.bones      = h->bones.data();
        h->ske.bone_count = (int32_t)n;
        h->ske.rig_bone_count = (int32_t)n;
        h->ske.name       = h->self.empty() ? nullptr : h->self.c_str();
        c->handles[&h->ske] = bf6_ctx::HK_SKELETON;
        return &h->ske;
    }
    return nullptr;
}

/* The handle's first member is the struct the caller holds, so the address it
 * was given is the address of the handle. Same arrangement as MeshHandle. */
void bf6__skeleton_delete(void* p) { delete (SkeletonHandle*)p; }


/* --------------------------------------------------- renderbones ---------- */

/* Renderbones are the procedural bones a skinned mesh addresses ABOVE its rig.
 * A skin index with 0x8000 set names a slot here, and the composed skeleton the
 * mesh really addresses is: SkeletonAsset bones 0..rigCount-1, then this array
 * in order, so entry k becomes bone rigCount + k.
 *
 * Each entry's parent is either a base-rig bone or an EARLIER entry of the same
 * array, which is what makes a single forward pass enough to compose them - the
 * same property the rig itself has. */
static const uint32_t kRbArray  = 0x0fb091a9u;
static const uint32_t kRbPose   = 0xe7062026u;   /* bind pose in the source bone's frame */
static const uint32_t kRbLocal  = 0x0b50d1d9u;   /* local pose (same hash as SkeletonAsset) */
static const uint32_t kRbParent = 0x7538578au;

struct RenderbonesHandle {
    bf6_renderbones             rb{};
    std::vector<bf6_renderbone> bones;
};

bf6_renderbones* bf6_renderbones_read(bf6_ctx* c, const char* name)
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

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::EbxValue v = e.read_instance(i);
        const bf6::EbxValue* arr = v.field(kRbArray);
        if (!arr || arr->kind != bf6::EbxValue::Kind::Array || arr->items.empty()) continue;

        RenderbonesHandle* h = new RenderbonesHandle();
        h->bones.reserve(arr->items.size());
        for (const auto& it : arr->items)
        {
            bf6_renderbone b{};
            b.parent = bf6__ske_int(it.field(kRbParent));
            const bf6::EbxValue* lp = it.field(kRbLocal);
            const bf6::EbxValue* bp = it.field(kRbPose);
            if (lp) bf6__ske_xform(*lp, b.local);
            if (bp) bf6__ske_xform(*bp, b.source_pose);
            h->bones.push_back(b);
        }
        h->rb.bones = h->bones.data();
        h->rb.bone_count = (int32_t)h->bones.size();
        c->handles[&h->rb] = bf6_ctx::HK_RENDERBONES;
        return &h->rb;
    }
    return nullptr;
}

void bf6__renderbones_delete(void* p) { delete (RenderbonesHandle*)p; }

/* ------------------------------------------------ the composed skeleton --- */

/* A rig plus a mesh's renderbones, as ONE array a skinned mesh can index
 * directly.
 *
 * WHY THIS IS IN THE LIBRARY. Every consumer of a character mesh needs it, and
 * every consumer that writes it themselves has to rediscover three things that
 * are not obvious and that fail quietly:
 *
 *   - a flagged skin index (0x8000) is renderbone slot ((raw & 0x7FFF) >> 1)
 *     and becomes bone rigCount + slot, NOT the shifted value on its own;
 *   - a renderbone's model pose is its local pose composed onto its PARENT's
 *     model pose, and its parent may be another renderbone;
 *   - the inverse bind must be a GENERAL affine inverse. These transforms carry
 *     scale, so the transpose shortcut gives a rig that looks almost right and
 *     drifts on exactly the bones where that is hardest to see.
 *
 * Getting any of them wrong still renders. */

static bool bf6__affine_invert(const float* m, float* o)
{
    const float a=m[0],b=m[1],c=m[2], d=m[3],e=m[4],f=m[5], g=m[6],h=m[7],i=m[8];
    const float A=e*i-f*h, B=f*g-d*i, C=d*h-e*g;
    const float det = a*A + b*B + c*C;
    if (det > -1e-12f && det < 1e-12f) return false;
    const float s = 1.0f/det;
    o[0]=A*s; o[1]=(c*h-b*i)*s; o[2]=(b*f-c*e)*s;
    o[3]=B*s; o[4]=(a*i-c*g)*s; o[5]=(c*d-a*f)*s;
    o[6]=C*s; o[7]=(b*g-a*h)*s; o[8]=(a*e-b*d)*s;
    const float tx=m[9], ty=m[10], tz=m[11];
    o[9]  = -(tx*o[0] + ty*o[3] + tz*o[6]);
    o[10] = -(tx*o[1] + ty*o[4] + tz*o[7]);
    o[11] = -(tx*o[2] + ty*o[5] + tz*o[8]);
    return true;
}

static void bf6__affine_mul(const float* a, const float* b, float* o)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s += a[r*3+k] * b[k*3+c];
            if (r == 3) s += b[9+c];
            o[r*3+c] = s;
        }
}

bf6_skeleton* bf6_skeleton_compose(bf6_ctx* c, const char* skeleton_ebx,
                                   const char* renderbones_ebx)
{
    if (!c || !skeleton_ebx) return nullptr;
    bf6_skeleton*    sk = bf6_skeleton_read(c, skeleton_ebx);
    if (!sk) return nullptr;
    bf6_renderbones* rb = renderbones_ebx ? bf6_renderbones_read(c, renderbones_ebx) : nullptr;

    const int rig   = sk->bone_count;
    const int extra = rb ? rb->bone_count : 0;

    SkeletonHandle* h = new SkeletonHandle();
    h->names.resize((size_t)(rig + extra));
    h->bones.resize((size_t)(rig + extra));
    for (int b = 0; b < rig; b++) {
        h->bones[(size_t)b] = sk->bones[b];
        h->names[(size_t)b] = sk->bones[b].name ? sk->bones[b].name : "";
    }

    char buf[32];
    for (int k = 0; k < extra; k++) {
        const int b   = rig + k;
        const int par = rb->bones[k].parent;
        bf6_bone& out = h->bones[(size_t)b];
        out = bf6_bone{};
        out.parent = par;
        std::snprintf(buf, sizeof(buf), "renderbone_%d", k);
        h->names[(size_t)b] = buf;
        for (int t = 0; t < 12; t++) out.local[t] = rb->bones[k].local[t];
        /* A parent is always ALREADY COMPOSED when reached - a base-rig bone or
         * an earlier entry - so one forward pass is enough, exactly as for the
         * rig itself. A parent that is not yet composed means the asset does not
         * obey the append-order rule, and the bone is left at identity rather
         * than composed against garbage. */
        if (par >= 0 && par < b) {
            bf6__affine_mul(out.local, h->bones[(size_t)par].model, out.model);
        } else {
            static const float I[12] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
            for (int t = 0; t < 12; t++) out.model[t] = I[t];
        }
        if (!bf6__affine_invert(out.model, out.inverse)) {
            static const float I[12] = {1,0,0, 0,1,0, 0,0,1, 0,0,0};
            for (int t = 0; t < 12; t++) out.inverse[t] = I[t];
        }
    }
    for (size_t b = 0; b < h->bones.size(); b++) h->bones[b].name = h->names[b].c_str();

    h->self = sk->name ? sk->name : "";
    h->ske.bones      = h->bones.data();
    h->ske.bone_count = (int32_t)h->bones.size();
    h->ske.name       = h->self.empty() ? nullptr : h->self.c_str();
    h->ske.rig_bone_count = rig;

    if (rb) bf6_free(c, rb);
    bf6_free(c, sk);
    c->handles[&h->ske] = bf6_ctx::HK_SKELETON;
    return &h->ske;
}

/* Resolve one per-vertex skin index against a composed skeleton. */
int32_t bf6_skin_index_to_bone(const bf6_skeleton* s, uint16_t raw)
{
    if (!s) return -1;
    const int32_t b = (raw & 0x8000)
                    ? s->rig_bone_count + (int32_t)((raw & 0x7FFF) >> 1)
                    : (int32_t)raw;
    return (b >= 0 && b < s->bone_count) ? b : -1;
}
