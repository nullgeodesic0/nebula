#pragma once

// Read-only access to files inside the user's game.pak (the disc's data partition as built by build-pak, with
// its file system table) for the host-side menus: the game's font, window frames and sounds. Never touches
// guest memory, and is independent of the guest's own disc reads.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galaxy::gx {

class GameDisc {
public:
    // Opens game.pak and reads its file system table. std::nullopt when the file is missing or not a disc image.
    [[nodiscard]] static std::unique_ptr<GameDisc> open(const std::filesystem::path& pak_path) noexcept;
    // The table alone, for tests: `image` holds the partition from offset 0 (the boot block and the table).
    [[nodiscard]] static std::unique_ptr<GameDisc> from_image(std::vector<std::byte> image) noexcept;

    // A file by its disc path ("LayoutData/Font.arc", any case, '/' or '\\'). Yaz0 is left as is.
    // std::nullopt when absent or unreadable. Files over 64 MiB are refused.
    [[nodiscard]] std::optional<std::vector<std::byte>> read(std::string_view path) const;
    // read() then Yaz0-decompressed when compressed, then the RARC entry `entry` (exact name): an archive's file.
    [[nodiscard]] std::optional<std::vector<std::byte>> read_arc_entry(
        std::string_view arc_path, std::string_view entry) const;

    // The game's executable (the boot block's DOL), whole: header and sections as stored on the disc.
    // std::nullopt when the boot block names no readable DOL.
    [[nodiscard]] std::optional<std::vector<std::byte>> read_main_dol() const;

    [[nodiscard]] std::size_t file_count() const noexcept { return files_.size(); }

private:
    struct File {
        std::string path;  // lower case, '/' separated
        std::uint64_t offset;
        std::uint32_t size;
    };
    bool parse_table(std::span<const std::byte> table, std::uint64_t table_offset);
    [[nodiscard]] bool read_bytes(std::uint64_t offset, std::size_t size, std::byte* out) const;

    std::vector<File> files_;
    std::vector<std::byte> image_;           // from_image only
    mutable std::ifstream stream_;           // open only
    mutable std::mutex stream_lock_;
};

}  // namespace galaxy::gx
