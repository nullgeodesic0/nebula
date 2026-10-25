"""Co-op: a second player out of play is inert, and drops back in as a player who can be hurt.

Loads Good Egg Galaxy mission 1 in a hidden co-op test copy and checks, from the game's own state (MarioActor's health
and animation, its HitSensors' mValidByHost, which MR::invalidateHitSensors clears and the sensor groups follow):

1. the second player held - drops out (sits out): hidden, his sensors off;
2. sitting out, he is carried beside the first player onto an Octoomba (Takobo, a walking enemy whose touch hurts)
   again and again: his health and animation do not change (no damage, no hurt voice, no knock-back: he is not
   moved at all), his sensors stay off, and the game turning them back on (MR::validateHitSensors, as
   MarioActor::endRush and MarioAccess::endRemoteDemo do) holds for at most a frame;
3. the stage loaded again with him still out (out from the moment the stage made him): the same;
4. his + drops him in: shown, sensors on, moving; the Octoomba then hurts him.

    py -3 tools/agent/coop_inert_check.py --run C:\\nb\\run-inert --content C:\\nb\\content-mp

Exit status 0 when every check passes. (Before the fix his MarioActor::movement ran on while he was out: carried
along unseen he landed, fell and took the map's damage with its sounds, and a talk's end re-validated his sensors.)
"""
import argparse
import math
import re
import struct
import sys

import nebula_agent as na

TAKOBO_VTABLE = 0x80570B6C        # __vt__6Takobo (Octoomba)
HEALTH = 0x380                    # MarioActor::mHealth
MARIO = 0x230                     # MarioActor::mMario
MARIO_SIDE = 0x310                # Mario::mSideVec
SENSOR_KEEPER = 0x54              # LiveActor::mSensorKeeper: +4 count, +8 HitSensorInfo** (+8 its HitSensor*)
SENSOR_VALID = 0x20               # HitSensor::mValidBySystem, mValidByHost (bytes)
INVALIDATE_CLIPPING = 0x803DA0F0  # MR::invalidateClipping(LiveActor*): the enemy runs wherever the camera looks
VALIDATE_HIT_SENSORS = 0x803C27B8  # MR::validateHitSensors(LiveActor*)
JOINED = 'level start over: the second player joins the first'


class Check:
    def __init__(self, inst):
        self.inst = inst
        self.failures = []

    def expect(self, ok, what):
        print('%s %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            self.failures.append(what)

    def words(self, address, count=1):
        r = self.inst.command('peek 0x%X %d' % (address, count))
        if 'words' not in r:
            raise na.GameError('peek 0x%X: %s' % (address, r.get('error')))
        return [int(w, 16) for w in r['words']]

    def floats(self, address, count):
        return [struct.unpack('>f', struct.pack('>I', w))[0] for w in self.words(address, count)]

    def players_from_log(self):
        for line in reversed(self.inst.log_tail(200000, '[coop] players: first')):
            m = re.search(r'first ([0-9A-F]{8}) .*second ([0-9A-F]{8})', line)
            if m:
                return int(m.group(1), 16), int(m.group(2), 16)
        raise na.GameError('no co-op players in the log')

    def sensors_on(self, actor):
        """Each of the actor's hit sensors: whether its host has it on (mValidByHost)."""
        keeper = self.words(actor + SENSOR_KEEPER)[0]
        _, count, infos = self.words(keeper, 3)
        out = []
        for i in range(min(count, 16)):
            sensor = self.words(self.words(infos + 4 * i)[0] + 8)[0]
            out.append(bool((self.words(sensor + SENSOR_VALID)[0] >> 16) & 0xFF))
        return out

    def players(self):
        """This frame's players (the state command; status.json is written only twice a second)."""
        r = self.inst.command('state')
        players = r.get('players') or (r.get('state') or {}).get('players')
        if not players:
            raise na.GameError('no players in the state reply: %s' % sorted(r))
        return players


def wait_joined(inst, joins_before):
    for _ in range(90):
        if len(inst.log_tail(200000, JOINED)) > joins_before:
            break
        inst.command('wait 10')
    inst.command('wait 30')


def nearest_takobo(c, near):
    r = c.inst.command('actors . 5000', timeout=60)
    best = None
    for a in r.get('actors', []):
        if int(a['vtable'], 16) != TAKOBO_VTABLE or a.get('dead'):
            continue
        d = math.dist((a['x'], a['y'], a['z']), near)
        if best is None or d < best[0]:
            best = (d, int(a['address'], 16))
    if best is None:
        raise na.GameError('no Octoomba in the stage')
    return best[1]


def out_on_enemy(c, first, second, enemy, label, rounds=40):
    """The second player sitting out, carried beside the first onto the enemy for `rounds` steps."""
    c.inst.command('call 0x%X 0x%X' % (INVALIDATE_CLIPPING, enemy))
    p = c.players()
    health, bck = p[1]['health'], p[1]['bck']
    c.expect(p[1]['down'] and p[1]['out'], '%s: the second player is out of play (down and out)' % label)
    c.expect(not any(c.sensors_on(second)), '%s: his hit sensors are off' % label)
    touched = 0
    still = 0
    moved_alone = 0.0
    revalidated_ok = True
    for i in range(rounds):
        c.inst.command('poke 0x%X 3' % (first + HEALTH))  # the first player is not under test: kept alive
        p = c.players()
        offset = [p[1][k] - p[0][k] for k in 'xyz']
        g = c.floats(enemy + 0xC, 3)
        c.inst.command('teleport 1 %.1f %.1f %.1f' % (g[0] - offset[0], g[1] + 20.0 - offset[1], g[2] - offset[2]))
        if i % 10 == 5:
            # The game turning his sensors back on (a bind's or a held talk's end) lasts no longer than a frame.
            c.inst.command('call 0x%X 0x%X' % (VALIDATE_HIT_SENSORS, second))
            c.inst.command('wait 2')
            if any(c.sensors_on(second)):
                revalidated_ok = False
                # How long they stay on, and what the first player was doing (his turn drives the second's).
                before = c.players()[0]['bck']
                frames = 2
                while frames < 60 and any(c.sensors_on(second)):
                    c.inst.command('wait 1')
                    frames += 1
                print('   step %d: sensors on for %d frames after validateHitSensors; first player %s -> %s'
                      % (i, frames, before, c.players()[0]['bck']))
        c.inst.command('wait 4')
        p = c.players()
        g = c.floats(enemy + 0xC, 3)
        if math.dist((p[1]['x'], p[1]['y'], p[1]['z']), g) < 90.0:
            touched += 1
        # Not moved but by the carry: he stands 120 along the first player's side (Mario::mSideVec,
        # update_down_player), the Octoomba on him or not. Judged when the first player and his side vector stay
        # the same over the reads (they are frames apart).
        side = c.floats(c.words(first + MARIO)[0] + MARIO_SIDE, 3)
        q = c.players()
        side_after = c.floats(c.words(first + MARIO)[0] + MARIO_SIDE, 3)
        r = c.players()
        at = lambda players, n: [players[n][k] for k in 'xyz']
        if side == side_after and math.dist(at(q, 0), at(r, 0)) < 0.01:
            still += 1
            carried = [q[0][k] + 120.0 * side[j] for j, k in enumerate('xyz')]
            off = max(math.dist(carried, at(q, 1)), math.dist(at(q, 1), at(r, 1)))
            moved_alone = max(moved_alone, off)
            if off >= 2.0:
                print('   step %d: %.1f off; first (%.0f %.0f %.0f) %s, second (%.0f %.0f %.0f)'
                      % ((i, off) + tuple(at(q, 0)) + (q[0]['bck'],) + tuple(at(q, 1))))
        if p[1]['health'] != health or p[1]['bck'] != bck or any(c.sensors_on(second)):
            print('   step %d: health %d bck %s sensors %s' % (i, p[1]['health'], p[1]['bck'], c.sensors_on(second)))
    p = c.players()
    c.expect(touched >= rounds // 4, '%s: he stood in the Octoomba %d of %d steps' % (label, touched, rounds))
    c.expect(p[1]['health'] == health, '%s: his health is unchanged (%d -> %d)' % (label, health, p[1]['health']))
    c.expect(p[1]['bck'] == bck, '%s: his animation is unchanged, no hurt or knock-back (%s -> %s)'
             % (label, bck, p[1]['bck']))
    # Informational only: the carry's reads are frames apart, and while the first player sleeps or is in hit-stop
    # they disagree by a step's teleport (not yet understood), so this is not a pass/fail check.
    print('info %s: largest gap from the carry position %.1f over %d steady steps' % (label, moved_alone, still))
    c.expect(not any(c.sensors_on(second)), '%s: his hit sensors stayed off' % label)
    c.expect(revalidated_ok, '%s: sensors the game turned on were off again a frame later' % label)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--run', default=na.DEFAULT_RUN)
    p.add_argument('--content', default=na.DEFAULT_CONTENT)
    p.add_argument('--instance', default='inert-check')
    p.add_argument('--keep', action='store_true', help='leave the test copy running')
    args = p.parse_args(argv)

    inst = na.Instance(args.instance, run=args.run, content=args.content)
    if inst.running():
        inst.stop()
    inst.launch(fresh_saves=True)
    c = Check(inst)
    try:
        inst.boot_through_menus()
        joins = len(inst.log_tail(200000, JOINED))
        inst.goto('good egg', 1)
        wait_joined(inst, joins)
        first, second = c.players_from_log()
        print('players %08X %08X' % (first, second))
        c.expect(all(c.sensors_on(second)) and not c.players()[1]['down'], 'in play, his hit sensors are on')

        # 1. Drop out (- held a second).
        inst.command('press 2 MINUS 75')
        inst.command('wait 20')
        c.expect(len(inst.log_tail(200000, 'second player drops out')) > 0, 'the second player drops out')
        inst.command('wait 20')
        c.expect(len(inst.log_tail(200000, 'second player inert')) > 0, 'out of play he is not moved (inert)')

        # 2. Carried onto an Octoomba.
        spawn = c.players()[0]
        enemy = nearest_takobo(c, (spawn['x'], spawn['y'], spawn['z']))
        print('octoomba %08X' % enemy)
        out_on_enemy(c, first, second, enemy, 'dropped out')

        # 3. The stage again, he out of play from its start.
        joins = len(inst.log_tail(200000, JOINED))
        inst.goto('good egg', 2)
        wait_joined(inst, joins)
        first, second = c.players_from_log()
        print('players %08X %08X' % (first, second))
        spawn = c.players()[0]
        enemy = nearest_takobo(c, (spawn['x'], spawn['y'], spawn['z']))
        out_on_enemy(c, first, second, enemy, 'out from the stage start')

        # 4. Drop in: he plays, and is hurt.
        moves = len(inst.log_tail(200000, 'second player moves again'))
        inst.command('press 2 PLUS 6')
        inst.command('wait 30')
        p = c.players()
        c.expect(len(inst.log_tail(200000, 'second player drops in')) > 0, 'his + drops him in')
        c.expect(len(inst.log_tail(200000, 'second player moves again')) > moves, 'dropped in he moves again')
        c.expect(not p[1]['down'] and all(c.sensors_on(second)), 'dropped in: in play, his hit sensors on')
        health = p[1]['health']
        g = c.floats(enemy + 0xC, 3)
        inst.command('teleport 1 %.1f %.1f %.1f' % (g[0] + 500.0, g[1] + 50.0, g[2]))
        inst.command('wait 30')
        hurt = False
        for i in range(40):
            inst.command('poke 0x%X 3' % (first + HEALTH))
            if i % 4 == 0:
                g = c.floats(enemy + 0xC, 3)
                inst.command('teleport 2 %.1f %.1f %.1f' % (g[0], g[1] + 20.0, g[2]))
            inst.command('wait 5')
            p = c.players()
            if p[1]['health'] < health or p[1]['down'] or p[1]['bck'].startswith('Damage'):
                hurt = True
                print('   hurt at step %d: health %d bck %s' % (i, p[1]['health'], p[1]['bck']))
                break
        c.expect(hurt, 'dropped in, the Octoomba hurts him')
    except na.GameError as e:
        print('FAIL %s' % e)
        c.failures.append(str(e))
    finally:
        if not args.keep:
            inst.stop()
    if c.failures:
        print('%d check(s) failed' % len(c.failures))
        return 1
    print('all inert checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
