"""Co-op poles: either brother climbs a pole, one at a time, and the game keeps running.

Loads Rolling Green Galaxy mission 1 (its pole is by the end of the Star Ball course) in a hidden co-op test copy
and checks, from the game's own state (MarioActor::_924, the sensor a player is bound to, and Pole::mRider):

1. the second player jumps onto the pole: he is bound and is its rider; his stick up climbs him (the first does
   not move); the first player's A does not let him go; his own A jumps him off;
2. the first player on the pole: the second jumping in is not bound (the pole holds one rider); the first climbs and
   jumps off as in one-player play;
3. the second player alone at the pole with the first far off: he still grabs it (its grab sensor runs at the brother
   nearer it);
4. the game is still running a minute later.

    py -3 tools/agent/coop_pole_check.py --run C:\\nb\\run-pole --content C:\\nb\\content-mp-nomods

Exit status 0 when every check passes. (Before the fix, case 1 left the second player stuck on the pole in his run
animation, and the first player's A then crashed the game.)
"""
import argparse
import struct
import sys
import time

import nebula_agent as na

POLE_VTABLE = 0x805BB430   # __vt__4Pole
RUSH_SENSOR = 0x924        # MarioActor::_924 (HitSensor*), its host at +0x24
SENSOR_HOST = 0x24
POLE_BASE = 0x8C           # Pole::mBasePos
POLE_RIDER = 0xB8          # Pole::mRider


class Check:
    def __init__(self, inst):
        self.inst = inst
        self.failures = []
        self.first, self.second = self.player_actors()

    def word(self, address):
        r = self.inst.command('peek 0x%X 1' % address)
        return int(r['words'][0], 16)

    def floats(self, address, count):
        r = self.inst.command('peek 0x%X %d' % (address, count))
        return [struct.unpack('>f', struct.pack('>I', int(w, 16)))[0] for w in r['words']]

    def player_actors(self):
        import re
        for line in reversed(self.inst.log_tail(100000, '[coop] players: first')):
            m = re.search(r'first ([0-9A-F]{8}) .*second ([0-9A-F]{8})', line)
            if m:
                return int(m.group(1), 16), int(m.group(2), 16)
        raise na.GameError('no co-op players in the log')

    def bound(self, actor):
        sensor = self.word(actor + RUSH_SENSOR)
        return self.word(sensor + SENSOR_HOST) if sensor else 0

    def players(self):
        s = self.inst.status() or {}
        return [(p['x'], p['y'], p['z']) if p else None for p in s.get('players') or []]

    def expect(self, ok, what):
        print('%s %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            self.failures.append(what)

    def wait_bound(self, actor, pole, frames=200):
        for _ in range(frames // 5):
            self.inst.command('wait 5')
            if self.bound(actor) == pole:
                return True
        return False

    def free_both(self, pole):
        for player, actor in ((1, self.first), (2, self.second)):
            if self.bound(actor) == pole:
                self.inst.command('press %d A 6' % player)
                self.inst.command('wait 30')


def find_pole(check):
    r = check.inst.command('actors . 3000', timeout=60)
    for a in r.get('actors', []):
        if int(a['vtable'], 16) == POLE_VTABLE and not a.get('dead'):
            return int(a['address'], 16)
    raise na.GameError('no pole in the stage')


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--run', default=na.DEFAULT_RUN)
    p.add_argument('--content', default=na.DEFAULT_CONTENT)
    p.add_argument('--instance', default='pole-check')
    p.add_argument('--keep', action='store_true', help='leave the test copy running')
    args = p.parse_args(argv)

    inst = na.Instance(args.instance, run=args.run, content=args.content)
    if inst.running():
        inst.stop()
    inst.launch(fresh_saves=True)
    try:
        inst.boot_through_menus()
        inst.goto('rolling green', 1)
        c = Check(inst)
        spawn = c.players()[0]  # where the mission starts, far from the pole
        pole = find_pole(c)
        base = c.floats(pole + POLE_BASE, 3)
        print('pole %08X, base (%.0f %.0f %.0f); players %08X %08X' % ((pole,) + tuple(base) + (c.first, c.second)))
        bx, by, bz = base
        near = (bx - 400.0, by + 50.0, bz - 400.0)   # beside the pole, on the ground
        grab = (bx + 60.0, by + 250.0, bz)           # in the air at the pole: falling into it grabs it

        # 1. The second player on the pole.
        inst.command('teleport 1 %.0f %.0f %.0f' % near)
        inst.command('wait 60')
        inst.command('teleport 2 %.0f %.0f %.0f' % grab)
        c.expect(c.wait_bound(c.second, pole), 'second player grabs the pole')
        c.expect(c.word(pole + POLE_RIDER) == c.second, 'the pole\'s rider is the second player')
        inst.command('wait 60')
        before = c.players()
        inst.command('stick 2 0 1 90')
        after = c.players()
        c.expect(after[1][1] > before[1][1] + 100.0, 'second player climbs with his stick (%.0f -> %.0f)'
                 % (before[1][1], after[1][1]))
        c.expect(abs(after[0][1] - before[0][1]) < 5.0, 'the first player stays where he stood')
        inst.command('press 1 A 6')
        inst.command('wait 30')
        c.expect(c.bound(c.second) == pole and c.word(pole + POLE_RIDER) == c.second,
                 'the first player\'s A does not let the second go')
        inst.command('press 2 A 6')
        inst.command('wait 60')
        c.expect(c.bound(c.second) == 0 and c.word(pole + POLE_RIDER) == 0, 'second player jumps off with his A')

        # 2. The first player on the pole; the second is refused.
        inst.command('teleport 2 %.0f %.0f %.0f' % near)
        inst.command('wait 90')
        inst.command('teleport 1 %.0f %.0f %.0f' % grab)
        c.expect(c.wait_bound(c.first, pole), 'first player grabs the pole')
        inst.command('wait 30')
        inst.command('teleport 2 %.0f %.0f %.0f' % (bx - 60.0, by + 250.0, bz))
        inst.command('wait 90')
        c.expect(c.bound(c.second) != pole, 'second player is not bound to the first player\'s pole')
        c.expect(c.bound(c.first) == pole and c.word(pole + POLE_RIDER) == c.first, 'the first player stays its rider')
        before = c.players()
        inst.command('stick 1 0 1 90')
        after = c.players()
        c.expect(after[0][1] > before[0][1] + 100.0, 'first player climbs with his stick (%.0f -> %.0f)'
                 % (before[0][1], after[0][1]))
        inst.command('press 1 A 6')
        inst.command('wait 60')
        c.expect(c.bound(c.first) == 0 and c.word(pole + POLE_RIDER) == 0, 'first player jumps off with his A')
        c.free_both(pole)

        # 3. The second player alone at the pole, the first far away.
        inst.command('teleport 1 %.0f %.0f %.0f' % spawn)
        inst.command('wait 120')
        inst.command('teleport 2 %.0f %.0f %.0f' % grab)
        c.expect(c.wait_bound(c.second, pole), 'second player grabs the pole with the first far away')
        inst.command('press 2 A 6')
        inst.command('wait 60')
        c.free_both(pole)

        # 4. Still running.
        start = (inst.status() or {}).get('frame', 0)
        deadline = time.time() + 60.0
        while time.time() < deadline:
            inst.command('wait 30', timeout=30)
            time.sleep(2.0)
        frame = (inst.status() or {}).get('frame', 0)
        c.expect(frame > start + 1800, 'the game runs on for a minute (frame %d -> %d)' % (start, frame))
    except na.GameError as e:
        print('FAIL %s' % e)
        return 1
    finally:
        if not args.keep:
            inst.stop()
    if c.failures:
        print('%d check(s) failed' % len(c.failures))
        return 1
    print('all pole checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
