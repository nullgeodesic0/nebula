"""Builds Nebula's custom-model mod: a skinned model from a COLLADA file (made for the Smash Bros.
Ultimate Mario rip) re-skinned onto Galaxy Mario's skeleton.

    py -3 tools/animations/build_model_mod.py --dae C:/path/mario.dae --content C:/nb/build/content

The model is scaled onto Galaxy's rest skeleton (both are T-poses), its bones are folded onto
Galaxy's 30 joints, and it is written into mario.bdl with Galaxy's own skeleton, scene graph and
materials. Everything is drawn by the body material from one texture atlas: base colour times ambient
occlusion. Galaxy's separate face, hand and hair/cap models are blanked, since the new model has
its own. Output: <content>/mods/model (a disc mod, like the animations). Nothing from the game is
shipped; the mod is built from the user's own files.
"""
import argparse
import re
import hashlib
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdl  # noqa: E402
import dae  # noqa: E402
import disc  # noqa: E402
import gxtex  # noqa: E402
import j3d  # noqa: E402

# Which source meshes make up the default look (the rest are alternative faces and the hatless head).
NODES = ('c00bodyShape', 'Mario_Hathead_VIS_O_OBJShape', 'Mario_Hair_VIS_O_OBJShape')
# Galaxy's face model (MarioFace.arc) holds 8 expressions on joints Face0..Face7 and shows one at a time:
# 0 neutral, 1 mouth half open, 2 mouth wide open, 3 lower lip pushed, 4 teeth, 5 small smile,
# 6 determined, 7 closed. Every slot is the neutral Smash face with a different mouth (the mouths are
# separate meshes): Smash's other expression faces reshape the eye sockets and brows, which then no longer
# fit the eyes and cap drawn by the body.
_N = '_VIS_O_OBJShape'
_FACE = 'Mario_FaceN' + _N
FACE_SLOTS = [
    (_FACE, 'Mario_FaceNMClose' + _N),       # 0 neutral
    (_FACE, 'Mario_TalkMHalf' + _N),         # 1 half open
    (_FACE, 'Mario_TalkMOpen' + _N),         # 2 wide open: the happy "wahoo" of jumps
    (_FACE, 'Mario_OuchMClose' + _N),        # 3 lip pushed out
    (_FACE, 'Mario_HeavyAttackOpen' + _N),   # 4 teeth: effort
    (_FACE, 'Mario_Result' + _N),            # 5 small smile
    (_FACE, 'Mario_TalkMClose' + _N),        # 6 determined
    (_FACE, 'Mario_FaceNMClose' + _N),       # 7 closed
]# The face model takes the body's "MarioFace" texture at run time; this atlas replaces it.
# The eyes are drawn with the face (in every slot), so they move exactly as the face model does.
FACE_REGIONS = {'skin_mario_001': (0, 0, 512, 384), 'alp_mario_002': (0, 384, 256, 128),
                'eye_mario_w': (256, 384, 128, 128)}
EYES = 'Mario_Eye_VIS_O_OBJShape'
# Faces in the body that the converted animations switch on where the source animates the mouth (see
# FACE_JOINTS in build_animation_mod.py): the head joint each hangs from, and its mouth.
BODY_FACES = (('Eye0', 'Mario_TalkMOpen' + _N), ('Cap0', 'Mario_Result' + _N))
FACE_JOINT = 'Face0'  # the body joint the face model hangs from
# Blinking: Galaxy draws an eyelid shape over the eyes and flips its texture through 4 frames (open to
# closed). The shape gets Smash's closed lids, and the frames show none, a third, two thirds and all of them.
LID_NODE = 'Mario_Blink' + _N
LID_SHAPE = 2
LID_TEXTURES = ('mario_eyeLid.0', 'mario_eyeLid.1', 'mario_eyeLid.2', 'mario_eyeLid.3')
# The bind pose has flat open hands; fingers are curled into a relaxed fist (degrees per joint, about the
# axis across the palm). Left fingers point +X, right -X, palms down.
CURL = {'1': 35.0, '2': 55.0, '3': 35.0}
THUMB_CURL = 20.0

# Source bone -> Galaxy joint. Bones not listed follow their nearest listed ancestor.
BONES = {
    'Trans': 'All_Root', 'Rot': 'Center', 'Throw': 'Center',
    'Hip': 'JointRoot', 'H_Waist': 'JointRoot', 'LegC': 'Hip',
    'Waist': 'Spine1', 'Bust': 'Spine2', 'Neck': 'Spine2', 'Head': 'Head',
    'ClavicleC': 'Spine2',
    'ClavicleL': 'ShoulderL', 'ShoulderL': 'ArmL1', 'H_SholderL': 'ArmL1', 'ArmL': 'ArmL2',
    'H_ArmtwistL': 'ArmL2', 'H_WristL': 'ArmL2', 'HandL': 'HandL',
    'ClavicleR': 'ShoulderR', 'ShoulderR': 'ArmR1', 'H_SholderR': 'ArmR1', 'ArmR': 'ArmR2',
    'H_ArmtwistR': 'ArmR2', 'H_WristR': 'ArmR2', 'HandR': 'HandR',
    'LegL': 'LegL1', 'H_LegL': 'LegL1', 'KneeL': 'LegL2', 'FootL': 'FootL', 'ToeL': 'FootL',
    'LegR': 'LegR1', 'H_LegR': 'LegR1', 'KneeR': 'LegR2', 'FootR': 'FootR', 'ToeR': 'FootR',
}
# Galaxy joint -> the source bone at the same place, for fitting scale and per-joint offsets.
ANCHORS = {
    # The head is left out: it keeps its place on the chest (Spine2's offset), so the neck is not stretched
    # toward Galaxy's head joint, which sits higher and further back.
    'JointRoot': 'Hip', 'Hip': 'LegC', 'Spine1': 'Waist', 'Spine2': 'Bust',
    'ShoulderL': 'ClavicleL', 'ArmL1': 'ShoulderL', 'ArmL2': 'ArmL', 'HandL': 'HandL',
    'ShoulderR': 'ClavicleR', 'ArmR1': 'ShoulderR', 'ArmR2': 'ArmR', 'HandR': 'HandR',
    'LegL1': 'LegL', 'LegL2': 'KneeL', 'FootL': 'FootL',
    'LegR1': 'LegR', 'LegR2': 'KneeR', 'FootR': 'FootR',
}

ATLAS = 512
# material colour image stem -> region (x, y, w, h) of the atlas
REGIONS = {
    'def_mario_001': (0, 0, 512, 256),
    'skin_mario_001': (0, 256, 256, 256),
    'eye_mario_w': (256, 256, 128, 128),
    'alp_mario_002': (384, 256, 128, 128),
    'alp_mario_001': (256, 384, 256, 128),
}
BODY_SHAPE = 8  # Body1_v: always drawn, the material the game recolours
# Galaxy's part models drawn by the game over the body; the new model has its own face, cap, hair and hands.
# (--game-hands keeps Galaxy's hands instead, which change pose but do not match the new arms.)
PARTS = ('MarioFace.arc', 'MarioHandL.arc', 'MarioHandR.arc', 'MarioHair.arc')
GAME_HANDS = ('HandL', 'HandR')
IRIS = 'eye_mario_b'  # drawn over the eye white through the eye mesh's second UV set
MAIN_DOL_SHA1 = '9a71008ae1ee9010e267fa67d1f0b0d4f0e895dd'
MOD_AREA = 0x04000000  # disc area searched for room, apart from the animation mod's


def load_image(folder, stem, kind='col'):
    path = os.path.join(folder, 'images', '%s_%s.png' % (stem, kind))
    return Image.open(path).convert('RGBA') if os.path.exists(path) else None


def make_atlas(folder, regions=None, size=None):
    regions = regions or REGIONS
    size = size or ATLAS
    atlas = np.zeros((size, size, 4), np.uint8)
    alpha_maps = {}
    occlusion = {}
    for stem, (x, y, w, h) in regions.items():
        col = load_image(folder, stem)
        if col is None:
            continue
        a = np.asarray(col.resize((w, h), Image.LANCZOS)).astype(np.float32)
        ao = load_image(folder, stem, 'ao')
        if ao is not None:  # occlusion goes into the vertex colours (shade_of), as the game's model does
            occlusion[stem] = np.asarray(ao.resize((w, h), Image.LANCZOS)).astype(np.float32)[..., 0] / 255.0
        alpha = a[..., 3] / 255.0
        alpha_maps[stem] = alpha
        if alpha.min() < 0.5:
            # Cut-out cards drawn opaque: fill the clear texels with the card's average colour.
            mask = alpha >= 0.5
            if mask.any():
                avg = a[mask][:, :3].mean(0)
                a[~mask, :3] = avg
        a[..., 3] = 255
        atlas[y:y + h, x:x + w] = np.clip(a, 0, 255).astype(np.uint8)
    return atlas, alpha_maps, occlusion


# Galaxy's model shades with grey vertex colours around 0x80..0xCC; occlusion maps onto that range.
SHADE_DARK, SHADE_LIGHT = 0.42, 0.85


def shade_of(occlusion, stem, uv):
    occ = occlusion.get(stem)
    if occ is None:
        return int(255 * SHADE_LIGHT)
    h, w = occ.shape
    o = occ[min(int((1 - np.clip(uv[1], 0, 1)) * h), h - 1), min(int(np.clip(uv[0], 0, 1) * w), w - 1)]
    return int(255 * (SHADE_DARK + (SHADE_LIGHT - SHADE_DARK) * o))


def finger_pose(src):
    """Skinning matrices (source bind space) that curl the fingers."""
    delta = {}
    for b in src.order:
        parent, _, bind = src.nodes[b]
        d = delta.get(parent, np.eye(4)) if parent else np.eye(4)
        m = re.match(r'Finger([LR])(\d)(\d)$', b)
        if m:
            side, finger, joint = m.groups()
            if finger == '5':
                angle = THUMB_CURL if joint != '1' else 0.0
            else:
                angle = CURL.get(joint, 0.0) if joint != '0' else 0.0
            if angle:
                pivot = (d @ bind)[:3, 3]
                axis = np.array([0.0, 0.0, -1.0 if side == 'L' else 1.0])
                c, s = np.cos(np.radians(angle)), np.sin(np.radians(angle))
                x, y, z = axis
                r = np.eye(4)
                r[:3, :3] = [[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                             [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                             [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]]
                t = np.eye(4); t[:3, 3] = pivot
                ti = np.eye(4); ti[:3, 3] = -pivot
                d = t @ r @ ti @ d
        delta[b] = d
    return delta


def bake_iris(atlas, folder, meshes, regions=None):
    """Paints the iris (second UV set) over the eye white region, through each eye triangle."""
    iris = load_image(folder, IRIS)
    if iris is None:
        return
    iris = np.asarray(iris).astype(np.float32)
    ih, iw = iris.shape[:2]
    x0, y0, w, h = (regions or REGIONS)['eye_mario_w']
    region = atlas[y0:y0 + h, x0:x0 + w].astype(np.float32)
    yy, xx = np.mgrid[0:h, 0:w]
    px = np.stack([(xx + 0.5) / w, 1.0 - (yy + 0.5) / h], -1)  # COLLADA uv of each texel
    for mesh in meshes:
        if len(mesh.uvs) < 2:
            continue
        for c in range(0, len(mesh.pos), 3):
            a, b, cc = mesh.uvs[0][c:c + 3]
            t0, t1, t2 = mesh.uvs[1][c:c + 3]
            m = np.array([[b[0] - a[0], cc[0] - a[0]], [b[1] - a[1], cc[1] - a[1]]])
            if abs(np.linalg.det(m)) < 1e-12:
                continue
            inv = np.linalg.inv(m)
            rel = px - a
            l1 = rel @ inv[0]
            l2 = rel @ inv[1]
            inside = (l1 >= -0.01) & (l2 >= -0.01) & (l1 + l2 <= 1.01)
            if not inside.any():
                continue
            uv2 = t0 + l1[..., None] * (t1 - t0) + l2[..., None] * (t2 - t0)
            ok = inside & (uv2[..., 0] >= 0) & (uv2[..., 0] <= 1) & (uv2[..., 1] >= 0) & (uv2[..., 1] <= 1)
            sx = np.clip((uv2[..., 0] * iw).astype(int), 0, iw - 1)
            sy = np.clip(((1 - uv2[..., 1]) * ih).astype(int), 0, ih - 1)
            col = iris[sy, sx]
            alpha = np.where(ok, col[..., 3] / 255.0, 0.0)[..., None]
            region[..., :3] = region[..., :3] * (1 - alpha) + col[..., :3] * alpha
    atlas[y0:y0 + h, x0:x0 + w] = np.clip(region, 0, 255).astype(np.uint8)


def atlas_uv(stem, uv, regions=None, size=None):
    x, y, w, h = (regions or REGIONS)[stem]
    size = size or ATLAS
    u = np.clip(uv[:, 0], 0.0, 1.0)
    v = np.clip(1.0 - uv[:, 1], 0.0, 1.0)  # COLLADA v runs up, GX t runs down
    inset = 0.5
    s = (x + inset + u * (w - 2 * inset)) / size
    t = (y + inset + v * (h - 2 * inset)) / size
    return np.stack([s, t], 1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dae', required=True)
    ap.add_argument('--content', required=True)
    # Galaxy's Mario crashes the game with a few hundred weight blends (it has 13); two joints per vertex in
    # steps of a half keep it near 30.
    ap.add_argument('--influences', type=int, default=2, help='most joints per vertex (1 to 4)')
    ap.add_argument('--weight-step', type=float, default=0.5, help='round weights to this step (0: none)')
    ap.add_argument('--game-hands', action='store_true', help="keep Galaxy's hand models")
    args = ap.parse_args()
    folder = os.path.dirname(os.path.abspath(args.dae))

    pak = disc.Pak(os.path.join(args.content, 'game.pak'))
    mario_arc = disc.yaz0_decode(pak.read('ObjectData/Mario.arc'))
    original = disc.rarc_files(mario_arc)['mario.bdl']
    joints, parents = j3d.bmd_joints(original)
    names = [j[0] for j in joints]
    jix = {n: i for i, n in enumerate(names)}
    world = bdl.joint_world(joints, parents)
    gpos = {n: world[i][:3, 3] for i, n in enumerate(names)}

    src = dae.Dae(args.dae)
    spos = {n: src.nodes[n][2][:3, 3] for n in src.order}
    a = np.array([spos[s] for s in ANCHORS.values()])
    b = np.array([gpos[g] for g in ANCHORS])
    scale = float((a * b).sum() / (a * a).sum())
    offset = {g: gpos[g] - scale * spos[s] for g, s in ANCHORS.items()}

    def joint_of(bone):
        while bone is not None and bone not in BONES:
            bone = src.nodes[bone][0] if bone in src.nodes else None
        return BONES.get(bone, 'JointRoot')

    def offset_of(joint):
        while joint not in offset:
            joint = names[parents[jix[joint]]]
        return offset[joint]

    atlas, alpha_maps, occlusion = make_atlas(folder)
    face_atlas, face_alpha, face_occlusion = make_atlas(folder, FACE_REGIONS, ATLAS)
    all_nodes = set(NODES) | {n for slot in FACE_SLOTS for n in slot} | {LID_NODE, EYES} | {m for _, m in BODY_FACES}
    by_node = {}
    for m in src.meshes(lambda n: n in all_nodes):
        by_node.setdefault(m.name, []).append(m)
    meshes = [m for n in NODES for m in by_node.get(n, [])]
    bake_iris(face_atlas, folder, by_node.get(EYES, []), FACE_REGIONS)
    bake_iris(atlas, folder, by_node.get(EYES, []))
    pose = finger_pose(src)
    parts = [p for p in PARTS if not (args.game_hands and 'Hand' in p)]
    report = []

    def stem_of(mesh):
        return os.path.basename(src.effect_image.get(mesh.material) or '').replace('_col.png', '')

    def convert(mesh, regions, alphas, occl, uv_override=None):
        """Corners (Galaxy body space position, normal, uv, joint weights, shade) of one source mesh."""
        stem = stem_of(mesh)
        if stem not in regions:
            report.append('skipped %s (%s): no texture region' % (mesh.name, mesh.material))
            return []
        uv = uv_override if uv_override is not None else atlas_uv(stem, mesh.uv, regions)
        alpha = alphas.get(stem)
        corners = []
        for c in range(0, len(mesh.pos), 3):
            if alpha is not None and alpha.min() < 0.5 and uv_override is None:
                # Triangles of a cut-out card that land on clear texels would draw as solid colour.
                x, y, w, h = regions[stem]
                cu = np.clip(mesh.uv[c:c + 3].mean(0), 0, 1)
                if alpha[min(int((1 - cu[1]) * h), h - 1), min(int(cu[0] * w), w - 1)] < 0.3:
                    continue
            if args.game_hands and all(max(zip(mesh.weights[k], mesh.joints[k]))[1] in src.nodes and
                   joint_of(max(zip(mesh.weights[k], mesh.joints[k]))[1]) in GAME_HANDS for k in range(c, c + 3)):
                continue  # the game's hand models draw the hands
            for k in range(c, c + 3):
                skin = sum(wt * pose.get(bone, np.eye(4)) for bone, wt in zip(mesh.joints[k], mesh.weights[k]))
                sw = sum(mesh.weights[k]) or 1.0
                posed = (skin[:3, :3] @ mesh.pos[k] + skin[:3, 3]) / sw
                normal = skin[:3, :3] @ mesh.nrm[k]
                weights = {}
                for bone, wt in zip(mesh.joints[k], mesh.weights[k]):
                    weights[joint_of(bone)] = weights.get(joint_of(bone), 0.0) + wt
                weights = dict(sorted(weights.items(), key=lambda e: -e[1])[:args.influences])
                if args.weight_step > 0:
                    total = sum(weights.values()) or 1.0
                    weights = {j: round(w / total / args.weight_step) * args.weight_step for j, w in weights.items()}
                    weights = {j: w for j, w in weights.items() if w > 0} or {max(weights, key=weights.get): 1.0}
                total = sum(weights.values()) or 1.0
                p = sum((wt / total) * (scale * posed + offset_of(j)) for j, wt in weights.items())
                corners.append((p, normal, uv[k], tuple((jix[j], wt / total) for j, wt in weights.items()),
                                shade_of(occl, stem, mesh.uv[k])))
        report.append('%-34s %-14s %5d of %5d triangles' % (mesh.name, stem, len(corners) // 3, len(mesh.pos) // 3))
        return corners

    # --- body
    geo = bdl.Geometry()
    for mesh in meshes:
        geo.add(BODY_SHAPE, convert(mesh, REGIONS, alpha_maps, occlusion))

    # --- the converted animations' extra faces, rigid on their head joints
    for joint, mouth in BODY_FACES:
        corners = []
        for n in (_FACE, mouth, EYES):
            for mesh in by_node.get(n, []):
                for p, nrm, t, w, shade in convert(mesh, REGIONS, alpha_maps, occlusion):
                    corners.append((p, nrm, t, ((jix[joint], 1.0),), shade))
        geo.add(BODY_SHAPE, corners)

    # --- eyelids: the closed lids, mapped top to bottom per eye onto the lid textures
    lid_frames = {}
    lids = by_node.get(LID_NODE, [])
    if lids:
        pts = np.concatenate([m.pos for m in lids])
        for mesh in lids:
            side = mesh.pos[:, 0] >= 0
            uvl = np.zeros((len(mesh.pos), 2))
            for s in (True, False):
                sel = pts[:, 0] >= 0 if s else pts[:, 0] < 0
                box = pts[sel]
                lo, hi = box.min(0), box.max(0)
                m = side == s
                uvl[m, 0] = (mesh.pos[m, 0] - lo[0]) / max(hi[0] - lo[0], 1e-6)
                uvl[m, 1] = (hi[1] - mesh.pos[m, 1]) / max(hi[1] - lo[1], 1e-6)  # 0 at the top of the lid
            uvl = np.clip(uvl, 0.02, 0.98)
            geo.add(LID_SHAPE, convert(mesh, {stem_of(mesh): (0, 0, 1, 1)}, {}, occlusion, uv_override=uvl))
        skin = load_image(folder, 'skin_mario_001')
        tone = np.asarray(skin).reshape(-1, 4)[:, :3].astype(np.float32)
        tone = np.median(tone, 0)
        for i, name in enumerate(LID_TEXTURES):
            img = np.zeros((64, 64, 4), np.uint8)
            covered = int(round(64 * i / (len(LID_TEXTURES) - 1)))
            img[:covered, :, :3] = tone.astype(np.uint8)
            img[:covered, :, 3] = 255
            if 0 < covered:
                img[max(covered - 3, 0):covered, :, :3] = (40, 28, 24)  # lash line at the lid's edge
            lid_frames[name] = (14, 64, 64, gxtex.encode_cmpr(img))

    # --- body model: body atlas, face atlas (read by the face model), lid frames
    texture = (14, ATLAS, ATLAS, gxtex.encode_cmpr(atlas))
    face_texture = (14, ATLAS, ATLAS, gxtex.encode_cmpr(face_atlas))
    textures = {n: texture for n in ('MarioBody.0', 'MarioBody.1', 'MarioBody.2')}
    textures['MarioFace'] = face_texture
    textures.update(lid_frames)
    model = bdl.build(original, geo, textures, world)

    # --- face model: one Smash expression per slot, in the space of the body's face joint
    face_files = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/MarioFace.arc')))
    face_name = next(n for n in face_files if n.endswith('.bdl'))
    face_original = face_files[face_name]
    fj, fp = j3d.bmd_joints(face_original)
    face_world = bdl.joint_world(fj, fp)
    to_face = np.linalg.inv(world[jix[FACE_JOINT]])
    face_geo = bdl.Geometry()
    for slot, nodes in enumerate(FACE_SLOTS):
        corners = []
        for n in nodes + (EYES,):
            for mesh in by_node.get(n, []):
                for p, nrm, t, w, shade in convert(mesh, FACE_REGIONS, face_alpha, face_occlusion):
                    corners.append((to_face[:3, :3] @ p + to_face[:3, 3], to_face[:3, :3] @ nrm, t,
                                    ((slot + 1, 1.0),), shade))
        face_geo.add(slot, corners)
    face_model = bdl.build(face_original, face_geo, {}, face_world)

    replace_files = {'Mario.arc': {'mario.bdl': model}, 'MarioFace.arc': {face_name: face_model}}
    for part in parts:
        if part == 'MarioFace.arc':
            continue
        files = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/' + part)))
        replace_files[part] = {n: bdl.blank_shapes(d) for n, d in files.items() if n.endswith('.bdl')}
    tris_total = sum(len(c) // 3 for c in geo.shapes.values())
    out = os.path.join(args.content, 'mods', 'model')
    os.makedirs(out, exist_ok=True)
    lines = ['# Nebula custom model, built by tools/animations/build_model_mod.py',
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
    report[0:0] = ['scale %.3f (source units to Galaxy), %d triangles in the body shape' % (scale, tris_total)]
    with open(os.path.join(out, 'report.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    print('wrote', out)


if __name__ == '__main__':
    main()
