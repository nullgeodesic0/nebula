"""J3D (GameCube/Wii) model joints and joint-animation (BCK, ANK1) reading and writing."""
import math
import struct



def _u16(d, o): return struct.unpack_from('>H', d, o)[0]
def _u32(d, o): return struct.unpack_from('>I', d, o)[0]


def _chunks(d):
    out = {}
    p = 32
    for _ in range(_u32(d, 12)):
        out[d[p:p + 4].decode('latin1')] = p
        p += _u32(d, p + 4)
    return out


def bmd_joints(d):
    """[(name, scale, rotation degrees (x, y, z), translation)], parent index per joint (-1 for roots)."""
    c = _chunks(d)
    p = c['JNT1']
    count = _u16(d, p + 8)
    table, remap, names_off = _u32(d, p + 12), _u32(d, p + 16), _u32(d, p + 20)
    jn = p + names_off
    names = []
    for i in range(_u16(d, jn)):
        no = _u16(d, jn + 4 + i * 4 + 2)
        names.append(d[jn + no:d.index(b'\0', jn + no)].decode('shift-jis'))
    joints = []
    for i in range(count):
        o = p + table + _u16(d, p + remap + i * 2) * 0x40
        sc = struct.unpack_from('>3f', d, o + 4)
        rot = tuple(r * 180.0 / 32768.0 for r in struct.unpack_from('>3h', d, o + 16))
        tr = struct.unpack_from('>3f', d, o + 24)
        joints.append((names[i], sc, rot, tr))
    # Hierarchy from the INF1 scene graph: 1 = down, 2 = up, 0x10 = joint.
    p = c['INF1']
    q = p + _u32(d, p + 0x14)
    parent = {}
    stack, cur, last = [], -1, -1
    while True:
        t, v = _u16(d, q), _u16(d, q + 2)
        q += 4
        if t == 0:
            break
        if t == 1:
            stack.append(cur); cur = last
        elif t == 2:
            cur = stack.pop()
        elif t == 0x10:
            parent[v] = cur; last = v
    return joints, [parent.get(i, -1) for i in range(count)]


def parse_bck(d):
    if d[:8] != b'J3D1bck1':
        raise ValueError('not a BCK')
    p = 32
    if d[p:p + 4] != b'ANK1':
        raise ValueError('BCK without ANK1')
    loop = d[p + 8]
    shift = struct.unpack_from('b', d, p + 9)[0]
    dur, jc, sc, rc, tc = struct.unpack_from('>5H', d, p + 10)
    jo, so, ro, to = struct.unpack_from('>4I', d, p + 20)
    return dict(
        loop=loop, shift=shift, dur=dur,
        S=struct.unpack_from('>%df' % sc, d, p + so),
        R=struct.unpack_from('>%dh' % rc, d, p + ro),
        T=struct.unpack_from('>%df' % tc, d, p + to),
        joints=[[struct.unpack_from('>3H', d, p + jo + j * 54 + k * 6) for k in range(9)] for j in range(jc)])


def track_keys(b, joint, k):
    """Keys (time, value, tangent in, tangent out) of track k = axis * 3 + (0 scale, 1 rotation in
    degrees, 2 translation)."""
    cnt, idx, tt = b['joints'][joint][k]
    comp = k % 3
    tab = (b['S'], b['R'], b['T'])[comp]
    sc = 180.0 / 32768.0 * (2 ** b['shift']) if comp == 1 else 1.0
    if cnt == 1:
        return [(0.0, tab[idx] * sc, 0.0, 0.0)]
    n = 3 if tt == 0 else 4
    return [(float(tab[idx + i * n]), tab[idx + i * n + 1] * sc, tab[idx + i * n + 2] * sc,
             tab[idx + i * n + (2 if tt == 0 else 3)] * sc) for i in range(cnt)]


def hermite(t0, v0, m0, t1, v1, m1, t):
    dt = t1 - t0
    u = (t - t0) / dt
    return ((2 * u ** 3 - 3 * u ** 2 + 1) * v0 + (-2 * u ** 3 + 3 * u ** 2) * v1 +
            (u ** 3 - 2 * u ** 2 + u) * m0 * dt + (u ** 3 - u ** 2) * m1 * dt)


def eval_track(keys, t):
    if len(keys) == 1 or t <= keys[0][0]:
        return keys[0][1]
    if t >= keys[-1][0]:
        return keys[-1][1]
    for a, b in zip(keys, keys[1:]):
        if a[0] <= t <= b[0]:
            return hermite(a[0], a[1], a[3], b[0], b[1], b[2], t)
    return keys[-1][1]


def reduce_track(vals, tol):
    """Fewest Hermite keys (time, value, slope) on whole frames that follow vals within tol."""
    n = len(vals)
    if n < 2 or (max(vals) - min(vals)) <= tol * 0.5:
        return [(0.0, float(vals[0]), 0.0)]
    der = [0.0] * n
    for i in range(n):
        a, b = max(i - 1, 0), min(i + 1, n - 1)
        der[i] = (vals[b] - vals[a]) / (b - a)
    keys = [0, n - 1]
    while True:
        worst, werr = -1, tol
        ks = sorted(keys)
        for a, b in zip(ks, ks[1:]):
            for t in range(a + 1, b):
                e = abs(hermite(a, vals[a], der[a], b, vals[b], der[b], t) - vals[t])
                if e > werr:
                    werr, worst = e, t
        if worst < 0:
            break
        keys.append(worst)
    return [(float(k), float(vals[k]), float(der[k])) for k in sorted(keys)]


def _s16(v):
    return max(-32768, min(32767, int(round(v))))


def write_bck(header, loop, dur, tracks, shift=0):
    """tracks[joint][axis*3 + comp] = list of keys (time, value, slope); one key = constant.
    Rotations are in degrees. header: the first 32 bytes of a BCK to copy."""
    # Rotations are shorts in units of 180/32768 * 2**shift degrees; flips unwrap past a full turn,
    # so the shift is the smallest that holds every angle and slope.
    biggest = max([abs(v) for joint in tracks for k in range(1, 9, 3) for key in joint[k] for v in key[1:]] + [1.0])
    # Keys are (time, value, slope) or (time, value, slope in, slope out); a track with any of the second
    # kind is written with separate tangents (type 1).
    # shift: the finest rotation unit wanted (a copied BCK keeps its own); raised when an angle needs it.
    while biggest > 180.0 * 32767.0 / 32768.0 * (2 ** shift):
        shift += 1
    rot_unit = 180.0 / 32768.0 * (2 ** shift)
    S, R, T = [], [], []
    consts = ({}, {}, {})

    def put(comp, keys):
        tab = (S, R, T)[comp]
        conv = (lambda v: v / rot_unit) if comp == 1 else (lambda v: v)
        if len(keys) == 1:
            v = _s16(conv(keys[0][1])) if comp == 1 else float(keys[0][1])
            if v not in consts[comp]:
                consts[comp][v] = len(tab); tab.append(v)
            return (1, consts[comp][v], 0)
        split = any(len(k) > 3 and k[2] != k[3] for k in keys)
        idx = len(tab)
        for key in keys:
            t, v, mi = key[0], key[1], key[2]
            slopes = (mi, key[3] if len(key) > 3 else mi) if split else (mi,)
            if comp == 1:
                tab.extend([int(t), _s16(conv(v))] + [_s16(conv(m)) for m in slopes])
            else:
                tab.extend([float(t), float(v)] + [float(m) for m in slopes])
        return (len(keys), idx, 1 if split else 0)

    entries = [[put(k % 3, joint[k]) for k in range(9)] for joint in tracks]
    nj = len(entries)
    jo = 0x24
    so = jo + nj * 54
    so = (so + 3) & ~3
    ro = so + 4 * len(S)
    to = (ro + 2 * len(R) + 3) & ~3
    end = (to + 4 * len(T) + 31) & ~31
    ank = bytearray(end)
    ank[0:4] = b'ANK1'
    struct.pack_into('>I', ank, 4, end)
    ank[8] = loop
    struct.pack_into('b', ank, 9, shift)
    struct.pack_into('>5H', ank, 10, dur, nj, len(S), len(R), len(T))
    struct.pack_into('>4I', ank, 20, jo, so, ro, to)
    for j, e in enumerate(entries):
        for k, (c, i, tt) in enumerate(e):
            struct.pack_into('>3H', ank, jo + j * 54 + k * 6, c, i, tt)
    struct.pack_into('>%df' % len(S), ank, so, *S)
    struct.pack_into('>%dh' % len(R), ank, ro, *R)
    struct.pack_into('>%df' % len(T), ank, to, *T)
    head = bytearray(header[:32])
    struct.pack_into('>I', head, 8, 32 + end)
    return bytes(head) + bytes(ank)
