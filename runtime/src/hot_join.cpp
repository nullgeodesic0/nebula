#include "galaxy/hot_join.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <Psapi.h>
#include <compressapi.h>
#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#pragma comment(lib, "Cabinet.lib")

namespace galaxy::hot_join {
namespace {

constexpr std::size_t kCommitStep = 64u * 1024u * 1024u;

thread_local int t_arena_depth = 0;
std::byte* g_arena = nullptr;
std::size_t g_used = 0u;
std::size_t g_committed = 0u;

bool reserve() {
    if (g_arena != nullptr) return true;
    void* const at = VirtualAlloc(reinterpret_cast<void*>(kArenaBase), kArenaReserve, MEM_RESERVE, PAGE_READWRITE);
    if (at != reinterpret_cast<void*>(kArenaBase)) {
        if (at != nullptr) VirtualFree(at, 0u, MEM_RELEASE);
        std::fprintf(stderr, "[hot-join] cannot reserve the snapshot arena at 0x%llx\n",
                     static_cast<unsigned long long>(kArenaBase));
        return false;
    }
    g_arena = static_cast<std::byte*>(at);
    return true;
}

bool commit(std::size_t end) {
    if (end <= g_committed) return true;
    if (end > kArenaReserve) return false;
    const std::size_t target = std::min(kArenaReserve, (end + kCommitStep - 1u) / kCommitStep * kCommitStep);
    if (VirtualAlloc(g_arena + g_committed, target - g_committed, MEM_COMMIT, PAGE_READWRITE) == nullptr) return false;
    g_committed = target;
    return true;
}

bool in_arena(const void* p) noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(p);
    return at >= kArenaBase && at < kArenaBase + kArenaReserve;
}

void* arena_allocate(std::size_t size, std::size_t align) {
    if (!reserve()) std::abort();
    const std::size_t at = (g_used + align - 1u) & ~(align - 1u);
    if (!commit(at + size)) {
        std::fprintf(stderr, "[hot-join] the snapshot arena is full (%zu bytes)\n", at + size);
        std::abort();
    }
    g_used = at + size;
    return g_arena + at;
}

void* allocate(std::size_t size, std::size_t align, bool nothrow) {
    if (size == 0u) size = 1u;
    if (t_arena_depth > 0) return arena_allocate(size, std::max<std::size_t>(align, 16u));
    void* const p = align > 16u ? _aligned_malloc(size, align) : std::malloc(size);
    if (p == nullptr && !nothrow) throw std::bad_alloc();
    return p;
}

void release(void* p, bool aligned) noexcept {
    // Freed snapshot objects stay in the arena until arena_reset.
    if (p == nullptr || in_arena(p)) return;
    if (aligned) {
        _aligned_free(p);
    } else {
        std::free(p);
    }
}

std::uint64_t address(const void* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

}  // namespace

ArenaScope::ArenaScope() noexcept {
    ++t_arena_depth;
}

ArenaScope::~ArenaScope() {
    // On a second process that resumed a snapshot inside a scope, this thread never opened it.
    if (t_arena_depth > 0) --t_arena_depth;
}

bool g_resuming = false;

bool trace_enabled() noexcept {
    static const bool enabled = [] {
        char* value = nullptr;
        const bool on = _dupenv_s(&value, nullptr, "GALAXY_HOT_JOIN_TRACE") == 0 && value != nullptr && value[0] == '1';
        std::free(value);
        return on;
    }();
    return enabled;
}

// With the trace: every exception after a load (the flat guest view's MMIO faults included, which are handled).
LONG CALLBACK log_exception(EXCEPTION_POINTERS* info) {
    if (g_resuming) {
        const CONTEXT& c = *info->ContextRecord;
        std::fprintf(stderr,
                     "[hot-join] resume: exception 0x%08lx at %p (rsp %p) rax %llx rbx %llx rcx %llx rdx %llx rsi %llx "
                     "rdi %llx r8 %llx r9 %llx r12 %llx r13 %llx r14 %llx r15 %llx\n",
                     info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress,
                     reinterpret_cast<void*>(c.Rsp), c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9, c.R12,
                     c.R13, c.R14, c.R15);
        const auto* stack = reinterpret_cast<const std::uint64_t*>(c.Rsp);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, "[hot-join]   [rsp+%02x] %016llx\n", i * 8, stack[i]);
        std::fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void set_resuming(bool on) noexcept {
    static void* handler = nullptr;
    if (on && handler == nullptr && trace_enabled()) handler = AddVectoredExceptionHandler(1, &log_exception);
    g_resuming = on;
}

void trace(const char* where) noexcept {
    if (!trace_enabled() || !g_resuming) return;
    std::fprintf(stderr, "[hot-join] resume: %s\n", where);
    std::fflush(stderr);
}

bool arena_reset() {
    if (!reserve()) return false;
    g_used = 0u;
    return true;
}

std::size_t arena_used() noexcept {
    return g_used;
}

bool arena_adopt(const std::byte* bytes, std::size_t size) {
    if (!reserve() || !commit(size)) return false;
    std::memcpy(g_arena, bytes, size);
    g_used = size;
    return true;
}

std::vector<Range> host_ranges(const void* address_on_thread_stack, const std::vector<Anchor>& anchors) {
    std::vector<Range> out;
    HMODULE modules[1024];
    DWORD needed = 0;
    const auto tls_array = reinterpret_cast<void**>(__readgsqword(0x58));  // TEB ThreadLocalStoragePointer
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024u; ++i) {
            MODULEINFO info{};
            char name[MAX_PATH]{};
            GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info));
            GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, MAX_PATH);
            out.push_back({std::string("module:") + name, address(info.lpBaseOfDll),
                           address(info.lpBaseOfDll) + info.SizeOfImage});
            // The module's thread-local block for this thread (implicit TLS: the loader allocates it on the heap).
            const auto* base = static_cast<const std::byte*>(info.lpBaseOfDll);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& tls = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            if (tls.VirtualAddress == 0u || tls_array == nullptr) continue;
            const auto* directory = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + tls.VirtualAddress);
            const auto index = *reinterpret_cast<const std::uint32_t*>(directory->AddressOfIndex);
            const std::uint64_t size =
                directory->EndAddressOfRawData - directory->StartAddressOfRawData + directory->SizeOfZeroFill;
            if (tls_array[index] != nullptr) {
                out.push_back({std::string("tls:") + name, address(tls_array[index]), address(tls_array[index]) + size});
            }
        }
    }
    // The thread's whole stack allocation (its reserve), whatever the TEB says while a fiber runs.
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address_on_thread_stack, &info, sizeof(info)) != 0) {
        const auto* allocation = static_cast<const std::byte*>(info.AllocationBase);
        const std::byte* at = allocation;
        for (;;) {
            MEMORY_BASIC_INFORMATION part{};
            if (VirtualQuery(at, &part, sizeof(part)) == 0 || part.AllocationBase != info.AllocationBase) break;
            at = static_cast<const std::byte*>(part.BaseAddress) + part.RegionSize;
        }
        out.push_back({"thread-stack", address(allocation), address(at)});
    }
    // Named objects first: relocate() takes the first range that holds a word, and an object on the thread's stack
    // need not be at the same depth in another process.
    std::vector<Range> objects;
    for (const Anchor& anchor : anchors) {
        if (anchor.at != nullptr) objects.push_back({std::string("object:") + anchor.name, address(anchor.at),
                                                     address(anchor.at) + anchor.size});
    }
    out.insert(out.begin(), objects.begin(), objects.end());
    return out;
}

std::size_t relocate(std::byte* bytes, std::size_t size, const std::vector<Range>& from, const std::vector<Range>& to,
                     std::vector<std::string>* unmatched) {
    struct Move {
        std::uint64_t lo, hi;
        std::int64_t delta;
    };
    std::vector<Move> moves;
    for (const Range& a : from) {
        const auto b = std::find_if(to.begin(), to.end(), [&](const Range& r) { return r.name == a.name; });
        if (b == to.end() || b->hi - b->lo != a.hi - a.lo) {
            if (unmatched != nullptr) unmatched->push_back(a.name);
            continue;
        }
        if (b->lo != a.lo) moves.push_back({a.lo, a.hi, static_cast<std::int64_t>(b->lo - a.lo)});
    }
    std::size_t moved = 0u;
    if (moves.empty()) return 0u;
    for (std::size_t at = 0u; at + 8u <= size; at += 8u) {
        std::uint64_t word;
        std::memcpy(&word, bytes + at, 8u);
        for (const Move& m : moves) {
            if (word >= m.lo && word < m.hi) {
                word = static_cast<std::uint64_t>(static_cast<std::int64_t>(word) + m.delta);
                std::memcpy(bytes + at, &word, 8u);
                ++moved;
                break;
            }
        }
    }
    return moved;
}

std::size_t audit_arena(const std::vector<std::pair<const void*, std::size_t>>& skip) {
    constexpr std::uint64_t kPoolLo = 0x0000200000000000ull, kPoolHi = 0x0000300000000000ull;
    constexpr std::uint64_t kGuestLo = 0x0000100000000000ull, kGuestHi = 0x0000100100000000ull;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> foreign;  // committed private or mapped regions
    for (std::uint64_t at = 0x10000u; at < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0) break;
        const std::uint64_t lo = address(info.BaseAddress);
        const std::uint64_t hi = lo + info.RegionSize;
        if (info.State == MEM_COMMIT && (info.Type == MEM_PRIVATE || info.Type == MEM_MAPPED) &&
            !(lo >= kArenaBase && lo < kArenaBase + kArenaReserve) && !(lo >= kPoolLo && lo < kPoolHi) &&
            !(lo >= kGuestLo && lo < kGuestHi)) {
            foreign.push_back({lo, hi});
        }
        at = hi;
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> skipped;
    for (const auto& [p, n] : skip) skipped.push_back({address(p), address(p) + n});
    std::sort(skipped.begin(), skipped.end());
    std::size_t hits = 0u;
    for (std::size_t at = 0u; at + 8u <= g_used; at += 8u) {
        const std::uint64_t here = address(g_arena + at);
        const auto s = std::upper_bound(skipped.begin(), skipped.end(), std::pair{here, ~0ull});
        if (s != skipped.begin() && here < std::prev(s)->second) {
            at = std::prev(s)->second - address(g_arena) - 8u;
            at &= ~std::size_t{7};
            continue;
        }
        std::uint64_t word;
        std::memcpy(&word, g_arena + at, 8u);
        const auto f = std::upper_bound(foreign.begin(), foreign.end(), std::pair{word, ~0ull});
        if (f == foreign.begin() || word >= std::prev(f)->second) continue;
        if (++hits <= 24u) {
            std::fprintf(stderr, "[hot-join] audit: arena+0x%zx = 0x%016llx (region 0x%llx..0x%llx)\n", at,
                         static_cast<unsigned long long>(word), static_cast<unsigned long long>(std::prev(f)->first),
                         static_cast<unsigned long long>(std::prev(f)->second));
        }
    }
    std::fprintf(stderr, "[hot-join] audit: %zu arena words point into this process's own memory\n", hits);
    return hits;
}

std::vector<std::uint8_t> make_image(const FileHeader& header) {
    std::vector<std::uint8_t> out;
    out.reserve(g_used + 8192u);
    const auto put = [&](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    const auto u32 = [&](std::uint32_t v) { put(&v, sizeof(v)); };
    const auto u64 = [&](std::uint64_t v) { put(&v, sizeof(v)); };
    put("NBJOIN1\0", 8);
    u32(header.frame);
    u64(header.root);
    u64(kArenaBase);
    u64(g_used);
    u32(static_cast<std::uint32_t>(header.ranges.size()));
    for (const Range& range : header.ranges) {
        u64(range.lo);
        u64(range.hi);
        u32(static_cast<std::uint32_t>(range.name.size()));
        put(range.name.data(), range.name.size());
    }
    put(g_arena, g_used);
    return out;
}

bool adopt_image(const std::uint8_t* data, std::size_t size, FileHeader& header) {
    std::size_t at = 0u;
    bool ok = true;
    const auto take = [&](void* out, std::size_t n) {
        ok = ok && at + n <= size;
        if (ok) std::memcpy(out, data + at, n);
        at += n;
    };
    const auto u32 = [&] {
        std::uint32_t v = 0u;
        take(&v, sizeof(v));
        return v;
    };
    const auto u64 = [&] {
        std::uint64_t v = 0u;
        take(&v, sizeof(v));
        return v;
    };
    char magic[8]{};
    take(magic, 8);
    ok = ok && std::memcmp(magic, "NBJOIN1\0", 8) == 0;
    header.frame = u32();
    header.root = u64();
    const std::uint64_t base = u64();
    const std::uint64_t used = u64();
    const std::uint32_t count = u32();
    header.ranges.clear();
    for (std::uint32_t i = 0; ok && i < count; ++i) {
        Range range;
        range.lo = u64();
        range.hi = u64();
        const std::uint32_t length = u32();
        ok = ok && length < 4096u && at + length <= size;
        if (!ok) break;
        range.name.assign(reinterpret_cast<const char*>(data + at), length);
        at += length;
        header.ranges.push_back(std::move(range));
    }
    ok = ok && base == kArenaBase && used <= kArenaReserve && at + used == size && reserve() && commit(used);
    if (!ok) return false;
    std::memcpy(g_arena, data + at, used);
    g_used = used;
    return true;
}

bool write_file(const std::string& path, const FileHeader& header) {
    const std::vector<std::uint8_t> image = make_image(header);
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr) return false;
    const bool ok = std::fwrite(image.data(), 1, image.size(), file) == image.size();
    return std::fclose(file) == 0 && ok;
}

bool read_file(const std::string& path, FileHeader& header) {
    std::FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr) return false;
    std::vector<std::uint8_t> image;
    std::uint8_t buffer[1u << 16];
    for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) != 0u;) image.insert(image.end(), buffer, buffer + n);
    std::fclose(file);
    return adopt_image(image.data(), image.size(), header);
}

std::size_t relocate_arena(const std::vector<std::pair<const void*, std::size_t>>& skip, const std::vector<Range>& from,
                           const std::vector<Range>& to) {
    std::vector<Range> modules_from, modules_to;
    for (const Range& r : from) {
        if (r.name.rfind("module:", 0) == 0) modules_from.push_back(r);
    }
    for (const Range& r : to) {
        if (r.name.rfind("module:", 0) == 0) modules_to.push_back(r);
    }
    std::vector<std::pair<std::size_t, std::size_t>> holes;  // arena offsets [begin, end) left alone
    for (const auto& [p, n] : skip) {
        // Empty entries (a guest memory view copied under another alias) have no bytes, here or anywhere.
        if (n == 0u || !in_arena(p)) continue;
        const std::size_t begin = static_cast<std::size_t>(address(p) - kArenaBase);
        holes.push_back({begin & ~std::size_t{7}, std::min(begin + n, g_used)});
    }
    std::sort(holes.begin(), holes.end());
    std::size_t moved = 0u;
    std::size_t at = 0u;
    for (const auto& [begin, end] : holes) {
        if (begin > at) moved += relocate(g_arena + at, begin - at, modules_from, modules_to);
        at = std::max(at, (end + 7u) & ~std::size_t{7});
    }
    if (at < g_used) moved += relocate(g_arena + at, g_used - at, modules_from, modules_to);
    return moved;
}

std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> out;
    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &compressor)) return out;
    SIZE_T needed = 0u;
    (void)Compress(compressor, data, size, nullptr, 0u, &needed);
    out.resize(needed);
    if (!Compress(compressor, data, size, out.data(), out.size(), &needed)) needed = 0u;
    out.resize(needed);
    CloseCompressor(compressor);
    return out;
}

bool decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &decompressor)) return false;
    SIZE_T needed = 0u;
    (void)Decompress(decompressor, data, size, nullptr, 0u, &needed);
    out.resize(needed);
    const bool ok = Decompress(decompressor, data, size, out.data(), out.size(), &needed) && needed == out.size();
    CloseDecompressor(decompressor);
    return ok;
}

}  // namespace galaxy::hot_join

// The global allocation functions: the snapshot arena while an ArenaScope is open on this thread, the CRT heap
// otherwise (what the default ones do).
void* operator new(std::size_t size) {
    return galaxy::hot_join::allocate(size, 16u, false);
}
void* operator new[](std::size_t size) {
    return galaxy::hot_join::allocate(size, 16u, false);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return galaxy::hot_join::allocate(size, 16u, true);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return galaxy::hot_join::allocate(size, 16u, true);
}
void* operator new(std::size_t size, std::align_val_t align) {
    return galaxy::hot_join::allocate(size, static_cast<std::size_t>(align), false);
}
void* operator new[](std::size_t size, std::align_val_t align) {
    return galaxy::hot_join::allocate(size, static_cast<std::size_t>(align), false);
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return galaxy::hot_join::allocate(size, static_cast<std::size_t>(align), true);
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return galaxy::hot_join::allocate(size, static_cast<std::size_t>(align), true);
}
void operator delete(void* p) noexcept {
    galaxy::hot_join::release(p, false);
}
void operator delete[](void* p) noexcept {
    galaxy::hot_join::release(p, false);
}
void operator delete(void* p, std::size_t) noexcept {
    galaxy::hot_join::release(p, false);
}
void operator delete[](void* p, std::size_t) noexcept {
    galaxy::hot_join::release(p, false);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    galaxy::hot_join::release(p, false);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    galaxy::hot_join::release(p, false);
}
void operator delete(void* p, std::align_val_t align) noexcept {
    galaxy::hot_join::release(p, static_cast<std::size_t>(align) > 16u);
}
void operator delete[](void* p, std::align_val_t align) noexcept {
    galaxy::hot_join::release(p, static_cast<std::size_t>(align) > 16u);
}
void operator delete(void* p, std::size_t, std::align_val_t align) noexcept {
    galaxy::hot_join::release(p, static_cast<std::size_t>(align) > 16u);
}
void operator delete[](void* p, std::size_t, std::align_val_t align) noexcept {
    galaxy::hot_join::release(p, static_cast<std::size_t>(align) > 16u);
}
void operator delete(void* p, std::align_val_t align, const std::nothrow_t&) noexcept {
    galaxy::hot_join::release(p, static_cast<std::size_t>(align) > 16u);
}
void operator delete[](void* p, std::align_val_t align, const std::nothrow_t&) noexcept {
    galaxy::hot_join::release(p, static_cast<std::size_t>(align) > 16u);
}
