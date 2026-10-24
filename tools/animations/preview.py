"""Contact sheet of the converted animations: Galaxy's Mario posed by the built mod's BCKs (blue)
over the source action in the .blend (orange), front and side views, a few frames each.

    py -3 tools/animations/preview.py --blend file.blend --content C:/nb/build/content --out sheet.png
Needs matplotlib.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blendfile  # noqa: E402
import build_animation_mod as mod  # noqa: E402
import disc  # noqa: E402
import j3d  # noqa: E402
import retarget  # noqa: E402

BODY = ['Hip', 'LegL1', 'LegL2', 'FootL', 'LegR1', 'LegR2', 'FootR', 'Spine1', 'Spine2', 'Head',
        'ShoulderL', 'ArmL1', 'ArmL2', 'HandL', 'ShoulderR', 'ArmR1', 'ArmR2', 'HandR']


def galaxy_positions(bck, names, parents, f):
    world, pos = [], []
    for j in range(len(names)):
        e = np.array([j3d.eval_track(j3d.track_keys(bck, j, a * 3 + 1), f) for a in range(3)])
        t = np.array([j3d.eval_track(j3d.track_keys(bck, j, a * 3 + 2), f) for a in range(3)])
        p = parents[j]
        pw = world[p] if p >= 0 else np.eye(3)
        pp = pos[p] if p >= 0 else np.zeros(3)
        world.append(pw @ retarget.euler_to_mat(e))
        pos.append(pp + pw @ t)
    return {n: pos[i] for i, n in enumerate(names)}


def source_positions(rig, bones, act, u):
    pose = {n: {k: a[min(int(u), act['nf'] - 1)] for k, a in b.items()} for n, b in act['bones'].items()}
    world, offset = rig.source_pose(pose)
    pos = {}
    for n in rig.src_order:
        p = rig.src_parent[n]
        m = bones[n]['mat']
        if p is None:
            pos[n] = m[:3, 3] + offset[n]
            continue
        pm = bones[p]['mat']
        local = pm[:3, :3].T @ (m[:3, 3] - pm[:3, 3])
        pos[n] = pos[p] + world[p] @ (local + offset[n])
    return pos


def main():
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    ap = argparse.ArgumentParser()
    ap.add_argument('--blend', required=True)
    ap.add_argument('--content', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--only')
    args = ap.parse_args()

    pak = disc.Pak(os.path.join(args.content, 'game.pak'))
    joints, parents = j3d.bmd_joints(disc.rarc_files(disc.yaz0_decode(pak.read(mod.MODEL)))['mario.bdl'])
    names = [j[0] for j in joints]
    built = disc.rarc_files(disc.yaz0_decode(open(
        os.path.join(args.content, 'mods', 'animations', 'MarioAnime.arc.yaz0'), 'rb').read()))
    blend = blendfile.Blend(args.blend)
    order, bones = blendfile.read_armature(blend)
    rig = retarget.Rig(joints, parents, order, bones)
    wanted = [k for k in mod.MAP if not args.only or k in args.only.split(',')]
    actions = blendfile.read_actions(blend, {mod.MAP[k][0] for k in wanted})

    cols = 4
    fig, axes = plt.subplots(len(wanted), cols * 2, figsize=(cols * 4, len(wanted) * 2.2))
    for r, name in enumerate(wanted):
        bck = j3d.parse_bck(built[name + '.bck'])
        act = actions[mod.MAP[name][0]]
        loop = bck['loop'] == 2
        period = retarget._cycle_period(act) if loop else act['nf'] - 1
        for c in range(cols):
            f = bck['dur'] * c / (cols - 1) if not loop else bck['dur'] * c / cols
            if loop:
                u = f / bck['dur'] * period
            elif act['nf'] - 1 >= bck['dur']:
                u = f
            else:
                u = f / bck['dur'] * (act['nf'] - 1)
            g = galaxy_positions(bck, names, parents, f)
            s = source_positions(rig, bones, act, u)
            for v, (ax_i, ax_j) in enumerate(((0, 1), (2, 1))):
                ax = axes[r][c * 2 + v]
                for pts, color in ((s, 'tab:orange'), (g, 'tab:blue')):
                    hip = pts['Hip']
                    for n in BODY[1:]:
                        par = names[parents[names.index(n)]]
                        if par in pts and n in pts:
                            a, b = pts[par] - hip, pts[n] - hip
                            ax.plot([a[ax_i], b[ax_i]], [a[ax_j], b[ax_j]], color=color, lw=1.5)
                ax.set_xlim(-45, 45); ax.set_ylim(-45, 50); ax.set_aspect('equal'); ax.axis('off')
                if v == 0:
                    ax.set_title('%s f%.0f' % (name if c == 0 else '', f), fontsize=7, loc='left')
    fig.tight_layout()
    fig.savefig(args.out, dpi=70)
    print('wrote', args.out)


if __name__ == '__main__':
    main()
