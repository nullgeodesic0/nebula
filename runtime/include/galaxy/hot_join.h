#pragma once

// Hot join (docs/HOT_JOIN.md): a rollback snapshot written to a file in one process and resumed in another.
//
// A snapshot's object graph (RollbackSnapshot and every container it owns) is allocated in the snapshot arena, a
// fixed range of virtual memory: while an ArenaScope is open, this thread's operator new allocates there. The file
// is the arena's bytes, so every pointer inside the snapshot is valid in a second process that maps the arena at the
// same address. The guest threads' stacks are at fixed addresses as well (coop::Scheduler's stack pool). What the
// stacks still point at elsewhere (the simulation thread's own stack and thread-local blocks, the runtime's modules,
// a few heap objects) is listed by name with its range and moved to the second process's range when it loads.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace galaxy::hot_join {

inline constexpr std::uintptr_t kArenaBase = 0x0000300000000000ull;
inline constexpr std::size_t kArenaReserve = 0x100000000ull;  // 4 GiB reserved; committed as it fills

// Allocations of this thread go to the arena while one is open (nesting allowed).
class ArenaScope {
public:
    ArenaScope() noexcept;
    ~ArenaScope();
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;
};

// Empties the arena (nothing allocated in it may be used afterwards). False if it cannot be reserved.
bool arena_reset();
[[nodiscard]] std::size_t arena_used() noexcept;
// The second process: the arena with `size` bytes from `bytes` (an image another process wrote), ready to use.
bool arena_adopt(const std::byte* bytes, std::size_t size);

// A named host range the snapshot's stacks may point into.
struct Range {
    std::string name;
    std::uint64_t lo = 0u, hi = 0u;
};
// This process's ranges: every module image, the calling thread's stack allocation and thread-local blocks, and
// the anchors given (name, address, size).
struct Anchor {
    const char* name;
    const void* at;
    std::size_t size;
};
[[nodiscard]] std::vector<Range> host_ranges(const void* address_on_thread_stack, const std::vector<Anchor>& anchors);

// Rewrites every aligned word of `bytes` that lies in one of `from` to the same offset in the range of the same name
// in `to`. Returns the number of words moved; `unmatched` collects names of `from` that `to` lacks.
std::size_t relocate(std::byte* bytes, std::size_t size, const std::vector<Range>& from, const std::vector<Range>& to,
                     std::vector<std::string>* unmatched = nullptr);

// The file: "NBJOIN1\0", frame, root address, ranges, then the arena image (zstd is left to the transport).
struct FileHeader {
    std::uint32_t frame = 0u;
    std::uint64_t root = 0u;
    std::vector<Range> ranges;
};
// The image: the header, then the arena's bytes (what a file holds, and what a live join sends compressed).
[[nodiscard]] std::vector<std::uint8_t> make_image(const FileHeader& header);
// Reads an image's header and adopts its arena.
bool adopt_image(const std::uint8_t* data, std::size_t size, FileHeader& header);
bool write_file(const std::string& path, const FileHeader& header);
bool read_file(const std::string& path, FileHeader& header);

// Another PC's image bases: words of the adopted arena (outside `skip`: the guest RAM copies and the stacks, which
// relocate() moves with every range) that lie in a module of `from` move to that module in `to`. Returns how many.
std::size_t relocate_arena(const std::vector<std::pair<const void*, std::size_t>>& skip, const std::vector<Range>& from,
                           const std::vector<Range>& to);

// The Windows Compression API's XPRESS with Huffman: fast, and part of every Windows since 8.
[[nodiscard]] std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size);
bool decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

// Words of the arena (outside `skip`, the guest RAM copies) that point into this process's private or mapped memory
// other than the arena, the stack pool and the flat guest view: what another process would not have. Logs the first
// few and returns how many.
std::size_t audit_arena(const std::vector<std::pair<const void*, std::size_t>>& skip);

// GALAXY_HOT_JOIN_TRACE=1: marks along the resume of a loaded snapshot (each one flushed), to find where it stops.
void trace(const char* where) noexcept;
void set_resuming(bool on) noexcept;

}  // namespace galaxy::hot_join
