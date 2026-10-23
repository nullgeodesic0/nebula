"""Determinism probe for netplay (docs/AGENT_TESTING.md): replay one input recording in several test copies at once
and find where their guest memory first differs.

    py -3 tools/agent/determinism.py record --out C:\\nb\\agent-mp\\det [--frames 2400] [--scenario observatory]
    py -3 tools/agent/determinism.py bisect --replay <file.rec> [--runs 2] [--frames 2400] [--every 30]
                                            [--symbols symbols.txt] [--env NAME=VALUE ...]

`record` launches one deterministic copy, presses A through the opening and records every frame's controller data.
`bisect` replays a recording in --runs copies in parallel, hashing MEM1 every --every frames; at the first sample
where the copies disagree it replays again with a hash every frame over just that window, then dumps MEM1 at the
exact first differing frame in two copies and lists the differing words (with names from a decomp symbol map,
`name = .section:0xADDR; // type:... size:0x...` lines, when one is given). Every copy runs hidden, from fresh
saves, and quits by itself at its last frame (GALAXY_STATE_EXIT_FRAME); replays run unpaced
(GALAXY_DETERMINISTIC_UNPACED) unless --paced is given. `record` always runs at real-time speed, since the agent's
presses are timed in frames it counts as they come.
"""
import argparse
import bisect as bisect_module
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nebula_agent  # noqa: E402

MEM1_SIZE = 0x01800000


def read_hashes(path):
    """{frame: (whole-hash, [per-megabyte hashes])} from a GALAXY_STATE_HASH file."""
    out = {}
    try:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    out[int(parts[0])] = (parts[1], parts[2:])
    except OSError:
        pass
    return out


def run_copies(args, tag, count, env, last_frame, timeout, per_copy=None):
    """Launches `count` deterministic copies at once with `env` (plus a per-copy {i} filled in, and per_copy[i] if
    given), waits for all of them to quit at `last_frame` (or crash, or time out) and returns their instances."""
    instances = []
    for i in range(count):
        # Named after the output folder as well: two probes at once must not share an instance (or its lease).
        name = 'det-%s-%s-%d' % (os.path.basename(os.path.normpath(args.out)), tag, i)
        inst = nebula_agent.Instance(name, root=args.root, run=args.run, content=args.content)
        inst.stop()
        extra = {'GALAXY_DETERMINISTIC': '1', 'GALAXY_STATE_EXIT_FRAME': str(last_frame)}
        if not args.paced:
            extra['GALAXY_DETERMINISTIC_UNPACED'] = '1'
        if getattr(args, 'trace', False):
            extra['GALAXY_DETERMINISTIC_TRACE'] = os.path.join(args.out, '%s-%d.trace' % (tag, i))
        extra.update({k: v.replace('{i}', str(i)) for k, v in env.items()})
        if per_copy is not None:
            extra.update(per_copy[i])
        inst.launch(fresh_saves=True, extra_env=extra)
        instances.append(inst)
    deadline = time.time() + timeout
    while time.time() < deadline and any(inst.running() for inst in instances):
        time.sleep(0.5)
    for inst in instances:
        if inst.running():
            print('  %s did not reach frame %d in %.0f s: %s' % (inst.name, last_frame, timeout, inst.status_line()))
            inst.stop()
        crash = inst.crash()
        if crash:
            print('  %s crashed: %s' % (inst.name, crash[0]))
    return instances


def first_difference(hash_sets):
    """(last frame all agree on, first frame some disagree on, differing megabytes), over the frames all have."""
    frames = sorted(set.intersection(*(set(h) for h in hash_sets)))
    last_same = None
    for frame in frames:
        values = [h[frame] for h in hash_sets]
        if all(v[0] == values[0][0] for v in values):
            last_same = frame
            continue
        megabytes = [k for k in range(len(values[0][1])) if any(v[1][k] != values[0][1][k] for v in values)]
        return last_same, frame, megabytes
    return last_same, None, []


def load_symbols(path):
    symbols = []
    if not path:
        return symbols
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?', line)
        if m:
            symbols.append((int(m.group(3), 16), int(m.group(5) or '0', 16), m.group(1), m.group(2)))
    symbols.sort()
    return symbols


def symbol_name(symbols, starts, address):
    i = bisect_module.bisect_right(starts, address) - 1
    if i < 0:
        return '?'
    start, size, name, section = symbols[i]
    if size and address < start + size:
        return '%s.%s+0x%x' % (section, name, address - start)
    return '(after %s.%s)' % (section, name)


def diff_dumps(path_a, path_b, symbols, limit):
    a = open(path_a, 'rb').read()
    b = open(path_b, 'rb').read()
    wa = struct.unpack('>%dI' % (len(a) // 4), a)
    wb = struct.unpack('>%dI' % (len(b) // 4), b)
    starts = [s[0] for s in symbols]
    runs = []
    i, n = 0, min(len(wa), len(wb))
    while i < n:
        if wa[i] != wb[i]:
            j = i
            while j < n and (wa[j] != wb[j] or (j + 1 < n and wa[j + 1] != wb[j + 1])):
                j += 1
            runs.append((i, j))
            i = j
        else:
            i += 1
    print('differing words: %d in %d runs' % (sum(j - i for i, j in runs), len(runs)))
    for i, j in runs[:limit]:
        address = 0x80000000 + i * 4
        where = symbol_name(symbols, starts, address) if symbols else ''
        print('  %08X +%5d words  %-58s a=%08X b=%08X' % (address, j - i, where[:58], wa[i], wb[i]))


def compare_traces(path_a, path_b, context=6):
    """The first line where two GALAXY_DETERMINISTIC_TRACE files part, with the lines before it."""
    try:
        a = open(path_a, encoding='utf-8', errors='replace').read().splitlines()
        b = open(path_b, encoding='utf-8', errors='replace').read().splitlines()
    except OSError as e:
        print('   no traces to compare: %s' % e)
        return
    n = min(len(a), len(b))
    i = next((k for k in range(n) if a[k] != b[k]), n)
    if i == n and len(a) == len(b):
        print('   traces identical (%d lines)' % n)
        return
    print('   traces part at line %d of %d/%d (%s vs %s):' % (i + 1, len(a), len(b),
                                                           os.path.basename(path_a), os.path.basename(path_b)))
    for k in range(max(0, i - context), i):
        print('      = %s' % a[k])
    for k in range(i, min(i + 4, n)):
        print('      a %s' % a[k])
        print('      b %s' % b[k])


def parse_env(pairs):
    env = {}
    for pair in pairs or []:
        name, _, value = pair.partition('=')
        env[name] = value
    return env


def cmd_bisect(args):
    out = args.out
    os.makedirs(out, exist_ok=True)
    base = parse_env(args.env)
    base['GALAXY_INPUT_REPLAY'] = os.path.abspath(args.replay)
    started = time.time()

    print('1. %d copies to frame %d, a hash every %d frames' % (args.runs, args.frames, args.every))
    env = dict(base, GALAXY_STATE_HASH=os.path.join(out, 'coarse-{i}.hash'),
               GALAXY_STATE_HASH_EVERY=str(args.every), GALAXY_STATE_HASH_CHUNKS='1')
    run_copies(args, 'coarse', args.runs, env, args.frames, args.timeout)
    hashes = [read_hashes(os.path.join(out, 'coarse-%d.hash' % i)) for i in range(args.runs)]
    if not all(hashes):
        print('a copy wrote no hashes: see its log (nebula_agent.py --instance det-<out>-coarse-N log)')
        return 2
    same, differ, megabytes = first_difference(hashes)
    reached = min(max(h) for h in hashes)
    if differ is None:
        print('identical at every sample through frame %d (%.0f s)' % (reached, time.time() - started))
        return 0
    print('   agree through frame %s; differ at frame %d in MB %s' % (same, differ, megabytes))
    if args.trace:
        other = next(i for i in range(1, args.runs) if hashes[i][differ][0] != hashes[0][differ][0])
        compare_traces(os.path.join(out, 'coarse-0.trace'), os.path.join(out, 'coarse-%d.trace' % other))

    window_start = (same or 0) + 1
    print('2. %d copies, a hash every frame from %d to %d' % (args.runs, window_start, differ))
    env = dict(base, GALAXY_STATE_HASH=os.path.join(out, 'fine-{i}.hash'),
               GALAXY_STATE_HASH_EVERY='1', GALAXY_STATE_HASH_FROM=str(window_start), GALAXY_STATE_HASH_CHUNKS='1')
    run_copies(args, 'fine', args.runs, env, differ + 1, args.timeout)
    hashes = [read_hashes(os.path.join(out, 'fine-%d.hash' % i)) for i in range(args.runs)]
    _, exact, megabytes = first_difference(hashes)
    if exact is None:
        print('   the window replayed identically this time: the divergence is intermittent')
        exact = differ
    print('   first differing frame %d, MB %s' % (exact, megabytes))

    print('3. two copies dump MEM1 at frame %d' % exact)
    env = dict(base, GALAXY_STATE_DUMP='%d,%s' % (exact, os.path.join(out, 'mem-{i}.bin')))
    run_copies(args, 'dump', 2, env, exact + 1, args.timeout)
    dumps = [os.path.join(out, 'mem-%d.bin' % i) for i in range(2)]
    if all(os.path.exists(d) and os.path.getsize(d) == MEM1_SIZE for d in dumps):
        diff_dumps(dumps[0], dumps[1], load_symbols(args.symbols), args.limit)
    else:
        print('   a dump is missing')
    print('done in %.0f s' % (time.time() - started))
    return 1


def play_observatory(inst):
    """Into file 1 through the title and file select with controller input only (no game calls, so a replay of
    the recording takes the same path), then both brothers run, turn and jump around the Comet Observatory."""
    s = inst.boot_through_menus(timeout=240.0)
    print('playable at frame %s in %s' % (s.get('frame'), s.get('stage')))
    moves = [(0.9, 0.0), (0.0, 0.9), (-0.9, 0.0), (0.0, -0.9), (0.7, 0.7), (-0.7, 0.7)]
    for lap in range(4):
        for k, (x, y) in enumerate(moves):
            inst.command('stick 1 %.1f %.1f 70 nowait' % (x, y), timeout=60.0)
            inst.command('stick 2 %.1f %.1f 70 nowait' % (-y, x), timeout=60.0)
            inst.command('wait 25', timeout=60.0)
            inst.command('press %d A 8' % (1 + (k + lap) % 2), timeout=60.0)
            inst.command('wait 40', timeout=60.0)
    print('played to frame %s' % (inst.status() or {}).get('frame'))


def cmd_snapshot(args):
    """A straight replay and one that saves at --save and rolls back to it at --restore, side by side: after the
    rollback the replayed frames must hash as the straight run's do."""
    out = args.out
    os.makedirs(out, exist_ok=True)
    started = time.time()
    env = dict(parse_env(args.env), GALAXY_INPUT_REPLAY=os.path.abspath(args.replay),
               GALAXY_STATE_HASH=os.path.join(out, 'snap-{i}.hash'), GALAXY_STATE_HASH_EVERY=str(args.every),
               GALAXY_STATE_HASH_FROM=str(args.save), GALAXY_STATE_HASH_CHUNKS='1')
    if args.dump:
        # The rolled-back copy reaches this frame twice; the second dump (after the rollback) is the one kept.
        env['GALAXY_STATE_DUMP'] = '%d,%s' % (args.dump, os.path.join(out, 'mem-{i}.bin'))
    test = {'GALAXY_SNAPSHOT_TEST': '%d,%d' % (args.save, args.restore)}
    print('straight replay and save at %d / roll back at %d, to frame %d' % (args.save, args.restore, args.frames))
    instances = run_copies(args, 'snap', 2, env, args.frames, args.timeout, per_copy=[{}, test])
    log = instances[1].log_tail(400, '[netplay]')
    for line in log:
        if 'snapshot' in line or 'saved' in line or 'roll' in line or 'restor' in line:
            print('  ' + line)
    straight = read_hashes(os.path.join(out, 'snap-0.hash'))
    rolled = read_hashes(os.path.join(out, 'snap-1.hash'))   # later lines win: the frames after the rollback
    frames = sorted(set(straight) & set(rolled))
    if not frames:
        print('no frames to compare')
        return 2
    reached = max(rolled)
    if args.dump:
        dumps = [os.path.join(out, 'mem-%d.bin' % i) for i in range(2)]
        if all(os.path.exists(d) for d in dumps):
            print('MEM1 at frame %d, straight vs rolled back:' % args.dump)
            diff_dumps(dumps[0], dumps[1], load_symbols(args.symbols), 30)
    for frame in frames:
        if straight[frame][0] != rolled[frame][0]:
            megabytes = [k for k in range(len(straight[frame][1])) if straight[frame][1][k] != rolled[frame][1][k]]
            print('differ from frame %d (MB %s); the rolled-back copy reached frame %d (%.0f s)'
                  % (frame, megabytes, reached, time.time() - started))
            return 1
    print('identical at every frame %d..%d after the rollback (%.0f s)' % (frames[0], frames[-1], time.time() - started))
    return 0


def cmd_netpair(args):
    """A host and a joiner on this machine over localhost (GALAXY_NET_HOST / GALAXY_NET_JOIN): the host's agent
    presses A through the opening; both copies hash MEM1 and must agree at every sample."""
    out = args.out
    os.makedirs(out, exist_ok=True)
    started = time.time()
    copies = []
    for role, extra in (('host', {'GALAXY_NET_HOST': str(args.port)}),
                        ('join', {'GALAXY_NET_JOIN': '127.0.0.1:%d' % args.port})):
        name = 'net-%s-%s' % (os.path.basename(os.path.normpath(out)), role)
        inst = nebula_agent.Instance(name, root=args.root, run=args.run, content=args.content)
        inst.stop()
        path = os.path.join(out, role + '.hash')
        if os.path.exists(path):
            os.remove(path)
        env = {'GALAXY_NET_DELAY': str(args.delay), 'GALAXY_STATE_HASH': path, 'GALAXY_STATE_HASH_EVERY': str(args.every),
               'GALAXY_STATE_HASH_CHUNKS': '1', 'GALAXY_STATE_EXIT_FRAME': str(args.frames)}
        if args.dump:
            env['GALAXY_STATE_DUMP'] = '%d,%s' % (args.dump, os.path.join(out, 'mem-%s.bin' % role))
        env.update(extra)
        env.update(parse_env(args.host_env if role == 'host' else args.join_env))
        inst.launch(fresh_saves=True, extra_env=env, audio=args.audio)
        copies.append(inst)
    host = copies[0]
    try:
        host.wait_for(lambda s: s.get('frame', 0) > 0, timeout=120.0, what='the host to start')
        if args.scenario == 'observatory':
            play_observatory(host)
        else:
            for _ in range(args.presses):
                host.command('press 1 A 4', timeout=120.0)
                host.command('wait 40', timeout=120.0)
    except nebula_agent.GameError as e:
        print('host input stopped: %s' % e)
    deadline = time.time() + args.timeout
    while time.time() < deadline and any(i.running() for i in copies):
        time.sleep(0.5)
    for inst in copies:
        if inst.running():
            print('  %s did not finish: %s' % (inst.name, inst.status_line()))
            inst.stop()
        for line in inst.log_tail(100000, '[net]')[-6:]:
            print('  %s %s' % (inst.name, line))
        crash = inst.crash()
        if crash:
            print('  %s crashed: %s' % (inst.name, crash[0]))
    hashes = [read_hashes(os.path.join(out, role + '.hash')) for role in ('host', 'join')]
    same, differ, megabytes = first_difference(hashes)
    dumps = [os.path.join(out, 'mem-%s.bin' % role) for role in ('host', 'join')]
    if args.dump and all(os.path.exists(d) for d in dumps):
        print('MEM1 at frame %d, host vs joiner:' % args.dump)
        diff_dumps(dumps[0], dumps[1], load_symbols(args.symbols), 30)
    if differ is None:
        print('host and joiner identical at every sample through frame %s (%.0f s)' % (same, time.time() - started))
        return 0
    print('host and joiner agree through frame %s; differ at %d in MB %s' % (same, differ, megabytes))
    return 1


def cmd_record(args):
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, 'input.rec')
    inst = nebula_agent.Instance('det-record', root=args.root, run=args.run, content=args.content)
    inst.stop()
    inst.launch(fresh_saves=True, extra_env={'GALAXY_DETERMINISTIC': '1', 'GALAXY_INPUT_RECORD': path,
                                             'GALAXY_STATE_EXIT_FRAME': str(args.frames)})
    inst.wait_for(lambda s: s.get('frame', 0) > 0, timeout=60.0, what='the game to start')
    try:
        if args.scenario == 'opening':
            for _ in range(args.presses):
                inst.command('press 1 A 4', timeout=120.0)
                inst.command('wait 40', timeout=120.0)
        else:
            play_observatory(inst)
    except nebula_agent.GameError as e:
        print('stopped: %s' % e)
    deadline = time.time() + args.timeout
    while time.time() < deadline and inst.running():
        time.sleep(0.5)
    inst.stop()
    print('recorded %s' % path)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--run', default=nebula_agent.DEFAULT_RUN, help='the build folder')
    parser.add_argument('--content', default=nebula_agent.DEFAULT_CONTENT)
    parser.add_argument('--root', default=nebula_agent.DEFAULT_ROOT, help='where test instances live')
    parser.add_argument('--out', default=os.path.join(nebula_agent.DEFAULT_ROOT, 'det'))
    parser.add_argument('--timeout', type=float, default=900.0, help='seconds a phase may take')
    parser.add_argument('--paced', action='store_true',
                        help='replay at real-time speed (default: unpaced, as fast as the machine runs, no window '
                             'contents, no audio)')
    sub = parser.add_subparsers(dest='command', required=True)
    rec = sub.add_parser('record', help='record a run\'s input')
    rec.add_argument('--frames', type=int, default=2400)
    rec.add_argument('--presses', type=int, default=30, help='A presses (opening scenario)')
    rec.add_argument('--scenario', choices=('opening', 'observatory'), default='opening',
                     help='opening: A presses through the title; observatory: into file 1 by the menus, then '
                          'both brothers run and jump')
    bis = sub.add_parser('bisect', help='replay in parallel and find the first difference')
    bis.add_argument('--replay', required=True)
    bis.add_argument('--runs', type=int, default=2)
    bis.add_argument('--frames', type=int, default=2400)
    bis.add_argument('--every', type=int, default=30)
    bis.add_argument('--symbols', default=None, help='a decomp symbol map, to name the differing words')
    bis.add_argument('--limit', type=int, default=40, help='differing runs to list')
    bis.add_argument('--trace', action='store_true',
                     help='trace interrupts, IPC replies and clock jumps (GALAXY_DETERMINISTIC_TRACE) and show '
                          'where two diverging copies\' traces part')
    bis.add_argument('--env', action='append', help='NAME=VALUE for every copy ({i} becomes the copy number)')
    snap = sub.add_parser('snapshot', help='check that a rollback replays exactly (GALAXY_SNAPSHOT_TEST)')
    snap.add_argument('--replay', required=True)
    snap.add_argument('--save', type=int, required=True, help='frame to save at')
    snap.add_argument('--restore', type=int, required=True, help='frame to roll back at')
    snap.add_argument('--frames', type=int, default=0, help='last frame (default: 120 after --restore)')
    snap.add_argument('--every', type=int, default=1)
    snap.add_argument('--dump', type=int, default=0, help='also dump MEM1 at this frame in both and diff them')
    snap.add_argument('--symbols', default=None, help='a decomp symbol map, to name the differing words')
    snap.add_argument('--env', action='append', help='NAME=VALUE for both copies')
    net = sub.add_parser('netpair', help='a host and a joiner over localhost must stay identical')
    net.add_argument('--frames', type=int, default=3000)
    net.add_argument('--every', type=int, default=30)
    net.add_argument('--delay', type=int, default=3)
    net.add_argument('--port', type=int, default=27777)
    net.add_argument('--presses', type=int, default=25)
    net.add_argument('--dump', type=int, default=0, help='dump MEM1 at this frame in both and diff them')
    net.add_argument('--scenario', choices=('opening', 'observatory'), default='opening',
                     help="the host's play: A presses through the opening, or into file 1 and around the Observatory")
    net.add_argument('--symbols', default=None)
    net.add_argument('--audio', action='store_true', help='audio output on in both copies')
    net.add_argument('--host-env', action='append', help='NAME=VALUE for the host only')
    net.add_argument('--join-env', action='append', help='NAME=VALUE for the joiner only')
    args = parser.parse_args(argv)
    if args.command == 'netpair':
        return cmd_netpair(args)
    if args.command == 'snapshot':
        args.frames = args.frames or args.restore + 120
        return cmd_snapshot(args)
    return cmd_bisect(args) if args.command == 'bisect' else cmd_record(args)


if __name__ == '__main__':
    sys.exit(main())
