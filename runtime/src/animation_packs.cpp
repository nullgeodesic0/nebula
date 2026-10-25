// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/animation_packs.h"

#include "galaxy/gx/game_asset_extract.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace galaxy::mods {
namespace {

std::uint32_t be32(std::span<const std::byte> d, std::size_t o) {
    return (std::to_integer<std::uint32_t>(d[o]) << 24) | (std::to_integer<std::uint32_t>(d[o + 1]) << 16) |
           (std::to_integer<std::uint32_t>(d[o + 2]) << 8) | std::to_integer<std::uint32_t>(d[o + 3]);
}

std::uint16_t be16(std::span<const std::byte> d, std::size_t o) {
    return static_cast<std::uint16_t>((std::to_integer<std::uint32_t>(d[o]) << 8) | std::to_integer<std::uint32_t>(d[o + 1]));
}

void put32(std::vector<std::byte>& d, std::size_t o, std::uint32_t v) {
    d[o] = std::byte(v >> 24);
    d[o + 1] = std::byte(v >> 16);
    d[o + 2] = std::byte(v >> 8);
    d[o + 3] = std::byte(v);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// The archive with the named files' contents replaced; its directories and names are kept, its data repacked.
std::vector<std::byte> rarc_replace(std::span<const std::byte> arc, const std::map<std::string, std::vector<std::byte>>& replace) {
    const std::uint32_t file_count = be32(arc, 0x28);
    const std::uint32_t files = 0x20 + be32(arc, 0x2C);
    const std::uint32_t strings = 0x20 + be32(arc, 0x34);
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
        for (std::uint32_t p = strings + be16(arc, e + 6); p < arc.size() && arc[p] != std::byte{0}; ++p) {
            name += static_cast<char>(arc[p]);
        }
        const std::uint32_t old = be32(arc, e + 8);
        const std::uint32_t size = be32(arc, e + 12);
        if (!placed.contains(old)) {
            const auto it = replace.find(lower(name));
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

}  // namespace

std::vector<std::uint8_t> replace_animations(std::span<const std::uint8_t> arc_bytes,
                                             const std::map<std::string, std::vector<std::uint8_t>>& ank1_by_bck,
                                             unsigned* replaced) {
    if (replaced != nullptr) *replaced = 0;
    const std::span<const std::byte> packed = std::as_bytes(arc_bytes);
    std::vector<std::byte> arc = packed.size() >= 4 && std::memcmp(packed.data(), "Yaz0", 4) == 0
                                     ? gx::yaz0_decompress(packed)
                                     : std::vector<std::byte>(packed.begin(), packed.end());
    if (arc.size() < 0x40u || std::memcmp(arc.data(), "RARC", 4) != 0) return {};
    std::map<std::string, std::span<const std::byte>> own;
    for (const gx::RarcFile& f : gx::rarc_list_files(arc)) own[lower(f.name)] = f.data;
    std::map<std::string, std::vector<std::byte>> replace;
    for (const auto& [bck, ank1] : ank1_by_bck) {
        const auto original = own.find(bck);
        if (original == own.end() || original->second.size() < 32u || ank1.size() < 8u ||
            std::memcmp(ank1.data(), "ANK1", 4) != 0) {
            continue;
        }
        // The animation's own J3D file header, sized for the pack's section.
        std::vector<std::byte> file(original->second.begin(), original->second.begin() + 32);
        put32(file, 8, static_cast<std::uint32_t>(32u + ank1.size()));
        const std::span<const std::byte> section = std::as_bytes(std::span(ank1));
        file.insert(file.end(), section.begin(), section.end());
        replace[bck] = std::move(file);
    }
    if (replaced != nullptr) *replaced = static_cast<unsigned>(replace.size());
    const std::vector<std::byte> rebuilt = rarc_replace(arc, replace);
    std::vector<std::uint8_t> out(rebuilt.size());
    std::memcpy(out.data(), rebuilt.data(), rebuilt.size());
    return out;
}

}  // namespace galaxy::mods
