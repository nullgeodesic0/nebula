"""MCP server for agent play testing (docs/AGENT_TESTING.md): the game's test copy as tools.

Speaks MCP over stdio (JSON-RPC 2.0, one message a line) with no dependencies beyond Python 3. Registered for
Claude Code in .mcp.json; each tool drives an Instance from nebula_agent.py.
"""
import base64
import json
import os
import sys
import tempfile
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nebula_agent  # noqa: E402

PROTOCOL = '2025-06-18'
POWERUPS = ['normal', 'rainbow', 'fire', 'ice', 'bee', 'spring', 'boo', 'fly']


def instance(args):
    return nebula_agent.Instance(args.get('instance') or 'default')


def text(value):
    return {'content': [{'type': 'text', 'text': value if isinstance(value, str) else json.dumps(value, indent=1)}]}


def shot(game):
    path = os.path.join(tempfile.gettempdir(), 'nebula-agent-%s-%d.png' % (game.name, int(time.time() * 1000)))
    game.screenshot(path)
    with open(path, 'rb') as f:
        data = base64.b64encode(f.read()).decode('ascii')
    return {'type': 'image', 'data': data, 'mimeType': 'image/png'}, path


def tool_launch(args):
    game = instance(args)
    pid = game.launch(coop=not args.get('solo', False), split=args.get('split', True),
                      audio=args.get('audio', False), fresh_saves=args.get('fresh_saves', False))
    if not args.get('boot', True):
        return text('launched, pid %d' % pid)
    return text({'pid': pid, 'status': game.boot(int(args.get('file', 1)))})


def tool_stop(args):
    return text(instance(args).stop())


def tool_status(args):
    game = instance(args)
    return text({'running': game.running(), 'status': game.status()})


def tool_command(args):
    return text(instance(args).command(args['text'], timeout=float(args.get('timeout', 30))))


def tool_goto(args):
    return text(instance(args).goto(args['stage'], int(args['mission']), timeout=float(args.get('timeout', 120))))


def tool_screenshot(args):
    image, path = shot(instance(args))
    return {'content': [image, {'type': 'text', 'text': path}]}


def tool_powerup(args):
    force = ' force' if args.get('force') else ''
    return text(instance(args).command('powerup %d %s%s' % (int(args['player']), args['powerup'], force)))


def tool_teleport(args):
    return text(instance(args).command('teleport %d %s %s %s' % (int(args['player']), args['x'], args['y'], args['z'])))


def tool_input(args):
    game = instance(args)
    player, frames = int(args.get('player', 1)), int(args.get('frames', 30))
    if 'stick_x' in args or 'stick_y' in args:
        game.send('stick %d %s %s %d nowait' % (player, args.get('stick_x', 0), args.get('stick_y', 0), frames))
    if args.get('buttons'):
        game.send('press %d %s %d nowait' % (player, args['buttons'], frames))
    game.command('wait %d' % frames, timeout=frames / 30.0 + 30)
    return text({'status': game.status()})


def tool_log(args):
    return text('\n'.join(instance(args).log_tail(int(args.get('lines', 60)), args.get('grep'))))


def tool_crash(args):
    return text('\n'.join(instance(args).crash()) or 'no crash in the last run')


def tool_smoke(args):
    """Load a mission, give each player power-ups, walk them about, and report what broke."""
    game = instance(args)
    report = {'stage': nebula_agent.stage_name(args['stage']), 'mission': int(args['mission']), 'steps': []}
    images = []

    def step(name, fn):
        try:
            result = fn()
            report['steps'].append({'step': name, 'ok': True, 'result': result})
            return True
        except Exception as error:  # a crash or hang ends the test
            report['steps'].append({'step': name, 'ok': False, 'error': str(error)})
            return False

    ok = step('load', lambda: game.goto(args['stage'], int(args['mission'])))
    if ok and args.get('screenshot', True):
        ok = step('screenshot', lambda: images.append(shot(game)[0]) or 'taken')
    def give(player, power):
        reply = game.command('powerup %d %s' % (player, power))
        if not reply.get('ok'):
            return 'skipped: %s' % reply.get('error')  # not loaded in this stage
        game.command('wait 120')
        return 'taken'

    for power in args.get('powerups', []) if ok else []:
        for player in (1, 2):
            ok = step('powerup %d %s' % (player, power), lambda: give(player, power))
            if not ok:
                break
        if not ok:
            break
    if ok and args.get('walk', True):
        for x, y in ((0, 1), (1, 0), (0, -1), (-1, 0)):
            game.send('stick 2 %d %d 60 nowait' % (-x, -y))
            ok = step('walk %d %d' % (x, y), lambda: game.command('stick 1 %d %d 60' % (x, y)) and 'ok')
            if not ok:
                break
    if ok and args.get('screenshot', True):
        step('screenshot after', lambda: images.append(shot(game)[0]) or 'taken')
    report['crash'] = game.crash() if not game.running() else []
    report['passed'] = all(s['ok'] for s in report['steps']) and not report['crash']
    report['final_status'] = game.status()
    return {'content': images + [{'type': 'text', 'text': json.dumps(report, indent=1)}]}


def schema(properties, required=()):
    properties = dict(properties)
    properties['instance'] = {'type': 'string', 'description': 'Test copy name (default "default")'}
    return {'type': 'object', 'properties': properties, 'required': list(required)}


PLAYER = {'type': 'integer', 'enum': [1, 2], 'description': '1 Mario (first player), 2 Luigi (second player)'}
TOOLS = [
    ('game_launch', tool_launch, 'Start a test copy of the game in its own folder (copied saves, no audio, window '
     'opens without focus) and, by default, load save file 1 straight away (no menus) until the player can move.',
     schema({'solo': {'type': 'boolean', 'description': 'One player (co-op off)'},
             'split': {'type': 'boolean', 'description': 'Split screen (default true)'},
             'audio': {'type': 'boolean'},
             'fresh_saves': {'type': 'boolean', 'description': 'Recopy the saves from the run folder'},
             'boot': {'type': 'boolean', 'description': 'Load a save file to its first scene (default true)'},
             'file': {'type': 'integer', 'minimum': 1, 'maximum': 6, 'description': 'Save file (default 1)'}})),
    ('game_stop', tool_stop, 'Quit the test copy (ends only its own process).', schema({})),
    ('game_status', tool_status, 'Whether it runs, and its frame, scene, stage, mission, cutscene state and players '
     '(position, power-up, health).', schema({})),
    ('game_goto', tool_goto, 'Go straight into a galaxy mission (no dome or star select) and wait until the player '
     'can move. Stage: a game stage name (StarDustGalaxy) or a common name ("space junk").',
     schema({'stage': {'type': 'string'}, 'mission': {'type': 'integer', 'minimum': 1, 'maximum': 8},
             'timeout': {'type': 'number'}}, ['stage', 'mission'])),
    ('game_screenshot', tool_screenshot, 'The next presented frame as an image.', schema({})),
    ('game_powerup', tool_powerup, 'Give a player a power-up form. Refused when its model is not loaded in this '
     'stage unless force is set (forcing a missing model crashes: useful to test guards).',
     schema({'player': PLAYER, 'powerup': {'type': 'string', 'enum': POWERUPS}, 'force': {'type': 'boolean'}},
            ['player', 'powerup'])),
    ('game_teleport', tool_teleport, 'Move a player to a world position.',
     schema({'player': PLAYER, 'x': {'type': 'number'}, 'y': {'type': 'number'}, 'z': {'type': 'number'}},
            ['player', 'x', 'y', 'z'])),
    ('game_input', tool_input, 'Hold buttons and/or the stick for a player for some frames (60 a second), then '
     'return the status. Buttons: A B 1 2 PLUS MINUS UP DOWN LEFT RIGHT C Z joined with +; stick -1..1, y up.',
     schema({'player': PLAYER, 'buttons': {'type': 'string'}, 'stick_x': {'type': 'number'},
             'stick_y': {'type': 'number'}, 'frames': {'type': 'integer', 'minimum': 1}})),
    ('game_command', tool_command, 'Send a raw command (runtime/include/galaxy/agent.h): ping, state, file, stage, wait, '
     'press, stick, pointer, release, powerup, teleport, screenshot, quit.',
     schema({'text': {'type': 'string'}, 'timeout': {'type': 'number'}}, ['text'])),
    ('game_log', tool_log, 'The last lines of the game log, optionally only those containing a text.',
     schema({'lines': {'type': 'integer'}, 'grep': {'type': 'string'}})),
    ('game_crash', tool_crash, 'The crash report (guest pc, call stack) of the last run, if it crashed.', schema({})),
    ('level_smoke_test', tool_smoke, 'Load a mission, screenshot it, give both players each listed power-up, walk '
     'them about, and report each step, any crash and the final state.',
     schema({'stage': {'type': 'string'}, 'mission': {'type': 'integer'},
             'powerups': {'type': 'array', 'items': {'type': 'string', 'enum': POWERUPS}},
             'walk': {'type': 'boolean'}, 'screenshot': {'type': 'boolean'}}, ['stage', 'mission'])),
]


def handle(message):
    method = message.get('method')
    params = message.get('params') or {}
    if method == 'initialize':
        return {'protocolVersion': params.get('protocolVersion', PROTOCOL),
                'capabilities': {'tools': {}},
                'serverInfo': {'name': 'nebula-playtest', 'version': '0.1'}}
    if method == 'ping':
        return {}
    if method == 'tools/list':
        return {'tools': [{'name': n, 'description': d, 'inputSchema': s} for n, _, d, s in TOOLS]}
    if method == 'tools/call':
        for name, fn, _, _ in TOOLS:
            if name == params.get('name'):
                try:
                    return fn(params.get('arguments') or {})
                except nebula_agent.GameError as error:
                    return {'content': [{'type': 'text', 'text': 'error: %s' % error}], 'isError': True}
                except Exception:
                    return {'content': [{'type': 'text', 'text': traceback.format_exc()}], 'isError': True}
        raise KeyError('unknown tool %s' % params.get('name'))
    raise KeyError('unknown method %s' % method)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            continue
        if 'id' not in message:
            continue  # a notification
        try:
            response = {'jsonrpc': '2.0', 'id': message['id'], 'result': handle(message)}
        except KeyError as error:
            response = {'jsonrpc': '2.0', 'id': message['id'], 'error': {'code': -32601, 'message': str(error)}}
        sys.stdout.write(json.dumps(response) + '\n')
        sys.stdout.flush()


if __name__ == '__main__':
    main()
