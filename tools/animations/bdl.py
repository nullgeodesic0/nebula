"""Rebuilds a J3D model (bdl4) with new skinned geometry and textures, keeping its skeleton (JNT1),
scene graph (INF1), materials (MAT3) and material display lists (MDL3)."""
import struct

import numpy as np

import retarget


def _u16(d, o): return struct.unpack_from('>H', d, o)[0]
def _u32(d, o): return struct.unpack_from('>I', d, o)[0]


def _pad(b, n=32, fill=b'This is padding data to align'):
    extra = (-len(b)) % n
    return b + (fill * 4)[:extra] if extra else b


def chunks(d):
    out = {}
    p = 32
    for _ in range(_u32(d, 12)):
        out[d[p:p + 4].decode('latin1')] = bytes(d[p:p + _u32(d, p + 4)])
        p += _u32(d, p + 4)
    return out


def joint_world(joints, parents):
    """4x4 rest world matrix of each joint (J3D: T * Rz * Ry * Rx * S, scale ignored)."""
    out = []
    for i, (_, _, rot, tr) in enumerate(joints):
        m = np.eye(4)
        m[:3, :3] = retarget.euler_to_mat(np.array(rot))
        m[:3, 3] = tr
        out.append((out[parents[i]] if parents[i] >= 0 else np.eye(4)) @ m)
    return out


class Geometry:
    """Triangles with per-corner position (model space), normal, uv and joint weights."""

    def __init__(self):
        self.shapes = {}  # shape index -> list of corners (pos, nrm, uv, ((joint, weight), ...))

    def add(self, shape, corners):
        self.shapes.setdefault(shape, []).extend(corners)


def build(original, geometry, textures, world, extra_joints=None):
    """original: bdl bytes. geometry: Geometry with model-space positions. textures: name ->
    (fmt, width, height, data) replacing TEX1 entries by name. world: joint rest world matrices."""
    ch = chunks(original)
    nshapes = _u16(ch['SHP1'], 8)
    njoints = len(world)
    inv = [np.linalg.inv(m) for m in world]

    # --- vertices, draw matrices
    pos, nrm, uv, col = [], [], [], []
    pos_ix, nrm_ix, uv_ix, col_ix = {}, {}, {}, {}
    drw, drw_ix = [], {}          # (weighted, joint or envelope index)
    envs, env_ix = [], {}

    def intern(table, index, key, value):
        if key not in index:
            index[key] = len(table)
            table.append(value)
        return index[key]

    def drw_for(weights):
        if len(weights) == 1:
            key = (False, weights[0][0])
        else:
            ek = tuple(weights)
            if ek not in env_ix:
                env_ix[ek] = len(envs)
                envs.append(ek)
            key = (True, env_ix[ek])
        return intern(drw, drw_ix, key, key)

    shape_tris = {}
    for s in range(nshapes):
        corners = geometry.shapes.get(s, [])
        tris = []
        for c in range(0, len(corners), 3):
            tri = []
            for corner in corners[c:c + 3]:
                p, n, t, w = corner[:4]
                shade = corner[4] if len(corner) > 4 else 255
                wq = [(j, round(x, 3)) for j, x in w if x >= 0.001]
                total = sum(x for _, x in wq)
                # Heaviest joint first, ties by joint number.
                wq = tuple(sorted(((j, round(x / total, 3)) for j, x in wq), key=lambda e: (-e[1], e[0])))
                di = drw_for(wq)
                if len(wq) == 1:  # rigid vertices are stored in their joint's space
                    m = inv[wq[0][0]]
                    p = m[:3, :3] @ p + m[:3, 3]
                    n = m[:3, :3] @ n
                n = n / (np.linalg.norm(n) or 1.0)
                # Galaxy skins Mario on the CPU, which gives each position and normal one matrix: one shared
                # by vertices with different matrices stays behind with the wrong one (the game's model shares
                # none), so they are kept apart per draw matrix.
                pi = intern(pos, pos_ix, (di,) + tuple(np.round(p, 4)), p)
                ni = intern(nrm, nrm_ix, (di,) + tuple(np.round(n, 3)), n)
                ti = intern(uv, uv_ix, tuple(np.round(t, 5)), t)
                ci = intern(col, col_ix, int(shade), int(shade))
                tri.append((di, pi, ni, ti, ci))
            tris.append(tri)
        shape_tris[s] = tris
    if not drw:
        drw_for(((0, 1.0),))
    # J3D takes the draw matrices before the first weighted one as joints and the rest as envelopes,
    # so single-joint entries come first.
    order = sorted(range(len(drw)), key=lambda i: (drw[i][0], i))
    remap = {old: new for new, old in enumerate(order)}
    drw = [drw[i] for i in order]
    shape_tris = {s: [[(remap[d], pi, ni, ti, ci) for d, pi, ni, ti, ci in tri] for tri in tris]
                  for s, tris in shape_tris.items()}

    # --- VTX1: float positions, normals and uvs; grey vertex colours carry the baked shading, as the game's do
    attrs = [(9, 1, 4, 0), (10, 0, 4, 0), (11, 1, 5, 0), (13, 1, 4, 0)]
    fmt = b''.join(struct.pack('>IIIB3x', *a) for a in attrs) + struct.pack('>IIIB3x', 0xFF, 1, 0, 0)
    arrays = [np.asarray(pos, '>f4').tobytes(), np.asarray(nrm, '>f4').tobytes(),
              bytes(b for c in (col or [255]) for b in (c, c, c, 255)), np.asarray(uv, '>f4').tobytes()]
    head = 0x40
    body = _pad(fmt)
    offsets = [0] * 13
    for slot, data in zip((0, 1, 3, 5), arrays):
        offsets[slot] = head + len(body)
        body = _pad(body + data)
    vtx1 = _pad(b'VTX1' + struct.pack('>II', head + len(body), head) + struct.pack('>13I', *offsets) + body)
    vtx1 = vtx1[:4] + struct.pack('>I', len(vtx1)) + vtx1[8:]

    # --- EVP1: envelopes and every joint's inverse bind matrix
    counts = bytes(len(e) for e in envs)
    idx = b''.join(struct.pack('>H', j) for e in envs for j, _ in e)
    wts = b''.join(struct.pack('>f', x) for e in envs for _, x in e)
    mats = b''.join(struct.pack('>12f', *inv[j][:3, :4].reshape(-1)) for j in range(njoints))
    o_counts = 0x1C
    o_idx = (o_counts + len(counts) + 1) & ~1
    o_w = (o_idx + len(idx) + 3) & ~3
    o_m = (o_w + len(wts) + 3) & ~3
    evp = bytearray(o_m + len(mats))
    evp[0:4] = b'EVP1'
    struct.pack_into('>HH4I', evp, 8, len(envs), 0xFFFF, o_counts, o_idx, o_w, o_m)
    evp[o_counts:o_counts + len(counts)] = counts
    evp[o_idx:o_idx + len(idx)] = idx
    evp[o_w:o_w + len(wts)] = wts
    evp[o_m:o_m + len(mats)] = mats
    evp1 = _pad(bytes(evp))
    evp1 = evp1[:4] + struct.pack('>I', len(evp1)) + evp1[8:]

    # --- DRW1. J3D counts the draw matrices as DRW1's count minus EVP1's, so the table ends with one
    # more weighted entry per envelope, as the game's own models do.
    table = drw + [(True, e) for e in range(len(envs))]
    flags = bytes(1 if w else 0 for w, _ in table)
    data = b''.join(struct.pack('>H', i) for _, i in table)
    o_f = 0x14
    o_d = (o_f + len(flags) + 1) & ~1
    d1 = bytearray(o_d + len(data))
    d1[0:4] = b'DRW1'
    struct.pack_into('>HHII', d1, 8, len(table), 0xFFFF, o_f, o_d)
    d1[o_f:o_f + len(flags)] = flags
    d1[o_d:o_d + len(data)] = data
    drw1 = _pad(bytes(d1))
    drw1 = drw1[:4] + struct.pack('>I', len(drw1)) + drw1[8:]

    # --- SHP1: every shape multi-matrix, packets of at most 10 draw matrices
    attrib = b''.join(struct.pack('>II', a, t) for a, t in ((0, 1), (9, 3), (10, 3), (11, 3), (13, 3), (0xFF, 0)))
    shape_entries, mtx_table, mtx_data, packets, prim = [], [], [], [], b''
    for s in range(nshapes):
        tris = shape_tris[s]
        groups = []
        cur, cur_tris = [], []
        for tri in tris:
            need = list(dict.fromkeys(d for d, _, _, _, _ in tri if d not in cur))
            if len(cur) + len(need) > 10:
                groups.append((cur, cur_tris))
                cur, cur_tris = [], []
                need = list(dict.fromkeys(d for d, _, _, _, _ in tri))
            cur += need
            cur_tris.append(tri)
        if cur_tris or not groups:
            groups.append((cur or [0], cur_tris))
        first_md, first_pk = len(mtx_data), len(packets)
        for mats_, ts in groups:
            mtx_data.append((mats_[0], len(mats_), len(mtx_table)))
            mtx_table += mats_
            slot = {d: i for i, d in enumerate(mats_)}
            dl = b''
            if ts:
                dl = struct.pack('>BH', 0x90, len(ts) * 3)
                for tri in ts:
                    # GX's front faces wind the other way from the source meshes' (the game's models' normals
                    # point against their counter-clockwise winding): reversed, the model is not drawn inside
                    # out by the game's passes that cull back faces (it turned silver against walls).
                    for d, pi, ni, ti, ci in (tri[0], tri[2], tri[1]):
                        dl += struct.pack('>BHHHH', slot[d] * 3, pi, ni, ci, ti)
            dl = dl + b'\0' * ((-len(dl)) % 32) if dl else b'\0' * 32
            packets.append((len(dl), len(prim)))
            prim += dl
        pts = np.array([pos[pi] if drw[d][0] else world[drw[d][1]][:3, :3] @ pos[pi] + world[drw[d][1]][:3, 3]
                        for tri in tris for d, pi, _, _, _ in tri]) if tris else np.zeros((1, 3))
        lo, hi = pts.min(0), pts.max(0)
        radius = float(np.linalg.norm(pts - (lo + hi) / 2, axis=1).max())
        shape_entries.append(struct.pack('>BBHHHHH f3f3f', 3, 0xFF, len(groups), 0, first_md, first_pk, 0xFFFF,
                                         radius, *lo, *hi))
    o_init = 0x2C
    o_remap = o_init + 0x28 * nshapes
    o_attr = (o_remap + 2 * nshapes + 3) & ~3
    o_attr = (o_attr + 31) & ~31
    o_mtx = o_attr + len(attrib)
    o_prim = (o_mtx + 2 * len(mtx_table) + 31) & ~31
    o_md = o_prim + len(prim)
    o_pk = o_md + 8 * len(mtx_data)
    end = o_pk + 8 * len(packets)
    shp = bytearray(end)
    shp[0:4] = b'SHP1'
    struct.pack_into('>HH8I', shp, 8, nshapes, 0xFFFF, o_init, o_remap, 0, o_attr, o_mtx, o_prim, o_md, o_pk)
    for i, e in enumerate(shape_entries):
        shp[o_init + i * 0x28:o_init + (i + 1) * 0x28] = e
        struct.pack_into('>H', shp, o_remap + 2 * i, i)
    shp[o_attr:o_attr + len(attrib)] = attrib
    for i, m in enumerate(mtx_table):
        struct.pack_into('>H', shp, o_mtx + 2 * i, m)
    shp[o_prim:o_prim + len(prim)] = prim
    for i, (a, n, f) in enumerate(mtx_data):
        struct.pack_into('>HHI', shp, o_md + 8 * i, a, n, f)
    for i, (size, off) in enumerate(packets):
        struct.pack_into('>II', shp, o_pk + 8 * i, size, off)
    shp1 = _pad(bytes(shp))
    shp1 = shp1[:4] + struct.pack('>I', len(shp1)) + shp1[8:]

    # --- JNT1/INF1: the skeleton (with any appended joints) and the scene graph, new packet and vertex counts
    jnt1 = ch['JNT1']
    inf_src = ch['INF1']
    if extra_joints:
        jnt1, inf_src = extend_skeleton(original, extra_joints)
    inf1 = bytearray(inf_src)
    struct.pack_into('>II', inf1, 0x0C, len(packets), len(pos))

    # --- TEX1 and MDL3: replaced textures (by name), their sizes patched into the material lists
    tex1, size_patch = _rebuild_tex1(ch['TEX1'], textures)
    mdl3 = _patch_mdl3(ch['MDL3'], size_patch)

    parts = [bytes(inf1), vtx1, evp1, drw1, jnt1, shp1, ch['MAT3'], mdl3, tex1]
    out = bytearray(original[:32])
    for p in parts:
        out += p
    struct.pack_into('>II', out, 8, len(out), len(parts))
    return bytes(out)


def with_joints(model, extra):
    """`model` with joints appended to its skeleton (extend_skeleton) and nothing else changed: for models
    that must share the custom model's joint count (Galaxy drives Mario's power-up models with the main
    model's animation player, sized to its joints)."""
    jnt1, inf1 = extend_skeleton(model, extra)
    out = bytearray(model[:32])
    for name, data in chunks(model).items():
        out += {'JNT1': jnt1, 'INF1': inf1}.get(name, data)
    struct.pack_into('>I', out, 8, len(out))
    return bytes(out)


def _names(d, o):
    n = _u16(d, o)
    return [d[o + _u16(d, o + 4 + i * 4 + 2):d.index(b'\0', o + _u16(d, o + 4 + i * 4 + 2))].decode('shift-jis')
            for i in range(n)]


def _rebuild_tex1(t, textures):
    n = _u16(t, 8)
    hdr = _u32(t, 0x0C)
    names = _names(t, _u32(t, 0x10))
    name_table = t[_u32(t, 0x10):]
    headers, datas = [], []
    size_patch = {}
    data_ix = {}
    for i in range(n):
        h = bytearray(t[hdr + i * 32:hdr + i * 32 + 32])
        name = names[i]
        if name in textures:
            fmt, w, hh, data = textures[name]
            old = (h[0], _u16(h, 2), _u16(h, 4))
            size_patch[old] = size_patch.get(old, set()) | {(fmt, w, hh)}
            h[0] = fmt
            struct.pack_into('>HH', h, 2, w, hh)
            h[0x10] = 0      # no mipmaps
            h[0x14] = 1; h[0x15] = 1
            h[0x16] = 0; h[0x17] = 0; h[0x18] = 1
            key = ('new', hash(data))
        else:
            off = hdr + i * 32 + _u32(h, 0x1C)
            nxt = sorted({hdr + j * 32 + _u32(t, hdr + j * 32 + 0x1C) for j in range(n)} | {_u32(t, 0x10)})
            end = min(x for x in nxt if x > off) if any(x > off for x in nxt) else _u32(t, 0x10)
            data = t[off:end]
            key = ('old', off)
        if key not in data_ix:
            data_ix[key] = len(datas)
            datas.append(data)
        headers.append((h, data_ix[key]))
    base = 0x20 + 32 * n
    blob = b''
    pos = []
    for d in datas:
        pos.append(base + len(blob))
        blob = _pad(blob + d)
    name_off = base + len(blob)
    out = bytearray(b'TEX1' + b'\0' * 4 + struct.pack('>HHII', n, 0xFFFF, 0x20, name_off) + b'\0' * 0x0C)
    for i, (h, di) in enumerate(headers):
        struct.pack_into('>I', h, 0x1C, pos[di] - (0x20 + 32 * i))
        out += h
    out += blob + name_table
    out = _pad(bytes(out))
    return out[:4] + struct.pack('>I', len(out)) + out[8:], size_patch


def _patch_mdl3(m, size_patch):
    """Rewrites TX_SETIMAGE0 (BP 0x88..0x8B, 0xA8..0xAB) values that name a replaced texture's old
    format and size. A texture size shared with a texture that is not replaced would be ambiguous; the
    replacements used here keep such textures distinct."""
    m = bytearray(m)

    def bp_writes(start, end):
        i = start
        while i < end - 4:
            if m[i] == 0x61:
                yield i, m[i + 1], (m[i + 2] << 16) | (m[i + 3] << 8) | m[i + 4]
                i += 5
            elif m[i] == 0x10:  # XF load: skip its data
                i += 5 + 4 * (((m[i + 1] << 8) | m[i + 2]) + 1)
            elif m[i] == 0x08:  # CP load
                i += 6
            else:
                i += 1

    def put(i, v):
        m[i + 2:i + 5] = bytes([v >> 16 & 255, v >> 8 & 255, v & 255])

    count = _u16(m, 8)
    table = _u32(m, 0x0C)
    for k in range(count):
        entry = table + 8 * k
        start = entry + _u32(m, entry)
        end = start + _u32(m, entry + 4)
        resized = {}
        for i, reg, v in bp_writes(start, end):
            if reg in (0x88, 0x89, 0x8A, 0x8B, 0xA8, 0xA9, 0xAA, 0xAB):
                old = (v >> 20 & 0xF, (v & 0x3FF) + 1, (v >> 10 & 0x3FF) + 1)
                if old in size_patch and len(size_patch[old]) == 1:
                    fmt, w, h = next(iter(size_patch[old]))
                    put(i, (fmt << 20) | ((h - 1) << 10) | (w - 1))
                    resized[(old[1], old[2])] = (w, h)
        if not resized:
            continue
        # The texture coordinate scale (SU_SSIZE/SU_TSIZE, BP 0x30..0x3F) of a coordinate that
        # addresses a resized texture follows the new size, or the texture is sampled squeezed.
        ((ow, oh), (nw, nh)), = list(resized.items())[:1]
        for i, reg, v in bp_writes(start, end):
            if 0x30 <= reg <= 0x3F:
                old, new = (ow, nw) if reg % 2 == 0 else (oh, nh)
                if (v & 0xFFFF) == old - 1:
                    put(i, (v & 0xFF0000) | (new - 1))
    return bytes(m)


def blank_shapes(model):
    """The model with every shape's display lists turned into no-ops (draws nothing)."""
    d = bytearray(model)
    p = 32
    for _ in range(_u32(d, 12)):
        if d[p:p + 4] == b'SHP1':
            o_prim, o_md = _u32(d, p + 0x20), _u32(d, p + 0x24)
            d[p + o_prim:p + o_md] = bytes(o_md - o_prim)
        p += _u32(d, p + 4)
    return bytes(d)


def _name_table(names):
    """J3D name table: count, entries (hash, offset), then the strings."""
    head = 4 + 4 * len(names)
    entries = b''
    strings = b''
    for n in names:
        raw = n.encode('shift-jis')
        h = 0
        for c in raw:
            h = (h * 3 + c) & 0xFFFF
        entries += struct.pack('>HH', h, head + len(strings))
        strings += raw + b'\0'
    return struct.pack('>HH', len(names), 0xFFFF) + entries + strings


def extend_skeleton(original, extra):
    """JNT1 and INF1 of `original` with joints appended: extra = [(name, parent index, rotation degrees (x, y,
    z), translation)], each new joint's index following the existing ones. Returns (jnt1, inf1)."""
    ch = chunks(original)
    j = ch['JNT1']
    count = _u16(j, 8)
    entries_off, remap_off, names_off = _u32(j, 12), _u32(j, 16), _u32(j, 20)
    names = _names(j, names_off)
    entries = [bytes(j[entries_off + 0x40 * _u16(j, remap_off + 2 * i):][:0x40]) for i in range(count)]
    template = entries[12] if count > 12 else entries[0]  # a plain joint (the head)
    for name, parent, rot, trans in extra:
        e = bytearray(template)
        struct.pack_into('>3f', e, 4, 1.0, 1.0, 1.0)
        struct.pack_into('>3h', e, 16, *[max(-32768, min(32767, int(round(r * 32768.0 / 180.0)))) for r in _wrap(rot)])
        struct.pack_into('>3f', e, 24, *trans)
        struct.pack_into('>f', e, 0x24, 1.0)
        struct.pack_into('>6f', e, 0x28, -1.0, -1.0, -1.0, 1.0, 1.0, 1.0)
        entries.append(bytes(e))
        names.append(name)
    n = len(entries)
    o_entries = 0x18
    o_remap = o_entries + 0x40 * n
    o_names = (o_remap + 2 * n + 3) & ~3
    body = b''.join(entries) + b''.join(struct.pack('>H', i) for i in range(n))
    body += b'\0' * (o_names - o_remap - 2 * n)
    body += _name_table(names)
    jnt = bytearray(b'JNT1' + b'\0' * 4 + struct.pack('>HHIII', n, 0xFFFF, o_entries, o_remap, o_names) + body)
    jnt = _pad(bytes(jnt))
    jnt1 = jnt[:4] + struct.pack('>I', len(jnt)) + jnt[8:]

    # INF1: parse the scene graph into a tree, hang the new joints under their parents, write it back.
    inf = ch['INF1']
    q = _u32(inf, 0x14)
    root = []
    stack = [root]
    last = None
    while True:
        t, v = _u16(inf, q), _u16(inf, q + 2)
        q += 4
        if t == 0:
            break
        if t == 1:
            stack.append(last[2])
        elif t == 2:
            stack.pop()
        else:
            last = [t, v, []]
            stack[-1].append(last)
    nodes = {}

    def index(lst):
        for node in lst:
            if node[0] == 0x10:
                nodes[node[1]] = node
            index(node[2])
    index(root)
    for k, (name, parent, rot, trans) in enumerate(extra):
        node = [0x10, count + k, []]
        nodes[parent][2].append(node)
        nodes[count + k] = node

    def write(lst):
        out = b''
        for t, v, kids in lst:
            out += struct.pack('>HH', t, v)
            if kids:
                out += struct.pack('>HH', 1, 0) + write(kids) + struct.pack('>HH', 2, 0)
        return out
    graph = write(root) + struct.pack('>HH', 0, 0)
    head = bytearray(inf[:_u32(inf, 0x14)])
    new = bytearray(_pad(bytes(head) + graph))
    struct.pack_into('>I', new, 4, len(new))
    return jnt1, bytes(new)


def _wrap(rot):
    return [((r + 180.0) % 360.0) - 180.0 for r in rot]
