// net_lockstep.h on one PC: a host and a joiner over localhost (the handshake, the save, a refusal), the frames'
// inputs in lockstep with the input delay, and the desync check.
#include "galaxy/net_lockstep.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS:" : "FAIL:", what);
    if (!ok) ++failures;
}

std::atomic<unsigned> g_port{0};
void on_ready(unsigned port) { g_port = port; }

galaxy::net::FrameInput input_with_buttons(bool a, bool b) {
    galaxy::net::FrameInput input;
    input.snapshot.buttons.a = a;
    input.snapshot.buttons.b = b;
    return input;
}

}  // namespace

int main() {
    using namespace galaxy::net;
    // A joiner with other mods is refused.
    {
        Session host;
        std::thread joiner;
        std::string join_error;
        SessionSettings host_settings;
        g_port = 0;
        std::thread hosting([&] {
            std::string error;
            (void)host.host(0, "build-1", "Odyssey Mario", host_settings, {}, error, on_ready);
        });
        while (g_port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        {
            Session refused;
            SessionSettings settings;
            std::vector<SaveFile> save;
            const bool ok = refused.join("127.0.0.1:" + std::to_string(g_port.load()), "build-1", "", settings, save,
                                         join_error);
            check("a joiner with different mods is refused", !ok && join_error.find("mods") != std::string::npos);
        }
        // Then a matching one gets in.
        Session good;
        SessionSettings settings;
        std::vector<SaveFile> save;
        std::string error;
        const bool ok = good.join("127.0.0.1:" + std::to_string(g_port.load()), "build-1", "Odyssey Mario", settings,
                                  save, error);
        hosting.join();
        check("a matching joiner gets in", ok && host.role() == Role::Host && good.role() == Role::Join);
        good.close();
        host.close();
    }
    // The session: settings and save to the joiner, inputs in lockstep, hashes.
    {
        Session host;
        Session joiner;
        SessionSettings sent;
        sent.calendar = 820454400u;
        sent.moveset = 0x3Fu;
        sent.motion_style = 2u;
        sent.delay = 3u;
        const std::vector<SaveFile> save = {{"title/00010000/524d4745/data/GameData.bin", {1, 2, 3, 4}},
                                            {"title/00010000/524d4745/data/banner.bin", std::vector<std::uint8_t>(5000, 7)}};
        g_port = 0;
        std::thread hosting([&] {
            std::string error;
            (void)host.host(0, "b", "m", sent, save, error, on_ready);
        });
        while (g_port == 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SessionSettings got;
        std::vector<SaveFile> got_save;
        std::string error;
        const bool joined = joiner.join("127.0.0.1:" + std::to_string(g_port.load()), "b", "m", got, got_save, error);
        hosting.join();
        check("joined", joined);
        check("the joiner has the host's settings",
              got.calendar == sent.calendar && got.moveset == sent.moveset && got.motion_style == 2u && got.delay == 3u);
        check("the joiner has the host's save", got_save.size() == 2 && got_save[0].path == save[0].path &&
                                                    got_save[0].data == save[0].data && got_save[1].data.size() == 5000);
        check("channels: host 0, joiner 1", host.local_channel() == 0 && joiner.local_channel() == 1);

        // 120 frames: each sends its input for frame + delay as it takes a frame; both see the same inputs.
        bool same = true;
        bool expected = true;
        std::thread joiner_frames([&] {
            for (std::uint32_t frame = 1; frame <= 120; ++frame) {
                joiner.send_input(frame + 3, input_with_buttons(false, (frame + 3) % 2 == 0));
                FrameInput inputs[2];
                if (!joiner.frame_inputs(frame, inputs)) {
                    same = false;
                    return;
                }
            }
        });
        for (std::uint32_t frame = 1; frame <= 120; ++frame) {
            host.send_input(frame + 3, input_with_buttons((frame + 3) % 3 == 0, false));
            FrameInput inputs[2];
            if (!host.frame_inputs(frame, inputs)) {
                same = false;
                break;
            }
            if (frame > 3) {
                expected = expected && inputs[0].snapshot.buttons.a == (frame % 3 == 0) &&
                           inputs[1].snapshot.buttons.b == (frame % 2 == 0);
            } else {
                expected = expected && !inputs[0].snapshot.buttons.a && !inputs[1].snapshot.buttons.b;
            }
        }
        joiner_frames.join();
        check("120 frames in lockstep", same);
        check("each frame's inputs are the ones sent for it (neutral before the delay)", expected);

        host.report_hash(300, 0x1234u);
        joiner.report_hash(300, 0x1234u);
        host.report_hash(600, 0x5555u);
        joiner.report_hash(600, 0x6666u);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check("matching hashes pass, a differing one is a desync", host.desyncs() == 1 && joiner.desyncs() == 1);

        joiner.close();
        FrameInput inputs[2];
        check("the session ends when the other PC leaves", !host.frame_inputs(500, inputs));
        host.close();
    }
    if (failures != 0) {
        std::printf("net lockstep tests: %d failed\n", failures);
        return 1;
    }
    std::printf("net lockstep tests passed\n");
    return 0;
}
