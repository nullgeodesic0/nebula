"""Agent play testing: run a test copy of the game and drive it (docs/AGENT_TESTING.md).

The game reads commands from <instance>/agent/commands.txt and answers in replies.jsonl, with status.json
rewritten twice a second (runtime/include/galaxy/agent.h). This module launches an instance in its own folder
(its own copy of the saves, settings and shader cache, never the real ones), sends commands, waits on the
game's state, takes screenshots and reads crashes from the log.

Command line:  py -3 tools/agent/nebula_agent.py <command> [arguments]   (--help lists them)
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib

DEFAULT_RUN = r'C:\nb\run-mp'          # the built game: NebulaRuntime.exe and the RMGE01 modules
DEFAULT_CONTENT = r'C:\nb\content-mp'  # the game's files
DEFAULT_ROOT = r'C:\nb\agent-mp'       # where test instances live

# Galaxies by the names players use; the game's stage names work as well.
GALAXIES = {
    'observatory': 'AstroGalaxy',
    'good egg': 'EggStarGalaxy',
    'honeyhive': 'HoneyBeeKingdomGalaxy',
    'loopdeeloop': 'SurfingLv1Galaxy',
    'flipswitch': 'FlipPanelExGalaxy',
    'robot reactor': 'TriLegLv1Galaxy',
    'space junk': 'StarDustGalaxy',
    'battlerock': 'BattleShipGalaxy',
    'rolling green': 'TamakoroExLv1Galaxy',
    'hurry-scurry': 'BreakDownPlanetGalaxy',
    'bowser star reactor': 'KoopaBattleVs1Galaxy',
    'beach bowl': 'HeavenlyBeachGalaxy',
    'ghostly': 'PhantomGalaxy',
    'gusty garden': 'CosmosGardenGalaxy',
    'freezeflame': 'IceVolcanoGalaxy',
    'dusty dune': 'SandClockGalaxy',
    'gold leaf': 'ReverseKingdomGalaxy',
    'sea slide': 'OceanRingGalaxy',
    'toy time': 'FactoryGalaxy',
    'deep dark': 'OceanPhantomCaveGalaxy',
    'dreadnought': 'DarkRoomGalaxy',
    'melty molten': 'HellProminenceGalaxy',
}


def stage_name(name):
    return GALAXIES.get(name.lower(), name)


class GameError(RuntimeError):
    pass


class Instance:
    """One test copy of the game, in <root>/<name>."""

    def __init__(self, name='default', root=DEFAULT_ROOT, run=DEFAULT_RUN, content=DEFAULT_CONTENT):
        self.name = name
        self.dir = os.path.join(root, name)
        self.run = run
        self.content = content
        self.agent_dir = os.path.join(self.dir, 'agent')
        self.logs = os.path.join(self.dir, 'logs')
        self._id_file = os.path.join(self.dir, 'next_id.txt')
        self._pid_file = os.path.join(self.dir, 'pid.txt')

    # ---- setup and process ----

    def prepare(self, fresh_saves=False):
        """The instance folder: saves and settings copied from the run folder the first time (or fresh)."""
        os.makedirs(self.agent_dir, exist_ok=True)
        os.makedirs(self.logs, exist_ok=True)
        saves = os.path.join(self.dir, 'saves')
        if fresh_saves and os.path.isdir(saves):
            shutil.rmtree(saves)
        if not os.path.isdir(saves):
            shutil.copytree(os.path.join(self.run, 'saves'), saves)
        for settings in ('runtime_settings.ini', 'runtime_settings_solo.ini'):
            target = os.path.join(self.dir, settings)
            source = os.path.join(self.run, settings)
            if not os.path.exists(target) and os.path.exists(source):
                shutil.copy2(source, target)
        cache = os.path.join(self.dir, 'shadercache')
        if not os.path.isdir(cache) and os.path.isdir(os.path.join(self.run, 'shadercache')):
            shutil.copytree(os.path.join(self.run, 'shadercache'), cache)

    def pid(self):
        try:
            with open(self._pid_file) as f:
                return int(f.read().strip())
        except (OSError, ValueError):
            return None

    def running(self):
        pid = self.pid()
        if pid is None:
            return False
        out = subprocess.run(['tasklist', '/FI', 'PID eq %d' % pid, '/FO', 'CSV', '/NH'],
                             capture_output=True, text=True).stdout
        return 'NebulaRuntime' in out

    def launch(self, coop=True, split=True, audio=False, fresh_saves=False, show=False, extra_env=None):
        """Starts the game; returns its process id. Without `show` it runs with no window at all
        (GALAXY_HIDE_WINDOW): only screenshots see it."""
        if self.running():
            raise GameError('instance %s is already running (pid %d)' % (self.name, self.pid()))
        self.prepare(fresh_saves)
        for name in ('commands.txt', 'replies.jsonl', 'status.json'):
            path = os.path.join(self.agent_dir, name)
            if os.path.exists(path):
                os.remove(path)
        open(os.path.join(self.agent_dir, 'commands.txt'), 'w').close()
        with open(self._id_file, 'w') as f:
            f.write('1')
        env = dict(os.environ)
        env.update({
            'GALAXY_AGENT_DIR': self.agent_dir,
            'GALAXY_NAND_ROOT': os.path.join(self.dir, 'saves'),
            'GALAXY_RUNTIME_SETTINGS_PATH': os.path.join(
                self.dir, 'runtime_settings.ini' if coop else 'runtime_settings_solo.ini'),
            'GALAXY_INSTANCE_LEASE': 'nebula-agent-' + self.name,
            'GALAXY_TWO_PLAYERS': '1' if coop else '0',
            'GALAXY_COOP_SPLIT': '1' if split else '0',
            'GALAXY_SHADER_CACHE_DIR': os.path.join(self.dir, 'shadercache'),
            'GALAXY_GPU_VERTEX_RING_MB': '256',
            'GALAXY_GPU_INDEX_RING_MB': '64',
            'GALAXY_GPU_CONSTANT_RING_MB': '32',
            'GALAXY_TRACE_COOP': '1',
            # A controller in use for another game must not play this one too.
            'GALAXY_XINPUT_REQUIRE_FOCUS': '1',
        })
        if not audio:
            env['GALAXY_AUDIO_DISABLE'] = '1'
        if not show:
            env['GALAXY_HIDE_WINDOW'] = '1'
        env.update(extra_env or {})
        stamp = time.strftime('%Y%m%d-%H%M%S')
        self.stdout_path = os.path.join(self.logs, 'out-%s.log' % stamp)
        self.stderr_path = os.path.join(self.logs, 'run-%s.log' % stamp)
        with open(os.path.join(self.dir, 'last_log.txt'), 'w') as f:
            f.write(self.stderr_path)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 4  # SW_SHOWNOACTIVATE
        process = subprocess.Popen(
            [os.path.join(self.run, 'NebulaRuntime.exe'), self.content, os.path.join(self.run, 'RMGE01_game.dll')],
            cwd=self.dir, env=env, stdout=open(self.stdout_path, 'wb'), stderr=open(self.stderr_path, 'wb'),
            startupinfo=startup, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        with open(self._pid_file, 'w') as f:
            f.write(str(process.pid))
        return process.pid

    def stop(self, timeout=10.0):
        """Asks the game to quit; ends this instance's own process if it does not."""
        if not self.running():
            return 'not running'
        try:
            self.command('quit', timeout=timeout)
        except GameError:
            pass
        deadline = time.time() + timeout
        while time.time() < deadline and self.running():
            time.sleep(0.25)
        if self.running():
            subprocess.run(['taskkill', '/PID', str(self.pid()), '/T', '/F'], capture_output=True)
            return 'ended'
        return 'quit'

    # ---- commands ----

    def _next_id(self):
        try:
            with open(self._id_file) as f:
                value = int(f.read().strip() or '1')
        except (OSError, ValueError):
            value = 1
        with open(self._id_file, 'w') as f:
            f.write(str(value + 1))
        return value

    def replies(self):
        path = os.path.join(self.agent_dir, 'replies.jsonl')
        out = []
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            out.append(json.loads(line))
                        except ValueError:
                            pass
        except OSError:
            pass
        return out

    def send(self, text):
        """Queues a command; returns its id."""
        command_id = self._next_id()
        with open(os.path.join(self.agent_dir, 'commands.txt'), 'a', encoding='utf-8') as f:
            f.write('%d %s\n' % (command_id, text))
        return command_id

    def command(self, text, timeout=30.0):
        """Sends a command and waits for its reply (a dict with "ok" and the command's fields)."""
        command_id = self.send(text)
        deadline = time.time() + timeout
        while time.time() < deadline:
            for r in self.replies():
                if r.get('id') == command_id:
                    return r
            if not self.running():
                raise GameError('the game is not running%s' % self._crash_suffix())
            time.sleep(0.05)
        raise GameError('no reply to "%s" within %.0f s (the game may be loading or hung): %s'
                        % (text, timeout, self.status_line()))

    def status(self):
        try:
            with open(os.path.join(self.agent_dir, 'status.json'), encoding='utf-8') as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def status_line(self):
        s = self.status()
        if s is None:
            return 'no status yet'
        return 'frame %s, scene %s, stage %s mission %s, in game %s' % (
            s.get('frame'), s.get('scene'), s.get('stage'), s.get('mission'), s.get('in_game'))

    def wait_for(self, predicate, timeout=60.0, what='the condition'):
        """Waits until predicate(status) holds; raises on a crash, a hang (no new frame for 20 s) or timeout."""
        deadline = time.time() + timeout
        last_frame, last_change = None, time.time()
        while time.time() < deadline:
            s = self.status()
            if s is not None:
                if predicate(s):
                    return s
                if s.get('frame') != last_frame:
                    last_frame, last_change = s.get('frame'), time.time()
                elif time.time() - last_change > 20.0:
                    raise GameError('the game stopped advancing at frame %s: %s' % (last_frame, self.status_line()))
            if not self.running():
                raise GameError('the game exited while waiting for %s%s' % (what, self._crash_suffix()))
            time.sleep(0.25)
        raise GameError('timed out waiting for %s: %s' % (what, self.status_line()))

    # ---- high-level steps ----

    def playable(self, s):
        """In a stage (not the file select), past its intro, no cutscene or event camera, the first player in
        control."""
        players = s.get('players') or []
        return bool(s.get('in_game')) and s.get('stage') != 'FileSelect' and not s.get('demo') \
            and not s.get('event_camera') and not s.get('intro') and len(players) >= 1 and players[0] is not None \
            and not players[0].get('input_disabled')

    def boot(self, file=1, timeout=180.0):
        """Loads a save file straight from the logo (the `file` command: no menus) and waits until the player can
        move in its first scene (the Comet Observatory)."""
        self.wait_for(lambda s: s.get('frame', 0) > 0, timeout=60.0, what='the game to start')
        r = self.command('file %d' % file, timeout=timeout)
        if not r.get('ok'):
            raise GameError('file %d: %s' % (file, r.get('error')))
        return self.wait_for(self.playable, timeout=timeout, what='the first scene')

    def boot_through_menus(self, timeout=180.0):
        """The same through the title and file select with the pointer (file 1; positions at 16:9)."""
        deadline = time.time() + timeout
        self.wait_for(lambda s: s.get('frame', 0) > 0, timeout=60.0, what='the game to start')
        while time.time() < deadline:
            s = self.status() or {}
            if s.get('in_game') and s.get('stage') not in ('FileSelect', ''):
                break
            if s.get('stage') != 'FileSelect':
                self.command('press 1 A 6')          # the wrist strap screen
            else:
                self.command('press 1 A+B 6')        # the title
                self.command('wait 90')
                self.command('pointer -0.25 0.3')    # file 1 (pointer y is down-positive)
                self.command('wait 20')
                self.command('press 1 A 6')
                self.command('wait 90')
                self.command('pointer 0.52 0.78')    # Play This File
                self.command('wait 20')
                self.command('press 1 A 6')
                self.command('pointer off')
            time.sleep(2.0)
        self.command('release 1')
        return self.wait_for(self.playable, timeout=max(10.0, deadline - time.time()), what='the first scene')

    def goto(self, stage, mission, timeout=120.0):
        """Into a galaxy's mission; waits until the player can move there."""
        stage = stage_name(stage)
        r = self.command('stage %s %d' % (stage, mission))
        if not r.get('ok'):
            raise GameError('stage refused: %s' % r.get('error'))
        self.wait_for(lambda s: s.get('stage') != stage or not s.get('in_game') or s.get('mission') != mission,
                      timeout=20.0, what='the stage change to begin')
        return self.wait_for(lambda s: s.get('stage') == stage and s.get('mission') == mission and self.playable(s),
                             timeout=timeout, what='%s mission %d to be playable' % (stage, mission))

    def screenshot(self, png_path, timeout=10.0, max_width=960):
        """The next presented frame as a PNG (scaled to max_width)."""
        ppm = os.path.splitext(os.path.abspath(png_path))[0] + '.ppm'
        if os.path.exists(ppm):
            os.remove(ppm)
        r = self.command('screenshot %s' % ppm)
        if not r.get('ok'):
            raise GameError('screenshot refused: %s' % r.get('error'))
        deadline = time.time() + timeout
        size = -1
        while time.time() < deadline:
            if os.path.exists(ppm):
                now = os.path.getsize(ppm)
                if now > 0 and now == size:
                    break
                size = now
            time.sleep(0.2)
        else:
            raise GameError('no frame was presented for the screenshot')
        ppm_to_png(ppm, png_path, max_width)
        os.remove(ppm)
        return png_path

    # ---- logs and crashes ----

    def log_path(self):
        try:
            with open(os.path.join(self.dir, 'last_log.txt')) as f:
                return f.read().strip()
        except OSError:
            return None

    def log_tail(self, lines=60, pattern=None):
        path = self.log_path()
        if not path or not os.path.exists(path):
            return []
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read().splitlines()
        if pattern:
            text = [l for l in text if pattern in l]
        return text[-lines:]

    def crash(self):
        """The [crash] lines of the last run, if it crashed."""
        return [l for l in self.log_tail(100000) if l.startswith('[crash] ') and 'fastmem-hot' not in l]

    def _crash_suffix(self):
        lines = self.crash()
        return (':\n' + '\n'.join(lines[:8])) if lines else ''


def ppm_to_png(ppm_path, png_path, max_width=960):
    with open(ppm_path, 'rb') as f:
        data = f.read()
    # P6 header: magic, width, height, maxval, each separated by whitespace.
    fields, at = [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b'#':
            at = data.index(b'\n', at) + 1
            continue
        start = at
        while not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    at += 1
    if fields[0] != b'P6':
        raise GameError('not a P6 image')
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at:at + width * height * 3]
    step = max(1, -(-width // max_width))
    out_w, out_h = width // step, height // step
    rows = bytearray()
    for y in range(out_h):
        row = pixels[(y * step) * width * 3:(y * step + 1) * width * 3]
        rows.append(0)
        if step == 1:
            rows += row
        else:
            for x in range(out_w):
                rows += row[x * step * 3:x * step * 3 + 3]

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', out_w, out_h, 8, 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b'')
    with open(png_path, 'wb') as f:
        f.write(png)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Drive a test copy of the game (docs/AGENT_TESTING.md).')
    parser.add_argument('--instance', default='default')
    parser.add_argument('--run', default=DEFAULT_RUN, help='the build folder (NebulaRuntime.exe and modules)')
    sub = parser.add_subparsers(dest='action', required=True)
    p = sub.add_parser('launch', help='start the game')
    p.add_argument('--solo', action='store_true', help='one player (co-op off)')
    p.add_argument('--audio', action='store_true')
    p.add_argument('--fresh-saves', action='store_true')
    p.add_argument('--show', action='store_true', help='open a window (default: none, screenshots only)')
    sub.add_parser('stop', help='quit the game')
    p = sub.add_parser('boot', help='load a save file (no menus) and wait until the player can move')
    p.add_argument('--file', type=int, default=1)
    sub.add_parser('status', help='the latest status.json')
    p = sub.add_parser('cmd', help='send one command and print the reply')
    p.add_argument('text', nargs='+')
    p.add_argument('--timeout', type=float, default=30.0)
    p = sub.add_parser('goto', help='into a galaxy mission and wait until playable')
    p.add_argument('stage')
    p.add_argument('mission', type=int)
    p = sub.add_parser('shot', help='screenshot to a PNG')
    p.add_argument('path')
    p = sub.add_parser('log', help='the last lines of the game log')
    p.add_argument('--lines', type=int, default=40)
    p.add_argument('--grep')
    sub.add_parser('crash', help='the crash report of the last run, if any')
    args = parser.parse_args(argv)
    game = Instance(args.instance, run=args.run)
    try:
        if args.action == 'launch':
            print('pid', game.launch(coop=not args.solo, audio=args.audio, fresh_saves=args.fresh_saves,
                                     show=args.show))
        elif args.action == 'boot':
            print(json.dumps(game.boot(args.file)))
        elif args.action == 'stop':
            print(game.stop())
        elif args.action == 'status':
            print(json.dumps(game.status(), indent=1), 'running' if game.running() else 'not running')
        elif args.action == 'cmd':
            print(json.dumps(game.command(' '.join(args.text), timeout=args.timeout)))
        elif args.action == 'goto':
            print(json.dumps(game.goto(args.stage, args.mission)))
        elif args.action == 'shot':
            print(game.screenshot(args.path))
        elif args.action == 'log':
            print('\n'.join(game.log_tail(args.lines, args.grep)))
        elif args.action == 'crash':
            print('\n'.join(game.crash()) or 'no crash')
    except GameError as error:
        print('error:', error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
