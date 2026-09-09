/* UI SOUNDS: the game's own interface audio, over the C ABI.
 *
 * The decode itself is already proven elsewhere - ui_runtime/installed_sound_wave
 * walks an authored sound config to its NewWaveResource, picks the authored
 * chunk GUID, finds the SPS block at the authored SamplesOffset and decodes
 * PCM16BE / XAS1 exactly, with negative controls on the chunk GUID, the SPS
 * offset and the config field hash. Nothing here re-derives any of that. This
 * file only answers three questions a tool actually asks:
 *
 *   what UI sounds does the install carry      -> bf6_ui_sound_list
 *   what does the game itself call each one    -> bf6_ui_sound_style
 *   give me the samples for one of them        -> bf6_ui_sound_decode
 *
 * and a fourth for the Portal object library:
 *
 *   this SFX_* placeable - what does it play   -> bf6_sfx_placeable_resolve
 *
 * THE PLACEABLE JOIN. asset_types.json carries NO sound reference for an SFX
 * placeable: SFX_UI_Select_A_2D ships constants mesh=SFX_Default and
 * category=spatial and nothing else. So the join has to be made against the
 * install, and the install makes it by NAME. Measured, on the mount:
 *
 *   SFX_UI_Select_A_2D
 *     game/glacierportal/modbuilder/audio/modbuilder_sfx_ui_select_a_2d      (prefab)
 *     game/glacierportal/modbuilder/audio/gem_modbuilder_sfx_ui_select_a_2d  (gem)
 *     game/glacierportal/modbuilder/audio/bf03_portal_ui_select_a_2d_config_01
 *
 *   SFX_UI_MenuNavigation_Default_SecondarySelect_OneShot2D
 *     common/sound/portal/ui/menunavigation/default/
 *       bf03_portal_ui_menunavigation_default_secondaryselect-oneshot2d_config_01
 *
 * The three name shapes differ by decoration, not by content: an authoring
 * prefix (bf03_, portal_, modbuilder_, gem_, sfx_), a role suffix (_config_01,
 * _prefab_01, _gem_01, _wave_01) and the separator used between the last two
 * words (_ in the placeable, - in the asset). Strip the decoration and lower
 * the case and the two names are the SAME STRING. That is the join, and it is
 * a join on the game's own naming, not a guess at which sound sounds right.
 *
 * A placeable that does not join is REPORTED as unresolved with the key that
 * failed. A missing link is a finding; it is never papered over with a
 * nearest-neighbour match, because "nearly the right name" in this catalogue
 * means a different sound. */

namespace {

/* Strings handed out over the C ABI have to outlive the call. They are interned
 * in one process-wide pool: the UI catalogue is a few hundred short names, it is
 * read once, and the alternative - growing bf6_ctx for this - would collide with
 * every other edit in flight against that struct. */
std::mutex                 g_uis_pool_lock;
std::set<std::string>      g_uis_pool;

const char* uis_keep(const std::string& text)
{
    std::lock_guard<std::mutex> guard(g_uis_pool_lock);
    return g_uis_pool.insert(text).first->c_str();
}

std::string uis_lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return text;
}

std::string uis_leaf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool uis_ends_with(const std::string& text, const char* tail)
{
    const size_t n = std::strlen(tail);
    return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

bool uis_starts_with(const std::string& text, const char* head)
{
    const size_t n = std::strlen(head);
    return text.size() >= n && text.compare(0, n, head) == 0;
}

/* The join key: decoration removed, case folded, separators dropped. Applied
 * identically to an asset leaf and to a placeable type, which is the whole
 * point - the two only agree because the same reduction ran on both. */
std::string uis_key(std::string leaf)
{
    leaf = uis_lower(std::move(leaf));
    static const char* kSuffix[] = {
        "_config_01", "_prefab_01", "_gem_01", "_wave_01", "_soundstyle_01",
        "_event_01", "_config", "_prefab" };
    for (const char* tail : kSuffix)
        if (uis_ends_with(leaf, tail)) { leaf.resize(leaf.size() - std::strlen(tail)); break; }
    static const char* kPrefix[] = {
        "bf03_", "portal_", "modbuilder_", "gem_", "sfx_", "vo_", "amb_" };
    bool cut = true;
    while (cut) {
        cut = false;
        for (const char* head : kPrefix)
            if (uis_starts_with(leaf, head)) {
                leaf.erase(0, std::strlen(head));
                cut = true;
                break;
            }
    }
    std::string key;
    key.reserve(leaf.size());
    for (char c : leaf)
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) key.push_back(c);
    return key;
}

/* The mount's sound partitions, indexed once per context by join key. Only the
 * two trees that carry authored sound assets are walked, so this is a bounded
 * read and not a catalogue-wide scan. */
struct UiSoundIndex {
    std::vector<std::string>                       configs;   // *_config_01
    std::vector<std::string>                       prefabs;   // *_prefab_01 / modbuilder_*
    std::map<std::string, std::vector<size_t>>     by_key_config;
    std::map<std::string, std::vector<size_t>>     by_key_prefab;
    /* The mount can WIDEN under a live context: the Portal sound tree and every
     * SFX_* placeable's sound arrive only with the level archives, so a tool
     * that mounts levels lazily would otherwise keep reading an index built
     * before they existed. The sound-partition count is the signature; when it
     * moves, the index is rebuilt. */
    int  signature = -1;
    bool built = false;
};

std::mutex                                g_uis_index_lock;
std::map<bf6_ctx*, UiSoundIndex>          g_uis_index;

void uis_collect(bf6_ctx* c, const char* search, UiSoundIndex& index)
{
    const int total = bf6_list_ebx(c, search, nullptr, 0);
    if (total <= 0) return;
    std::vector<bf6_asset> rows;
    rows.resize(size_t(total));
    const int got = bf6_list_ebx(c, search, rows.data(), total);
    for (int i = 0; i < got; i++) {
        if (!rows[size_t(i)].name) continue;
        const std::string name = rows[size_t(i)].name;
        const std::string leaf = uis_leaf(name);
        if (uis_ends_with(leaf, "_config_01")) {
            index.by_key_config[uis_key(leaf)].push_back(index.configs.size());
            index.configs.push_back(name);
        } else if (uis_ends_with(leaf, "_prefab_01") ||
                   uis_starts_with(leaf, "modbuilder_")) {
            index.by_key_prefab[uis_key(leaf)].push_back(index.prefabs.size());
            index.prefabs.push_back(name);
        }
    }
}

UiSoundIndex& uis_index(bf6_ctx* c)
{
    std::lock_guard<std::mutex> guard(g_uis_index_lock);
    UiSoundIndex& index = g_uis_index[c];
    const int signature = bf6_list_ebx(c, "common/sound/", nullptr, 0);
    if (!index.built || index.signature != signature) {
        index = UiSoundIndex();
        index.built = true;
        index.signature = signature;
        uis_collect(c, "common/sound/ui/", index);
        uis_collect(c, "common/sound/portal/", index);
        uis_collect(c, "modbuilder/audio/", index);
    }
    return index;
}

/* Generalised read: the caller hands over ANY sound identity and the right
 * route is chosen from what the mount says it is, so a tool never has to know
 * whether it is holding a config partition or a wave resource. */
bf6::ui_runtime::InstalledSoundResult uis_read_any(
    bf6_ctx* c, const std::string& path, uint32_t variation, bool metadata_only)
{
    bf6_asset res{};
    if (bf6_list_res(c, path.c_str(), &res, 1) == 1 && res.name &&
        path == res.name && res.type == 0xB2C465F6u) {
        bf6::ui_runtime::InstalledSoundRequest request;
        request.wave_asset = path;
        request.variation_index = variation;
        request.metadata_only = metadata_only;
        return bf6::ui_runtime::ReadInstalledSoundWave(c, request);
    }
    bf6::ui_runtime::InstalledSoundConfigRequest request;
    request.config_asset = path;
    request.variation_index = variation;
    request.metadata_only = metadata_only;
    return bf6::ui_runtime::ReadInstalledSoundConfig(c, request);
}

float uis_duration(const bf6::ui_runtime::InstalledSoundWave& wave)
{
    return wave.sample_rate ? float(wave.sample_count) / float(wave.sample_rate) : 0.0f;
}

void uis_fill_row(bf6_ui_sound& row, const std::string& path,
                  const bf6::ui_runtime::InstalledSoundResult& read)
{
    row.name            = uis_keep(uis_leaf(path));
    row.ebx_path        = uis_keep(path);
    row.codec           = read.wave.codec;
    row.channels        = read.wave.channels;
    row.sample_rate     = int32_t(read.wave.sample_rate);
    row.variation_count = int32_t(read.wave.variation_count);
    row.duration_sec    = uis_duration(read.wave);
}

/* The tail of the asset name is what the game itself calls the event: the
 * default style ships ..._default_<event>_config_01, so "primaryactivation",
 * "goback", "focus", "actionfailed" are read off the authored names rather than
 * assigned by us. The Portal front end's own copies decorate the same events
 * with a -oneshot2d role suffix; that is stripped so both styles answer to the
 * same event name. */
std::string uis_event_name(const std::string& path, const char* style_marker)
{
    std::string leaf = uis_lower(uis_leaf(path));
    if (uis_ends_with(leaf, "_config_01")) leaf.resize(leaf.size() - 10);
    const size_t at = leaf.rfind(style_marker);
    if (at == std::string::npos) return std::string();
    std::string event = leaf.substr(at + std::strlen(style_marker));
    const size_t dash = event.rfind("-oneshot2d");
    if (dash != std::string::npos) event.resize(dash);
    const size_t dash3d = event.rfind("-oneshot3d");
    if (dash3d != std::string::npos) event.resize(dash3d);
    return event;
}

} // namespace

/* ------------------------------------------------------------------------- */

int bf6_ui_sound_list(bf6_ctx* c, bf6_ui_sound* out, int out_max)
{
    if (!c) return 0;
    const UiSoundIndex& index = uis_index(c);
    const int total = int(index.configs.size());
    if (!out || out_max <= 0) return total;

    int written = 0;
    for (size_t i = 0; i < index.configs.size() && written < out_max; i++) {
        const std::string& path = index.configs[i];
        const auto read = uis_read_any(c, path, 0, true);
        if (!read.ok()) continue;
        uis_fill_row(out[written++], path, read);
    }
    return written;
}

int bf6_ui_sound_style(bf6_ctx* c, bf6_ui_sound_event* out, int out_max)
{
    if (!c) return 0;
    const UiSoundIndex& index = uis_index(c);

    /* Both authored default styles: the front end's own
     * common/sound/ui/menunavigation/default/, which the UI sound route probe
     * pinned through the UM Main style player, and the Portal front end's
     * common/sound/portal/ui/menunavigation/default/. Portal names the two
     * select events the base style does not (primaryselect, secondaryselect),
     * so a tool that wants the Portal voice needs both listed. */
    std::vector<std::pair<std::string, std::string>> rows;
    for (const std::string& path : index.configs) {
        const char* marker = nullptr;
        if (path.find("common/sound/ui/menunavigation/default/") == 0)
            marker = "_default_";
        else if (path.find("common/sound/portal/ui/menunavigation/default/") == 0)
            marker = "_default_";
        else
            continue;
        const std::string event = uis_event_name(path, marker);
        if (event.empty()) continue;
        rows.emplace_back(event, path);
    }
    std::sort(rows.begin(), rows.end());

    const int total = int(rows.size());
    if (!out || out_max <= 0) return total;
    const int write = total < out_max ? total : out_max;
    for (int i = 0; i < write; i++) {
        out[i].event    = uis_keep(rows[size_t(i)].first);
        out[i].ebx_path = uis_keep(rows[size_t(i)].second);
    }
    return write;
}

int bf6_ui_sound_decode(bf6_ctx* c, const char* ebx_path, int variation_index,
                        int16_t* out_pcm16, int max_samples,
                        int* out_channels, int* out_rate)
{
    if (!c || !ebx_path || !*ebx_path || variation_index < 0) return -1;
    const bool sizing = (out_pcm16 == nullptr || max_samples <= 0);
    const auto read = uis_read_any(c, ebx_path, uint32_t(variation_index), sizing);
    if (!read.ok()) return -1;
    if (out_channels) *out_channels = read.wave.channels;
    if (out_rate)     *out_rate     = int(read.wave.sample_rate);
    if (sizing)
        return int(size_t(read.wave.sample_count) * size_t(read.wave.channels));
    const int have = int(read.wave.pcm.size());
    const int write = have < max_samples ? have : max_samples;
    std::memcpy(out_pcm16, read.wave.pcm.data(), size_t(write) * sizeof(int16_t));
    return write;
}

int bf6_sfx_placeable_resolve(bf6_ctx* c, const char* placeable_type,
                              bf6_sfx_placeable* out, int out_max)
{
    if (!c || !placeable_type || !*placeable_type) return 0;
    const UiSoundIndex& index = uis_index(c);
    const std::string type = placeable_type;
    const std::string key = uis_key(type);

    /* Loop and 2D/3D come off the placeable's own name, which is the catalogue's
     * convention and not an inference: ..._SimpleLoop3D loops and is placed in
     * the world, ..._OneShot2D fires once and is not. The catalogue carries no
     * other statement of either, so this is reported as what it is. */
    const std::string lower = uis_lower(type);
    const int32_t loop = lower.find("loop") != std::string::npos ? 1 : 0;
    const int32_t two_d = uis_ends_with(lower, "2d") ? 1
                        : uis_ends_with(lower, "3d") ? 0 : -1;

    std::string blueprint;
    auto pit = index.by_key_prefab.find(key);
    if (pit != index.by_key_prefab.end() && !pit->second.empty()) {
        /* An authored prefab beats a generated gem wrapper when both exist. */
        for (size_t i : pit->second) {
            const std::string& name = index.prefabs[i];
            if (blueprint.empty() ||
                uis_leaf(name).rfind("gem_", 0) != 0) blueprint = name;
        }
    }

    std::vector<std::string> sounds;
    auto cit = index.by_key_config.find(key);
    if (cit != index.by_key_config.end())
        for (size_t i : cit->second) sounds.push_back(index.configs[i]);

    /* No config joined by name. Fall back to the blueprint's own reflected
     * content: dump it and take the sound partitions it names. This is the
     * honest second route - it reads what the prefab actually points at rather
     * than guessing another name shape. */
    if (sounds.empty() && !blueprint.empty()) {
        std::vector<char> text(size_t(256) * 1024, 0);
        const int64_t used = bf6_ebx_dump(c, blueprint.c_str(), 4,
                                          text.data(), int(text.size()));
        if (used > 0) {
            const std::string dump(text.data(), size_t(used < int64_t(text.size())
                                                       ? used : int64_t(text.size() - 1)));
            size_t at = 0;
            while ((at = dump.find("_config_01", at)) != std::string::npos) {
                size_t start = at;
                while (start > 0) {
                    const char ch = dump[start - 1];
                    const bool token = (ch >= 'a' && ch <= 'z') ||
                                       (ch >= '0' && ch <= '9') ||
                                       ch == '_' || ch == '-' || ch == '/';
                    if (!token) break;
                    start--;
                }
                std::string found = dump.substr(start, at + 10 - start);
                if (found.find('/') != std::string::npos &&
                    std::find(sounds.begin(), sounds.end(), found) == sounds.end())
                    sounds.push_back(found);
                at += 10;
            }
        }
    }

    if (sounds.empty()) {
        if (out && out_max > 0) {
            bf6_sfx_placeable& row = out[0];
            row.placeable       = uis_keep(type);
            row.blueprint       = uis_keep(blueprint);
            row.sound_ebx       = uis_keep(std::string());
            row.variation_count = 0;
            row.loop            = loop;
            row.two_d           = two_d;
            row.channels        = 0;
            row.sample_rate     = 0;
            row.duration_sec    = 0.0f;
            row.status          = uis_keep("no-sound-asset-joins-key-" + key);
        }
        return 0;
    }

    std::sort(sounds.begin(), sounds.end());
    const int total = int(sounds.size());
    if (!out || out_max <= 0) return total;
    const int write = total < out_max ? total : out_max;
    for (int i = 0; i < write; i++) {
        const std::string& path = sounds[size_t(i)];
        const auto read = uis_read_any(c, path, 0, true);
        bf6_sfx_placeable& row = out[i];
        row.placeable       = uis_keep(type);
        row.blueprint       = uis_keep(blueprint);
        row.sound_ebx       = uis_keep(path);
        row.variation_count = int32_t(read.wave.variation_count);
        row.loop            = loop;
        row.two_d           = two_d;
        row.channels        = read.wave.channels;
        row.sample_rate     = int32_t(read.wave.sample_rate);
        row.duration_sec    = uis_duration(read.wave);
        row.status          = uis_keep(
            bf6::ui_runtime::InstalledSoundStatusName(read.status));
    }
    return write;
}

void bf6__uisound_forget(bf6_ctx* c)
{
    std::lock_guard<std::mutex> guard(g_uis_index_lock);
    g_uis_index.erase(c);
}
