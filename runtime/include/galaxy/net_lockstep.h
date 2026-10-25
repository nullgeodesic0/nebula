#pragma once

// Online co-op by lockstep (docs/NETPLAY.md): two PCs run the same game in deterministic mode (the module's virtual
// clock, sdk/os/os_native.c) and exchange only their players' input. The host plays Mario (Wii Remote channel 0), the
// joiner Luigi (channel 1). Each PC sends its player's input for frame N + delay as it grants retrace N; retrace N waits
// until both players' input for N is in. Both PCs then compute the same frames.
//
// Session start (before the game boots): the joiner connects and sends its build and mods; the host refuses a
// different build or mods, else answers with the session: the game's settings that change what it computes, the
// calendar, the input delay and its save (the joiner plays the host's save, kept apart from its own).
//
// Every kHashEvery scene frames both PCs send a hash of the game's memory; a difference is a desync, logged.
//
// Live join (settings.live): the host plays from boot and a joiner may come at any time. The session then starts at a
// boundary the host chooses (activate(base): inputs up to base + delay are neutral on both PCs), and the host sends
// the game as it was there (send_state); the joiner becomes it (take_state) and both go on from that frame.

#include "galaxy/native_input.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace galaxy::net {

inline constexpr std::uint32_t kProtocol = 1u;
inline constexpr std::uint32_t kHashEvery = 300u;

// What both PCs must compute alike, as the host has it.
struct SessionSettings {
    std::uint32_t calendar = 0;       // seconds since 2000-01-01
    std::uint32_t coop = 1;
    std::uint32_t moveset = 0;        // NEBULA_MOVESET_* bits
    std::uint32_t motion_style = 0;
    std::uint32_t text_speed = 1;
    std::uint32_t language = 0;
    std::uint32_t delay = 3;          // input delay in frames
    std::uint32_t rollback = 0;       // 1: rollback (each PC runs ahead on the other's predicted input)
    std::uint32_t live = 0;           // 1: live join (the host plays on until a joiner comes; implies rollback)
};

struct SaveFile {
    std::string path;  // relative to the save folder, '/' separated
    std::vector<std::uint8_t> data;
};

enum class Role { None, Host, Join };

// One frame's input of one player: the snapshot the host turns into that player's remote report.
struct FrameInput {
    input::WiimoteInputSnapshot snapshot{};
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Host: listens on `port` (0: any, see port()) and waits for a joiner; `ready` runs once listening. Join:
    // connects to `address` ("host[:port]"). `build_id` and `mods` must match the other PC's. The host's `settings`
    // and `save` go to the joiner; on the joiner they come back filled. False (with `error`) when no session started.
    bool host(unsigned port, const std::string& build_id, const std::string& mods, const SessionSettings& settings,
              const std::vector<SaveFile>& save, std::string& error, void (*ready)(unsigned port) = nullptr);
    bool join(const std::string& address, const std::string& build_id, const std::string& mods,
              SessionSettings& settings, std::vector<SaveFile>& save, std::string& error);

    [[nodiscard]] Role role() const noexcept { return role_.load(); }
    [[nodiscard]] bool active() const noexcept { return role_.load() != Role::None; }
    // Live join: the other PC shook hands (the session starts at activate).
    [[nodiscard]] bool connected() const noexcept { return pending_role_ != Role::None || active(); }
    void activate(std::uint32_t base);
    // Live join: the game image, sent by the host; the joiner's waits for it (false when the session ended first).
    void send_state(const std::vector<std::uint8_t>& image);
    bool take_state(std::vector<std::uint8_t>& image);
    [[nodiscard]] const SessionSettings& settings() const noexcept { return settings_; }
    // This PC's player's remote channel: 0 hosting, 1 joined.
    [[nodiscard]] unsigned local_channel() const noexcept {
        return role_.load() == Role::Join || pending_role_ == Role::Join ? 1u : 0u;
    }
    [[nodiscard]] unsigned port() const noexcept { return port_; }

    // This PC's player's input for `frame` (sent at once; kept for this PC's own use).
    void send_input(std::uint32_t frame, const FrameInput& input);
    // Both players' input for `frame`, by channel: waits for the other PC's (false when the session ended). Frames
    // up to the delay are neutral on both PCs.
    bool frame_inputs(std::uint32_t frame, FrameInput (&inputs)[2]);
    // Rollback: the other PC's input for `frame` when it has come (false: not yet), the newest frame up to which all
    // of it has, and this PC's own input for a frame (neutral up to the delay).
    bool remote_input(std::uint32_t frame, FrameInput& input);
    [[nodiscard]] std::uint32_t remote_confirmed() const;
    [[nodiscard]] FrameInput local_input(std::uint32_t frame);
    [[nodiscard]] bool ended() const;
    // The game memory's hash at a scene frame: sent, and compared with the other PC's.
    void report_hash(std::uint32_t scene_frame, std::uint64_t hash);
    [[nodiscard]] std::uint32_t desyncs() const noexcept;
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<Role> role_{Role::None};
    Role pending_role_ = Role::None;  // live join: the role once activated
    std::uint32_t base_ = 0;          // inputs up to base_ + delay are neutral
    SessionSettings settings_{};
    unsigned port_ = 0;
};

// The process's session (host_main): started from GALAXY_NET_HOST / GALAXY_NET_JOIN before the game boots.
Session& session();

}  // namespace galaxy::net
