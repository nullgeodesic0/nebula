// The native game's host (docs/NATIVE_PORT.md, "Module layout"): loads the game module (smg_game.dll), gives it
// Nebula's services (nebula_game_api.h) and runs it. Built three ways:
//   - nebula_native_host (native/CMakeLists.txt): headless. The GX stream is released as it arrives and PE events
//     answered at once; audio is paced but not played; no remotes connect.
//   - NebulaNative (the top-level build, NEBULA_HOST_RENDERER): the stream goes to Nebula's D3D12 renderer, which
//     raises the PE events and presents the frame buffer the game displays.
//   - On SDL platforms (platform/sdl, NEBULA_HOST_SDL): the same with the Vulkan renderer, SDL's window, sound and
//     input; on Android SDL's activity calls SDL_main.
//
//   <host> --pak <game.pak> --nand <dir> [--module <smg_game.dll>] [--seconds N]
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <nebula_game_api.h>

#include "galaxy/atomic_file.h"
#include "galaxy/crash_report.h"
#include "galaxy/discord_presence.h"
#include "galaxy/mods.h"
#if defined(NEBULA_HOST_RENDERER)
#include "galaxy/experimental_ultrawide_aspect.h"
#include "galaxy/frame_timeline.h"
#include "galaxy/gx/fifo_parser.h"
#include "galaxy/mii_import.h"
#include "galaxy/gx_d3d12.h"
#include "galaxy/guest_memory.h"
#include "galaxy/native_host.h"
#include "galaxy/native_input.h"
#include "galaxy/net_lockstep.h"
#include "galaxy/native_thp_video.h"
#include "galaxy/perf_hint.h"
#include "galaxy/achievements.h"
#include "galaxy/button_prompts.h"
#include "galaxy/stats.h"
#include "galaxy/runtime_settings.h"
#include "galaxy/gx/render_config.h"

#if defined(_WIN32)
#include <crtdbg.h>
#else
#include "galaxy/virtual_keys.h"
#endif
#endif
#if !defined(NEBULA_HOST_RENDERER)
namespace galaxy::host {
// The headless host gives the platform no performance hints.
inline void perf_hint_add_current_thread() noexcept {}
inline int64_t perf_hint_retrace() noexcept { return 0; }
}  // namespace galaxy::host
#endif
#if defined(NEBULA_HOST_SDL)
#include "sdl_host.h"

#include <SDL3/SDL_main.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <iterator>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <intrin.h>
#include <dbghelp.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

// Sleeps until `when` on a high-resolution waitable timer (Windows 10 1803+; the scheduler tick otherwise), so the
// retrace, the audio interface and the remotes keep their periods to well under a millisecond.
void sleep_until_precise(Clock::time_point when) {
#if defined(_WIN32)
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const auto now = Clock::now();
    if (when <= now) {
        return;
    }
    if (timer == nullptr) {
        std::this_thread::sleep_until(when);
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>((std::chrono::duration_cast<std::chrono::nanoseconds>(when - now).count() + 99) / 100);
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
#else
    std::this_thread::sleep_until(when);
#endif
}
const Clock::time_point kStart = Clock::now();

void log_line(const char* tag, const char* text) {
    const double t = std::chrono::duration<double>(Clock::now() - kStart).count();
    std::fprintf(stdout, "[%9.3f] %s %s\n", t, tag, text);
    std::fflush(stdout);
}

// Crash reports (galaxy/crash_report.h): why Nebula stopped, as a code with what to do. What the failing thread knew,
// kept for the report (gathered on that thread: its activity is its own).
galaxy::crash::Facts gCrashFacts;
char gCrashText[1024];
char gCrashActivityFile[512];
char gCrashModRead[640];

void gather_crash_facts(galaxy::crash::Facts::Kind kind, const char* text, bool in_game = false,
                        bool stack_overflow = false) noexcept {
    std::snprintf(gCrashText, sizeof(gCrashText), "%s", text != nullptr ? text : "");
    gCrashFacts.kind = kind;
    gCrashFacts.in_game = in_game;
    gCrashFacts.stack_overflow = stack_overflow;
    gCrashFacts.text = gCrashText;
    gCrashFacts.activity = galaxy::crash::current_activity(gCrashActivityFile, sizeof(gCrashActivityFile));
    gCrashFacts.activity_file = gCrashActivityFile;
    double seconds = -1.0;
    if (galaxy::crash::last_mod_read(gCrashModRead, sizeof(gCrashModRead), &seconds)) {
        gCrashFacts.last_mod_read = gCrashModRead;
        gCrashFacts.last_mod_seconds = seconds;
    }
    gCrashFacts.mods_change_game = galaxy::crash::session_mods_change_game();
}

// Crash dumps and reports: GALAXY_CRASH_DUMP_DIR, else %LOCALAPPDATA%\Nebula\crashes for the player's executable and
// crashes\ in the working directory for the headless test host.
fs::path crash_folder() {
#if defined(_WIN32)
    wchar_t dir[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"GALAXY_CRASH_DUMP_DIR", dir, MAX_PATH) != 0) return fs::path(dir);
#if defined(NEBULA_HOST_RENDERER)
    wchar_t local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return fs::path(local) / "Nebula" / "crashes";
#endif
#else
    if (const char* dir = std::getenv("GALAXY_CRASH_DUMP_DIR"); dir != nullptr && dir[0] != 0) return fs::path(dir);
#endif
    return fs::path("crashes");
}

// The player sees the report; hidden test runs (GALAXY_HIDE_WINDOW) and the headless host keep it in its file.
bool crash_dialog_wanted() {
#if defined(NEBULA_HOST_RENDERER) || defined(NEBULA_HOST_SDL)
    const char* hidden = std::getenv("GALAXY_HIDE_WINDOW");
    return !(hidden != nullptr && hidden[0] != 0 && hidden[0] != '0');
#else
    return false;
#endif
}

FILE* open_for_writing(const fs::path& file) {
#if defined(_WIN32)
    return _wfopen(file.c_str(), L"wb");
#else
    return std::fopen(file.c_str(), "wb");
#endif
}

// gCrashFacts as a report: its code logged, its text written beside `dump` (or as nebula-<time>.txt in the crash
// folder) and, for the player, shown by a new instance of this executable (this process may be what failed; it ends
// right after). The SDL hosts show it in a message box here.
void report_crash_facts(const fs::path& dump) {
    const galaxy::crash::Report report = galaxy::crash::classify(gCrashFacts);
    char line[160];
    std::snprintf(line, sizeof(line), "%s %s", report.code, report.title);
    log_line("crash:", line);
    char technical[1600];
    std::snprintf(technical, sizeof(technical), "%s%s%s%s%s", gCrashText,
                  gCrashFacts.last_mod_read.empty() ? "" : "\nLast file read from a mod: ", gCrashModRead,
                  dump.empty() ? "" : "\nDump: ", dump.empty() ? "" : dump.string().c_str());
    static char text[8192];
    const std::size_t length = galaxy::crash::format_report(report, technical, text, sizeof(text));
    std::error_code error;
    fs::path file = dump;
    if (file.empty()) {
        const fs::path folder = crash_folder();
        fs::create_directories(folder, error);
        const auto stamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char name[64];
        std::strftime(name, sizeof(name), "nebula-%Y%m%d-%H%M%S.txt", std::localtime(&stamp));
        file = folder / name;
    } else {
        file.replace_extension(".txt");
    }
    if (FILE* out = open_for_writing(file)) {
        std::fwrite(text, 1, length, out);
        std::fclose(out);
        std::snprintf(line, sizeof(line), "report %s", file.string().c_str());
        log_line("crash:", line);
    }
    if (!crash_dialog_wanted()) return;
#if defined(_WIN32) && defined(NEBULA_HOST_RENDERER)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --crash-report \"" + file.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(exe, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
#elif defined(NEBULA_HOST_SDL)
    const std::string_view shown(text, length);
    const std::size_t technical_at = shown.find("\nTechnical details:");
    const std::string message(shown.substr(0, technical_at));
    galaxy::sdl::show_fatal(message.c_str());
#endif
}

[[noreturn]] void die(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    log_line("fatal:", text);
    std::fflush(stdout);
    gather_crash_facts(galaxy::crash::Facts::Kind::Startup, text);
    report_crash_facts({});
    std::fflush(stdout);
    std::_Exit(3);
}

uint32_t load_be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// The module's entry points.
struct Module {
    decltype(&nebula_game_attach) attach;
    decltype(&nebula_game_run) run;
    decltype(&nebula_game_disc_complete) disc_complete;
    decltype(&nebula_game_vi_retrace) vi_retrace;
    decltype(&nebula_game_fs_complete) fs_complete;
    decltype(&nebula_game_ai_interrupt) ai_interrupt;
    decltype(&nebula_game_audio_channel_owner) audio_channel_owner;
    decltype(&nebula_game_mii_prepare) mii_prepare;
    decltype(&nebula_game_mii_install) mii_install;
    decltype(&nebula_game_gx_event) gx_event;
    decltype(&nebula_game_gx_release) gx_release;
    decltype(&nebula_game_memory_regions) memory_regions;
    decltype(&nebula_game_wpad_connect) wpad_connect;
    decltype(&nebula_game_wpad_report) wpad_report;
    decltype(&nebula_game_wpad_shake) wpad_shake;
    void (*debug_dump)();  // optional: the module's thread report
};
Module gModule;

// The GX command ring's size (sdk/gx/gx_fifo.c), mapped twice in a row: a span past the first mapping's end
// continues in the second.
constexpr size_t kGxRingSize = 16u << 20;

// Progress counters for the host's log.
std::atomic<uint64_t> gGxBytes{0};
std::atomic<uint32_t> gDiscReads{0};
std::atomic<uint32_t> gFsOps{0};
std::atomic<uint32_t> gAiBlocks{0};
std::atomic<uint32_t> gDspFrames{0};
std::atomic<uint32_t> gMovieFrames{0};
std::atomic<uint64_t> gMovieDecodeNs{0};   // the THP decoder's time (svc_thp_video_decode)
std::atomic<uint32_t> gMovieSkipped{0};    // THP frames not decoded: a replacement video showed instead
std::atomic<uint32_t> gSceneBegins{0};

void* find_symbol(void* module, const char* name) {
#if defined(_WIN32)
    void* p = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    void* p = dlsym(module, name);
#endif
    if (p == nullptr) {
        die("the game module has no %s", name);
    }
    return p;
}

void load_module(const fs::path& path) {
#if defined(_WIN32)
    void* module = LoadLibraryW(path.wstring().c_str());
#else
    void* module = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (module == nullptr) {
        die("cannot load the game module %s", path.string().c_str());
    }
    gModule.attach = reinterpret_cast<decltype(gModule.attach)>(find_symbol(module, "nebula_game_attach"));
    gModule.run = reinterpret_cast<decltype(gModule.run)>(find_symbol(module, "nebula_game_run"));
    gModule.disc_complete = reinterpret_cast<decltype(gModule.disc_complete)>(find_symbol(module, "nebula_game_disc_complete"));
    gModule.vi_retrace = reinterpret_cast<decltype(gModule.vi_retrace)>(find_symbol(module, "nebula_game_vi_retrace"));
    gModule.fs_complete = reinterpret_cast<decltype(gModule.fs_complete)>(find_symbol(module, "nebula_game_fs_complete"));
    gModule.ai_interrupt = reinterpret_cast<decltype(gModule.ai_interrupt)>(find_symbol(module, "nebula_game_ai_interrupt"));
    gModule.audio_channel_owner = reinterpret_cast<decltype(gModule.audio_channel_owner)>(
        find_symbol(module, "nebula_game_audio_channel_owner"));
    gModule.mii_prepare =
        reinterpret_cast<decltype(gModule.mii_prepare)>(find_symbol(module, "nebula_game_mii_prepare"));
    gModule.mii_install =
        reinterpret_cast<decltype(gModule.mii_install)>(find_symbol(module, "nebula_game_mii_install"));
    gModule.gx_event = reinterpret_cast<decltype(gModule.gx_event)>(find_symbol(module, "nebula_game_gx_event"));
    gModule.gx_release = reinterpret_cast<decltype(gModule.gx_release)>(find_symbol(module, "nebula_game_gx_release"));
    gModule.memory_regions = reinterpret_cast<decltype(gModule.memory_regions)>(find_symbol(module, "nebula_game_memory_regions"));
    gModule.wpad_connect = reinterpret_cast<decltype(gModule.wpad_connect)>(find_symbol(module, "nebula_game_wpad_connect"));
    gModule.wpad_report = reinterpret_cast<decltype(gModule.wpad_report)>(find_symbol(module, "nebula_game_wpad_report"));
    gModule.wpad_shake = reinterpret_cast<decltype(gModule.wpad_shake)>(find_symbol(module, "nebula_game_wpad_shake"));
#if defined(_WIN32)
    gModule.debug_dump = reinterpret_cast<void (*)()>(GetProcAddress(static_cast<HMODULE>(module), "nebula_game_debug_dump"));
#else
    gModule.debug_dump = reinterpret_cast<void (*)()>(dlsym(module, "nebula_game_debug_dump"));
#endif
}

#if defined(NEBULA_HOST_RENDERER)
void svc_gpu_coherent(void*, uint32_t physical, uint32_t size);

// Host writes into game memory (disc DMA): the renderer drops what it holds of it. `dst` is in one of the game's
// regions.
void mark_written(const void* dst, uint32_t size) {
    NebulaGameMemoryRegion regions[8];
    const int count = gModule.memory_regions(regions, 8);
    const auto* p = static_cast<const uint8_t*>(dst);
    for (int i = 0; i < count; i++) {
        if (p >= regions[i].host && p < regions[i].host + regions[i].size) {
            galaxy::gx::notify_guest_memory_write(regions[i].physical + (uint32_t)(p - regions[i].host), size);
            return;
        }
    }
}
#endif

// The game partition: Setup's game.pak, the decrypted partition data at its disc offsets (boot.bin at 0, main.dol
// and the file table where boot.bin says), mapped read-only.
struct Pak {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
    const uint8_t* fst = nullptr;
    size_t fst_size = 0;
    const uint8_t* dol = nullptr;
    size_t dol_size = 0;
};
Pak gPak;
// The mods this session (galaxy::mods): the disc's file table with their files, main.dol's patches, a save folder.
std::unique_ptr<galaxy::mods::ActiveMods> gMods;

void open_pak(const fs::path& path) {
#if defined(_WIN32)
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size)) {
        die("cannot open %s", path.string().c_str());
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const void* view = mapping != nullptr ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (view == nullptr) {
        die("cannot map %s", path.string().c_str());
    }
    gPak.data = static_cast<const uint8_t*>(view);
    gPak.size = static_cast<uint64_t>(size.QuadPart);
#else
    const int fd = open(path.c_str(), O_RDONLY);
    struct stat st {};
    if (fd < 0 || fstat(fd, &st) != 0) {
        die("cannot open %s", path.c_str());
    }
    void* view = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (view == MAP_FAILED) {
        die("cannot map %s", path.c_str());
    }
    gPak.data = static_cast<const uint8_t*>(view);
    gPak.size = (uint64_t)st.st_size;
#endif
    if (gPak.size < 0x440) {
        die("%s is too short for a game partition", path.string().c_str());
    }
    // Wii partition offsets are words: boot.bin holds them divided by 4.
    const uint64_t dol = (uint64_t)load_be32(gPak.data + 0x420) << 2;
    const uint64_t fst = (uint64_t)load_be32(gPak.data + 0x424) << 2;
    const uint64_t fst_size = (uint64_t)load_be32(gPak.data + 0x428) << 2;
    if (dol + 0x100 > gPak.size || fst + fst_size > gPak.size) {
        die("%s: boot.bin points past the partition", path.string().c_str());
    }
    gPak.fst = gPak.data + fst;
    gPak.fst_size = (size_t)fst_size;
    gPak.dol = gPak.data + dol;
    uint64_t end = 0x100;
    for (int s = 0; s < 18; s++) {
        const uint64_t section_end = (uint64_t)load_be32(gPak.dol + s * 4) + load_be32(gPak.dol + 0x90 + s * 4);
        if (load_be32(gPak.dol + 0x90 + s * 4) != 0 && section_end > end) {
            end = section_end;
        }
    }
    if (dol + end > gPak.size) {
        die("%s: main.dol runs past the partition", path.string().c_str());
    }
    gPak.dol_size = (size_t)end;
}

// The calling thread's name, as profilers, debuggers and the CPU report show it.
void name_thread(const wchar_t* name) {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), name);
#else
    char narrow[16];
    std::snprintf(narrow, sizeof(narrow), "%ls", name);
#if defined(__APPLE__)
    pthread_setname_np(narrow);
#else
    pthread_setname_np(pthread_self(), narrow);
#endif
#endif
}

// A worker thread taking jobs in order.
class Worker {
public:
    explicit Worker(const wchar_t* name) : name_(name) {}

    template <typename F>
    void post(F&& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.emplace_back(std::forward<F>(job));
        }
        ready_.notify_one();
    }

    void start() {
        thread_ = std::thread([this] {
            name_thread(name_);
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this] { return !jobs_.empty(); });
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                job();
            }
        });
        thread_.detach();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
    const wchar_t* name_;
};

Worker gDisc(L"Nebula Host Disc");
Worker gFs(L"Nebula Host NAND");

// NAND: the console's absolute paths under the host's NAND root, in the Wii's layout.
fs::path gNandRoot;

struct OpenFile {
    std::FILE* file = nullptr;
    fs::path path;
    fs::path temp;  // a safe open's copy, moved over `path` at the safe close
};
std::vector<OpenFile> gFiles(16);

constexpr int32_t kNandOk = 0;
constexpr int32_t kNandAccess = -1;
constexpr int32_t kNandExists = -6;
constexpr int32_t kNandInvalid = -8;
constexpr int32_t kNandMaxFd = -10;
constexpr int32_t kNandNoExists = -12;
constexpr int32_t kNandNotEmpty = -13;

// The game's save folder on the NAND (title/00010000/<the game's four-character code in hex>/data).
std::string gSaveDataPath;

bool nand_path(const char* path, fs::path* out) {
    if (path == nullptr || path[0] != '/') {
        return false;
    }
    fs::path result = gNandRoot;
    // A Riivolution mod's savegame: the game's saves in its folder.
    if (gMods != nullptr && !gMods->save_folder.empty() && !gSaveDataPath.empty() &&
        std::strncmp(path, gSaveDataPath.c_str(), gSaveDataPath.size()) == 0 &&
        (path[gSaveDataPath.size()] == '/' || path[gSaveDataPath.size()] == '\0')) {
        result = gMods->save_folder;
        path += gSaveDataPath.size();
        if (*path == '\0') {
            *out = result;
            return true;
        }
    }
    for (const fs::path& part : fs::path(path + 1)) {
        if (part == ".." || part == ".") {
            return false;
        }
        result /= part;
    }
    *out = result;
    return true;
}

// A rename, else a copy and a removal: a mod's save folder can be on another volume than the NAND's /tmp.
void move_path(const fs::path& from, const fs::path& to, std::error_code& error) {
    fs::rename(from, to, error);
    if (!error) {
        return;
    }
    std::error_code copy_error;
    if (fs::is_directory(from, copy_error)) {
        fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_error);
    } else {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, copy_error);
    }
    if (!copy_error) {
        fs::remove_all(from, copy_error);
        error.clear();
    }
}

std::FILE* open_file(const fs::path& path, uint32_t mode) {
    // NAND_ACCESS_READ 1, WRITE 2, RW 3: the file must exist.
#if defined(_WIN32)
    return _wfopen(path.wstring().c_str(), mode == 1 ? L"rb" : L"r+b");
#else
    return std::fopen(path.c_str(), mode == 1 ? "rb" : "r+b");
#endif
}

// Rollback (netplay's rollback, GALAXY_SNAPSHOT_TEST): the NAND in memory, kept and restored with the game at each
// boundary (svc_det_boundary), so a NAND operation of frames a restore undid is undone too; written to the host's files
// once no rollback can undo it (nand_flush). Files are shared between the kept states until one is written. The NAND
// thread uses it only while the game waits for an operation, so never during a boundary.
bool deterministic();
struct MemNand {
    struct Entry {
        bool dir = false;
        std::shared_ptr<std::vector<uint8_t>> data;  // a file's bytes
    };
    struct Open {
        bool used = false;
        std::string key;
        std::shared_ptr<std::vector<uint8_t>> data;  // a safe open's copy, else null (the file's own)
        uint32_t pos = 0;
        uint32_t mode = 0;
    };
    std::map<std::string, std::optional<Entry>> entries;  // by host path; nullopt: not there (removed, or never was)
    std::vector<Open> files = std::vector<Open>(16);
};
MemNand gMemNand;
std::map<uint32_t, MemNand> gMemNandSaved;  // by boundary
std::map<std::string, std::optional<MemNand::Entry>> gMemNandOnDisk;  // as written to the host's files

bool nand_in_memory() {
    static const bool in_memory = [] {
        if (!deterministic()) return false;
        if (std::getenv("GALAXY_SNAPSHOT_TEST") != nullptr) return true;
        return galaxy::net::session().active() && galaxy::net::session().settings().rollback != 0;
    }();
    return in_memory;
}

// The entry at a host path, from the host's files the first time.
const std::optional<MemNand::Entry>& mem_lookup(const fs::path& path) {
    const std::string key = path.generic_string();
    if (const auto it = gMemNand.entries.find(key); it != gMemNand.entries.end()) {
        return it->second;
    }
    std::optional<MemNand::Entry> entry;
    std::error_code error;
    if (fs::is_directory(path, error)) {
        entry = MemNand::Entry{true, nullptr};
    } else if (fs::is_regular_file(path, error)) {
        auto data = std::make_shared<std::vector<uint8_t>>();
        if (std::ifstream in(path, std::ios::binary); in) {
            data->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        entry = MemNand::Entry{false, std::move(data)};
    }
    gMemNandOnDisk.emplace(key, entry);
    return gMemNand.entries.emplace(key, std::move(entry)).first->second;
}

// The keys under a directory (from the host's files too), deepest last.
std::vector<std::string> mem_descendants(const fs::path& dir) {
    std::error_code error;
    if (fs::is_directory(dir, error)) {
        for (fs::recursive_directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
            (void)mem_lookup(it->path());
        }
    }
    const std::string prefix = dir.generic_string() + "/";
    std::vector<std::string> keys;
    for (auto it = gMemNand.entries.lower_bound(prefix); it != gMemNand.entries.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        if (it->second.has_value()) keys.push_back(it->first);
    }
    return keys;
}

void mem_remove(const fs::path& path) {
    for (const std::string& key : mem_descendants(path)) gMemNand.entries[key] = std::nullopt;
    (void)mem_lookup(path);
    gMemNand.entries[path.generic_string()] = std::nullopt;
}

// A file's bytes to write: its own unless a kept state shares them.
std::vector<uint8_t>& mem_writable(std::shared_ptr<std::vector<uint8_t>>& data) {
    if (data.use_count() > 1) data = std::make_shared<std::vector<uint8_t>>(*data);
    return *data;
}

int32_t mem_fs_run(const NebulaFsRequest& r) {
    fs::path path;
    auto open_at = [](int32_t fd) -> MemNand::Open* {
        return fd >= 0 && fd < (int32_t)gMemNand.files.size() && gMemNand.files[fd].used ? &gMemNand.files[fd] : nullptr;
    };
    // An open file's bytes: a safe open's copy, else the file's.
    auto bytes_of = [](MemNand::Open& f) -> std::shared_ptr<std::vector<uint8_t>>& {
        if (f.data) return f.data;
        return gMemNand.entries[f.key]->data;
    };
    switch (r.op) {
    case NEBULA_FS_OPEN:
    case NEBULA_FS_SAFE_OPEN: {
        if (!nand_path(r.path, &path)) return kNandInvalid;
        const auto& entry = mem_lookup(path);
        if (!entry.has_value() || entry->dir) return kNandNoExists;
        int32_t fd = 0;
        while (fd < (int32_t)gMemNand.files.size() && gMemNand.files[fd].used) fd++;
        if (fd == (int32_t)gMemNand.files.size()) return kNandMaxFd;
        MemNand::Open& f = gMemNand.files[fd];
        f = {};
        f.used = true;
        f.key = path.generic_string();
        f.mode = r.mode;
        if (r.op == NEBULA_FS_SAFE_OPEN && r.mode != 1) f.data = entry->data;  // written apart (copy on write)
        return fd;
    }
    case NEBULA_FS_CLOSE:
    case NEBULA_FS_SAFE_CLOSE: {
        MemNand::Open* f = open_at(r.fd);
        if (f == nullptr) return kNandInvalid;
        if (f->data) gMemNand.entries[f->key] = MemNand::Entry{false, f->data};
        *f = {};
        return kNandOk;
    }
    case NEBULA_FS_READ: {
        MemNand::Open* f = open_at(r.fd);
        if (f == nullptr) return kNandInvalid;
        const std::vector<uint8_t>& data = *bytes_of(*f);
        const uint32_t count = f->pos >= data.size() ? 0u : (std::min<uint32_t>)(r.size, (uint32_t)data.size() - f->pos);
        if (count != 0) std::memcpy(r.buffer, data.data() + f->pos, count);
        f->pos += count;
        return (int32_t)count;
    }
    case NEBULA_FS_WRITE: {
        MemNand::Open* f = open_at(r.fd);
        if (f == nullptr || f->mode == 1) return kNandInvalid;
        std::vector<uint8_t>& data = mem_writable(bytes_of(*f));
        if (data.size() < (size_t)f->pos + r.size) data.resize((size_t)f->pos + r.size);
        if (r.size != 0) std::memcpy(data.data() + f->pos, r.buffer, r.size);
        f->pos += r.size;
        return (int32_t)r.size;
    }
    case NEBULA_FS_SEEK: {
        MemNand::Open* f = open_at(r.fd);
        if (f == nullptr) return kNandInvalid;
        const int64_t base = r.whence == 0 ? 0 : r.whence == 1 ? (int64_t)f->pos : (int64_t)bytes_of(*f)->size();
        const int64_t at = base + r.offset;
        if (at < 0) return kNandInvalid;
        f->pos = (uint32_t)at;
        return (int32_t)at;
    }
    case NEBULA_FS_LENGTH: {
        MemNand::Open* f = open_at(r.fd);
        return f == nullptr ? kNandInvalid : (int32_t)bytes_of(*f)->size();
    }
    case NEBULA_FS_CREATE:
    case NEBULA_FS_CREATE_DIR: {
        if (!nand_path(r.path, &path)) return kNandInvalid;
        if (mem_lookup(path).has_value()) return kNandExists;
        const auto& parent = mem_lookup(path.parent_path());
        if (!parent.has_value() || !parent->dir) return kNandNoExists;
        gMemNand.entries[path.generic_string()] = r.op == NEBULA_FS_CREATE_DIR
            ? MemNand::Entry{true, nullptr}
            : MemNand::Entry{false, std::make_shared<std::vector<uint8_t>>()};
        return kNandOk;
    }
    case NEBULA_FS_DELETE: {
        if (!nand_path(r.path, &path)) return kNandInvalid;
        if (!mem_lookup(path).has_value()) return kNandNoExists;
        mem_remove(path);
        return kNandOk;
    }
    case NEBULA_FS_RENAME: {
        fs::path to;
        if (!nand_path(r.path, &path) || !nand_path(r.path2, &to)) return kNandInvalid;
        const std::optional<MemNand::Entry> from = mem_lookup(path);
        if (!from.has_value()) return kNandNoExists;
        const auto& target = mem_lookup(to);
        if (target.has_value() && target->dir && !mem_descendants(to).empty()) return kNandNotEmpty;
        // What `to` becomes: `path` and everything under it, moved.
        std::vector<std::pair<std::string, std::optional<MemNand::Entry>>> moved;
        moved.emplace_back(to.generic_string(), from);
        const std::string old_prefix = path.generic_string();
        for (const std::string& key : mem_descendants(path)) {
            moved.emplace_back(to.generic_string() + key.substr(old_prefix.size()), gMemNand.entries[key]);
        }
        mem_remove(path);
        if (target.has_value()) mem_remove(to);
        for (auto& [key, entry] : moved) gMemNand.entries[key] = std::move(entry);
        return kNandOk;
    }
    }
    return kNandInvalid;
}

// The host's files brought to a kept state: what was removed, then what is new or changed (parents first).
void nand_flush(const std::map<std::string, std::optional<MemNand::Entry>>& state) {
    auto same = [](const std::optional<MemNand::Entry>& a, const std::optional<MemNand::Entry>& b) {
        if (a.has_value() != b.has_value()) return false;
        return !a.has_value() || (a->dir == b->dir && a->data == b->data);
    };
    std::error_code error;
    for (const auto& [key, entry] : state) {
        const auto on_disk = gMemNandOnDisk.find(key);
        if (!entry.has_value() && (on_disk == gMemNandOnDisk.end() || on_disk->second.has_value())) {
            fs::remove_all(fs::path(key), error);
        }
    }
    for (const auto& [key, entry] : state) {
        const auto on_disk = gMemNandOnDisk.find(key);
        if (!entry.has_value() || (on_disk != gMemNandOnDisk.end() && same(on_disk->second, entry))) continue;
        const fs::path path(key);
        if (entry->dir) {
            fs::create_directories(path, error);
        } else {
            fs::create_directories(path.parent_path(), error);
            // As a save does: a new file beside it, moved over it.
            fs::path temp = path;
            temp += L".nebula-flush";
            if (std::ofstream out(temp, std::ios::binary | std::ios::trunc); out) {
                out.write(reinterpret_cast<const char*>(entry->data->data()), (std::streamsize)entry->data->size());
            }
            move_path(temp, path, error);
            if (error) {
                std::fprintf(stderr, "[nand] could not write %s\n", key.c_str());
            }
        }
    }
    for (const auto& [key, entry] : state) gMemNandOnDisk[key] = entry;
}

int32_t disk_fs_run(const NebulaFsRequest& r);

int32_t fs_run(const NebulaFsRequest& r) {
    return nand_in_memory() ? mem_fs_run(r) : disk_fs_run(r);
}

int32_t disk_fs_run(const NebulaFsRequest& r) {
    std::error_code error;
    fs::path path;
    auto file_at = [](int32_t fd) -> OpenFile* {
        return fd >= 0 && fd < (int32_t)gFiles.size() && gFiles[fd].file != nullptr ? &gFiles[fd] : nullptr;
    };
    switch (r.op) {
    case NEBULA_FS_OPEN:
    case NEBULA_FS_SAFE_OPEN: {
        if (!nand_path(r.path, &path)) {
            return kNandInvalid;
        }
        if (!fs::is_regular_file(path, error)) {
            return kNandNoExists;
        }
        int32_t fd = 0;
        while (fd < (int32_t)gFiles.size() && gFiles[fd].file != nullptr) {
            fd++;
        }
        if (fd == (int32_t)gFiles.size()) {
            return kNandMaxFd;
        }
        OpenFile entry;
        entry.path = path;
        if (r.op == NEBULA_FS_SAFE_OPEN && r.mode != 1) {
            entry.temp = path;
            entry.temp += L".nebula-safe";
            fs::copy_file(path, entry.temp, fs::copy_options::overwrite_existing, error);
            if (error) {
                return kNandAccess;
            }
            entry.file = open_file(entry.temp, r.mode);
        } else {
            entry.file = open_file(path, r.mode);
        }
        if (entry.file == nullptr) {
            return kNandAccess;
        }
        gFiles[fd] = std::move(entry);
        return fd;
    }
    case NEBULA_FS_CLOSE:
    case NEBULA_FS_SAFE_CLOSE: {
        OpenFile* f = file_at(r.fd);
        if (f == nullptr) {
            return kNandInvalid;
        }
        std::fflush(f->file);
        std::fclose(f->file);
        f->file = nullptr;
        if (!f->temp.empty()) {
            move_path(f->temp, f->path, error);
            f->temp.clear();
            if (error) {
                return kNandAccess;
            }
        }
        return kNandOk;
    }
    case NEBULA_FS_READ: {
        OpenFile* f = file_at(r.fd);
        return f == nullptr ? kNandInvalid : (int32_t)std::fread(r.buffer, 1, r.size, f->file);
    }
    case NEBULA_FS_WRITE: {
        OpenFile* f = file_at(r.fd);
        return f == nullptr ? kNandInvalid : (int32_t)std::fwrite(r.buffer, 1, r.size, f->file);
    }
    case NEBULA_FS_SEEK: {
        OpenFile* f = file_at(r.fd);
        if (f == nullptr || std::fseek(f->file, r.offset, r.whence == 0 ? SEEK_SET : r.whence == 1 ? SEEK_CUR : SEEK_END) != 0) {
            return kNandInvalid;
        }
        return (int32_t)std::ftell(f->file);
    }
    case NEBULA_FS_LENGTH: {
        OpenFile* f = file_at(r.fd);
        if (f == nullptr) {
            return kNandInvalid;
        }
        const long at = std::ftell(f->file);
        std::fseek(f->file, 0, SEEK_END);
        const long length = std::ftell(f->file);
        std::fseek(f->file, at, SEEK_SET);
        return (int32_t)length;
    }
    case NEBULA_FS_CREATE:
    case NEBULA_FS_CREATE_DIR: {
        if (!nand_path(r.path, &path)) {
            return kNandInvalid;
        }
        if (fs::exists(path, error)) {
            return kNandExists;
        }
        if (!fs::is_directory(path.parent_path(), error)) {
            return kNandNoExists;
        }
        if (r.op == NEBULA_FS_CREATE_DIR) {
            return fs::create_directory(path, error) ? kNandOk : kNandAccess;
        }
#if defined(_WIN32)
        std::FILE* f = _wfopen(path.wstring().c_str(), L"wb");
#else
        std::FILE* f = std::fopen(path.c_str(), "wb");
#endif
        if (f == nullptr) {
            return kNandAccess;
        }
        std::fclose(f);
        return kNandOk;
    }
    case NEBULA_FS_DELETE: {
        if (!nand_path(r.path, &path)) {
            return kNandInvalid;
        }
        if (!fs::exists(path, error)) {
            return kNandNoExists;
        }
        fs::remove_all(path, error);
        return error ? kNandAccess : kNandOk;
    }
    case NEBULA_FS_RENAME: {
        fs::path to;
        if (!nand_path(r.path, &path) || !nand_path(r.path2, &to)) {
            return kNandInvalid;
        }
        if (!fs::exists(path, error)) {
            return kNandNoExists;
        }
        if (fs::is_directory(to, error) && !fs::is_empty(to, error)) {
            return kNandNotEmpty;
        }
        move_path(path, to, error);
        return error ? kNandAccess : kNandOk;
    }
    }
    return kNandInvalid;
}

// Audio interface pacing: the AI takes a latched block at each block's end, as the console's DMA engine does when
// its count runs out, and raises the AI interrupt for the game to latch the next.
std::mutex gAiLock;
std::condition_variable gAiWake;
uint32_t gAiLatchedSize = 0;
uint32_t gAiLatchedStart = 0;
uint32_t gAiRate = 32000;
bool gAiRunning = false;

#if defined(NEBULA_HOST_RENDERER)
const std::byte* guest_bytes(uint32_t address, uint32_t size);
extern galaxy::host::NativeGameAudio* gAudio;
#endif

// The app out of sight (SDL hosts: Android's background): the display and audio clocks stop, so the game waits at
// its next retrace; they restart from its return, with nothing to catch up.
std::mutex gSuspendLock;
std::condition_variable gSuspendWake;
bool gSuspended = false;  // guarded by gSuspendLock

// True when it waited.
bool wait_while_suspended() {
    std::unique_lock<std::mutex> lock(gSuspendLock);
    if (!gSuspended) {
        return false;
    }
    gSuspendWake.wait(lock, [] { return !gSuspended; });
    return true;
}

// The audio interface's clock: a block taken every block's length (4 bytes a stereo sample at the AI rate), on
// fixed deadlines, as the console's DMA engine runs. With an output device the period follows the device's own clock:
// at most 1 % shorter while fewer blocks than the sink's restart level wait there unplayed, longer while more, so the
// queue holds about that level and the two clocks never drift apart. It never waits for the device: an audio
// interface that stalls leaves the game's sounds playing forever until its sound pools run out. A silent or failed
// device leaves the clock as it is.
void ai_thread() {
    name_thread(L"Nebula Host AI");
    Clock::time_point next{};
    bool scheduled = false;
    for (;;) {
        if (wait_while_suspended()) {
            scheduled = false;
        }
        uint32_t size = 0;
        uint32_t start = 0;
        uint32_t rate = 32000;
        {
            std::unique_lock<std::mutex> lock(gAiLock);
            // A stopped interface (or none latched yet) restarts the schedule from the block that resumes it.
            if (!(gAiRunning && gAiLatchedSize != 0)) {
                gAiWake.wait(lock, [] { return gAiRunning && gAiLatchedSize != 0; });
                scheduled = false;
            }
            size = gAiLatchedSize;
            start = gAiLatchedStart;
            rate = gAiRate != 0 ? gAiRate : 32000;
        }
        if (!scheduled) {
            next = Clock::now();
            scheduled = true;
        }
        double period_ns = static_cast<double>((uint64_t)size / 4 * 1000000000ull) / rate;
#if defined(NEBULA_HOST_RENDERER)
        bool played = false;
        if (const std::byte* samples = guest_bytes(start, size)) {
            played = gAudio->play(std::span<const std::byte>(samples, size), rate);
        }
        if (played && gAudio->output_available()) {
            const int error = static_cast<int>(gAudio->queued_blocks()) - static_cast<int>(gAudio->target_blocks());
            period_ns *= 1.0 + std::clamp(error * 0.0025, -0.01, 0.01);
        }
#else
        (void)start;
#endif
        gAiBlocks.fetch_add(1, std::memory_order_relaxed);
        // GALAXY_TEST_AI_STALL_AFTER=<blocks>: no interrupt after that many blocks. The game's sounds then never end and
        // its sound pools fill; it must refuse new sounds, not crash.
        static const uint64_t stall_after = [] {
            const char* value = std::getenv("GALAXY_TEST_AI_STALL_AFTER");
            return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
        }();
        if (stall_after == 0 || gAiBlocks.load(std::memory_order_relaxed) < stall_after) {
            gModule.ai_interrupt();
        }
        next += std::chrono::nanoseconds(static_cast<int64_t>(period_ns));
        // A stall past 100 ms (a debugger, a hitch) starts the schedule again rather than catching up in a burst.
        const auto now = Clock::now();
        if (next + std::chrono::milliseconds(100) < now) {
            next = now;
        }
        sleep_until_precise(next);
    }
}

// Rollback (deterministic mode; svc_det_boundary): the retraces granted so far, a restore's frame (the frame clock goes
// on from it), and the frame up to which it grants without pacing (the frames a restore undid, run again).
std::atomic<uint32_t> gGranted{0};
std::atomic<bool> gRewindPending{false};
std::atomic<uint32_t> gRewindFrame{0};
std::atomic<uint32_t> gCatchUpTo{0};

// Rollback netplay (net_lockstep.h, settings.rollback): the other PC's input each frame ran with, predicted (its last
// known) or confirmed. A late input that differs from the prediction sends the game back to the boundary before its
// frame (svc_det_boundary). The game runs at most kRollbackAhead frames past the confirmed input.
struct UsedRemote {
    galaxy::input::WiimoteInputSnapshot snapshot{};
    bool confirmed = false;
};
std::mutex gRollbackLock;
std::map<uint32_t, UsedRemote> gUsedRemote;
constexpr uint32_t kRollbackAhead = 15;
// Every 600 frames: how rollback went (frames run again, time waiting for the game and for the other PC).
struct RollbackStats {
    uint32_t frames = 0;
    uint32_t rollbacks = 0;
    uint32_t rerun = 0;
    double wait_game_ms = 0;
    double wait_ahead_ms = 0;
};
RollbackStats gRollbackStats;

// Deterministic mode's frame clock (vi_thread) needs these, defined below.
bool deterministic();
void send_wpad_reports(uint32_t frame);
extern std::atomic<uint32_t> gRetraces;
extern std::atomic<int> gWpadStarted;
extern std::atomic<uint32_t> gWpadStartRetrace;
extern std::atomic<int32_t> gStarPointerMode;
extern std::atomic<uint32_t> gTiltSection;
extern std::atomic<int> gWpadDpd;
extern const galaxy::input::WiimoteInputSnapshot* gNetInputs;

// The display's retrace at 60 Hz: the console's NTSC 59.94 would repeat a frame every 17 s on a 60 Hz display. A late
// wake-up skips the fields it missed, as a display does, rather than delivering them back to back. On Android the
// retrace follows the display's own vsync (every second one at 120 Hz), so frames never drift against it and each
// stays on screen for the same time; the host's clock takes over where the display gives none.
void vi_thread() {
    name_thread(L"Nebula Host VI");
    const auto period = std::chrono::nanoseconds(1000000000ull / 60);
    auto next = Clock::now() + period;
#if defined(NEBULA_HOST_SDL)
    int64_t vsync_ns = 0;
    uint32_t on_vsync = 0;
    uint32_t retraces = 0;
    int64_t work_sum = 0;
    int64_t work_max = 0;
#endif
    for (;;) {
#if defined(NEBULA_HOST_SDL)
        const bool vsync = galaxy::sdl::wait_display_retrace(period.count(), vsync_ns);
        if (!vsync) {
            sleep_until_precise(next);
        }
#else
        // Catching up after a rollback: the undone frames run again at once, off the schedule (it goes on from the
        // first frame not run before).
        const bool catching_up =
            deterministic() && gGranted.load(std::memory_order_acquire) < gCatchUpTo.load(std::memory_order_acquire);
        if (!catching_up) {
            sleep_until_precise(next);
        }
#endif
        if (wait_while_suspended()) {
            next = Clock::now() + period;
            continue;
        }
        // The last frame's work (the game and render threads' CPU time), to the performance hints (Android).
        [[maybe_unused]] const int64_t work = galaxy::host::perf_hint_retrace();
        if (deterministic()) {
            // Retrace N once the game has taken N - 1 (vi_frame counts them), with frame N's remote reports. After a
            // restore the game is back at an earlier boundary: the clock goes on from there.
            uint32_t granted = gGranted.load(std::memory_order_acquire);
            const auto wait_start = Clock::now();
            for (;;) {
                if (gRewindPending.exchange(false, std::memory_order_acq_rel)) {
                    granted = gRewindFrame.load(std::memory_order_acquire);
                    gGranted.store(granted, std::memory_order_release);
                }
                if (gRetraces.load(std::memory_order_acquire) >= granted) break;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            // Online: this PC's player's input for frame N + delay goes out, then frame N waits for both players'.
            galaxy::input::WiimoteInputSnapshot net_inputs[2];
            gNetInputs = nullptr;
            if (galaxy::net::session().active() && galaxy::net::session().settings().rollback != 0) {
                // Rollback: this PC's input goes out once per frame (a frame run again keeps it); the other's is
                // the confirmed one, else predicted.
                galaxy::net::Session& net = galaxy::net::session();
                const uint32_t frame = granted + 1;
                static uint32_t last_sent = 0;
                if (frame + net.settings().delay > last_sent) {
                    last_sent = frame + net.settings().delay;
                    galaxy::net::FrameInput local;
                    local.snapshot = galaxy::host::native_game_wiimote_snapshot(
                        last_sent, gWpadDpd.load(std::memory_order_relaxed) != 0,
                        gStarPointerMode.load(std::memory_order_relaxed),
                        static_cast<uint8_t>(gTiltSection.load(std::memory_order_relaxed)));
                    net.send_input(last_sent, local);
                }
                gRollbackStats.wait_game_ms += std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
                const auto ahead_start = Clock::now();
                while (frame > net.remote_confirmed() + kRollbackAhead) {
                    if (net.ended()) {
                        if (nand_in_memory()) nand_flush(gMemNand.entries);
                        std::fprintf(stderr, "[netplay] the session ended: closing\n");
                        std::fflush(stderr);
                        std::_Exit(0);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                gRollbackStats.wait_ahead_ms += std::chrono::duration<double, std::milli>(Clock::now() - ahead_start).count();
                if (frame < gCatchUpTo.load(std::memory_order_acquire)) ++gRollbackStats.rerun;
                if (++gRollbackStats.frames == 600) {
                    const RollbackStats& r = gRollbackStats;
                    std::fprintf(stderr, "[netplay] last 600 frames: %u rollbacks, %u frames run again, %.0f ms waiting for the game, "
                                 "%.0f ms for the other PC\n", r.rollbacks, r.rerun, r.wait_game_ms, r.wait_ahead_ms);
                    gRollbackStats = {};
                }
                galaxy::net::FrameInput remote;
                const bool confirmed = net.remote_input(frame, remote);
                if (!confirmed) {
                    (void)net.remote_input(net.remote_confirmed(), remote);
                }
                {
                    std::lock_guard<std::mutex> lock(gRollbackLock);
                    gUsedRemote[frame] = {remote.snapshot, confirmed};
                }
                const unsigned local_channel = net.local_channel();
                net_inputs[local_channel] = net.local_input(frame).snapshot;
                net_inputs[1 - local_channel] = remote.snapshot;
                gNetInputs = net_inputs;
            } else if (galaxy::net::session().active()) {
                galaxy::net::Session& net = galaxy::net::session();
                const uint32_t frame = granted + 1;
                galaxy::net::FrameInput local;
                local.snapshot = galaxy::host::native_game_wiimote_snapshot(
                    frame + net.settings().delay, gWpadDpd.load(std::memory_order_relaxed) != 0,
                    gStarPointerMode.load(std::memory_order_relaxed),
                    static_cast<uint8_t>(gTiltSection.load(std::memory_order_relaxed)));
                net.send_input(frame + net.settings().delay, local);
                galaxy::net::FrameInput both[2];
                if (!net.frame_inputs(frame, both)) {
                    std::fprintf(stderr, "[netplay] the session ended: closing\n");
                    std::fflush(stderr);
                    std::_Exit(0);
                }
                net_inputs[0] = both[0].snapshot;
                net_inputs[1] = both[1].snapshot;
                gNetInputs = net_inputs;
            }
            // Once WPAD started in a retrace before this one: the game took that retrace, so it shows here whatever
            // the threads' timing.
            if (gWpadStarted.load(std::memory_order_acquire) &&
                gWpadStartRetrace.load(std::memory_order_relaxed) < granted) {
                send_wpad_reports(granted + 1);
            }
            gNetInputs = nullptr;
            gGranted.store(granted + 1, std::memory_order_release);
        }
        gModule.vi_retrace();
#if !defined(NEBULA_HOST_SDL)
        if (catching_up) continue;
#endif
        next += period;
        const auto now = Clock::now();
        if (now >= next) {
            next = now + period;
        }
#if defined(NEBULA_HOST_SDL)
        if (vsync) {
            next = now + period;  // the host's clock continues from the display's
            ++on_vsync;
        }
        // Every 10 s: how the retraces were paced, and the frame work (the busier of the game and render threads).
        work_sum += work;
        work_max = std::max(work_max, work);
        if (++retraces == 600u) {
            std::fprintf(stderr, "[pacing] %u retraces, %u on the display's vsync (%.1f Hz); frame work %.2f ms average, "
                         "%.2f ms longest\n", retraces, on_vsync, galaxy::sdl::display_refresh_hz(),
                         static_cast<double>(work_sum) / retraces / 1e6, static_cast<double>(work_max) / 1e6);
            retraces = on_vsync = 0u;
            work_sum = work_max = 0;
        }
#endif
    }
}

void svc_log(void*, const char* text) {
    log_line("game:", text);
}

#if defined(_WIN32)
// GALAXY_TRACE_THREAD_CPU=1: each thread's CPU time and context switches over the last second, by name (or,
// unnamed, the module and offset it started at), busiest first; threads under 0.5 ms/s are counted in the total only.
// The report's own cost (a snapshot of every thread in the system) is left out of the reporting thread's figure.
void report_thread_cpu(double tsc_hz) {
    static std::unordered_map<DWORD, uint64_t> last;
    static std::unordered_map<DWORD, uint32_t> last_switches;
    static uint64_t report_cycles = 0;
    ULONG64 report_start = 0;
    QueryThreadCycleTime(GetCurrentThread(), &report_start);
    // The process's threads and their context switches: NtQuerySystemInformation(SystemProcessInformation). On x64
    // each process record is 0x100 bytes followed by 0x50-byte thread records (thread id at +0x30, switches at +0x40).
    using SystemQueryFn = LONG(WINAPI*)(int, void*, ULONG, ULONG*);
    static const auto system_query = reinterpret_cast<SystemQueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")));
    static std::vector<uint8_t> buffer(4u << 20);
    struct ThreadSample {
        DWORD id;
        uint32_t switches;
    };
    std::vector<ThreadSample> threads;
    ULONG needed = 0;
    LONG status = system_query != nullptr ? system_query(5, buffer.data(), (ULONG)buffer.size(), &needed) : -1;
    if (status == (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */ && needed != 0) {
        buffer.resize(needed + (1u << 20));
        status = system_query(5, buffer.data(), (ULONG)buffer.size(), &needed);
    }
    if (status != 0) {
        return;
    }
    const DWORD pid = GetCurrentProcessId();
    for (size_t offset = 0;;) {
        const uint8_t* process = buffer.data() + offset;
        uint32_t next = 0, count = 0;
        uintptr_t process_id = 0;
        std::memcpy(&next, process, 4);
        std::memcpy(&count, process + 4, 4);
        std::memcpy(&process_id, process + 0x50, sizeof(process_id));
        if (process_id == pid) {
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* thread = process + 0x100 + i * 0x50;
                uintptr_t thread_id = 0;
                uint32_t switches = 0;
                std::memcpy(&thread_id, thread + 0x30, sizeof(thread_id));
                std::memcpy(&switches, thread + 0x40, 4);
                threads.push_back({(DWORD)thread_id, switches});
            }
            break;
        }
        if (next == 0) {
            break;
        }
        offset += next;
    }
    using QueryFn = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    struct Row {
        double ms;
        std::string name;
        uint32_t switches;
    };
    std::vector<Row> rows;
    double total = 0.0;
    const DWORD self = GetCurrentThreadId();
    for (const ThreadSample& sample : threads) {
        const DWORD thread_id = sample.id;
        const HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, thread_id);
        if (thread == nullptr) {
            continue;
        }
        ULONG64 cycles = 0;
        QueryThreadCycleTime(thread, &cycles);
        if (thread_id == self) {
            cycles = report_start - report_cycles;
        }
        std::string name;
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr && description[0] != 0) {
            char narrow[64];
            WideCharToMultiByte(CP_UTF8, 0, description, -1, narrow, sizeof(narrow), nullptr, nullptr);
            name = narrow;
        }
        LocalFree(description);
        void* start = nullptr;
        HMODULE owner = nullptr;
        if (name.empty()) {
            // ThreadQuerySetWin32StartAddress (9).
            char where[160] = "?";
            if (query != nullptr && query(thread, 9, &start, sizeof(start), nullptr) == 0 &&
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(start), &owner)) {
                char path[MAX_PATH];
                const DWORD length = GetModuleFileNameA(owner, path, sizeof(path));
                const char* file = length != 0 ? std::strrchr(path, '\\') : nullptr;
                std::snprintf(where, sizeof(where), "%s+0x%llx", file != nullptr ? file + 1 : "?",
                              (unsigned long long)(static_cast<char*>(start) - reinterpret_cast<char*>(owner)));
            }
            name = "tid " + std::to_string(thread_id) + " " + where;
        }
        CloseHandle(thread);
        uint64_t& before = last[thread_id];
        const double ms = (double)(cycles - before) * 1000.0 / tsc_hz;
        before = cycles;
        total += ms;
        uint32_t& switches_before = last_switches[thread_id];
        const uint32_t switches = sample.switches - switches_before;
        switches_before = sample.switches;
        if (ms >= 0.5) {
            rows.push_back({ms, std::move(name), switches});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
    std::string line = "total " + std::to_string((int)(total + 0.5)) + " ms/s";
    for (const Row& row : rows) {
        char item[160];
        std::snprintf(item, sizeof(item), "; %s %.1f (%u sw)", row.name.c_str(), row.ms, row.switches);
        line += item;
    }
    log_line("cpu:", line.c_str());
    ULONG64 report_end = 0;
    QueryThreadCycleTime(GetCurrentThread(), &report_end);
    report_cycles += report_end - report_start;
}
#endif

#if defined(_WIN32)
// Crash reports. A fatal exception in the game module or the host, or the game's own fatal call, writes a minidump and
// the report (report_crash_facts) to the crash folder from a thread kept for it (the failing thread's stack may be what
// failed), logs where it happened and the game's threads, and ends the process.
using MiniDumpWriteDumpFn = decltype(&MiniDumpWriteDump);  // loaded at the crash: dbghelp.dll is not linked

HANDLE gCrashRequest;
HANDLE gCrashDone;
EXCEPTION_POINTERS* gCrashInfo;
DWORD gCrashThread;
std::atomic<int> gCrashing{0};

void crash_writer() {
    for (;;) {
        WaitForSingleObject(gCrashRequest, INFINITE);
        const fs::path folder = crash_folder();
        std::error_code ignored;
        fs::create_directories(folder, ignored);
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t path[MAX_PATH];
        std::swprintf(path, MAX_PATH, L"%ls\\nebula-native-%04u%02u%02u-%02u%02u%02u-%lu.dmp", folder.c_str(), now.wYear, now.wMonth,
                      now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId());
        const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
        const auto write = dbghelp != nullptr
                               ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
        const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write != nullptr && file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION exception{gCrashThread, gCrashInfo, FALSE};
            // Stacks and what they point at (the module's data segments would add its half-gigabyte image window).
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
            const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                  gCrashInfo != nullptr ? &exception : nullptr, nullptr, nullptr);
            char text[MAX_PATH + 32];
            std::snprintf(text, sizeof(text), "%s %ls", ok ? "minidump" : "minidump failed:", path);
            log_line("crash:", text);
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        report_crash_facts(fs::path(path));
        SetEvent(gCrashDone);
    }
}

void report_crash(EXCEPTION_POINTERS* info) {
    if (gCrashRequest == nullptr) {
        return;
    }
    gCrashInfo = info;
    gCrashThread = GetCurrentThreadId();
    SetEvent(gCrashRequest);
    WaitForSingleObject(gCrashDone, 60000);
    if (gModule.debug_dump != nullptr) {
        gModule.debug_dump();
    }
    std::fflush(stdout);
}

// The report, out of line: the filter below runs for every exception in the process and keeps its frame small.
[[noreturn]] __declspec(noinline) void report_fatal_exception(EXCEPTION_POINTERS* info, HMODULE owner) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const void* address = info->ExceptionRecord->ExceptionAddress;
    wchar_t name[MAX_PATH] = {};
    GetModuleFileNameW(owner, name, MAX_PATH);
    char text[MAX_PATH + 160];
    std::snprintf(text, sizeof(text), "exception 0x%08lX at %ls+0x%llx (thread %lu), address 0x%llx", code,
                  fs::path(name).filename().c_str(),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(owner)),
                  GetCurrentThreadId(),
                  static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters >= 2
                                                      ? info->ExceptionRecord->ExceptionInformation[1]
                                                      : 0));
    log_line("crash:", text);
    gather_crash_facts(galaxy::crash::Facts::Kind::Fault, text, owner == GetModuleHandleW(L"smg_game.dll"),
                       code == EXCEPTION_STACK_OVERFLOW);
    report_crash(info);
    std::_Exit(4);
}

// A C++ exception's record (MSVC's 0xE06D7363: the object, its ThrowInfo, the image base its offsets start from):
// the thrown type and, for a std::exception, what(). Read under SEH: the record comes from a failing process.
void describe_cpp_exception(const EXCEPTION_RECORD* record, char* out, std::size_t size) {
    std::snprintf(out, size, "uncaught C++ exception");
    if (record->NumberParameters < 4u) {
        return;
    }
    __try {
        const auto base = static_cast<uintptr_t>(record->ExceptionInformation[3]);
        const auto* object = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
        // ThrowInfo: attributes, unwind, forward compat, catchable types (32-bit offsets from the image base).
        const auto* throw_info = reinterpret_cast<const int32_t*>(record->ExceptionInformation[2]);
        const auto* types = reinterpret_cast<const int32_t*>(base + static_cast<uint32_t>(throw_info[3]));
        const char* thrown = nullptr;
        const char* what = nullptr;
        for (int32_t i = 0; i < types[0] && i < 16; ++i) {
            // CatchableType: properties, type descriptor, this displacement (mdisp, pdisp, vdisp), size, copy.
            const auto* type = reinterpret_cast<const int32_t*>(base + static_cast<uint32_t>(types[1 + i]));
            const char* name = reinterpret_cast<const char*>(base + static_cast<uint32_t>(type[1])) + 16;
            if (thrown == nullptr) {
                thrown = name;
            }
            if (std::strcmp(name, ".?AVexception@std@@") == 0) {
                what = reinterpret_cast<const std::exception*>(object + type[2])->what();
            }
        }
        std::snprintf(out, size, "uncaught C++ exception %s: %s", thrown != nullptr ? thrown : "?",
                      what != nullptr ? what : "(not a std::exception)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

#if defined(__clang__)
__attribute__((no_sanitize("address", "undefined")))
#endif
LONG CALLBACK crash_handler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // A C++ exception, logged where it is thrown (the first 32): one thrown on the game's own stacks cannot unwind
    // through them and ends the process before any filter runs. Handled or not, the line says what was thrown.
    if (code == 0xE06D7363u) {
        static std::atomic<unsigned> logged{0u};
        if (logged.fetch_add(1u, std::memory_order_relaxed) < 32u) {
            char text[512];
            describe_cpp_exception(info->ExceptionRecord, text, sizeof(text));
            char line[640];
            std::snprintf(line, sizeof(line), "thrown: %s (thread %lu)", text + std::strlen("uncaught "),
                          GetCurrentThreadId());
            log_line("exception:", line);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_INT_OVERFLOW &&
        code != EXCEPTION_IN_PAGE_ERROR) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Only faults in the game module or this executable: drivers and system libraries handle their own.
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress), &owner) ||
        (owner != GetModuleHandleW(nullptr) && owner != GetModuleHandleW(L"smg_game.dll"))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (gCrashing.exchange(1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    report_fatal_exception(info, owner);
}

// abort(), which std::terminate calls for an uncaught C++ exception: no exception filter sees it. Logs what was
// thrown, when that is why, and writes the dump from the aborting thread's context.
void abort_handler(int) {
    const char* text = "abort";
    std::string what;
    if (const std::exception_ptr active = std::current_exception()) {
        try {
            std::rethrow_exception(active);
        } catch (const std::exception& error) {
            what = std::string("uncaught exception: ") + error.what();
            text = what.c_str();
        } catch (...) {
            text = "uncaught exception of unknown type";
        }
    }
    log_line("crash:", text);
    if (gCrashing.exchange(1) == 0) {
        gather_crash_facts(galaxy::crash::Facts::Kind::Abort, text);
        CONTEXT context{};
        RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = 0xE0000003u;  // not a system code: marks the dump as an abort
        record.ExceptionAddress = _ReturnAddress();
        EXCEPTION_POINTERS pointers{&record, &context};
        report_crash(&pointers);
    }
    std::_Exit(3);
}

// The last filter: an exception no frame handled, of any kind. A C++ exception that leaves a thread through C or
// system frames reaches no terminate handler, so without this filter the process would end with 0xE06D7363 and no
// line or dump.
LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
    if (gCrashing.exchange(1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char text[512];
    const bool cpp = info->ExceptionRecord->ExceptionCode == 0xE06D7363u;
    if (cpp) {
        describe_cpp_exception(info->ExceptionRecord, text, sizeof(text));
    } else {
        std::snprintf(text, sizeof(text), "unhandled exception 0x%08lX at 0x%p", info->ExceptionRecord->ExceptionCode,
                      info->ExceptionRecord->ExceptionAddress);
    }
    char line[640];
    std::snprintf(line, sizeof(line), "%s (thread %lu)", text, GetCurrentThreadId());
    log_line("crash:", line);
    HMODULE owner = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress), &owner);
    gather_crash_facts(cpp ? galaxy::crash::Facts::Kind::Exception : galaxy::crash::Facts::Kind::Fault, line,
                       !cpp && owner != nullptr && owner == GetModuleHandleW(L"smg_game.dll"),
                       info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW);
    report_crash(info);
    std::_Exit(4);
}

#if defined(NEBULA_HOST_RENDERER)
// NebulaNative --crash-report <file>: the report of an instance that stopped (report_crash_facts), shown by this new
// process. The technical details stay in the file, which the dialog offers to show.
int show_crash_report_dialog() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr) return 1;
    const fs::path file = count >= 3 ? fs::path(args[2]) : fs::path();
    LocalFree(args);
    std::string text;
    if (FILE* in = file.empty() ? nullptr : _wfopen(file.c_str(), L"rb")) {
        char buffer[4096];
        std::size_t n = 0;
        while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0) text.append(buffer, n);
        std::fclose(in);
    }
    if (text.empty()) return 1;
    if (const std::size_t technical = text.find("\nTechnical details:"); technical != std::string::npos) {
        text.resize(technical);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    text += "\n\nOpen the folder with the report?";
    std::wstring wide(static_cast<std::size_t>(
                          MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)),
                      L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), static_cast<int>(wide.size()));
    if (MessageBoxW(nullptr, wide.c_str(), L"Nebula stopped", MB_ICONERROR | MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST) ==
        IDYES) {
        const std::wstring select = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", select.c_str(), nullptr, SW_SHOWNORMAL);
    }
    return 0;
}
#endif

void install_crash_reporter() {
    gCrashRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gCrashDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(crash_writer).detach();
    AddVectoredExceptionHandler(0, crash_handler);  // last: the module's own handlers (division, stack guards) first
    SetUnhandledExceptionFilter(unhandled_filter);  // whatever no frame handled, C++ exceptions included
    std::signal(SIGABRT, abort_handler);            // process-wide in the UCRT, unlike set_terminate
}
#endif

void svc_fatal(void*, const char* text) {
    log_line("fatal:", text);
    gather_crash_facts(galaxy::crash::Facts::Kind::GameFatal, text);
#if defined(_WIN32)
    if (gCrashing.exchange(1) == 0) {
        report_crash(nullptr);
    }
#else
    report_crash_facts({});
#endif
    std::fflush(stdout);
    std::_Exit(3);
}

#if defined(NEBULA_HOST_RENDERER)
// The renderer. The game's memory is the renderer's GuestMemory: the module's physical regions and the cached and
// uncached mirrors of MEM1 and MEM2 (virtual addresses in display lists). The command stream reaches it in frames:
// the spans since the last submission, contiguous in the ring's double mapping, go to render_frame at a PE marker
// (draw sync, draw done) or at the retrace, which also presents; render_frame copies them, so they are released at
// once. The renderer raises PE events through gNative's callbacks when its parse and the GPU reach them. All of this
// runs on the simulation thread, where the game makes these calls.
using galaxy::GuestMemoryRegion;
using galaxy::GuestMemory;
using galaxy::GxLogLevel;
using galaxy::GxServices;

GuestMemory gMemory{};
std::vector<GuestMemoryRegion> gRegions;
GxServices gNative{};
const uint8_t* gPending = nullptr;
size_t gPendingSize = 0;
std::deque<galaxy::gx::FramePeCompletionToken> gTokens;
std::atomic<uint64_t> gSubmissions{0};  // render_frame calls with commands: the renderer's frame index

void native_log(void*, GxLogLevel, const char* text) {
    log_line("renderer:", text);
}

void native_pe_finish(void*) {
    gModule.gx_event(1, 0);
}

void native_pe_token(void*, uint16_t token, bool) {
    gModule.gx_event(0, token);
}

galaxy::host::NativeGameAudio* gAudio = nullptr;

void setup_renderer_memory() {
    NebulaGameMemoryRegion regions[8];
    const int count = gModule.memory_regions(regions, 8);
    for (int i = 0; i < count; i++) {
        const NebulaGameMemoryRegion& r = regions[i];
        std::byte* const host = reinterpret_cast<std::byte*>(r.host);
        for (const uint32_t mirror : {0x00000000u, 0x80000000u, 0xC0000000u}) {
            gRegions.push_back({r.physical | mirror, r.size, host});
            if ((r.physical & 0x0FFFFFFFu) == 0) {
                gMemory.fast_regions[(r.physical | mirror) >> 28] = {r.size, host};
            }
        }
    }
    gMemory.regions = gRegions.data();
    gMemory.region_count = static_cast<uint32_t>(gRegions.size());
    gNative.log = native_log;
    gNative.gx_pe_finish = native_pe_finish;
    gNative.gx_pe_token = native_pe_token;
}

std::once_flag gMemoryOnce;

void ensure_guest_memory() {
    std::call_once(gMemoryOnce, setup_renderer_memory);
}

// `size` bytes at a console address, in the module's arena or image.
const std::byte* guest_bytes(uint32_t address, uint32_t size) {
    ensure_guest_memory();
    for (const galaxy::GuestMemoryRegion& region : gRegions) {
        if (address - region.guest_base < region.size && size <= region.size - (address - region.guest_base)) {
            return region.host_base + (address - region.guest_base);
        }
    }
    return nullptr;
}

// Takes the receipts of finished submissions, in order (each is consumed once).
void take_finished_tokens() {
    while (!gTokens.empty() && galaxy::gx::wait_for_frame_pe_completion_for(gTokens.front(), 0)) {
        gTokens.pop_front();
    }
}

void submit_frame_unchecked(bool present, uint32_t xfb) {
    const size_t size = gPendingSize;
    const auto token = galaxy::gx::render_frame(reinterpret_cast<const std::byte*>(gPending), size, &gMemory, &gNative,
                                                present, xfb);
    if (token) {
        gTokens.push_back(token);
        gSubmissions.fetch_add(1, std::memory_order_relaxed);
    }
    if (size != 0) {
        gPendingSize = 0;
        gModule.gx_release(size);
    }
    take_finished_tokens();
}

// The renderer's hard failures (the GX parser's: an unknown command, a truncated payload) end the game with their
// line and a dump. Caught here: thrown on the game's stacks they could not unwind through its frames.
void submit_frame(bool present, uint32_t xfb) {
    try {
        submit_frame_unchecked(present, xfb);
    } catch (const galaxy::gx::GxFatalError& error) {
        char text[1024];
        std::snprintf(text, sizeof(text), "the renderer stopped: %s (opcode 0x%02X at stream offset %zu)", error.what(),
                      static_cast<unsigned>(error.opcode()), error.fifo_offset());
        svc_fatal(nullptr, text);
    } catch (const std::exception& error) {
        char text[1024];
        std::snprintf(text, sizeof(text), "the renderer stopped: %s", error.what());
        svc_fatal(nullptr, text);
    }
}
#endif

void svc_gx_submit(void*, const uint8_t* bytes, size_t size) {
    gGxBytes.fetch_add(size, std::memory_order_relaxed);
#if defined(NEBULA_HOST_RENDERER)
    ensure_guest_memory();
    if (gPendingSize == 0) {
        gPending = bytes;
    } else if (bytes != gPending + gPendingSize && bytes + kGxRingSize != gPending + gPendingSize) {
        die("a GX span does not continue the last (%p after %p + %zu)", (const void*)bytes, (const void*)gPending, gPendingSize);
    }
    gPendingSize += size;
#else
    (void)bytes;
    gModule.gx_release(size);
#endif
}

void svc_gx_pe_marker(void*, int finish, uint16_t token) {
#if defined(NEBULA_HOST_RENDERER)
    (void)finish;
    (void)token;
    submit_frame(false, 0);
#else
    gModule.gx_event(finish, token);
#endif
}

// The stage the game reports before each 3D scene (game_stage); the scene's begin marker carries it.
std::string gSceneStage;

void svc_game_stage(void*, const char* name) {
    if (name != nullptr && gSceneStage != name) {
        gSceneStage = name;
    }
}

void svc_object_name(void*, uint32_t id, const char* name) {
#if defined(NEBULA_HOST_RENDERER)
    galaxy::gx::note_object_name(id, name);
#else
    (void)id;
    (void)name;
#endif
}

float svc_light_reach(void*, const char* model, float radius) {
#if defined(NEBULA_HOST_RENDERER)
    return galaxy::gx::rt_light_reach(model, radius);
#else
    (void)model;
    (void)radius;
    return 0.0f;
#endif
}

int svc_traced_sun(void*, float* direction) {
#if defined(NEBULA_HOST_RENDERER)
    std::array<float, 3> sun{};
    if (direction == nullptr || !galaxy::gx::traced_sun(sun, 500u)) {
        return 0;
    }
    direction[0] = sun[0];
    direction[1] = sun[1];
    direction[2] = sun[2];
    return 1;
#else
    (void)direction;
    return 0;
#endif
}

// The Discord application Nebula's status shows under ("Super Mario Galaxy"; GALAXY_DISCORD_CLIENT_ID replaces it).
constexpr const char* kDiscordClientId = "1562359210463727617";

// The game's message text (UTF-16, its tags skipped: 0x1A, then the tag's size in bytes in the next unit's high byte)
// as UTF-8.
std::string message_utf8(const uint16_t* text) {
    std::string out;
    if (text == nullptr) return out;
    for (size_t i = 0; i < 256u && text[i] != 0u; ++i) {
        uint32_t c = text[i];
        if (c == 0x1Au) {
            const uint32_t units = (static_cast<uint32_t>(text[i + 1u]) >> 8u) / 2u;
            if (units < 2u) break;
            i += units - 1u;
            continue;
        }
        if (c >= 0xD800u && c <= 0xDBFFu && text[i + 1u] >= 0xDC00u && text[i + 1u] <= 0xDFFFu) {
            c = 0x10000u + ((c - 0xD800u) << 10u) + (text[i + 1u] - 0xDC00u);
            ++i;
        }
        if (c < 0x20u) c = ' ';
        if (c < 0x80u) {
            out += static_cast<char>(c);
        } else if (c < 0x800u) {
            out += static_cast<char>(0xC0u | (c >> 6u));
            out += static_cast<char>(0x80u | (c & 0x3Fu));
        } else if (c < 0x10000u) {
            out += static_cast<char>(0xE0u | (c >> 12u));
            out += static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu));
            out += static_cast<char>(0x80u | (c & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (c >> 18u));
            out += static_cast<char>(0x80u | ((c >> 12u) & 0x3Fu));
            out += static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu));
            out += static_cast<char>(0x80u | (c & 0x3Fu));
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

// What the player is doing, for Discord: the galaxy (or the file select) and the star, else the Power Stars.
void svc_presence(void*, const uint16_t* galaxy, const uint16_t* star, int32_t power_stars) {
    galaxy::discord::Activity activity;
    if (power_stars < 0) {
        activity.details = "Choosing a file";
    } else {
        activity.details = message_utf8(galaxy);
        activity.state = message_utf8(star);
        if (activity.details.empty()) {
            activity.details = "At the title screen";  // the title's stage has no galaxy name
            activity.state.clear();
        } else if (activity.state.empty()) {
            activity.state = std::to_string(power_stars) + (power_stars == 1 ? " Power Star" : " Power Stars");
        }
    }
    static std::string last;
    const std::string line = activity.details + " | " + activity.state;
    if (line != last) {
        last = line;
        log_line("presence:", line.c_str());
    }
    galaxy::discord::set_activity(activity);
}

// An achievement the save data meets: unlocked once (achievements.ini), with its English name. The game names it in
// the played language itself (NebulaText). The headless host keeps no achievements.
int svc_achievement(void*, const char* id, uint16_t* name, uint32_t capacity) {
#if !defined(NEBULA_HOST_RENDERER)
    (void)id, (void)name, (void)capacity;
    return 0;
#else
    const galaxy::achievements::Info* info = id != nullptr ? galaxy::achievements::find(id) : nullptr;
    if (info == nullptr || !galaxy::achievements::unlock(id)) return 0;
    uint32_t i = 0;
    for (; name != nullptr && i + 1u < capacity && info->name[i] != '\0'; ++i) {
        name[i] = static_cast<unsigned char>(info->name[i]);
    }
    if (name != nullptr && capacity != 0u) name[i] = 0u;
    log_line("achievement:", info->name);
    return 1;
#endif
}

int32_t svc_achievement_date(void*, const char* id) {
#if !defined(NEBULA_HOST_RENDERER)
    (void)id;
    return 0;
#else
    return id != nullptr ? galaxy::achievements::unlocked_date(id) : 0;
#endif
}

// The all-time counters the achievement board shows (stats.ini). The headless host keeps none.
void svc_stat_add(void*, const char* name, uint64_t delta) {
#if defined(NEBULA_HOST_RENDERER)
    if (name != nullptr) galaxy::stats::add(name, delta);
#else
    (void)name, (void)delta;
#endif
}

uint64_t svc_stat_get(void*, const char* name) {
#if defined(NEBULA_HOST_RENDERER)
    return name != nullptr ? galaxy::stats::get(name) : 0u;
#else
    (void)name;
    return 0u;
#endif
}

#if defined(NEBULA_HOST_RENDERER)
// UTF-8 text into a fixed field, cut at a whole character, always terminated.
template <std::size_t N>
void copy_settings_text(char (&field)[N], const std::string& text) {
    std::size_t length = std::min(text.size(), N - 1u);
    while (length > 0u && length < text.size() && (static_cast<unsigned char>(text[length]) & 0xC0u) == 0x80u) {
        --length;
    }
    std::memcpy(field, text.data(), length);
    field[length] = '\0';
}
#endif

#if defined(NEBULA_HOST_RENDERER)
static_assert(static_cast<uint32_t>(galaxy::gx::NativeSettingsStatus::Leave) == NEBULA_SETTINGS_STATUS_LEAVE &&
              static_cast<uint32_t>(galaxy::gx::NativeSettingsStatus::ModsRestartAsk) == NEBULA_SETTINGS_STATUS_MODS_RESTART_ASK &&
              static_cast<uint32_t>(galaxy::gx::NativeSettingsStatus::KeepDisplay) == NEBULA_SETTINGS_STATUS_KEEP_DISPLAY,
              "the settings statuses are numbered alike");
static_assert(static_cast<uint32_t>(galaxy::gx::NativeSettingsInput::Open) == NEBULA_SETTINGS_OPEN &&
              static_cast<uint32_t>(galaxy::gx::NativeSettingsInput::Revert) == NEBULA_SETTINGS_REVERT &&
              static_cast<uint32_t>(galaxy::gx::NativeSettingsInput::MoveMod) == NEBULA_SETTINGS_MOVE_MOD,
              "the settings inputs are numbered alike");
#endif

// The settings menu for the game's own (NebulaSettingsMenu): the overlay's state, which the game draws from then on.
// The headless host has no settings menu: always closed.
void svc_settings_view(void*, NebulaSettingsView* view) {
    std::memset(view, 0, sizeof(*view));
#if defined(NEBULA_HOST_RENDERER)
    const galaxy::gx::NativeSettingsView source = galaxy::gx::native_settings_view();
    view->open = source.open ? 1u : 0u;
    view->page = source.page;
    view->page_count = static_cast<uint32_t>(std::min<std::size_t>(source.pages.size(), NEBULA_SETTINGS_PAGES));
    for (uint32_t i = 0; i < view->page_count; ++i) {
        copy_settings_text(view->pages[i], source.pages[i]);
    }
    view->controls_page = source.controls_page;
    view->input = source.input;
    // A long page (many mods) is sent in part: the rows around the selection, more than the menu shows at once.
    const std::size_t total = source.rows.size();
    const std::size_t selected = std::min<std::size_t>(source.selected, total == 0u ? 0u : total - 1u);
    const std::size_t first = total <= NEBULA_SETTINGS_ROWS
                                  ? 0u
                                  : std::min(selected - std::min<std::size_t>(selected, NEBULA_SETTINGS_ROWS / 2u),
                                             total - NEBULA_SETTINGS_ROWS);
    view->total_rows = static_cast<uint32_t>(total);
    view->first_row = static_cast<uint32_t>(first);
    view->row_count = static_cast<uint32_t>(std::min<std::size_t>(total - first, NEBULA_SETTINGS_ROWS));
    for (uint32_t i = 0; i < view->row_count; ++i) {
        const galaxy::gx::NativeSettingsViewRow& row = source.rows[first + i];
        copy_settings_text(view->rows[i].label, row.label);
        copy_settings_text(view->rows[i].value, row.value);
        view->rows[i].flags = (row.heading ? NEBULA_SETTINGS_ROW_HEADING : 0u) |
                              (row.changes ? NEBULA_SETTINGS_ROW_CHANGES : 0u) | (row.name ? NEBULA_SETTINGS_ROW_NAME : 0u) |
                              (row.active ? NEBULA_SETTINGS_ROW_ACTIVE : 0u) |
                              (row.label_name ? NEBULA_SETTINGS_ROW_LABEL_NAME : 0u) |
                              (row.buttons ? NEBULA_SETTINGS_ROW_BUTTONS : 0u) |
                              (row.mod_list == 1u ? NEBULA_SETTINGS_ROW_MOD_OFF : 0u) |
                              (row.mod_list == 2u ? NEBULA_SETTINGS_ROW_MOD_ON : 0u) | (row.held ? NEBULA_SETTINGS_ROW_HELD : 0u);
    }
    view->selected = static_cast<uint32_t>(selected);
    view->selected_button = source.selected_button;
    for (std::size_t r = 0; r < total; ++r) {
        const galaxy::gx::NativeSettingsViewRow& row = source.rows[r];
        if (row.mod_list == 0u || row.mod_list > 2u || row.heading) continue;
        if (r < first) ++view->mod_list_first[row.mod_list - 1u];
        ++view->mod_list_count[row.mod_list - 1u];
    }
    copy_settings_text(view->help, source.help);
    view->status = static_cast<uint32_t>(source.status);
    view->status_number = source.status_number;
    copy_settings_text(view->status_text, source.status_text);
#endif
}

// The pointer's input to the settings menu: NEBULA_SETTINGS_* are galaxy::gx::NativeSettingsInput's values.
void svc_settings_input(void*, uint32_t action, int32_t arg) {
#if defined(NEBULA_HOST_RENDERER)
    if (action >= NEBULA_SETTINGS_OPEN && action <= NEBULA_SETTINGS_MOVE_MOD) {
        galaxy::gx::native_settings_input(static_cast<galaxy::gx::NativeSettingsInput>(action), arg);
    }
#else
    (void)action, (void)arg;
#endif
}

// Nebula's screens with small text (sharp_2d): the 2D layer at the window's resolution while they show.
void svc_sharp_2d(void*, uint32_t on) {
#if defined(NEBULA_HOST_RENDERER)
    galaxy::gx::request_sharp_2d(on != 0u);
#else
    (void)on;
#endif
}

// A mod's code patch the game runs natively (NEBULA_PATCH_*), as the mods layer matched it at the start.
int svc_code_patch(void*, uint32_t id, uint32_t* value) {
    static_assert(galaxy::mods::kPatchFlipPanelGroup == NEBULA_PATCH_FLIP_PANEL_GROUP &&
                  galaxy::mods::kPatchMessengerExOneUps == NEBULA_PATCH_MESSENGER_EX_ONE_UPS);
    if (gMods == nullptr) return 0;
    const auto it = gMods->code_patches.find(id);
    if (it == gMods->code_patches.end()) return 0;
    if (value != nullptr) *value = it->second;
    // Logged the first time the game uses each.
    static std::atomic<uint64_t> logged{0};
    if (id < 64u && (logged.fetch_or(uint64_t{1} << id) & (uint64_t{1} << id)) == 0u) {
        char text[64];
        std::snprintf(text, sizeof(text), "native patch %u in use (value 0x%08X)", id, it->second);
        log_line("mods:", text);
    }
    return 1;
}

void svc_movie_frame(void*, const char* name, int32_t frame, float frame_rate, uint32_t y, uint32_t u, uint32_t v,
                     uint32_t width, uint32_t height) {
#if defined(NEBULA_HOST_RENDERER)
    galaxy::gx::attach_movie_frame(name, frame, frame_rate, y, u, v, width, height);
#else
    (void)name, (void)frame, (void)frame_rate, (void)y, (void)u, (void)v, (void)width, (void)height;
#endif
}

void svc_gx_scene_marker(void*, uint32_t marker, const float* view) {
    if (marker == NEBULA_SCENE_3D_BEGIN) {
        gSceneBegins.fetch_add(1, std::memory_order_relaxed);
    }
#if defined(NEBULA_HOST_RENDERER)
    if (marker == NEBULA_SCENE_3D_BEGIN && view != nullptr) {
        galaxy::gx::attach_scene_view(view, view[15] != 0.0f ? view + 12 : nullptr,
                                      gSceneStage.empty() ? nullptr : gSceneStage.c_str());
    }
    static constexpr galaxy::gx::FifoMarker kMarkers[] = {
        galaxy::gx::FifoMarker::SceneBegin, galaxy::gx::FifoMarker::SceneEnd,
        galaxy::gx::FifoMarker::Scene2DBegin, galaxy::gx::FifoMarker::Scene2DEnd};
    if (marker < std::size(kMarkers)) {
        galaxy::gx::attach_fifo_marker(kMarkers[marker], gPendingSize);
    }
#else
    (void)marker;
    (void)view;
#endif
}

void svc_gx_split_clip(void*, const float* plane, uint32_t pass, const float* separator) {
#if defined(NEBULA_HOST_RENDERER)
    static constexpr float kNone[4] = {};
    galaxy::gx::attach_split_clip(plane, pass, separator != nullptr ? separator : kNone, gPendingSize);
#else
    (void)plane;
    (void)pass;
    (void)separator;
#endif
}

uint32_t svc_luigi_swap(void*, char* out, uint32_t size) {
    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
#if defined(NEBULA_HOST_RENDERER)
    for (const galaxy::mods::ModInfo& mod : galaxy::mods::session_mods().started_with) {
        if (!mod.enabled) continue;
        std::ifstream file(mod.path / "luigi_swap.txt", std::ios::binary);
        if (!file) continue;
        file.read(out, size - 1);
        const auto length = static_cast<uint32_t>(file.gcount());
        out[length] = '\0';
        std::fprintf(stderr, "[anim] Luigi's swap table from mod %s (%u bytes)\n", mod.id.c_str(), length);
        return length;
    }
#endif
    return 0;
}

void svc_gx_stripe_particle(void*, size_t span_offset, uint32_t particle, uint32_t age_lifetime, uint8_t strip) {
#if defined(NEBULA_HOST_RENDERER)
    galaxy::gx::attach_stripe_particle(gPendingSize + span_offset, particle, age_lifetime, strip);
#else
    (void)span_offset;
    (void)particle;
    (void)age_lifetime;
    (void)strip;
#endif
}

// Deterministic mode (GALAXY_DETERMINISTIC=1, netplay; the module's sdk/os/os_native.c has the scheme): the module keeps
// a virtual clock and takes its events itself; the host grants each retrace from the frame clock, with that frame's
// remote reports, once the game has taken the previous one.
bool deterministic() {
    static const bool on = [] {
        const char* value = std::getenv("GALAXY_DETERMINISTIC");
        return value != nullptr && value[0] == '1';
    }();
    return on || galaxy::net::session().active();
}

uint32_t svc_gx_peek_efb(void*, uint16_t x, uint16_t y, uint32_t type, uint16_t token) {
    // Deterministic mode: what the GPU drew is this PC's (its resolution, its timing); the game reads a far depth and
    // black everywhere.
    if (deterministic()) {
        (void)x, (void)y, (void)token;
        return type == 0 ? 0u : 0x00FFFFFFu;
    }
#if defined(NEBULA_HOST_RENDERER)
    const auto kind = type == 0 ? galaxy::gx::EfbPeekKind::Color : galaxy::gx::EfbPeekKind::Depth;
    uint32_t value = 0;
    if (!galaxy::gx::peek_efb_published(x, y, kind, token, value)) {
        submit_frame(false, 0);
        (void)galaxy::gx::peek_efb(x, y, kind, value);
    }
    return value;
#else
    (void)x;
    (void)y;
    (void)type;
    (void)token;
    return 0;
#endif
}

std::atomic<int32_t> gStarPointerMode{-1};
std::atomic<uint32_t> gTiltSection{0};
std::atomic<bool> gMotor{false};

void svc_game_context(void*, uint32_t what, int32_t value) {
    char text[64];
    if (what == NEBULA_CONTEXT_STAR_POINTER_MODE) {
        if (gStarPointerMode.exchange(value, std::memory_order_relaxed) != value) {
            std::snprintf(text, sizeof(text), "star pointer mode %d", static_cast<int>(value));
            log_line("context:", text);
        }
    } else if (what == NEBULA_CONTEXT_TILT_SECTION) {
        if (gTiltSection.exchange(static_cast<uint32_t>(value), std::memory_order_relaxed) != static_cast<uint32_t>(value)) {
            std::snprintf(text, sizeof(text), "tilt section %d", static_cast<int>(value));
            log_line("context:", text);
        }
    }
}

void svc_gpu_coherent(void*, uint32_t physical, uint32_t size) {
#if defined(NEBULA_HOST_RENDERER)
    galaxy::gx::notify_guest_memory_write(physical, size);
#else
    (void)physical;
    (void)size;
#endif
}

const uint8_t* svc_disc_fst(void*, size_t* size) {
    if (gMods != nullptr && !gMods->fst.empty()) {
        *size = gMods->fst.size();
        return gMods->fst.data();
    }
    *size = gPak.fst_size;
    return gPak.fst;
}

// A read of a file a mod changed: the crash report names the last one (galaxy::crash). The disc thread only.
void note_mod_file(uint64_t offset) {
    const std::string* path = nullptr;
    const galaxy::mods::DiscNode* file = gMods->disc.placed_file(offset, &path);
    if (file == nullptr) return;
    static const galaxy::mods::DiscNode* last = nullptr;
    static std::string mod;
    if (file != last) {
        last = file;
        // The mod is the folder under the mods folder its first host file is in.
        mod = "a mod";
        const fs::path mods = galaxy::mods::mods_folder();
        for (const galaxy::mods::DiscPiece& piece : file->pieces) {
            if (piece.source != galaxy::mods::DiscPiece::Source::HostFile) continue;
            const fs::path relative = piece.path.lexically_relative(mods);
            if (!relative.empty() && relative.begin()->native() != fs::path("..").native()) {
                mod = relative.begin()->string();
            }
            break;
        }
    }
    galaxy::crash::note_mod_read(mod, *path);
}

// Disc reads posted and not yet finished (the state hash waits for them: a read in progress fills memory the game does
// not look at yet).
std::atomic<int> gDiscInFlight{0};

void svc_disc_read(void*, uint64_t offset, uint32_t size, void* dst, uint32_t slot) {
    gDiscReads.fetch_add(1, std::memory_order_relaxed);
    gDiscInFlight.fetch_add(1, std::memory_order_acq_rel);
    // GALAXY_TEST_DISC_DELAY_US: a slow drive (each read takes this long), so waits on loads are exercised.
    static const uint32_t test_delay_us = [] {
        const char* value = std::getenv("GALAXY_TEST_DISC_DELAY_US");
        return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    gDisc.post([=] {
        struct Done {
            ~Done() { gDiscInFlight.fetch_sub(1, std::memory_order_acq_rel); }
        } done;
        if (test_delay_us != 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(test_delay_us));
        }
        if (gMods != nullptr && !gMods->fst.empty()) {
            // The mods' disc: the partition, or a changed file's pieces past it.
            if (!gMods->disc.read(offset, size, static_cast<uint8_t*>(dst), gPak.data)) {
                gModule.disc_complete(slot, -1);
                return;
            }
            note_mod_file(offset);
        } else if (offset > gPak.size || gPak.size - offset < size) {
            gModule.disc_complete(slot, -1);
            return;
        } else {
            std::memcpy(dst, gPak.data + offset, size);
        }
#if defined(NEBULA_HOST_RENDERER)
        // The console's disc DMA writes memory the GPU sees.
        mark_written(dst, size);
#endif
        gModule.disc_complete(slot, (int32_t)size);
    });
}

const uint8_t* svc_disc_dol(void*, size_t* size) {
    if (gMods != nullptr && !gMods->dol.empty()) {
        *size = gMods->dol.size();
        return gMods->dol.data();
    }
    *size = gPak.dol_size;
    return gPak.dol;
}

std::atomic<uint32_t> gRetraces{0};
std::atomic<uint32_t> gXfb{0};

#if defined(NEBULA_HOST_RENDERER) && defined(_WIN32)
// Button prompts (button_prompts.h): the picture font, the HUD's A icon and the tutorial pictures in game memory get
// the controller in use's buttons. On the simulation thread, before the frame's commands are parsed.
void prompts_tick() {
    static galaxy::prompts::GuestView view = [] {
        galaxy::prompts::GuestView v;
        // The scan reads whole slices: the game's thread stacks at MEM2's top have no-access guard pages between them.
        v.resolve = [](void*, uint32_t physical, uint32_t size) -> std::byte* {
            std::byte* const bytes = const_cast<std::byte*>(guest_bytes(physical, size));
            if (bytes == nullptr) return nullptr;
            for (std::byte* p = bytes; p < bytes + size;) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(p, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
                    (info.Protect & (PAGE_READWRITE | PAGE_READONLY)) == 0 || (info.Protect & PAGE_GUARD) != 0) {
                    return nullptr;
                }
                p = static_cast<std::byte*>(info.BaseAddress) + info.RegionSize;
            }
            return bytes;
        };
        v.written = [](void*, uint32_t physical, uint32_t size) {
            galaxy::gx::notify_guest_memory_write(physical, size);
        };
        NebulaGameMemoryRegion regions[8];
        const int count = gModule.memory_regions(regions, 8);
        for (int i = 0; i < count && v.region_count < v.regions.size(); i++) {
            // MEM1 and MEM2 (the module's image holds no fonts or textures the game loads).
            if (regions[i].physical == 0u || regions[i].physical == 0x10000000u) {
                v.regions[v.region_count++] = {regions[i].physical, regions[i].size};
            }
        }
        return v;
    }();
    galaxy::prompts::tick(view);
}
#endif

void svc_vi_frame(void*, uint32_t xfb, int black, uint32_t retrace) {
    gXfb.store(xfb, std::memory_order_relaxed);
    gRetraces.store(retrace, std::memory_order_relaxed);
#if defined(NEBULA_HOST_RENDERER)
#if defined(_WIN32)
    galaxy::timeline::record(galaxy::timeline::Event::GuestFrame, retrace, xfb);
    if (!gRegions.empty()) prompts_tick();
#endif
    if (!gRegions.empty()) {
        // Frames a rollback runs again are not shown (the clock grants them at once).
        const bool catching_up = deterministic() && retrace < gCatchUpTo.load(std::memory_order_acquire);
        submit_frame(!catching_up, xfb == 0 || black ? galaxy::gx::kNoDisplayedXfbAddr : xfb);
    }
    // GALAXY_TEST_MII_IMPORT=<retrace>:<file>: imports the file at that retrace, as the settings do.
    {
        static const char* const test_import = std::getenv("GALAXY_TEST_MII_IMPORT");
        static bool test_imported = false;
        if (test_import != nullptr && !test_imported) {
            const char* const colon = std::strchr(test_import, ':');
            if (colon != nullptr && retrace >= std::strtoul(test_import, nullptr, 10)) {
                test_imported = true;
                const std::filesystem::path file(colon + 1);
                std::string message;
                (void)galaxy::host::import_mii_files(std::span<const std::filesystem::path>(&file, 1u), &message);
                std::fprintf(stderr, "[mii] test import at %u: %s\n", retrace, message.c_str());
            }
        }
    }
    // Miis imported from the settings while the game runs: into its loaded database too (the file has them).
    for (const auto& [slot, mii] : galaxy::host::take_mii_installs()) {
        const uint32_t placed = gModule.mii_install(slot, reinterpret_cast<const uint8_t*>(mii.data()));
        std::fprintf(stderr, "[mii] slot %u %s\n", slot, placed != 0u ? "installed" : "left for the next start");
    }
    // The output's shape (the window's, or the chosen aspect) is latched once this retrace's picture is presented:
    // queued frames finish first, so a picture is never fitted with the next frame's camera and layouts.
    {
        const auto client = galaxy::gx::aspect_latch_extent();
        if (client.width > 1 && client.height > 1 &&
            galaxy::experimental_dynamic_aspect_change_pending(client.width, client.height)) {
            galaxy::gx::wait_for_render_idle();
            const auto settled = galaxy::gx::aspect_latch_extent();
            if (settled.width > 1 && settled.height > 1) {
                galaxy::experimental_latch_dynamic_aspect(settled.width, settled.height);
            }
        }
    }
#else
    (void)black;
#endif
}

// The shape the game draws for (display_native.c turns it into the game's screen width and camera aspect): the
// latched output shape; headless, the Wii's 16:9.
uint32_t svc_display_extent(void*) {
    // Deterministic mode: the game's own picture shape on every PC (the window's would change what the camera sees).
    if (deterministic()) {
        return 0;
    }
#if defined(NEBULA_HOST_RENDERER)
    return static_cast<uint32_t>(galaxy::g_experimental_dynamic_aspect_word.load(std::memory_order_acquire));
#else
    return 0;
#endif
}

void svc_scene_surround(void*, float intensity) {
#if defined(NEBULA_HOST_RENDERER)
    galaxy::gx::set_safety_scene_surround(intensity);
#else
    (void)intensity;
#endif
}

// The game's lights (NEBULA_LIGHT_*) into the renderer's light registry (game_lights.cpp), which the ray tracer reads.
void svc_game_light(void*, uint32_t kind, uint32_t id, const float* v) {
#if defined(NEBULA_HOST_RENDERER)
    switch (kind) {
    case NEBULA_LIGHT_SUN:
        galaxy::gx::set_game_sun({v[0], v[1], v[2]}, v[3]);
        break;
    case NEBULA_LIGHT_STAR_CORE:
        galaxy::gx::set_star_core({v[0], v[1], v[2]}, v[3]);
        break;
    case NEBULA_LIGHT_POINT:
        galaxy::gx::note_game_point_light(id, {v[0], v[1], v[2]}, {v[3], v[4], v[5]}, v[6]);
        break;
    case NEBULA_LIGHT_POINT_REQUEST:
        galaxy::gx::note_game_point_request(id, {v[0], v[1], v[2]}, {v[3], v[4], v[5]}, v[6]);
        break;
    case NEBULA_LIGHT_WORLD:
        galaxy::gx::note_game_world_light({v[0], v[1], v[2]}, {v[3], v[4], v[5]});
        break;
    case NEBULA_LIGHT_AREA:
    case NEBULA_LIGHT_PLAYER: {
        galaxy::gx::GameAreaLight light{};
        light.ambient = {v[0], v[1], v[2]};
        for (unsigned i = 0; i < 2u; ++i) {
            const float* l = v + 3 + 7 * i;
            light.lights[i].color = {l[0], l[1], l[2]};
            light.lights[i].position = {l[3], l[4], l[5]};
            light.lights[i].follows_camera = l[6] != 0.0f;
        }
        galaxy::gx::set_game_area_light(kind == NEBULA_LIGHT_AREA ? galaxy::gx::GameAreaLightKind::Stage
                                                                  : galaxy::gx::GameAreaLightKind::Player,
                                        light);
        break;
    }
    default:
        break;
    }
#else
    (void)kind;
    (void)id;
    (void)v;
#endif
}

uint32_t svc_option(void*, uint32_t what) {
    // Online: what changes the game's computation is the session's (the host's), the same on both PCs.
    if (galaxy::net::session().active()) {
        const galaxy::net::SessionSettings& session = galaxy::net::session().settings();
        switch (what) {
        case NEBULA_OPTION_COOP: return session.coop;
        case NEBULA_OPTION_MOVESET: return session.moveset;
        case NEBULA_OPTION_MOTION_STYLE: return session.motion_style;
        case NEBULA_OPTION_TEXT_SPEED: return session.text_speed;
        case NEBULA_OPTION_CALENDAR: return session.calendar;
        case NEBULA_OPTION_RT_SHADOW_CASTERS: return 0;  // ray tracing keeps clipped actors drawn: this PC's choice
        default: break;
        }
    }
    switch (what) {
    case NEBULA_OPTION_TEXT_SPEED:
#if defined(NEBULA_HOST_RENDERER)
        return std::clamp(galaxy::get_runtime_settings().text_speed, 1u, 4u);
#else
        return 1;
#endif
    case NEBULA_OPTION_MOVESET: {
#if defined(NEBULA_HOST_RENDERER)
        const galaxy::OdysseyMovesetSettings moves = galaxy::get_runtime_settings().moveset;
        return (moves.enabled ? NEBULA_MOVESET_ON : 0u) | (moves.roll ? NEBULA_MOVESET_ROLL : 0u) |
               (moves.roll_jump ? NEBULA_MOVESET_ROLL_JUMP : 0u) |
               (moves.ground_pound_jump ? NEBULA_MOVESET_GROUND_POUND_JUMP : 0u) |
               (moves.dive ? NEBULA_MOVESET_DIVE : 0u) | (moves.turn_boost ? NEBULA_MOVESET_TURN_BOOST : 0u) |
               (moves.second_player ? NEBULA_MOVESET_SECOND_PLAYER : 0u);
#else
        return 0;
#endif
    }
    case NEBULA_OPTION_MOTION_STYLE:
#if defined(NEBULA_HOST_RENDERER)
        return std::min(galaxy::get_runtime_settings().motion_style, 2u);
#else
        return 0;
#endif
    case NEBULA_OPTION_DETERMINISTIC:
        return deterministic() ? 1u : 0u;
    case NEBULA_OPTION_CALENDAR: {
        // GALAXY_DET_CALENDAR: seconds since 2000-01-01 (a netplay session's host sends its own); 2026-01-01 else.
        static const uint32_t seconds = [] {
            const char* value = std::getenv("GALAXY_DET_CALENDAR");
            return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 820454400u;
        }();
        return seconds;
    }
    case NEBULA_OPTION_COOP:
#if defined(NEBULA_HOST_RENDERER)
        return galaxy::coop_for_boot() ? 1u : 0u;
#else
        return 0;
#endif
    case NEBULA_OPTION_DRAW_IDENTITIES:
#if defined(NEBULA_HOST_RENDERER)
        return galaxy::gx::draw_identity_wanted() ? 1u : 0u;
#else
        return 0;
#endif
    case NEBULA_OPTION_RT_SHADOW_CASTERS:
#if defined(NEBULA_HOST_RENDERER)
        return galaxy::gx::rt_shadow_casters_wanted() ? 1u : 0u;
#else
        return 0;
#endif
    case NEBULA_OPTION_TEST_STAR_BITS: {
        static const uint32_t count = [] {
            const char* value = std::getenv("GALAXY_TEST_STAR_BITS");
            return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        }();
        return count;
    }
    case NEBULA_OPTION_TEST_PUT: {
        static const uint32_t put = [] {
            const char* value = std::getenv("GALAXY_TEST_PUT");
            unsigned kind = 0, player = 0, turn = 0;
            return value != nullptr && sscanf_s(value, "%u,%u,%u", &kind, &player, &turn) == 3
                ? (turn & 0xFFFFFu) | (kind & 0xFu) << 20 | (player & 1u) << 28 : 0u;
        }();
        return put;
    }
    case NEBULA_OPTION_TEST_LAUNCH_STAR: {
        static const uint32_t turn = [] {
            const char* value = std::getenv("GALAXY_TEST_LAUNCH_STAR");
            return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        }();
        return turn;
    }
    case NEBULA_OPTION_TEST_DOWN_SECOND: {
        static const uint32_t turn = [] {
            const char* value = std::getenv("GALAXY_TEST_DOWN_SECOND");
            return value != nullptr ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        }();
        return turn;
    }
    case NEBULA_OPTION_TEST_DUMP_LAYOUTS: {
        static const bool dump = [] {
            const char* value = std::getenv("GALAXY_TEST_DUMP_LAYOUTS");
            return value != nullptr && value[0] == '1';
        }();
        return dump ? 1u : 0u;
    }
    default:
        return 0;
    }
}

void svc_fs_request(void*, const NebulaFsRequest* request, uint32_t slot) {
    gFsOps.fetch_add(1, std::memory_order_relaxed);
    const NebulaFsRequest r = *request;
    gFs.post([r, slot] {
        gModule.fs_complete(slot, fs_run(r));
    });
}

// The system settings (SYSCONF) of a new console, as the game reads them. The pointer mapping (native_input's
// virtual sensor bar) assumes the bar above the screen.
uint32_t svc_sc_get(void*, uint32_t item) {
    switch (item) {
    case NEBULA_SC_LANGUAGE:
#if defined(NEBULA_HOST_RENDERER)
        return static_cast<uint32_t>(galaxy::game_language_for_boot());  // the language setting, fixed at boot
#else
        return 1;  // English
#endif
    case NEBULA_SC_SOUND_MODE:
        return 1;  // stereo
    case NEBULA_SC_ASPECT_RATIO:
        return 1;  // 16:9
    case NEBULA_SC_PROGRESSIVE_MODE:
        return 0;
    case NEBULA_SC_EURGB60_MODE:
        return 1;
    case NEBULA_SC_SENSOR_BAR_POSITION:
        return 1;  // above the screen
    case NEBULA_SC_DPD_SENSITIVITY:
        return 3;
    case NEBULA_SC_SPEAKER_VOLUME:
        return 0x58;
    case NEBULA_SC_SCREEN_SAVER_MODE:
        return 1;  // IPL.SSV
    case NEBULA_SC_MOTOR_MODE:
        return 1;
    default:
        return 0;
    }
}

// Remote 0: Nebula's controls (the bindings, keyboard and mouse, controllers) as a Wii Remote with a Nunchuk,
// connected when the game starts WPAD and reporting 100 times a second in the format the game asks for. Headless, the
// same remote at rest: the game sees a connected controller (no "connect the Wii Remote" pause) and no input.
std::atomic<int> gWpadStarted{0};
// The retrace the game had last taken when it started WPAD (the simulation thread's own count, so the same on every
// run): deterministic mode sends reports from the retraces after it.
std::atomic<uint32_t> gWpadStartRetrace{0};
// Per channel: 0 the first player's remote, 1 co-op's second player's (connected while he plays).
std::atomic<uint32_t> gWpadReport[2]{0x30, 0x30};
std::atomic<int> gWpadDpd{0};

uint8_t report_id_for_format(uint32_t format) {
    switch (format) {
    case 0:
        return 0x30;  // WPAD_FMT_CORE
    case 1:
        return 0x31;  // WPAD_FMT_CORE_ACC
    case 2:
        return 0x33;  // WPAD_FMT_CORE_ACC_DPD
    default:
        return 0x37;  // the Nunchuk formats: core, accelerometer, IR and extension
    }
}

#if !defined(NEBULA_HOST_RENDERER)
// The virtual remote's EEPROM calibration, as native_host.cpp gives it: IR, then the accelerometer's 0 g and 1 g, each
// twice; and its Nunchuk's: accelerometer 0 g and 1 g, stick maximum, minimum and centre.
void idle_wiimote_calibration(uint8_t (&eeprom)[0x2A], uint8_t (&nunchuk)[32]) {
    static const uint8_t ir[11] = {0x7f, 0xa2, 0x8b, 0x80, 0xa2, 0x80, 0x5d, 0x30, 0x7f, 0x5d, 0x0c};
    static const uint8_t accel[10] = {0x80, 0x80, 0x80, 0x00, 0x99, 0x99, 0x99, 0x00, 0x00, 0xa0};
    static const uint8_t extension[16] = {0x7d, 0x7e, 0x7a, 0x3c, 0xb1, 0xaf, 0xae, 0x1b,
                                          0xe0, 0x15, 0x7b, 0xe6, 0x1d, 0x81, 0x23, 0x78};
    std::memset(eeprom, 0, sizeof(eeprom));
    std::memset(nunchuk, 0, sizeof(nunchuk));
    std::memcpy(eeprom, ir, sizeof(ir));
    std::memcpy(eeprom + 0x0B, ir, sizeof(ir));
    std::memcpy(eeprom + 0x16, accel, sizeof(accel));
    std::memcpy(eeprom + 0x20, accel, sizeof(accel));
    std::memcpy(nunchuk, extension, sizeof(extension));
}

// A report from the remote at rest, from its id: no buttons, the accelerometer at 1 g on Z, no IR points, the Nunchuk
// level with its stick at the calibrated centre and C and Z up.
size_t idle_wiimote_report(uint8_t id, uint8_t* report) {
    std::memset(report, 0, 22);
    report[0] = id;
    if (id == 0x30) {
        return 3;
    }
    report[3] = 0x80;
    report[4] = 0x80;
    report[5] = 0x99;
    if (id == 0x31) {
        return 6;
    }
    if (id == 0x33) {
        std::memset(report + 6, 0xFF, 12);
        return 18;
    }
    std::memset(report + 6, 0xFF, 10);
    static const uint8_t nunchuk[6] = {0x7b, 0x81, 0x7d, 0x7e, 0xae, 0x03};
    std::memcpy(report + 16, nunchuk, sizeof(nunchuk));
    return 22;
}
#endif

// The remotes' reports for one moment (`frame`: the retrace the input scripts count): remote 0 connected with the
// first report, co-op's remote 1 while its player is in. The WPAD thread sends them 100 times a second; in deterministic
// mode the frame clock sends one set per retrace, before granting it.
// Online (net_lockstep.h): both players' input for the frame being granted, by channel (the frame clock sets it).
const galaxy::input::WiimoteInputSnapshot* gNetInputs = nullptr;

// Whether the remotes' connections were sent (send_wpad_reports; a rollback takes them back with the game).
bool gWpadConnected = false;
bool gPartnerConnected = false;

void send_wpad_reports(uint32_t frame) {
    static NebulaWiimoteConnection connection{};
    bool& connected = gWpadConnected;
    bool& partner_connected = gPartnerConnected;
    if (!connected) {
#if defined(NEBULA_HOST_RENDERER)
        galaxy::host::native_game_wiimote_calibration(connection.eeprom, connection.extension_calibration);
#else
        idle_wiimote_calibration(connection.eeprom, connection.extension_calibration);
#endif
        connection.extension = 1;  // WPAD_DEV_FREESTYLE: the Nunchuk
        connection.battery = 0xC0;
        gModule.wpad_connect(0, 1, &connection);
        connected = true;
    }
    (void)partner_connected;
    (void)frame;
    {
        const uint8_t id = static_cast<uint8_t>(gWpadReport[0].load(std::memory_order_relaxed));
#if !defined(NEBULA_HOST_RENDERER)
        uint8_t idle[22];
        gModule.wpad_report(0, idle, idle_wiimote_report(id, idle), nullptr);
        return;
#else
        const int32_t mode = gStarPointerMode.load(std::memory_order_relaxed);
        const auto snapshot = gNetInputs != nullptr
            ? gNetInputs[0]
            : galaxy::host::native_game_wiimote_snapshot(frame, gWpadDpd.load(std::memory_order_relaxed) != 0, mode,
                                                         static_cast<uint8_t>(gTiltSection.load(std::memory_order_relaxed)));
        // Every report renews the motor's state (a stale one stops), as the console's remote keeps its motor bit.
        galaxy::host::native_game_rumble(gMotor.load(std::memory_order_relaxed), mode);
        uint8_t report[galaxy::input::kMaxWiimoteInputReportBytes];
        const auto built = galaxy::input::build_wiimote_input_report(id, snapshot, report);
        if (built.status == galaxy::input::WiimoteReportBuildStatus::Ok && built.size > 1) {
            // From the report id, past the HID header; the exact pointer its IR dots stand for, when there is one.
            const NebulaWpadPointer pointer{snapshot.pointer_x, snapshot.pointer_y};
            gModule.wpad_shake(0, (snapshot.shake ? 1u : 0u) | (snapshot.nunchuk_shake ? 2u : 0u));
            gModule.wpad_report(0, report + 1, built.size - 1, snapshot.pointer_exact ? &pointer : nullptr);
        }
        // Co-op's second player: his remote connects when he joins and goes when he leaves (NebulaCoop.cpp).
        galaxy::input::WiimoteInputSnapshot partner{};
        // Online, the second player is the other PC's (or this one's, joined) and always in.
        const bool partner_in = gNetInputs != nullptr ? galaxy::net::session().settings().coop != 0
                                                      : galaxy::host::native_game_partner_snapshot(frame, mode, partner);
        if (gNetInputs != nullptr) {
            partner = gNetInputs[1];
        }
        if (partner_in != partner_connected) {
            partner_connected = partner_in;
            gModule.wpad_connect(1, partner_in ? 1 : 0, &connection);
            std::fprintf(stderr, "[coop] remote 2 %s\n", partner_in ? "connected" : "disconnected");
        }
        if (partner_in) {
            const uint8_t partner_id = static_cast<uint8_t>(gWpadReport[1].load(std::memory_order_relaxed));
            const auto partner_built = galaxy::input::build_wiimote_input_report(partner_id, partner, report);
            if (partner_built.status == galaxy::input::WiimoteReportBuildStatus::Ok && partner_built.size > 1) {
                gModule.wpad_shake(1, (partner.shake ? 1u : 0u) | (partner.nunchuk_shake ? 2u : 0u));
                gModule.wpad_report(1, report + 1, partner_built.size - 1, nullptr);
            }
        }
#endif
    }
}

void wpad_thread() {
    name_thread(L"Nebula Host WPAD");
    while (!gWpadStarted.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (deterministic()) {
        return;  // the frame clock sends them (vi_thread)
    }
    const auto period = std::chrono::milliseconds(10);
    auto next = Clock::now() + period;
    for (;;) {
        sleep_until_precise(next);
        next += period;
        send_wpad_reports(gRetraces.load(std::memory_order_relaxed));
    }
}

void svc_wpad_control(void*, uint32_t chan, uint32_t what, uint32_t value) {
    if (chan == 1 && what == NEBULA_WPAD_FORMAT) {
        gWpadReport[1].store(report_id_for_format(value), std::memory_order_relaxed);
        return;
    }
    if (chan != 0) {
        return;
    }
    switch (what) {
    case NEBULA_WPAD_INIT:
        gWpadStartRetrace.store(gRetraces.load(std::memory_order_relaxed), std::memory_order_relaxed);
        gWpadStarted.store(1, std::memory_order_release);
        break;
    case NEBULA_WPAD_FORMAT:
        gWpadReport[0].store(report_id_for_format(value), std::memory_order_relaxed);
        break;
    case NEBULA_WPAD_DPD:
        gWpadDpd.store(value != 0, std::memory_order_relaxed);
        break;
    case NEBULA_WPAD_MOTOR:
        gMotor.store(value != 0, std::memory_order_relaxed);
#if defined(NEBULA_HOST_RENDERER)
        galaxy::host::native_game_rumble(value != 0, gStarPointerMode.load(std::memory_order_relaxed));
#endif
        break;
    default:
        break;
    }
}
void svc_wpad_speaker_data(void*, uint32_t, const uint8_t*, uint32_t) {}

// The JAudio DSP program, run on the host: it mixes each subframe when the CPU releases its last channel group
// (JASDSPChannel::updateAll's releaseHalt(3), a one-word command). Headless, the subframe completes unmixed.
int svc_audio_dsp(void*, const uint32_t* words, uint32_t count) {
#if defined(NEBULA_HOST_RENDERER)
    ensure_guest_memory();
    const int subframe = gAudio->dsp_command(words, count, gMemory) ? 1 : 0;
#else
    const int subframe = count == 0 && (words[0] >> 16) == 3;
#endif
    gDspFrames.fetch_add(subframe, std::memory_order_relaxed);
    return subframe;
}

void svc_ai_dma(void*, uint32_t start, uint32_t size) {
    std::lock_guard<std::mutex> lock(gAiLock);
    gAiLatchedStart = start;
    gAiLatchedSize = size;
    gAiWake.notify_one();
}

void svc_ai_control(void*, uint32_t what, uint32_t value) {
    std::lock_guard<std::mutex> lock(gAiLock);
    if (what == NEBULA_AI_START) {
        gAiRunning = true;
    } else if (what == NEBULA_AI_STOP) {
        gAiRunning = false;
    } else if (what == NEBULA_AI_RATE) {
        gAiRate = value;
    }
    gAiWake.notify_one();
}

int svc_hbm(void*, uint32_t what, uint32_t value) {
#if defined(NEBULA_HOST_RENDERER)
    switch (what) {
    case NEBULA_HBM_OPEN:
        galaxy::gx::home_menu_open();
        return 0;
    case NEBULA_HBM_POLL: {
        // GALAXY_TEST_HOME_ROW=<row>:<polls>: picks a row by the keys a player would press, and answers yes when it
        // asks (reset, leave).
        static const char* test = std::getenv("GALAXY_TEST_HOME_ROW");
        static unsigned polls = 0;
        if (test != nullptr && galaxy::gx::home_menu_visible()) {
            unsigned row = 0, after = 0;
            if (std::sscanf(test, "%u:%u", &row, &after) == 2 && ++polls == after) {
                for (unsigned i = 0; i < row; ++i) {
                    galaxy::gx::native_settings_post_key(VK_DOWN);
                }
                galaxy::gx::native_settings_post_key(VK_RETURN);
                if (row != 0) {
                    galaxy::gx::native_settings_post_key(VK_RIGHT);
                    galaxy::gx::native_settings_post_key(VK_RETURN);
                }
            }
        }
        return galaxy::gx::home_menu_poll();
    }
    case NEBULA_HBM_CLOSE:
        galaxy::gx::home_menu_close();
        return 0;
    case NEBULA_HBM_VIEW:
        return galaxy::gx::home_menu_view(value != 0u);
    case NEBULA_HBM_POINT:
        galaxy::gx::home_menu_point(value);
        return 0;
    case NEBULA_HBM_CHOOSE:
        galaxy::gx::home_menu_choose(value);
        return 0;
    default:
        return 0;
    }
#else
    (void)value;
    return what == NEBULA_HBM_VIEW ? -1 : 0;  // headless: the menu closes at once
#endif
}

// Test hooks for hidden runs; the game performs them with its own calls each scene frame.
//   GALAXY_TEST_GALAXY_TOUR=<file of "Stage scenario" lines>: each stop in turn, GALAXY_TEST_GALAXY_TOUR_FRAMES scene
//     frames apart (default 1200), through MR::requestChangeStageInGameMoving.
//   GALAXY_TEST_TELEPORT=x,y,z,frame[/x,y,z,frame...]: MR::setPlayerPos at those scene frames, held 30 frames (the
//     player settles there); counted from the latest tour stop when touring.
//   GALAXY_TEST_EVENTS=kind@frame[/kind@frame...]: once at that scene frame (from the latest stop when touring): kill (a
//     miss: MR::forceKillPlayerByAbyss), movieN (MR::startMovie(N), in a stage with a movie sequence), wait (nothing:
//     keeps a tour's last stop running until then), star / grandstar (the stop's star is collected: its result, the get
//     demo, the save and what follows, the ending after KoopaBattleVs3Galaxy).
//   GALAXY_TEST_GRAND_STARS=n: the save holds Grand Stars 1-n from the second scene frame, and the stage starts again
//     with them (the Observatory's domes restored, lit and furnished as the game shows them for that many).
//   GALAXY_TEST_EVENT_FLAGS=name[,name=value...]: the save's stored event flags on and event values set from the second
//     scene frame, before the Grand Stars (with GALAXY_TEST_GRAND_STARS>=3, LibraryOpenNewStarCount=0 opens the Library
//     and its first storybook chapter; PictureBookAlreadyRead=n: chapters 1-n read, which the Library's book lists;
//     SpinAndStarPointer: the spin and pointer the Gateway's Luma grants; Achievement:<id>: that achievement's pop-up; AchievementBoard: the
//     Observatory's achievement board).
//   GALAXY_TEST_EVENTS actors@frame: the live actors with a model, by model name and position (where to teleport).
std::atomic<bool> gTourDone{false};

// GALAXY_STATE_HASH=<file>: every GALAXY_STATE_HASH_EVERY scene frames (60), a line "<scene frame> <hash>" of the
// game's memory (each region, FNV-1a over 64-bit words), and with GALAXY_STATE_HASH_CHUNKS=1 a hash per megabyte
// after it. Two runs with the same input that write the same lines computed the same game (netplay's determinism).
// Scene frames so far (the state hash's and the test actions' count; a rollback takes it back with the game).
uint64_t gSceneFrame = 0;

void write_state_hash(uint64_t scene_frame) {
    static FILE* file = [] {
        const char* path = std::getenv("GALAXY_STATE_HASH");
        FILE* opened = nullptr;
        if (path != nullptr && path[0] != '\0') (void)fopen_s(&opened, path, "w");
        return opened;
    }();
    // Online, the two PCs compare this hash every galaxy::net::kHashEvery scene frames (a desync is logged).
    const bool net_due = galaxy::net::session().active() && galaxy::net::session().settings().rollback == 0 &&
                         scene_frame % galaxy::net::kHashEvery == 0;
    if (file == nullptr && !net_due) return;
    static const uint64_t every = [] {
        const char* value = std::getenv("GALAXY_STATE_HASH_EVERY");
        const uint64_t parsed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0;
        return parsed != 0 ? parsed : 60ull;
    }();
    static const bool chunks = [] {
        const char* value = std::getenv("GALAXY_STATE_HASH_CHUNKS");
        return value != nullptr && value[0] == '1';
    }();
    if (scene_frame % every != 0 && !net_due) return;
    ensure_guest_memory();
    while (gDiscInFlight.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    // GALAXY_STATE_DUMP=<scene frame>:<file>: the readable memory regions at that hash, raw (to diff two runs).
    static const char* const dump = std::getenv("GALAXY_STATE_DUMP");
    if (dump != nullptr && std::strtoull(dump, nullptr, 10) == scene_frame && std::strchr(dump, ':') != nullptr) {
        FILE* out = nullptr;
        if (fopen_s(&out, std::strchr(dump, ':') + 1, "wb") == 0 && out != nullptr) {
            for (const galaxy::GuestMemoryRegion& region : gRegions) {
                for (size_t at = 0; at < region.size; at += 4096) {
                    MEMORY_BASIC_INFORMATION info{};
                    const bool ok = VirtualQuery(region.host_base + at, &info, sizeof(info)) != 0 &&
                                    info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
                                    (info.Protect & (PAGE_READONLY | PAGE_READWRITE)) != 0;
                    static const std::byte zeros[4096]{};
                    std::fwrite(ok ? region.host_base + at : zeros, 1, std::min<size_t>(4096, region.size - at), out);
                }
            }
            std::fclose(out);
        }
    }
    // MEM1 and MEM2 once each (not their mirrors), without what is this process's alone: the native stacks (the boot
    // stack below MEM1's arena, the stack pool at MEM2's top: dead frames hold the host runtime's values) and words that
    // look like host addresses outside the game's fixed memory and module (host buffers some game objects point to).
    auto host_address = [](uint64_t word) {
        return word >= 0x0000010000000000ull && word < 0x00007FF000000000ull &&
               !(word >= 0x00007D0000000000ull && word < 0x00007E0100000000ull);
    };
    uint64_t total = 1469598103934665603ull;
    std::string chunk_text;
    for (const galaxy::GuestMemoryRegion& region : gRegions) {
        if (region.guest_base != 0x00000000u && region.guest_base != 0x10000000u) continue;
        const auto* words = reinterpret_cast<const uint64_t*>(region.host_base);
        const size_t count = region.size / sizeof(uint64_t);
        size_t skip_from = count;
        size_t skip_to = count;
        if (region.guest_base == 0x00000000u) {
            skip_from = 0x80000u / 8u;
            skip_to = 0x100000u / 8u;
        } else if (region.size > 0x2000000u) {
            skip_from = (region.size - 0x2000000u) / 8u;
            skip_to = count;
        }
        constexpr size_t kChunkWords = (1u << 20) / sizeof(uint64_t);
        for (size_t start = 0; start < count; start += kChunkWords) {
            uint64_t hash = 1469598103934665603ull;
            const size_t end = std::min(count, start + kChunkWords);
            size_t i = start;
            while (i < end) {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(words + i, &info, sizeof(info)) == 0) break;
                const auto* region_end = reinterpret_cast<const uint64_t*>(
                    static_cast<const std::byte*>(info.BaseAddress) + info.RegionSize);
                const size_t stop = std::min(end, static_cast<size_t>(region_end - words));
                const bool readable = info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0 &&
                                      (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                                       PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
                for (; i < stop; ++i) {
                    uint64_t word = readable && (i < skip_from || i >= skip_to) ? words[i] : 0ull;
                    if (host_address(word)) word = 0;
                    hash = (hash ^ word) * 1099511628211ull;
                }
            }
            total = (total ^ hash) * 1099511628211ull;
            if (chunks) {
                char text[24];
                std::snprintf(text, sizeof(text), " %08llx", static_cast<unsigned long long>(hash & 0xFFFFFFFFull));
                chunk_text += text;
            }
        }
    }
    if (net_due) {
        galaxy::net::session().report_hash(static_cast<uint32_t>(scene_frame), total);
    }
    if (file != nullptr && scene_frame % every == 0) {
        std::fprintf(file, "%llu %016llx%s\n", static_cast<unsigned long long>(scene_frame),
                     static_cast<unsigned long long>(total), chunk_text.c_str());
        std::fflush(file);
    }
}

// Deterministic mode, at each retrace boundary: keep it, or go back to one kept (sdk/os/snapshot.c). The host's own part
// of the game's state goes with it: the DSP program's (it writes the game's memory) and the scene frame count the
// tests and the state hash use. GALAXY_SNAPSHOT_TEST="<save>,<restore>": keeps boundary <save> and, the first time
// boundary <restore> comes, goes back to it; the frames between run twice (the state hash shows whether the second
// run matched the first).
std::map<uint32_t, uint64_t> gSavedSceneFrames;
struct TestRunState;
void keep_test_run(uint32_t frame);
void restore_test_run(uint32_t frame);

// What the game told the host about its remotes and pointer (services), which the next frames' reports follow: kept
// and restored with the game, as the console's remotes would have been told again.
struct HostInputState {
    uint32_t report[2];
    int dpd;
    bool motor;
    int32_t pointer_mode;
    uint32_t tilt_section;
    int wpad_started;
    uint32_t wpad_start_retrace;
    bool connected;
    bool partner_connected;

    static HostInputState take() {
        return {{gWpadReport[0].load(), gWpadReport[1].load()}, gWpadDpd.load(), gMotor.load(), gStarPointerMode.load(),
                gTiltSection.load(), gWpadStarted.load(), gWpadStartRetrace.load(), gWpadConnected, gPartnerConnected};
    }
    void put() const {
        gWpadReport[0] = report[0];
        gWpadReport[1] = report[1];
        gWpadDpd = dpd;
        gMotor = motor;
        gStarPointerMode = pointer_mode;
        gTiltSection = tilt_section;
        gWpadStarted = wpad_started;
        gWpadStartRetrace = wpad_start_retrace;
        gWpadConnected = connected;
        gPartnerConnected = partner_connected;
    }
};
std::map<uint32_t, HostInputState> gSavedInputState;

uint32_t svc_det_boundary(void*, uint32_t frame, uint32_t* arg) {
    static const std::pair<uint32_t, uint32_t> test = [] {
        const char* value = std::getenv("GALAXY_SNAPSHOT_TEST");
        unsigned save = 0, restore = 0;
        if (value == nullptr || sscanf_s(value, "%u,%u", &save, &restore) != 2 || restore <= save) return std::pair{0u, 0u};
        return std::pair{save, restore};
    }();
    static uint32_t last_saved = 0;
    static bool test_restored = false;
    auto save = [&] {
        last_saved = frame;
        gSavedSceneFrames[frame] = gSceneFrame;
        while (gSavedSceneFrames.size() > 32) gSavedSceneFrames.erase(gSavedSceneFrames.begin());
        gSavedInputState[frame] = HostInputState::take();
        keep_test_run(frame);
        while (gSavedInputState.size() > 32) gSavedInputState.erase(gSavedInputState.begin());
        if (nand_in_memory()) {
            gMemNandSaved[frame] = gMemNand;
            while (gMemNandSaved.size() > 32) gMemNandSaved.erase(gMemNandSaved.begin());
        }
#if defined(NEBULA_HOST_RENDERER)
        gAudio->save_dsp(frame);
        // The renderer's registers as of every command so far (those waiting for a PE marker or the retrace first),
        // and its writes to the game's memory (EFB copies) done, so that the snapshot has them.
        if (gPendingSize != 0) {
            submit_frame(false, galaxy::gx::kNoDisplayedXfbAddr);
        }
        galaxy::gx::stream_checkpoint(frame);
        galaxy::gx::wait_for_render_idle();
#endif
        return static_cast<uint32_t>(NEBULA_DET_SAVE);
    };
    auto restore = [&](uint32_t to) {
#if defined(NEBULA_HOST_RENDERER)
        // Commands of the undone frames not yet given to the renderer: dropped; the renderer's registers back, once
        // it has written what the undone frames had it write (the restore then takes the game's memory back).
        if (gPendingSize != 0) {
            gModule.gx_release(gPendingSize);
            gPendingSize = 0;
        }
        galaxy::gx::wait_for_render_idle();
        galaxy::gx::stream_rewind(to);
        if (!gAudio->restore_dsp(to)) {
            std::fprintf(stderr, "[rollback] the DSP state of frame %u is gone\n", to);
        }
#endif
        if (const auto saved = gSavedSceneFrames.find(to); saved != gSavedSceneFrames.end()) {
            gSceneFrame = saved->second;
        }
        gSavedSceneFrames.erase(gSavedSceneFrames.upper_bound(to), gSavedSceneFrames.end());
        if (const auto input = gSavedInputState.find(to); input != gSavedInputState.end()) {
            input->second.put();
        }
        restore_test_run(to);
        gSavedInputState.erase(gSavedInputState.upper_bound(to), gSavedInputState.end());
        if (const auto nand = gMemNandSaved.find(to); nand != gMemNandSaved.end()) {
            gMemNand = nand->second;
        }
        gMemNandSaved.erase(gMemNandSaved.upper_bound(to), gMemNandSaved.end());
        last_saved = to;
        // The game took grant `to` (and has not shown its retrace yet); the clock grants from to + 1, at once up to
        // where it was.
        gCatchUpTo.store(std::max(gCatchUpTo.load(), frame), std::memory_order_release);
        gRetraces.store(to - 1, std::memory_order_release);
        gRewindFrame.store(to, std::memory_order_release);
        gRewindPending.store(true, std::memory_order_release);
        *arg = to;
        std::fprintf(stderr, "[rollback] back from frame %u to %u\n", frame, to);
        return static_cast<uint32_t>(NEBULA_DET_RESTORE);
    };
    // Rollback netplay: a confirmed input that differs from the one its frame ran with sends the game back to the
    // boundary before that frame; else every boundary is kept.
    if (galaxy::net::session().active() && galaxy::net::session().settings().rollback != 0) {
        if (const uint32_t granted = gGranted.load(); granted != frame) {
            static uint32_t reported = 0;
            if (reported++ < 20) std::fprintf(stderr, "[rollback] boundary %u while %u granted\n", frame, granted);
        }
        uint32_t mismatch = 0;
        {
            std::lock_guard<std::mutex> lock(gRollbackLock);
            gUsedRemote.erase(gUsedRemote.begin(), gUsedRemote.lower_bound(frame > 64 ? frame - 64 : 0));
            for (auto& [used_frame, used] : gUsedRemote) {
                if (used.confirmed || used_frame > frame) continue;
                galaxy::net::FrameInput real;
                if (!galaxy::net::session().remote_input(used_frame, real)) break;
                if (std::memcmp(&real.snapshot, &used.snapshot, sizeof(real.snapshot)) != 0) {
                    mismatch = used_frame;
                    break;
                }
                used.confirmed = true;
            }
            if (mismatch != 0) {
                gUsedRemote.erase(gUsedRemote.lower_bound(mismatch), gUsedRemote.end());
            }
        }
        if (mismatch == 0 && nand_in_memory()) {
            uint32_t safe = frame;
            {
                std::lock_guard<std::mutex> lock(gRollbackLock);
                for (const auto& [used_frame, used] : gUsedRemote) {
                    if (!used.confirmed && used_frame <= frame) {
                        safe = used_frame - 1;
                        break;
                    }
                }
            }
            static uint32_t flushed = 0;
            auto kept = gMemNandSaved.upper_bound(safe);
            if (kept != gMemNandSaved.begin() && (--kept)->first != flushed) {
                flushed = kept->first;
                nand_flush(kept->second.entries);
            }
        }
        if (mismatch != 0 && gSavedSceneFrames.count(mismatch - 1) != 0) {
            static uint32_t rollbacks = 0;
            ++gRollbackStats.rollbacks;
            if (++rollbacks % 60 == 1) {
                std::fprintf(stderr, "[netplay] rollback %u: frame %u ran with a predicted input\n", rollbacks, mismatch);
            }
            return restore(mismatch - 1);
        }
        if (last_saved != frame) return save();
        return NEBULA_DET_NONE;
    }
    if (test.first != 0) {
        if (frame == test.first && last_saved != frame) return save();
        if (frame == test.second && !test_restored) {
            test_restored = true;
            return restore(test.first);
        }
    }
    return NEBULA_DET_NONE;
}

// The test scripts' progress (svc_test_scene_frame): game state as far as a rollback goes (svc_det_boundary).
struct TestRunState {
    size_t next = 0;
    uint64_t scene_frames = 0, since_stop = 0, stop_frame = 0;
    size_t event_flags_taken = 0;
    bool grand_stars_set = false;
};
TestRunState gTestRun;
std::map<uint32_t, TestRunState> gSavedTestRun;

void keep_test_run(uint32_t frame) {
    gSavedTestRun[frame] = gTestRun;
    while (gSavedTestRun.size() > 32) gSavedTestRun.erase(gSavedTestRun.begin());
}

void restore_test_run(uint32_t frame) {
    if (const auto kept = gSavedTestRun.find(frame); kept != gSavedTestRun.end()) gTestRun = kept->second;
    gSavedTestRun.erase(gSavedTestRun.upper_bound(frame), gSavedTestRun.end());
}

int svc_test_scene_frame(void*, NebulaTestAction* action) {
    write_state_hash(++gSceneFrame);
    struct Stop {
        std::string stage;
        int32_t scenario;
    };
    struct Teleport {
        float x, y, z;
        uint64_t frame;
    };
    static std::vector<Stop> stops;
    static std::vector<Teleport> teleports;
    struct Event {
        uint32_t kind;
        uint64_t frame;
        int32_t value;
    };
    static std::vector<Event> events;
    static uint64_t tour_frames = 1200;
    static bool touring = false;
    static bool loaded = false;
    size_t& next = gTestRun.next;
    uint64_t& scene_frames = gTestRun.scene_frames;
    uint64_t& since_stop = gTestRun.since_stop;
    uint64_t& stop_frame = gTestRun.stop_frame;
    if (!loaded) {
        loaded = true;
        if (const char* tour = std::getenv("GALAXY_TEST_GALAXY_TOUR")) {
            if (FILE* file = std::fopen(tour, "r")) {
                char stage[64];
                int scenario = 0;
                while (std::fscanf(file, "%63s %d", stage, &scenario) == 2) {
                    stops.push_back({stage, scenario});
                }
                std::fclose(file);
            }
            touring = true;
            if (const char* frames = std::getenv("GALAXY_TEST_GALAXY_TOUR_FRAMES")) {
                tour_frames = std::max<uint64_t>(std::strtoull(frames, nullptr, 10), 60u);
            }
            char text[64];
            std::snprintf(text, sizeof(text), "stops %zu, %llu frames apart", stops.size(),
                          static_cast<unsigned long long>(tour_frames));
            log_line("tour:", text);
        }
        if (const char* list = std::getenv("GALAXY_TEST_EVENTS")) {
            std::string entries = list;
            size_t start = 0;
            while (start < entries.size()) {
                const size_t end = std::min(entries.find('/', start), entries.size());
                const std::string entry = entries.substr(start, end - start);
                const size_t at = entry.find('@');
                if (at != std::string::npos) {
                    const std::string kind = entry.substr(0, at);
                    const uint64_t frame = std::strtoull(entry.c_str() + at + 1, nullptr, 10);
                    if (kind == "kill") {
                        events.push_back({NEBULA_TEST_KILL_PLAYER, frame, 0});
                    } else if (kind == "star") {
                        events.push_back({NEBULA_TEST_GET_STAR, frame, -1});
                    } else if (kind == "grandstar") {
                        events.push_back({NEBULA_TEST_GET_GRAND_STAR, frame, -1});
                    } else if (kind == "mario" || kind == "luigi") {
                        events.push_back({NEBULA_TEST_SET_PLAYER, frame, kind == "luigi" ? 1 : 0});
                    } else if (kind == "actors") {
                        events.push_back({NEBULA_TEST_LIST_ACTORS, frame, 0});
                    } else if (kind == "wait") {
                        events.push_back({0, frame, 0});
                    } else if (kind.rfind("movie", 0) == 0) {
                        events.push_back({NEBULA_TEST_START_MOVIE, frame, std::atoi(kind.c_str() + 5)});
                    }
                }
                start = end + 1;
            }
        }
        if (const char* list = std::getenv("GALAXY_TEST_TELEPORT")) {
            std::string entries = list;
            size_t start = 0;
            while (start < entries.size()) {
                const size_t end = std::min(entries.find('/', start), entries.size());
                Teleport t{};
                unsigned long long frame = 0;
                if (std::sscanf(entries.substr(start, end - start).c_str(), "%f,%f,%f,%llu", &t.x, &t.y, &t.z,
                                &frame) == 4) {
                    t.frame = frame;
                    teleports.push_back(t);
                }
                start = end + 1;
            }
        }
    }
    ++scene_frames;
    static std::vector<std::string> event_flags = [] {
        std::vector<std::string> names;
        if (const char* value = std::getenv("GALAXY_TEST_EVENT_FLAGS")) {
            std::string list = value;
            for (std::size_t start = 0; start < list.size();) {
                const std::size_t comma = std::min(list.find(',', start), list.size());
                if (comma > start) names.push_back(list.substr(start, comma - start));
                start = comma + 1;
            }
        }
        return names;
    }();
    if (gTestRun.event_flags_taken < event_flags.size() && scene_frames >= 2u) {
        action->kind = NEBULA_TEST_SET_EVENT_FLAG;
        std::snprintf(action->stage, sizeof(action->stage), "%s", event_flags[gTestRun.event_flags_taken++].c_str());
        log_line("test: event flag", action->stage);
        return 1;
    }
    static const int32_t grand_stars = [] {
        const char* value = std::getenv("GALAXY_TEST_GRAND_STARS");
        return value != nullptr ? std::atoi(value) : 0;
    }();
    bool& grand_stars_set = gTestRun.grand_stars_set;
    if (grand_stars > 0 && !grand_stars_set && scene_frames >= 2u) {
        grand_stars_set = true;
        action->kind = NEBULA_TEST_SET_GRAND_STARS;
        action->scenario = grand_stars;
        log_line("test:", "grand stars set");
        return 1;
    }
    // The last stop stays for its frames, or until 20 s after the last test event, then the run ends.
    static const uint64_t last_frame = [] {
        uint64_t last = 0;
        for (const Event& event : events) {
            last = std::max(last, event.frame + 1200u);
        }
        for (const Teleport& t : teleports) {
            last = std::max(last, t.frame + 1200u);
        }
        return last;
    }();
    if (touring && next == stops.size() && !gTourDone.load(std::memory_order_relaxed) &&
        ++since_stop >= std::max(tour_frames, last_frame)) {
        gTourDone.store(true, std::memory_order_relaxed);  // the run ends once the last stop has had its frames
        log_line("tour:", "done");
    }
    if (touring && next < stops.size() && ++since_stop >= tour_frames) {
        since_stop = 0;
        stop_frame = scene_frames;
        const Stop& stop = stops[next++];
        action->kind = NEBULA_TEST_CHANGE_STAGE;
        action->scenario = stop.scenario;
        std::snprintf(action->stage, sizeof(action->stage), "%s", stop.stage.c_str());
        char text[128];
        std::snprintf(text, sizeof(text), "%zu/%zu %s %d", next, stops.size(), stop.stage.c_str(), stop.scenario);
        log_line("tour:", text);
        return 1;
    }
    if (touring && stop_frame == 0) {
        return 0;
    }
    const uint64_t frame = scene_frames - stop_frame;
    for (const Event& event : events) {
        if (frame == event.frame && event.kind != 0) {
            action->kind = event.kind;
            action->scenario = event.value >= 0 ? event.value : (next > 0 ? stops[next - 1].scenario : 1);
            char text[64];
            std::snprintf(text, sizeof(text), "event %u at frame %llu", event.kind, static_cast<unsigned long long>(frame));
            log_line("test:", text);
            return 1;
        }
    }
    for (const Teleport& t : teleports) {
        if (frame >= t.frame && frame < t.frame + 30u) {
            action->kind = NEBULA_TEST_PLACE_PLAYER;
            action->position[0] = t.x;
            action->position[1] = t.y;
            action->position[2] = t.z;
            return 1;
        }
    }
    return 0;
}

// A restart starts this executable again with the same arguments (the game's boot, as the console's reset); the
// other power events close the game.
[[noreturn]] void svc_power(void*, uint32_t what) {
    log_line("host:", what == NEBULA_POWER_RESTART ? "restart" : "the game ended");
#if defined(NEBULA_HOST_RENDERER)
    galaxy::achievements::flush();
    galaxy::stats::flush();
#endif
    std::fflush(stdout);
#if defined(_WIN32)
    if (what == NEBULA_POWER_RESTART) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0) {
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            std::wstring command = GetCommandLineW();
            if (CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }
    }
#endif
    std::_Exit(0);
}

#if defined(NEBULA_HOST_RENDERER)
// The physical address of a host pointer into the game's memory (0 outside it).
uint32_t physical_of(const void* pointer) {
    const auto* p = static_cast<const std::byte*>(pointer);
    for (const galaxy::GuestMemoryRegion& region : gRegions) {
        if (region.guest_base < 0x80000000u && p >= region.host_base && p < region.host_base + region.size) {
            return region.guest_base + static_cast<uint32_t>(p - region.host_base);
        }
    }
    return 0;
}

// A THP frame's video component is one baseline JPEG whose SOF0 gives the movie's size; the decoder writes its planes
// in GX's I8 tiling. The planes are then the renderer's to re-read, as after the console's decode.
int32_t svc_thp_video_decode(void*, const void* frame, uint32_t size, void* y, void* u, void* v) {
    gMovieFrames.fetch_add(1, std::memory_order_relaxed);
    if (galaxy::gx::movie_replacement_showing()) {
        // A replacement video's frames show in place of these planes: their decode would be unseen work.
        gMovieSkipped.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    const auto decode_start = std::chrono::steady_clock::now();
    struct DecodeTime {
        std::chrono::steady_clock::time_point start;
        ~DecodeTime() {
            gMovieDecodeNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start).count()),
                                     std::memory_order_relaxed);
        }
    } decode_time{decode_start};
    ensure_guest_memory();
    const auto* p = static_cast<const uint8_t*>(frame);
    if (size < 4 || p[0] != 0xFF || p[1] != 0xD8) {
        return -1;
    }
    uint16_t width = 0, height = 0;
    for (size_t i = 2; i + 9 <= size && p[i] == 0xFF && p[i + 1] != 0xDA;
         i += 2 + ((size_t(p[i + 2]) << 8) | p[i + 3])) {
        if (p[i + 1] == 0xC0) {
            height = static_cast<uint16_t>((p[i + 5] << 8) | p[i + 6]);
            width = static_cast<uint16_t>((p[i + 7] << 8) | p[i + 8]);
        }
    }
    if (width == 0 || height == 0) {
        return -1;
    }
    const size_t end = size;
    const auto plane = [](size_t w, size_t h) { return ((w + 7) / 8) * ((h + 3) / 4) * 32; };
    const size_t luma = plane(width, height);
    const size_t chroma = plane((width + 1u) / 2u, (height + 1u) / 2u);
    const int32_t result = galaxy::thp::decode_video(
        {p, end}, width, height, {static_cast<uint8_t*>(y), luma}, {static_cast<uint8_t*>(u), chroma},
        {static_cast<uint8_t*>(v), chroma});
    for (const auto& [plane_pointer, size] : {std::pair{y, luma}, std::pair{u, chroma}, std::pair{v, chroma}}) {
        if (const uint32_t physical = physical_of(plane_pointer)) {
            galaxy::gx::notify_guest_memory_write(physical, static_cast<uint32_t>(size));
        }
    }
    return result;
}
#else
int32_t svc_thp_video_decode(void*, const void*, uint32_t, void*, void*, void*) {
    return 0;
}
#endif

}  // namespace

#if defined(NEBULA_HOST_RENDERER)
// The renderer settings read from the environment, at their shipped values (installer/runtime-env.json) where the
// environment does not set them.
void apply_shipped_renderer_defaults() {
    static const struct {
        const char* name;
        const char* value;
    } kDefaults[] = {
        {"GALAXY_ALLOW_TEARING", "0"},
        {"GALAXY_ASYNC_RENDER_THREAD", "1"},
        {"GALAXY_CAPTURE_MOUSE_BUTTONS", "1"},
        {"GALAXY_D3D_DEBUG", "0"},
        {"GALAXY_DISPLAY_LIST_DIRTY_INVALIDATE", "0"},
        {"GALAXY_FLIP_DISCARD", "1"},
        {"GALAXY_FRAME_RATE_DECOUPLE", "0"},
        {"GALAXY_FRAME_TELEMETRY", "1"},
        {"GALAXY_GPU_TIMESTAMPS", "1"},
        {"GALAXY_GX_DECODED_VERTEX_CACHE", "1"},
        {"GALAXY_GX_FRAME_TIMING_SAMPLE", "0"},
        {"GALAXY_GX_PSO_CYCLES", "0"},
        {"GALAXY_GX_SORTED_VERTEX_INVALIDATION", "1"},
        {"GALAXY_GX_UBER", "1"},
        {"GALAXY_HIDE_OS_CURSOR", "1"},
        {"GALAXY_MOUSE_IR_CURSOR_POLL", "auto"},
        {"GALAXY_MOUSE_IR_MESSAGE_RECENT_MS", "120"},
        {"GALAXY_NATIVE_BLOOM_WORKSPACE", "1"},
        {"GALAXY_PE_EVENTS_GPU_FENCE", "1"},
        {"GALAXY_PSO_PREWARM", "1"},
        {"GALAXY_PSO_PREWARM_BLOCKING", "1"},
        {"GALAXY_PSO_PREWARM_BLOCKING_MS", "30000"},
        {"GALAXY_PSO_PREWARM_LIMIT", "4096"},
        {"GALAXY_REALTIME_VI", "1"},
        {"GALAXY_RENDER_IMMUTABLE_RANGE_OWNERSHIP", "0"},
        {"GALAXY_RENDER_LIVE_MEMORY", "1"},
        {"GALAXY_RENDER_LIVE_MEMORY_WAIT", "0"},
        {"GALAXY_RENDER_MEMORY_SNAPSHOT", "0"},
        {"GALAXY_RENDER_MEMORY_SNAPSHOT_COMPACT", "1"},
        {"GALAXY_RENDER_QUEUE_DEPTH", "2"},
        {"GALAXY_RENDER_WORKER_MEMORY_SNAPSHOT", "0"},
        {"GALAXY_SKIP_DUPLICATE_XFB_PRESENTS", "1"},
        {"GALAXY_SMOOTH_REALTIME_PRESENT", "1"},
        {"GALAXY_SMOOTH_REALTIME_PRESENT_MAX_WAIT_US", "2000"},
        {"GALAXY_SWAP_CHAIN_FRAME_LATENCY", "1"},
        {"GALAXY_VI_RENDER_FIFO_WAIT", "0"},
        {"GALAXY_WAIT_FOR_FRAME_LATENCY", "0"},
        {"GALAXY_XFB_MAX_PRESERVE_AGE", "30"},
    };
    for (const auto& setting : kDefaults) {
        size_t length = 0;
        if (getenv_s(&length, nullptr, 0, setting.name) != 0 || length == 0) {
            _putenv_s(setting.name, setting.value);
        }
    }
}
#endif

#if defined(NEBULA_HOST_RENDERER)
#include "galaxy/mod_import.h"

namespace {

std::string json_text(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20u) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += escaped;
        } else {
            out.push_back(c);
        }
    }
    return out + "\"";
}

std::string utf8_of(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

// The game id and main.dol from Setup's game.pak, read without mapping it; false (and nothing changed) for a file that
// is not one: the tools then work without it.
bool read_game_dol(const fs::path& path, std::string& id, std::vector<std::uint8_t>& dol) {
    std::ifstream file(path, std::ios::binary);
    std::uint8_t boot[0x440];
    if (!file.read(reinterpret_cast<char*>(boot), sizeof(boot))) return false;
    const std::uint64_t offset = (std::uint64_t)load_be32(boot + 0x420) << 2;
    std::uint8_t header[0x100];
    if (!file.seekg((std::streamoff)offset) || !file.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
    std::uint64_t end = 0x100;
    for (int s = 0; s < 18; s++) {
        const std::uint32_t size = load_be32(header + 0x90 + s * 4);
        const std::uint64_t section_end = (std::uint64_t)load_be32(header + s * 4) + size;
        if (size != 0 && section_end > end) end = section_end;
    }
    // main.dol is a few megabytes; a larger one is not a game.pak.
    if (end > (64u << 20)) return false;
    std::vector<std::uint8_t> bytes((std::size_t)end);
    if (!file.seekg((std::streamoff)offset) || !file.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)end)) {
        return false;
    }
    id.assign(reinterpret_cast<const char*>(boot), 6);
    dol = std::move(bytes);
    return true;
}

// The launcher's mod tools: NebulaNative --import-mod <folder or archive>..., --remove-mod <id> or --list-mods
// [--game <id>] [--pak <game.pak>], on the mods folder beside the settings (as the game uses it). One JSON line on
// stdout; exit code 0 when everything succeeded.
int run_mods_tool() {
    std::vector<fs::path> args;
#if defined(_WIN32)
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) args.emplace_back(wide[i]);
        LocalFree(wide);
    }
#endif
    std::string game_id = "RMGE01";
    std::vector<fs::path> sources;
    const bool list = !args.empty() && args[0] == "--list-mods";
    const bool remove = !args.empty() && args[0] == "--remove-mod";
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--game" && i + 1u < args.size()) {
            game_id = utf8_of(args[++i]);
        } else if (args[i] == "--pak" && i + 1u < args.size()) {
            // The game's own disc: its id and main.dol (another release's patches are then told apart).
            std::string id;
            std::vector<std::uint8_t> dol;
            if (read_game_dol(args[++i], id, dol)) {
                game_id = id;
                galaxy::mods::set_game_dol(dol);
            }
        } else {
            sources.push_back(args[i]);
        }
    }
    const fs::path settings_folder = galaxy::runtime_settings_folder();
    if (settings_folder.empty()) {
        std::printf("{\"ok\":false,\"message\":\"no settings folder\"}\n");
        return 2;
    }
    std::error_code error;
    fs::create_directories(settings_folder / "mods", error);
    galaxy::mods::set_mods_folder(settings_folder / "mods", settings_folder / "mods.ini", game_id);
    if (remove) {
        std::string message;
        const bool ok = !sources.empty() &&
                        galaxy::mods::remove_mod(settings_folder / "mods", utf8_of(sources.front()), &message);
        std::printf("{\"ok\":%s,\"message\":%s}\n", ok ? "true" : "false", json_text(message).c_str());
        return ok ? 0 : 1;
    }
    if (list) {
        std::string out = "{\"ok\":true,\"folder\":" + json_text(utf8_of(settings_folder / "mods")) + ",\"mods\":[";
        bool first = true;
        for (const galaxy::mods::ModInfo& mod : galaxy::mods::scan_mods(game_id)) {
            out += first ? "{" : ",{";
            first = false;
            out += "\"id\":" + json_text(mod.id) + ",\"enabled\":" + (mod.enabled ? "true" : "false") +
                   ",\"order\":" + std::to_string(mod.order) + ",\"riivolution\":" + (mod.riivolution ? "true" : "false") +
                   ",\"files\":" + ((mod.files || mod.loose) ? "true" : "false") + ",\"textures\":" +
                   (mod.textures ? "true" : "false") + ",\"movies\":" + (mod.movies ? "true" : "false") +
                   ",\"code_patches\":" + std::to_string(mod.code_patches) + ",\"native_patches\":" +
                   std::to_string(mod.native_patches) + ",\"problem\":" + json_text(mod.problem) + ",\"languages\":[";
            for (std::size_t l = 0; l < mod.languages.size(); ++l) out += (l ? "," : "") + json_text(mod.languages[l]);
            out += "],\"options\":[";
            for (std::size_t o = 0; o < mod.options.size(); ++o) {
                const galaxy::mods::ModOption& option = mod.options[o];
                out += std::string(o ? "," : "") + "{\"key\":" + json_text(option.key) + ",\"section\":" +
                       json_text(option.section) + ",\"name\":" + json_text(option.name) +
                       ",\"selected\":" + std::to_string(option.selected) + ",\"choices\":[";
                for (std::size_t c = 0; c < option.choices.size(); ++c) {
                    const bool other = c < option.other_region.size() && option.other_region[c];
                    out += std::string(c ? "," : "") + "{\"name\":" + json_text(option.choices[c]) +
                           ",\"other_region\":" + (other ? "true" : "false") + "}";
                }
                out += "]}";
            }
            out += "],\"overlaps\":[";
            for (std::size_t o = 0; o < mod.overlaps.size(); ++o) {
                const galaxy::mods::ModOverlap& overlap = mod.overlaps[o];
                out += std::string(o ? "," : "") + "{\"other\":" + json_text(overlap.other) + ",\"above\":" +
                       (overlap.other_above ? "true" : "false") + ",\"files\":" + std::to_string(overlap.files) +
                       ",\"textures\":" + std::to_string(overlap.textures) + "}";
            }
            out += "]}";
        }
        out += "]}";
        std::printf("%s\n", out.c_str());
        return 0;
    }
    int failed = 0;
    std::string out = "{\"results\":[";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const galaxy::mods::ImportResult result = galaxy::mods::import_mod(sources[i], settings_folder / "mods");
        failed += result.ok ? 0 : 1;
        out += std::string(i ? "," : "") + "{\"ok\":" + (result.ok ? "true" : "false") + ",\"id\":" + json_text(result.id) +
               ",\"replaced\":" + (result.replaced ? "true" : "false") + ",\"files\":" + std::to_string(result.files) +
               ",\"bytes\":" + std::to_string(result.bytes) + ",\"kinds\":" + json_text(result.kinds) +
               ",\"message\":" + json_text(result.message) + "}";
    }
    out += "],\"ok\":" + std::string(failed == 0 && !sources.empty() ? "true" : "false") + "}";
    std::printf("%s\n", out.c_str());
    return failed == 0 && !sources.empty() ? 0 : 1;
}

}  // namespace
#endif

#if defined(NEBULA_HOST_RENDERER)
// A file's contents as a short digest (FNV-1a): netplay's build check.
std::string file_digest(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    uint64_t hash = 1469598103934665603ull;
    char buffer[1 << 16];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            hash = (hash ^ static_cast<uint8_t>(buffer[i])) * 1099511628211ull;
        }
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

// Online co-op (net_lockstep.h), before the game boots: GALAXY_NET_HOST=<port> hosts (this PC plays Mario and its
// save), GALAXY_NET_JOIN=<address>[:port] joins (this PC plays Luigi in the host's save, kept under the settings
// folder's netplay\<host>). GALAXY_NET_DELAY: the input delay in frames (3; the host's counts).
void start_netplay(const fs::path& module) {
    const char* host_port = std::getenv("GALAXY_NET_HOST");
    const char* join_address = std::getenv("GALAXY_NET_JOIN");
    if (host_port == nullptr && join_address == nullptr) {
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::string build_id = file_digest(exe) + "-" + file_digest(module);
    std::string mods;
    for (const galaxy::mods::ModInfo& mod : galaxy::mods::session_mods().started_with) {
        if (!mod.enabled) continue;
        mods += (mods.empty() ? "" : ", ") + mod.id;
    }
    std::string error;
    if (host_port != nullptr) {
        galaxy::net::SessionSettings settings;
        // The calendar now, as seconds since 2000-01-01.
        settings.calendar = static_cast<uint32_t>(std::time(nullptr) - 946684800);
        settings.coop = 1;
        settings.moveset = svc_option(nullptr, NEBULA_OPTION_MOVESET);
        settings.motion_style = svc_option(nullptr, NEBULA_OPTION_MOTION_STYLE);
        settings.text_speed = svc_option(nullptr, NEBULA_OPTION_TEXT_SPEED);
        settings.language = static_cast<uint32_t>(galaxy::game_language_for_boot());
        const char* delay = std::getenv("GALAXY_NET_DELAY");
        settings.delay = std::clamp(delay != nullptr ? static_cast<uint32_t>(std::strtoul(delay, nullptr, 10)) : 3u,
                                    1u, 15u);
        // GALAXY_NET_ROLLBACK=1: rollback (each PC runs ahead on the other's predicted input) instead of lockstep.
        const char* rollback = std::getenv("GALAXY_NET_ROLLBACK");
        settings.rollback = rollback != nullptr && rollback[0] == '1' ? 1u : 0u;
        // The save, and the Mii database the game reads at boot.
        std::vector<galaxy::net::SaveFile> save;
        for (const fs::path& part : {fs::path(gSaveDataPath.substr(1)), fs::path("shared2/menu/FaceLib")}) {
            std::error_code walk;
            for (fs::recursive_directory_iterator it(gNandRoot / part, walk), end; it != end; it.increment(walk)) {
                if (walk || !it->is_regular_file()) continue;
                std::ifstream in(it->path(), std::ios::binary);
                galaxy::net::SaveFile file;
                file.path = fs::relative(it->path(), gNandRoot).generic_string();
                file.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                save.push_back(std::move(file));
            }
        }
        if (!galaxy::net::session().host(static_cast<unsigned>(std::strtoul(host_port, nullptr, 10)), build_id, mods,
                                         settings, save, error)) {
            die("online: %s", error.c_str());
        }
        return;
    }
    galaxy::net::SessionSettings settings;
    std::vector<galaxy::net::SaveFile> save;
    if (!galaxy::net::session().join(join_address, build_id, mods, settings, save, error)) {
        die("online: %s", error.c_str());
    }
    if (settings.language != static_cast<uint32_t>(galaxy::game_language_for_boot())) {
        std::fprintf(stderr, "[netplay] the host plays in another language: the game may not stay in step\n");
    }
    // The host's save, apart from this PC's own.
    std::string folder_name = join_address;
    for (char& c : folder_name) {
        if (c == ':' || c == '/' || c == '\\') c = '_';
    }
    const fs::path root = galaxy::runtime_settings_folder() / "netplay" / folder_name;
    std::error_code error_code;
    fs::remove_all(root, error_code);
    for (const galaxy::net::SaveFile& file : save) {
        const fs::path path = root / fs::path(file.path);
        fs::create_directories(path.parent_path(), error_code);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data.data()), static_cast<std::streamsize>(file.data.size()));
    }
    gNandRoot = root;
    fs::create_directories(gNandRoot / gSaveDataPath.substr(1), error_code);
    fs::create_directories(gNandRoot / "tmp", error_code);
    std::fprintf(stderr, "[netplay] playing the host's save from %s\n", root.string().c_str());
}
#endif

int main(int argc, char** argv) {
#if defined(NEBULA_HOST_RENDERER) && defined(_WIN32)
    if (argc >= 3 && std::strcmp(argv[1], "--crash-report") == 0) {
        return show_crash_report_dialog();
    }
#endif
#if defined(NEBULA_HOST_SDL)
    if (!galaxy::sdl::start()) {
        return 1;
    }
#endif
#if defined(NEBULA_HOST_RENDERER)
    apply_shipped_renderer_defaults();
#endif
#if defined(NEBULA_HOST_RENDERER) && defined(_WIN32)
    // GALAXY_FRAME_TIMELINE=1: the frame pipeline's events (frame_timeline.h), written at exit.
    galaxy::timeline::initialize_from_environment();
#endif
#if defined(NEBULA_HOST_RENDERER) && defined(_WIN32)
    // Physical-pixel DPI awareness before any window (sizes and mouse coordinates in pixels), and CRT assertion and
    // error reports to stderr instead of dialogs.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
#endif
#if defined(_WIN32)
    // GALAXY_TEST_CPU_AFFINITY: the logical processors the process may use (hex mask; 0xF = two cores with SMT, a
    // 2C/4T laptop's shape).
    if (const char* mask = std::getenv("GALAXY_TEST_CPU_AFFINITY")) {
        SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(std::strtoull(mask, nullptr, 16)));
    }
    install_crash_reporter();
#endif
#if defined(NEBULA_HOST_RENDERER)
    if (argc >= 2 && (std::strcmp(argv[1], "--import-mod") == 0 || std::strcmp(argv[1], "--list-mods") == 0 ||
                      std::strcmp(argv[1], "--remove-mod") == 0)) {
        return run_mods_tool();
    }
#endif
    fs::path pak, nand, module;
    double seconds = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key == "--pak") {
            pak = argv[i + 1];
        } else if (key == "--nand") {
            nand = argv[i + 1];
        } else if (key == "--module") {
            module = argv[i + 1];
        } else if (key == "--seconds") {
            seconds = std::atof(argv[i + 1]);
        } else {
            die("unknown option %s", key.c_str());
        }
    }
    if (pak.empty() || nand.empty()) {
        die("usage: nebula_native_host --pak <game.pak> --nand <dir> [--module <smg_game.dll>] [--seconds N]");
    }
    if (module.empty()) {
#if defined(_WIN32)
        module = fs::path(argv[0]).parent_path() / "smg_game.dll";
#elif defined(__APPLE__)
        module = fs::path(argv[0]).parent_path() / "libsmg_game.dylib";
#else
        module = fs::path(argv[0]).parent_path() / "libsmg_game.so";
#endif
    }
    gNandRoot = nand;
    std::error_code error;
    fs::create_directories(gNandRoot / "title/00010000/524d4745/data", error);
    fs::create_directories(gNandRoot / "tmp", error);  // where saves stage their banner before moving it
#if defined(NEBULA_HOST_RENDERER)
    // The system's Mii database, which every console has: an empty one when this NAND has none (the game treats a
    // missing database as a NAND error when it saves).
    const fs::path mii_database = gNandRoot / "shared2/menu/FaceLib/RFL_DB.dat";
    if (!fs::exists(mii_database, error)) {
        fs::create_directories(mii_database.parent_path(), error);
        const std::vector<std::byte> bytes = galaxy::host::native_game_empty_mii_database();
        if (FILE* file = std::fopen(mii_database.string().c_str(), "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }
#endif

    open_pak(pak);
    load_module(module);
#if defined(NEBULA_HOST_RENDERER)
    {
        // Mii import: the files in the import folder (next to the settings: any console's Mii files, mii_formats.h)
        // join the database before the game reads it; the settings import more while it runs.
        const fs::path settings_folder = galaxy::runtime_settings_folder();
        const fs::path import_folder = settings_folder.empty() ? fs::path{} : settings_folder / "miis";
        if (!import_folder.empty()) {
            fs::create_directories(import_folder, error);
        }
        galaxy::host::set_mii_library(mii_database, import_folder,
                                      [](galaxy::host::MiiRecord& mii) {
                                          return gModule.mii_prepare(reinterpret_cast<uint8_t*>(mii.data())) != 0u;
                                      });
        std::string message;
        (void)galaxy::host::import_mii_folder(&message);
        std::fprintf(stderr, "[mii] import folder %s: %s (%u in the database)\n", import_folder.string().c_str(),
                     message.c_str(), galaxy::host::mii_library_count());
        (void)galaxy::host::take_mii_installs();  // in the file the game reads at boot
    }
#if defined(NEBULA_HOST_RENDERER)
    // The player's settings (GALAXY_RUNTIME_SETTINGS_PATH in tests) before the mods: the language they choose
    // picks the mods' text (and is the game's language from then on).
    try {
        galaxy::load_runtime_settings_from_file();
    } catch (const std::exception& failure) {
        die("the settings did not load: %s", failure.what());
    }
#endif
    {
        // Mods: the mods folder next to the settings (galaxy::mods), applied before the game reads its disc.
        const fs::path settings_folder = galaxy::runtime_settings_folder();
        const std::string game_id(reinterpret_cast<const char*>(gPak.data), 6);
        char title[9];
        std::snprintf(title, sizeof(title), "%02x%02x%02x%02x", gPak.data[0], gPak.data[1], gPak.data[2], gPak.data[3]);
        gSaveDataPath = std::string("/title/00010000/") + title + "/data";
        if (!settings_folder.empty()) {
            fs::create_directories(settings_folder / "mods", error);
            galaxy::mods::set_mods_folder(settings_folder / "mods", settings_folder / "mods.ini", game_id);
            gMods = galaxy::mods::apply_mods(std::span<const std::uint8_t>(gPak.fst, gPak.fst_size),
                                             std::span<const std::uint8_t>(gPak.dol, gPak.dol_size),
                                             static_cast<uint64_t>(gPak.dol - gPak.data), gPak.data, gPak.size, game_id,
                                             gPak.data[7], gPak.data[6],
                                             galaxy::mods::language_folder(
                                                 game_id, static_cast<unsigned>(galaxy::game_language_for_boot())));
        }
        if (gMods != nullptr) {
            galaxy::gx::set_texture_pack_mod_folders(gMods->texture_folders);
            galaxy::gx::set_movie_mod_folders(gMods->movie_folders);
            std::fprintf(stderr, "[mods] %zu mod(s), %u file patch(es), %u unsupported\n%s", gMods->active.size(),
                         gMods->files_changed, gMods->unsupported, gMods->report.c_str());
            const std::string text = "Nebula mods applied at the last start (" + game_id + ")\n\n" + gMods->report;
            (void)galaxy::io::write_text_file_atomically(settings_folder / "mods-report.txt", text);
            if (!gMods->save_folder.empty()) {
                // Riivolution's clone: the NAND's saves copied into an empty save folder.
                const fs::path nand_saves = gNandRoot / gSaveDataPath.substr(1);
                const bool empty = !fs::exists(gMods->save_folder, error) || fs::is_empty(gMods->save_folder, error);
                fs::create_directories(gMods->save_folder, error);
                if (empty && gMods->save_clone && fs::exists(nand_saves, error)) {
                    fs::copy(nand_saves, gMods->save_folder, fs::copy_options::recursive, error);
                }
            }
        }
        {
            // The session's mods, for crash reports: one line each, and whether one changes the game itself.
            std::string lines;
            bool change_game = false;
            for (const galaxy::mods::ModInfo& mod : galaxy::mods::session_mods().started_with) {
                if (!mod.enabled) continue;
                std::string kinds;
                for (const auto& [on, name] : {std::pair{mod.riivolution, "Riivolution"},
                                               std::pair{mod.files || mod.loose, "game files"},
                                               std::pair{mod.textures, "textures"}, std::pair{mod.movies, "movies"}}) {
                    if (on) kinds += kinds.empty() ? name : std::string(", ") + name;
                }
                lines += "  " + mod.id + (kinds.empty() ? "" : " (" + kinds + ")") + "\n";
                change_game = change_game || mod.riivolution || mod.files || mod.loose;
            }
            galaxy::crash::set_session_mods(lines, change_game);
        }
        if (gMods == nullptr && !settings_folder.empty() && fs::exists(settings_folder / "mods-report.txt", error)) {
            // No mod this start: an earlier start's report must not stand.
            (void)galaxy::io::write_text_file_atomically(settings_folder / "mods-report.txt",
                                                         "Nebula mods applied at the last start: none\n");
        }
    }
    start_netplay(module);
#endif

    static NebulaHostServices services{};
    services.version = NEBULA_GAME_API_VERSION;
    services.size = sizeof(services);
    services.log = svc_log;
    services.fatal = svc_fatal;
    services.gx_submit = svc_gx_submit;
    services.gx_pe_marker = svc_gx_pe_marker;
    services.gx_peek_efb = svc_gx_peek_efb;
    services.gx_scene_marker = svc_gx_scene_marker;
    services.gx_split_clip = svc_gx_split_clip;
    services.luigi_swap = svc_luigi_swap;
    services.det_boundary = svc_det_boundary;
    services.movie_frame = svc_movie_frame;
    services.code_patch = svc_code_patch;
    services.presence = svc_presence;
    services.achievement = svc_achievement;
    services.achievement_date = svc_achievement_date;
    services.stat_add = svc_stat_add;
    services.stat_get = svc_stat_get;
    services.settings_view = svc_settings_view;
    services.settings_input = svc_settings_input;
    services.sharp_2d = svc_sharp_2d;
    services.gx_stripe_particle = svc_gx_stripe_particle;
    services.disc_fst = svc_disc_fst;
    services.disc_read = svc_disc_read;
    services.disc_dol = svc_disc_dol;
    services.vi_frame = svc_vi_frame;
    services.fs_request = svc_fs_request;
    services.sc_get = svc_sc_get;
    services.wpad_control = svc_wpad_control;
    services.wpad_speaker_data = svc_wpad_speaker_data;
    services.audio_dsp = svc_audio_dsp;
    services.ai_dma = svc_ai_dma;
    services.ai_control = svc_ai_control;
    services.hbm = svc_hbm;
    services.thp_video_decode = svc_thp_video_decode;
    services.gpu_coherent = svc_gpu_coherent;
    services.game_context = svc_game_context;
    services.power = svc_power;
    services.test_scene_frame = svc_test_scene_frame;
    services.display_extent = svc_display_extent;
    services.scene_surround = svc_scene_surround;
    services.option = svc_option;
    services.game_light = svc_game_light;
    services.game_stage = svc_game_stage;
    services.object_name = svc_object_name;
    services.light_reach = svc_light_reach;
    services.traced_sun = svc_traced_sun;
#if defined(NEBULA_HOST_RENDERER)
    // The display configuration (GALAXY_RENDER_CONFIG_PATH in tests), loaded before the window; the settings overlay
    // reads its frame art from the game files.
    try {
        galaxy::gx::load_render_config_from_file();
    } catch (const std::exception& failure) {
        die("the settings did not load: %s", failure.what());
    }
    galaxy::gx::set_native_settings_content_root(galaxy::host::locate_game(pak).content_root.string());
    // Discord: never from a hidden test run, which must leave the player's status alone.
    if (const char* hidden = std::getenv("GALAXY_HIDE_WINDOW"); hidden == nullptr || hidden[0] == '\0' || hidden[0] == '0') {
        const char* id = std::getenv("GALAXY_DISCORD_CLIENT_ID");
        galaxy::discord::set_enabled_provider([] { return galaxy::get_runtime_settings().discord_status; });
        galaxy::discord::start(id != nullptr ? id : kDiscordClientId);
    }
    // The environment is final from here: the input report's reads are served from a table (each read otherwise scans
    // the whole environment block, 100 reports a second).
    galaxy::host::set_env_flag_cache_enabled(true);
    const galaxy::gx::RenderConfig render_config = galaxy::gx::get_render_config();
    if (!galaxy::gx::initialize(static_cast<int>(render_config.window_width),
                                static_cast<int>(render_config.window_height))) {
        const std::string reason = galaxy::gx::startup_failure();
        die("the renderer did not start%s%s", reason.empty() ? "" : ": ", reason.c_str());
    }
    galaxy::gx::set_indexed_load_host_order(true);
    gAudio = new galaxy::host::NativeGameAudio();  // lives as long as the process (the AI thread is detached)
    gAudio->set_channel_owner(gModule.audio_channel_owner);
    {
        // Custom voices (voice_replacement.h): the highest enabled mod with a voice folder (voice\voices.txt).
        std::vector<galaxy::mods::ModInfo> mods = galaxy::mods::session_mods().started_with;
        std::sort(mods.begin(), mods.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        for (const galaxy::mods::ModInfo& mod : mods) {
            if (mod.enabled && fs::is_regular_file(mod.path / "voice" / "voices.txt", error)) {
                std::fprintf(stderr, "[voice] clips from mod %s\n", mod.id.c_str());
                gAudio->load_voices(mod.path / "voice");
                break;
            }
        }
    }
    // The live controls' links to the window: the mouse as the pointer, keyboard capture and controller navigation
    // while the settings overlay or the HOME Menu is open.
    galaxy::host::install_host_pointer_provider([]() noexcept {
        galaxy::host::HostPointerState state{};
        const galaxy::gx::HostPointerState pointer = galaxy::gx::poll_host_pointer_state();
        state.window_focused = pointer.window_focused;
        state.inside_client = pointer.inside_client;
        state.absolute_valid = pointer.absolute_valid && pointer.client_width > 1 && pointer.client_height > 1;
        state.left_button = pointer.left_button;
        state.right_button = pointer.right_button;
        state.client_x = pointer.client_x;
        state.client_y = pointer.client_y;
        state.client_width = pointer.client_width;
        state.client_height = pointer.client_height;
        state.absolute_sequence = pointer.absolute_sequence;
        state.debug_window = pointer.debug_window;
        return state;
    });
    galaxy::host::install_host_pointer_transition_provider(
        [](std::uint64_t after) noexcept { return galaxy::gx::poll_host_pointer_transition(after); });
    galaxy::host::install_native_keyboard_capture_provider([]() noexcept { return galaxy::gx::native_window_input_captured(); });
    galaxy::host::install_native_settings_pad_hooks(
        []() noexcept { return galaxy::gx::native_settings_overlay_visible() || galaxy::gx::home_menu_visible(); },
        [](std::uintptr_t key) noexcept { galaxy::gx::native_settings_post_key(key); });
#endif
    if (gModule.attach(&services) != NEBULA_GAME_API_VERSION) {
        die("the game module's API version differs from the host's (%u)", NEBULA_GAME_API_VERSION);
    }

    name_thread(L"Nebula Host Main");
    gDisc.start();
    gFs.start();
    std::thread(ai_thread).detach();
    std::thread(vi_thread).detach();
    std::thread(wpad_thread).detach();
#if defined(NEBULA_HOST_SDL)
    galaxy::sdl::start_thread_sampler();
#endif
    std::thread game([] {
        name_thread(L"Nebula Game");
        galaxy::host::perf_hint_add_current_thread();
        gModule.run();
    });
#if defined(_WIN32)
    // The simulation thread's CPU time, reported each second (the game and everything it calls on that thread).
    HANDLE game_thread = nullptr;  // its own handle: detach() closes the thread object's
    DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(game.native_handle()), GetCurrentProcess(), &game_thread,
                    THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    uint64_t game_cycles_last = 0;
    const char* trace_env = std::getenv("GALAXY_TRACE_THREAD_CPU");
    const bool trace_thread_cpu = trace_env != nullptr && trace_env[0] == '1';
    // Thread cycles count at the time-stamp counter's rate, measured here against the performance counter.
    const double tsc_hz = [] {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const uint64_t t0 = __rdtsc();
        do {
            QueryPerformanceCounter(&b);
        } while (b.QuadPart - a.QuadPart < f.QuadPart / 20);
        return (double)(__rdtsc() - t0) * (double)f.QuadPart / (double)(b.QuadPart - a.QuadPart);
    }();
#endif
    game.detach();

    const auto end = Clock::now() + std::chrono::duration<double>(seconds > 0 ? seconds : 1e9);
    uint32_t reported = 0;
    auto next_report = Clock::now() + std::chrono::seconds(1);
    while (Clock::now() < end && !gTourDone.load(std::memory_order_relaxed)) {
#if defined(NEBULA_HOST_RENDERER)
        // The window's messages, on the thread that created it.
#if defined(NEBULA_HOST_SDL)
        galaxy::sdl::pump_events(4);
        if (const bool away = galaxy::sdl::in_background(); away != gSuspended) {
            {
                std::lock_guard<std::mutex> lock(gSuspendLock);
                gSuspended = away;
            }
            gSuspendWake.notify_all();
            gAudio->set_paused(away);
            log_line("host:", away ? "suspended (out of sight)" : "resumed");
        }
#else
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 4, QS_ALLINPUT);
#endif
        galaxy::gx::pump_messages();
        if (galaxy::gx::quit_requested()) {
            break;
        }
        if (Clock::now() < next_report) {
            continue;
        }
#else
        std::this_thread::sleep_until(next_report);
#endif
        next_report += std::chrono::seconds(1);
        const uint32_t retraces = gRetraces.load(std::memory_order_relaxed);
        if (retraces != reported) {
            char text[384];
            std::snprintf(text, sizeof(text), "retrace %u, xfb 0x%08X, gx %.2f MB, disc reads %u, nand ops %u, ai blocks %u, dsp subframes %u",
                          retraces, gXfb.load(std::memory_order_relaxed), gGxBytes.load(std::memory_order_relaxed) / 1048576.0,
                          gDiscReads.load(std::memory_order_relaxed), gFsOps.load(std::memory_order_relaxed),
                          gAiBlocks.load(std::memory_order_relaxed), gDspFrames.load(std::memory_order_relaxed));
#if defined(NEBULA_HOST_RENDERER)
            if (retraces / 60 % 30 == 0) {
                log_line("host:", ("jaudio " + gAudio->stats_line()).c_str());
            }
            const size_t length = std::strlen(text);
            static uint64_t last_decode_ns = 0;
            const uint64_t decode_ns = gMovieDecodeNs.load(std::memory_order_relaxed);
            std::snprintf(text + length, sizeof(text) - length,
                          ", renderer frames %llu, movie frames %u (decoded in %.1f ms/s, %u replaced)",
                          (unsigned long long)gSubmissions.load(std::memory_order_relaxed),
                          gMovieFrames.load(std::memory_order_relaxed),
                          static_cast<double>(decode_ns - last_decode_ns) / 1.0e6,
                          gMovieSkipped.load(std::memory_order_relaxed));
            last_decode_ns = decode_ns;
#endif
#if defined(_WIN32)
            if (trace_thread_cpu) {
                report_thread_cpu(tsc_hz);
            }
            ULONG64 cycles = 0;
            if (QueryThreadCycleTime(game_thread, &cycles)) {
                const size_t used = std::strlen(text);
                std::snprintf(text + used, sizeof(text) - used, ", game cpu %.1f ms/s",
                              (double)(cycles - game_cycles_last) * 1000.0 / tsc_hz);
                game_cycles_last = cycles;
            }
#endif
            log_line("host:", text);
            reported = retraces;
        }
    }
    log_line("host:", Clock::now() < end ? "the window was closed" : "time limit reached");
    log_line("host:", ("3D scenes " + std::to_string(gSceneBegins.load(std::memory_order_relaxed))).c_str());
    if (gModule.debug_dump != nullptr) {
        gModule.debug_dump();
    }
#if defined(NEBULA_HOST_RENDERER) && defined(_WIN32)
    galaxy::timeline::dump();
#endif
#if defined(NEBULA_HOST_RENDERER)
    galaxy::achievements::flush();
    galaxy::stats::flush();
#endif
    std::fflush(stdout);
    std::_Exit(0);
}
