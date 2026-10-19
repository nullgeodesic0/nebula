"""Builds Nebula's custom-model mod from the Super Mario Odyssey Mario model (a Blender file on the Odyssey
player rig, with its textures), with real face and finger joints.

    py -3 tools/animations/build_odyssey_model_mod.py --blend C:/path/mario-model-FK.blend
        --textures C:/path/textures --anim-blend "C:/path/64  24.blend" --content C:/nb/content-anim

The model is scaled onto Galaxy Mario's rest skeleton (both are T-poses). Body bones fold onto Galaxy's 30
joints as before; the face bones (brows, cheeks, eyelids, lips, jaw, mouth corners, mustache, nose, tongue,
teeth) and the finger bones become new joints appended to Galaxy's skeleton, so the converted animations
(build_animation_mod.py --model) drive them from the Odyssey actions. The new joints take the rest orientation
of the same bones in the animation rig (--anim-blend), so the animations carry over without correction.
Writes <content>/mods/model: Mario.arc with the new model, and Galaxy's face, hand and hair/cap models blanked.
"""
import argparse
import gzip
import hashlib
import os
import struct
import sys
import tempfile
from collections import Counter

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdl  # noqa: E402
import blendfile  # noqa: E402
import disc  # noqa: E402
import gxtex  # noqa: E402
import j3d  # noqa: E402
import retarget  # noqa: E402

# Model bone -> Galaxy joint for the body. Bones not listed and not under the head or a hand follow their
# nearest listed ancestor; bones under the head or a hand become new joints.
BODY = {
    'Root': 'JointRoot', 'Hip': 'Hip', 'LegL1': 'LegL1', 'LegL2': 'LegL2', 'IKLegL': 'LegL2', 'FootL': 'FootL',
    'ToeL': 'FootL', 'LegR1': 'LegR1', 'LegR2': 'LegR2', 'IKLegR': 'LegR2', 'FootR': 'FootR', 'ToeR': 'FootR',
    'Spine1': 'Spine1', 'Spine2': 'Spine2', 'Head': 'Head', 'Cap': 'Head',
    'ShoulderL': 'ShoulderL', 'ArmL1': 'ArmL1', 'ArmL1Sub': 'ArmL1', 'ArmL2': 'ArmL2', 'ArmL2Sub': 'ArmL2',
    'HandL': 'HandL', 'ShoulderR': 'ShoulderR', 'ArmR1': 'ArmR1', 'ArmR1Sub': 'ArmR1', 'ArmR2': 'ArmR2',
    'ArmR2Sub': 'ArmR2', 'HandR': 'HandR',
}
# Galaxy joint -> model bone at the same place (scale fit and per-joint offsets).
ANCHORS = {
    'Hip': 'Hip', 'LegL1': 'LegL1', 'LegL2': 'LegL2', 'FootL': 'FootL', 'LegR1': 'LegR1', 'LegR2': 'LegR2',
    'FootR': 'FootR', 'Spine1': 'Spine1', 'Spine2': 'Spine2', 'ArmL1': 'ArmL1', 'ArmL2': 'ArmL2',
    'HandL': 'HandL', 'ArmR1': 'ArmR1', 'ArmR2': 'ArmR2', 'HandR': 'HandR',
}
NEW_UNDER = ('Head', 'HandL', 'HandR')  # bones below these become joints of their own
# Joints kept in the skeleton without geometry (it goes to the hand). They come last, so the game's own
# animations only need tracks up to the face joints (build_animation_mod.py --extend-game-animations).
UNSKINNED = ('IndexFinger', 'MiddleFinger', 'ThirdFinger', 'LittleFinger', 'Thumb')
FINGER_POSE_ACTION = 'Wait oddysey idle'  # the hand shape the unskinned fingers keep
# Galaxy draws Mario through J3DModelX "extra" matrix buffers of 0xC00 bytes, 64 matrices
# (MarioActorDraw.cpp initExtraMtxBuffer), one per draw matrix (each joint with vertices of its own, and each
# blend). A model with more overruns them: vertices take other matrices (spikes) and, far enough over, the
# game crashes. So small face bones give their vertices to a neighbour, and blends fill what is left.
DRAW_MATRICES = 64
MERGE = {
    'Tongue01': 'Jaw', 'Tongue02': 'Jaw', 'Tongue03': 'Jaw', 'ToothBottom': 'Jaw', 'ToothUpper': 'MouthRoot',
    'CheekBlowUpL': 'CheekL', 'CheekL02': 'CheekL', 'CheekBlowUpR': 'CheekR', 'CheekR02': 'CheekR',
    'MustacheL02': 'MustacheL01', 'MustacheL03': 'MustacheL01', 'MustacheR02': 'MustacheR01', 'MustacheR03': 'MustacheR01',
    'EyelidLeftInside': 'EyelidLeft_1', 'EyelidLeftOutside': 'EyelidLeft_1',
    'EyelidRightInside': 'EyelidRight_1', 'EyelidRightOutside': 'EyelidRight_1',
    'CheekL': 'Head', 'CheekR': 'Head', 'BrowOutL': 'BrowCenterL', 'BrowOutR': 'BrowCenterR',
    'EyelidLowerL': 'Head', 'EyelidLowerR': 'Head',
}
# Body vertices get quarter weights; blends are kept in this order while the budget lasts: the body's half
# blends, quarter blends around the hips (half weights alone crease them), then the face's. Other quarter
# blends fall back to the nearest kept ones.
BODY_WEIGHT_STEP = 0.25
QUARTER_JOINTS = ('Hip', 'LegL1', 'LegR1')

ATLAS = 512  # the game loads the model into a fixed heap: the textures are halved to fit
# material -> (texture file, region x, y, w, h)
MATERIALS = {
    'BodyMT': ('MarioBody_alb.png', (0, 0, 256, 128)),
    'HairFaceMT': ('MarioHairFace_alb.png', (256, 0, 256, 256)),
    'CapMT': ('MarioCap_alb.png', (0, 128, 128, 128)),
    'FaceMT': ('MarioFace_alb_2.png', (128, 128, 128, 128)),
    'HairMT': ('MarioHair_alb.png', (0, 256, 128, 128)),
    'HandMT': ('MarioHand_alb.png', (128, 256, 128, 128)),
    'EyePupilLeftMT': ('MarioEyePupil_alb.png', (256, 256, 128, 128)),
    'EyePupilRightMT': ('MarioEyePupil_alb.png', (256, 256, 128, 128)),
    'EyeMT': ('MarioEye_alb.png', (384, 256, 64, 64)),
    'EyeBallMT': ('MarioEyeBall_alb.png', (448, 256, 64, 64)),
}
BODY_SHAPE = 8
# Galaxy part models drawn over the body. The face and hands stay: the power-up suits (Bee, Hopper) use them,
# and Nebula skips them on the normal body, which has its own (native_runtime.cpp hide_mario_parts).
PARTS = ('MarioHair.arc',)
# Mario's power-up models. Galaxy plays them with the main model's animation player, which is sized to the
# main model's joints: with fewer joints of their own it writes past them (a crash on taking a Bee
# Mushroom). They get the same added joints, without geometry.
FORMS = ('BeeMario.arc', 'IceMario.arc', 'InvincibleMario.arc', 'HopperMario.arc', 'BoneMario.arc')
MAIN_DOL_SHA1 = '9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd'
MOD_AREA = 0x04000000
SHADE = 0.85  # vertex colour (Galaxy's model shades with grey vertex colours around 0x80..0xCC)


def material_key(name):
    base = name.split('.')[0]
    for key in MATERIALS:
        if base == key:
            return key
    return None


def to_galaxy_axes(p):
    """Blender Z-up, facing -Y -> Galaxy Y-up, facing +Z."""
    p = np.asarray(p, dtype=float)
    return np.stack([p[..., 0], p[..., 2], -p[..., 1]], -1)


def make_atlas(folder):
    atlas = np.zeros((ATLAS, ATLAS, 4), np.uint8)
    atlas[..., 3] = 255
    done = set()
    for key, (file, (x, y, w, h)) in MATERIALS.items():
        if (x, y) in done:
            continue
        done.add((x, y))
        path = os.path.join(folder, file)
        if not os.path.exists(path):
            continue
        img = Image.open(path).convert('RGBA').resize((w, h), Image.LANCZOS)
        a = np.asarray(img).astype(np.float32)
        alpha = a[..., 3] / 255.0
        if alpha.min() < 0.5:
            # Cut-outs drawn opaque: clear texels take the image's average opaque colour.
            mask = alpha >= 0.5
            if mask.any():
                a[~mask, :3] = a[mask][:, :3].mean(0)
        a[..., 3] = 255
        atlas[y:y + h, x:x + w] = np.clip(a, 0, 255).astype(np.uint8)
    return atlas


def atlas_uv(region, uv_tri):
    """UVs of one triangle (3, 2) into the atlas; whole-number tiling (the face's tongue and teeth sit at v 2..3)
    is removed per triangle."""
    x, y, w, h = region
    shift = np.floor(uv_tri.min(0))
    u = np.clip(uv_tri[:, 0] - shift[0], 0.0, 1.0)
    v = np.clip(1.0 - (uv_tri[:, 1] - shift[1]), 0.0, 1.0)
    inset = 0.5
    return np.stack([(x + inset + u * (w - 2 * inset)) / ATLAS, (y + inset + v * (h - 2 * inset)) / ATLAS], 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--blend', required=True)
    ap.add_argument('--textures', required=True)
    ap.add_argument('--anim-blend', required=True)
    ap.add_argument('--content', required=True)
    ap.add_argument('--influences', type=int, default=2)
    ap.add_argument('--weight-step', type=float, default=0.5)
    # Galaxy's Mario crashes (DisplayListMaker::update) once the model has too many draw matrices (48 in the
    # game's model; 145 blends / 379 entries run, 187 / 447 crash). The blends shared by the most vertices are
    # kept, up to this many; every other vertex is bound to its strongest bone.
    ap.add_argument('--envelopes', type=int, default=1000, help='at most this many blends')
    ap.add_argument('--draw-matrices', type=int, default=DRAW_MATRICES,
                    help='joints with vertices plus blends; Galaxy holds 64 (see DRAW_MATRICES)')
    ap.add_argument('--rigid-new-joints', action='store_true', help='bind the face and finger joints rigidly')
    ap.add_argument('--blend-needs-game-joint', action='store_true',
                    help='keep only blends that include one of the game model\'s own joints')
    ap.add_argument('--no-skin-new-joints', action='store_true',
                    help='diagnostic: keep the added joints but bind their vertices to the game joints above them')
    ap.add_argument('--rigid-joints', help='comma-separated joint name prefixes never blended (e.g. fingers)')
    args = ap.parse_args()

    blend_path = args.blend
    with open(blend_path, 'rb') as f:
        if f.read(2) == b'\x1f\x8b':
            tmp = os.path.join(tempfile.gettempdir(), 'nebula_odyssey_model.blend')
            with gzip.open(blend_path, 'rb') as g, open(tmp, 'wb') as o:
                o.write(g.read())
            blend_path = tmp

    pak = disc.Pak(os.path.join(args.content, 'game.pak'))
    original = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/Mario.arc')))['mario.bdl']
    joints, parents = j3d.bmd_joints(original)
    names = [j[0] for j in joints]
    jix = {n: i for i, n in enumerate(names)}
    world = bdl.joint_world(joints, parents)
    gpos = {n: world[i][:3, 3] for i, n in enumerate(names)}

    model = blendfile.Blend(blend_path)
    border, bones = blendfile.read_armature(model)
    mpos = {n: to_galaxy_axes(b['mat'][:3, 3]) for n, b in bones.items()}
    a = np.array([mpos[m] for m in ANCHORS.values()])
    b = np.array([gpos[g] for g in ANCHORS])
    # Least squares scale and shift: the model's origin is at its feet, Galaxy's joints are not.
    am, bm = a.mean(0), b.mean(0)
    scale = float(((a - am) * (b - bm)).sum() / ((a - am) ** 2).sum())
    shift = bm - scale * am
    offset = {g: gpos[g] - (scale * mpos[m] + shift) for g, m in ANCHORS.items()}

    def owner(bone):
        """The Galaxy joint whose offset moves this bone's geometry."""
        while bone is not None and bone not in BODY:
            bone = bones[bone]['parent'] if bone in bones else None
        return BODY.get(bone, 'JointRoot')

    def offset_of(joint):
        while joint not in offset:
            joint = names[parents[jix[joint]]]
        return offset[joint]

    def placed(p_model, joint):
        return scale * p_model + shift + offset_of(joint)

    # --- new joints: every bone below the head or a hand, parented like the model rig
    anim = blendfile.Blend(args.anim_blend)
    aorder, abones = blendfile.read_armature(anim)
    arot = {n: bb['mat'][:3, :3] for n, bb in abones.items()}

    # The fingers have no vertices of their own in game (UNSKINNED), so their geometry is posed once, in the
    # idle's first frame, before it is bound to the hand: a relaxed hand instead of the straight rest pose.
    idle = blendfile.read_actions(anim, {FINGER_POSE_ACTION}).get(FINGER_POSE_ACTION)
    finger_delta = {}

    def rig_pose(bone):
        """World pose of a bone of the animation rig (where the action's keys belong), its hand at rest."""
        if bone in NEW_UNDER or bone not in abones or idle is None:
            return abones[bone]['mat'] if bone in abones else np.eye(4)
        parent = abones[bone]['parent']
        rest = abones[bone]['mat']
        local = np.linalg.inv(abones[parent]['mat']) @ rest if parent else rest
        ch = idle['bones'].get(bone, {})
        motion = np.eye(4)
        if 'rotation_quaternion' in ch:
            motion[:3, :3] = retarget.quat_to_mat(np.asarray(ch['rotation_quaternion'][0], float))
        return (rig_pose(parent) if parent else np.eye(4)) @ local @ motion

    # The animation rig's armature is in Galaxy's axes (Y up), the model's mesh and bones in Blender's (Z up).
    to_galaxy = to_galaxy_axes(np.eye(3)).T

    def turn(bone):
        """World rotation of a bone from rest to the idle pose (the bones' own rolls do not matter), in the
        model's axes."""
        if bone in NEW_UNDER or bone not in abones:
            return np.eye(3)
        return to_galaxy.T @ rig_pose(bone)[:3, :3] @ abones[bone]['mat'][:3, :3].T @ to_galaxy

    def joint_pos(bone):
        """Posed position of a bone's head in the model: its parent's turn carries it."""
        parent = bones[bone]['parent']
        if bone in NEW_UNDER or parent is None:
            return bones[bone]['mat'][:3, 3]
        return joint_pos(parent) + turn(parent) @ (bones[bone]['mat'][:3, 3] - bones[parent]['mat'][:3, 3])

    def delta(bone):
        """Rest-to-idle motion of an unskinned finger bone, with its hand held at rest."""
        if bone not in finger_delta:
            m = np.eye(4)
            m[:3, :3] = turn(bone)
            m[:3, 3] = joint_pos(bone) - m[:3, :3] @ bones[bone]['mat'][:3, 3]
            finger_delta[bone] = m
        return finger_delta[bone]

    def under(bone, roots):
        p = bones[bone]['parent']
        while p is not None:
            if p in roots:
                return p
            p = bones[p]['parent']
        return None

    extra = []
    ext_world = list(world)
    joint_of = {}
    for bone in border:
        if bone in BODY or under(bone, NEW_UNDER) is None:
            continue
        parent_bone = bones[bone]['parent']
        parent = joint_of.get(parent_bone) if parent_bone in joint_of else jix[owner(parent_bone)]
        rot = arot.get(bone, ext_world[parent][:3, :3])  # the animation rig's rest orientation
        pos = placed(mpos[bone], owner(bone))
        wm = np.eye(4)
        wm[:3, :3] = rot
        wm[:3, 3] = pos
        pw = ext_world[parent]
        local = np.linalg.inv(pw) @ wm
        extra.append((bone, parent, list(retarget.mat_to_euler(local[:3, :3])), list(local[:3, 3])))
        joint_of[bone] = len(ext_world)
        ext_world.append(pw @ local)
    print('%d new joints, scale %.2f' % (len(extra), scale))

    # --- geometry
    atlas = make_atlas(args.textures)
    geo = bdl.Geometry()
    report = []
    meshes = []
    for mesh in blendfile.read_meshes(model):
        key = material_key(mesh['material'])
        if key is None or not any(mesh['joints']):
            report.append('skipped %s (%s)' % (mesh['object'], mesh['material']))
            continue
        region = MATERIALS[key][1]
        pos = to_galaxy_axes(mesh['pos'])
        nrm = to_galaxy_axes(mesh['nrm'])
        corners = []
        for c in range(0, len(pos), 3):
            uv = atlas_uv(region, mesh['uv'][c:c + 3])
            for k in range(3):
                weights = {}
                geom = np.zeros(3)
                total = 0.0
                vp, vn = pos[c + k], nrm[c + k]
                skin = [(b, w) for b, w in zip(mesh['joints'][c + k], mesh['weights'][c + k]) if b in bones]
                if not args.no_skin_new_joints and any(b.startswith(UNSKINNED) for b, _ in skin):
                    # A finger vertex: posed by its finger bones (the hand's share stays at rest).
                    tw = sum(w for _, w in skin)
                    m = sum(w * (delta(b) if b.startswith(UNSKINNED) else np.eye(4)) for b, w in skin) / tw
                    vp = to_galaxy_axes((m @ np.append(mesh['pos'][c + k], 1.0))[:3][None])[0]
                    vn = to_galaxy_axes((m[:3, :3] @ mesh['nrm'][c + k])[None])[0]
                for bone, w in skin:
                    unskinned = args.no_skin_new_joints or bone.startswith(UNSKINNED)
                    skin_bone = MERGE.get(bone, bone)
                    target = jix[owner(bone)] if unskinned else joint_of.get(skin_bone, jix[owner(bone)])
                    weights[target] = weights.get(target, 0.0) + w
                    geom += w * placed(vp, owner(bone))
                    total += w
                if not weights:
                    weights = {jix['Head']: 1.0}
                    geom, total = placed(pos[c + k], 'Head'), 1.0
                geom /= total
                limit = args.influences
                if args.rigid_new_joints and any(j >= len(world) for j in weights):
                    limit = 1
                top = sorted(weights.items(), key=lambda e: -e[1])[:limit]
                s = sum(w for _, w in top)
                step = BODY_WEIGHT_STEP if all(j < len(world) for j, _ in top) else args.weight_step
                q = [(j, round(w / s / step) * step) for j, w in top]
                q = [(j, w) for j, w in q if w > 0] or [top[0][:1] + (1.0,)]
                s = sum(w for _, w in q)
                # Heaviest first, ties by joint number: a pair is one blend whatever order its weights came in.
                q = sorted(((j, w / s) for j, w in q), key=lambda e: (-e[1], e[0]))
                corners.append((geom, vn, uv[k], tuple(q), int(255 * SHADE)))
        meshes.append((mesh, key, corners))

    # Keep the blends most vertices share, within the envelope budget; the rest go to their strongest bone.
    all_names = names + [e[0] for e in extra]
    rigid_names = tuple(n for n in (args.rigid_joints or '').split(',') if n)

    def blendable(w):
        if args.blend_needs_game_joint and not any(j < len(world) for j, _ in w):
            return False
        return not any(all_names[j].startswith(rigid_names) for j, _ in w) if rigid_names else True
    use = Counter(c[3] for _, _, corners in meshes for c in corners if len(c[3]) > 1 and blendable(c[3]))

    def strongest(w):
        return max(w, key=lambda e: e[1])[0]
    # Every joint that ends up with vertices of its own, counting each blend as if it were dropped.
    singles = {c[3][0][0] for _, _, corners in meshes for c in corners if len(c[3]) == 1}
    singles |= {strongest(c[3]) for _, _, corners in meshes for c in corners if len(c[3]) > 1}
    kept = set()
    # Body blends first (they keep the spine, shoulders and knees smooth), then the face's, by vertex count.
    def tier(w):
        if any(j >= len(world) for j, _ in w):
            return 2
        if all(abs(x - 0.5) < 1e-6 for _, x in w):
            return 0
        return 1 if all(all_names[j] in QUARTER_JOINTS for j, _ in w) else 3
    ranked = sorted(((w, n) for w, n in use.items() if tier(w) < 3), key=lambda e: (tier(e[0]), -e[1]))

    def nearest(w, kept):
        """A dropped blend: the kept half blend of the same pair, else its strongest joint."""
        half = tuple(sorted(((j, 0.5) for j, _ in w), key=lambda e: e[0]))
        return half if half in kept else ((strongest(w), 1.0),)

    all_weights = Counter(c[3] for _, _, corners in meshes for c in corners)

    def matrices(kept):
        used = {w if len(w) == 1 or w in kept else nearest(w, kept) for w in all_weights}
        return len({w[0][0] for w in used if len(w) == 1}) + len({w for w in used if len(w) > 1})
    for w, _ in ranked[:args.envelopes]:
        if matrices(kept | {w}) <= args.draw_matrices:
            kept.add(w)
    final = []
    for mesh, key, corners in meshes:
        corners = [c if len(c[3]) == 1 or c[3] in kept else c[:3] + (nearest(c[3], kept),) + c[4:] for c in corners]
        final += corners
        geo.add(BODY_SHAPE, corners)
        report.append('%-34s %-16s %5d triangles' % (mesh['object'], key, len(corners) // 3))
    used = {c[3] for c in final}
    count = len({w[0][0] for w in used if len(w) == 1}) + len({w for w in used if len(w) > 1})
    if count > args.draw_matrices:
        raise RuntimeError('%d draw matrices, more than %d' % (count, args.draw_matrices))
    report.append('draw matrices %d of %d: %d joints, %d blends kept of %d (%d of %d blended vertices); '
                  '%d blends use an added joint' % (
                      count, args.draw_matrices, count - len(kept), len(kept), len(use),
                      sum(use[w] for w in kept), sum(use.values()),
                      sum(1 for w in kept if any(j >= len(world) for j, _ in w))))
    print(report[-1])
    for rank, (w, n) in enumerate((w, n) for w, n in ranked if w in kept):
        report.append('blend %3d: %4d vertices  %s' % (rank, n, ' + '.join('%s %.1f' % (all_names[j], x) for j, x in w)))

    texture = (14, ATLAS, ATLAS, gxtex.encode_cmpr(atlas))
    textures = {n: texture for n in ('MarioBody.0', 'MarioBody.1', 'MarioBody.2')}
    built = bdl.build(original, geo, textures, ext_world, extra)

    replace_files = {'Mario.arc': {'mario.bdl': built}}
    for part in PARTS:
        files = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/' + part)))
        replace_files[part] = {n: bdl.blank_shapes(d) for n, d in files.items() if n.endswith('.bdl')}
    for form in FORMS:
        if form not in {n.split('/')[-1] for n in pak.files}:
            continue
        files = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/' + form)))
        replace_files[form] = {n: bdl.with_joints(d, extra) for n, d in files.items() if n.endswith('.bdl')}

    out = os.path.join(args.content, 'mods', 'model')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    lines = ['# Nebula custom model (Odyssey Mario), built by tools/animations/build_odyssey_model_mod.py',
             'version 1', 'game RMGE01', 'main_dol_sha1 ' + MAIN_DOL_SHA1]
    area = MOD_AREA
    for arc_name, repl in replace_files.items():
        name = 'ObjectData/' + arc_name
        arc = disc.rarc_replace(disc.yaz0_decode(pak.read(name)), repl)
        packed = disc.yaz0_encode(arc)
        packed += b'\0' * ((-len(packed)) % 32)
        target = pak.filler_area(len(packed), start=area)
        area = target + len(packed) + 0x100000
        offset0, size0, entry_pos = pak.files[name]
        entry = bytearray(pak.m[entry_pos:entry_pos + 12])
        struct.pack_into('>II', entry, 4, target >> 2, len(packed))
        stem = arc_name.replace('.arc', '')
        with open(os.path.join(out, stem + '.arc.yaz0'), 'wb') as f:
            f.write(packed)
        with open(os.path.join(out, 'fst_' + stem + '.bin'), 'wb') as f:
            f.write(entry)
        lines.append('0x%X 12 fst_%s.bin  # %s: 0x%X -> 0x%X' % (entry_pos, stem, name, offset0, target))
        lines.append('0x%X %d %s.arc.yaz0' % (target, len(packed), stem))
        report.append('%s: %d bytes at 0x%X, sha1 %s' % (name, len(packed), target, hashlib.sha1(packed).hexdigest()))
    with open(os.path.join(out, 'manifest.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    Image.fromarray(gxtex.decode_cmpr(texture[3], ATLAS, ATLAS)).save(os.path.join(out, 'atlas_preview.png'))
    report[0:0] = ['scale %.3f, %d new joints: %s' % (scale, len(extra), ', '.join(e[0] for e in extra))]
    with open(os.path.join(out, 'report.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    print('wrote', out)


if __name__ == '__main__':
    main()
