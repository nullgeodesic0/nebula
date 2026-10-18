"""Odyssey's face animations (PlayerAnimationFace.szs) on the custom model's face joints.

Odyssey animates Mario's face as a separate skeleton rooted at "Face", which hangs from the head. The custom
model has the same bones (by name) under Galaxy's Head joint, with their own rest frames and sometimes another
parent. So each bone's motion is taken relative to Face, carried into Head's frame through the rotation and
scale that best line the two rest skeletons up (fitted on the bones' rest positions), and turned back into
local joint transforms for the custom model's own hierarchy.
"""
import struct

import numpy as np

import bfres
import disc
import fbx
import retarget

# Galaxy-side action names whose face animation has another name in Odyssey's file.
FACE_ACTION = {'Wait oddysey idle': 'Wait', 'WalkSoft': 'Walk', 'Dash': 'Run', 'DashFast corrrer turbo': 'Run','SquatWalk': 'SquatWait'}
# How far a face animation moves from the idle face. Odyssey's run opens the jaw 20 degrees for as long as
# Mario runs; Odyssey's smooth skinning carries that, the model's coarser one stretches the chin.
FACE_STRENGTH = {'Run': 0.4}
NEUTRAL_FACE = 'Wait'


def read_szs(path):
    data = disc.yaz0_decode(open(path, 'rb').read())
    assert data[:4] == b'SARC'
    E = '<' if data[6:8] == b'\xff\xfe' else '>'
    hdr, = struct.unpack_from(E + 'H', data, 4)
    data_off, = struct.unpack_from(E + 'I', data, 0xC)
    count, = struct.unpack_from(E + 'H', data, hdr + 6)
    nodes = hdr + 12
    names = nodes + count * 16 + 8
    out = {}
    for i in range(count):
        _, attr, s, e = struct.unpack_from(E + 'IIII', data, nodes + i * 16)
        name = data[names + (attr & 0xFFFF) * 4:].split(b'\0')[0].decode()
        out[name] = data[data_off + s:data_off + e]
    return out


def _local(r_rad, t):
    m = np.eye(4)
    m[:3, :3] = np.array(bfres.euler_xyz_matrix(r_rad))
    m[:3, 3] = t
    return m


class FaceDriver:
    def __init__(self, rig, szs_path, fbx_path):
        files = read_szs(szs_path)
        self.anims = {a.name: a for a in bfres.Bfres(files['PlayerAnimationFace.bfres']).skeletal_anims()}
        skel = fbx.bones(fbx_path)
        self.parent = {n: b['parent'] for n, b in skel.items()}
        self.rest_local = {n: _local(np.radians(b['r']), b['t']) for n, b in skel.items()}
        names = rig.names
        head = names.index('Head')

        def under_head(j):
            while j >= 0 and j != head:
                j = rig.par[j]
            return j == head
        # The custom model's face joints: those Odyssey's face skeleton has, below Galaxy's Head.
        self.joints = [j for j, n in enumerate(names) if n in skel and n != 'Face' and under_head(j)]
        self.rig = rig
        # Head-relative rest transforms of the model's joints.
        self.h_rest = {head: np.eye(4)}
        for j in self._order():
            m = np.eye(4)
            m[:3, :3] = rig.rest_rot[j]
            m[:3, 3] = rig.rest_trans[j]
            self.h_rest[j] = self.h_rest[rig.par[j]] @ m
        f_rest = self.face_relative({}, rest=True)
        # Head <- Face: rotation and scale fitted on the rest positions (Umeyama).
        src = np.array([f_rest[names[j]][:3, 3] for j in self.joints])
        dst = np.array([self.h_rest[j][:3, 3] for j in self.joints])
        ms, md = src.mean(0), dst.mean(0)
        cov = (dst - md).T @ (src - ms) / len(src)
        u, d, vt = np.linalg.svd(cov)
        fix = np.eye(3)
        if np.linalg.det(u @ vt) < 0:
            fix[2, 2] = -1
        self.M = u @ fix @ vt                       # Face-frame vectors -> Head frame
        self.s = float(np.trace(np.diag(d) @ fix) / ((src - ms) ** 2).sum(1).mean())
        self.fit_error = float(np.abs((self.s * (self.M @ (src - ms).T)).T + md - dst).max())
        # Per joint: the constant rotation from the Odyssey bone's frame to the model joint's (both seen from Head).
        self.K = {j: (self.M @ f_rest[names[j]][:3, :3]).T @ self.h_rest[j][:3, :3] for j in self.joints}
        self.f_rest = f_rest

    def _order(self):
        """The face joints and the joints between them and Head, parents first."""
        need = set()
        head = self.rig.names.index('Head')
        for j in self.joints:
            while j != head:
                need.add(j)
                j = self.rig.par[j]
        return sorted(need)

    def face_relative(self, pose, rest=False):
        """Face-relative transforms of Odyssey's face bones; pose: {bone: (s, r, t)} overriding the rest."""
        out = {'Face': np.eye(4)}

        def get(n):
            if n in out:
                return out[n]
            if n in pose and not rest:
                _, r, t = pose[n]
                local = _local(r, t)
            else:
                local = self.rest_local[n]
            p = self.parent.get(n)
            m = (get(p) if p and p != 'Face' and p in self.parent else np.eye(4)) @ local
            out[n] = m
            return m
        for n in self.parent:
            if n not in ('Face', 'AllRoot', 'nw4f_root', 'Armature') and self._under_face(n):
                get(n)
        return out

    def _under_face(self, n):
        while n:
            if n == 'Face':
                return True
            n = self.parent.get(n)
        return False

    def anim_for(self, action_name):
        return self.anims.get(FACE_ACTION.get(action_name, action_name))

    def sample(self, anim, frames):
        """Per face joint: Euler angles (degrees) and translations of the model's joint, per output frame.
        frames: Odyssey face-animation frame per output frame."""
        names = self.rig.names
        eul = {j: np.zeros((len(frames), 3)) for j in self.joints}
        tr = {j: np.zeros((len(frames), 3)) for j in self.joints}
        bones = {b.name: b for b in anim.bones}
        strength = FACE_STRENGTH.get(anim.name, 1.0)
        neutral = {b.name: b.sample(0.0) for b in self.anims[NEUTRAL_FACE].bones}
        for i, u in enumerate(frames):
            pose = {}
            for n, b in bones.items():
                v = b.sample(u)
                if strength != 1.0 and n in neutral:
                    z = neutral[n]
                    v = {k: [z[k][c] + strength * (v[k][c] - z[k][c]) for c in range(len(v[k]))] for k in v}
                pose[n] = (v['s'], v['r'][:3], v['t'])
            f = self.face_relative(pose)
            h = {}
            for j in self.joints:
                n = names[j]
                rot = self.M @ f[n][:3, :3] @ self.K[j]
                pos = self.h_rest[j][:3, 3] + self.s * (self.M @ (f[n][:3, 3] - self.f_rest[n][:3, 3]))
                m = np.eye(4)
                m[:3, :3] = rot
                m[:3, 3] = pos
                h[j] = m
            for j in self.joints:
                p = self.rig.par[j]
                parent = h[p] if p in h else self.h_rest[p]
                local = np.linalg.inv(parent) @ h[j]
                eul[j][i] = retarget.mat_to_euler(local[:3, :3])
                tr[j][i] = local[:3, 3]
        for j in self.joints:  # no jumps of a full turn between frames
            eul[j] = np.degrees(np.unwrap(np.radians(eul[j]), axis=0))
        return eul, tr
