"""Minimal glTF 2.0 reader for skinned models (embedded or external buffers): meshes, skins and node rest poses."""
import base64
import json
import os

import numpy as np

_COMPONENTS = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


class Gltf:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.g = json.load(f)
        self.buffers = []
        for b in self.g['buffers']:
            uri = b.get('uri', '')
            if uri.startswith('data:'):
                self.buffers.append(base64.b64decode(uri.split(',', 1)[1]))
            else:
                with open(os.path.join(os.path.dirname(path), uri), 'rb') as f:
                    self.buffers.append(f.read())
        self.nodes = self.g['nodes']
        self.parent = {}
        for i, n in enumerate(self.nodes):
            for c in n.get('children', []):
                self.parent[c] = i

    def accessor(self, i):
        a = self.g['accessors'][i]
        view = self.g['bufferViews'][a['bufferView']]
        data = self.buffers[view['buffer']]
        dtype = np.dtype(_COMPONENTS[a['componentType']])
        width = _WIDTH[a['type']]
        start = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = view.get('byteStride') or dtype.itemsize * width
        raw = np.frombuffer(data, np.uint8, count=stride * (a['count'] - 1) + dtype.itemsize * width, offset=start)
        rows = np.lib.stride_tricks.as_strided(raw, (a['count'], dtype.itemsize * width), (stride, 1))
        out = rows.copy().view(dtype).reshape(a['count'], width)
        if a.get('normalized') and dtype.kind in 'iu':
            out = out.astype(np.float64) / np.iinfo(dtype).max
        return out

    def local(self, i):
        n = self.nodes[i]
        if 'matrix' in n:
            return np.array(n['matrix'], float).reshape(4, 4).T
        m = np.eye(4)
        m[:3, :3] = quat_to_mat(n.get('rotation', [0, 0, 0, 1])) * np.asarray(n.get('scale', [1, 1, 1]), float)
        m[:3, 3] = n.get('translation', [0, 0, 0])
        return m

    def world(self, i):
        m = self.local(i)
        while i in self.parent:
            i = self.parent[i]
            m = self.local(i) @ m
        return m

    def image_bytes(self, i):
        img = self.g['images'][i]
        uri = img.get('uri', '')
        if uri.startswith('data:'):
            return base64.b64decode(uri.split(',', 1)[1])
        if 'bufferView' in img:
            v = self.g['bufferViews'][img['bufferView']]
            return self.buffers[v['buffer']][v.get('byteOffset', 0):v.get('byteOffset', 0) + v['byteLength']]
        with open(os.path.join(os.path.dirname(self.path), uri), 'rb') as f:
            return f.read()

    def meshes(self):
        """Each skinned primitive as triangle corners: node name, material image, pos, nrm, uv, joints, weights."""
        out = []
        for ni, n in enumerate(self.nodes):
            if 'mesh' not in n:
                continue
            skin = self.g['skins'][n['skin']] if 'skin' in n else None
            names = [self.nodes[j].get('name') for j in skin['joints']] if skin else []
            for p in self.g['meshes'][n['mesh']]['primitives']:
                at = p['attributes']
                idx = self.accessor(p['indices'])[:, 0] if 'indices' in p else np.arange(self.g['accessors'][at['POSITION']]['count'])
                mat = self.g['materials'][p['material']] if 'material' in p else {}
                tex = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
                image = self.g['textures'][tex['index']]['source'] if tex else None
                joints = self.accessor(at['JOINTS_0'])[idx] if 'JOINTS_0' in at else None
                weights = self.accessor(at['WEIGHTS_0'])[idx].astype(float) if 'WEIGHTS_0' in at else None
                out.append({'node': n.get('name'), 'image': image, 'double_sided': mat.get('doubleSided', False),
                            'pos': self.accessor(at['POSITION'])[idx].astype(float),
                            'nrm': self.accessor(at['NORMAL'])[idx].astype(float) if 'NORMAL' in at else None,
                            'uv': self.accessor(at['TEXCOORD_0'])[idx].astype(float) if 'TEXCOORD_0' in at else None,
                            'joints': [[names[j] for j in row] for row in joints] if joints is not None else None,
                            'weights': weights, 'skin': skin})
        return out
