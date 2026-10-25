#include "galaxy/animation_packs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>

#include "galaxy/gx/game_asset_extract.h"

namespace galaxy::animation_packs {
namespace {

constexpr std::string_view kArchive = "ObjectData/MarioAnime.arc";

std::uint32_t be32(std::span<const std::byte> d, std::size_t o) {
    return (std::to_integer<std::uint32_t>(d[o]) << 24) | (std::to_integer<std::uint32_t>(d[o + 1]) << 16) |
           (std::to_integer<std::uint32_t>(d[o + 2]) << 8) | std::to_integer<std::uint32_t>(d[o + 3]);
}
std::uint16_t be16(std::span<const std::byte> d, std::size_t o) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint32_t>(d[o]) << 8) | std::to_integer<std::uint32_t>(d[o + 1]));
}
void put32(std::vector<std::byte>& d, std::size_t o, std::uint32_t v) {
    d[o] = std::byte(v >> 24); d[o + 1] = std::byte(v >> 16); d[o + 2] = std::byte(v >> 8); d[o + 3] = std::byte(v);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(raw.size());
    std::transform(raw.begin(), raw.end(), out.begin(), [](char c) { return std::byte(static_cast<unsigned char>(c)); });
    return out;
}

// A file of the disc: its file-table entry's disc offset, and its data's offset and size.
struct DiscFile {
    std::uint64_t entry{};
    std::uint64_t offset{};
    std::uint32_t size{};
};

std::optional<DiscFile> find_file(std::span<const std::byte> pak, std::string_view path) {
    if (pak.size() < 0x428u) return std::nullopt;
    const std::uint64_t fst = static_cast<std::uint64_t>(be32(pak, 0x424)) << 2;
    if (fst + 12u > pak.size()) return std::nullopt;
    const std::uint32_t count = be32(pak, fst + 8);
    const std::uint64_t names = fst + static_cast<std::uint64_t>(count) * 12u;
    if (names > pak.size()) return std::nullopt;
    const auto name_of = [&](std::uint32_t i) {
        const std::uint64_t at = names + (be32(pak, fst + i * 12u) & 0x00FFFFFFu);
        std::string s;
        for (std::uint64_t p = at; p < pak.size() && pak[p] != std::byte{0}; ++p) s += static_cast<char>(pak[p]);
        return lower(s);
    };
    // Walk the path one directory at a time (the FST lists a directory's entries up to its end index).
    std::vector<std::string> parts;
    for (std::size_t a = 0; a <= path.size();) {
        const std::size_t b = std::min(path.find('/', a), path.size());
        parts.push_back(lower(std::string(path.substr(a, b - a))));
        a = b + 1;
    }
    std::uint32_t i = 1, end = count;
    for (std::size_t k = 0; k < parts.size(); ++k) {
        bool found = false;
        while (i < end) {
            const bool dir = (be32(pak, fst + i * 12u) >> 24) != 0;
            if (name_of(i) == parts[k]) {
                if (k + 1 == parts.size()) {
                    if (dir) return std::nullopt;
                    return DiscFile{fst + i * 12u, static_cast<std::uint64_t>(be32(pak, fst + i * 12u + 4)) << 2,
                                    be32(pak, fst + i * 12u + 8)};
                }
                if (!dir) return std::nullopt;
                end = be32(pak, fst + i * 12u + 8);
                ++i;
                found = true;
                break;
            }
            i = dir ? be32(pak, fst + i * 12u + 8) : i + 1;
        }
        if (!found) return std::nullopt;
    }
    return std::nullopt;
}

// The RARC with the named files' contents replaced (directory and node tables kept), as the tools do.
std::vector<std::byte> rarc_replace(std::span<const std::byte> arc,
                                    const std::map<std::string, std::vector<std::byte>>& replace) {
    const std::uint32_t node_count = be32(arc, 0x20), file_count = be32(arc, 0x28);
    (void)node_count;
    const std::uint32_t files = 0x20 + be32(arc, 0x2C), strings = 0x20 + be32(arc, 0x34);
    const std::uint32_t data = 0x20 + be32(arc, 0x0C);
    std::vector<std::byte> head(arc.begin(), arc.begin() + data);
    std::vector<std::byte> out;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> placed;  // old data offset -> new
    std::vector<std::uint32_t> order;
    for (std::uint32_t j = 0; j < file_count; ++j) order.push_back(j);
    const auto entry = [&](std::uint32_t j) { return files + j * 20u; };
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return be32(arc, entry(a) + 8) < be32(arc, entry(b) + 8);
    });
    for (const std::uint32_t j : order) {
        const std::uint32_t e = entry(j);
        const bool dir = be16(arc, e) == 0xFFFFu || (std::to_integer<std::uint32_t>(arc[e + 4]) & 0x02u) != 0;
        if (dir) continue;
        std::string name;
        for (std::uint32_t p = strings + be16(arc, e + 6); arc[p] != std::byte{0}; ++p) name += static_cast<char>(arc[p]);
        const std::uint32_t old = be32(arc, e + 8), size = be32(arc, e + 12);
        if (!placed.contains(old)) {
            const auto it = replace.find(name);
            const std::size_t at = out.size();
            if (it != replace.end()) {
                out.insert(out.end(), it->second.begin(), it->second.end());
            } else {
                out.insert(out.end(), arc.begin() + data + old, arc.begin() + data + old + size);
            }
            placed[old] = {static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(out.size() - at)};
            out.resize((out.size() + 31u) & ~std::size_t{31u}, std::byte{0});
        }
        put32(head, e + 8, placed[old].first);
        put32(head, e + 12, placed[old].second);
    }
    put32(head, 0x04, static_cast<std::uint32_t>(head.size() + out.size()));
    put32(head, 0x10, static_cast<std::uint32_t>(out.size()));
    put32(head, 0x14, static_cast<std::uint32_t>(out.size()));
    head.insert(head.end(), out.begin(), out.end());
    return head;
}

std::vector<std::filesystem::path> pack_folders(const std::filesystem::path& content_root) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::filesystem::path root = content_root / L"mods" / L"animation-packs";
    if (!std::filesystem::is_directory(root, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (e.is_directory(ec)) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    // Loose files in the pack folder itself are a pack too.
    out.insert(out.begin(), root);
    return out;
}

}  // namespace

bool present(const std::filesystem::path& content_root) {
    std::error_code ec;
    const std::filesystem::path root = content_root / L"mods" / L"animation-packs";
    if (!std::filesystem::is_directory(root, ec)) return false;
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".ank1") return true;
    }
    return false;
}

Result build(const std::filesystem::path& content_root, std::span<const std::byte> pak,
             std::span<const std::pair<std::uint64_t, std::uint64_t>> taken, bool custom_model) {
    Result result;
    const std::optional<DiscFile> file = find_file(pak, kArchive);
    if (!file || file->offset + file->size > pak.size()) {
        result.note = "MarioAnime.arc not found on the disc";
        return result;
    }
    const std::span<const std::byte> packed = pak.subspan(file->offset, file->size);
    std::vector<std::byte> arc = packed.size() >= 4 && std::memcmp(packed.data(), "Yaz0", 4) == 0
                                     ? gx::yaz0_decompress(packed)
                                     : std::vector<std::byte>(packed.begin(), packed.end());
    if (arc.size() < 0x40u || std::memcmp(arc.data(), "RARC", 4) != 0) {
        result.note = "MarioAnime.arc is not an archive";
        return result;
    }
    std::map<std::string, std::span<const std::byte>> game;
    for (const gx::RarcFile& f : gx::rarc_list_files(arc)) game[lower(f.name)] = f.data;

    // Later packs win over earlier ones; a pack's model/ files over its own when the custom model is on.
    std::map<std::string, std::vector<std::byte>> replace;
    for (const std::filesystem::path& folder : pack_folders(content_root)) {
        bool any = false;
        std::vector<std::filesystem::path> sources{folder};
        if (custom_model) sources.push_back(folder / L"model");
        for (const std::filesystem::path& source : sources) {
            std::error_code ec;
            if (!std::filesystem::is_directory(source, ec)) continue;
            for (const auto& e : std::filesystem::directory_iterator(source, ec)) {
                if (!e.is_regular_file(ec) || lower(e.path().extension().string()) != ".ank1") continue;
                const std::string bck = lower(e.path().stem().string()) + ".bck";
                const auto original = game.find(bck);
                const std::optional<std::vector<std::byte>> ank = read_file(e.path());
                if (original == game.end() || original->second.size() < 32u || !ank || ank->size() < 8u ||
                    std::memcmp(ank->data(), "ANK1", 4) != 0) {
                    continue;
                }
                std::vector<std::byte> b(original->second.begin(), original->second.begin() + 32);
                put32(b, 8, static_cast<std::uint32_t>(32u + ank->size()));
                b.insert(b.end(), ank->begin(), ank->end());
                replace[bck] = std::move(b);
                any = true;
            }
        }
        result.packs += any ? 1u : 0u;
    }
    if (replace.empty()) {
        result.note = "no animation of MarioAnime.arc in the packs";
        return result;
    }
    std::vector<std::byte> rebuilt = rarc_replace(arc, replace);
    rebuilt.resize((rebuilt.size() + 31u) & ~std::size_t{31u}, std::byte{0});

    // A place for it: an unused stretch of the disc (one repeated filler byte) between the file table and
    // the first file, clear of the other mods.
    const std::uint64_t fst = static_cast<std::uint64_t>(be32(pak, 0x424)) << 2;
    std::uint64_t first = pak.size();
    const std::uint32_t count = be32(pak, fst + 8);
    for (std::uint32_t i = 1; i < count; ++i) {
        if ((be32(pak, fst + i * 12u) >> 24) == 0) {
            first = std::min<std::uint64_t>(first, static_cast<std::uint64_t>(be32(pak, fst + i * 12u + 4)) << 2);
        }
    }
    constexpr std::uint64_t kAlign = 0x100000;
    std::optional<std::uint64_t> area;
    for (std::uint64_t at = (fst + 0x400000 + kAlign - 1) / kAlign * kAlign; at + rebuilt.size() <= first; at += kAlign) {
        const bool clear = std::none_of(taken.begin(), taken.end(), [&](const auto& r) {
            return at < r.first + r.second && r.first < at + rebuilt.size();
        });
        if (!clear) continue;
        const std::span<const std::byte> region = pak.subspan(at, rebuilt.size());
        if (std::all_of(region.begin(), region.end(), [&](std::byte b) { return b == region[0]; })) {
            area = at;
            break;
        }
    }
    if (!area) {
        result.note = "no unused disc area for the rebuilt MarioAnime.arc";
        return result;
    }
    std::vector<std::byte> entry(pak.begin() + file->entry, pak.begin() + file->entry + 12);
    put32(entry, 4, static_cast<std::uint32_t>(*area >> 2));
    put32(entry, 8, static_cast<std::uint32_t>(rebuilt.size()));
    result.replaced = replace.size();
    result.note = std::to_string(result.replaced) + " animations from " + std::to_string(result.packs) +
                  " pack(s), MarioAnime.arc rebuilt (" + std::to_string(rebuilt.size()) + " bytes)";
    result.overrides.push_back(Override{file->entry, std::move(entry)});
    result.overrides.push_back(Override{*area, std::move(rebuilt)});
    return result;
}

}  // namespace galaxy::animation_packs
