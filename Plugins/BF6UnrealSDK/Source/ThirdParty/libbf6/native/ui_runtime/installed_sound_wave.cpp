#include "installed_sound_wave.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#endif

namespace bf6::ui_runtime {
namespace {

#ifdef _WIN32
std::string xaudio_error(const char* operation, HRESULT hr) {
    char text[96]{};
    std::snprintf(text, sizeof(text), "%s failed (HRESULT 0x%08lx)",
                  operation, static_cast<unsigned long>(hr));
    return text;
}
#endif

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | p[3];
}

uint32_t harmony_hash(const char* text) {
    uint32_t value = 5381;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p; ++p)
        value = value * 33u ^ *p;
    return value;
}

struct Field {
    uint8_t data_type = 0;
    uint8_t store_type = 0;
    uint16_t param1 = 0;
    uint64_t param2 = 0;
    uint32_t table = 0;
    uint32_t data = 0;
    std::vector<uint64_t> values;
};
struct Dataset {
    uint32_t data = 0;
    uint32_t count = 0;
    std::unordered_map<uint32_t, Field> fields;
};

bool in_range(size_t at, size_t bytes, size_t size) {
    return at <= size && bytes <= size - at;
}

bool parse_field(const std::vector<uint8_t>& bank, size_t at,
                 uint32_t count, uint32_t data, Field& out) {
    if (!in_range(at, 24, bank.size()) || count > 10000000u) return false;
    out.data_type = bank[at + 4];
    out.store_type = bank[at + 5];
    out.param1 = le16(bank.data() + at + 6);
    out.param2 = le64(bank.data() + at + 8);
    out.table = le32(bank.data() + at + 16);
    out.data = data;
    out.values.clear();
    out.values.reserve(count);
    if (out.store_type == 0) {
        out.values.assign(count, out.param2);
    } else if (out.store_type == 1) {
        const int16_t delta = static_cast<int16_t>(out.param1);
        for (uint32_t i = 0; i < count; ++i)
            out.values.push_back(out.param2 + int64_t(delta) * i);
    } else if (out.store_type == 2) {
        const unsigned shift = out.param1 & 0xffu;
        const unsigned width = out.param1 >> 8;
        if (shift >= 64 || (width != 1 && width != 2 && width != 4 && width != 8))
            return false;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t pos = size_t(out.table) + size_t(width) * i;
            if (!in_range(pos, width, bank.size())) return false;
            uint64_t raw = width == 1 ? bank[pos] : width == 2 ? le16(bank.data() + pos) :
                           width == 4 ? le32(bank.data() + pos) : le64(bank.data() + pos);
            out.values.push_back((raw << shift) + out.param2);
        }
    } else if (out.store_type == 3) {
        const unsigned bits = out.param1 & 0xffu;
        const unsigned width = out.param1 >> 8;
        const uint64_t unique = out.param2;
        if (!bits || bits > 8 || unique > 256 ||
            (width != 1 && width != 2 && width != 4 && width != 8)) return false;
        const size_t index_at = size_t(out.table) + size_t(unique) * width;
        for (uint32_t i = 0; i < count; ++i) {
            const size_t bit = size_t(i) * bits;
            if (!in_range(index_at + bit / 8, 1, bank.size())) return false;
            const unsigned index = (bank[index_at + bit / 8] >> (bit % 8)) &
                                   ((1u << bits) - 1u);
            if (index >= unique) return false;
            const size_t pos = size_t(out.table) + size_t(index) * width;
            if (!in_range(pos, width, bank.size())) return false;
            out.values.push_back(width == 1 ? bank[pos] : width == 2 ? le16(bank.data() + pos) :
                                 width == 4 ? le32(bank.data() + pos) : le64(bank.data() + pos));
        }
    } else if (out.store_type == 4) {
        for (uint32_t i = 0; i < count; ++i) {
            const size_t pos = size_t(out.table) + size_t(i) * 8;
            if (!in_range(pos, 8, bank.size())) return false;
            out.values.push_back(le64(bank.data() + pos));
        }
    } else return false;
    return true;
}

bool parse_dataset(const std::vector<uint8_t>& bank, size_t at,
                   uint32_t& id, Dataset& out) {
    if (!in_range(at, 0x48, bank.size()) ||
        std::memcmp(bank.data() + at, "TESD", 4) != 0) return false;
    id = le32(bank.data() + at + 8);
    out.data = le32(bank.data() + at + 0x18);
    out.count = le32(bank.data() + at + 0x38);
    const uint16_t fields = le16(bank.data() + at + 0x3c);
    if (fields > 4096 || !in_range(at + 0x48, size_t(fields) * 24, bank.size()))
        return false;
    for (uint16_t i = 0; i < fields; ++i) {
        const size_t fp = at + 0x48 + size_t(i) * 24;
        const uint32_t field_id = le32(bank.data() + fp);
        Field field;
        if (!parse_field(bank, fp, out.count, out.data, field) ||
            !out.fields.emplace(field_id, std::move(field)).second) return false;
    }
    return true;
}

const Field* field(const Dataset& data, const char* name) {
    auto it = data.fields.find(harmony_hash(name));
    return it == data.fields.end() ? nullptr : &it->second;
}

std::string hex_guid(const uint8_t* p, bool canonical) {
    char text[33]{};
    if (canonical) {
        std::snprintf(text, sizeof(text), "%08x%04x%04x", le32(p), le16(p + 4), le16(p + 6));
        for (int i = 8; i < 16; ++i) std::snprintf(text + 16 + (i - 8) * 2, 3, "%02x", p[i]);
    } else {
        for (int i = 0; i < 16; ++i) std::snprintf(text + i * 2, 3, "%02x", p[i]);
    }
    return text;
}

bool sps_header(const std::vector<uint8_t>& chunk, size_t at,
                uint8_t& codec, uint16_t& channels,
                uint32_t& rate, uint32_t& samples, uint32_t& header) {
    if (!in_range(at, 16, chunk.size())) return false;
    header = be32(chunk.data() + at) & 0xffffffu;
    codec = chunk[at + 4];
    channels = uint16_t((chunk[at + 5] >> 2) + 1);
    rate = be16(chunk.data() + at + 6);
    samples = be32(chunk.data() + at + 8) & 0xffffffu;
    return (codec == 0x12 || codec == 0x14 || codec == 0x16) &&
           channels >= 1 && channels <= 8 && rate >= 8000 && rate <= 192000 &&
           samples > 0 && header >= 8 && at + header <= chunk.size();
}

int16_t clamp16(float value) {
    return static_cast<int16_t>((std::max)(-32768.0f, (std::min)(32767.0f, value)));
}

bool decode_sps(const std::vector<uint8_t>& chunk, size_t at,
                InstalledSoundWave& wave) {
    if (wave.codec != 0x12 && wave.codec != 0x14) return false;
    size_t pos = at + wave.sps_header_size;
    size_t decoded_frames = 0;
    bool saw_data = false, saw_end = false;
    wave.pcm.clear();
    constexpr size_t kBlockBytes = 76, kBlockSamples = 128;
    static constexpr float coefs[16][2] = {
        {0, 0}, {0.9375f, 0}, {1.796875f, -0.8125f},
        {1.53125f, -0.859375f},
        {0, 0}, {0, 0}, {0, 0}, {0, 0},
        {0, 0}, {0, 0}, {0, 0}, {0, 0},
        {0, 0}, {0, 0}, {0, 0}, {0, 0}};
    while (in_range(pos, 4, chunk.size())) {
        const uint8_t tag = chunk[pos];
        const size_t block_size = be32(chunk.data() + pos) & 0xffffffu;
        if (block_size < 4 || !in_range(pos, block_size, chunk.size())) return false;
        if (tag == 0x45) {
            if (block_size != 4) return false;
            saw_end = true;
            pos += block_size;
            break;
        }
        if (tag != 0x44 || block_size < 8) return false;
        const size_t block_frames = be32(chunk.data() + pos + 4);
        if (!block_frames || block_frames > wave.sample_count - decoded_frames) return false;
        const uint8_t* body = chunk.data() + pos + 8;
        const size_t body_bytes = block_size - 8;
        const size_t block_samples = block_frames * wave.channels;
        const size_t output_base = wave.pcm.size();
        wave.pcm.resize(output_base + block_samples);
        if (wave.codec == 0x12) {
            if (body_bytes != block_samples * 2) return false;
            for (size_t i = 0; i < block_samples; ++i)
                wave.pcm[output_base + i] =
                    static_cast<int16_t>(be16(body + i * 2));
        } else {
            const size_t blocks = (block_frames + kBlockSamples - 1) / kBlockSamples;
            if (body_bytes != blocks * kBlockBytes * wave.channels) return false;
            for (size_t block = 0; block < blocks; ++block) {
                for (uint16_t channel = 0; channel < wave.channels; ++channel) {
                    const uint8_t* frame = body +
                        (block * wave.channels + channel) * kBlockBytes;
                    size_t sample = block * kBlockSamples;
                    for (int group = 0; group < 4; ++group) {
                        const uint32_t h = le32(frame + group * 4);
                        const unsigned ci = h & 0xfu;
                        int16_t hist2 = static_cast<int16_t>(h & 0xfff0u);
                        int16_t hist1 = static_cast<int16_t>((h >> 16) & 0xfff0u);
                        const int shift = int((h >> 16) & 0xfu);
                        for (int k = 0; k < 32; ++k) {
                            int16_t value = k == 0 ? hist2 : hist1;
                            if (k >= 2) {
                                const uint8_t packed =
                                    frame[0x10 + ((k - 2) >> 1) * 4 + group];
                                const int nibble = ((k - 2) & 1)
                                    ? packed & 0xf : packed >> 4;
                                value = clamp16(
                                    float(static_cast<int16_t>(nibble << 12) >> shift) +
                                    hist1 * coefs[ci][0] + hist2 * coefs[ci][1]);
                                hist2 = hist1;
                                hist1 = value;
                            }
                            const size_t index = sample + size_t(k);
                            if (index < block_frames)
                                wave.pcm[output_base + index * wave.channels + channel] = value;
                        }
                        sample += 32;
                    }
                }
            }
        }
        saw_data = true;
        decoded_frames += block_frames;
        pos += block_size;
    }
    return saw_data && saw_end && decoded_frames == wave.sample_count &&
           wave.pcm.size() == size_t(wave.sample_count) * wave.channels;
}

} // namespace

InstalledSoundResult ReadInstalledSoundConfig(
    bf6_ctx* context, const InstalledSoundConfigRequest& request) {
    InstalledSoundResult result;
    if (!context || request.config_asset.empty()) {
        result.error = "sound config request has no exact EBX identity";
        return result;
    }
    bf6_asset config{};
    if (bf6_list_ebx(context, request.config_asset.c_str(), &config, 1) != 1 ||
        !config.name || request.config_asset != config.name) {
        result.status = InstalledSoundStatus::ConfigNotMounted;
        result.error = "exact sound config EBX is not mounted";
        return result;
    }

    constexpr uint32_t kWave = 0x49D2E039u;
    constexpr uint32_t kAsset = 0x73E9A894u;
    constexpr int kPathStride = 512;
    auto imports = [&](uint32_t hash, std::vector<std::string>& paths) -> int {
        const int count = bf6_ebx_imports_by_field(
            context, request.config_asset.c_str(), hash, nullptr, kPathStride, 0);
        if (count <= 0) return count;
        std::vector<char> rows(size_t(count) * kPathStride, 0);
        const int got = bf6_ebx_imports_by_field(
            context, request.config_asset.c_str(), hash,
            rows.data(), kPathStride, count);
        if (got != count) return -1;
        for (int i = 0; i < count; ++i)
            paths.emplace_back(rows.data() + size_t(i) * kPathStride);
        return count;
    };

    std::vector<std::string> paths;
    const int wave_count = imports(kWave, paths);
    const int asset_count = imports(kAsset, paths);
    std::vector<std::string> control_paths;
    const int wave_control = imports(kWave ^ 1u, control_paths);
    const int asset_control = imports(kAsset ^ 1u, control_paths);
    if (wave_count < 0 || asset_count < 0 || wave_control < 0 || asset_control < 0) {
        result.status = InstalledSoundStatus::ConfigWaveMissing;
        result.error = "sound config reflection/import decode failed";
        return result;
    }
    result.controls.config_wave_imports = wave_count + asset_count;
    result.controls.config_xor_import_hits = wave_control + asset_control;
    if (result.controls.config_xor_import_hits != 0) {
        result.status = InstalledSoundStatus::NegativeControlFailed;
        result.error = "one-bit-perturbed config field hash selected an import";
        return result;
    }
    std::set<std::string> unique;
    for (std::string path : paths) {
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ebx") == 0)
            path.resize(path.size() - 4);
        if (!path.empty()) unique.insert(std::move(path));
    }
    if (unique.empty()) {
        result.status = InstalledSoundStatus::ConfigWaveMissing;
        result.error = "sound config has no typed Wave/Asset ImportRef";
        return result;
    }
    if (unique.size() != 1) {
        result.status = InstalledSoundStatus::ConfigWaveAmbiguous;
        result.error = "sound config Wave/Asset ImportRefs disagree";
        return result;
    }

    InstalledSoundRequest wave_request;
    wave_request.wave_asset = *unique.begin();
    wave_request.variation_index = request.variation_index;
    wave_request.variation_segment_index = request.variation_segment_index;
    wave_request.metadata_only = request.metadata_only;
    result = ReadInstalledSoundWave(context, wave_request);
    result.controls.config_wave_imports = wave_count + asset_count;
    result.controls.config_xor_import_hits = wave_control + asset_control;
    return result;
}

InstalledSoundResult ReadInstalledSoundWave(
    bf6_ctx* context, const InstalledSoundRequest& request) {
    InstalledSoundResult result;
    result.wave.wave_asset = request.wave_asset;
    if (!context || request.wave_asset.empty()) {
        result.error = "sound request has no exact resource identity";
        return result;
    }
    bf6_asset asset{};
    if (bf6_list_res(context, request.wave_asset.c_str(), &asset, 1) != 1 ||
        !asset.name || request.wave_asset != asset.name) {
        result.status = InstalledSoundStatus::AssetNotMounted;
        result.error = "exact NewWaveResource is not mounted";
        return result;
    }
    result.controls.exact_resource_matches = 1;
    if (asset.type != 0xB2C465F6u) {
        result.status = InstalledSoundStatus::NotNewWaveResource;
        result.error = "exact asset is not a NewWaveResource";
        return result;
    }
    const uint8_t* raw = nullptr;
    const int64_t raw_size = bf6_read_raw(
        context, BF6_RAW_RES, request.wave_asset.c_str(), &raw);
    if (raw_size < 0 || !raw) {
        result.status = InstalledSoundStatus::AssetNotMounted;
        result.error = "NewWaveResource bytes could not be read";
        return result;
    }
    std::vector<uint8_t> bank(raw, raw + raw_size);
    if (bank.size() < 0x28 || std::memcmp(bank.data(), "SBle", 4) != 0) {
        result.status = InstalledSoundStatus::MalformedBank;
        result.error = "NewWaveResource is not an SBle sample bank";
        return result;
    }
    const uint16_t dataset_count = le16(bank.data() + 0x0a);
    const uint32_t table = le32(bank.data() + 0x18);
    if (!dataset_count || dataset_count > 128 ||
        !in_range(table, size_t(dataset_count) * 8, bank.size())) {
        result.status = InstalledSoundStatus::MalformedBank;
        result.error = "SBle dataset table is malformed";
        return result;
    }
    std::unordered_map<uint32_t, Dataset> datasets;
    for (uint16_t i = 0; i < dataset_count; ++i) {
        uint32_t id = 0; Dataset data;
        if (!parse_dataset(bank, le32(bank.data() + table + size_t(i) * 8), id, data) ||
            !datasets.emplace(id, std::move(data)).second) {
            result.status = InstalledSoundStatus::MalformedBank;
            result.error = "SBle dataset decode failed";
            return result;
        }
    }
    auto get = [&](const char* name) -> const Dataset* {
        auto it = datasets.find(harmony_hash(name));
        return it == datasets.end() ? nullptr : &it->second;
    };
    const Dataset* chunks = get("Chunks");
    const Dataset* segments = get("Segments");
    const Dataset* variations = get("Variations");
    if (!chunks || !segments || !variations) {
        result.status = InstalledSoundStatus::MalformedBank;
        result.error = "SBle lacks Chunks/Segments/Variations";
        return result;
    }
    result.controls.core_datasets = 3;
    result.wave.variation_count = variations->count;
    const Field* chunk_ids = field(*chunks, "ChunkId");
    const Field* sample_offsets = field(*segments, "SamplesOffset");
    const Field* memory_indices = field(*variations, "MemoryChunkIndex");
    const Field* stream_indices = field(*variations, "StreamChunkIndex");
    const Field* first_segments = field(*variations, "FirstSegmentIndex");
    const Field* segment_counts = field(*variations, "SegmentCount");
    if (!chunk_ids || !sample_offsets || !memory_indices || !stream_indices ||
        !first_segments || !segment_counts) {
        result.status = InstalledSoundStatus::MalformedBank;
        result.error = "SBle lacks required named fields";
        return result;
    }
    if (request.variation_index >= variations->count) {
        result.status = InstalledSoundStatus::VariationOutOfRange;
        result.error = "variation index exceeds authored array";
        return result;
    }
    const size_t vi = request.variation_index;
    const uint64_t memory = memory_indices->values[vi];
    const uint64_t stream = stream_indices->values[vi];
    const uint64_t chunk_index = memory & 1u ? memory >> 1 : stream >> 1;
    const uint64_t first = first_segments->values[vi];
    const uint64_t count = segment_counts->values[vi];
    if (request.variation_segment_index >= count || first + request.variation_segment_index >= segments->count) {
        result.status = InstalledSoundStatus::SegmentOutOfRange;
        result.error = "segment index exceeds authored variation";
        return result;
    }
    if (chunk_index >= chunks->count || chunk_index >= chunk_ids->values.size()) {
        result.status = InstalledSoundStatus::MalformedBank;
        result.error = "variation selects a missing chunk";
        return result;
    }
    const uint64_t pointer = chunk_ids->values[size_t(chunk_index)];
    const uint64_t guid_at = uint64_t(chunk_ids->data) + pointer - 1u;
    if (pointer == 0 || guid_at > bank.size() || 16 > bank.size() - size_t(guid_at)) {
        result.status = InstalledSoundStatus::MalformedBank;
        result.error = "authored chunk GUID pointer is out of range";
        return result;
    }
    const std::string canonical = hex_guid(bank.data() + guid_at, true);
    const std::string direct = hex_guid(bank.data() + guid_at, false);
    const uint8_t* chunk_raw = nullptr;
    int64_t chunk_size = bf6_read_raw(context, BF6_RAW_CHUNK, canonical.c_str(), &chunk_raw);
    result.wave.chunk_guid = canonical;
    if (chunk_size < 0 || !chunk_raw) {
        chunk_size = bf6_read_raw(context, BF6_RAW_CHUNK, direct.c_str(), &chunk_raw);
        result.wave.chunk_guid = direct;
    }
    if (chunk_size < 0 || !chunk_raw) {
        result.status = InstalledSoundStatus::ChunkNotMounted;
        result.error = "authored audio chunk is not mounted";
        return result;
    }
    result.controls.chunk_guid_matches = 1;
    std::vector<uint8_t> chunk(chunk_raw, chunk_raw + chunk_size);
    result.wave.chunk_size = static_cast<uint64_t>(chunk.size());
    std::string mutated = result.wave.chunk_guid;
    mutated.back() = mutated.back() == '0' ? '1' : '0';
    const uint8_t* control_raw = nullptr;
    result.controls.mutated_chunk_trials = 1;
    if (bf6_read_raw(context, BF6_RAW_CHUNK, mutated.c_str(), &control_raw) >= 0)
        result.controls.mutated_chunk_hits = 1;
    const size_t si = size_t(first + request.variation_segment_index);
    const size_t offset = size_t(sample_offsets->values[si] & ~uint64_t(3));
    result.wave.sample_offset = static_cast<uint64_t>(offset);
    uint32_t header = 0;
    if (!sps_header(chunk, offset, result.wave.codec, result.wave.channels,
                    result.wave.sample_rate, result.wave.sample_count, header)) {
        result.status = InstalledSoundStatus::InvalidSps;
        result.error = "authored SamplesOffset has no plausible SPS header";
        return result;
    }
    result.wave.sps_header_size = header;
    uint8_t cc = 0; uint16_t ch = 0; uint32_t rr = 0, ss = 0, hh = 0;
    result.controls.shifted_sps_hits =
        sps_header(chunk, offset + 4, cc, ch, rr, ss, hh) ? 1 : 0;
    if (result.controls.mutated_chunk_hits || result.controls.shifted_sps_hits) {
        result.status = InstalledSoundStatus::NegativeControlFailed;
        result.error = "audio chunk or SPS negative control matched";
        return result;
    }
    if (request.metadata_only) {
        // Every control above has already run. What is skipped is only the
        // sample decode, so a metadata row is as trustworthy as a decoded one
        // about codec, channels, rate and length.
        result.status = InstalledSoundStatus::Ok;
        return result;
    }
    if (result.wave.codec == 0x16) {
        result.status = InstalledSoundStatus::UnsupportedCodec;
        result.error = "EA Layer3 sample reconstruction is not implemented";
        return result;
    }
    if (!decode_sps(chunk, offset, result.wave) || result.wave.pcm.empty()) {
        result.status = InstalledSoundStatus::DecodeFailed;
        result.error = "SPS sample decode failed";
        return result;
    }
    result.status = InstalledSoundStatus::Ok;
    return result;
}

const char* InstalledSoundStatusName(InstalledSoundStatus status) {
    switch (status) {
    case InstalledSoundStatus::Ok: return "ok";
    case InstalledSoundStatus::InvalidArgument: return "invalid-argument";
    case InstalledSoundStatus::ConfigNotMounted: return "config-not-mounted";
    case InstalledSoundStatus::ConfigWaveMissing: return "config-wave-missing";
    case InstalledSoundStatus::ConfigWaveAmbiguous: return "config-wave-ambiguous";
    case InstalledSoundStatus::AssetNotMounted: return "asset-not-mounted";
    case InstalledSoundStatus::NotNewWaveResource: return "not-newwave-resource";
    case InstalledSoundStatus::MalformedBank: return "malformed-bank";
    case InstalledSoundStatus::VariationOutOfRange: return "variation-out-of-range";
    case InstalledSoundStatus::SegmentOutOfRange: return "segment-out-of-range";
    case InstalledSoundStatus::ChunkNotMounted: return "chunk-not-mounted";
    case InstalledSoundStatus::InvalidSps: return "invalid-sps";
    case InstalledSoundStatus::UnsupportedCodec: return "unsupported-codec";
    case InstalledSoundStatus::DecodeFailed: return "decode-failed";
    case InstalledSoundStatus::NegativeControlFailed: return "negative-control-failed";
    }
    return "unknown";
}

struct PcmOneShotPlayer::Impl {
#ifdef _WIN32
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* mastering = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    bool com_owned = false;
#endif
    std::vector<int16_t> samples;
};

PcmOneShotPlayer::PcmOneShotPlayer() : impl_(std::make_unique<Impl>()) {}
PcmOneShotPlayer::~PcmOneShotPlayer() {
    Stop();
#ifdef _WIN32
    if (impl_ && impl_->mastering) {
        impl_->mastering->DestroyVoice();
        impl_->mastering = nullptr;
    }
    if (impl_ && impl_->engine) {
        impl_->engine->Release();
        impl_->engine = nullptr;
    }
    if (impl_ && impl_->com_owned) {
        CoUninitialize();
        impl_->com_owned = false;
    }
#endif
}

void PcmOneShotPlayer::Stop() {
#ifdef _WIN32
    if (impl_ && impl_->source) {
        impl_->source->Stop();
        impl_->source->FlushSourceBuffers();
        impl_->source->DestroyVoice();
        impl_->source = nullptr;
    }
#endif
    if (impl_) impl_->samples.clear();
}

bool PcmOneShotPlayer::Play(const InstalledSoundWave& wave, float volume,
                            std::string& error) {
    Stop();
    if (!impl_ || wave.pcm.empty() || !wave.channels || !wave.sample_rate) {
        error = "decoded PCM is empty";
        return false;
    }
#ifdef _WIN32
    volume = (std::max)(0.0f, (std::min)(1.0f, volume));
    if (!impl_->engine) {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com)) impl_->com_owned = true;
        else if (com != RPC_E_CHANGED_MODE) {
            error = xaudio_error("COM initialization for XAudio2", com);
            return false;
        }
        HRESULT hr = XAudio2Create(&impl_->engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) { error = xaudio_error("XAudio2Create", hr); return false; }
        hr = impl_->engine->CreateMasteringVoice(&impl_->mastering);
        if (FAILED(hr)) {
            impl_->engine->Release();
            impl_->engine = nullptr;
            error = xaudio_error("XAudio2 mastering voice creation", hr);
            return false;
        }
    }
    impl_->samples = wave.pcm;
    const size_t byte_count = impl_->samples.size() * sizeof(int16_t);
    if (byte_count > (std::numeric_limits<UINT32>::max)()) {
        error = "decoded PCM exceeds XAudio2 one-shot buffer limit";
        Stop();
        return false;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = wave.channels;
    format.nSamplesPerSec = wave.sample_rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = WORD(format.nChannels * 2);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    HRESULT hr = impl_->engine->CreateSourceVoice(&impl_->source, &format);
    if (FAILED(hr)) { error = xaudio_error("XAudio2 source voice creation", hr); Stop(); return false; }
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(byte_count);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(impl_->samples.data());
    hr = impl_->source->SetVolume(volume);
    if (SUCCEEDED(hr)) hr = impl_->source->SubmitSourceBuffer(&buffer);
    if (SUCCEEDED(hr)) hr = impl_->source->Start();
    if (FAILED(hr)) { error = xaudio_error("XAudio2 one-shot submit/start", hr); Stop(); return false; }
    error.clear();
    return true;
#else
    (void)volume;
    error = "native PCM output is available only on Windows";
    return false;
#endif
}

} // namespace bf6::ui_runtime
