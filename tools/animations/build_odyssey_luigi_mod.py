"""Adds an Odyssey Luigi to Nebula's custom-model mod, from a glTF export of Super Mario Odyssey's Luigi
(skinned to the Odyssey player rig, textures embedded or next to it).

    py -3 tools/animations/build_odyssey_luigi_mod.py --gltf C:/path/Luigi.gltf --content C:/nb/content-mp

Run it after build_odyssey_model_mod.py on the same content: Galaxy plays Mario's animations on Luigi, so
Luigi gets the Odyssey Mario model's skeleton (Galaxy's 30 joints, which Galaxy's Luigi shares, and the
model's 70 added face and finger joints, taken from <content>/mods/model/Mario.arc.yaz0). Luigi's own bones
fold onto Galaxy's body joints; he has no face rig, so his face, cap and fingers ride on the head and
hands. The animations set every joint's offset, so each limb and spine segment of the mesh is stretched
along its bone onto Galaxy's joint spacing (Luigi's legs are longer than Mario's, his neck shorter);
across the bone the mesh keeps one overall scale. Luigi.arc and his power-up models are appended to
<content>/mods/model (manifest.txt), next to Mario's.
"""
import argparse
import hashlib
import io
import os
import struct
import sys
from collections import Counter

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bdl  # noqa: E402
import build_odyssey_model_mod as mario  # noqa: E402
import disc  # noqa: E402
import gltf  # noqa: E402
import gxtex  # noqa: E402
import j3d  # noqa: E402

# Luigi bone -> Galaxy joint. Bones not listed follow their nearest listed ancestor (face bones, cap,
# fingers, the overalls' bag and tie).
BODY = {
    'JointRoot': 'JointRoot', 'Hip': 'Hip', 'LegL1': 'LegL1', 'LegL2': 'LegL2', 'FootL': 'FootL', 'ToeL': 'FootL',
    'LegR1': 'LegR1', 'LegR2': 'LegR2', 'FootR': 'FootR', 'ToeR': 'FootR', 'Spine1': 'Spine1',
    'Spine2': 'Spine2', 'Head': 'Head', 'ShoulderL': 'ShoulderL', 'ArmL1': 'ArmL1', 'ArmL1Sub': 'ArmL1',
    'ArmL2': 'ArmL2', 'ArmL2Sub': 'ArmL2', 'HandL': 'HandL', 'ShoulderR': 'ShoulderR', 'ArmR1': 'ArmR1',
    'ArmR1Sub': 'ArmR1', 'ArmR2': 'ArmR2', 'ArmR2Sub': 'ArmR2', 'HandR': 'HandR',
}
ANCHORS = ('Hip', 'LegL1', 'LegL2', 'FootL', 'LegR1', 'LegR2', 'FootR', 'Spine1', 'Spine2', 'ArmL1', 'ArmL2',
           'HandL', 'ArmR1', 'ArmR2', 'HandR')
# Galaxy joint -> the next joint along its bone: the mesh between them is stretched onto Galaxy's length.
SEGMENTS = {'LegL1': 'LegL2', 'LegL2': 'FootL', 'LegR1': 'LegR2', 'LegR2': 'FootR', 'Spine1': 'Spine2',
            'Spine2': 'Head', 'ShoulderL': 'ArmL1', 'ArmL1': 'ArmL2', 'ArmL2': 'HandL',
            'ShoulderR': 'ArmR1', 'ArmR1': 'ArmR2', 'ArmR2': 'HandR'}
# Meshes left out: the closed eyelids (Galaxy does not blink this model). The hair mesh (HairCapOff) stays:
# it is also the hair below the cap.
SKIP = ('Eyelid',)
EYE_WHITE = np.array([246.0, 246.0, 240.0])  # under the pupil's see-through texels
# texture (image name without _alb.png) -> atlas region x, y, w, h (the layout of the Mario model's atlas)
REGIONS = {
    'LuigiBody': (0, 0, 256, 128), 'LuigiHairFace': (256, 0, 256, 256), 'LuigiCap': (0, 128, 128, 128),
    'LuigiFace': (128, 128, 128, 128), 'LuigiHair': (0, 256, 128, 128), 'MarioHand': (128, 256, 128, 128),
    'LuigiEyePupil': (256, 256, 128, 128), 'LuigiEyeBall': (384, 256, 64, 64), 'LuigiEyelid': (448, 256, 64, 64),
}
FORMS = ('BeeLuigi.arc', 'IceLuigi.arc', 'InvincibleLuigi.arc', 'HopperLuigi.arc', 'BoneLuigi.arc')
BODY_SHAPE = mario.BODY_SHAPE
DRAW_MATRICES = mario.DRAW_MATRICES
WEIGHT_STEP = 0.25
INFLUENCES = 2


def rotation_between(a, b):
    """Smallest rotation taking unit vector a onto unit vector b."""
    v = np.cross(a, b)
    c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    k = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + k + k @ k / (1.0 + c)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gltf', required=True)
    ap.add_argument('--content', required=True, help='content folder with game.pak and the Mario model mod')
    ap.add_argument('--keep', default='', help='comma-separated SKIP prefixes to keep after all')
    args = ap.parse_args()
    skip = tuple(s for s in SKIP if s not in args.keep.split(','))

    pak = disc.Pak(os.path.join(args.content, 'game.pak'))
    original = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/Luigi.arc')))['luigi.bdl']
    joints, parents = j3d.bmd_joints(original)
    names = [j[0] for j in joints]
    jix = {n: i for i, n in enumerate(names)}
    world = bdl.joint_world(joints, parents)
    gpos = {n: world[i][:3, 3] for i, n in enumerate(names)}

    # The Mario model's skeleton: Galaxy's joints (the same as Luigi's) and the joints it adds.
    out = os.path.join(args.content, 'mods', 'model')
    built_mario = disc.rarc_files(disc.yaz0_decode(open(os.path.join(out, 'Mario.arc.yaz0'), 'rb').read()))
    mj, mp = j3d.bmd_joints(built_mario['mario.bdl'])
    if [j[0] for j in mj[:len(joints)]] != names:
        raise RuntimeError("the Mario model's first joints are not Luigi's")
    extra = [(j[0], mp[i], list(j[2]), list(j[3])) for i, j in enumerate(mj) if i >= len(joints)]
    ext_world = bdl.joint_world(mj, mp)

    model = gltf.Gltf(args.gltf)
    node_ix = {n.get('name'): i for i, n in enumerate(model.nodes) if 'mesh' not in n}
    lpos = {n: model.world(i)[:3, 3] for n, i in node_ix.items()}
    a = np.array([lpos[n] for n in ANCHORS])
    b = np.array([gpos[n] for n in ANCHORS])
    am, bm = a.mean(0), b.mean(0)
    scale = float(((a - am) * (b - bm)).sum() / ((a - am) ** 2).sum())

    def owner(bone):
        while bone is not None and bone not in BODY:
            i = node_ix.get(bone)
            bone = model.nodes[model.parent[i]].get('name') if i in model.parent else None
        return BODY.get(bone, 'JointRoot')

    # Per Galaxy joint: Luigi's joint, and the stretch along its bone onto Galaxy's.
    fits = {}
    for g in set(BODY.values()):
        fit = {'from': lpos[g], 'to': gpos[g], 'rot': np.eye(3), 'axis': None, 'stretch': 1.0}
        if g in SEGMENTS:
            la = lpos[SEGMENTS[g]] - lpos[g]
            ga = gpos[SEGMENTS[g]] - gpos[g]
            fit['axis'] = la / np.linalg.norm(la)
            fit['rot'] = rotation_between(fit['axis'], ga / np.linalg.norm(ga))
            fit['stretch'] = float(np.linalg.norm(ga) / (scale * np.linalg.norm(la)))
        fits[g] = fit

    def placed(p, n, g):
        f = fits[g]
        d = p - f['from']
        if f['axis'] is not None:
            along = float(np.dot(d, f['axis']))
            d = d + (f['stretch'] - 1.0) * along * f['axis']
            # Normals take the inverse stretch along the bone.
            n = n + (1.0 / f['stretch'] - 1.0) * float(np.dot(n, f['axis'])) * f['axis']
        return f['to'] + f['rot'] @ (scale * d), f['rot'] @ n

    # --- textures
    atlas = np.zeros((mario.ATLAS, mario.ATLAS, 4), np.uint8)
    atlas[..., 3] = 255
    region_of = {}
    for i, img in enumerate(model.g['images']):
        key = (img.get('name') or img.get('uri', '')).split('/')[-1].split('_alb')[0]
        if key not in REGIONS:
            continue
        region_of[i] = REGIONS[key]
        x, y, w, h = REGIONS[key]
        im = Image.open(io.BytesIO(model.image_bytes(i))).convert('RGBA').resize((w, h), Image.LANCZOS)
        px = np.asarray(im).astype(np.float32)
        alpha = px[..., 3] / 255.0
        if 'EyePupil' in key:
            # The pupil sheet covers the whole eye and is mostly see-through around the iris: the eye white
            # shows there, not the iris's average colour.
            px[..., :3] = px[..., :3] * alpha[..., None] + EYE_WHITE * (1.0 - alpha[..., None])
        elif alpha.min() < 0.5 and (alpha >= 0.5).any():
            px[alpha < 0.5, :3] = px[alpha >= 0.5][:, :3].mean(0)
        px[..., 3] = 255
        atlas[y:y + h, x:x + w] = np.clip(px, 0, 255).astype(np.uint8)

    # --- geometry
    meshes = model.meshes()
    # Some exporters (Prisma3D) write the normals in another frame than the positions (Z up): take the one
    # in which the normals agree with the triangles' winding.
    frames = {'as written': np.eye(3), 'Z up to Y up': np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], float)}

    def agreement(r):
        cos = []
        for m in meshes:
            t = m['pos'].reshape(-1, 3, 3)
            fn = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
            fn /= np.maximum(np.linalg.norm(fn, axis=1), 1e-12)[:, None]
            cos.append(np.einsum('ij,ikj->ik', fn, (m['nrm'] @ r.T).reshape(-1, 3, 3)).ravel())
        return float(np.mean(np.concatenate(cos) > 0))
    frame = max(frames, key=lambda k: agreement(frames[k]))
    for m in meshes:
        m['nrm'] = m['nrm'] @ frames[frame].T
    geo = bdl.Geometry()
    report = ['normals: %s (%.0f%% of corners agree with the winding)' % (frame, 100 * agreement(np.eye(3)))]
    corners_all = []
    for mesh in meshes:
        node = mesh['node'] or ''
        if node.startswith(skip) or mesh['image'] not in region_of or mesh['joints'] is None:
            report.append('skipped %s' % node)
            continue
        region = region_of[mesh['image']]
        corners = []
        for c in range(0, len(mesh['pos']), 3):
            # glTF's v runs down the image, Blender's (which atlas_uv expects) up.
            uv = mario.atlas_uv(region, mesh['uv'][c:c + 3] * [1.0, -1.0] + [0.0, 1.0])
            # glTF winds counter-clockwise like Blender; the Mario builder's corners keep that order.
            for k in range(3):
                i = c + k
                weights = {}
                geom, nsum, total = np.zeros(3), np.zeros(3), 0.0
                for bone, w in zip(mesh['joints'][i], mesh['weights'][i]):
                    if w <= 0.0:
                        continue
                    g = owner(bone)
                    p, n = placed(mesh['pos'][i], mesh['nrm'][i], g)
                    geom += w * p
                    nsum += w * n
                    total += w
                    weights[jix[g]] = weights.get(jix[g], 0.0) + w
                geom /= total
                top = sorted(weights.items(), key=lambda e: -e[1])[:INFLUENCES]
                s = sum(w for _, w in top)
                q = [(j, round(w / s / WEIGHT_STEP) * WEIGHT_STEP) for j, w in top]
                q = [(j, w) for j, w in q if w > 0] or [(top[0][0], 1.0)]
                s = sum(w for _, w in q)
                q = tuple(sorted(((j, w / s) for j, w in q), key=lambda e: (-e[1], e[0])))
                corners.append((geom, nsum / (np.linalg.norm(nsum) or 1.0), uv[k], q, int(255 * mario.SHADE)))
        corners_all.append((node, corners))

    # Blends most vertices share, within Galaxy's 64 draw matrices; the rest go to the nearest kept blend.
    use = Counter(c[3] for _, corners in corners_all for c in corners if len(c[3]) > 1)
    singles = {c[3][0][0] for _, corners in corners_all for c in corners if len(c[3]) == 1}

    def strongest(w):
        return max(w, key=lambda e: e[1])[0]

    def nearest(w, kept):
        half = tuple(sorted(((j, 0.5) for j, _ in w), key=lambda e: e[0]))
        return half if half in kept else ((strongest(w), 1.0),)

    def matrices(kept):
        used = {w if len(w) == 1 or w in kept else nearest(w, kept) for w in use} | {((j, 1.0),) for j in singles}
        return len({w[0][0] for w in used if len(w) == 1}) + len({w for w in used if len(w) > 1})
    kept = set()
    for w, _ in sorted(use.items(), key=lambda e: (0 if all(abs(x - 0.5) < 1e-6 for _, x in e[0]) else 1, -e[1])):
        if matrices(kept | {w}) <= DRAW_MATRICES:
            kept.add(w)
    final = []
    for node, corners in corners_all:
        corners = [c if len(c[3]) == 1 or c[3] in kept else c[:3] + (nearest(c[3], kept),) + c[4:] for c in corners]
        final += corners
        geo.add(BODY_SHAPE, corners)
        report.append('%-34s %5d triangles' % (node, len(corners) // 3))
    used = {c[3] for c in final}
    count = len({w[0][0] for w in used if len(w) == 1}) + len({w for w in used if len(w) > 1})
    if count > DRAW_MATRICES:
        raise RuntimeError('%d draw matrices, more than %d' % (count, DRAW_MATRICES))
    report.append('draw matrices %d of %d: %d blends kept of %d' % (count, DRAW_MATRICES, len(kept), len(use)))
    print(report[-1])

    texture = (14, mario.ATLAS, mario.ATLAS, gxtex.encode_cmpr(atlas))
    textures = {n: texture for n in ('MarioBody.0', 'MarioBody.1', 'MarioBody.2')}
    luigi = bdl.build(original, geo, textures, ext_world, extra)
    replace_files = {'Luigi.arc': {'luigi.bdl': luigi}}
    for form in FORMS:
        if 'ObjectData/' + form not in pak.files:
            continue
        files = disc.rarc_files(disc.yaz0_decode(pak.read('ObjectData/' + form)))
        replace_files[form] = {n: bdl.with_joints(d, extra) for n, d in files.items() if n.endswith('.bdl')}

    # --- append to the model mod, after the areas Mario's archives use
    manifest = os.path.join(out, 'manifest.txt')
    with open(manifest) as f:
        lines = [ln.rstrip('\n') for ln in f]
    kept_lines, area = [], mario.MOD_AREA
    for ln in lines:
        fields = ln.split('#')[0].split()
        if len(fields) == 3 and fields[2].endswith('.arc.yaz0'):
            if fields[2].replace('.arc.yaz0', '.arc') in replace_files:
                continue  # a Luigi archive from an earlier run
            area = max(area, int(fields[0], 16) + int(fields[1]) + 0x100000)
        elif len(fields) == 3 and fields[2].startswith('fst_') and \
                fields[2][4:].replace('.bin', '.arc') in replace_files:
            continue
        kept_lines.append(ln)
    lines = kept_lines
    for arc_name, repl in replace_files.items():
        name = 'ObjectData/' + arc_name
        arc = disc.rarc_replace(disc.yaz0_decode(pak.read(name)), repl)
        packed = disc.yaz0_encode(arc)
        packed += b'\0' * ((-len(packed)) % 32)
        target = pak.filler_area(len(packed), start=area)
        area = target + len(packed) + 0x100000
        offset0, _, entry_pos = pak.files[name]
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
    with open(manifest, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    Image.fromarray(gxtex.decode_cmpr(texture[3], mario.ATLAS, mario.ATLAS)).save(
        os.path.join(out, 'luigi_atlas_preview.png'))
    stretch = ', '.join('%s %.2f' % (g, fits[g]['stretch']) for g in SEGMENTS)
    report[0:0] = ['Luigi: scale %.3f; stretch along the bones: %s' % (scale, stretch)]
    with open(os.path.join(out, 'luigi_report.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report))
    print('wrote', out)


if __name__ == '__main__':
    main()
