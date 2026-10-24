#include "galaxy/menu_sounds.h"

// Host synthesis of the game's system sound effects (see menu_sounds.h).
//
// What runs on the game's CPU for a sound effect is restated here from the SMG decompilation's JAudio2
// (JASSeqParser, JASSeqCtrl, JASTrack, JASChannel, JASOscillator, JASLfo, JASDSPChannel, JASDsp::TChannel,
// JASBasicInst, JASDrumSet, JASBNKParser Ver1, JASWSParser) and the game's channelStart/noteOn overrides. Each
// subframe (80 samples at the DAC rate) does what JASDriver::updateDSP does: the 64 DSP channel updates
// (envelopes, mixer volumes, pitch) into channel records, the DSP mix of that subframe, then the sequence
// tick. The DSP's part is the runtime's own JAudioHostMixer (the game's ucode restated as host code) running on
// private memory that holds the channel records, the resampler and ADPCM tables and the waves; its output is
// what the game's DSP would write to the DAC buffer for that sound alone.
//
// Checked against the game itself (the runtime's GALAXY_AUDIO_WAV_DUMP of its output, the native DSP ucode,
// MR::startSystemSE with every other category muted): the CPU side matches operation for operation, so every
// channel's pitch, volumes and timing are the game's; JAudioHostMixer differs from the real ucode by at most a
// unit or two before the output gain at a few samples near note starts. Six of the fifteen sounds compared are
// bit-identical, the rest within 65-92 dB SNR, and every length and peak sample is the game's.

#include "galaxy/gx/game_asset_extract.h"
#include "galaxy/gx/game_disc.h"
#include "galaxy/jaudio_host_mixer.h"
#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>
#endif

namespace galaxy::menu_sounds {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

// ---- Driver constants (JASDriver for RMGE01: 32 kHz output, 7 subframes of 80 samples a frame) -----------
constexpr float kDacRate = 32000.0f * 1.0008897f;
constexpr u32 kSubframeSamples = 80u;
constexpr u32 kSubframesPerFrame = 7u;
constexpr u16 kChannelLevel = 0x2EE0;  // JASDriver::MAX_MIXERLEVEL
// DsetMixerLevel(4 * AudParams::outputGain (0.9)): DSP_MIXERLEVEL = 4096 * 3.6, truncated.
constexpr u16 kDspMixerLevel = 14745;
constexpr u32 kOutputMode = 1u;  // stereo (the surround setting mixes these sounds the same way)
constexpr u32 kDspChannels = 64u;

// The default of RenderOptions::sound_volume_scale: the JAISe's sound-parameter volume per unit of the sound
// table's volume byte, the byte / 255 times AudParams::masterVolSe (1.2) times the system category's volume
// (scCtgVolume, 1.2). Checked against the game's output: every menu sound's peak sample is the capture's.
constexpr float kDefaultSoundVolumeScale = 1.2f * 1.2f / 255.0f;

// The DSP's ADPCM coefficient pairs (JASDsp::DSPADPCM_FILTER, the standard AFC table).
constexpr std::array<s16, 32> kAdpcmFilter{
    0x0000, 0x0000, 0x0800, 0x0000, 0x0000, 0x0800, 0x0400, 0x0400, 0x1000, -0x0800, 0x0E00,
    -0x0600, 0x0C00, -0x0400, 0x1200, -0x0A00, 0x1068, -0x08C8, 0x12C0, -0x08FC, 0x1400, -0x0C00,
    0x0800, -0x0800, 0x0400, -0x0400, -0x0400, 0x0400, -0x0400, 0x0000, -0x0800, 0x0000,
};

// The DSP's 64-phase 4-tap resampler (the first 256 words of JASDsp::DSPRES_FILTER; the same table the
// runtime's own DSP paths use, see native_host.cpp).
constexpr std::array<std::array<s16, 4>, 64> kResamplerFilter{{
    {{3129, 26285, 3398, -33}},   {{2873, 26262, 3679, -40}},   {{2628, 26217, 3971, -48}},
    {{2394, 26150, 4276, -56}},   {{2173, 26061, 4592, -65}},   {{1963, 25950, 4920, -74}},
    {{1764, 25817, 5260, -84}},   {{1576, 25663, 5611, -95}},   {{1399, 25487, 5974, -106}},
    {{1233, 25291, 6347, -118}},  {{1077, 25075, 6732, -130}},  {{932, 24838, 7127, -143}},
    {{796, 24583, 7532, -156}},   {{671, 24309, 7947, -170}},   {{554, 24016, 8371, -184}},
    {{446, 23706, 8804, -198}},   {{347, 23379, 9246, -212}},   {{257, 23036, 9696, -226}},
    {{174, 22678, 10153, -240}},  {{99, 22304, 10618, -254}},   {{31, 21917, 11088, -268}},
    {{-30, 21517, 11564, -280}},  {{-84, 21104, 12045, -293}},  {{-132, 20679, 12531, -304}},
    {{-173, 20244, 13020, -314}}, {{-210, 19799, 13512, -323}}, {{-241, 19345, 14006, -330}},
    {{-267, 18882, 14501, -336}}, {{-289, 18413, 14997, -340}}, {{-306, 17937, 15493, -341}},
    {{-320, 17456, 15988, -340}}, {{-330, 16970, 16480, -337}}, {{-337, 16480, 16970, -330}},
    {{-340, 15988, 17456, -320}}, {{-341, 15493, 17937, -306}}, {{-340, 14997, 18413, -289}},
    {{-336, 14501, 18882, -267}}, {{-330, 14006, 19345, -241}}, {{-323, 13512, 19799, -210}},
    {{-314, 13020, 20244, -173}}, {{-304, 12531, 20679, -132}}, {{-293, 12045, 21104, -84}},
    {{-280, 11564, 21517, -30}},  {{-268, 11088, 21917, 31}},   {{-254, 10618, 22304, 99}},
    {{-240, 10153, 22678, 174}},  {{-226, 9696, 23036, 257}},   {{-212, 9246, 23379, 347}},
    {{-198, 8804, 23706, 446}},   {{-184, 8371, 24016, 554}},   {{-170, 7947, 24309, 671}},
    {{-156, 7532, 24583, 796}},   {{-143, 7127, 24838, 932}},   {{-130, 6732, 25075, 1077}},
    {{-118, 6347, 25291, 1233}},  {{-106, 5974, 25487, 1399}},  {{-95, 5611, 25663, 1576}},
    {{-84, 5260, 25817, 1764}},   {{-74, 4920, 25950, 1963}},   {{-65, 4592, 26061, 2173}},
    {{-56, 4276, 26150, 2394}},   {{-48, 3971, 26217, 2628}},   {{-40, 3679, 26262, 2873}},
    {{-33, 3398, 26285, 3129}},
}};

// ---- Big-endian reads with bounds (a malformed archive reads zeros, never out of range) ------------------
struct Bytes {
    const std::byte* data = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool has(std::size_t offset, std::size_t count) const noexcept {
        return offset <= size && count <= size - offset;
    }
    [[nodiscard]] u8 u8at(std::size_t o) const noexcept {
        return has(o, 1) ? static_cast<u8>(data[o]) : 0u;
    }
    [[nodiscard]] u16 u16at(std::size_t o) const noexcept {
        return static_cast<u16>((u8at(o) << 8u) | u8at(o + 1u));
    }
    [[nodiscard]] u32 u32at(std::size_t o) const noexcept {
        return (static_cast<u32>(u16at(o)) << 16u) | u16at(o + 2u);
    }
    [[nodiscard]] float f32at(std::size_t o) const noexcept {
        const u32 bits = u32at(o);
        float value;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
};

constexpr u32 tag(const char (&t)[5]) {
    return (static_cast<u32>(static_cast<u8>(t[0])) << 24u) | (static_cast<u32>(static_cast<u8>(t[1])) << 16u) |
        (static_cast<u32>(static_cast<u8>(t[2])) << 8u) | static_cast<u32>(static_cast<u8>(t[3]));
}

// ---- Instrument data --------------------------------------------------------------------------------------

// JASOscillator::Data. Tables are runs of (mode, time, value) points in host order, null when absent.
struct OscData {
    u32 target = 0;
    float rate = 1.0f;
    const s16* table = nullptr;
    const s16* rel_table = nullptr;
    float scale = 1.0f;
    float offset = 0.0f;
};

struct Effect {
    bool sense = false;  // JASInstSense, else JASInstRand
    u8 type = 0;         // 0 volume, 1 pitch, 2 pan, 3 fxmix, 4 dolby
    float base = 0.0f, range = 0.0f;  // Rand
    u8 sense_type = 0, max_point = 0;
    float start_level = 0.0f, end_level = 0.0f;  // Sense
};

struct VeloRegion {
    u8 max_velocity = 0;
    u16 wave_id = 0;
    float volume = 1.0f;
    float pitch = 1.0f;
};

struct KeyRegion {
    u8 high_key = 0;
    std::vector<VeloRegion> velos;
};

struct Perc {
    float volume = 1.0f, pitch = 1.0f, pan = 0.5f;
    u16 release = 1000;
    std::vector<Effect> effects;
    std::vector<VeloRegion> velos;
};

struct Inst {
    bool drum = false;
    std::array<const OscData*, 2> osc{};
    std::vector<Effect> effects;
    std::vector<KeyRegion> keys;
    float volume = 1.0f, pitch = 1.0f;
    std::array<std::optional<Perc>, 128> percs;
};

// JASInstParam.
struct InstParam {
    float volume = 1.0f, fxmix = 0.0f, pitch = 1.0f, pan = 0.5f, dolby = 0.0f;
    u32 wave_id = 0;
    std::array<const OscData*, 2> osc{};
    u32 osc_count = 0;
    bool drum = false;
    u16 direct_release = 0;
};

struct Bank {
    u32 number = 0;
    u32 wave_bank = 0;
    std::vector<s16> envt;
    std::vector<OscData> osc;
    std::array<std::unique_ptr<Inst>, 256> insts;
};

// JASWaveInfo, plus where the wave lives on the disc.
struct WaveInfo {
    u8 format = 0;
    u8 base_key = 60;
    float rate = 32000.0f;
    u32 aw_offset = 0, aw_length = 0;
    bool loop = false;
    u32 loop_start = 0, loop_end = 0, sample_count = 0;
    s16 last = 0, penult = 0;
    std::string archive;
};

struct WaveBank {
    std::unordered_map<u32, WaveInfo> waves;
};

void apply_effect(const Effect& effect, int key, int velocity, InstParam& p) {
    float value = 0.0f;
    if (!effect.sense) {
        // JASInstRand: base + range * (2 r - 0.9999999) with r uniform in [0, 1), drawn from a generator the
        // whole game shares. Rendered at its mean, the base; the menu sounds carry no random effects (load()
        // refuses a sound that does).
        value = effect.base;
    } else {
        const int v = effect.sense_type == 1 ? velocity : effect.sense_type == 2 ? key : 0;
        if (effect.max_point == 0x7F || effect.max_point == 0) {
            value = effect.start_level + (static_cast<float>(v) * (effect.end_level - effect.start_level)) / 127.0f;
        } else if (v < effect.max_point) {
            value = effect.start_level + (1.0f - effect.start_level) * (static_cast<float>(v) / effect.max_point);
        } else {
            value = (effect.end_level - 1.0f) * (static_cast<float>(v - effect.max_point) / (0x7F - effect.max_point)) +
                1.0f;
        }
    }
    switch (effect.type) {
    case 0: p.volume *= value; break;
    case 1: p.pitch *= value; break;
    case 2: p.pan += value - 0.5f; break;
    case 3: p.fxmix += value; break;
    case 4: p.dolby += value; break;
    default: break;
    }
}

bool inst_param(const Inst& inst, int key, int velocity, InstParam& p) {
    p = InstParam{};
    if (!inst.drum) {
        p.osc = inst.osc;
        p.osc_count = 2;
        p.volume = inst.volume;
        p.pitch = inst.pitch;
        for (const Effect& e : inst.effects) {
            apply_effect(e, key, velocity, p);
        }
        const KeyRegion* region = nullptr;
        for (const KeyRegion& k : inst.keys) {
            if (key <= k.high_key) {
                region = &k;
                break;
            }
        }
        if (region == nullptr) {
            return false;
        }
        for (const VeloRegion& v : region->velos) {
            if (velocity <= v.max_velocity) {
                p.volume *= v.volume;
                p.pitch *= v.pitch;
                p.wave_id = v.wave_id;
                return true;
            }
        }
        return false;
    }
    if (key < 0 || key >= 128 || !inst.percs[static_cast<std::size_t>(key)]) {
        return false;
    }
    const Perc& perc = *inst.percs[static_cast<std::size_t>(key)];
    static const OscData drum_osc{0, 1.0f, nullptr, nullptr, 1.0f, 0.0f};
    p.drum = true;
    p.volume = perc.volume;
    p.pitch = perc.pitch;
    p.pan = perc.pan;
    p.direct_release = perc.release;
    p.osc = {&drum_osc, nullptr};
    p.osc_count = 1;
    for (const Effect& e : perc.effects) {
        apply_effect(e, key, velocity, p);
    }
    for (const VeloRegion& v : perc.velos) {
        if (velocity <= v.max_velocity) {
            p.volume *= v.volume;
            p.pitch *= v.pitch;
            p.wave_id = v.wave_id;
            return true;
        }
    }
    return false;
}

// ---- The archive ------------------------------------------------------------------------------------------

struct Archive {
    std::vector<std::byte> baa;
    u32 bst = 0, bst_end = 0, bstn = 0, bsc = 0, bsc_end = 0;
    std::map<u32, WaveBank> wave_banks;   // by ws id
    std::map<u32, Bank> banks;            // by the bank's own number
    // Names of section 0 group 0, and every (section, group, index) by name.
    std::vector<std::string> system_names;
    // JASOscillator's four curve tables (linear, square, square root, sample cell), read from the game's DOL.
    std::array<std::array<float, 17>, 4> curves{};
    std::unordered_map<std::string, u32> ids;  // packed (s << 24) | (g << 16) | i

    [[nodiscard]] Bytes bytes() const noexcept { return {baa.data(), baa.size()}; }
};

std::optional<Effect> parse_effect(Bytes b, u32 at) {
    Effect e;
    switch (b.u32at(at)) {
    case tag("Rand"):
        e.sense = false;
        e.type = b.u8at(at + 4u);
        e.base = b.f32at(at + 8u);
        e.range = b.f32at(at + 12u);
        return e;
    case tag("Sens"):
        e.sense = true;
        e.type = b.u8at(at + 4u);
        e.sense_type = b.u8at(at + 5u);
        e.max_point = b.u8at(at + 6u);
        e.start_level = b.f32at(at + 8u);
        e.end_level = b.f32at(at + 12u);
        return e;
    default:
        return std::nullopt;
    }
}

bool parse_bank(Bytes b, u32 bank_at, u32 wave_bank, Archive& out, std::string& error) {
    if (b.u32at(bank_at) != tag("IBNK") || b.u32at(bank_at + 0xCu) != 1u) {
        error = "instrument bank is not IBNK version 1";
        return false;
    }
    const u32 size = b.u32at(bank_at + 4u);
    if (!b.has(bank_at, size)) {
        error = "instrument bank overruns the archive";
        return false;
    }
    Bank bank;
    bank.number = b.u32at(bank_at + 8u);
    bank.wave_bank = wave_bank;
    u32 envt = 0, osct = 0, list = 0;
    for (u32 c = bank_at + 0x20u; c + 8u <= bank_at + size;) {
        const u32 id = b.u32at(c);
        if (id == tag("ENVT")) envt = c;
        if (id == tag("OSCT")) osct = c;
        if (id == tag("LIST")) list = c;
        const u32 next = (c + 0xBu + b.u32at(c + 4u)) & ~3u;
        if (next <= c) break;
        c = next;
    }
    if (envt == 0u || osct == 0u || list == 0u) {
        error = "instrument bank lacks ENVT/OSCT/LIST";
        return false;
    }
    const u32 envt_size = b.u32at(envt + 4u);
    bank.envt.resize(envt_size / 2u + 1u);
    for (u32 i = 0; i < envt_size / 2u; ++i) {
        bank.envt[i] = static_cast<s16>(b.u16at(envt + 8u + i * 2u));
    }
    const u32 osc_count = std::min<u32>(b.u32at(osct + 8u), 4096u);
    bank.osc.resize(osc_count);
    for (u32 i = 0; i < osc_count; ++i) {
        const u32 o = osct + 0xCu + i * 0x1Cu;
        OscData& d = bank.osc[i];
        d.target = b.u8at(o + 4u);
        d.rate = b.f32at(o + 8u);
        const u32 table = b.u32at(o + 0xCu);
        const u32 rel = b.u32at(o + 0x10u);
        d.table = table / 2u < envt_size / 2u ? bank.envt.data() + table / 2u : nullptr;
        d.rel_table = rel / 2u < envt_size / 2u ? bank.envt.data() + rel / 2u : nullptr;
        d.scale = b.f32at(o + 0x14u);
        d.offset = b.f32at(o + 0x18u);
    }
    const u32 count = std::min<u32>(b.u32at(list + 8u), 256u);
    const auto read_velos = [&](u32& p, std::vector<VeloRegion>& velos) {
        const u32 n = std::min<u32>(b.u32at(p), 128u);
        p += 4u;
        for (u32 k = 0; k < n; ++k, p += 16u) {
            velos.push_back({b.u8at(p), static_cast<u16>(b.u32at(p + 4u) & 0xFFFFu), b.f32at(p + 8u),
                             b.f32at(p + 12u)});
        }
    };
    for (u32 i = 0; i < count; ++i) {
        const u32 offset = b.u32at(list + 0xCu + i * 4u);
        if (offset == 0u) continue;
        u32 p = bank_at + offset;
        const u32 kind = b.u32at(p);
        p += 4u;
        auto inst = std::make_unique<Inst>();
        if (kind == tag("Inst")) {
            const u32 n_osc = b.u32at(p);
            p += 4u;
            for (u32 j = 0; j < n_osc; ++j, p += 4u) {
                const u32 index = b.u32at(p);
                if (j < 2u && index < bank.osc.size()) inst->osc[j] = &bank.osc[index];
            }
            const u32 n_eff = std::min<u32>(b.u32at(p), 16u);
            p += 4u;
            for (u32 j = 0; j < n_eff; ++j, p += 4u) {
                if (auto e = parse_effect(b, bank_at + b.u32at(p))) inst->effects.push_back(*e);
            }
            const u32 n_keys = std::min<u32>(b.u32at(p), 128u);
            p += 4u;
            for (u32 j = 0; j < n_keys; ++j) {
                KeyRegion region;
                region.high_key = b.u8at(p);
                p += 4u;
                read_velos(p, region.velos);
                inst->keys.push_back(std::move(region));
            }
            inst->volume = b.f32at(p);
            inst->pitch = b.f32at(p + 4u);
        } else if (kind == tag("Perc")) {
            inst->drum = true;
            const u32 n = std::min<u32>(b.u32at(p), 128u);
            p += 4u;
            for (u32 j = 0; j < n; ++j, p += 4u) {
                const u32 entry_offset = b.u32at(p);
                if (entry_offset == 0u) continue;
                const u32 e = bank_at + entry_offset;
                Perc perc;
                perc.volume = b.f32at(e + 4u);
                perc.pitch = b.f32at(e + 8u);
                perc.pan = static_cast<float>(b.u8at(e + 12u)) / 127.0f;
                perc.release = b.u16at(e + 14u);
                u32 q = e + 16u;
                const u32 n_eff = std::min<u32>(b.u32at(q), 16u);
                q += 4u;
                for (u32 k = 0; k < n_eff; ++k, q += 4u) {
                    if (auto ef = parse_effect(b, bank_at + b.u32at(q))) perc.effects.push_back(*ef);
                }
                read_velos(q, perc.velos);
                inst->percs[j] = std::move(perc);
            }
        } else {
            continue;
        }
        bank.insts[i] = std::move(inst);
    }
    out.banks[bank.number] = std::move(bank);
    return true;
}

bool parse_wave_system(Bytes b, u32 ws, WaveBank& out, std::string& error) {
    if (b.u32at(ws) != tag("WSYS")) {
        error = "wave system table is not WSYS";
        return false;
    }
    const u32 winf = ws + b.u32at(ws + 0x10u);
    const u32 wbct = ws + b.u32at(ws + 0x14u);
    const u32 groups = std::min<u32>(b.u32at(wbct + 8u), 256u);
    for (u32 g = 0; g < groups; ++g) {
        const u32 scene = ws + b.u32at(wbct + 0xCu + g * 4u);
        const u32 ctrl = ws + b.u32at(scene + 0xCu);
        const u32 archive = ws + b.u32at(winf + 8u + g * 4u);
        std::string name;
        for (u32 k = 0; k < 0x70u && b.u8at(archive + k) != 0u; ++k) {
            name.push_back(static_cast<char>(b.u8at(archive + k)));
        }
        const u32 waves = std::min<u32>(b.u32at(ctrl + 4u), 4096u);
        for (u32 j = 0; j < waves; ++j) {
            const u32 w = ws + b.u32at(archive + 0x74u + j * 4u);
            const u32 id = b.u32at(ws + b.u32at(ctrl + 8u + j * 4u)) & 0xFFFFu;
            WaveInfo info;
            info.format = b.u8at(w + 1u);
            info.base_key = b.u8at(w + 2u);
            info.rate = b.f32at(w + 4u);
            info.aw_offset = b.u32at(w + 8u);
            info.aw_length = b.u32at(w + 0xCu);
            info.loop = b.u32at(w + 0x10u) != 0u;
            info.loop_start = b.u32at(w + 0x14u);
            info.loop_end = b.u32at(w + 0x18u);
            info.sample_count = b.u32at(w + 0x1Cu);
            info.last = static_cast<s16>(b.u16at(w + 0x20u));
            info.penult = static_cast<s16>(b.u16at(w + 0x22u));
            info.archive = name;
            out.waves[id] = std::move(info);
        }
    }
    return true;
}

std::string read_cstring(Bytes b, u32 at) {
    std::string s;
    for (u32 k = 0; k < 128u && b.u8at(at + k) != 0u; ++k) s.push_back(static_cast<char>(b.u8at(at + k)));
    return s;
}

bool parse_archive(std::vector<std::byte> baa, Archive& out, std::string& error) {
    out.baa = std::move(baa);
    const Bytes b = out.bytes();
    if (b.u32at(0) != tag("AA_<")) {
        error = "SMR.baa is not an audio archive";
        return false;
    }
    std::vector<std::pair<u32, u32>> ws_list, bnk_list;
    u32 p = 4u;
    for (u32 guard = 0; guard < 4096u; ++guard) {
        const u32 cmd = b.u32at(p);
        p += 4u;
        if (cmd == tag(">_AA") || !b.has(p, 0)) break;
        switch (cmd) {
        case tag("ws  "): ws_list.emplace_back(b.u32at(p), b.u32at(p + 4u)); p += 12u; break;
        case tag("bnk "): bnk_list.emplace_back(b.u32at(p), b.u32at(p + 4u)); p += 8u; break;
        case tag("bl_<"): p += 8u; break;
        case tag(">_bl"): break;
        case tag("bsc "): out.bsc = b.u32at(p); out.bsc_end = b.u32at(p + 4u); p += 8u; break;
        case tag("bst "): out.bst = b.u32at(p); out.bst_end = b.u32at(p + 4u); p += 8u; break;
        case tag("bstn"): out.bstn = b.u32at(p); p += 8u; break;
        case tag("bms "): p += 12u; break;
        case tag("bmsa"): p += 4u; break;
        case tag("vbnk"): p += 8u; break;
        case tag("dsqb"): p += 4u; break;
        case tag("bsft"): p += 4u; break;
        case tag("sect"): p += 4u; break;
        default:
            error = "unknown audio archive command";
            return false;
        }
    }
    if (out.bst == 0u || out.bstn == 0u || out.bsc == 0u || ws_list.empty() || bnk_list.empty()) {
        error = "SMR.baa lacks its sound table, names, sequences or banks";
        return false;
    }
    if (b.u8at(out.bsc) != 'S' || b.u8at(out.bsc + 1u) != 'C') {
        error = "sequence collection is not SC";
        return false;
    }
    for (const auto& [id, offset] : ws_list) {
        if (!parse_wave_system(b, offset, out.wave_banks[id], error)) return false;
    }
    for (const auto& [wave_bank, offset] : bnk_list) {
        if (!parse_bank(b, offset, wave_bank, out, error)) return false;
    }
    // Names (BSTN): sections, groups, items, all offsets relative to the table.
    const u32 sections = std::min<u32>(b.u32at(out.bstn + 0x20u), 16u);
    for (u32 s = 0; s < sections; ++s) {
        const u32 so = out.bstn + b.u32at(out.bstn + 0x24u + s * 4u);
        const u32 groups = std::min<u32>(b.u32at(so), 256u);
        for (u32 g = 0; g < groups; ++g) {
            const u32 go = out.bstn + b.u32at(so + 8u + g * 4u);
            const u32 items = std::min<u32>(b.u32at(go), 0x10000u);
            for (u32 i = 0; i < items; ++i) {
                std::string name = read_cstring(b, out.bstn + b.u32at(go + 8u + i * 4u));
                if (s == 0u && g == 0u) out.system_names.push_back(name);
                out.ids.emplace(std::move(name), (s << 24u) | (g << 16u) | i);
            }
        }
    }
    return true;
}

// The sound table's item for a packed id: (type, item offset), or nullopt.
std::optional<std::pair<u8, u32>> sound_item(const Archive& a, u32 id) {
    const Bytes b = a.bytes();
    const u32 s = id >> 24u, g = (id >> 16u) & 0xFFu, i = id & 0xFFFFu;
    if (s >= b.u32at(a.bst + 0x20u)) return std::nullopt;
    const u32 so = a.bst + b.u32at(a.bst + 0x24u + s * 4u);
    if (g >= b.u32at(so)) return std::nullopt;
    const u32 go = a.bst + b.u32at(so + 4u + g * 4u);
    if (i >= b.u32at(go)) return std::nullopt;
    const u32 entry = b.u32at(go + 8u + i * 4u);
    return std::make_pair(static_cast<u8>(entry >> 24u), a.bst + (entry & 0xFFFFFFu));
}

// ---- The DSP side: channel records in private memory -------------------------------------------------------

constexpr u32 kMemBase = 0x8000'0000u;
constexpr u32 kResamplerAt = 0x8000'0000u;
constexpr u32 kAdpcmAt = 0x8000'1000u;
constexpr u32 kFxAt = 0x8000'2000u;
constexpr u32 kLeftAt = 0x8000'3000u;
constexpr u32 kRightAt = 0x8000'3800u;
constexpr u32 kChannelsAt = 0x8000'4000u;
constexpr u32 kRecordBytes = 0x180u;
constexpr u32 kWavesAt = 0x8001'0000u;  // VARAM: waves are addressed relative to this

struct Memory {
    std::vector<std::byte> bytes;

    std::byte* at(u32 address, u32 size) {
        if (address < kMemBase) return nullptr;
        const std::size_t offset = address - kMemBase;
        if (offset > bytes.size() || size > bytes.size() - offset) return nullptr;
        return bytes.data() + offset;
    }
    void put16(u32 address, u32 value) {
        if (std::byte* p = at(address, 2)) {
            p[0] = static_cast<std::byte>((value >> 8u) & 0xFFu);
            p[1] = static_cast<std::byte>(value & 0xFFu);
        }
    }
    void put32(u32 address, u32 value) {
        put16(address, value >> 16u);
        put16(address + 2u, value & 0xFFFFu);
    }
    u16 get16(u32 address) {
        const std::byte* p = at(address, 2);
        return p ? static_cast<u16>((static_cast<u32>(p[0]) << 8u) | static_cast<u32>(p[1])) : 0u;
    }
};

// JASDsp::TChannel, written into its record.
struct Record {
    Memory* mem;
    u32 base;

    void w16(u32 o, u32 v) const { mem->put16(base + o, v); }
    void w32(u32 o, u32 v) const { mem->put32(base + o, v); }
    [[nodiscard]] u16 r16(u32 o) const { return mem->get16(base + o); }

    void init() const {
        w16(0x00C, 0);  // pause
        w16(0x002, 0);  // finished
        w16(0x10A, 0);  // forced stop
        w16(0x000, 0);  // active
        w16(0x058, 0);
        w32(0x068, 0);
        for (u32 i = 0; i < 8; ++i) w16(0x120 + i * 2, 0);
        w16(0x120, 0x7FFF);
        for (u32 i = 0; i < 8; ++i) w16(0x148 + i * 2, 0);
        w16(0x148, 0x7FFF);
    }
    void play_start() const {
        w32(0x10C, 0);
        w16(0x060, 0);
        w16(0x008, 1);
        w16(0x066, 0);
        for (u32 i = 0; i < 4; ++i) {
            w16(0x078 + i * 2, 0);
            w16(0x0A8 + i * 2, 0);
        }
        for (u32 i = 0; i < 20; ++i) w16(0x080 + i * 2, 0);
        w16(0x000, 1);
    }
    void play_stop() const { w16(0x000, 0); }
    void reply_finish() const {
        w16(0x002, 0);
        w16(0x000, 0);
    }
    void force_stop() const { w16(0x10A, 1); }
    [[nodiscard]] bool is_active() const { return r16(0x000) != 0; }
    [[nodiscard]] bool is_finish() const { return r16(0x002) != 0; }
    void set_wave_info(const WaveInfo& w, u32 wave_ptr, u32 skip) const {
        static constexpr u8 kBlockSamples[8] = {0x10, 0x10, 0x01, 0x01, 0x01, 0x10, 0x10, 0x01};
        static constexpr u8 kBlockBytes[8] = {0x09, 0x05, 0x08, 0x10, 0x01, 0x01, 0x01, 0x01};
        const u32 format = w.format & 7u;
        u32 ptr = wave_ptr;
        w16(0x064, kBlockSamples[format]);
        const u32 block_bytes = kBlockBytes[format];
        w16(0x100, block_bytes);
        w32(0x068, 0);
        if (block_bytes >= 4u) {
            w32(0x11C, w.sample_count);
            w16(0x102, w.loop ? 0xFFu : 0u);
            u32 loop_start = 0, loop_end = 0;
            if (w.loop) {
                if (skip == 1u) skip = w.loop_start;
                loop_start = w.loop_start;
                loop_end = w.loop_end;
                w16(0x104, static_cast<u16>(w.last));
                w16(0x106, static_cast<u16>(w.penult));
            } else {
                loop_end = w.sample_count;
            }
            if (skip != 0u && loop_end > skip) {
                if (format <= 1u) {
                    w32(0x068, skip);
                    ptr += skip * block_bytes >> 4u;
                    loop_start -= skip;
                    loop_end -= skip;
                } else if (format <= 3u) {
                    w32(0x068, skip);
                }
            }
            if (w.loop) w32(0x110, loop_start);
            w32(0x114, loop_end);
            for (u32 i = 0; i < 16; ++i) w16(0x0B0 + i * 2, 0);
        }
        w32(0x118, ptr);
    }
    void set_pitch(u32 pitch) const { w16(0x004, std::min<u32>(pitch, 0x7FFFu)); }
    void set_mixer_init_volume(u32 bus, u16 v) const {
        w16(0x010 + bus * 8 + 4, v);
        w16(0x010 + bus * 8 + 2, v);
        w16(0x010 + bus * 8 + 6, 0);
    }
    void set_mixer_volume(u32 bus, u16 v) const {
        if (r16(0x10A) == 0u) {
            w16(0x010 + bus * 8 + 2, v);
            w16(0x010 + bus * 8 + 6, r16(0x010 + bus * 8 + 6) & 0xFFu);
        }
    }
    void set_pause(bool pause) const { w16(0x00C, pause ? 1u : 0u); }
    void set_filter_mode(u16 mode) const {
        const u32 iir = mode & 0x20u;
        u32 fir = mode & 0x1Fu;
        fir = std::min<u32>(fir, iir != 0u ? 0x14u : 0x18u);
        w16(0x108, iir + fir);
    }
    void set_iir(const std::array<s16, 8>& c) const {
        for (u32 i = 0; i < 8; ++i) w16(0x148 + i * 2, static_cast<u16>(c[i]));
    }
    void set_fir(const std::array<s16, 8>& c) const {
        for (u32 i = 0; i < 8; ++i) w16(0x120 + i * 2, static_cast<u16>(c[i]));
    }
    void set_dist_filter(s16 v) const { w16(0x148 + 8, static_cast<u16>(v)); }
    void set_bus_connect(u32 bus, u32 to) const {
        static constexpr u16 kConnect[12] = {0x0000, 0x0D00, 0x0D60, 0x0DC0, 0x0E20, 0x0E80,
                                             0x0EE0, 0x0CA0, 0x0F40, 0x0FA0, 0x0B00, 0x09A0};
        w16(0x010 + bus * 8, to < 12u ? kConnect[to] : 0u);
    }
};

// ---- CPU side -----------------------------------------------------------------------------------------------

// JASOscillator::sCurveTableLinear / Square / SqRoot / SampleCell (big-endian floats in the game's DOL). The
// linear table's bytes are known exactly; the others sit beside it and are told apart by their shape. The
// sample-cell curve is a table of hand-picked levels, so it is read rather than reconstructed.
bool find_curve_tables(const std::vector<std::byte>& dol, std::array<std::array<float, 17>, 4>& out) {
    const Bytes b{dol.data(), dol.size()};
    const auto table_at = [&](std::size_t o) {
        std::array<float, 17> t{};
        for (u32 i = 0; i < 17u; ++i) t[i] = b.f32at(o + i * 4u);
        return t;
    };
    const auto matches = [](const std::array<float, 17>& t, auto&& f, float tolerance) {
        for (u32 i = 0; i < 17u; ++i) {
            if (!(std::fabs(t[i] - f(static_cast<float>(i) / 16.0f)) <= tolerance)) return false;
        }
        return true;
    };
    const auto linear = [](float x) { return 1.0f - x; };
    const auto square = [](float x) { return std::sqrt(std::max(0.0f, 1.0f - x)); };
    const auto sqroot = [](float x) { return (1.0f - x) * (1.0f - x); };
    for (std::size_t o = 0; o + 68u <= dol.size(); o += 4u) {
        if (b.u32at(o) != 0x3F800000u || b.u32at(o + 4u) != 0x3F700000u) continue;  // 1.0, 0.9375
        const std::array<float, 17> lin = table_at(o);
        if (!matches(lin, linear, 0.0f)) continue;
        std::optional<std::array<float, 17>> sq, sr, cell;
        for (int k = -3; k <= 3; ++k) {
            if (k == 0) continue;
            const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(o) + k * 68;
            if (at < 0 || static_cast<std::size_t>(at) + 68u > dol.size()) continue;
            const std::array<float, 17> t = table_at(static_cast<std::size_t>(at));
            if (matches(t, square, 2e-6f)) {
                sq = t;
            } else if (matches(t, sqroot, 2e-6f)) {
                sr = t;
            } else if (t[0] == 1.0f && t[16] == 0.0f && std::is_sorted(t.rbegin(), t.rend()) && !cell) {
                cell = t;
            }
        }
        if (sq && sr && cell) {
            out = {lin, *sq, *sr, *cell};
            return true;
        }
    }
    return false;
}
// JASOscillator.
struct Oscillator {
    const OscData* data = nullptr;
    float t = 0.0f;      // _04
    float value = 0.0f;  // _08
    float from = 0.0f;   // _0C
    float to = 0.0f;     // _10
    u16 index = 0;       // _14
    u16 direct_release = 0;
    u8 curve = 0;        // _18
    int state = 0;       // _1C: 0 stop, 1 attack, 2 sustain, 3 release, 4 direct release
    const std::array<std::array<float, 17>, 4>* curves = nullptr;

    [[nodiscard]] bool valid() const { return data != nullptr; }
    [[nodiscard]] bool stopped() const { return state == 0; }
    [[nodiscard]] bool releasing() const { return state == 3 || state == 4; }

    void init_start(const OscData* d) {
        data = d;
        t = value = from = 0.0f;
        index = 0;
        direct_release = 0;
        if (d == nullptr) {
            state = 0;
            return;
        }
        if (d->table == nullptr) {
            state = 2;
            value = 1.0f;
            return;
        }
        to = static_cast<float>(d->table[2]) / 32768.0f;
        curve = static_cast<u8>(d->table[0]);
        state = 1;
    }
    void inc(float step) {
        if (state != 1 && state != 3 && state != 4) return;
        t += step * data->rate;
        update();
    }
    [[nodiscard]] float get() const { return value * data->scale + data->offset; }
    void release() {
        if (data->rel_table == nullptr && direct_release == 0u) {
            state = 0;
            return;
        }
        if (direct_release != 0u) {
            t = 0.0f;
            from = value;
            to = 0.0f;
            index = 0;
            curve = static_cast<u8>((direct_release >> 14u) & 3u);
            state = 4;
            update();
            return;
        }
        if (data->table != data->rel_table) {
            t = 0.0f;
            from = value;
            to = static_cast<float>(data->rel_table[2]) / 32768.0f;
            index = 0;
            curve = static_cast<u8>(data->rel_table[0]);
        }
        state = 3;
        update();
    }
    void update() {
        if (state == 4) {
            const s32 x = direct_release & 0x3FFF;
            if (t < static_cast<float>(x)) {
                update_current(static_cast<float>(x));
            } else {
                value = to;
                state = 0;
            }
            return;
        }
        if (state == 2) return;
        const s16* table = state == 1 ? data->table : data->rel_table;
        if (table == nullptr) {
            state = 2;
            return;
        }
        for (u32 guard = 0; t >= static_cast<float>(table[index * 3u + 1u]); ++guard) {
            if (guard > 256u) {
                state = 0;
                return;
            }
            t -= static_cast<float>(table[index * 3u + 1u]);
            value = to;
            ++index;
            from = value;
            const s16 mode = table[index * 3u];
            switch (mode) {
            case 0xF: state = 0; return;
            case 0xE: state = 2; return;
            case 0xD: index = static_cast<u16>(table[index * 3u + 2u]); break;
            default:
                curve = static_cast<u8>(mode);
                to = static_cast<float>(table[index * 3u + 2u]) / 32768.0f;
                break;
            }
        }
        update_current(static_cast<float>(table[index * 3u + 1u]));
    }
    void update_current(float duration) {
        const float f = 16.0f * (t / duration);
        const u32 i = std::min<u32>(static_cast<u32>(f), 15u);
        const float frac = f - static_cast<float>(i);
        const std::array<float, 17>& table = (*curves)[curve & 3u];
        const float c = (1.0f - frac) * table[i] + frac * table[i + 1u];
        value = from * c + to * (1.0f - c);
    }
};

// JASCalc::pow2, operation for operation (single precision, no fused multiply-adds, as the game's code is
// compiled): channel pitches are truncated to 16 bits, so a different rounding of 2^x can move one by a step.
float jas_pow2(float x) {
    static constexpr float kScaleFrac[2] = {0.0f, 0.5f};
    static constexpr float kTwoToFrac[2] = {1.0f, 0.70710677f};
    static constexpr float kC[6] = {0.6931472f, 0.24022661f, 0.055502914f, 0.009625022f, 0.0013131053f, 1.8300806E-4f};
    s32 exponent = x >= 0.0f ? static_cast<s32>(x - 0.5f) : static_cast<s32>(x + 0.5f);
    const float panned = x - static_cast<float>(exponent);
    if (exponent > 128) return INFINITY;
    const u32 bits = static_cast<u32>(exponent + 127) << 23u;
    float scale;
    std::memcpy(&scale, &bits, sizeof scale);
    const int i = panned < 0.0f ? 1 : 0;
    const float r = panned + kScaleFrac[i];
    float p = r * kC[5];
    p = kC[4] + p;
    p = r * p;
    p = kC[3] + p;
    p = r * p;
    p = kC[2] + p;
    p = r * p;
    p = kC[1] + p;
    p = r * p;
    p = kC[0] + p;
    p = r * p;
    float m = 0.25f + p;
    m = m * kTwoToFrac[i];
    m = 0.75f * kTwoToFrac[i] + m;
    return scale * m;
}

// JASLfo.
struct Lfo {
    u32 mode = 1;  // _4
    float phase = 0.0f;
    float depth = 0.0f, pitch = 1.0f;
    u16 delay = 0, wait = 0;

    [[nodiscard]] float get() const {
        if (wait != 0u || depth == 0.0f) return 0.0f;
        return depth * std::sin(phase * 6.2831853f);
    }
    void inc(float step) {
        if (wait != 0u) {
            --wait;
            return;
        }
        phase += step * ((1.0f / 72.0f) * pitch);
        if (phase >= 1.0f) phase -= 1.0f;
    }
    void reset() {
        wait = delay;
        if (mode == 1u) phase = 0.0f;  // mode 0 follows the free-running LFO, which starts at 0 here too
    }
};

struct ChannelParams {
    float volume = 1.0f, pitch = 1.0f, bend = 0.0f, pan = 0.5f, fxmix = 0.0f, dolby = 0.0f;
};

struct SoundParams {
    float volume = 1.0f, fxmix = 0.0f, pitch = 1.0f, pan = 0.5f, dolby = 0.0f;
};

struct EffectParams {
    float volume = 1.0f, pitch = 1.0f, pan = 0.5f, fxmix = 0.0f, dolby = 0.0f, vib = 1.0f, trem = 1.0f;
};

struct Track;
struct ChannelMgr;

// JASChannel.
struct Channel {
    bool alive = false;
    int status = 0;  // 0 stop, 1 play, 2 release
    bool pause = false;
    int dsp = -1;
    ChannelMgr* callback = nullptr;
    u32 update_timer = 0;
    std::array<Oscillator, 2> osc{};
    Lfo vib, trem;
    std::array<u16, 6> mix{0x150, 0x210, 0x352, 0x412, 0, 0};
    u16 priority = 0x13F;
    ChannelParams params;
    SoundParams init;
    s16 key = 0;
    u16 velocity = 0x7F;
    float key_sweep = 0.0f, key_sweep_target = 0.0f;
    u32 key_sweep_count = 0;
    u32 skip = 0;
    WaveInfo wave;
    u32 wave_ptr = 0;
    u32 serial = 0;
};

struct ChannelMgr {
    std::array<Channel*, 8> channels{};
    ChannelParams params;
    const SoundParams* sound = nullptr;
    Track* track = nullptr;
};

struct MoveParam {
    float value = 0.0f, target = 0.0f;
    u32 count = 0;
};

// JASTrack with its JASSeqCtrl and JASSeqReader.
struct Track {
    // Reader: positions are offsets into the sequence collection.
    bool has_data = false;
    u32 cursor = 0;
    std::array<u32, 8> stack{};
    std::array<u16, 8> loops{};
    u32 depth = 0;
    // Ctrl.
    s32 timer = 0;
    std::optional<u32> cursor_swap;
    u32 intr_table = 0;
    u16 intr_pending = 0, intr_mask = 0;
    u8 int_timer = 0;
    bool wait_note = false;
    u32 timer54 = 0, timer58 = 0;
    // Ports and registers.
    std::array<u16, 16> ports{};
    u16 port_import = 0, port_export = 0;
    std::array<u16, 14> regs{};
    // Track.
    std::array<MoveParam, 6> move{};  // volume, pitch, fxmix, pan, dolby, dist filter
    std::array<OscData, 2> osc_param{};
    std::array<s16, 12> osc_point{};
    Track* parent = nullptr;
    std::array<Track*, 16> children{};
    ChannelMgr mgr;
    float playtime = 1.0f, sample_interval = 1.0f;
    float vib_depth = 0.0f, vib_pitch = 1.0f, trem_depth = 0.0f, trem_pitch = 1.0f;
    u16 vib_delay = 0, trem_delay = 0;
    float pan_power = 1.0f;
    u16 direct_release = 0;
    u32 skip_sample = 0;
    u16 tempo = 120, timebase = 48;
    float tempo_rate = 1.0f;
    s32 transpose = 0;
    u8 pitch = 60;
    u16 bank = 0, prog = 0xF0;
    u8 bend_sense = 12, note_on_prio = 0x40, release_prio = 0, gate_rate = 100;
    std::array<u16, 6> mix{};
    std::array<s16, 8> fir{}, iir{};
    u16 filter_mode = 0;
    bool paused = false, muted = false, direct = true, invalidate = true, ready_to_play = false, stopped = false;
    int status = 0;  // 0 free, 1 run, 2 stopped
    u32 name = 0;    // for the trace
};

// One rendering of one sound.
class Engine {
public:
    using FetchArchive = std::function<const std::vector<std::byte>*(const std::string&)>;
    Engine(const Archive& archive, FetchArchive fetch, bool trace)
        : archive_(archive), fetch_(std::move(fetch)), trace_on_(trace) {}

    Rendered run(u32 seq_offset, float sound_volume, float max_seconds);
    std::string missing;  // what was not found (a bank, a program, a wave file)
    u32 mute_tracks = 0;
    u32 dsp_channel_limit = kDspChannels;
    bool used_random = false;
    bool used_osc_channel = false;
    bool used_iir_table = false;

private:
    // Trace.
    void log(const char* fmt, ...);

    // Sequence.
    [[nodiscard]] u8 byte_at(u32 o) const {
        const Bytes b = archive_.bytes();
        return o < archive_.bsc_end - archive_.bsc ? b.u8at(archive_.bsc + o) : 0u;
    }
    u32 read8(Track& t) { return byte_at(t.cursor++); }
    u32 read16(Track& t) {
        const u32 v = (static_cast<u32>(byte_at(t.cursor)) << 8u) | byte_at(t.cursor + 1u);
        t.cursor += 2u;
        return v;
    }
    u32 read24(Track& t) {
        const u32 v = (static_cast<u32>(byte_at(t.cursor)) << 16u) | (static_cast<u32>(byte_at(t.cursor + 1u)) << 8u) |
            byte_at(t.cursor + 2u);
        t.cursor += 3u;
        return v;
    }
    [[nodiscard]] u32 get16(u32 o) const { return (static_cast<u32>(byte_at(o)) << 8u) | byte_at(o + 1u); }
    [[nodiscard]] u32 get24(u32 o) const {
        return (static_cast<u32>(byte_at(o)) << 16u) | (static_cast<u32>(byte_at(o + 1u)) << 8u) | byte_at(o + 2u);
    }
    [[nodiscard]] u32 get32(u32 o) const { return (get16(o) << 16u) | get16(o + 2u); }
    u32 read_midi(Track& t);

    void track_init(Track& t);
    void track_inherit(Track& t, const Track& parent);
    Track* open_child(Track& t, u32 index);
    void close_child(Track& t, u32 index);
    void track_close(Track& t);
    int track_seq_main(Track& t);
    int track_tick(Track& t);
    int seq_tick(Track& t);
    int parse(Track& t);
    int parse_command(Track& t, u32 code, u32 widths);
    void update_seq(Track& t, bool update, float num);
    void update_track(Track& t, float num);
    void update_tempo(Track& t);
    void update_timed(Track& t);
    [[nodiscard]] Track* root(Track& t) {
        Track* r = &t;
        while (r->parent != nullptr) r = r->parent;
        return r;
    }
    [[nodiscard]] s32 transpose_total(const Track& t) const {
        return t.parent != nullptr ? transpose_total(*t.parent) + t.transpose : t.transpose;
    }
    [[nodiscard]] bool is_mute(const Track& t) const {
        return t.parent != nullptr ? (t.muted || is_mute(*t.parent)) : t.muted;
    }
    void write_reg(Track& t, u32 reg, u32 value);
    u32 read_reg(Track& t, u32 reg);
    void reg_write(Track& t, u32 id, u32 value);
    [[nodiscard]] u32 reg_read(const Track& t, u32 id) const;
    bool condition(Track& t, u32 cond);
    void set_param(Track& t, u32 param, float value, u32 count);
    bool gate_on(Track& t, u32 key, u32 velocity, float time, u32 flags);
    bool note_on(Track& t, u32 slot, u32 key, u32 velocity);
    bool note_off(Track& t, u32 slot, u16 release);
    void exec_note_on_gate(Track& t, u32 key, u32 velocity, u32 time, u32 ctrl);
    [[nodiscard]] u32 seq_time_to_dsp_time(Track& t, float time);
    Channel* channel_start(Track& t, u32 key, u32 velocity, u32 timer);
    void channel_callback(u32 type, Channel* c, const Record* rec, ChannelMgr* mgr);
    void update_channel(Track& t, Channel& c, const Record& rec);
    const s16* seq_osc_table(u32 offset);

    // Channels.
    Channel* new_channel(ChannelMgr* mgr);
    void delete_channel(Channel* c);
    Channel* bank_note_on(Track& t, int prog, u32 key, u32 velocity, u16 priority, ChannelMgr* mgr);
    bool channel_play(Channel* c);
    void channel_release(Channel* c, u16 direct_release);
    int channel_dsp_callback(u32 type, Channel* c, const Record* rec);
    int channel_initial_update(Channel* c, const Record& rec);
    int channel_update(Channel* c, const Record& rec);
    void effect_osc(Channel& c, u32 i, EffectParams& e);
    void effector_param(Channel& c, const Record& rec, std::array<u16, 6>& volumes, const EffectParams& e);
    void update_mixer(Channel& c, float volume, float pan, float fxmix, float dolby, std::array<u16, 6>& out);
    [[nodiscard]] u32 channel_pitch(const Channel& c, const EffectParams& e) const;
    u32 wave_ptr(const WaveInfo& w);

    // DSP channels (JASDSPChannel).
    struct DspChannel {
        int status = 1;  // 0 active, 1 inactive, 2 drop
        s16 priority = -1;
        u32 flags = 0;
        u32 age = 0;
        Channel* callback = nullptr;
    };
    [[nodiscard]] Record record(int i) { return Record{&memory_, kChannelsAt + static_cast<u32>(i) * kRecordBytes}; }
    int dsp_alloc(u8 priority, Channel* c);
    void dsp_drop(int i);
    void dsp_update(int i);

    const Archive& archive_;
    FetchArchive fetch_;
    bool trace_on_;
    std::string trace_;
    u32 subframe_ = 0;
    Memory memory_;
    std::unordered_map<std::string, u32> placed_;  // "archive:offset" -> wave pointer
    u32 next_wave_ = 0;
    std::array<DspChannel, kDspChannels> dsp_{};
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<std::unique_ptr<Track>> tracks_;
    std::vector<Track*> track_list_;  // JASTrack::sTrackList
    SoundParams sound_params_;
    u32 serial_ = 0;
    std::vector<s16> seq_tables_;  // EnvSet tables (host order), stable after reserve
    std::map<u32, u32> seq_table_at_;
};

void Engine::log(const char* fmt, ...) {
    if (!trace_on_) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    char prefix[32];
    std::snprintf(prefix, sizeof prefix, "%5u ", subframe_);
    trace_ += prefix;
    trace_ += line;
    trace_ += '\n';
}

u32 Engine::read_midi(Track& t) {
    const u32 first = read8(t);
    if ((first & 0x80u) == 0u) return first;
    u32 value = first & 0x7Fu;
    for (u32 i = 0;; ++i) {
        if (i > 2u) return 0;
        value <<= 7u;
        const u32 next = read8(t);
        value |= next & 0x7Fu;
        if ((next & 0x80u) == 0u) break;
    }
    return value;
}

void Engine::track_init(Track& t) {
    Track fresh;
    fresh.osc_param[0] = OscData{0, 1.0f, nullptr, nullptr, 1.0f, 0.0f};
    fresh.osc_param[1] = OscData{1, 1.0f, nullptr, nullptr, 1.0f, 0.0f};
    // sDefaultAdsr: {0,0,0x7fff} {0,0,0x7fff} {0,0,0} {0xe,0,0}
    fresh.osc_point = {0, 0, 0x7FFF, 0, 0, 0x7FFF, 0, 0, 0, 0xE, 0, 0};
    fresh.move[0].value = 1.0f;
    fresh.move[3].value = 0.5f;
    for (MoveParam& m : fresh.move) m.target = m.value;
    fresh.mix = {0x150, 0x210, 0x352, 0x412, 0, 0};
    fresh.fir[0] = 0x7FFF;
    fresh.iir[0] = 0x7FFF;
    fresh.name = t.name;
    t = fresh;
    t.mgr.track = &t;
    update_tempo(t);
}

void Engine::track_inherit(Track& t, const Track& p) {
    t.direct = p.direct;
    t.bank = p.bank;
    t.prog = p.prog;
    t.note_on_prio = p.note_on_prio;
    t.release_prio = p.release_prio;
    t.skip_sample = p.skip_sample;
    t.fir = p.fir;
    t.iir = p.iir;
    t.filter_mode = p.filter_mode;
    t.mix = p.mix;
}

Track* Engine::open_child(Track& t, u32 index) {
    index &= 15u;
    Track* child = t.children[index];
    if (child != nullptr) {
        if (child->status == 1) {
            update_seq(*root(t), false, 1.0f);
            track_close(*child);
        }
        if (child->status != 0) {
            const u32 name = child->name;
            track_init(*child);
            child->name = name;
        }
        t.children[index] = child;
        child->parent = &t;
    } else {
        tracks_.push_back(std::make_unique<Track>());
        child = tracks_.back().get();
        child->name = t.name * 16u + index + 1u;
        track_init(*child);
        child->parent = &t;
        t.children[index] = child;
    }
    track_inherit(*child, t);
    return child;
}

void Engine::close_child(Track& t, u32 index) {
    Track* child = t.children[index & 15u];
    if (child != nullptr) {
        update_seq(*root(t), false, 1.0f);
        track_close(*child);
        t.children[index & 15u] = nullptr;
    }
}

void Engine::track_close(Track& t) {
    for (u32 i = 0; i < 16u; ++i) {
        if (Track* child = t.children[i]) {
            track_close(*child);
            t.children[i] = nullptr;
        }
    }
    for (u32 i = 0; i < 8u; ++i) {
        if (Channel* c = t.mgr.channels[i]) {
            channel_release(c, 0);
            c->callback = nullptr;
            t.mgr.channels[i] = nullptr;
        }
    }
    t.parent = nullptr;
    t.status = 2;
}

void Engine::update_tempo(Track& t) {
    if (t.parent == nullptr) {
        t.sample_interval = t.tempo_rate * (1.33333337307f * static_cast<float>(t.timebase * t.tempo) / kDacRate);
    } else {
        t.tempo = t.parent->tempo;
        t.timebase = t.parent->timebase;
        t.sample_interval = t.parent->sample_interval;
    }
}

void Engine::update_timed(Track& t) {
    for (MoveParam& m : t.move) {
        if (m.count != 0u) {
            m.value += (m.target - m.value) / static_cast<float>(m.count);
            --m.count;
        }
    }
}

void Engine::update_track(Track& t, float num) {
    update_tempo(t);
    float volume = t.move[0].value * t.move[0].value;
    float pitch = 1.0f;
    const float bend = t.move[1].value * static_cast<float>(t.bend_sense) * 0.333333343267f;
    float pan = (t.move[3].value - 0.5f) * t.pan_power;
    float fxmix = t.move[2].value;
    float dolby = t.move[4].value;
    if (t.mgr.sound != nullptr) {
        volume *= t.mgr.sound->volume;
        pitch *= t.mgr.sound->pitch;
        pan += t.mgr.sound->pan - 0.5f;
        fxmix += t.mgr.sound->fxmix;
        dolby += t.mgr.sound->dolby;
    }
    pan *= num;
    ChannelParams& p = t.mgr.params;
    if (t.parent == nullptr) {
        p = {volume, pitch, bend, pan, fxmix, dolby};
    } else {
        const ChannelParams& q = t.parent->mgr.params;
        p.volume = q.volume * volume;
        p.pitch = q.pitch * pitch;
        p.pan = q.pan - 0.5f + pan;
        p.fxmix = q.fxmix + fxmix;
        p.dolby = q.dolby + dolby;
        p.bend = q.bend + bend;
    }
    p.pan += 0.5f;
}

void Engine::update_seq(Track& t, bool update, float num) {
    if (!update) update = t.invalidate;
    t.invalidate = false;
    if (update) update_track(t, num);
    num *= t.pan_power;
    for (Track* child : t.children) {
        if (child != nullptr && child->status == 1) update_seq(*child, update, num);
    }
}

u32 Engine::seq_time_to_dsp_time(Track& t, float time) {
    if (t.direct) {
        time /= t.sample_interval;
    } else {
        time *= 120.0f / static_cast<float>(t.timebase);
        time *= static_cast<float>(kSubframesPerFrame) / 10.0f;
    }
    return static_cast<u32>(time);
}

void Engine::set_param(Track& t, u32 param, float value, u32 count) {
    if (param >= 6u) return;
    MoveParam& m = t.move[param];
    m.target = value;
    if (count == 0u) m.value = m.target;
    m.count = count;
}

void Engine::reg_write(Track& t, u32 id, u32 v) {
    auto& r = t.regs;
    if (id < 14u) {
        r[id] = id <= 2u ? static_cast<u16>(v & 0xFFu) : static_cast<u16>(v);
        return;
    }
    switch (id) {
    case 32: r[0] = (v >> 8u) & 0xFFu; r[1] = v & 0xFFu; break;
    case 33: r[4] = static_cast<u16>(v >> 16u); r[5] = static_cast<u16>(v); break;
    case 34: r[4] = static_cast<u16>((r[4] & 0xFFu) | ((v & 0xFFu) << 8u)); break;
    case 35: r[4] = static_cast<u16>((r[4] & 0xFF00u) | (v & 0xFFu)); break;
    case 36: r[5] = static_cast<u16>((r[5] & 0xFFu) | ((v & 0xFFu) << 8u)); break;
    case 37: r[5] = static_cast<u16>((r[5] & 0xFF00u) | (v & 0xFFu)); break;
    case 38: r[6] = static_cast<u16>(v >> 16u); r[7] = static_cast<u16>(v); break;
    case 39: r[8] = static_cast<u16>(v >> 16u); r[9] = static_cast<u16>(v); break;
    case 40: r[10] = static_cast<u16>(v >> 16u); r[11] = static_cast<u16>(v); break;
    case 41: r[12] = static_cast<u16>(v >> 16u); r[13] = static_cast<u16>(v); break;
    default: break;
    }
}

u32 Engine::reg_read(const Track& t, u32 id) const {
    const auto& r = t.regs;
    if (id < 14u) return r[id];
    switch (id) {
    case 32: return ((r[0] & 0xFFu) << 8u) | (r[1] & 0xFFu);
    case 33: return (static_cast<u32>(r[4]) << 16u) | r[5];
    case 34: return r[4] >> 8u;
    case 35: return r[4] & 0xFFu;
    case 36: return r[5] >> 8u;
    case 37: return r[5] & 0xFFu;
    case 38: return (static_cast<u32>(r[6]) << 16u) | r[7];
    case 39: return (static_cast<u32>(r[8]) << 16u) | r[9];
    case 40:
    case 41: return (static_cast<u32>(r[10]) << 16u) | r[11];
    default: return 0;
    }
}

void Engine::write_reg(Track& t, u32 reg, u32 value) {
    reg_write(t, 3, value);  // REG_F
    if (reg < 0x40u) {
        reg_write(t, reg, value);
        return;
    }
    const u32 k = reg - 0x40u;
    if (k < 16u) {
        t.ports[k] = static_cast<u16>(value);
        return;
    }
    switch (k) {
    case 0x22: t.timebase = static_cast<u16>(value); t.invalidate = true; update_tempo(t); break;
    case 0x23: t.transpose = static_cast<s32>(value); break;
    case 0x24: t.bend_sense = static_cast<u8>(value); break;
    case 0x25: t.gate_rate = static_cast<u8>(value); break;
    case 0x26: t.skip_sample = value; break;
    case 0x27: t.bank = static_cast<u8>(value); break;
    case 0x28: t.prog = static_cast<u8>(value); break;
    case 0x29: t.pan_power = static_cast<float>(value) / 32767.0f; break;
    case 0x2A: t.note_on_prio = static_cast<u8>(value); t.release_prio = static_cast<u8>(value >> 8u); break;
    case 0x2B: t.note_on_prio = static_cast<u8>(value); break;
    case 0x2C: t.release_prio = static_cast<u8>(value); break;
    case 0x2D: t.direct_release = static_cast<u16>(value); break;
    case 0x2E: t.vib_depth = static_cast<float>(value) / 1524.0f; break;
    case 0x2F: t.vib_depth = static_cast<float>(value) / 12192.0f; break;
    case 0x30: t.trem_depth = static_cast<float>(value) / 256.0f; break;
    case 0x31: t.vib_pitch = static_cast<float>(value) / 64.0f; break;
    case 0x32: t.trem_pitch = static_cast<float>(value) / 64.0f; break;
    case 0x33: t.vib_delay = static_cast<u16>(value); break;
    case 0x34: t.trem_delay = static_cast<u16>(value); break;
    default: break;
    }
}

u32 Engine::read_reg(Track& t, u32 reg) {
    if (reg < 0x40u) return reg_read(t, reg);
    const u32 k = reg - 0x40u;
    if (k < 16u) return t.ports[k];
    switch (k) {
    case 0x20: {
        u32 bits = 0;
        for (u32 i = 0; i < 16u; ++i) {
            if (t.children[i] != nullptr && t.children[i]->status == 1) bits |= 1u << i;
        }
        return bits;
    }
    case 0x21: return t.depth == 0u ? 0u : t.loops[t.depth - 1u];
    case 0x22: return t.timebase;
    case 0x23: return static_cast<u32>(t.transpose);
    case 0x24: return t.bend_sense;
    case 0x25: return t.gate_rate;
    case 0x26: return t.skip_sample;
    case 0x27: return t.bank;
    case 0x28: return t.prog;
    case 0x29: return static_cast<u32>(t.pan_power * 32767.0f);
    case 0x2A: return static_cast<u32>(t.release_prio) | t.note_on_prio;
    case 0x2B: return t.note_on_prio;
    case 0x2C: return t.release_prio;
    case 0x2D: return t.direct_release;
    case 0x2E: return static_cast<u32>(t.vib_depth * 1524.0f);
    case 0x2F: return static_cast<u32>(t.vib_depth * 12192.0f);
    case 0x30: return static_cast<u32>(t.trem_depth * 256.0f);
    case 0x31: return static_cast<u32>(t.vib_pitch * 64.0f);
    case 0x32: return static_cast<u32>(t.trem_pitch * 64.0f);
    case 0x33: return t.vib_delay;
    case 0x34: return t.trem_delay;
    default: return 0;
    }
}

bool Engine::condition(Track& t, u32 cond) {
    const u16 f = static_cast<u16>(reg_read(t, 3));
    switch (cond) {
    case 0: return true;
    case 1: return f == 0u;
    case 2: return f != 0u;
    case 3: return f == 1u;
    case 4: return f >= 0x8000u;
    case 5: return f < 0x8000u;
    default: return false;
    }
}

const s16* Engine::seq_osc_table(u32 offset) {
    if (auto it = seq_table_at_.find(offset); it != seq_table_at_.end()) return seq_tables_.data() + it->second;
    const u32 start = static_cast<u32>(seq_tables_.size());
    if (start + 3u * 65u > seq_tables_.capacity()) return nullptr;  // capacity is reserved; never reallocate
    for (u32 k = 0; k < 64u; ++k) {
        const s16 mode = static_cast<s16>(get16(offset + k * 6u));
        seq_tables_.push_back(mode);
        seq_tables_.push_back(static_cast<s16>(get16(offset + k * 6u + 2u)));
        seq_tables_.push_back(static_cast<s16>(get16(offset + k * 6u + 4u)));
        if (mode > 10) break;
    }
    seq_table_at_[offset] = start;
    return seq_tables_.data() + start;
}

void Engine::exec_note_on_gate(Track& t, u32 key, u32 velocity, u32 time, u32 ctrl) {
    u32 flags = 0;
    if ((ctrl >> 6u) & 1u) flags |= 2u;
    if ((ctrl >> 7u) & 1u) flags |= 1u;
    if (time == 0u) flags |= 4u;
    log("track%X note key=%u vel=%u gate=%u flags=%u", t.name, key, velocity, time, flags);
    gate_on(t, key, velocity, static_cast<float>(time), flags);
    if (time != 0u) {
        t.timer = static_cast<s32>(time);
    } else {
        t.wait_note = true;
    }
}

bool Engine::gate_on(Track& t, u32 key, u32 velocity, float time, u32 flags) {
    key += static_cast<u32>(transpose_total(t));
    if (t.gate_rate != 100u) time *= static_cast<float>(t.gate_rate) / 100.0f;
    const u32 dsp_time = seq_time_to_dsp_time(t, time);
    const bool sweep = (flags & 1u) != 0u;
    const u32 timer = (flags & 6u) != 0u ? 0u : dsp_time;
    const u32 pitch = sweep ? t.pitch : key;
    const s32 delta = static_cast<s32>(key) - static_cast<s32>(pitch);
    ChannelMgr& mgr = t.mgr;
    if (!t.ready_to_play) {
        note_off(t, 0, 0);
        if (!is_mute(t) && !(t.name < 32u && ((mute_tracks >> t.name) & 1u)))
            mgr.channels[0] = channel_start(t, pitch, velocity, timer);
    } else if (Channel* c = mgr.channels[0]) {
        c->key = static_cast<s16>(static_cast<s32>(pitch) - c->wave.base_key);
        c->velocity = static_cast<u16>(velocity);
        c->update_timer = timer;
    }
    if (sweep) {
        if (Channel* c = mgr.channels[0]) {
            if (dsp_time == 0u) {
                c->key_sweep = static_cast<float>(delta);
            } else {
                c->key_sweep = 0.0f;
                c->key_sweep_target = static_cast<float>(delta);
            }
            c->key_sweep_count = dsp_time;
        }
    }
    t.pitch = static_cast<u8>(key);
    t.ready_to_play = (flags & 2u) != 0u;
    return true;
}

bool Engine::note_on(Track& t, u32 slot, u32 key, u32 velocity) {
    if (is_mute(t)) return false;
    key += static_cast<u32>(transpose_total(t));
    note_off(t, slot, 0);
    Channel* c = channel_start(t, key, velocity, 0);
    t.mgr.channels[slot & 7u] = c;
    return c != nullptr;
}

bool Engine::note_off(Track& t, u32 slot, u16 release) {
    Channel* c = t.mgr.channels[slot & 7u];
    if (c == nullptr) return false;
    channel_release(c, release);
    c->callback = nullptr;
    t.mgr.channels[slot & 7u] = nullptr;
    return true;
}

Channel* Engine::channel_start(Track& t, u32 key, u32 velocity, u32 timer) {
    Channel* c = bank_note_on(t, t.prog, key & 0xFFu, velocity & 0xFFu,
                              static_cast<u16>(t.note_on_prio | (t.release_prio << 8u)), &t.mgr);
    if (c == nullptr) return nullptr;
    c->update_timer = timer;
    c->skip = t.skip_sample;
    c->vib.delay = t.vib_delay;
    c->trem.delay = t.trem_delay;
    c->params = t.mgr.params;
    for (u32 i = 0; i < 6u; ++i) c->mix[i] = t.mix[i];
    for (u32 i = 0; i < 2u; ++i) {
        if (t.osc_param[i].table != nullptr) c->osc[i].init_start(&t.osc_param[i]);
    }
    if (t.direct_release != 0u) c->osc[0].direct_release = t.direct_release;
    return c;
}

Channel* Engine::new_channel(ChannelMgr* mgr) {
    Channel* slot = nullptr;
    for (auto& c : channels_) {
        if (!c->alive) {
            slot = c.get();
            break;
        }
    }
    if (slot == nullptr) {
        channels_.push_back(std::make_unique<Channel>());
        slot = channels_.back().get();
    }
    *slot = Channel{};
    slot->alive = true;
    slot->callback = mgr;
    slot->serial = ++serial_;
    for (Oscillator& o : slot->osc) o.curves = &archive_.curves;
    return slot;
}

void Engine::delete_channel(Channel* c) {
    if (c->dsp >= 0) {
        const int i = c->dsp;
        c->dsp = -1;
        dsp_drop(i);
    }
    if (c->callback != nullptr) channel_callback(2, c, nullptr, c->callback);
    log("channel %u ends", c->serial);
    c->alive = false;
}

u32 Engine::wave_ptr(const WaveInfo& w) {
    const std::string key = w.archive + ":" + std::to_string(w.aw_offset);
    if (auto it = placed_.find(key); it != placed_.end()) return it->second;
    const std::vector<std::byte>* file = fetch_(w.archive);
    if (file == nullptr || static_cast<std::size_t>(w.aw_offset) + w.aw_length > file->size()) {
        missing = "wave archive " + w.archive;
        return 0;
    }
    const u32 ptr = next_wave_ + 0x20u;  // never 0 (a null wave pointer refuses the note)
    const std::size_t at = static_cast<std::size_t>(kWavesAt - kMemBase) + ptr;
    if (memory_.bytes.size() < at + w.aw_length + 0x40u) memory_.bytes.resize(at + w.aw_length + 0x40u);
    std::memcpy(memory_.bytes.data() + at, file->data() + w.aw_offset, w.aw_length);
    next_wave_ = (ptr + w.aw_length + 0x3Fu) & ~0x1Fu;
    placed_[key] = ptr;
    return ptr;
}

Channel* Engine::bank_note_on(Track& t, int prog, u32 key, u32 velocity, u16 priority, ChannelMgr* mgr) {
    if (prog >= 0xF0) {
        used_osc_channel = true;
        missing = "oscillator program";
        return nullptr;
    }
    const auto bank = archive_.banks.find(t.bank);
    if (bank == archive_.banks.end() || prog < 0 || bank->second.insts[static_cast<std::size_t>(prog)] == nullptr) {
        missing = "bank " + std::to_string(t.bank) + " program " + std::to_string(prog);
        return nullptr;
    }
    const Inst& inst = *bank->second.insts[static_cast<std::size_t>(prog)];
    for (const Effect& e : inst.effects) used_random |= !e.sense;
    if (inst.drum && key < 128u && inst.percs[key]) {
        for (const Effect& e : inst.percs[key]->effects) used_random |= !e.sense;
    }
    InstParam p;
    if (!inst_param(inst, static_cast<int>(key), static_cast<int>(velocity), p)) return nullptr;
    const auto waves = archive_.wave_banks.find(bank->second.wave_bank);
    if (waves == archive_.wave_banks.end()) return nullptr;
    const auto wave = waves->second.waves.find(p.wave_id);
    if (wave == waves->second.waves.end()) {
        missing = "wave " + std::to_string(p.wave_id);
        return nullptr;
    }
    const u32 ptr = wave_ptr(wave->second);
    if (ptr == 0u) return nullptr;
    Channel* c = new_channel(mgr);
    c->priority = priority;
    c->wave = wave->second;
    c->wave_ptr = ptr;
    c->init.pitch = p.pitch * (wave->second.rate / kDacRate);
    if (!p.drum) c->key = static_cast<s16>(static_cast<s32>(key) - wave->second.base_key);
    c->init.volume = p.volume;
    c->velocity = static_cast<u16>(velocity);
    c->init.pan = p.pan;
    c->init.fxmix = p.fxmix;
    c->init.dolby = p.dolby;
    for (u32 i = 0; i < p.osc_count; ++i) c->osc[i].init_start(p.osc[i]);
    c->osc[0].direct_release = p.direct_release;
    log("channel %u: bank %u prog %u key %u vel %u wave %u (%s+0x%X) rate %.0f basekey %u loop %d vol %.4f pitch %.4f",
        c->serial, t.bank, prog, key, velocity, p.wave_id, wave->second.archive.c_str(), wave->second.aw_offset,
        wave->second.rate, wave->second.base_key, wave->second.loop ? 1 : 0, p.volume, p.pitch);
    if (!channel_play(c)) return nullptr;
    return c;
}

bool Engine::channel_play(Channel* c) {
    const int i = dsp_alloc(static_cast<u8>(c->priority & 0xFFu), c);
    if (i < 0) {
        delete_channel(c);
        return false;
    }
    c->dsp = i;
    dsp_[static_cast<std::size_t>(i)].flags |= 1u;
    c->status = 1;
    return true;
}

void Engine::channel_release(Channel* c, u16 direct_release) {
    if (c->status != 1) return;
    if (direct_release != 0u) c->osc[0].direct_release = direct_release;
    for (Oscillator& o : c->osc) {
        if (o.valid()) o.release();
    }
    if (c->dsp >= 0) dsp_[static_cast<std::size_t>(c->dsp)].priority = static_cast<s16>(c->priority >> 8u);
    c->status = 2;
    log("channel %u release", c->serial);
}

void Engine::channel_callback(u32 type, Channel* c, const Record* rec, ChannelMgr* mgr) {
    Track& t = *mgr->track;
    switch (type) {
    case 0:
    case 1:
        c->params = mgr->params;
        if (rec != nullptr) update_channel(t, *c, *rec);
        break;
    case 3:
        channel_release(c, 0);
        c->callback = nullptr;
        mgr->channels[0] = nullptr;
        break;
    case 2:
        for (Channel*& slot : mgr->channels) {
            if (slot == c) {
                slot = nullptr;
                return;
            }
        }
        break;
    default:
        break;
    }
}

void Engine::update_channel(Track& t, Channel& c, const Record& rec) {
    c.vib.depth = t.vib_depth;
    c.vib.pitch = t.vib_pitch;
    c.trem.depth = t.trem_depth;
    c.trem.pitch = t.trem_pitch;
    if ((t.filter_mode & 0x20u) != 0u) rec.set_iir(t.iir);
    if ((t.filter_mode & 0x1Fu) != 0u) rec.set_fir(t.fir);
    rec.set_filter_mode(t.filter_mode);
    rec.set_dist_filter(static_cast<s16>(32767.0f * t.move[5].value));
}

void Engine::effect_osc(Channel& c, u32 i, EffectParams& e) {
    const float value = c.osc[i].get();
    switch (c.osc[i].data->target) {
    case 0: e.volume *= value; break;
    case 1: e.pitch *= value; break;
    case 2: e.pan += value - 0.5f; break;
    case 3: e.fxmix += value; break;
    case 4: e.dolby += value; break;
    case 5: e.vib *= value; break;
    case 6: e.trem *= value; break;
    default: break;
    }
}

void Engine::update_mixer(Channel& c, float volume, float pan, float fxmix, float dolby, std::array<u16, 6>& out) {
    constexpr float kHalfPi = 1.5707963267948966f;
    const auto scale_of = [&](u32 sel) -> float {
        switch (sel) {
        case 1: return pan;
        case 2: return fxmix;
        case 3: return dolby;
        case 5: return 1.0f - pan;
        case 6: return 1.0f - fxmix;
        case 7: return 1.0f - dolby;
        default: return 0.0f;
        }
    };
    for (u32 i = 0; i < 6u; ++i) {
        float v = volume;
        const u32 upper = c.mix[i] >> 8u;
        const u32 lower0 = (c.mix[i] >> 4u) & 0xFu;
        const u32 lower1 = c.mix[i] & 0xFu;
        if (upper == 0u) {
            out[i] = 0;
            continue;
        }
        if (lower0 != 0u) {
            const float s = scale_of(lower0);
            if (lower0 == 2u || lower0 == 6u || kOutputMode == 0u) {
                v *= s;
            } else {
                v *= std::sin(s * kHalfPi);
            }
        }
        if (lower1 != 0u) {
            const float s = scale_of(lower1);
            if (lower1 == 3u || lower1 == 7u) {
                v *= std::sin((s * 0.34776f + 0.32612f) * kHalfPi);
            } else if (lower1 == 2u || lower1 == 6u || kOutputMode == 0u) {
                v *= s;
            } else {
                v *= std::sin(s * kHalfPi);
            }
        }
        v = std::clamp(v, 0.0f, 1.0f);
        out[i] = static_cast<u16>(v * static_cast<float>(kChannelLevel));
    }
}

void Engine::effector_param(Channel& c, const Record& rec, std::array<u16, 6>& volumes, const EffectParams& e) {
    float pan = 0.5f;
    float dolby = 0.0f;
    if constexpr (kOutputMode >= 1u) {
        pan = 0.5f + (c.init.pan - 0.5f) + (e.pan - 0.5f) + (c.params.pan - 0.5f);
    }
    if constexpr (kOutputMode == 2u) dolby = c.init.dolby + e.dolby + c.params.dolby;
    float fxmix = c.init.fxmix + e.fxmix + c.params.fxmix;
    float volume = static_cast<float>(c.velocity) / 127.0f;
    volume *= volume;
    volume *= c.init.volume * e.volume * c.params.volume * (e.trem * c.trem.get() + 1.0f);
    volume = std::max(volume, 0.0f);
    pan = std::clamp(pan, 0.0f, 1.0f);
    fxmix = std::clamp(fxmix, 0.0f, 1.0f);
    dolby = std::clamp(dolby, 0.0f, 1.0f);
    if (c.mix[0] == 0xFFFFu) {
        // isDolbyMode: the DSP's auto mixer (JASChannel::updateAutoMixer, TChannel::setAutoMixer).
        if constexpr (kOutputMode == 0u) volume *= 0.707f;
        volume = std::clamp(volume, 0.0f, 1.0f);
        const u16 level = static_cast<u16>(volume * static_cast<float>(kChannelLevel));
        const u8 pan_byte = static_cast<u8>(pan * 127.5f);
        const u8 dolby_byte = static_cast<u8>(dolby * 127.5f);
        const u8 fx_byte = static_cast<u8>(fxmix * 127.5f);
        rec.w16(0x050, (static_cast<u32>(pan_byte) << 8u) | dolby_byte);
        rec.w16(0x052, (static_cast<u32>(fx_byte) << 8u) | (static_cast<u32>(fx_byte) << 1u));
        rec.w16(0x056, level);
        rec.w16(0x058, 1);
        volumes.fill(0);
        return;
    }
    update_mixer(c, volume, pan, fxmix, dolby, volumes);
}

u32 Engine::channel_pitch(const Channel& c, const EffectParams& e) const {
    float pitch = jas_pow2(c.params.bend + (static_cast<float>(c.key) + c.key_sweep) / 12.0f + e.vib * c.vib.get());
    pitch = c.init.pitch * e.pitch * pitch * c.params.pitch * 4096.0f;
    if (pitch < 0.0f) pitch = 0.0f;
    // setPitch(u16): the float converts to the 16-bit argument (truncating), then saturates at 0x7FFF.
    return static_cast<u32>(static_cast<u16>(std::min(pitch, 65535.0f)));
}

int Engine::channel_initial_update(Channel* c, const Record& rec) {
    if (c->mix[0] == 0xFFFFu) {
        // TChannel::initAutoMixer: a fresh record (init cleared 0x058) starts its volume ramp at 0.
        if (rec.r16(0x058) != 0u) {
            rec.w16(0x054, rec.r16(0x056));
        } else {
            rec.w16(0x054, 0);
            rec.w16(0x058, 1);
        }
    }
    if (c->callback != nullptr) channel_callback(1, c, &rec, c->callback);
    rec.set_wave_info(c->wave, c->wave_ptr, c->skip);
    for (u32 i = 0; i < 6u; ++i) {
        u32 upper = c->mix[i] >> 8u;
        // Stereo and mono send the surround bus (8) to the centre; mono also the inverted right (9) to the right.
        if constexpr (kOutputMode <= 1u) {
            if (upper == 8u) upper = 11u;
        }
        if constexpr (kOutputMode == 0u) {
            if (upper == 9u) upper = 2u;
        }
        rec.set_bus_connect(i, upper);
    }
    EffectParams e;
    for (u32 i = 0; i < 2u; ++i) {
        if (c->osc[i].valid()) {
            c->osc[i].update();
            effect_osc(*c, i, e);
        }
    }
    c->vib.reset();
    c->trem.reset();
    std::array<u16, 6> volumes{};
    effector_param(*c, rec, volumes, e);
    for (u32 i = 0; i < 6u; ++i) rec.set_mixer_init_volume(i, volumes[i]);
    rec.set_pitch(channel_pitch(*c, e));
    rec.set_pause(c->pause);
    rec.w16(0x066, 0);
    log("channel %u start: pitch 0x%04X L %u R %u fx %u %u auto %04X/%04X/%04X", c->serial, channel_pitch(*c, e),
        volumes[0], volumes[1], volumes[2], volumes[3], rec.r16(0x050), rec.r16(0x052), rec.r16(0x056));
    return 0;
}

int Engine::channel_update(Channel* c, const Record& rec) {
    if (c->callback != nullptr) channel_callback(0, c, &rec, c->callback);
    rec.set_pause(c->pause);
    EffectParams e;
    if (c->pause) {
        if (c->osc[0].releasing()) {
            const int i = c->dsp;
            if (i >= 0) dsp_[static_cast<std::size_t>(i)].callback = nullptr;
            c->dsp = -1;
            delete_channel(c);
            return -1;
        }
    } else {
        c->vib.inc(32028.5f / kDacRate);
        c->trem.inc(32028.5f / kDacRate);
        if (c->update_timer != 0u) {
            --c->update_timer;
            if (c->update_timer == 0u && c->callback != nullptr) channel_callback(3, c, &rec, c->callback);
        }
        const float step = 48000.0f / kDacRate;
        for (u32 i = 0; i < 2u; ++i) {
            if (c->osc[i].valid()) {
                c->osc[i].inc(step);
                effect_osc(*c, i, e);
                if (i == 0u && c->osc[i].stopped()) {
                    const int d = c->dsp;
                    if (d >= 0) dsp_[static_cast<std::size_t>(d)].callback = nullptr;
                    c->dsp = -1;
                    log("channel %u envelope ends", c->serial);
                    delete_channel(c);
                    return -1;
                }
            }
        }
    }
    std::array<u16, 6> volumes{};
    effector_param(*c, rec, volumes, e);
    for (u32 i = 0; i < 6u; ++i) rec.set_mixer_volume(i, volumes[i]);
    rec.set_pitch(channel_pitch(*c, e));
    if (!c->pause && c->key_sweep_count != 0u) {
        c->key_sweep += (c->key_sweep_target - c->key_sweep) / static_cast<float>(c->key_sweep_count);
        --c->key_sweep_count;
    }
    return 0;
}

int Engine::channel_dsp_callback(u32 type, Channel* c, const Record* rec) {
    switch (type) {
    case 0: return channel_update(c, *rec);
    case 1: return channel_initial_update(c, *rec);
    case 2:
    case 3:
        c->dsp = -1;  // the DSP channel frees itself
        delete_channel(c);
        return -1;
    default: return 0;
    }
}

int Engine::dsp_alloc(u8 priority, Channel* c) {
    s16 best_priority = 0xFF;
    int best = -1;
    u32 best_age = 0;
    int found = -1;
    for (u32 i = 0; i < std::min(kDspChannels, dsp_channel_limit); ++i) {
        const DspChannel& d = dsp_[i];
        if (d.priority < 0) {
            found = static_cast<int>(i);
            break;
        }
        if (d.priority <= priority && d.priority <= best_priority) {
            if (d.priority != best_priority || d.age > best_age) {
                best_age = d.age;
                best = static_cast<int>(i);
                best_priority = d.priority;
            }
        }
    }
    if (found < 0) found = best;
    if (found < 0) return -1;
    dsp_drop(found);
    DspChannel& d = dsp_[static_cast<std::size_t>(found)];
    d.priority = priority;
    d.age = 0;
    d.callback = c;
    return found;
}

void Engine::dsp_drop(int i) {
    DspChannel& d = dsp_[static_cast<std::size_t>(i)];
    if (Channel* c = d.callback) {
        d.callback = nullptr;
        channel_dsp_callback(3, c, nullptr);
    }
    d.callback = nullptr;
    d.priority = -1;
    d.flags &= ~1u;
    if (d.status == 0) {
        d.flags |= 2u;
        d.status = 2;
    }
}

void Engine::dsp_update(int i) {
    DspChannel& d = dsp_[static_cast<std::size_t>(i)];
    const Record rec = record(i);
    const auto stop_callback = [&]() -> int {
        if (Channel* c = d.callback) {
            d.callback = nullptr;
            return channel_dsp_callback(2, c, nullptr);
        }
        return -1;
    };
    if (rec.is_finish()) {
        d.flags &= ~2u;
        if (d.status == 0) {
            if (stop_callback() < 0) d.priority = -1;
        }
        d.status = 1;
        rec.reply_finish();
    } else if ((d.flags & 2u) != 0u) {
        d.flags &= ~2u;
        rec.force_stop();
    } else if (d.status != 2) {
        if ((d.flags & 1u) != 0u && d.status == 1) {
            d.flags &= ~1u;
            d.status = 0;
            rec.init();
            int ret = 0;
            if (Channel* c = d.callback) ret = channel_dsp_callback(1, c, &rec);
            if (ret < 0) {
                d.status = 1;
                if (stop_callback() < 0) d.priority = -1;
            } else {
                rec.play_start();
            }
        } else if (d.status != 1) {
            int ret = 0;
            if (Channel* c = d.callback) ret = channel_dsp_callback(0, c, &rec);
            if (ret < 0) {
                d.status = 1;
                if (stop_callback() < 0) d.priority = -1;
                rec.play_stop();
            } else {
                ++d.age;
            }
        }
    }
}

int Engine::parse_command(Track& t, u32 code, u32 widths) {
    struct Cmd {
        u8 args;
        u16 widths;
    };
    // JASSeqParser::sCmdInfo for 0xA0..0xFF: argument count and 2-bit widths (0 byte, 1 u16, 2 u24, 3 reg).
    static constexpr std::array<Cmd, 0x60> kCmds = [] {
        std::array<Cmd, 0x60> c{};
        const auto set = [&](u32 op, u8 n, u16 w) { c[op - 0xA0u] = {n, w}; };
        set(0xB1, 3, 0); set(0xB2, 1, 0); set(0xB3, 4, 0x40); set(0xB4, 1, 0);
        set(0xB8, 2, 0); set(0xB9, 2, 4); set(0xBA, 3, 0x10); set(0xBB, 3, 0x14);
        set(0xC1, 2, 8); set(0xC2, 1, 0); set(0xC3, 1, 2); set(0xC4, 2, 8); set(0xC5, 0, 0); set(0xC6, 1, 0);
        set(0xC7, 1, 2); set(0xC8, 2, 8); set(0xC9, 2, 0xB); set(0xCA, 2, 0xB); set(0xCB, 1, 1); set(0xCC, 0, 0);
        set(0xD0, 2, 0); set(0xD1, 2, 0xC); set(0xD2, 1, 0); set(0xD3, 1, 0); set(0xD4, 2, 0xC); set(0xD5, 2, 0xC);
        set(0xD6, 2, 0); set(0xD7, 2, 0); set(0xD8, 2, 4); set(0xD9, 3, 0x30); set(0xDA, 3, 0x10); set(0xDB, 2, 0);
        set(0xDC, 4, 0xE0);
        set(0xE0, 1, 1); set(0xE1, 1, 1); set(0xE2, 1, 0); set(0xE3, 1, 0);
        set(0xE7, 2, 4); set(0xE8, 2, 8); set(0xE9, 5, 0x155); set(0xEA, 2, 4); set(0xEB, 1, 0); set(0xEC, 4, 0x55);
        set(0xED, 1, 2);
        set(0xF0, 0, 0); set(0xF1, 1, 0); set(0xF3, 1, 2); set(0xF4, 1, 1); set(0xF5, 1, 1); set(0xF6, 0, 0);
        set(0xF7, 0, 0); set(0xF8, 2, 4); set(0xF9, 1, 1); set(0xFD, 0, 0); set(0xFE, 0, 0); set(0xFF, 0, 0);
        return c;
    }();
    if (code == 0xB0u) {
        const u32 ext = read8(t);
        log("track%X ext command 0x%02X (ignored)", t.name, ext);
        return 0;  // the only extended command is cmdDump
    }
    if (code < 0xA0u) return 0;
    const Cmd cmd = kCmds[code - 0xA0u];
    u32 type = cmd.widths | widths;
    std::array<u32, 8> a{};
    for (u32 i = 0; i < cmd.args; ++i) {
        switch (type & 3u) {
        case 0: a[i] = read8(t); break;
        case 1: a[i] = read16(t); break;
        case 2: a[i] = read24(t); break;
        case 3: a[i] = read_reg(t, read8(t)); break;
        }
        type = (type & 0xFFFFu) >> 2u;
    }
    if (trace_on_) {
        char args[96] = {};
        int n = 0;
        for (u32 i = 0; i < cmd.args && n < 80; ++i) n += std::snprintf(args + n, sizeof args - n, " %X", a[i]);
        log("track%X @%05X cmd %02X%s", t.name, t.cursor, code, args);
    }
    switch (code) {
    case 0xB1: note_on(t, a[0], a[1], a[2]); return 0;  // note on (slot, key, velocity)
    case 0xB2: note_off(t, a[0], 0); return 0;
    case 0xB3: exec_note_on_gate(t, a[1], a[2], a[3], a[0]); return 0;
    case 0xB4:
        if (a[0] < 0x100u) t.pitch = static_cast<u8>(a[0] + static_cast<u32>(transpose_total(t)));
        return 0;
    case 0xB8:
    case 0xBA: {
        s16 v = static_cast<s16>(a[1] << 8u);
        if ((a[1] & 0x80u) == 0u) v = static_cast<s16>(v | static_cast<s16>(a[1] << 1u));
        set_param(t, a[0], static_cast<float>(v) / 32767.0f, code == 0xBAu ? a[2] : 0u);
        return 0;
    }
    case 0xB9: set_param(t, a[0], static_cast<float>(static_cast<s16>(a[1])) / 32767.0f, 0); return 0;
    case 0xBB: set_param(t, a[0], static_cast<float>(static_cast<s16>(a[1])) / 32767.0f, a[2]); return 0;
    case 0xC1:
        if (Track* child = open_child(t, a[0])) {
            child->has_data = true;
            child->cursor = a[1];
            child->depth = 0;
            child->status = 1;
        }
        return 0;
    case 0xC2: close_child(t, a[0]); return 0;
    case 0xC3:
    case 0xC4:
        if (code == 0xC3u || condition(t, a[0])) {
            if (t.depth < 8u) {
                t.stack[t.depth++] = t.cursor;
                t.cursor = code == 0xC3u ? a[0] : a[1];
            }
        }
        return 0;
    case 0xC5:
    case 0xC6:
        if ((code == 0xC5u || condition(t, a[0])) && t.depth != 0u) t.cursor = t.stack[--t.depth];
        return 0;
    case 0xC7: t.cursor = a[0]; return 0;
    case 0xC8:
        if (condition(t, a[0])) t.cursor = a[1];
        return 0;
    case 0xC9: t.cursor = get24(a[1] + a[0] * 3u); return 0;
    case 0xCA:
        if (t.depth < 8u) {
            t.stack[t.depth++] = t.cursor;
            t.cursor = get24(a[1] + a[0] * 3u);
        }
        return 0;
    case 0xCB:
        if (t.depth < 8u) {
            t.stack[t.depth] = t.cursor;
            t.loops[t.depth++] = static_cast<u16>(a[0]);
        }
        return 0;
    case 0xCC:
        if (t.depth != 0u) {
            u16 count = t.loops[t.depth - 1u];
            if (count > 0u) --count;
            if (count == 0u) {
                --t.depth;
            } else {
                t.loops[t.depth - 1u] = count;
                t.cursor = t.stack[t.depth - 1u];
            }
        }
        return 0;
    case 0xD0:
        t.port_import &= static_cast<u16>(~(1u << (a[0] & 15u)));
        write_reg(t, a[1], t.ports[a[0] & 15u]);
        return 0;
    case 0xD1:
        t.port_export |= static_cast<u16>(1u << (a[0] & 15u));
        t.ports[a[0] & 15u] = static_cast<u16>(a[1]);
        return 0;
    case 0xD2: reg_write(t, 3, (t.port_import >> (a[0] & 15u)) & 1u); return 0;
    case 0xD3: reg_write(t, 3, (t.port_export >> (a[0] & 15u)) & 1u); return 0;
    case 0xD4:
    case 0xD5: {
        // JASTrack::writePort: an import, and ports 0 and 1 raise interrupts 2 and 3.
        Track* target = code == 0xD4u ? t.parent : t.children[(a[0] >> 4u) & 15u];
        if (target != nullptr) {
            const u32 port = a[0] & 15u;
            target->port_import |= static_cast<u16>(1u << port);
            target->ports[port] = static_cast<u16>(a[1]);
            if (port <= 1u) {
                const u32 intr = port == 0u ? 2u : 3u;
                if ((target->intr_mask >> intr) & 1u) target->intr_pending = static_cast<u16>(target->intr_pending | (1u << intr));
            }
        }
        return 0;
    }
    case 0xD6:
        if (t.parent != nullptr) {
            t.parent->port_export &= static_cast<u16>(~(1u << (a[0] & 15u)));
            write_reg(t, a[1], t.parent->ports[a[0] & 15u]);
        }
        return 0;
    case 0xD7:
        if (Track* child = t.children[(a[0] >> 4u) & 15u]) {
            child->port_export &= static_cast<u16>(~(1u << (a[0] & 15u)));
            write_reg(t, a[1], child->ports[a[0] & 15u]);
        }
        return 0;
    case 0xD8: write_reg(t, a[0], a[1]); return 0;
    case 0xD9:
    case 0xDA: {
        u32 reg = a[1], value = a[2];
        switch (a[0]) {
        case 0: break;
        case 1: value += read_reg(t, reg); break;
        case 2: value = read_reg(t, reg) - value; break;
        case 3: value = read_reg(t, reg) - value; reg = 3; break;
        case 4: value *= read_reg(t, reg); reg = 0x21; break;
        case 5: value &= read_reg(t, reg); break;
        case 6: value |= read_reg(t, reg); break;
        case 7: value ^= read_reg(t, reg); break;
        case 8:
            // A random value below `value` from the game-wide generator; rendered as 0, and reported.
            used_random = true;
            value = 0u;
            break;
        case 9: value = read_reg(t, reg) << value; break;
        case 10: value = read_reg(t, reg) >> value; break;
        default: return 0;
        }
        write_reg(t, reg, value);
        return 0;
    }
    case 0xDB:
        if (a[0] == 0xBu) write_reg(t, a[1], 0u - read_reg(t, a[1]));
        return 0;
    case 0xDC: {
        u32 v = 0;
        switch (a[0]) {
        case 0xC: v = byte_at(a[2] + a[3]); break;
        case 0xD: v = get16(a[2] + a[3] * 2u); break;
        case 0xE: v = get24(a[2] + a[3] * 3u); break;
        case 0xF: v = get32(a[2] + a[3] * 4u); break;
        case 0x10: v = get32(a[2] + a[3]); break;
        default: return 0;
        }
        write_reg(t, a[1], v);
        return 0;
    }
    case 0xE0: t.tempo = static_cast<u16>(a[0]); t.invalidate = true; update_tempo(t); return 0;
    case 0xE1: t.bank = (a[0] >> 8u) & 0xFFu; t.prog = a[0] & 0xFFu; return 0;
    case 0xE2: t.bank = a[0] & 0xFFu; return 0;
    case 0xE3: t.prog = a[0] & 0xFFu; return 0;
    case 0xE7:
        if (a[0] < 2u) t.osc_param[a[0]].scale = static_cast<float>(a[1]) / 16383.0f;
        return 0;
    case 0xE8:
        if (a[0] < 2u) {
            t.osc_param[a[0]].table = seq_osc_table(a[1]);
            if (a[0] != 0u) t.osc_param[a[0]].rel_table = nullptr;
        }
        return 0;
    case 0xE9:
        t.osc_param[0] = OscData{0, 1.0f, nullptr, nullptr, 1.0f, 0.0f};
        t.osc_point[1] = static_cast<s16>(a[0]);   // attack time
        t.osc_param[0].table = t.osc_point.data();
        t.osc_point[4] = static_cast<s16>(a[1]);   // decay time
        t.osc_point[7] = static_cast<s16>(a[2]);   // sustain time
        t.osc_point[8] = static_cast<s16>(a[3]);   // sustain level
        t.direct_release = static_cast<u16>(a[4]);
        return 0;
    case 0xEA:
        if (a[0] < 6u) t.mix[a[0]] = static_cast<u16>(a[1]);
        return 0;
    case 0xEB:
        used_iir_table = true;  // JASCalc::sIIRCutoff, not reproduced; load() refuses a sound that uses it
        return 0;
    case 0xEC:
        for (u32 i = 0; i < 4u; ++i) t.iir[i] = static_cast<s16>(a[i]);
        for (u32 i = 4; i < 8u; ++i) t.iir[i] = 0;
        t.filter_mode |= 0x20u;
        return 0;
    case 0xED:
        for (u32 i = 0; i < 8u; ++i) t.fir[i] = static_cast<s16>(get16(a[0] + i * 2u));
        t.filter_mode = static_cast<u16>((t.filter_mode & 0x20u) | 8u);
        return 0;
    case 0xF0: t.timer = static_cast<s32>(read_midi(t)); return 0;
    case 0xF1: t.timer = static_cast<s32>(a[0]); return 0;
    case 0xF3: t.intr_table = a[0]; return 0;
    case 0xF4: t.intr_mask = static_cast<u16>(t.intr_mask | a[0]); return 0;
    case 0xF5: t.intr_mask = static_cast<u16>(t.intr_mask & ~a[0]); return 0;
    case 0xF6:
        if (t.cursor_swap) {
            t.cursor = *t.cursor_swap;
            t.cursor_swap.reset();
        }
        [[fallthrough]];
    case 0xF7:
        if (code == 0xF7u) t.cursor_swap.reset();
        if (!t.cursor_swap) {
            const u32 pending = t.intr_mask & t.intr_pending;
            for (u32 i = 0; i < 16u; ++i) {
                if ((pending >> i) & 1u) {
                    t.intr_pending = static_cast<u16>(t.intr_pending & ~(1u << i));
                    t.cursor_swap = t.cursor;
                    t.cursor = get24(i * 3u + t.intr_table);
                    break;
                }
            }
        }
        return 0;
    case 0xF8:
        t.int_timer = static_cast<u8>(a[0]);
        t.timer54 = a[1];
        t.timer58 = a[1];
        return 0;
    case 0xF9: reg_write(t, 3, 0xFFFFu); return 0;  // no CPU sync callback on the host
    case 0xFD:
        // cmdPrintf: a NUL-terminated format, then one byte per conversion.
        for (u32 guard = 0, conversions = 0; guard < 128u; ++guard) {
            const u32 c = read8(t);
            if (c == 0u) {
                for (u32 k = 0; k < conversions; ++k) read8(t);
                break;
            }
            if (c == '\\') {
                if (read8(t) == 0u) break;
            } else if (c == '%') {
                if (read8(t) == 0u) break;
                ++conversions;
            }
        }
        return 0;
    case 0xFE: return 0;
    case 0xFF: return -1;
    default: return 0;
    }
}

int Engine::parse(Track& t) {
    const u32 data = read8(t);
    if ((data & 0x80u) == 0u) {
        const u32 ctrl = read8(t);
        const u32 slot = ctrl & 7u;
        const u32 velocity = read8(t);
        if (slot == 0u) {
            exec_note_on_gate(t, data, velocity, read_midi(t), ctrl);
        } else {
            log("track%X note on slot %u key %u vel %u", t.name, slot, data, velocity);
            note_on(t, slot, data, velocity);
        }
        return 0;
    }
    switch (data & 0xF0u) {
    case 0x80:
        if ((data & 7u) != 0u) {
            log("track%X note off slot %u", t.name, data & 7u);
            note_off(t, data & 7u, 0);
        }
        return 0;
    case 0x90: {
        u32 flags = read8(t);
        u32 widths = 0, bit = 3;
        for (u32 i = 0; i < (data & 7u) + 1u; ++i) {
            if ((flags & 0x80u) != 0u) widths |= bit;
            flags <<= 1u;
            bit <<= 2u;
        }
        return parse_command(t, read8(t), widths);
    }
    default:
        return parse_command(t, data, 0);
    }
}

int Engine::seq_tick(Track& t) {
    if (!t.has_data) return 0;
    // interrupt(6)
    if ((t.intr_mask >> 6u) & 1u) t.intr_pending = static_cast<u16>(t.intr_pending | (1u << 6u));
    // timerProcess
    if (t.timer54 != 0u) {
        if (--t.timer54 == 0u) {
            if ((t.intr_mask >> 5u) & 1u) t.intr_pending = static_cast<u16>(t.intr_pending | (1u << 5u));
            if (t.int_timer != 0u) {
                if (--t.int_timer != 0u) t.timer54 = t.timer58;
            } else {
                t.timer54 = t.timer58;
            }
        }
    }
    if (t.wait_note) {
        if (t.mgr.channels[0] != nullptr) return 0;
        t.wait_note = false;
    }
    if (t.timer > 0) --t.timer;
    // checkIntr
    if (!t.cursor_swap) {
        const u32 pending = t.intr_mask & t.intr_pending;
        for (u32 i = 0; i < 16u; ++i) {
            if ((pending >> i) & 1u) {
                t.intr_pending = static_cast<u16>(t.intr_pending & ~(1u << i));
                t.cursor_swap = t.cursor;
                t.cursor = get24(i * 3u + t.intr_table);
                break;
            }
        }
    }
    for (u32 guard = 0; (t.timer == 0 || t.cursor_swap) && !t.wait_note; ++guard) {
        if (guard > 100000u || t.cursor >= archive_.bsc_end - archive_.bsc) return -1;
        if (parse(t) < 0) return -1;
    }
    return 0;
}

int Engine::track_tick(Track& t) {
    if (t.paused) return 0;
    const int result = seq_tick(t);
    update_timed(t);
    t.invalidate = true;
    if (result < 0) return -1;
    for (u32 i = 0; i < 16u; ++i) {
        Track* child = t.children[i];
        if (child != nullptr && child->status == 1) {
            if (track_tick(*child) < 0) {
                update_seq(*root(t), false, 1.0f);
                track_close(*child);
                t.children[i] = nullptr;
            }
        }
    }
    return 0;
}

int Engine::track_seq_main(Track& t) {
    if (t.stopped) {
        update_seq(t, true, 1.0f);
        track_close(t);
        return -1;
    }
    while (t.playtime >= 1.0f) {
        t.playtime -= 1.0f;
        if (track_tick(t) < 0) {
            update_seq(t, false, 1.0f);
            log("track%X finishes", t.name);
            track_close(t);
            return -1;
        }
    }
    t.playtime += t.sample_interval;
    update_seq(t, false, 1.0f);
    return 0;
}

Rendered Engine::run(u32 seq_offset, float sound_volume, float max_seconds) {
    Rendered result;
    memory_.bytes.assign(static_cast<std::size_t>(kWavesAt - kMemBase), std::byte{0});
    seq_tables_.reserve(3u * 65u * 64u);
    for (u32 i = 0; i < 64u; ++i) {
        for (u32 k = 0; k < 4u; ++k) memory_.put16(kResamplerAt + (i * 4u + k) * 2u, static_cast<u16>(kResamplerFilter[i][k]));
    }
    // DSPRES_FILTER words 0x200..0x27F: the auto mixer's quarter-sine pan law, round(32767 sin(i pi / 256)).
    for (u32 i = 0; i < 128u; ++i) {
        const double gain = std::round(32767.0 * std::sin(static_cast<double>(i) * 3.14159265358979323846 / 256.0));
        memory_.put16(kResamplerAt + (0x200u + i) * 2u, static_cast<u16>(static_cast<s16>(gain)));
    }
    for (u32 i = 0; i < 32u; ++i) memory_.put16(kAdpcmAt + i * 2u, static_cast<u16>(kAdpcmFilter[i]));

    galaxy::host::JAudioGuestMemory guest{&memory_, [](void* user, u32 address, u32 size) {
        return static_cast<Memory*>(user)->at(address, size);
    }};
    auto mixer = std::make_unique<galaxy::host::JAudioHostMixer>();
    mixer->setup(0x8100'0000u | kDspChannels, kChannelsAt, kResamplerAt, kAdpcmAt, kFxAt, guest);
    mixer->set_varam(kWavesAt);

    sound_params_.volume = sound_volume;
    tracks_.push_back(std::make_unique<Track>());
    Track& root_track = *tracks_.back();
    track_init(root_track);
    root_track.mgr.sound = &sound_params_;
    root_track.has_data = true;
    root_track.cursor = seq_offset;
    root_track.status = 1;
    track_list_.push_back(&root_track);

    const u32 max_frames = static_cast<u32>(max_seconds * kDacRate / (kSubframeSamples * kSubframesPerFrame)) + 1u;
    std::vector<s16>& pcm = result.pcm;
    // The track starts at a subframe callback; its first channel renders in the next subframe.
    subframe_ = 0;
    for (Track* t : track_list_) track_seq_main(*t);
    for (u32 frame = 0; frame < max_frames; ++frame) {
        mixer->sync_frame(0x8200'0000u | (kSubframesPerFrame << 16u) | kDspMixerLevel, kLeftAt, kRightAt, 0, 0, guest);
        for (u32 sub = 0; sub < kSubframesPerFrame; ++sub) {
            ++subframe_;
            // JASDSPChannel::updateAll, then the mix of this subframe (on the release of group 3).
            for (u32 i = 0; i < kDspChannels; ++i) dsp_update(static_cast<int>(i));
            for (u32 group = 0; group < 4u; ++group) {
                u32 map = 0;
                for (u32 i = 0; i < 16u; ++i) map = (map << 1u) | (record(static_cast<int>(group * 16u + i)).is_active() ? 1u : 0u);
                mixer->release_halt((group << 16u) | map, guest);
            }
            // JASDriver::subframeCallback: the sequence ticks.
            for (auto it = track_list_.begin(); it != track_list_.end();) {
                if (track_seq_main(**it) < 0) {
                    it = track_list_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (u32 k = 0; k < kSubframeSamples * kSubframesPerFrame; ++k) {
            pcm.push_back(static_cast<s16>(memory_.get16(kLeftAt + k * 2u)));
            pcm.push_back(static_cast<s16>(memory_.get16(kRightAt + k * 2u)));
        }
        bool busy = !track_list_.empty();
        for (const DspChannel& d : dsp_) busy = busy || d.status != 1 || (d.flags & 1u) != 0u;
        for (const auto& c : channels_) busy = busy || c->alive;
        if (!busy) break;
        if (frame + 1u == max_frames) log("stopped at the length limit");
    }
    // Trailing silence is not part of the sound.
    std::size_t end = pcm.size();
    while (end >= 2u && pcm[end - 1u] == 0 && pcm[end - 2u] == 0) end -= 2u;
    pcm.resize(end);
    result.frames = static_cast<u32>(pcm.size() / 2u);
    result.ok = true;
    result.trace = std::move(trace_);
    return result;
}

// ---- The renderer -------------------------------------------------------------------------------------------

class SeRendererImpl final : public SeRenderer {
public:
    SeRendererImpl(const galaxy::gx::GameDisc& disc, Archive archive) : disc_(disc), archive_(std::move(archive)) {}

    Rendered render(std::string_view name, const RenderOptions& options) override {
        Rendered failed;
        u32 id = 0;
        if (auto it = archive_.ids.find(std::string(name)); it != archive_.ids.end()) {
            id = it->second;
        } else {
            unsigned s = 0, g = 0, i = 0;
            if (sscanf_s(std::string(name).c_str(), "%u,%u,%u", &s, &g, &i) != 3) {
                failed.error = "unknown sound " + std::string(name);
                return failed;
            }
            id = (s << 24u) | (g << 16u) | i;
        }
        const auto item = sound_item(archive_, id);
        if (!item || (item->first & 0xF0u) != 0x50u) {  // SE items
            failed.error = "not a sound effect in the sound table";
            return failed;
        }
        const Bytes b = archive_.bytes();
        const u32 volume_byte = b.u8at(item->second + 1u);
        // The sequence: the collection's group (the sound's group) and its index.
        const u32 group = (id >> 16u) & 0xFFu, index = id & 0xFFFFu;
        const u32 bsc = archive_.bsc;
        if (group >= b.u16at(bsc + 2u)) {
            failed.error = "no sequence group";
            return failed;
        }
        const u32 table = bsc + b.u32at(bsc + 8u + group * 4u);
        if (index >= b.u32at(table)) {
            failed.error = "no sequence";
            return failed;
        }
        const u32 seq = b.u32at(table + 4u + index * 4u);
        const float scale = options.sound_volume_scale >= 0.0f ? options.sound_volume_scale : kDefaultSoundVolumeScale;
        Engine engine(archive_, [this](const std::string& archive) { return wave_archive(archive); }, options.trace);
        engine.mute_tracks = options.mute_tracks;
        if (options.dsp_channels != 0u) engine.dsp_channel_limit = options.dsp_channels;
        Rendered r = engine.run(seq, std::min(1.0f, static_cast<float>(volume_byte) * scale), options.max_seconds);
        char header[256];
        std::snprintf(header, sizeof header,
                      "sound %s id 0x%08X volume byte 0x%02X sequence @0x%X%s%s%s%s%s\n", std::string(name).c_str(),
                      id, volume_byte, seq, engine.missing.empty() ? "" : " missing: ", engine.missing.c_str(),
                      engine.used_random ? " [random]" : "",
                      engine.used_osc_channel ? " [oscillator channel]" : "",
                      engine.used_iir_table ? " [IIR cutoff table]" : "");
        r.trace = header + r.trace;
        if (!engine.missing.empty() && r.frames == 0u) {
            r.ok = false;
            r.error = engine.missing;
        }
        if (engine.used_osc_channel || engine.used_iir_table) {
            // Data this library does not reproduce; reported so the tests can refuse it.
            r.error = "uses " + std::string(engine.used_osc_channel ? "an oscillator channel " : "") +
                (engine.used_iir_table ? "the IIR cutoff table" : "");
        }
        if (engine.used_random && r.error.empty()) r.error = "uses random effects (rendered at their mean)";
        return r;
    }

    std::vector<std::string> system_sound_names() const override { return archive_.system_names; }

private:
    // An AudioRes/Waves archive, read from the disc the first time a note needs one of its waves.
    const std::vector<std::byte>* wave_archive(const std::string& name) {
        auto it = aw_files_.find(name);
        if (it == aw_files_.end()) {
            auto data = disc_.read("AudioRes/Waves/" + name);
            it = aw_files_.emplace(name, data ? std::move(*data) : std::vector<std::byte>{}).first;
        }
        return it->second.empty() ? nullptr : &it->second;
    }

    const galaxy::gx::GameDisc& disc_;
    Archive archive_;
    std::map<std::string, std::vector<std::byte>> aw_files_;
};
}  // namespace

std::unique_ptr<SeRenderer> SeRenderer::create(const galaxy::gx::GameDisc& disc, std::string* error) {
    std::string message;
    auto fail = [&](std::string m) -> std::unique_ptr<SeRenderer> {
        if (error != nullptr) *error = std::move(m);
        return nullptr;
    };
    auto raw = disc.read("AudioRes/SMR.szs");
    if (!raw) return fail("AudioRes/SMR.szs is missing");
    std::vector<std::byte> baa;
    if (raw->size() >= 4u && std::memcmp(raw->data(), "Yaz0", 4u) == 0) {
        baa = galaxy::gx::yaz0_decompress(*raw);
        if (baa.empty()) return fail("AudioRes/SMR.szs does not decompress");
    } else {
        baa = std::move(*raw);
    }
    Archive archive;
    if (!parse_archive(std::move(baa), archive, message)) return fail(message);
    const auto dol = disc.read_main_dol();
    if (!dol) return fail("the game's DOL is missing");
    if (!find_curve_tables(*dol, archive.curves)) return fail("the envelope curve tables are not in the DOL");
    auto renderer = std::make_unique<SeRendererImpl>(disc, std::move(archive));
    return renderer;
}


// ---- The menu sounds and their playback ---------------------------------------------------------------------

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(Sound::Count)> kSoundNames{
    "SE_SY_BUTTON_CURSOR_ON",    "SE_SY_FILE_SEL_UPPER_DECIDE", "SE_SY_GALAXY_DECIDE_CANCEL",
    "SE_SY_PAUSE_ON",            "SE_SY_PAUSE_OFF",             "SE_SY_GALAMAP_WINDOW_OPEN",
    "SE_SY_GALAMAP_WINDOW_CLOSE", "SE_SY_TALK_SELECT_YES",      "SE_SY_TALK_SELECT_NO",
    "SE_SY_TALK_OK",             "SE_SY_TALK_FOCUS_ITEM",
};

constexpr u32 kVoices = 8u;

using Pcm = std::array<std::vector<float>, static_cast<std::size_t>(Sound::Count)>;

// The private output: an XAudio2 engine of its own (independent of the game's audio path) with a few
// 32 kHz stereo float voices. Built and torn down outside the player's lock.
struct Output {
#if defined(_WIN32)
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    std::array<IXAudio2SourceVoice*, kVoices> voices{};
#endif
    std::array<std::uint64_t, kVoices> started{};

    [[nodiscard]] bool ready() const {
#if defined(_WIN32)
        return engine != nullptr;
#else
        return false;
#endif
    }

    void destroy() {
#if defined(_WIN32)
        for (IXAudio2SourceVoice*& v : voices) {
            if (v != nullptr) {
                v->DestroyVoice();  // synchronous: XAudio2 reads none of the buffers afterwards
                v = nullptr;
            }
        }
        if (master != nullptr) {
            master->DestroyVoice();
            master = nullptr;
        }
        if (engine != nullptr) {
            engine->Release();
            engine = nullptr;
        }
#endif
    }

    bool create() {
#if defined(_WIN32)
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        (void)com;  // S_FALSE or RPC_E_CHANGED_MODE: COM is already set up on this thread, which is enough
        if (FAILED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || engine == nullptr) {
            engine = nullptr;
            return false;
        }
        if (FAILED(engine->CreateMasteringVoice(&master)) || master == nullptr) {
            master = nullptr;
            destroy();
            return false;
        }
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = static_cast<WORD>(kChannels);
        format.nSamplesPerSec = kSampleRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8u);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        for (IXAudio2SourceVoice*& v : voices) {
            if (FAILED(engine->CreateSourceVoice(&v, &format))) v = nullptr;
        }
        return true;
#else
        return false;
#endif
    }
};

struct Player {
    std::mutex lock;  // guards the fields below; play() only ever tries it
    Pcm pcm;
    Output output;
    std::uint64_t plays = 0;
    bool loaded = false;
};

Player& player() {
    static Player p;
    return p;
}

std::vector<float> to_float(const std::vector<std::int16_t>& pcm) {
    std::vector<float> out(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) out[i] = static_cast<float>(pcm[i]) / 32768.0f;
    return out;
}

}  // namespace

const char* sound_name(Sound sound) noexcept {
    const auto i = static_cast<std::size_t>(sound);
    return i < kSoundNames.size() ? kSoundNames[i] : "";
}

bool load(const galaxy::gx::GameDisc& disc) {
    std::string error;
    auto renderer = SeRenderer::create(disc, &error);
    if (renderer == nullptr) return false;
    Pcm pcm;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const Rendered r = renderer->render(kSoundNames[i], RenderOptions{});
        if (!r.ok || r.frames == 0u || !r.error.empty()) return false;
        pcm[i] = to_float(r.pcm);
    }
    // A new output is built before the swap and the old one destroyed after it, both outside the lock, so
    // play() never waits for XAudio2. The old buffers are freed only once their voices are gone.
    Output output;
    output.create();  // no audio device: the sounds still load, play() stays silent
    Player& p = player();
    {
        std::lock_guard guard(p.lock);
        std::swap(p.output, output);
        std::swap(p.pcm, pcm);
        p.loaded = true;
    }
    output.destroy();
    return true;
}

bool loaded() noexcept {
    Player& p = player();
    std::lock_guard guard(p.lock);
    return p.loaded;
}

std::vector<float> rendered(Sound sound) {
    Player& p = player();
    std::lock_guard guard(p.lock);
    const auto i = static_cast<std::size_t>(sound);
    return i < p.pcm.size() ? p.pcm[i] : std::vector<float>{};
}

void play(Sound sound, float volume) {
#if defined(_WIN32)
    const auto index = static_cast<std::size_t>(sound);
    if (index >= kSoundNames.size()) return;
    const galaxy::RuntimeAudioSettings audio = galaxy::get_runtime_settings().audio;
    const float gain = std::clamp(volume, 0.0f, 4.0f) * std::max(audio.master, 0.0f) * std::max(audio.sfx, 0.0f);
    Player& p = player();
    std::unique_lock guard(p.lock, std::try_to_lock);
    if (!guard.owns_lock()) return;  // a load() is swapping the sounds in; this click goes unheard
    Output& out = p.output;
    if (!p.loaded || !out.ready() || p.pcm[index].empty()) return;
    // A voice with nothing queued, else the one that started longest ago.
    u32 pick = kVoices;
    for (u32 i = 0; i < kVoices && pick == kVoices; ++i) {
        if (out.voices[i] == nullptr) continue;
        XAUDIO2_VOICE_STATE state{};
        out.voices[i]->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0u) pick = i;
    }
    if (pick == kVoices) {
        for (u32 i = 0; i < kVoices; ++i) {
            if (out.voices[i] != nullptr && (pick == kVoices || out.started[i] < out.started[pick])) pick = i;
        }
        if (pick == kVoices) return;
        out.voices[pick]->Stop(0);
        out.voices[pick]->FlushSourceBuffers();
    }
    IXAudio2SourceVoice* v = out.voices[pick];
    XAUDIO2_BUFFER buffer{};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = static_cast<UINT32>(p.pcm[index].size() * sizeof(float));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(p.pcm[index].data());
    if (FAILED(v->SubmitSourceBuffer(&buffer))) return;
    v->SetVolume(gain);
    v->Start(0);
    out.started[pick] = ++p.plays;
#else
    (void)sound;
    (void)volume;
#endif
}

bool has_output() noexcept {
    Player& p = player();
    std::lock_guard guard(p.lock);
    return p.output.ready();
}

std::uint32_t voices_playing() noexcept {
    Player& p = player();
    std::lock_guard guard(p.lock);
    std::uint32_t count = 0;
#if defined(_WIN32)
    for (IXAudio2SourceVoice* v : p.output.voices) {
        if (v == nullptr) continue;
        XAUDIO2_VOICE_STATE state{};
        v->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        count += state.BuffersQueued != 0u ? 1u : 0u;
    }
#endif
    return count;
}

void shutdown() noexcept {
    Player& p = player();
    Output output;
    Pcm pcm;
    {
        std::lock_guard guard(p.lock);
        std::swap(p.output, output);
        std::swap(p.pcm, pcm);
        p.loaded = false;
    }
    output.destroy();
}
}  // namespace galaxy::menu_sounds