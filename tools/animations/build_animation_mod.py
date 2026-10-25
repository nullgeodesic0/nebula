"""Builds Nebula's custom-animation mod from a Blender file of Mario animations.

    py -3 tools/animations/build_animation_mod.py --blend "C:/path/file.blend" --content C:/nb/build/content

Converts the mapped actions (MAP below) of the file's armature to Galaxy BCKs, puts them into the
player's MarioAnime.arc taken from the user's own game.pak, and writes a disc mod to
<content>/mods/animations: the new archive in an unused area of the disc image and a patched file
system entry pointing at it. Nebula loads it at start while General > Custom animations is on.
Everything is built on the user's machine from their own files; nothing from the game is shipped.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zipfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blendfile  # noqa: E402
import disc  # noqa: E402
import face_anim  # noqa: E402
import j3d  # noqa: E402
import retarget  # noqa: E402

# Galaxy animation in MarioAnime.arc -> (action in the .blend, scale of the body's translation).
# Jumps scale the body's own lift down: in Galaxy the jump height comes from Mario's physics, and the
# source's full lift would float him off his collision.
MAP = {
    'walk': ('Walk', 1.0),
    'walksoft': ('WalkSoft', 1.0),
    'run': ('Run', 1.0),
    'rundash': ('DashFast corrrer turbo', 1.0),  # ãƒ¡ã‚¿ãƒ«ãƒ€ãƒƒã‚·ãƒ¥: the moveset's fastest run band
    'walkbury': ('Dash', 1.0),  # æ³¥é«˜é€Ÿæ­©è¡Œ (fast walking in mud): the moveset's run band between Run and DashFast
    'wait': ('Wait oddysey idle', 1.0),
    'jump': ('Jump', 0.6),
    'jump2': ('Jump2', 0.6),
    # The flips keep the source's own root motion (scaled), which plays more like Odyssey than pinning the
    # body's centre as Galaxy's flips do.
    'jumproll': ('Jump3', 0.5, None, 'root'),
    'jumpback': ('JumpBack', 0.5, None, 'root'),
    'jumpbroad': ('JumpBroad', 0.6),
    'fall': ('Fall', 0.6),
    'land': ('Land', 1.0),
    'walljump': ('WallJump', 0.6),
    # Galaxy's wall slide continues the wall hang, facing the wall; Odyssey's WallSlideL faces away, so the
    # slide holds the converted hang's last pose instead.
    'wallslide': ('WallKeep', 0.6, -1),
    'wallkeep': ('WallKeep', 0.6),
    'hipdropstart': ('HipDropStart', 0.6),
    'hipdrop': ('HipDrop', 0.6),
    'hipdropland': ('HipDropLand', 1.0),
    'squatstart': ('SquatStart', 1.0),
    'squatwait': ('SquatWait', 1.0),
    'squatwalk': ('SquatWalk', 1.0),
    'squatend': ('SquatEnd', 1.0),
    'swimwait': ('SwimWait', 0.6),
    'swimbreast': ('SwimWalk', 0.6),
    'swimflutter': ('SwimWalk', 0.6),
    'swimbreastsurface': ('SwimSurface', 0.6),
    'swimfluttersurface': ('SwimSurface', 0.6),
    # For the Odyssey moveset (wt-moveset): the ground-pound jump plays ã‚¹ã‚«ã‚¤ãƒ©ãƒ–ã‚¸ãƒ£ãƒ³ãƒ— (JumpTwin), the dive
    # é£›ã³è¾¼ã¿ã‚¸ãƒ£ãƒ³ãƒ— (Dive), the ground roll ç©ºä¸­ä¸€å›žè»¢ (AirControl). JumpRoll, the obvious high jump, is the
    # triple jump's.
    'jumptwin': ('JumpHipDrop', 0.6),
    'dive': ('HeadSlidingStart', 1.0),  # Odyssey's dive: the headfirst lunge (its "Dive" is into water)
    'aircontrol': ('Rolling', 1.0),
    # The roll's pieces, on rare animations whose files only these names use (swimming, sliding, skating and
    # surfing moves, which then show them too): start, jump out of a roll, landing from it, end of the roll.
    'swimdashringstart': ('RollingStart', 1.0),
    'slidestomachrecover': ('RollingJump', 0.6),
    'icejumpland': ('RollingLand', 1.0),
    'surfland': ('RollingEnd', 1.0),
    # Ground pound into a roll: Odyssey goes from the ground pound's landing straight into the roll's start
    # (its LandRolling is the stand-up out of a roll).
    'icejumpstopland': ('RollingStart', 1.0),
    'rolling': ('JumpDashFast', 0.6),  # ãƒ€ãƒƒã‚·ãƒ¥ã‚¸ãƒ£ãƒ³ãƒ—: the jump at full running speed
    # The dive's belly slide (é£›ã³è¾¼ã¿æº–å‚™: waiting at the water's edge to dive). The dive's lunge ends on the
    # belly, so its landing goes straight into the slide.
    'divewait': ('HeadSliding', 1.0),
    # The dive's landing without Z (å¼•ãæˆ»ã—ç€åœ°, the landing after a Pull Star): Odyssey's roll-out, which
    # picks up from the belly slide's pose and tumbles forward onto the feet.
    'pullbackland': ('RollingStandUp', 1.0),
    # Round poles (Galaxy's trees) and square poles.
    'treeclimbstart': ('PoleCatch', 1.0),
    'treeclimb': ('PoleClimb', 1.0),
    'treewait': ('PoleWait', 1.0),
    'climbturn': ('PoleTurn', 1.0),
    'treefall': ('PoleFall', 1.0),
    'treehandstandstart': ('PoleHandStandStart', 1.0),
    'treehandstandwait': ('PoleHandStandWait', 1.0),
    'treehandstandturn': ('PoleHandStandTurn', 1.0),
    'treehandstandjump': ('PoleHandStandJump', 1.0),
    'treehandstandend': ('PoleHandStandEnd', 1.0),
    'squarepolestart': ('PoleCatch', 1.0),
    'squarepoleclimb': ('PoleClimb', 1.0),
    'squarepolewait': ('PoleWait', 1.0),
    'squarepoleturnl': ('PoleTurn', 1.0),
    'squarepoleturnr': ('PoleTurn', 1.0),
    'squarepolefall': ('PoleFall', 1.0),
    # Ledge hang.
    'hangstart': ('WallCatchStart', 1.0),
    'hangwait': ('WallCatch', 1.0),
    'hangup': ('WallCatchEnd', 1.0),
    # Swimming.
    'swimdive': ('SwimDive', 1.0),
    'landwaterdive': ('DiveInWater', 1.0),
    'swimdamagesmall': ('DamageSwim', 1.0),
    'swimdamagemiddle': ('DamageSwim', 1.0),
    'swimdamagemiddlesurface': ('DamageSwimSurface', 1.0),
    'swimdamagemiddlesurfaceland': ('DamageSwimSurfaceLand', 1.0),
    'swimdie': ('SwimDieOver', 1.0),
    # Ground.
    'brake': ('Brake', 1.0),
    'turn': ('Turn', 1.0),
    'jumpturn': ('JumpTurn', 0.6),
    'jumpturnland': ('LandTurn', 1.0),
    'push': ('Push empujar', 1.0),
    'sleep': ('Sleep', 1.0),
    'firerun': ('FireRun', 1.0),
    'firerunstart': ('FireRunStart', 1.0),
    'jumprollland': ('LandJump3 Yeah', 1.0),
    # Carrying.
    'carrystart': ('CarryUpStart', 1.0),
    'carrywait': ('CarryUp', 1.0),
    'throw': ('CarryUpThrow', 1.0),
    # Damage and death.
    'damagewait': ('DamageWait', 1.0),
    'diefall': ('DeadFall', 1.0),
    'diefire': ('DeadFire', 1.0),
    # The hard landing (ãƒãƒ¼ãƒ‰ç€åœ°, landstiffen: after a Launch Star and long falls) keeps Galaxy's own.
    # Getting a Power Star or Grand Star keeps Galaxy's own animations (powerstarget, grandstarget):
    # Odyssey's GetShine is a short pose that does not fit the game's long star-get sequence. So do the
    # pipes (earthenpipein, earthenpipeout): Odyssey's Dokan clips played wrong in them.
}
# One-shots whose source is far shorter than the game's animation: 'hold' plays the source at its own speed
# and holds its last frame (a grab, a pose), 'repeat' plays its cycle over and over (sleeping), instead of
# stretching it into slow motion.
FILL = {'treeclimbstart': 'hold', 'squarepolestart': 'hold', 'carrystart': 'hold', 'turn': 'hold',
        'sleep': 'repeat'}
# Body offsets (model units: x right, y up, z forward) moving Odyssey's grip onto where Galaxy puts the pole
# or ledge relative to Mario: Odyssey holds a pole further forward and hangs lower from a ledge.
_TREE, _HANDSTAND, _SQUARE, _HANG = (0.0, 7.0, -14.0), (0.0, 25.0, -6.0), (0.0, 0.0, -33.0), (0.0, 32.0, 0.0)
OFFSET = {**{n: _TREE for n in ('treeclimbstart', 'treeclimb', 'treewait', 'climbturn', 'treefall')},
          **{n: _HANDSTAND for n in ('treehandstandstart', 'treehandstandwait', 'treehandstandturn',
                                     'treehandstandjump', 'treehandstandend')},
          **{n: _SQUARE for n in ('squarepolestart', 'squarepoleclimb', 'squarepolewait', 'squarepoleturnl',
                                  'squarepoleturnr', 'squarepolefall')},
          **{n: _HANG for n in ('hangstart', 'hangwait', 'hangup')}}

# Expressions from the source's face animation. Galaxy picks Mario's face itself (the face model, which
# hangs from FacePosition); where a converted animation's source animates the mouth, the converted
# animation shows one of two extra faces in the body model instead, on head joints the game never moves:
# Eye0 a wide open grin (the jumps' "wahoo"), Cap0 a smile. The choice follows how far the source's mouth
# corners stretch. Every other animation keeps the two extra faces folded away.
FACE_JOINTS = {'game': 'FacePosition', 'open': 'Eye0', 'smile': 'Cap0'}
OPEN_STRETCH, SMILE_STRETCH = 3.0, 1.5
MIN_HOLD = 6
# The custom model's neutral face and hands, for animations that do not drive them: this action's first frame.
NEUTRAL_ACTION = 'Wait oddysey idle'
# Game animations written with another length: (frames, loop). The roll pieces keep Odyssey's own timing
# instead of being stretched onto their host animation's length.
FRAMES = {'aircontrol': (9, True),'swimdashringstart': (6, False), 'slidestomachrecover': (28, False),
          'icejumpland': (20, False), 'surfland': (12, False), 'icejumpstopland': (6, False),
          'rolling': (30, False), 'dive': (48, False), 'divewait': (20, True),
          'pullbackland': (18, False),
          # Loops at Odyssey's own speed rather than squeezed or stretched onto the game's cycle.
          'carrywait': (59, True), 'damagewait': (120, True), 'diefall': (180, True), 'diefire': (190, True)}
# The Odyssey moveset's animations (wt-moveset), which Luigi shares with Mario in co-op (--luigi-originals).
MOVESET = {'rundash', 'walkbury', 'jumptwin', 'dive', 'aircontrol', 'swimdashringstart', 'slidestomachrecover',
           'icejumpland', 'surfland', 'icejumpstopland', 'rolling', 'divewait', 'pullbackland'}
# Galaxy's own swap table for Luigi (Petari MarioAnimatorData luigiAnimeSwapTable): name -> luigi<name>.
LUIGI_OWN = ('run', 'jump', 'jumproll', 'jumpback', 'runend', 'spin', 'spinground', 'spaceflyshort', 'wait',
             'waitslopel', 'waitsloper', 'waitslopeu', 'waitsloped')


def replace_names(replace):
    return [n[:-4] for n in replace if n.endswith('.bck')]


# The custom model's joints without geometry (build_odyssey_model_mod.py UNSKINNED).
UNSKINNED = ('IndexFinger', 'MiddleFinger', 'ThirdFinger', 'LittleFinger', 'Thumb')
HIDDEN = 0.001

ROT_TOLERANCE = 0.35   # degrees
TRANS_TOLERANCE = 0.12  # model units (about 1 mm on Mario)
# The custom model's face joints: small bones whose keys would otherwise fill the archive, which the game
# loads into a fixed heap (co-op's second player leaves it less room).
ADDED_ROT_TOLERANCE = 1.0
ADDED_TRANS_TOLERANCE = 0.25
ARCHIVE = 'ObjectData/MarioAnime.arc'
MODEL = 'ObjectData/Mario.arc'
MAIN_DOL_SHA1 = '9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd'  # native_host.cpp kDiscModMainDolSha1


def expression_per_frame(act, frames):
    corners = [act['bones'].get(b, {}).get('location') for b in ('MouthCornerL', 'MouthCornerR')]
    corners = [c for c in corners if c is not None]
    labels = []
    for u in frames:
        f = min(int(round(u)), act['nf'] - 1)
        stretch = max((float(np.linalg.norm(c[f])) for c in corners), default=0.0)
        labels.append('open' if stretch > OPEN_STRETCH else 'smile' if stretch > SMILE_STRETCH else 'game')
    # No expression shorter than MIN_HOLD frames: a short one reads as a flicker.
    runs = []
    for label in labels:
        if runs and runs[-1][0] == label:
            runs[-1][1] += 1
        else:
            runs.append([label, 1])
    for i, run in enumerate(runs):
        if run[1] < MIN_HOLD and len(runs) > 1:
            run[0] = runs[i - 1][0] if i > 0 else runs[i + 1][0]
    return [label for label, n in runs for _ in range(n)]


def chain_actions(acts):
    """One action playing these one after another. A bone a piece does not key keeps its rest pose there."""
    rest = {'rotation_quaternion': [1.0, 0.0, 0.0, 0.0], 'location': [0.0, 0.0, 0.0]}
    bones = {}
    for name in {b for a in acts for b in a['bones']}:
        bones[name] = {}
        for kind in {k for a in acts for k in a['bones'].get(name, {})}:
            default = rest.get(kind, [1.0, 1.0, 1.0])
            bones[name][kind] = np.concatenate([
                np.asarray(a['bones'][name][kind]) if kind in a['bones'].get(name, {})
                else np.tile(default, (a['nf'], 1)) for a in acts])
    return {'nf': sum(a['nf'] for a in acts), 'bones': bones}


def step_keys(values):
    """Keys that hold each value and switch within a hundredth of a frame: the game plays between
    frames, and a slower switch shows the face shrinking or growing."""
    keys = [(0.0, values[0], 0.0)]
    for f in range(1, len(values)):
        if values[f] != values[f - 1]:
            keys += [(f - 0.01, values[f - 1], 0.0), (float(f), values[f], 0.0)]
    keys.append((float(len(values) - 1), values[-1], 0.0))
    out = []
    for k in keys:
        if not out or out[-1][0] != k[0]:
            out.append(k)
    return out if len(out) > 2 or out[0][1] != out[-1][1] else [out[0]]


def extend_game_animation(original, defaults, upto=None):
    """A copy of a game animation with tracks for the joints the custom model adds (its neutral pose),
    up to joint upto (all of them when None)."""
    ob = j3d.parse_bck(original)
    tracks = [[[(t, v, ti, to) for t, v, ti, to in j3d.track_keys(ob, j, k)] for k in range(9)]
              for j in range(len(ob['joints']))]
    tracks += [defaults[j] for j in sorted(defaults) if len(ob['joints']) <= j < (upto or 1 << 16)]
    return j3d.write_bck(original, ob['loop'], ob['dur'], tracks, ob['shift'])


def neutral_pose(rig, action, first):
    """Constant tracks for joints from irst on, from the first frame of ction (the idle)."""
    eul, tr, _ = retarget.sample(rig, action, 1, False, 1.0, 0)
    out = {}
    for j in range(first, rig.nj):
        out[j] = [[(0.0, 1.0, 0.0)] if k % 3 == 0 else
                  [(0.0, float(eul[0, j, k // 3]), 0.0)] if k % 3 == 1 else
                  [(0.0, float(tr[0, j, k // 3] + rig.rest_trans[j][k // 3]), 0.0)] for k in range(9)]
    return out


def hide_extra_faces(rig, original):
    """A copy of a game animation with the converted animations' extra faces folded away."""
    ob = j3d.parse_bck(original)
    tracks = [[[(t, v, ti, to) for t, v, ti, to in j3d.track_keys(ob, j, k)] for k in range(9)]
              for j in range(len(ob['joints']))]
    for label in ('open', 'smile'):
        j = rig.names.index(FACE_JOINTS[label])
        for axis in range(3):
            tracks[j][axis * 3] = [(0.0, HIDDEN, 0.0)]
    return j3d.write_bck(original, ob['loop'], ob['dur'], tracks, ob['shift'])


def convert(rig, act, original, faces=False, defaults=None, face=None, frames=None, fill=None, offset=None):
    ob = j3d.parse_bck(original)
    loop_mode = (2 if frames[1] else 0) if frames else ob['loop']
    loop = loop_mode == 2
    dur = frames[0] if frames else ob['dur']
    eul, tr, period = retarget.sample(rig, act[0], dur, loop, act[1], act[2] if len(act) > 2 else None,
                                      scale_root=len(act) > 3 and act[3] == 'root', fill=fill)
    if offset is not None:
        retarget.shift_body(rig, eul, tr, offset)
    if face is not None:
        # The face joints follow Odyssey's face animation for the same action, on the body's timing.
        driver, anim = face
        nf = max(act[0]['nf'] - 1, 1)
        face_eul, face_tr = driver.sample(anim, [min(u * anim.frames / nf, anim.frames)
                                                 for u in retarget.sample.source_frames])
        for j in driver.joints:
            eul[:, j] = face_eul[j]
            tr[:, j] = face_tr[j] - rig.rest_trans[j]
    labels = expression_per_frame(act[0], retarget.sample.source_frames) if faces else []
    face_scale = {} if not faces else {j: step_keys([1.0 if l == label else HIDDEN for l in labels])
                  for label, j in ((k, rig.names.index(v)) for k, v in FACE_JOINTS.items())}
    if faces:
        face_scale[rig.names.index(FACE_JOINTS['game'])] = step_keys([1.0 if l == 'game' else HIDDEN for l in labels])
    animated = {i for i, s in rig.map.items() if s in act[0]['bones']} | (set(face[0].joints) if face else set())
    tracks = []
    for j in range(rig.nj):
        if j >= len(ob['joints']) and (j not in animated or rig.names[j].startswith(UNSKINNED)):
            # A joint the game's animation does not have: the model's neutral pose. The fingers too: they have
            # no vertices (their pose is baked into the hands), so their motion would only cost archive room.
            tracks.append(defaults[j])
            continue
        added = j >= len(ob['joints'])
        rot_tol, trans_tol = (ADDED_ROT_TOLERANCE, ADDED_TRANS_TOLERANCE) if added else (ROT_TOLERANCE, TRANS_TOLERANCE)
        rot_keys = None
        if j in animated:
            rot_keys = [j3d.reduce_track(list(eul[:, j, a]), rot_tol) for a in range(3)]
            # Each Euler angle stays within tolerance, but near a quarter-turn pitch (a flip) small angle
            # errors make large rotation errors; such a joint keeps a key on every frame.
            for f in range(dur + 1):
                got = retarget.euler_to_mat(np.array([j3d.eval_track(
                    [(t, v, m, m) for t, v, m in rot_keys[a]], f) for a in range(3)]))
                want = retarget.euler_to_mat(eul[f, j])
                if np.degrees(np.arccos(np.clip((np.trace(got.T @ want) - 1) / 2, -1, 1))) > 3 * rot_tol + 0.45:
                    rot_keys = [j3d.reduce_track(list(eul[:, j, a]), 0.0) for a in range(3)]
                    break
        joint = []
        for axis in range(3):
            for comp in range(3):
                k = axis * 3 + comp
                if comp == 0 and j in face_scale:
                    keys = face_scale[j]
                elif comp == 1 and j in animated:
                    keys = rot_keys[axis]
                elif comp == 2 and j in animated:
                    keys = j3d.reduce_track(list(tr[:, j, axis] + rig.rest_trans[j][axis]), trans_tol)
                elif j >= len(ob['joints']):
                    keys = defaults[j][k]
                else:
                    # Scale, and every track of a joint the source does not drive, stays the game's.
                    keys = [(t, v, ti) for t, v, ti, _ in j3d.track_keys(ob, j, k)]
                joint.append(keys)
        tracks.append(joint)
    # Trailing joints that only hold the neutral pose are left out: Nebula gives any joint past an
    # animation's table that pose (neutral_joints.txt), and each joint costs the archive 54 bytes.
    while defaults is not None and len(tracks) > len(ob['joints']) and tracks[-1] == defaults.get(len(tracks) - 1):
        tracks.pop()
    return j3d.write_bck(original, loop_mode, dur, tracks), dur, loop, period, sorted(set(rig.map) - animated)


EXPORT_FORMAT = 'nebula-custom-animations 1'


def write_export(path, export, lines):
    """The converted animations without any game data: per variant and Galaxy animation, the ANK1 section the
    converter wrote. Building from it (--from-export) adds back the game's own BCK header from the player's
    disc, so no Blender file is needed."""
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr('manifest.json', json.dumps({
            'format': EXPORT_FORMAT, 'game': 'RMGE01', 'archive': ARCHIVE,
            'map': {k: list(v) for k, v in MAP.items()}, 'moveset': sorted(MOVESET),
            'variants': {f: sorted(v) for f, v in export['variants'].items()},
            'notes': export['notes'], 'lines': lines}, indent=1, ensure_ascii=False))
        for folder, anims in export['variants'].items():
            for name, ank in sorted(anims.items()):
                z.writestr('%s/%s.ank1' % (folder, name), ank)
        if export['neutral']:
            z.writestr('neutral_joints.txt', export['neutral'])
    print('exported', path, os.path.getsize(path), 'bytes')


def write_pack(folder, variants, notes):
    """An animation pack: a folder to drop into <content>/mods/animation-packs, which Nebula loads at boot
    (runtime animation_packs.cpp). Galaxy's model's variant as <name>.ank1, the custom model's in model/."""
    os.makedirs(folder, exist_ok=True)
    for sub in (folder, os.path.join(folder, 'model')):
        if os.path.isdir(sub):
            for f in os.listdir(sub):
                if f.endswith('.ank1'):
                    os.remove(os.path.join(sub, f))
    for variant, anims in variants.items():
        out = folder if variant == 'animations' else os.path.join(folder, 'model')
        os.makedirs(out, exist_ok=True)
        for name, ank in anims.items():
            with open(os.path.join(out, name + '.ank1'), 'wb') as f:
                f.write(ank)
    with open(os.path.join(folder, 'pack.txt'), 'w', newline='\n') as f:
        f.write('Nebula animation pack (%s): put this folder in <content>\\mods\\animation-packs and turn on\n'
                'F1 > General > Custom animations. Each .ank1 replaces the MarioAnime.arc animation of the same\n'
                'name; the rest of the archive comes from your own game.\n\n' % EXPORT_FORMAT)
        f.write('\n'.join(notes) + '\n')
    print('wrote pack', folder, sum(len(v) for v in variants.values()), 'animations')


def read_export(path, bcks):
    """From a file --export wrote: variants (folder -> (BCK name -> bytes, notes)) completed with the game's own
    BCK headers from bcks, the report's lines and the model's neutral pose."""
    with zipfile.ZipFile(path) as z:
        m = json.loads(z.read('manifest.json'))
        if m.get('format') != EXPORT_FORMAT:
            raise ValueError('%s is not a %s file' % (path, EXPORT_FORMAT))
        variants = {}
        for folder, names in m['variants'].items():
            replace = {}
            for name in names:
                ank = z.read('%s/%s.ank1' % (folder, name))
                head = bytearray(bcks[name + '.bck'][:32])
                struct.pack_into('>I', head, 8, 32 + len(ank))
                replace[name + '.bck'] = bytes(head) + ank
            variants[folder] = (replace, m['notes'].get(folder, []))
        neutral = z.read('neutral_joints.txt').decode() if 'neutral_joints.txt' in z.namelist() else None
    return variants, m['lines'] + ['(built from %s)' % os.path.basename(path)], neutral


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--blend', help='the .blend with the actions (or --from-export)')
    ap.add_argument('--export', help='also write the converted animations, without any game data, to this file '
                    '(a zip: one ANK1 section per animation and variant, the neutral pose, notes)')
    ap.add_argument('--from-export', help='build from a file written by --export instead of a .blend')
    ap.add_argument('--pack', help='also write the converted animations as an animation pack folder (for '
                    '<content>/mods/animation-packs, which Nebula loads at boot without a build step)')
    ap.add_argument('--content', required=True, help='Nebula content folder (holds game.pak)')
    ap.add_argument('--only', help='comma-separated Galaxy animation names to convert')
    ap.add_argument('--model', help='the custom model mod folder (mods/model): its skeleton, with any joints '
                    'it adds, is what the animations-model build animates')
    ap.add_argument('--face-anims', help="Odyssey's PlayerAnimationFace.szs: drives the model's face joints")
    ap.add_argument('--extend-game-animations', action='store_true',
                    help="give the game's own animations neutral tracks for the model's extra joints")
    ap.add_argument('--face-skeleton', help='an FBX of the face skeleton those animations are made for (its rest pose)')
    ap.add_argument('--luigi-originals', action='store_true',
                    help='co-op: keep Galaxy\'s clips for Luigi outside the Odyssey moveset (luigi_swap.txt)')
    args = ap.parse_args()
    if not args.blend and not args.from_export:
        ap.error('--blend or --from-export is needed')

    pak = disc.Pak(os.path.join(args.content, 'game.pak'))
    model = disc.rarc_files(disc.yaz0_decode(pak.read(MODEL)))['mario.bdl']
    joints, parents = j3d.bmd_joints(model)
    original_arc = pak.read(ARCHIVE)
    arc = disc.yaz0_decode(original_arc)
    bcks = disc.rarc_files(arc)

    if args.from_export:
        variants, lines, neutral = read_export(args.from_export, bcks)
        if args.pack:
            write_pack(args.pack, {f: {n[:-4]: b[32:] for n, b in r.items()} for f, (r, _) in variants.items()},
                       lines + variants.get('animations', ({}, []))[1])
        if neutral and args.model:
            with open(os.path.join(args.model, 'neutral_joints.txt'), 'w', newline='\n') as f:
                f.write(neutral)
        for folder, (replace, notes) in variants.items():
            if folder == 'animations-model' and not args.model:
                continue  # the custom model's variant only goes with that model
            write_variant(args, pak, arc, bcks, folder, replace, notes, lines, False)
        return
    export = {'variants': {}, 'notes': {}, 'neutral': None} if args.export or args.pack else None

    blend = blendfile.Blend(args.blend)
    order, bones = blendfile.read_armature(blend)
    rig = retarget.Rig(joints, parents, order, bones)
    wanted = {k: v for k, v in MAP.items() if not args.only or k in args.only.split(',')}
    parts = {p for v in wanted.values() for p in v[0].split('+')}
    actions = blendfile.read_actions(blend, parts | {NEUTRAL_ACTION})
    for name in {v[0] for v in wanted.values() if '+' in v[0]}:
        if all(p in actions for p in name.split('+')):
            actions[name] = chain_actions([actions[p] for p in name.split('+')])
    rig_model, defaults = rig, None
    if args.model:
        built = disc.rarc_files(disc.yaz0_decode(open(os.path.join(args.model, 'Mario.arc.yaz0'), 'rb').read()))
        mj, mp = j3d.bmd_joints(built['mario.bdl'])
        rig_model = retarget.Rig(mj, mp, order, bones)
        if rig_model.nj > rig.nj:
            defaults = neutral_pose(rig_model, actions[NEUTRAL_ACTION], rig.nj)
    driver = None
    if args.face_anims and defaults is not None:
        driver = face_anim.FaceDriver(rig_model, args.face_anims, args.face_skeleton)
        # The neutral face of every other animation: the first frame of Odyssey's idle face.
        e, t = driver.sample(driver.anim_for(NEUTRAL_ACTION), [0.0])
        for j in driver.joints:
            defaults[j] = [[(0.0, 1.0, 0.0)] if k % 3 == 0 else
                           [(0.0, float(e[j][0, k // 3]), 0.0)] if k % 3 == 1 else
                           [(0.0, float(t[j][0, k // 3]), 0.0)] for k in range(9)]

    if defaults is not None:
        # The model's neutral pose for its extra joints. Nebula gives it to a joint past the end of an
        # animation's joint table (the game's animations cover its 30 joints).
        with open(os.path.join(args.model, 'neutral_joints.txt'), 'w', newline='\n') as f:
            f.write('# joint  scale xyz  rotation xyz (degrees)  translation xyz; built by build_animation_mod.py\n')
            for j in sorted(defaults):
                v = [defaults[j][axis * 3 + comp][0][1] for comp in range(3) for axis in range(3)]
                f.write('%d %s\n' % (j, ' '.join('%.6g' % x for x in v)))
        if export is not None and driver is None:
            with open(os.path.join(args.model, 'neutral_joints.txt')) as f:
                export['neutral'] = f.read()
    lines = ['Custom animations: Galaxy animation <- action in %s' % os.path.basename(args.blend),
             'blend scene rate %s fps; rest-pose difference per joint (degrees): %s' % (
                 blendfile.scene_fps(blend),
                 ', '.join('%s %.0f' % kv for kv in rig.rest_difference().items())), '']
    # Two archives: "animations" for Galaxy's own model, and "animations-model" for the custom model, whose
    # extra faces the converted animations switch (Nebula picks the second while the custom model is on).
    # With a model that adds face joints, its faces come from those joints and no faces are switched.
    for folder, faces, frig in (('animations', False, rig), ('animations-model', defaults is None, rig_model)):
        replace = {}
        notes = []
        for name, entry in wanted.items():
            action = entry[0]
            if action not in actions:
                notes.append('%-20s skipped: no action %r' % (name, action))
                continue
            face = None
            if driver is not None and frig is rig_model and driver.anim_for(action) is not None:
                face = (driver, driver.anim_for(action))
            data, dur, loop, period, kept = convert(frig, (actions[action],) + tuple(entry[1:]),
                                                    bcks[name + '.bck'], faces,
                                                    defaults if frig is rig_model else None, face,
                                                    FRAMES.get(name), FILL.get(name), OFFSET.get(name))
            label = action
            if face is not None:
                kept = [i for i in kept if i not in driver.joints]
                label = '%s (face: %s)' % (action, face[1].name)
            replace[name + '.bck'] = data
            # The ANK1 section alone. The custom model's variant is left out when Odyssey's face animations drive
            # it: those are the game's data, not the .blend's.
            if export is not None and not (driver is not None and frig is rig_model):
                export['variants'].setdefault(folder, {})[name] = bytes(data[32:])
            if loop and len(entry) > 2 and entry[2] is not None:
                timing = 'held pose (source frame %s), game loop of %d' % ('last' if entry[2] < 0 else entry[2], dur)
            elif loop:
                timing = 'loop: one %d-frame cycle onto the game\'s %d' % (period, dur)
            elif name in FILL:
                timing = 'one-shot: %d source frames, game length %d (%s)' % (actions[action]['nf'], dur,
                                                                          'last frame held' if FILL[name] == 'hold'
                                                                          else 'cycle of %d repeated' % period)
            else:
                timing = 'one-shot: %d source frames, game length %d' % (actions[action]['nf'], dur)
            notes.append('%-20s <- %-20s %s%s' % (name, label, timing,
                         ('; game tracks kept for ' + ', '.join(frig.names[i] for i in kept)) if kept else ''))
        # Game animations keep their own joint count: an animation with fewer joints than the model leaves the
        # rest at their rest pose, and extending all of them would outgrow the heap the archive is loaded into.
        # The game's other animations carry tracks for its 30 joints only, and the game reads past the end of
        # an animation's joint table for the model's extra joints (spikes on the face and hands). They get
        # constant tracks holding the model's neutral pose.
        if frig is rig_model and defaults is not None and args.extend_game_animations:
            # Up to the first joint without geometry (the fingers): the heap the archive loads into holds
            # tracks for the face joints, not for all the added ones.
            upto = next((j for j in range(rig.nj, rig_model.nj)
                         if rig_model.names[j].startswith(UNSKINNED)), rig_model.nj)
            for name in bcks:
                if name.endswith('.bck') and name not in replace and len(bcks[name]) > 64:
                    replace[name] = extend_game_animation(bcks[name], defaults, upto)
        if faces:
            for name in bcks:
                if name.endswith('.bck') and name not in replace and len(bcks[name]) > 64:
                    replace[name] = hide_extra_faces(rig, bcks[name])
        if export is not None:
            export['notes'][folder] = notes
        write_variant(args, pak, arc, bcks, folder, replace, notes, lines, faces)
    if args.export:
        write_export(args.export, export, lines)
    if args.pack:
        write_pack(args.pack, export['variants'], lines + export['notes'].get('animations', []))


def write_variant(args, pak, arc, bcks, folder, replace, notes, lines, faces):
    """Writes <content>/mods/<folder>: the game's MarioAnime.arc with these animations replaced."""
    offset, size, entry_pos = pak.files[ARCHIVE]
    new_arc = disc.rarc_replace(arc, replace)
    swaps = []
    if args.luigi_originals:
        # Co-op: Luigi keeps Galaxy's animations where Mario has converted ones, except the Odyssey
        # moveset's. Galaxy gives Luigi his own clips through a swap table of BCK names (run -> luigirun);
        # the clips Mario's conversions replace are added as luigi<name>, and luigi_swap.txt lists the
        # whole table for the runtime to install for Luigi.
        own = {name: 'luigi' + name for name in LUIGI_OWN}
        added = {'luigi%s.bck' % name: bcks[name + '.bck'] for name in replace_names(replace)
                 if name not in MOVESET and name not in own and 'luigi%s.bck' % name not in bcks}
        new_arc = disc.rarc_add(new_arc, added)
        swaps = sorted(own.items()) + sorted((n[5:-4], n[:-4]) for n in added)
        notes += ['', 'Luigi (co-op): his own Galaxy clips for %d animations, Mario\'s converted ones for the '
                  'moveset (%s)' % (len(swaps), ', '.join(sorted(MOVESET & set(replace_names(replace)))))]
    packed = disc.yaz0_encode(new_arc)
    if disc.yaz0_decode(packed) != new_arc:
        raise RuntimeError('Yaz0 round trip failed')
    packed += b'\0' * ((-len(packed)) % 32)
    # Only one of the two is loaded at a time, so both use the same unused area of the disc.
    target = pak.filler_area(len(packed))
    entry = bytearray(pak.m[entry_pos:entry_pos + 12])
    struct.pack_into('>II', entry, 4, target >> 2, len(packed))

    out = os.path.join(args.content, 'mods', folder)
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, 'MarioAnime.arc.yaz0'), 'wb') as f:
        f.write(packed)
    with open(os.path.join(out, 'fst_marioanime.bin'), 'wb') as f:
        f.write(entry)
    with open(os.path.join(out, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('# Nebula custom animations, built by tools/animations/build_animation_mod.py\n'
                'version 1\ngame RMGE01\nmain_dol_sha1 %s\n' % MAIN_DOL_SHA1)
        f.write('0x%X 12 fst_marioanime.bin  # %s: 0x%X -> 0x%X\n' % (entry_pos, ARCHIVE, offset, target))
        f.write('0x%X %d MarioAnime.arc.yaz0\n' % (target, len(packed)))
    report = lines + notes + ['', 'archive %d bytes (game %d), at disc offset 0x%X; sha1 %s' % (
        len(packed), size, target, hashlib.sha1(packed).hexdigest())]
    with open(os.path.join(out, 'report.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(report) + '\n')
    swap_file = os.path.join(out, 'luigi_swap.txt')
    if swaps:
        with open(swap_file, 'w', newline='\n') as f:
            f.write('# Luigi\'s animation swap table (BCK name -> BCK name, without .bck), built by '
                    'build_animation_mod.py --luigi-originals\n')
            f.writelines('%s %s\n' % s for s in swaps)
    elif os.path.exists(swap_file):
        os.remove(swap_file)
    print('\n'.join(notes) if not faces else '', 'wrote', out)

if __name__ == '__main__':
    main()
