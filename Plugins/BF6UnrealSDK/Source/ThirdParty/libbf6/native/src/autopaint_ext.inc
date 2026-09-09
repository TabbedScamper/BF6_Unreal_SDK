/* AUTOPAINT: per-level authored paint content, and the ECS system descriptor.
 *
 * AUTOPAINT is authored as a per-level tree -
 * `<level>/autopaint/{nature,pois,presets,shapes}/` - and every asset in it is
 * an ordinary `SpatialPrefabBlueprint` (fc194913), the same blueprint type the
 * rest of the engine places objects with. There is no bespoke container format.
 *
 *     4eb6b47c  AutopaintCapturePreset   0x20, ONE field: a Name
 *     e82c446c  AutopaintOutputs         0x28: Name + Outputs[]
 *     8defd794  AutopaintOutput          0x20, ONE field: a Name
 *
 * IDENTITY IS POSITIONAL. Every leaf ships an EMPTY name - 27 of 27 capture
 * presets and 2 of 2 outputs across the granite tree - while the containers in
 * the same partitions are named: the AutopaintOutputs root carries its full
 * asset path and 8 of 9 blueprint roots carry theirs. A reader that failed to
 * resolve CStrings would blank the containers too, so the empty leaf name is
 * shipped data. A consumer must address a slot BY INDEX within its named
 * container; there is no name to match on, and the reflection table gives these
 * types exactly one field, so there is no hidden payload either.
 *
 * The counts this reader returns are therefore the useful part, and
 * `outputs_named` / `presets_named` are reported so a caller can SEE that the
 * leaves are anonymous rather than take it on trust. */

static const uint32_t kApName    = 0x0c59fa06u;
static const uint32_t kApOutputs = 0x24f0f98eu;

/* Mixed-endian on disk. */
static const uint8_t kApOutputsType[16] = {
    0x6c,0x44,0x2c,0xe8, 0x47,0x5b, 0x8c,0x06, 0x6d,0x8f,0xe6,0xb6,0x20,0x8e,0x09,0x7c
};
static const uint8_t kApOutputType[16] = {
    0x94,0xd7,0xef,0x8d, 0x7f,0x41, 0xe9,0x2d, 0x36,0x5b,0xce,0x7a,0x3d,0x98,0x4a,0x65
};
static const uint8_t kApPresetType[16] = {
    0x7c,0xb4,0xb6,0x4e, 0xbc,0x4f, 0x07,0x05, 0x43,0xee,0x95,0x30,0x0c,0x38,0xf4,0xb3
};
static const uint8_t kApBlueprintType[16] = {
    0x13,0x49,0x19,0xfc, 0x82,0x9d, 0x11,0x66, 0x50,0x7c,0x33,0x45,0xa0,0x1b,0x1a,0x05
};

struct AutopaintHandle {
    bf6_autopaint            s{};
    std::vector<std::string> names;
    std::vector<const char*> nameptr;
};

bf6_autopaint* bf6_autopaint_read(bf6_ctx* c, const char* name)
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

    AutopaintHandle* h = new AutopaintHandle();

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        bf6::TypeGuid g = e.instance_type(i);
        const bool outs   = memcmp(g.data(), kApOutputsType,   16) == 0;
        const bool out    = memcmp(g.data(), kApOutputType,    16) == 0;
        const bool preset = memcmp(g.data(), kApPresetType,    16) == 0;
        const bool bp     = memcmp(g.data(), kApBlueprintType, 16) == 0;
        if (!outs && !out && !preset && !bp) continue;

        bf6::EbxValue v = e.read_instance(i);
        const bf6::EbxValue* n = v.field(kApName);
        const bool named = n && n->kind == bf6::EbxValue::Kind::Str && !n->s.empty();

        if (outs) {
            h->s.outputs_groups++;
            if (named) { h->s.groups_named++; h->names.push_back(n->s); }
            if (const bf6::EbxValue* a = v.field(kApOutputs))
                if (a->kind == bf6::EbxValue::Kind::Array)
                    h->s.outputs_in_last_group = (int32_t)a->items.size();
        } else if (out) {
            h->s.outputs++;      if (named) h->s.outputs_named++;
        } else if (preset) {
            h->s.capture_presets++; if (named) h->s.presets_named++;
        } else {
            h->s.blueprints++;
            if (named) { h->s.blueprints_named++; h->names.push_back(n->s); }
        }
    }

    for (const std::string& s : h->names) h->nameptr.push_back(s.c_str());
    h->s.named_count = (int32_t)h->nameptr.size();
    h->s.named       = h->nameptr.empty() ? nullptr : h->nameptr.data();
    c->handles[&h->s] = bf6_ctx::HK_AUTOPAINT;
    return &h->s;
}

void bf6__autopaint_delete(void* p) { delete (AutopaintHandle*)p; }

/* ---------------------------------------------------------------------------
 * ECS SYSTEM ASSET: how a runtime-only system ships when nothing authored
 * backs it. `systems/ecssystems/<name>/ecssystemasset` names its schedulables
 * by hash and by import.
 *
 *     15bbc6b1  EcsSystemAsset
 *       0x0C59FA06  Name
 *       0x258E57C4  a system-level hash
 *       0x8A0D8E03  one hash per schedulable, in step with
 *       0xCC9ADA59  imports to the execution descriptors
 *
 * These hashes are NOT djb2-XOR of the asset or descriptor names: eleven
 * candidate strings in bare, path and lowercased form scored 0. The name hash
 * cracked for NameHash (0xC124C7D8) fields does not generalise here.
 *
 * EcsWorldAnchorSystem is the worked example - onArchetypeMatched,
 * onArchetypeUnmatched and onComponentDataChangeWorldAnchorTags - which is how
 * we know anchors are composition-driven and never placed by a designer. */

static const uint32_t kEcsName     = 0x0c59fa06u;
static const uint32_t kEcsSysHash  = 0x258e57c4u;
static const uint32_t kEcsSchedHsh = 0x8a0d8e03u;
static const uint32_t kEcsSchedImp = 0xcc9ada59u;

static const uint8_t kEcsSystemType[16] = {
    0xb1,0xc6,0xbb,0x15, 0x5a,0x6c, 0x7b,0x5c, 0x22,0x0a,0x13,0x55,0xc9,0xb1,0x73,0xef
};

struct EcsSystemHandle {
    bf6_ecs_system           s{};
    std::string              name;
    std::vector<uint32_t>    hashes;
    std::vector<std::string> imports;
    std::vector<const char*> importptr;
};

bf6_ecs_system* bf6_ecs_system_read(bf6_ctx* c, const char* name)
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

    EcsSystemHandle* h = new EcsSystemHandle();
    bool found = false;

    for (size_t i = 0; i < e.instance_count(); i++)
    {
        if (memcmp(e.instance_type(i).data(), kEcsSystemType, 16) != 0) continue;
        bf6::EbxValue v = e.read_instance(i);
        found = true;

        if (const bf6::EbxValue* n = v.field(kEcsName))
            if (n->kind == bf6::EbxValue::Kind::Str) h->name = n->s;
        if (const bf6::EbxValue* f = v.field(kEcsSysHash)) h->s.system_hash = (uint32_t)f->u;
        if (const bf6::EbxValue* a = v.field(kEcsSchedHsh))
            if (a->kind == bf6::EbxValue::Kind::Array)
                for (const bf6::EbxValue& el : a->items)
                    h->hashes.push_back((uint32_t)el.u);
        if (const bf6::EbxValue* a = v.field(kEcsSchedImp))
            if (a->kind == bf6::EbxValue::Kind::Array)
                for (const bf6::EbxValue& el : a->items)
                    h->imports.push_back(el.import_path.empty() ? el.s : el.import_path);
        break;
    }
    if (!found) { delete h; return nullptr; }

    for (const std::string& s : h->imports) h->importptr.push_back(s.c_str());
    h->s.name            = h->name.c_str();
    h->s.schedulables    = (int32_t)h->hashes.size();
    h->s.schedule_hashes = h->hashes.empty() ? nullptr : h->hashes.data();
    h->s.import_count    = (int32_t)h->importptr.size();
    h->s.imports         = h->importptr.empty() ? nullptr : h->importptr.data();
    c->handles[&h->s] = bf6_ctx::HK_ECSSYSTEM;
    return &h->s;
}

void bf6__ecs_system_delete(void* p) { delete (EcsSystemHandle*)p; }
