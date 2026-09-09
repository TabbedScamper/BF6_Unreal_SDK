/* GAMEPLAY LOGIC readers: behaviour trees, the network registry, and the
 * per-level unlock manifest.
 *
 * WHY ONE FILE. Three systems, one shape: open a partition, find instances of
 * a known type, read named fields. They are kept together because what makes
 * them correct is also shared - every GUID below is GENERATED from its textual
 * form rather than typed, after a hand-typed array (0x94 for 0xc4, and one
 * dropped byte) silently made a reader return zero instances everywhere while
 * every other control still read green. A GUID reader needs an exact expected
 * count, so every test on these asserts one.
 *
 * STORED byte order, not textual: the first three groups are BYTE-REVERSED and
 * the rest are as written. The textual spelling sits in the comment above each
 * array so the two can be checked against each other by eye.
 */

/* e66768c0-2ffa-1bb4-060b-42c776045d22  BehaviorTreeData */
static const uint8_t kBtTree[16] = {
    0xc0, 0x68, 0x67, 0xe6, 0xfa, 0x2f, 0xb4, 0x1b,
    0x06, 0x0b, 0x42, 0xc7, 0x76, 0x04, 0x5d, 0x22
};

/* 757f169a-4c17-3ba9-3b1d-f5a44d07809c  BTTreeLink */
static const uint8_t kBtLink[16] = {
    0x9a, 0x16, 0x7f, 0x75, 0x17, 0x4c, 0xa9, 0x3b,
    0x3b, 0x1d, 0xf5, 0xa4, 0x4d, 0x07, 0x80, 0x9c
};

/* 95f8e0f5-1a73-0cf5-0e39-0eefcb3143dd  BTRootNode */
static const uint8_t kBtRoot[16] = {
    0xf5, 0xe0, 0xf8, 0x95, 0x73, 0x1a, 0xf5, 0x0c,
    0x0e, 0x39, 0x0e, 0xef, 0xcb, 0x31, 0x43, 0xdd
};

/* a6ef7725-72e9-f05e-1398-0957f10cecde  BTSelectorNode */
static const uint8_t kBtSelector[16] = {
    0x25, 0x77, 0xef, 0xa6, 0xe9, 0x72, 0x5e, 0xf0,
    0x13, 0x98, 0x09, 0x57, 0xf1, 0x0c, 0xec, 0xde
};

/* 08b96045-a9e6-03a6-0368-f9edc629cc87  BTSequenceNode */
static const uint8_t kBtSequence[16] = {
    0x45, 0x60, 0xb9, 0x08, 0xe6, 0xa9, 0xa6, 0x03,
    0x03, 0x68, 0xf9, 0xed, 0xc6, 0x29, 0xcc, 0x87
};

/* 6ff43f96-ed28-a2ed-dc8f-bb35e2e99997  BTRunningNode */
static const uint8_t kBtRunning[16] = {
    0x96, 0x3f, 0xf4, 0x6f, 0x28, 0xed, 0xed, 0xa2,
    0xdc, 0x8f, 0xbb, 0x35, 0xe2, 0xe9, 0x99, 0x97
};

/* 74ca462d-380c-28d9-46fa-dd6d84b71858  BTFailNode */
static const uint8_t kBtFail[16] = {
    0x2d, 0x46, 0xca, 0x74, 0x0c, 0x38, 0xd9, 0x28,
    0x46, 0xfa, 0xdd, 0x6d, 0x84, 0xb7, 0x18, 0x58
};

/* 44d0655e-5d5f-d69d-c8d9-0048dd94080b  BTSucceededNode */
static const uint8_t kBtSucceeded[16] = {
    0x5e, 0x65, 0xd0, 0x44, 0x5f, 0x5d, 0x9d, 0xd6,
    0xc8, 0xd9, 0x00, 0x48, 0xdd, 0x94, 0x08, 0x0b
};

/* a73e8ce2-ce60-7c2d-7c4c-697c6babf69a  NetworkRegistryAsset */
static const uint8_t kNetRegistry[16] = {
    0xe2, 0x8c, 0x3e, 0xa7, 0x60, 0xce, 0x2d, 0x7c,
    0x7c, 0x4c, 0x69, 0x7c, 0x6b, 0xab, 0xf6, 0x9a
};

/* 88444fee-671d-97c6-3d89-c1f5c4703041  UnlockLevelData */
static const uint8_t kUnlockLevel[16] = {
    0xee, 0x4f, 0x44, 0x88, 0x1d, 0x67, 0xc6, 0x97,
    0x3d, 0x89, 0xc1, 0xf5, 0xc4, 0x70, 0x30, 0x41
};

/* 289833a3-f2b8-2855-8f6f-2c0e307b4010  unlocks payload (unnamed type) */
static const uint8_t kUnlockList[16] = {
    0xa3, 0x33, 0x98, 0x28, 0xb8, 0xf2, 0x55, 0x28,
    0x8f, 0x6f, 0x2c, 0x0e, 0x30, 0x7b, 0x40, 0x10
};

/* ---- field name hashes, all from data/sdk_field_names.tsv ---------------- */
static const uint32_t kFldRoot      = 0xd0ea1060u;  /* BehaviorTreeData.Root        */
static const uint32_t kFldNodes     = 0x4100ae67u;  /* BehaviorTreeData.Nodes       */
static const uint32_t kFldTree      = 0xe10a1cd5u;  /* BTTreeLink.Tree              */
static const uint32_t kFldTitle     = 0x5959ca2eu;  /* node Title                   */
static const uint32_t kFldNodeType  = 0xcaad5086u;  /* BTTreeLink.NodeType          */
static const uint32_t kFldVisual    = 0x8950e8c7u;  /* BTTreeLink.VisualEnabled     */
static const uint32_t kFldObjects   = 0x5a6115b2u;  /* NetworkRegistryAsset.Objects */
static const uint32_t kFldName      = 0x0c59fa06u;  /* NetworkRegistryAsset.Name    */
static const uint32_t kFldChecksum  = 0x258e57c4u;  /* Checksum, SHARED across types */
static const uint32_t kFldUnlockRef = 0xbd29ed33u;  /* UnlockLevelData -> manifest (unnamed) */
static const uint32_t kFldUnlockArr = 0xfe8cfe0au;  /* manifest array (unnamed)     */

static bool bf6__gl_open(bf6_ctx* c, const char* name, bf6::Ebx& e)
{
    std::string err;
    std::vector<uint8_t> raw = c->src.get_ebx(std::string(name) + ".ebx", err);
    if (raw.empty()) raw = c->src.get_ebx(name, err);
    if (raw.empty()) return false;
    /* Imports must resolve to readable paths: that IS the payload for a
     * BTTreeLink and for the registry's Objects table. */
    e.set_guid_index(&c->src.partition_index());
    return e.parse(std::move(raw), err);
}

static void bf6__gl_str(char* dst, size_t cap, const bf6::EbxValue* v)
{
    if (!dst || !cap) return;
    dst[0] = 0;
    if (!v) return;
    const std::string& s = !v->import_path.empty() ? v->import_path : v->s;
    if (s.empty()) return;
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}

/* ---- behaviour trees ----------------------------------------------------- */

struct BtHandle {
    bf6_behavior_tree        t{};
    std::vector<bf6_bt_node> nodes;
};

bf6_behavior_tree* bf6_behavior_tree_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx x(*c->types);
    if (!bf6__gl_open(c, name, x)) return nullptr;

    BtHandle* h = new BtHandle();
    h->t.root_index = -1;

    for (size_t i = 0; i < x.instance_count(); i++) {
        const bf6::TypeGuid g = x.instance_type(i);
        if (memcmp(g.data(), kBtTree, 16) == 0) {
            bf6::EbxValue v = x.read_instance(i);
            if (const bf6::EbxValue* r = v.field(kFldRoot))
                if (r->kind == bf6::EbxValue::Kind::InstanceRef) h->t.root_index = r->instance;
            if (const bf6::EbxValue* n = v.field(kFldNodes))
                h->t.declared_nodes = (int32_t)n->items.size();
            continue;
        }
        bf6_bt_node nd{};
        nd.index = (int32_t)i;
        nd.kind  = BF6_BT_OTHER;
        if      (memcmp(g.data(), kBtLink,      16) == 0) nd.kind = BF6_BT_TREELINK;
        else if (memcmp(g.data(), kBtRoot,      16) == 0) nd.kind = BF6_BT_ROOT;
        else if (memcmp(g.data(), kBtSelector,  16) == 0) nd.kind = BF6_BT_SELECTOR;
        else if (memcmp(g.data(), kBtSequence,  16) == 0) nd.kind = BF6_BT_SEQUENCE;
        else if (memcmp(g.data(), kBtRunning,   16) == 0) nd.kind = BF6_BT_RUNNING;
        else if (memcmp(g.data(), kBtFail,      16) == 0) nd.kind = BF6_BT_FAIL;
        else if (memcmp(g.data(), kBtSucceeded, 16) == 0) nd.kind = BF6_BT_SUCCEEDED;

        bf6::EbxValue v = x.read_instance(i);
        bf6__gl_str(nd.title, sizeof(nd.title), v.field(kFldTitle));
        if (nd.kind == BF6_BT_TREELINK) {
            bf6__gl_str(nd.subtree, sizeof(nd.subtree), v.field(kFldTree));
            if (const bf6::EbxValue* t = v.field(kFldNodeType))
                nd.node_type = (int32_t)bf6__spawn_num(*t);
            nd.visual_enabled = bf6__spawn_bool(v.field(kFldVisual));
        }
        /* A node with no title is KEPT, never dropped: the instance count is a
         * control, and silently discarding the untitled ones would break it. */
        h->nodes.push_back(nd);
    }

    h->t.count = (int32_t)h->nodes.size();
    h->t.nodes = h->nodes.empty() ? nullptr : h->nodes.data();
    c->handles[&h->t] = bf6_ctx::HK_BEHAVIORTREE;
    return &h->t;
}

void bf6__behaviortree_delete(void* p) { delete (BtHandle*)p; }

/* ---- network registry ---------------------------------------------------- */

struct NetRegHandle {
    bf6_net_registry            r{};
    std::vector<bf6_net_object> objs;
};

bf6_net_registry* bf6_net_registry_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx x(*c->types);
    if (!bf6__gl_open(c, name, x)) return nullptr;

    NetRegHandle* h = new NetRegHandle();
    bool found = false;
    for (size_t i = 0; i < x.instance_count(); i++) {
        if (memcmp(x.instance_type(i).data(), kNetRegistry, 16) != 0) continue;
        found = true;
        bf6::EbxValue v = x.read_instance(i);
        bf6__gl_str(h->r.name, sizeof(h->r.name), v.field(kFldName));
        if (const bf6::EbxValue* ck = v.field(kFldChecksum)) h->r.checksum = bf6__ws_u32(ck);
        if (const bf6::EbxValue* o = v.field(kFldObjects)) {
            /* ORDER IS IDENTITY. The array carries REPEATED imports - four
             * identical entries in a ten-entry table on mp_dumbo - so the slot
             * index is the network id and the import only says what occupies
             * it. Never deduplicate and never sort this. */
            for (const bf6::EbxValue& it : o->items) {
                bf6_net_object n{};
                n.slot = (int32_t)h->objs.size();
                bf6__gl_str(n.object, sizeof(n.object), &it);
                h->objs.push_back(n);
            }
        }
        break;
    }
    if (!found) { delete h; return nullptr; }
    h->r.count   = (int32_t)h->objs.size();
    h->r.objects = h->objs.empty() ? nullptr : h->objs.data();
    c->handles[&h->r] = bf6_ctx::HK_NETREGISTRY;
    return &h->r;
}

void bf6__netregistry_delete(void* p) { delete (NetRegHandle*)p; }

/* ---- the per-level unlock manifest --------------------------------------- */

struct UnlockHandle {
    bf6_unlock_manifest     m{};
    std::vector<bf6_unlock> items;
};

bf6_unlock_manifest* bf6_unlocks_read(bf6_ctx* c, const char* level_root)
{
    if (!c || !level_root) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx root(*c->types);
    if (!bf6__gl_open(c, level_root, root)) return nullptr;

    UnlockHandle* h = new UnlockHandle();
    std::string manifest;
    for (size_t i = 0; i < root.instance_count(); i++) {
        if (memcmp(root.instance_type(i).data(), kUnlockLevel, 16) != 0) continue;
        bf6::EbxValue v = root.read_instance(i);
        if (const bf6::EbxValue* ck = v.field(kFldChecksum)) h->m.root_checksum = bf6__ws_u32(ck);
        char buf[512];
        bf6__gl_str(buf, sizeof(buf), v.field(kFldUnlockRef));
        manifest = buf;
        break;
    }
    if (manifest.empty()) { delete h; return nullptr; }

    /* The import resolves to "<name>.ebx"; the mount is keyed without it. */
    std::string mname = manifest;
    if (mname.size() > 4 && mname.compare(mname.size() - 4, 4, ".ebx") == 0)
        mname.resize(mname.size() - 4);
    const size_t n = mname.size() < sizeof(h->m.manifest) - 1 ? mname.size()
                                                              : sizeof(h->m.manifest) - 1;
    memcpy(h->m.manifest, mname.data(), n);
    h->m.manifest[n] = 0;

    bf6::Ebx man(*c->types);
    if (!bf6__gl_open(c, mname.c_str(), man)) { delete h; return nullptr; }
    for (size_t i = 0; i < man.instance_count(); i++) {
        if (memcmp(man.instance_type(i).data(), kUnlockList, 16) != 0) continue;
        bf6::EbxValue v = man.read_instance(i);
        if (const bf6::EbxValue* a = v.field(kFldUnlockArr)) {
            for (const bf6::EbxValue& it : a->items) {
                bf6_unlock u{};
                bf6__gl_str(u.asset, sizeof(u.asset), &it);
                h->items.push_back(u);
            }
        }
        break;
    }
    h->m.count = (int32_t)h->items.size();
    h->m.items = h->items.empty() ? nullptr : h->items.data();
    c->handles[&h->m] = bf6_ctx::HK_UNLOCKS;
    return &h->m;
}

void bf6__unlocks_delete(void* p) { delete (UnlockHandle*)p; }

/* ---- gem modules --------------------------------------------------------- */
/* A GEM IS A PARAMETERISED MODULE, and this is where it binds to its schematic.
 * `FieldHashes` is an array of {PinHash, FieldInfoHash}: the first is a
 * SCHEMATIC GRAPH PIN, the second an entry in the gem's typed interface. On
 * gem_capturepoint, 42 of 44 PinHash values appear in the engine pin universe
 * and 0 of 44 FieldInfoHash values do - two hash namespaces, cleanly
 * dissociated - against 0 hits for 400 x 44 random u32. That is the join
 * between the gems layer and the schematics layer, in the bytes. */

/* 08780b19-98bf-107a-f976-b05452616568  GemType (module GemsShared).
 * Recorded as unnamed when this reader was written; named 2026-09-02 from the
 * TypeInfo corpus by MD5, NOT by reversing the name hash - djb2 reproduces 0 of
 * 8,577 known nameHash pairs and that remains true. */
static const uint8_t kGemRoot[16] = {
    0x19, 0x0b, 0x78, 0x08, 0xbf, 0x98, 0x7a, 0x10,
    0xf9, 0x76, 0xb0, 0x54, 0x52, 0x61, 0x65, 0x68
};

static const uint32_t kFldGemIface   = 0xa3861728u;  /* -> mi_* interface  (unnamed) */
static const uint32_t kFldGemView    = 0xbeab4b09u;  /* -> mi_*_view       (unnamed) */
static const uint32_t kFldFieldHash  = 0xd5e6ef7eu;  /* FieldHashes                  */
static const uint32_t kFldPinHash    = 0x7d40db39u;  /* FieldHashes[].PinHash        */
static const uint32_t kFldFieldInfo  = 0x22180ccau;  /* FieldHashes[].FieldInfoHash  */
static const uint32_t kFldGemNumber  = 0xde6f63b3u;  /* u32, identity unknown        */

struct GemHandle {
    bf6_gem                 g{};
    std::vector<bf6_gem_bind> binds;
};

bf6_gem* bf6_gem_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx x(*c->types);
    if (!bf6__gl_open(c, name, x)) return nullptr;

    GemHandle* h = new GemHandle();
    bool found = false;
    for (size_t i = 0; i < x.instance_count(); i++) {
        if (memcmp(x.instance_type(i).data(), kGemRoot, 16) != 0) continue;
        found = true;
        bf6::EbxValue v = x.read_instance(i);
        bf6__gl_str(h->g.interface_ebx, sizeof(h->g.interface_ebx), v.field(kFldGemIface));
        bf6__gl_str(h->g.view_ebx,      sizeof(h->g.view_ebx),      v.field(kFldGemView));
        if (const bf6::EbxValue* n = v.field(kFldGemNumber)) h->g.number = bf6__ws_u32(n);
        if (const bf6::EbxValue* a = v.field(kFldFieldHash)) {
            for (const bf6::EbxValue& it : a->items) {
                bf6_gem_bind b{};
                /* BOTH ARE HASHES AND MUST NOT PASS THROUGH A FLOAT. A 32-bit
                 * hash read as a float loses its low bits - 0xA75E6E5F became
                 * 0xA75E6E00 once already, and the damage looked like the hash
                 * algorithm being wrong rather than the accessor. */
                if (const bf6::EbxValue* p = it.field(kFldPinHash))   b.pin_hash        = bf6__ws_u32(p);
                if (const bf6::EbxValue* f = it.field(kFldFieldInfo)) b.field_info_hash = bf6__ws_u32(f);
                h->binds.push_back(b);
            }
        }
        break;
    }
    if (!found) { delete h; return nullptr; }
    h->g.count = (int32_t)h->binds.size();
    h->g.binds = h->binds.empty() ? nullptr : h->binds.data();
    c->handles[&h->g] = bf6_ctx::HK_GEM;
    return &h->g;
}

void bf6__gem_delete(void* p) { delete (GemHandle*)p; }

/* ---- schematics: the entity graph ---------------------------------------- */
/* A blueprint partition's root carries Objects[] plus THREE connection arrays.
 * The three edge kinds are not interchangeable:
 *   Property - data flow, names a pin on BOTH ends
 *   Event    - control flow, carries NO field ids
 *   Link     - object reference, node level, no pins
 * Pin hash 0 means "no pin", and a reader must not mint a slot for it: that
 * invents pins that do not exist. 0xFFFFFFFF is a second no-pin sentinel.
 *
 * THE ROOT IS FOUND BY ITS FIELDS, NOT BY A TYPE GUID. Blueprint roots are of
 * several types, and matching a GUID would silently read zero edges on the
 * ones not listed - the exact failure mode a hand-typed GUID already caused
 * once here. An instance that carries the connection arrays IS the root. */

static const uint32_t kFldPropConn   = 0xc78408a3u;  /* PropertyConnections */
static const uint32_t kFldEventConn  = 0xf5dfcfc5u;  /* EventConnections    */
static const uint32_t kFldLinkConn   = 0xf7791e41u;  /* LinkConnections     */
static const uint32_t kFldSource     = 0x4676ba2cu;  /* Source PointerRef   */
static const uint32_t kFldTarget     = 0x645b319cu;  /* Target PointerRef   */
static const uint32_t kFldSourceField= 0xc41fb636u;  /* SourceFieldId (pin) */
static const uint32_t kFldTargetField= 0x2d7314c9u;  /* TargetFieldId (pin) */
/* Objects[] is the SAME hash the network registry uses - 0x5A6115B2. Field
 * hashes are global, not per type, so one name has one hash everywhere. */
static const uint32_t kFldObjectsArr = 0x5a6115b2u;  /* Objects[]           */

struct SchemHandle {
    bf6_schematic            s{};
    std::vector<bf6_schem_edge> edges;
};

static void bf6__schem_edges(const bf6::EbxValue* arr, int kind,
                             std::vector<bf6_schem_edge>& out)
{
    if (!arr) return;
    for (const bf6::EbxValue& e : arr->items) {
        bf6_schem_edge d{};
        d.kind = kind;
        d.source = d.target = -1;
        if (const bf6::EbxValue* s = e.field(kFldSource))
            if (s->kind == bf6::EbxValue::Kind::InstanceRef) d.source = s->instance;
        if (const bf6::EbxValue* t = e.field(kFldTarget))
            if (t->kind == bf6::EbxValue::Kind::InstanceRef) d.target = t->instance;
        /* Pin ids are HASHES. Never through a float. */
        if (const bf6::EbxValue* f = e.field(kFldSourceField)) d.source_pin = bf6__ws_u32(f);
        if (const bf6::EbxValue* f = e.field(kFldTargetField)) d.target_pin = bf6__ws_u32(f);
        out.push_back(d);
    }
}

bf6_schematic* bf6_schematic_read(bf6_ctx* c, const char* name)
{
    if (!c || !name) return nullptr;
    std::string terr;
    if (!ensure_types(c, terr)) return nullptr;
    bf6::Ebx x(*c->types);
    if (!bf6__gl_open(c, name, x)) return nullptr;

    SchemHandle* h = new SchemHandle();
    h->s.root_index = -1;
    for (size_t i = 0; i < x.instance_count(); i++) {
        bf6::EbxValue v = x.read_instance(i);
        const bf6::EbxValue* pc = v.field(kFldPropConn);
        const bf6::EbxValue* ec = v.field(kFldEventConn);
        const bf6::EbxValue* lc = v.field(kFldLinkConn);
        if (!pc && !ec && !lc) continue;
        h->s.root_index = (int32_t)i;
        if (const bf6::EbxValue* o = v.field(kFldObjectsArr)) h->s.objects = (int32_t)o->items.size();
        h->s.property_edges = pc ? (int32_t)pc->items.size() : 0;
        h->s.event_edges    = ec ? (int32_t)ec->items.size() : 0;
        h->s.link_edges     = lc ? (int32_t)lc->items.size() : 0;
        bf6__schem_edges(pc, BF6_SCHEM_PROPERTY, h->edges);
        bf6__schem_edges(ec, BF6_SCHEM_EVENT,    h->edges);
        bf6__schem_edges(lc, BF6_SCHEM_LINK,     h->edges);
        break;
    }
    if (h->s.root_index < 0) { delete h; return nullptr; }
    h->s.instances = (int32_t)x.instance_count();
    h->s.count = (int32_t)h->edges.size();
    h->s.edges = h->edges.empty() ? nullptr : h->edges.data();
    c->handles[&h->s] = bf6_ctx::HK_SCHEMATIC;
    return &h->s;
}

void bf6__schematic_delete(void* p) { delete (SchemHandle*)p; }
