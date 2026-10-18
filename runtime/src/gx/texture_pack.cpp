#include "galaxy/gx/texture_pack.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace galaxy::gx {

namespace {

constexpr std::uint64_t kP1 = 11400714785074694791ull;
constexpr std::uint64_t kP2 = 14029467366897019727ull;
constexpr std::uint64_t kP3 = 1609587929392839161ull;
constexpr std::uint64_t kP4 = 9650029242287828579ull;
constexpr std::uint64_t kP5 = 2870177450012600261ull;

std::uint64_t read64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
std::uint32_t read32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::uint64_t xxh_round(std::uint64_t acc, std::uint64_t input) {
    return std::rotl(acc + input * kP2, 31) * kP1;
}
std::uint64_t xxh_merge(std::uint64_t acc, std::uint64_t val) {
    return (acc ^ xxh_round(0, val)) * kP1 + kP4;
}

std::string getenv_string(const char* name) {
    char value[1024]{};
    std::size_t length = 0;
    if (getenv_s(&length, value, sizeof(value), name) != 0 || length <= 1) {
        return {};
    }
    return value;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::uint64_t texture_pack_xxh64(
    const std::uint8_t* data, std::size_t size, std::uint64_t seed) {
    const std::uint8_t* p = data;
    const std::uint8_t* const end = data + size;
    std::uint64_t h;
    if (size >= 32u) {
        const std::uint8_t* const limit = end - 32;
        std::uint64_t v1 = seed + kP1 + kP2;
        std::uint64_t v2 = seed + kP2;
        std::uint64_t v3 = seed;
        std::uint64_t v4 = seed - kP1;
        do {
            v1 = xxh_round(v1, read64(p));
            v2 = xxh_round(v2, read64(p + 8));
            v3 = xxh_round(v3, read64(p + 16));
            v4 = xxh_round(v4, read64(p + 24));
            p += 32;
        } while (p <= limit);
        h = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h = xxh_merge(h, v1);
        h = xxh_merge(h, v2);
        h = xxh_merge(h, v3);
        h = xxh_merge(h, v4);
    } else {
        h = seed + kP5;
    }
    h += static_cast<std::uint64_t>(size);
    while (p + 8 <= end) {
        h ^= xxh_round(0, read64(p));
        h = std::rotl(h, 27) * kP1 + kP4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= static_cast<std::uint64_t>(read32(p)) * kP1;
        h = std::rotl(h, 23) * kP2 + kP3;
        p += 4;
    }
    while (p < end) {
        h ^= static_cast<std::uint64_t>(*p) * kP5;
        h = std::rotl(h, 11) * kP1;
        ++p;
    }
    h ^= h >> 33;
    h *= kP2;
    h ^= h >> 29;
    h *= kP3;
    h ^= h >> 32;
    return h;
}

bool TexturePack::initialize() {
    index_.clear();
    std::filesystem::path dir;
    const std::string configured = getenv_string("NEBULA_TEXTURE_PACK");
    if (lower(configured) == "off" || configured == "0") {
        return false;
    }
    if (!configured.empty()) {
        dir = configured;
    } else {
        const std::string appdata = getenv_string("APPDATA");
        if (appdata.empty()) {
            return false;
        }
        dir = std::filesystem::path(appdata) / "Dolphin Emulator" / "Load" /
              "Textures" / "RMGE01";
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    std::size_t skipped = 0;
    for (std::filesystem::recursive_directory_iterator it(
             dir, std::filesystem::directory_options::skip_permission_denied, ec), last;
         !ec && it != last; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const auto& path = it->path();
        if (lower(path.extension().string()) != ".dds") {
            ++skipped;
            continue;
        }
        std::string stem = lower(path.stem().string());
        if (stem.rfind("tex1_", 0) != 0) {
            ++skipped;
            continue;
        }
        // "_arb" marks authored mip chains; the texture is otherwise the same.
        if (const auto arb = stem.rfind("_arb"); arb != std::string::npos) {
            stem.erase(arb, 4);
        }
        index_.try_emplace(std::move(stem), path);
    }
    std::fprintf(stderr, "[texpack] %s: %zu textures indexed (%zu files skipped)\n",
                 dir.string().c_str(), index_.size(), skipped);
    return !index_.empty();
}

const std::filesystem::path* TexturePack::find(const std::string& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &it->second;
}

bool TexturePack::load(const std::filesystem::path& path, PackImage& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize file_size = file.tellg();
    if (file_size < 128) {
        return false;
    }
    file.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
        return false;
    }
    const std::uint8_t* h = bytes.data();
    if (read32(h) != 0x20534444u /* "DDS " */ || read32(h + 4) != 124u) {
        return false;
    }
    const std::uint32_t height = read32(h + 12);
    const std::uint32_t width = read32(h + 16);
    std::uint32_t mips = std::max(1u, read32(h + 28));
    const std::uint32_t pf_flags = read32(h + 80);
    const std::uint32_t fourcc = read32(h + 84);
    const std::uint32_t bit_count = read32(h + 88);
    const std::uint32_t r_mask = read32(h + 92);
    const std::uint32_t b_mask = read32(h + 100);
    const std::uint32_t a_mask = read32(h + 104);
    std::size_t data_offset = 128;
    if (width == 0u || height == 0u || width > 16384u || height > 16384u) {
        return false;
    }

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t block_bytes = 0;
    bool compressed = true;
    auto from_dxgi = [&](std::uint32_t dxgi) {
        switch (dxgi) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM:
            block_bytes = 8; break;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
            block_bytes = 16; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            compressed = false; block_bytes = 4; break;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
        return static_cast<DXGI_FORMAT>(dxgi);
    };
    if (fourcc == 0x30315844u /* "DX10" */) {
        if (bytes.size() < 148) {
            return false;
        }
        format = from_dxgi(read32(h + 128));
        data_offset = 148;
        if (read32(h + 140) != 0u && read32(h + 140) != 1u) {
            // Array textures (misc flag cube) are not supported.
        }
    } else if ((pf_flags & 0x4u) != 0u) {  // DDPF_FOURCC
        switch (fourcc) {
        case 0x31545844u: format = from_dxgi(DXGI_FORMAT_BC1_UNORM); break;  // DXT1
        case 0x33545844u: format = from_dxgi(DXGI_FORMAT_BC2_UNORM); break;  // DXT3
        case 0x35545844u: format = from_dxgi(DXGI_FORMAT_BC3_UNORM); break;  // DXT5
        default: break;
        }
    } else if ((pf_flags & 0x40u) != 0u && bit_count == 32u) {  // DDPF_RGB
        if (b_mask == 0x000000ffu && r_mask == 0x00ff0000u) {
            format = from_dxgi(a_mask != 0u ? DXGI_FORMAT_B8G8R8A8_UNORM
                                            : DXGI_FORMAT_B8G8R8X8_UNORM);
        } else if (r_mask == 0x000000ffu && b_mask == 0x00ff0000u) {
            format = from_dxgi(DXGI_FORMAT_R8G8B8A8_UNORM);
        }
    }
    if (format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    if (compressed && ((width | height) & 3u) != 0u) {
        return false;
    }

    // Level layout, truncated to whatever the file actually contains.
    out = PackImage{};
    out.format = format;
    out.block_compressed = compressed;
    std::size_t cursor = data_offset;
    for (std::uint32_t l = 0; l < mips; ++l) {
        PackImage::Level lv;
        lv.width = std::max(1u, width >> l);
        lv.height = std::max(1u, height >> l);
        if (compressed) {
            lv.rows = (lv.height + 3u) / 4u;
            lv.row_bytes = ((lv.width + 3u) / 4u) * block_bytes;
        } else {
            lv.rows = lv.height;
            lv.row_bytes = lv.width * 4u;
        }
        const std::size_t level_bytes = static_cast<std::size_t>(lv.rows) * lv.row_bytes;
        if (cursor + level_bytes > bytes.size()) {
            break;
        }
        lv.offset = cursor - data_offset;
        cursor += level_bytes;
        out.levels.push_back(lv);
    }
    if (out.levels.empty()) {
        return false;
    }
    out.data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
    return true;
}

} // namespace galaxy::gx
