"""Play every mission's Power Star cutscenes in a test copy and report which ones fail (docs/AGENT_TESTING.md).

    py -3 tools/agent/cutscene_sweep.py [--solo] [--only StageName ...] [--out C:\\nb\\agent-mp\\cutscenes]

For each mission: jump in, make one of the stage's Power Stars appear (PowerStar::requestAppear, its appear
cutscene) unless one is out already, have a brother take it (the second player on even missions, the first on
odd ones, in co-op), and press A through the star-get scene, the dome's "You got a Star!" and any save prompt
until the game is playable again. Records ok / no-star / not-taken / stuck / crash, with screenshots; a crash or
hang relaunches the game and the sweep carries on. Results go to <out>/results.json and <out>/report.md.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nebula_agent  # noqa: E402
import sweep  # noqa: E402

POWER_STAR_VTABLE = '80587430'      # __vt__9PowerStar
POWER_STAR_REQUEST_APPEAR = 0x8020EE98  # PowerStar::requestAppear()


def stars(game):
    reply = game.command('actors . 4000', timeout=60)
    return [a for a in reply.get('actors', []) if a.get('vtable') == POWER_STAR_VTABLE]


def wait_frames(game, frames):
    game.command('wait %d' % frames, timeout=30 + frames / 30)


def run_mission(game, stage, mission, taker, out):
    """One mission's star cutscenes; returns the result entry's fields."""
    s = game.goto(stage, mission, timeout=150.0)
    wait_frames(game, 120)
    found = stars(game)
    if not found:
        return {'result': 'no-star', 'detail': 'no PowerStar in the stage'}
    star = next((a for a in found if not a['dead']), None)
    appeared = False
    if star is None:
        star = found[0]
        game.command('call 0x%08X 0x%s' % (POWER_STAR_REQUEST_APPEAR, star['address']), timeout=30)
        appeared = True
        # The appear cutscene: up to 20 s for it to end and the star to be out.
        deadline = time.time() + 20
        while time.time() < deadline:
            wait_frames(game, 30)
            if not (game.status() or {}).get('demo'):
                break
        star = next((a for a in stars(game) if a['address'] == star['address']), star)
        if star['dead']:
            return {'result': 'no-star', 'detail': 'the star did not appear (requestAppear)'}
    game.screenshot(os.path.join(out, '%s-%d-star.png' % (stage, mission)))
    # Into the star (gravity may point any way, so no offset); if it is still there, again with a jump.
    game.command('teleport %d %s %s %s' % (taker, star['x'], star['y'], star['z']), timeout=30)
    deadline = time.time() + 90
    left = False
    shots = 0
    tries = 0
    calm_checks = 0
    while time.time() < deadline:
        wait_frames(game, 30)
        st = game.status() or {}
        if not left and not st.get('demo') and st.get('stage') == stage and tries < 4:
            tries += 1
            game.command('teleport %d %s %s %s' % (taker, star['x'], star['y'], star['z']), timeout=30)
            game.command('press %d A 6' % taker, timeout=30)
        if st.get('stage') != stage:
            left = True
        if left and shots == 0:
            game.screenshot(os.path.join(out, '%s-%d-after.png' % (stage, mission)))
            shots = 1
        if left:
            # Back and calm for 3 s: the dome may play story scenes one after another (the butler's report),
            # and leaving between them frees what the story sequence still holds.
            calm = st.get('in_game') and not st.get('demo') and st.get('stage') in ('AstroGalaxy', 'AstroDome')
            calm_checks = calm_checks + 1 if calm else 0
            if calm_checks >= 6:
                game.command('pointer 1 off', timeout=30)
                return {'result': 'ok', 'detail': '%s; back in %s%s' % (
                    'appeared, then taken' if appeared else 'taken', st.get('stage'),
                    '' if taker == 1 else ' (taken by the second player)')}
            # The result screens take A; the save prompt takes its Yes button, pointed at (right half, low).
            game.command('pointer 1 0.29 0.6', timeout=30)
            game.command('press 1 A 4', timeout=30)
    st = game.status() or {}
    shot = game.screenshot(os.path.join(out, '%s-%d-stuck.png' % (stage, mission)))
    if not left:
        return {'result': 'not-taken', 'detail': 'still in the stage 60 s after the teleport (demo %s)' % st.get('demo'),
                'screenshot': shot}
    return {'result': 'stuck', 'detail': 'not playable 60 s after leaving: stage %s, demo %s' % (
        st.get('stage'), st.get('demo')), 'screenshot': shot}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--solo', action='store_true')
    parser.add_argument('--only', nargs='*')
    parser.add_argument('--out', default=os.path.join(nebula_agent.DEFAULT_ROOT, 'cutscenes'))
    parser.add_argument('--instance', default='cutscenes')
    parser.add_argument('--run', default=nebula_agent.DEFAULT_RUN, help='the build folder')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    plan = sweep.missions(nebula_agent.DEFAULT_CONTENT)
    if args.only:
        plan = {k: v for k, v in plan.items() if k in args.only}
    # As the level sweep: hidden stars and the Observatory beyond mission 1 are not reachable by a jump.
    plan = {k: [m for m in v if m < 6] for k, v in plan.items() if k != 'AstroGalaxy'}
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
        lines = ['# Power Star cutscene sweep', '', '| Stage | Mission | Result | Detail |', '|---|---|---|---|']
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
    count = 0
    for stage, numbers in plan.items():
        for mission in numbers:
            if (stage, mission) in done:
                continue
            taker = 1 if args.solo or count % 2 else 2
            count += 1
            entry = {'stage': stage, 'mission': mission, 'taker': taker}
            began = time.time()
            try:
                if not game.running():
                    start()
                entry.update(run_mission(game, stage, mission, taker, args.out))
            except nebula_agent.GameError as error:
                text = str(error)
                crash = game.crash()
                entry['result'] = 'crash' if crash else ('hang' if 'stopped advancing' in text else 'error')
                entry['detail'] = ' '.join(l for l in crash if 'guest pc' in l or 'guest-stack' in l) or text
                try:
                    start()
                except nebula_agent.GameError as again:
                    entry['detail'] += ' (relaunch failed: %s)' % again
            if entry['result'] in ('stuck', 'not-taken'):
                start()  # a clean game for the next mission
            entry['seconds'] = round(time.time() - began, 1)
            results.append(entry)
            save()
            print('%-28s %d  %-9s %s' % (stage, mission, entry['result'], entry.get('detail', '')[:120]), flush=True)
    game.stop()
    save()


if __name__ == '__main__':
    main()
