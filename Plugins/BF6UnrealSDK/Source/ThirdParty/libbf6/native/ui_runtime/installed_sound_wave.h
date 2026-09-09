#pragma once

#include "bf6_core.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bf6::ui_runtime {

enum class InstalledSoundStatus {
    Ok,
    InvalidArgument,
    ConfigNotMounted,
    ConfigWaveMissing,
    ConfigWaveAmbiguous,
    AssetNotMounted,
    NotNewWaveResource,
    MalformedBank,
    VariationOutOfRange,
    SegmentOutOfRange,
    ChunkNotMounted,
    InvalidSps,
    UnsupportedCodec,
    DecodeFailed,
    NegativeControlFailed,
};

struct InstalledSoundRequest {
    // Exact NewWaveResource name reached from an authored sound-config Wave
    // PointerRef. The reader never searches for a nearby *_wave_* asset.
    std::string wave_asset;
    uint32_t variation_index = 0;
    uint32_t variation_segment_index = 0;
    // Stop once the authored SPS header has been read and its negative controls
    // have passed. Codec, channels, rate, sample count and variation count are
    // filled; pcm stays empty. This is what a catalogue listing wants: the same
    // proven route, without paying the sample decode for every row.
    bool metadata_only = false;
};

struct InstalledSoundConfigRequest {
    // Exact authored sound-config EBX path. The Wave ImportRef is resolved
    // directly from this partition; callers never carry a derived wave name.
    std::string config_asset;
    uint32_t variation_index = 0;
    uint32_t variation_segment_index = 0;
    bool metadata_only = false;
};

struct InstalledSoundControls {
    int exact_resource_matches = 0;
    int core_datasets = 0;
    int chunk_guid_matches = 0;
    int mutated_chunk_trials = 0;
    int mutated_chunk_hits = 0;
    int shifted_sps_hits = 0;
    int config_wave_imports = 0;
    int config_xor_import_hits = 0;
};

struct InstalledSoundWave {
    std::string wave_asset;
    std::string chunk_guid;
    uint8_t codec = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t sample_count = 0;
    uint32_t sps_header_size = 0;
    // How many variations the authored bank carries. Read straight off the
    // Variations dataset row count, so it is the game's own number and not a
    // count of what happened to decode.
    uint32_t variation_count = 0;
    uint64_t chunk_size = 0;
    uint64_t sample_offset = 0;
    std::vector<int16_t> pcm;
};

struct InstalledSoundResult {
    InstalledSoundStatus status = InstalledSoundStatus::InvalidArgument;
    InstalledSoundControls controls;
    InstalledSoundWave wave;
    std::string error;
    bool ok() const { return status == InstalledSoundStatus::Ok; }
};

// Directly reads the selected bank, authored chunk GUID and SPS block from the
// current mount. PCM16BE and XAS1 are decoded exactly. EA Layer3 remains an
// explicit unsupported-codec boundary; compressed bytes are never called PCM.
InstalledSoundResult ReadInstalledSoundWave(
    bf6_ctx* context, const InstalledSoundRequest& request);

// Resolves the config's reflected Wave/Asset ImportRef and then performs the
// same direct installed-bank read. The two currently authored field shapes
// (nested Wave=0x49D2E039 and typed Asset=0x73E9A894) must agree when both are
// present. A one-bit-perturbed field hash is measured as a control.
InstalledSoundResult ReadInstalledSoundConfig(
    bf6_ctx* context, const InstalledSoundConfigRequest& request);

const char* InstalledSoundStatusName(InstalledSoundStatus status);

// Minimal XAudio2 one-shot output for already decoded installed PCM. Samples
// are copied, so callers may release InstalledSoundResult as soon as Play
// returns. Starting another sound retracts the previous buffer.
class PcmOneShotPlayer {
public:
    PcmOneShotPlayer();
    ~PcmOneShotPlayer();
    PcmOneShotPlayer(const PcmOneShotPlayer&) = delete;
    PcmOneShotPlayer& operator=(const PcmOneShotPlayer&) = delete;

    bool Play(const InstalledSoundWave& wave, float volume, std::string& error);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bf6::ui_runtime
