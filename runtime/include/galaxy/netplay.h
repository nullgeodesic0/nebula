#pragma once

#include "galaxy/native_api.h"

#include <cstdint>

namespace galaxy {

// Groundwork for rollback netcode (docs/NETPLAY.md), around every KPADRead (coop_guest.cpp's call hook, id 1):
// GALAXY_INPUT_RECORD=<file> records both players' controller data per game frame as the game receives it,
// GALAXY_INPUT_REPLAY=<file> gives the game a recording's instead, and GALAXY_STATE_HASH=<file> writes a hash of
// MEM1 every GALAXY_STATE_HASH_EVERY frames (60), with per-megabyte hashes when GALAXY_STATE_HASH_CHUNKS is set.
void netplay_kpad_before(PpcContext* context, const GuestMemoryV1* memory, std::uint32_t guest_pc);
void netplay_kpad_after(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                        std::uint32_t guest_pc);
// The channel of the KPADRead in flight at the per-frame read site (its before-hook seen), else 0xFFFFFFFF.
[[nodiscard]] std::uint32_t netplay_pending_channel() noexcept;
// Game frames so far (the first player's per-frame controller reads).
[[nodiscard]] std::uint32_t netplay_frame() noexcept;
// GALAXY_DETERMINISTIC_TRACE=<file>: true when tracing; netplay_trace writes "<frame> <text>" lines there (flushed,
// since probe runs end with TerminateProcess), for diffing two replays' event order.
[[nodiscard]] bool netplay_tracing() noexcept;

// An EFB peek's answer as a netplay input (deterministic mode). The game reads EFB depth every gameplay frame
// (the Star Pointer's GXPeekZ): an exact read would wait for the GPU, and its value depends on the GPU and the
// resolution, so two machines would disagree. The local answer (the render thread's latest published field) is
// recorded with the frame's input and a replay uses the recorded one, which is what a peer will send.
[[nodiscard]] std::uint32_t netplay_efb_peek(bool depth, std::uint32_t local_value) noexcept;

// Rollback snapshots, provided by the runtime. save(user) captures the whole machine and returns false, or true
// when a later restore(user) brought the machine back to it (execution continues from the save). restore(user)
// does not return. GALAXY_SNAPSHOT_TEST="<save frame>,<restore frame>" saves when that frame starts and rolls back
// to it once when the second starts; the frames in between run twice, and their hashes are written again.
struct NetplaySnapshotHooks {
    void* user = nullptr;
    bool (*save)(void* user) = nullptr;
    void (*restore)(void* user) = nullptr;
    // Re-simulation on/off: frames are simulated but not rendered (GALAXY_SNAPSHOT_RENDER_RESIM=1 renders them).
    void (*resimulating)(void* user, bool on) = nullptr;
    // Hot join (docs/HOT_JOIN.md). write: saves into a file and returns false, or true in another process that
    // loaded it and resumed here. load: resumes a written file (does not return unless it cannot be used).
    // GALAXY_SNAPSHOT_WRITE="<frame>,<file>" writes when that frame starts; GALAXY_SNAPSHOT_LOAD="<frame>,<file>"
    // loads when this process's own frame count reaches it, after which its frames are the writer's.
    bool (*write)(void* user, const char* path, std::uint32_t frame) = nullptr;
    void (*load)(void* user, const char* path) = nullptr;
};
void netplay_set_snapshot_hooks(const NetplaySnapshotHooks& hooks) noexcept;
// With GALAXY_DETERMINISTIC_TRACE: a line per frame of host-side state digests (written by `trace(user)` with
// netplay_trace), to find which part of a rollback snapshot is missing.
void netplay_set_digest_hook(void (*trace)(void* user), void* user) noexcept;
void netplay_trace(const char* format, ...) noexcept;

}  // namespace galaxy
