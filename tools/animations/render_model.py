"""Renders a J3D model (bdl/bmd) in its rest pose from its own data: decodes VTX1, SHP1 display lists,
DRW1/EVP1 skinning and the first texture of each shape's material, and draws front and side views.
Used to check built models against the game's.

    py -3 tools/animations/render_model.py model.bdl out.png
"""
import struct
import sys

import numpy as np

import bdl
import gxtex
import j3d


def _u16(d, o): return struct.unpack_from('>H', d, o)[0]
def _u32(d, o): return struct.unpack_from('>I', d, o)[0]


TYPES = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}


def decode(model):
    ch = {}
    p = 32
    for _ in range(_u32(model, 12)):
        ch[model[p:p + 4]] = p
        p += _u32(model, p + 4)
    d = model
    # VTX1
    v = ch[b'VTX1']
    fo = v + _u32(d, v + 8)
    arrays = {}
    fmts = []
    while True:
        attr, comp, typ, frac = struct.unpack_from('>IIIB', d, fo)
        fo += 16
        if attr == 0xFF:
            break
        fmts.append((attr, comp, typ, frac))
    slot_of = {9: 0, 10: 1, 11: 3, 13: 5}
    starts = [_u32(d, v + 12 + 4 * i) for i in range(13)]
    for attr, comp, typ, frac in fmts:
        if attr not in slot_of:
            continue
        s = slot_of[attr]
        off = starts[s]
        nxt = sorted(x for x in starts if x > off) + [_u32(d, v + 4)]
        end = nxt[0]
        if attr == 11:
            arrays[attr] = None
            continue
        n = {9: 3, 10: 3, 13: 2}[attr]
        f, sz = TYPES[typ]
        cnt = (end - off) // sz
        raw = np.array(struct.unpack_from('>%d%s' % (cnt, f), d, v + off), np.float64)
        if typ != 4:
            raw /= float(1 << frac)
        arrays[attr] = raw[:cnt // n * n].reshape(-1, n)
    # skinning
    joints, parents = j3d.bmd_joints(model)
    world = bdl.joint_world(joints, parents)
    e = ch[b'EVP1']
    ne = _u16(d, e + 8)
    oc, oi, ow, om = (_u32(d, e + 12 + 4 * i) for i in range(4))
    envs = []
    k = 0
    for i in range(ne):
        c = d[e + oc + i]
        envs.append([(_u16(d, e + oi + 2 * (k + j)), struct.unpack_from('>f', d, e + ow + 4 * (k + j))[0]) for j in range(c)])
        k += c
    inv = {}
    for j in range(len(joints)):
        if e + om + 48 * (j + 1) <= e + _u32(d, e + 4):
            m = np.eye(4)
            m[:3, :4] = np.array(struct.unpack_from('>12f', d, e + om + 48 * j)).reshape(3, 4)
            inv[j] = m
    w = ch[b'DRW1']
    nd = _u16(d, w + 8)
    drw = [(d[w + _u32(d, w + 12) + i], _u16(d, w + _u32(d, w + 16) + 2 * i)) for i in range(nd)]

    def draw_matrix(i):
        weighted, idx = drw[i]
        if not weighted:
            return world[idx]
        m = np.zeros((4, 4))
        for j, wt in envs[idx]:
            m += wt * (world[j] @ inv[j])
        return m

    # SHP1
    s = ch[b'SHP1']
    ns = _u16(d, s + 8)
    o = [_u32(d, s + 12 + 4 * i) for i in range(8)]
    shapes = []
    for i in range(ns):
        so = s + o[0] + i * 0x28
        pc, ao, fmd, fp = _u16(d, so + 2), _u16(d, so + 4), _u16(d, so + 6), _u16(d, so + 8)
        a = s + o[3] + ao
        desc = []
        while True:
            at, tp = _u32(d, a), _u32(d, a + 4)
            a += 8
            if at == 0xFF:
                break
            desc.append((at, tp))
        tris = []
        for pk in range(pc):
            un, cnt, first = struct.unpack_from('>HHI', d, s + o[6] + 8 * (fmd + pk))
            table = [_u16(d, s + o[4] + 2 * (first + q)) for q in range(cnt)]
            size, off = struct.unpack_from('>II', d, s + o[7] + 8 * (fp + pk))
            q = s + o[5] + off
            end = q + size
            cur_mtx = table[0] if table else 0
            while q < end:
                op = d[q]
                if op == 0:
                    q += 1
                    continue
                n = _u16(d, q + 1)
                q += 3
                verts = []
                for _ in range(n):
                    vtx = {}
                    for at, tp in desc:
                        if tp == 1:
                            vtx[at] = d[q]; q += 1
                        elif tp == 2:
                            vtx[at] = d[q]; q += 1
                        elif tp == 3:
                            vtx[at] = _u16(d, q); q += 2
                    mi = table[vtx[0] // 3] if 0 in vtx else cur_mtx
                    if mi == 0xFFFF:
                        mi = cur_mtx
                    m = draw_matrix(mi)
                    pos = m[:3, :3] @ arrays[9][vtx[9]] + m[:3, 3]
                    uv = arrays[13][vtx[13]] if 13 in vtx else np.zeros(2)
                    verts.append((pos, uv))
                kind = op & 0xF8
                if kind == 0x90:
                    tris += [verts[t:t + 3] for t in range(0, len(verts) - 2, 3)]
                elif kind == 0x98:
                    for t in range(len(verts) - 2):
                        tris.append(verts[t:t + 3] if t % 2 == 0 else [verts[t + 1], verts[t], verts[t + 2]])
                elif kind == 0xA0:
                    tris += [[verts[0], verts[t], verts[t + 1]] for t in range(1, len(verts) - 1)]
        shapes.append(tris)
    # first texture (for colour)
    t = ch[b'TEX1']
    h = t + _u32(d, t + 12)
    texs = []
    for i in range(_u16(d, t + 8)):
        hh = h + 32 * i
        fmt, wdt, hgt = d[hh], _u16(d, hh + 2), _u16(d, hh + 4)
        off = hh + _u32(d, hh + 0x1C)
        texs.append(gxtex.decode_cmpr(d[off:off + wdt * hgt // 2], wdt, hgt) if fmt == 14 else None)
    return shapes, texs


def render(model, out, texture_index=0):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    shapes, texs = decode(model)
    tex = texs[texture_index]
    fig, axes = plt.subplots(1, 2, figsize=(10, 7))
    for ax, (i, j, depth, sign) in zip(axes, ((0, 1, 2, 1), (2, 1, 0, -1))):
        polys, colors, depths = [], [], []
        for tris in shapes:
            for tri in tris:
                p = np.array([v[0] for v in tri])
                uv = np.mean([v[1] for v in tri], 0)
                if tex is not None:
                    hh, ww = tex.shape[:2]
                    c = tex[int(np.clip(uv[1], 0, 0.999) * hh), int(np.clip(uv[0], 0, 0.999) * ww), :3] / 255.0
                else:
                    c = (0.6, 0.6, 0.6)
                polys.append(p[:, [i, j]] * [sign, 1])
                colors.append(c)
                depths.append(p[:, depth].mean() * (1 if i == 0 else -1))
        order = np.argsort(depths)
        ax.add_collection(PolyCollection([polys[k] for k in order], facecolors=[colors[k] for k in order],
                                         edgecolors='none'))
        ax.set_xlim(-70, 70); ax.set_ylim(-5, 140); ax.set_aspect('equal'); ax.axis('off')
    fig.tight_layout()
    fig.savefig(out, dpi=80)


if __name__ == '__main__':
    render(open(sys.argv[1], 'rb').read(), sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
