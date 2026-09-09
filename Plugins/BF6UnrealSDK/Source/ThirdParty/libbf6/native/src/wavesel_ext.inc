/* WAVE SELECTION: how a sound picks its next variation.
 *
 *     10b3d36f  NewWaveAsset                        the owner
 *     6cac59e0  NewWavePrivateSelectionBehaviorData 0x30
 *        0x38141883 Owner   0x657AFD32 HistoryEntryCount   0x78F84FA8 ScoringMode
 *        0xE7E44453 BehaviorType -> one of:
 *     c54701c6  NewWaveSelectionBehaviorTypeRandom  0x30
 *     b1ecf5ed  NewWaveSelectionBehaviorTypeRank    0x30
 *        0xA688BE49 RandomizeCandidates
 *        0x888284C1 GetScoreFunction / 0x415042FA CommitSelectionFunction (TypeRefs)
 *
 * THE AUTHORED VALUES SPLIT BY ROLE, 9 of 9 measured:
 *
 *   impact FX markers  -> Random, HistoryEntryCount 3,   randomise TRUE
 *   area ambiences     -> Rank,   HistoryEntryCount 100, randomise FALSE
 *
 * which reads as: a rock impact fires in bursts and only needs to avoid an
 * audible immediate repeat, while an ambience loops for a whole match and keeps
 * a long memory so a repeat is inaudible. Only mp_contaminated ships this
 * family - 12 instances on 1 of 28 levels - so treat the rule as observed
 * rather than universal.
 *
 * The two behaviour types have IDENTICAL field layouts; the type itself is what
 * selects the runtime scoring and commit functions. */

static const uint32_t kNwHistory   = 0x657afd32u;
static const uint32_t kNwScoring   = 0x78f84fa8u;
static const uint32_t kNwRandomize = 0xa688be49u;

static const uint8_t kNwBehaviorType[16] = {
    0xe0,0x59,0xac,0x6c, 0x7c,0x5e, 0x86,0x14, 0xef,0x5f,0xa7,0x47,0x91,0x13,0x51,0x93
};
static const uint8_t kNwRandomType[16] = {
    0xc6,0x01,0x47,0xc5, 0x31,0x2e, 0xf4,0x87, 0xfc,0x50,0x84,0xe7,0xb3,0x8d,0xe6,0x34
};
static const uint8_t kNwRankType[16] = {
    0xed,0xf5,0xec,0xb1, 0xf0,0xaa, 0x97,0x79, 0x49,0xda,0x91,0x1c,0x8e,0x75,0x27,0x0a
};

struct WaveSelHandle {
    bf6_wave_selection             s{};
    std::vector<bf6_wave_selector> sel;
};

bf6_wave_selection* bf6_wave_selection_read(bf6_ctx* c, const char* name)
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

    WaveSelHandle* h = new WaveSelHandle();

    /* Which behaviour type is present in this partition, and its randomise flag. */
    int kind = -1; uint8_t randomize = 0; bool sawkind = false;
    for (size_t i = 0; i < e.instance_count(); i++) {
        bf6::TypeGuid g = e.instance_type(i);
        const bool rnd = memcmp(g.data(), kNwRandomType, 16) == 0;
        const bool rnk = memcmp(g.data(), kNwRankType,   16) == 0;
        if (!rnd && !rnk) continue;
        bf6::EbxValue v = e.read_instance(i);
        kind = rnd ? 0 : 1; sawkind = true;
        randomize = bf6__spawn_bool(v.field(kNwRandomize));
        break;
    }
    for (size_t i = 0; i < e.instance_count(); i++) {
        if (memcmp(e.instance_type(i).data(), kNwBehaviorType, 16) != 0) continue;
        bf6::EbxValue v = e.read_instance(i);
        bf6_wave_selector w{};
        w.behavior = sawkind ? kind : -1;
        w.randomize_candidates = randomize;
        /* HistoryEntryCount is a UInt32; never take it through a float. */
        if (const bf6::EbxValue* f = v.field(kNwHistory)) w.history_entry_count = bf6__ws_u32(f);
        if (const bf6::EbxValue* f = v.field(kNwScoring)) w.scoring_mode = (int32_t)bf6__spawn_num(*f);
        h->sel.push_back(w);
    }

    h->s.count     = (int32_t)h->sel.size();
    h->s.selectors = h->sel.empty() ? nullptr : h->sel.data();
    c->handles[&h->s] = bf6_ctx::HK_WAVESEL;
    return &h->s;
}

void bf6__wavesel_delete(void* p) { delete (WaveSelHandle*)p; }
