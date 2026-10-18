"""COLLADA (.dae) reader for skinned meshes: joints, bind pose, triangles, UVs, normals and weights."""
import math
import xml.etree.ElementTree as ET

import numpy as np

NS = '{http://www.collada.org/2005/11/COLLADASchema}'


def _floats(text):
    return np.array([float(x) for x in text.split()]) if text else np.zeros(0)


def _rot(axis, deg):
    x, y, z = axis / (np.linalg.norm(axis) or 1.0)
    a = math.radians(deg)
    c, s, t = math.cos(a), math.sin(a), 1 - math.cos(a)
    m = np.eye(4)
    m[:3, :3] = [[t * x * x + c, t * x * y - s * z, t * x * z + s * y],
                 [t * x * y + s * z, t * y * y + c, t * y * z - s * x],
                 [t * x * z - s * y, t * y * z + s * x, t * z * z + c]]
    return m


def _local(node):
    m = np.eye(4)
    for ch in node:
        tag = ch.tag[len(NS):]
        if tag == 'matrix':
            m = m @ _floats(ch.text).reshape(4, 4)
        elif tag == 'translate':
            t = np.eye(4); t[:3, 3] = _floats(ch.text); m = m @ t
        elif tag == 'rotate':
            v = _floats(ch.text); m = m @ _rot(v[:3], v[3])
        elif tag == 'scale':
            s = np.eye(4); s[0, 0], s[1, 1], s[2, 2] = _floats(ch.text); m = m @ s
    return m


class Mesh:
    """One skinned triangle list: positions (bind space), normals, uvs, weights per corner."""

    def __init__(self, name, material, pos, nrm, uvs, joints, weights):
        self.name, self.material = name, material
        self.pos, self.nrm = pos, nrm                      # (n,3), (n,3) per corner
        self.uvs = uvs                                     # every UV set, (n,2) per corner
        self.uv = uvs[0]
        self.joints, self.weights = joints, weights         # lists per corner: [(joint name, weight)]


class Dae:
    def __init__(self, path):
        root = ET.parse(path).getroot()
        self.root = root
        unit = root.find(NS + 'asset/' + NS + 'unit')
        self.meter = float(unit.get('meter')) if unit is not None else 1.0
        self.up = root.findtext(NS + 'asset/' + NS + 'up_axis') or 'Y_UP'
        self.nodes = {}       # name -> (parent name, local, world)
        self.sid_to_name = {}
        self.order = []
        self.instances = []   # (node name, controller or geometry url, {symbol: material})
        scene = root.find(NS + 'library_visual_scenes/' + NS + 'visual_scene')
        for n in scene.findall(NS + 'node'):
            self._walk(n, None, np.eye(4))
        self.geometries = {g.get('id'): g for g in root.iter(NS + 'geometry')}
        self.controllers = {c.get('id'): c for c in root.iter(NS + 'controller')}
        self.materials = {m.get('id'): m.get('name') for m in root.iter(NS + 'material')}
        self.effect_image = {}
        images = {i.get('id'): i.findtext(NS + 'init_from') for i in root.iter(NS + 'image')}
        effects = {}
        for e in root.iter(NS + 'effect'):
            surf = [s.findtext(NS + 'init_from') for s in e.iter(NS + 'surface')]
            effects[e.get('id')] = images.get(surf[0]) if surf else None
        for m in root.iter(NS + 'material'):
            url = m.find(NS + 'instance_effect').get('url')[1:]
            self.effect_image[m.get('name')] = effects.get(url)

    def _walk(self, node, parent, pw):
        name = node.get('name') or node.get('id')
        local = _local(node)
        world = pw @ local
        if node.get('type') == 'JOINT':
            self.nodes[name] = (parent, local, world)
            self.order.append(name)
            if node.get('sid'):
                self.sid_to_name[node.get('sid')] = name
            parent_joint = name
        else:
            parent_joint = parent
        for inst in list(node.findall(NS + 'instance_controller')) + list(node.findall(NS + 'instance_geometry')):
            binds = {b.get('symbol'): b.get('target')[1:] for b in inst.iter(NS + 'instance_material')}
            self.instances.append((name, inst.get('url')[1:], binds))
        for ch in node.findall(NS + 'node'):
            self._walk(ch, parent_joint, world)

    def _triangles(self, geom):
        mesh = geom.find(NS + 'mesh')
        sources = {s.get('id'): _floats(s.find(NS + 'float_array').text).reshape(
            -1, int(s.find('.//' + NS + 'accessor').get('stride'))) for s in mesh.findall(NS + 'source')}
        verts = mesh.find(NS + 'vertices')
        vsrc = {i.get('semantic'): i.get('source')[1:] for i in verts.findall(NS + 'input')}
        out = []
        for tri in mesh.findall(NS + 'triangles'):
            inputs = tri.findall(NS + 'input')
            stride = max(int(i.get('offset')) for i in inputs) + 1
            p = np.array([int(x) for x in tri.findtext(NS + 'p').split()]).reshape(-1, stride)
            sel = {}
            for i in inputs:
                sem, off, src = i.get('semantic'), int(i.get('offset')), i.get('source')[1:]
                key = sem + (i.get('set') or '0') if sem == 'TEXCOORD' else sem
                if sem == 'VERTEX':
                    sel['POSITION'] = (vsrc['POSITION'], off)
                    sel['VINDEX'] = (None, off)
                    if 'NORMAL' in vsrc and 'NORMAL' not in sel:
                        sel['NORMAL'] = (vsrc['NORMAL'], off)
                else:
                    sel.setdefault(key, (src, off))
            vidx = p[:, sel['VINDEX'][1]]
            pos = sources[sel['POSITION'][0]][p[:, sel['POSITION'][1]]]
            nrm = sources[sel['NORMAL'][0]][p[:, sel['NORMAL'][1]]] if 'NORMAL' in sel else np.zeros_like(pos)
            uvkey = sorted(k for k in sel if k.startswith('TEXCOORD'))
            uvs = [sources[sel[k][0]][p[:, sel[k][1]]][:, :2] for k in uvkey] or [np.zeros((len(pos), 2))]
            out.append((tri.get('material'), vidx, pos, nrm, uvs))
        return out

    def meshes(self, node_filter=None):
        """Skinned meshes of every instanced controller whose node name passes node_filter, in the
        skeleton's bind space."""
        res = []
        for node, url, binds in self.instances:
            if node_filter and not node_filter(node):
                continue
            if url not in self.controllers:
                continue
            skin = self.controllers[url].find(NS + 'skin')
            geom = self.geometries[skin.get('source')[1:]]
            bsm = _floats(skin.findtext(NS + 'bind_shape_matrix')).reshape(4, 4)
            src = {s.get('id'): s for s in skin.findall(NS + 'source')}
            jin = {i.get('semantic'): i.get('source')[1:] for i in skin.find(NS + 'joints').findall(NS + 'input')}
            sids = src[jin['JOINT']].find(NS + 'Name_array').text.split()
            names = [self.sid_to_name.get(s, s) for s in sids]
            vw = skin.find(NS + 'vertex_weights')
            win = {i.get('semantic'): (i.get('source')[1:], int(i.get('offset'))) for i in vw.findall(NS + 'input')}
            wts = _floats(src[win['WEIGHT'][0]].find(NS + 'float_array').text)
            vcount = [int(x) for x in vw.findtext(NS + 'vcount').split()]
            v = [int(x) for x in vw.findtext(NS + 'v').split()]
            per_vertex = []
            k = 0
            for c in vcount:
                lst = []
                for _ in range(c):
                    lst.append((names[v[k + win['JOINT'][1]]], float(wts[v[k + win['WEIGHT'][1]]])))
                    k += 2
                per_vertex.append(lst)
            for sym, vidx, pos, nrm, uvs in self._triangles(geom):
                p4 = np.c_[pos, np.ones(len(pos))] @ bsm.T
                n3 = nrm @ bsm[:3, :3].T
                mat = self.materials.get(binds.get(sym, sym), sym)
                res.append(Mesh(node, mat, p4[:, :3], n3, uvs,
                                [[j for j, _ in per_vertex[i]] for i in vidx],
                                [[w for _, w in per_vertex[i]] for i in vidx]))
        return res
