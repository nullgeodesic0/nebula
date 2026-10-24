"""Load every galaxy mission in a test copy and report which ones fail (docs/AGENT_TESTING.md).

    py -3 tools/agent/sweep.py [--solo] [--only StageName ...] [--out C:\\nb\\agent-mp\\sweep]

For each stage with scenario data in game.pak, each mission: jump in, wait until playable, walk the second player
away (split screen) and back, screenshot, and record ok / crash / hang / timeout. A crash or hang relaunches the
game and the sweep carries on. Results go to <out>/results.json and <out>/report.md as they come.
"""
import argparse
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'animations'))
import disc  # noqa: E402
import nebula_agent  # noqa: E402

# Not galaxies: menus and ending stages.
SKIP = {'FileSelect', 'EpilogueDemoStage', 'AstroDome', 'LibraryRoom', 'PeachCastleFinalGalaxy'}


def _hash(name):
    h = 0
    for c in name.encode('shift_jis'):
        h = (h * 31 + (c if c < 128 else c - 256)) & 0xFFFFFFFF
    return h


def _bcsv(data):
    count, field_count, offset, entry_size = struct.unpack_from('>IIII', data, 0)
    fields = [struct.unpack_from('>IIHBB', data, 16 + i * 12) for i in range(field_count)]
    rows = []
    for r in range(count):
        base = offset + r * entry_size
        row = {}
        for h, mask, at, shift, kind in fields:
            if kind in (0, 3):
                row[h] = (struct.unpack_from('>I', data, base + at)[0] & mask) >> shift
        rows.append(row)
    return rows


def missions(content):
    """{stage: [scenario numbers]} from each galaxy's scenario data."""
    pak = disc.Pak(os.path.join(content, 'game.pak'))
    out = {}
    scenario_no = _hash('ScenarioNo')
    for name in sorted(pak.files):
        if not name.endswith('Scenario.arc') or name.count('/') != 2:
            continue
        stage = name.split('/')[1]
        if stage in SKIP:
            continue
        files = disc.rarc_files(disc.yaz0_decode(pak.read(name)))
        data = files.get('scenariodata.bcsv') or files.get('ScenarioData.bcsv')
        if data is None:
            continue
        numbers = sorted({r.get(scenario_no) for r in _bcsv(data) if r.get(scenario_no)})
        if numbers:
            out[stage] = numbers
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--solo', action='store_true')
    parser.add_argument('--only', nargs='*')
    parser.add_argument('--out', default=os.path.join(nebula_agent.DEFAULT_ROOT, 'sweep'))
    parser.add_argument('--instance', default='sweep')
    parser.add_argument('--all', action='store_true',
                        help='include the missions the direct jump cannot start (hidden stars 6-7, the Observatory '
                             'beyond mission 1)')
    parser.add_argument('--retry', help='a results.json: run only its missions that did not load')
    parser.add_argument('--run', default=nebula_agent.DEFAULT_RUN, help='the build folder')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    plan = missions(nebula_agent.DEFAULT_CONTENT)
    if args.only:
        plan = {k: v for k, v in plan.items() if k in args.only}
    if not args.all:
        # Hidden stars need their own entrance and the Observatory follows the story: not reachable by a jump.
        plan = {k: [m for m in v if m < 6 and (k != 'AstroGalaxy' or m == 1)] for k, v in plan.items()}
    if args.retry:
        with open(args.retry) as f:
            failed = {(e['stage'], e['mission']) for e in json.load(f) if e['result'] != 'ok'}
        plan = {k: [m for m in v if (k, m) in failed] for k, v in plan.items()}
        plan = {k: v for k, v in plan.items() if v}
    game = nebula_agent.Instance(args.instance, run=args.run)
    results_path = os.path.join(args.out, 'results.json')
    results = []
    if os.path.exists(results_path):
        with open(results_path) as f:
            results = json.load(f)
    done = {(r['stage'], r['mission']) for r in results}

    def save():
        with open(results_path, 'w') as f:
            json.dump(results, f, indent=1)
        lines = ['# Co-op level sweep' if not args.solo else '# Solo level sweep', '',
                 '| Stage | Mission | Result | Detail |', '|---|---|---|---|']
        for r in results:
            lines.append('| %s | %d | %s | %s |' % (r['stage'], r['mission'], r['result'],
                                                    r.get('detail', '').replace('|', '/').replace('\n', ' ')[:300]))
        with open(os.path.join(args.out, 'report.md'), 'w') as f:
            f.write('\n'.join(lines) + '\n')

    def start():
        if game.running():
            game.stop()
        game.launch(coop=not args.solo, fresh_saves=True)
        game.boot()

    start()
    for stage, numbers in plan.items():
        for mission in numbers:
            if (stage, mission) in done:
                continue
            entry = {'stage': stage, 'mission': mission}
            began = time.time()
            try:
                if not game.running():
                    start()
                s = game.goto(stage, mission, timeout=150.0)
                game.command('wait 120', timeout=30)
                if not args.solo:
                    game.command('stick 2 1 0 150', timeout=30)
                    game.command('stick 2 -1 0 150', timeout=30)
                shot = os.path.join(args.out, '%s-%d.png' % (stage, mission))
                game.screenshot(shot)
                s = game.status() or s
                entry.update(result='ok', screenshot=shot,
                             detail='MEM1 %s KB, MEM2 %s KB free' % (
                                 (s.get('heap_free_kb') or {}).get('scene_mem1'),
                                 (s.get('heap_free_kb') or {}).get('scene_mem2')))
            except nebula_agent.GameError as error:
                text = str(error)
                crash = game.crash()
                if not crash and game.running():
                    try:  # what the screen shows when a mission never became playable
                        entry['screenshot'] = game.screenshot(os.path.join(args.out, '%s-%d-stuck.png' % (stage, mission)))
                        entry['state'] = game.status()
                    except nebula_agent.GameError:
                        pass
                entry['result'] = 'crash' if crash else ('hang' if 'stopped advancing' in text else 'timeout')
                entry['detail'] = ' '.join(l for l in crash if 'guest pc' in l or 'guest-stack' in l) or text
                try:
                    start()
                except nebula_agent.GameError as again:
                    entry['detail'] += ' (relaunch failed: %s)' % again
            entry['seconds'] = round(time.time() - began, 1)
            results.append(entry)
            save()
            print('%-28s %d  %-7s %s' % (stage, mission, entry['result'], entry.get('detail', '')[:120]), flush=True)
    game.stop()
    save()


if __name__ == '__main__':
    main()
