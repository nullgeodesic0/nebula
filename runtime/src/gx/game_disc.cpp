#include "galaxy/gx/game_disc.h"

#include "galaxy/gx/game_asset_extract.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>

namespace galaxy::gx {

namespace {

constexpr std::size_t kMaxFileSize = 64u * 1024u * 1024u;
// The boot block's table offset and size (Wii: shifted right by 2).
constexpr std::size_t kTableOffsetField = 0x424u;
constexpr std::size_t kTableSizeField = 0x428u;

std::uint32_t be32(std::span<const std::byte> data, std::size_t offset) {
    return (static_cast<std::uint32_t>(data[offset]) << 24u) | (static_cast<std::uint32_t>(data[offset + 1u]) << 16u) |
           (static_cast<std::uint32_t>(data[offset + 2u]) << 8u) | static_cast<std::uint32_t>(data[offset + 3u]);
}

std::string normalize(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (const char c : path) {
        const char d = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (d == '/' && (out.empty() || out.back() == '/')) continue;
        out.push_back(d);
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

}  // namespace

bool GameDisc::parse_table(std::span<const std::byte> table, std::uint64_t) {
    // Entries of 12 bytes: flags/name offset, data offset (>> 2) or parent, size or next index. The root is entry
    // 0, whose size is the entry count; the names follow the entries.
    if (table.size() < 12u) return false;
    const std::uint32_t count = be32(table, 8u);
    if (count == 0u || static_cast<std::uint64_t>(count) * 12u > table.size()) return false;
    const std::size_t names = static_cast<std::size_t>(count) * 12u;
    struct Dir {
        std::string path;
        std::uint32_t end;
    };
    std::vector<Dir> dirs{{"", count}};
    for (std::uint32_t index = 1u; index < count; ++index) {
        while (dirs.size() > 1u && index >= dirs.back().end) dirs.pop_back();
        const std::size_t entry = static_cast<std::size_t>(index) * 12u;
        const std::uint32_t word = be32(table, entry);
        const bool is_dir = (word >> 24u) != 0u;
        const std::size_t name_offset = names + (word & 0x00FFFFFFu);
        if (name_offset >= table.size()) return false;
        std::string name;
        for (std::size_t i = name_offset; i < table.size() && table[i] != std::byte{0}; ++i) {
            name.push_back(static_cast<char>(table[i]));
        }
        std::string path = dirs.back().path.empty() ? name : dirs.back().path + "/" + name;
        if (is_dir) {
            dirs.push_back({std::move(path), be32(table, entry + 8u)});
        } else {
            files_.push_back({normalize(path), static_cast<std::uint64_t>(be32(table, entry + 4u)) << 2u,
                              be32(table, entry + 8u)});
        }
    }
    std::sort(files_.begin(), files_.end(), [](const File& a, const File& b) { return a.path < b.path; });
    return !files_.empty();
}

std::unique_ptr<GameDisc> GameDisc::open(const std::filesystem::path& pak_path) noexcept try {
    auto disc = std::unique_ptr<GameDisc>(new GameDisc());
    disc->stream_.open(pak_path, std::ios::binary);
    if (!disc->stream_) return nullptr;
    std::byte boot[0x430]{};
    if (!disc->read_bytes(0u, sizeof(boot), boot)) return nullptr;
    const std::uint64_t table_offset = static_cast<std::uint64_t>(be32(boot, kTableOffsetField)) << 2u;
    const std::uint64_t table_size = static_cast<std::uint64_t>(be32(boot, kTableSizeField)) << 2u;
    if (table_size == 0u || table_size > kMaxFileSize) return nullptr;
    std::vector<std::byte> table(static_cast<std::size_t>(table_size));
    if (!disc->read_bytes(table_offset, table.size(), table.data())) return nullptr;
    if (!disc->parse_table(table, table_offset)) return nullptr;
    return disc;
} catch (...) {
    return nullptr;
}

std::unique_ptr<GameDisc> GameDisc::from_image(std::vector<std::byte> image) noexcept try {
    if (image.size() < 0x430u) return nullptr;
    auto disc = std::unique_ptr<GameDisc>(new GameDisc());
    disc->image_ = std::move(image);
    const std::uint64_t table_offset = static_cast<std::uint64_t>(be32(disc->image_, kTableOffsetField)) << 2u;
    const std::uint64_t table_size = static_cast<std::uint64_t>(be32(disc->image_, kTableSizeField)) << 2u;
    if (table_offset + table_size > disc->image_.size()) return nullptr;
    const std::span<const std::byte> table(disc->image_.data() + table_offset, static_cast<std::size_t>(table_size));
    if (!disc->parse_table(table, table_offset)) return nullptr;
    return disc;
} catch (...) {
    return nullptr;
}

bool GameDisc::read_bytes(std::uint64_t offset, std::size_t size, std::byte* out) const {
    if (!image_.empty()) {
        if (offset + size > image_.size()) return false;
        std::memcpy(out, image_.data() + offset, size);
        return true;
    }
    std::lock_guard lock(stream_lock_);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    return static_cast<bool>(stream_.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size)));
}

std::optional<std::vector<std::byte>> GameDisc::read(std::string_view path) const try {
    const std::string key = normalize(path);
    const auto it = std::lower_bound(files_.begin(), files_.end(), key,
                                     [](const File& f, const std::string& k) { return f.path < k; });
    if (it == files_.end() || it->path != key || it->size > kMaxFileSize) return std::nullopt;
    std::vector<std::byte> bytes(it->size);
    if (!read_bytes(it->offset, bytes.size(), bytes.data())) return std::nullopt;
    return bytes;
} catch (...) {
    return std::nullopt;
}

std::optional<std::vector<std::byte>> GameDisc::read_main_dol() const try {
    // Boot block 0x420: the DOL's offset (>> 2). The DOL header holds 7 text and 11 data section offsets
    // (0x00, 0x1C) and sizes (0x90, 0xAC); the file ends with its last section.
    std::byte boot[0x424]{};
    if (!read_bytes(0u, sizeof(boot), boot)) return std::nullopt;
    const std::uint64_t dol = static_cast<std::uint64_t>(be32(boot, 0x420u)) << 2u;
    if (dol == 0u) return std::nullopt;
    std::byte header[0x100]{};
    if (!read_bytes(dol, sizeof(header), header)) return std::nullopt;
    std::uint64_t end = sizeof(header);
    for (std::size_t i = 0; i < 18u; ++i) {
        const std::uint32_t offset = be32(header, i * 4u);
        const std::uint32_t size = be32(header, 0x90u + i * 4u);
        if (offset != 0u && size != 0u) end = std::max<std::uint64_t>(end, static_cast<std::uint64_t>(offset) + size);
    }
    if (end > kMaxFileSize) return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    if (!read_bytes(dol, bytes.size(), bytes.data())) return std::nullopt;
    return bytes;
} catch (...) {
    return std::nullopt;
}

std::optional<std::vector<std::byte>> GameDisc::read_arc_entry(std::string_view arc_path, std::string_view entry) const try {
    auto raw = read(arc_path);
    if (!raw) return std::nullopt;
    std::vector<std::byte> archive;
    if (raw->size() >= 4u && std::memcmp(raw->data(), "Yaz0", 4u) == 0) {
        archive = yaz0_decompress(*raw);
        if (archive.empty()) return std::nullopt;
    } else {
        archive = std::move(*raw);
    }
    const auto file = rarc_find_file(archive, entry);
    if (!file) return std::nullopt;
    return std::vector<std::byte>(file->begin(), file->end());
} catch (...) {
    return std::nullopt;
}

}  // namespace galaxy::gx
