"""Minimal reader for Blender 4.x/5.x .blend files: just enough for an armature and its actions.

Reads the file's own SDNA (struct layout) so no Blender install is needed. Supports the 64-bit
little-endian layouts, both the classic 12-byte header ("BLENDER-v4xx") and the 17-byte header of
newer files ("BLENDER17-01v0501"), and the layered actions of Blender 4.4+ (layers, strips,
channelbags) as well as legacy action fcurves.
"""
import re
import struct

import numpy as np

_PRIM = {'char': 1, 'uchar': 1, 'short': 2, 'ushort': 2, 'int': 4, 'uint': 4, 'float': 4, 'double': 8,
         'int64_t': 8, 'uint64_t': 8, 'long': 4, 'ulong': 4, 'void': 0, 'int8_t': 1, 'uint8_t': 1,
         'int16_t': 2, 'uint16_t': 2, 'int32_t': 4, 'uint32_t': 4}
_FMT = {'char': 'b', 'uchar': 'B', 'short': 'h', 'ushort': 'H', 'int': 'i', 'uint': 'I', 'float': 'f',
        'double': 'd', 'int64_t': 'q', 'uint64_t': 'Q', 'int8_t': 'b', 'uint8_t': 'B', 'int16_t': 'h',
        'uint16_t': 'H', 'int32_t': 'i', 'uint32_t': 'I'}


class Blend:
    def __init__(self, path):
        self.f = open(path, 'rb')
        magic = self.f.read(7)
        if magic != b'BLENDER':
            raise ValueError('not a .blend file (compressed files must be saved uncompressed)')
        rest = self.f.read(5)
        if rest[:2].isdigit():  # BLENDER17-01v0501
            size = int(rest[:2])
            self.f.seek(0)
            hdr = self.f.read(size)
            if hdr[9:10] != b'-' or hdr[10:12] != b'01':
                raise ValueError('unsupported .blend header %r' % hdr)
            self.bhead = 32
        else:  # BLENDER-v405
            if rest[:2] != b'-v':
                raise ValueError('only 64-bit little-endian .blend files are supported')
            self.bhead = 24
        self.blocks = []
        self.byptr = {}
        dna = None
        while True:
            h = self.f.read(self.bhead)
            if len(h) < self.bhead:
                break
            code = h[:4]
            if self.bhead == 32:
                sdna, ptr, ln, nr = struct.unpack('<IQqq', h[4:32])
            else:
                ln, ptr, sdna, nr = struct.unpack('<iQii', h[4:24])
            off = self.f.tell()
            if code == b'DNA1':
                dna = self.f.read(ln)
            else:
                self.f.seek(ln, 1)
            b = (code, sdna, ptr, ln, nr, off)
            self.blocks.append(b)
            if ptr:
                self.byptr.setdefault(ptr, b)
            if code == b'ENDB':
                break
        if dna is None:
            raise ValueError('.blend file has no DNA1 block')
        self._sal = {}
        self._parse_dna(dna)

    def _parse_dna(self, d):
        p = 8
        n = struct.unpack_from('<I', d, p)[0]; p += 4
        names = []
        for _ in range(n):
            e = d.index(b'\0', p); names.append(d[p:e].decode()); p = e + 1
        p = (p + 3) & ~3
        p += 4
        n = struct.unpack_from('<I', d, p)[0]; p += 4
        types = []
        for _ in range(n):
            e = d.index(b'\0', p); types.append(d[p:e].decode()); p = e + 1
        p = (p + 3) & ~3
        p += 4
        tlen = struct.unpack_from('<%dH' % n, d, p); p += 2 * n
        p = (p + 3) & ~3
        p += 4
        n = struct.unpack_from('<I', d, p)[0]; p += 4
        structs = []
        for _ in range(n):
            t, nf = struct.unpack_from('<HH', d, p); p += 4
            fl = []
            for _ in range(nf):
                ft, fn = struct.unpack_from('<HH', d, p); p += 4
                fl.append((types[ft], names[fn]))
            structs.append((types[t], fl))
        self.tlen = dict(zip(types, tlen))
        self.structs = structs
        self.sidx = {s[0]: i for i, s in enumerate(structs)}
        self.layout = {}
        for name, fl in structs:
            off = 0
            lay = []
            for ft, fn in fl:
                ptr = fn.startswith('*') or fn.startswith('(*')
                core = fn.lstrip('*')
                dims = []
                if '[' in core and not fn.startswith('(*'):
                    base = core[:core.index('[')]
                    for part in core[core.index('['):].split('[')[1:]:
                        dims.append(int(part.rstrip(']')))
                else:
                    base = core
                cnt = 1
                for x in dims:
                    cnt *= x
                if fn.startswith('(*'):
                    sz = 8; base = fn
                elif ptr:
                    sz = 8 * cnt
                else:
                    sz = self.tlen[ft] * cnt
                al = 8 if ptr else (_PRIM[ft] if ft in _PRIM else self._salign(ft))
                if al:
                    off = (off + al - 1) & ~(al - 1)
                lay.append((base, ft, ptr, dims, off, sz))
                off += sz
            self.layout[name] = lay

    def _salign(self, t):
        if t in self._sal:
            return self._sal[t]
        self._sal[t] = 1
        a = 1
        for ft, fn in self.structs[self.sidx[t]][1]:
            if fn.startswith('*') or fn.startswith('(*'):
                a = max(a, 8)
            elif ft in _PRIM:
                a = max(a, _PRIM[ft] or 1)
            elif ft in self.sidx:
                a = max(a, self._salign(ft))
            else:
                a = max(a, 8)
        self._sal[t] = a
        return a

    def data(self, b):
        self.f.seek(b[5])
        return self.f.read(b[3])

    def read(self, data, off, tname):
        out = {}
        for base, ft, ptr, dims, o, sz in self.layout[tname]:
            q = off + o
            cnt = 1
            for x in dims:
                cnt *= x
            if ptr:
                out[base] = struct.unpack_from('<%dQ' % cnt, data, q) if dims else struct.unpack_from('<Q', data, q)[0]
            elif ft in _FMT:
                if ft == 'char' and dims:
                    out[base] = data[q:q + cnt].split(b'\0')[0].decode('utf8', 'replace')
                elif cnt > 1:
                    out[base] = struct.unpack_from('<%d%s' % (cnt, _FMT[ft]), data, q)
                else:
                    out[base] = struct.unpack_from('<' + _FMT[ft], data, q)[0]
            elif ft in self.layout and not dims:
                out[base] = self.read(data, q, ft)
        return out

    def raw(self, ptr):
        b = self.byptr.get(ptr) if ptr else None
        return self.data(b) if b else None

    def struct_at(self, ptr, tname):
        d = self.raw(ptr)
        return self.read(d, 0, tname) if d else None

    def ptr_array(self, ptr, tname, n):
        if not ptr or not n:
            return []
        ptrs = struct.unpack('<%dQ' % n, self.raw(ptr)[:8 * n])
        return [self.struct_at(p, tname) for p in ptrs]

    def cstr(self, ptr):
        d = self.raw(ptr)
        return d.split(b'\0')[0].decode('utf8', 'replace') if d else None

    def ids(self, code):
        return [b for b in self.blocks if b[0] == code]


def read_armature(B):
    """Bones of the file's (first) armature: name -> parent, armature-space rest matrix (4x4, columns)."""
    ar = B.ids(b'AR\0\0')
    if not ar:
        raise ValueError('the .blend file has no armature')
    a = B.read(B.data(ar[0]), 0, 'bArmature')
    bones, order = {}, []

    def walk(ptr, parent):
        while ptr:
            bd = B.struct_at(ptr, 'Bone')
            m = np.array(bd['arm_mat'], dtype=float).reshape(4, 4).T  # columns are axes
            bones[bd['name']] = {'parent': parent, 'mat': m}
            order.append(bd['name'])
            walk(bd['childbase']['first'], bd['name'])
            ptr = bd['next']

    walk(a['bonebase']['first'], None)
    return order, bones


def scene_fps(B):
    sc = B.ids(b'SC\0\0')
    if not sc:
        return None
    r = B.read(B.data(sc[0]), 0, 'Scene')['r']
    return r['frs_sec'] / (r['frs_sec_base'] or 1.0)


def _action_fcurves(B, a):
    """(rna_path, array_index, [(frame, value)]) for every fcurve of a layered or legacy action."""
    out = []

    def add(fc):
        n = fc['totvert']
        keys = []
        if n and fc['bezt']:
            d = B.raw(fc['bezt'])
            sz = B.tlen['BezTriple']
            for i in range(n):
                v = struct.unpack_from('<9f', d, i * sz)
                keys.append((v[3], v[4]))
        out.append((B.cstr(fc['rna_path']), fc['array_index'], keys))

    if 'layer_array' in a and a.get('layer_array_num'):
        kd = B.ptr_array(a['strip_keyframe_data_array'], 'ActionStripKeyframeData', a['strip_keyframe_data_array_num'])
        for layer in B.ptr_array(a['layer_array'], 'ActionLayer', a['layer_array_num']):
            for st in B.ptr_array(layer['strip_array'], 'ActionStrip', layer['strip_array_num']):
                data = kd[st['data_index']]
                for cb in B.ptr_array(data['channelbag_array'], 'ActionChannelBag', data['channelbag_array_num']):
                    for fc in B.ptr_array(cb['fcurve_array'], 'FCurve', cb['fcurve_array_num']):
                        add(fc)
    ptr = a['curves']['first']
    while ptr:
        fc = B.struct_at(ptr, 'FCurve')
        add(fc)
        ptr = fc['next']
    return out


def list_actions(B):
    return [B.read(B.data(b), 0, 'bAction')['id']['name'][2:] for b in B.ids(b'AC\0\0')]


def read_actions(B, names):
    """name -> {'nf': frames, 'bones': {bone: {'location': (nf,3), 'rotation_quaternion': (nf,4)}}},
    sampled at whole frames (linear between keys; the files this reads are baked per frame)."""
    want = set(names)
    out = {}
    pat = re.compile(r'pose\.bones\["(.+)"\]\.(location|rotation_quaternion)$')
    for b in B.ids(b'AC\0\0'):
        a = B.read(B.data(b), 0, 'bAction')
        name = a['id']['name'][2:]
        if name not in want:
            continue
        ch = {}
        nf = 0
        for path, idx, keys in _action_fcurves(B, a):
            m = pat.match(path or '')
            if not m or not keys:
                continue
            nf = max(nf, int(max(k[0] for k in keys)) + 1)
            ch[(m.group(1), m.group(2), idx)] = keys
        bones = {}
        frames = np.arange(nf, dtype=float)
        for (bone, kind, idx), keys in ch.items():
            kt = np.array([k[0] for k in keys]); kv = np.array([k[1] for k in keys])
            bones.setdefault(bone, {}).setdefault(kind, {})[idx] = np.interp(frames, kt, kv)
        res = {}
        for bone, kinds in bones.items():
            res[bone] = {}
            for kind, comps in kinds.items():
                width = 4 if kind == 'rotation_quaternion' else 3
                default = [1.0, 0.0, 0.0, 0.0] if width == 4 else [0.0, 0.0, 0.0]
                res[bone][kind] = np.stack(
                    [comps.get(i, np.full(nf, default[i])) for i in range(width)], 1)
        out[name] = {'nf': nf, 'bones': res}
    return out


def read_meshes(B):
    """Skinned mesh objects of a Blender 2.8x/2.9x file (MVert/MPoly/MLoop/MLoopUV/MDeformVert):
    [{'object', 'material', 'pos', 'nrm', 'uv', 'joints', 'weights'}] per material, positions and normals in
    world space, one entry per triangle corner, weights by vertex group (bone) name."""
    out = []
    for b in B.ids(b'OB\0\0'):
        o = B.read(B.data(b), 0, 'Object')
        if o['type'] != 1 or not o['data']:
            continue
        me = B.struct_at(o['data'], 'Mesh')
        nv, npoly, nloop = me['totvert'], me['totpoly'], me['totloop']
        if not npoly or not me['mvert'] or not me['mloop']:
            continue
        obmat = np.array(o['obmat'], dtype=float).reshape(4, 4).T
        vraw = B.raw(me['mvert'])
        sv = B.tlen['MVert']
        co = np.array([struct.unpack_from('<3f', vraw, i * sv) for i in range(nv)])
        no = np.array([struct.unpack_from('<3h', vraw, i * sv + 12) for i in range(nv)], dtype=float) / 32767.0
        praw = B.raw(me['mpoly'])
        polys = [struct.unpack_from('<iih', praw, i * B.tlen['MPoly']) for i in range(npoly)]
        lraw = B.raw(me['mloop'])
        loops = [struct.unpack_from('<i', lraw, i * B.tlen['MLoop'])[0] for i in range(nloop)]
        uvraw = B.raw(me['mloopuv']) if me['mloopuv'] else None
        uvs = (np.array([struct.unpack_from('<2f', uvraw, i * B.tlen['MLoopUV']) for i in range(nloop)])
               if uvraw else np.zeros((nloop, 2)))
        groups = []
        g = o['defbase']['first']
        while g:
            dg = B.struct_at(g, 'bDeformGroup')
            groups.append(dg['name'])
            g = dg['next']
        weights = [[] for _ in range(nv)]
        if me['dvert']:
            draw = B.raw(me['dvert'])
            for i in range(nv):
                dwp, tot, _ = struct.unpack_from('<Qii', draw, i * B.tlen['MDeformVert'])
                if dwp and tot:
                    dwr = B.raw(dwp)
                    for k in range(tot):
                        nr, w = struct.unpack_from('<if', dwr, k * 8)
                        if 0 <= nr < len(groups) and w > 0:
                            weights[i].append((groups[nr], w))
        mats = []
        if me['mat'] and me['totcol']:
            ptrs = struct.unpack('<%dQ' % me['totcol'], B.raw(me['mat'])[:8 * me['totcol']])
            for q in ptrs:
                mats.append(B.struct_at(q, 'Material')['id']['name'][2:] if q else '')
        world = (obmat[:3, :3] @ co.T).T + obmat[:3, 3]
        nworld = (np.linalg.inv(obmat[:3, :3]).T @ no.T).T
        per_mat = {}
        for start, count, mat_nr in polys:
            for t in range(1, count - 1):
                for li in (start, start + t, start + t + 1):
                    per_mat.setdefault(mat_nr, []).append(li)
        for mat_nr, corner_loops in per_mat.items():
            vi = [loops[li] for li in corner_loops]
            out.append({
                'object': o['id']['name'][2:],
                'material': mats[mat_nr] if mat_nr < len(mats) else '',
                'pos': world[vi], 'nrm': nworld[vi], 'uv': uvs[corner_loops],
                'joints': [[j for j, _ in weights[v]] for v in vi],
                'weights': [[w for _, w in weights[v]] for v in vi],
            })
    return out