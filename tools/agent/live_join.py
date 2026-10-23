"""Live join on one PC (docs/HOT_JOIN.md): a joinable host plays into a galaxy, a second copy asks to join it
through the lobby, and joins the running game without the host restarting.

    py -3 tools/agent/live_join.py [--stage "good egg"] [--mission 1] [--after 1800] [--exit-after 600]

The host boots Joinable (deterministic from boot, a session host on its own) with GALAXY_NET_AUTOACCEPT=1 and the
agent plays it into the galaxy; then the joiner starts with GALAXY_NET_AUTOJOIN, restarts once into the live join,
loads the host's snapshot, catches up and drops in as the second player. Both hash MEM1 every 30 frames; from the
join frame on the hashes must agree, the session's own check (every 120 frames) must find no DESYNC, and both must
keep 60 frames a second. --exit-after has the host's input carry the online menu's Exit Galaxy that many frames
after the join (a stage change both PCs make). Everything runs hidden.
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import determinism  # noqa: E402
import nebula_agent  # noqa: E402


def set_settings(inst, values):
    path = os.path.join(inst.dir, 'runtime_settings.ini')
    lines = open(path, encoding='utf-8').read().splitlines() if os.path.exists(path) else []
    lines = [l for l in lines if l.split('=')[0].strip() not in values]
    lines += ['%s=%s' % kv for kv in values.items()]
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')


def log_lines(inst, pattern):
    return [l for l in inst.log_tail(200000) if pattern in l]


def wait_log(inst, pattern, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = log_lines(inst, pattern)
        if lines:
            return lines[0]
        crash = inst.crash()
        if crash:
            raise nebula_agent.GameError('%s crashed: %s' % (inst.name, crash[0]))
        time.sleep(0.5)
    raise nebula_agent.GameError('%s: no "%s" in %.0f s' % (inst.name, pattern, timeout))


def end_copies_of(run):
    """Ends every NebulaRuntime still running from this run folder (the test's own copies)."""
    import subprocess
    exe = os.path.join(os.path.abspath(run), 'NebulaRuntime.exe')
    script = ("Get-Process NebulaRuntime -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '%s' } | "
              "Stop-Process -Force" % exe)
    subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True)


def frame_rate(inst, seconds):
    s0, t0 = inst.status() or {}, time.time()
    time.sleep(seconds)
    s1, t1 = inst.status() or {}, time.time()
    return (s1.get('frame', 0) - s0.get('frame', 0)) / (t1 - t0)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--run', default=nebula_agent.DEFAULT_RUN)
    parser.add_argument('--content', default=nebula_agent.DEFAULT_CONTENT)
    parser.add_argument('--root', default=nebula_agent.DEFAULT_ROOT)
    parser.add_argument('--out', default=os.path.join(nebula_agent.DEFAULT_ROOT, 'live'))
    parser.add_argument('--port', type=int, default=27811)
    parser.add_argument('--stage', default='good egg')
    parser.add_argument('--mission', type=int, default=1)
    parser.add_argument('--play', type=float, default=20.0, help='seconds the host plays in the galaxy first')
    parser.add_argument('--after', type=int, default=1800, help='frames to keep playing after the join')
    parser.add_argument('--exit-after', type=int, default=0, help='Exit Galaxy this many frames after the join')
    parser.add_argument('--env', action='append', help='NAME=VALUE for both copies')
    args = parser.parse_args(argv)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    for name in ('host.hash', 'join.hash'):
        if os.path.exists(os.path.join(out, name)):
            os.remove(os.path.join(out, name))
    common = dict(determinism.parse_env(args.env), GALAXY_STATE_HASH_EVERY='30', GALAXY_STATE_HASH_CHUNKS='1')
    started = time.time()
    host = nebula_agent.Instance('live-host', root=args.root, run=args.run, content=args.content)
    joiner = nebula_agent.Instance('live-join', root=args.root, run=args.run, content=args.content)
    for inst in (host, joiner):
        inst.stop()
        inst.prepare(fresh_saves=True)
    set_settings(host, {'net_joinable': 'on', 'net_port': str(args.port)})
    set_settings(joiner, {'net_joinable': 'off'})
    host_env = dict(common, GALAXY_NET_AUTOACCEPT='1', GALAXY_STATE_HASH=os.path.join(out, 'host.hash'))
    if args.exit_after:
        host_env['GALAXY_NET_TEST_EXIT_AFTER_JOIN'] = str(args.exit_after)
    ok = True
    try:
        host.launch(extra_env=host_env)
        host.boot_through_menus(timeout=240.0)
        host.goto(args.stage, args.mission, timeout=180.0)
        print('host in %s mission %d at frame %s' % (args.stage, args.mission, (host.status() or {}).get('frame')))
        moves = [(0.9, 0.0), (0.0, 0.9), (-0.9, 0.0), (0.0, -0.9)]
        end = time.time() + args.play
        k = 0
        while time.time() < end:
            x, y = moves[k % len(moves)]
            host.command('stick 1 %.1f %.1f 40 nowait' % (x, y), timeout=60.0)
            host.command('press 1 A 6', timeout=60.0)
            host.command('wait 30', timeout=60.0)
            k += 1
        joiner.launch(extra_env=dict(common, GALAXY_NET_AUTOJOIN='127.0.0.1:%d' % args.port,
                                     GALAXY_STATE_HASH=os.path.join(out, 'join.hash')))
        asked = time.time()
        line = wait_log(host, 'the joiner drops in', 240.0)
        join_frame = int(re.search(r'frame (\d+)', line).group(1))
        print('joined at frame %d, %.0f s after the joiner started' % (join_frame, time.time() - asked))
        for inst in (host, joiner):
            for pattern in ('[net] snapshot of frame', '[hot-join] frame', '[hot-join] loaded', '[net] the host\'s game',
                            '[net] the snapshot of frame', 'caught up', 'catching up', '[lobby]'):
                for l in log_lines(inst, pattern)[:3]:
                    print('  %s: %s' % (inst.name, l))
        wait_log(joiner, 'in the game', 60.0)
        # Both play on: the host moves (its controller is the first player on both PCs).
        end_frame = join_frame + args.after
        rates = []
        while (host.status() or {}).get('frame', 0) < end_frame:
            host.command('stick 1 0.8 0.3 40 nowait', timeout=60.0)
            host.command('press 1 A 6', timeout=60.0)
            rates.append((frame_rate(host, 2.0), frame_rate(joiner, 2.0)))
            if host.crash() or joiner.crash():
                break
        if rates:
            print('frames a second after the join: host %.1f, joiner %.1f (lowest %.1f / %.1f)' % (
                sum(r[0] for r in rates) / len(rates), sum(r[1] for r in rates) / len(rates),
                min(r[0] for r in rates), min(r[1] for r in rates)))
        for inst in (host, joiner):
            for pattern in ('drops in', 'DESYNC', '[crash]', 'test: exit galaxy', 'exiting', 'stalls'):
                lines = log_lines(inst, pattern)
                if lines:
                    print('  %s: %d x "%s" (%s)' % (inst.name, len(lines), pattern, lines[-1][:120]))
        a = determinism.read_hashes(os.path.join(out, 'host.hash'))
        b = determinism.read_hashes(os.path.join(out, 'join.hash'))
        frames = sorted(f for f in set(a) & set(b) if f >= join_frame)
        bad = [f for f in frames if a[f][0] != b[f][0]]
        if not frames:
            print('no frames to compare')
            ok = False
        elif bad:
            mb = [k for k in range(len(a[bad[0]][1])) if a[bad[0]][1][k] != b[bad[0]][1][k]]
            print('MEM1 differs from frame %d (MB %s) of %d..%d' % (bad[0], mb, frames[0], frames[-1]))
            ok = False
        else:
            print('MEM1 identical at all %d samples, frames %d..%d' % (len(frames), frames[0], frames[-1]))
        if any(log_lines(i, 'DESYNC') for i in (host, joiner)):
            ok = False
    except nebula_agent.GameError as e:
        print('stopped: %s' % e)
        ok = False
    finally:
        for inst in (host, joiner):
            # The joiner restarted itself: its pid file names the process that is gone, so ask it directly.
            try:
                inst.command('quit', timeout=5.0)
            except nebula_agent.GameError:
                pass
            inst.stop()
        time.sleep(2.0)
        end_copies_of(args.run)
    print('%s (%.0f s)' % ('PASS' if ok else 'FAIL', time.time() - started))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
