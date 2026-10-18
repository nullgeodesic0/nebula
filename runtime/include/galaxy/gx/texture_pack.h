#pragma once

// Custom texture packs in Dolphin's naming scheme. A pack is a folder of
// DDS files named tex1_<w>x<h>[_m]_<xxh64 of the level-0 GX texture bytes>_<format>.dds
// (Dolphin's HiresTextures / TextureInfo::CalculateTextureName). Files are
// indexed once and parsed lazily, so a multi-GB pack costs memory only for
// what the game actually draws.
//
// Pack folder: NEBULA_TEXTURE_PACK=<dir> (or "off"); default is Dolphin's
// %APPDATA%\Dolphin Emulator\Load\Textures\RMGE01.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dxgiformat.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace galaxy::gx {

// XXH64 (seed 0 matches Dolphin's texture naming).
[[nodiscard]] std::uint64_t texture_pack_xxh64(
    const std::uint8_t* data, std::size_t size, std::uint64_t seed = 0u);

struct PackImage {
    struct Level {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t rows = 0;       // block rows
        std::uint32_t row_bytes = 0;  // bytes per block row
        std::size_t offset = 0;       // into `data`
    };
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool block_compressed = false;
    std::vector<Level> levels;
    std::vector<std::uint8_t> data;
};

class TexturePack {
public:
    // Scans the pack folder. Returns false when no pack is configured/found.
    bool initialize();
    [[nodiscard]] bool enabled() const { return !index_.empty(); }
    [[nodiscard]] std::size_t size() const { return index_.size(); }
    [[nodiscard]] const std::filesystem::path* find(const std::string& name) const;
    // Parses a DDS file (BC1-BC7 or 32-bit RGBA/BGRA, any mip count).
    [[nodiscard]] static bool load(const std::filesystem::path& path, PackImage& out);

private:
    std::unordered_map<std::string, std::filesystem::path> index_;
};

} // namespace galaxy::gx
