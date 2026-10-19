"""Retargets a Blender armature's actions onto Mario's skeleton in Super Mario Galaxy.

The source rig (the Super Mario Odyssey Mario skeleton as imported into Blender) and Galaxy's Mario
share joint names and local axis conventions for the body: each bone's x axis runs along the limb.
Measured on the two rest poses, the legs, spine and head agree exactly, the feet differ by 10 degrees
and the arms by a fixed half-turn about the limb (a roll convention). So for every mapped joint

    galaxy world rotation = source world rotation (posed) * C,  C = source rest^-1 * galaxy rest

which keeps each limb pointing the same way in the world, and the Galaxy local rotation follows from
its parent's. Translations carry over as offsets from the rest pose, in the parent's frame.
"""
import math

import numpy as np

# Galaxy joint (mario.bdl, MarioAnime.arc BCKs) -> source bone.
GALAXY_TO_SOURCE = {
    'All_Root': 'nw4f_root', 'Center': 'AllRoot', 'JointRoot': 'JointRoot', 'Hip': 'Hip',
    'LegL1': 'LegL1', 'LegL2': 'LegL2', 'FootL': 'FootL',
    'LegR1': 'LegR1', 'LegR2': 'LegR2', 'FootR': 'FootR',
    'Spine1': 'Spine1', 'Spine2': 'Spine2', 'Head': 'Head',
    'ShoulderL': 'ShoulderL', 'ArmL1': 'ArmL1', 'ArmL2': 'ArmL2', 'HandL': 'HandL',
    'ShoulderR': 'ShoulderR', 'ArmR1': 'ArmR1', 'ArmR2': 'ArmR2', 'HandR': 'HandR',
}
# The source's cap bone is not followed: Galaxy's model has hair and scalp under the cap that do not move
# with it, so a moving cap shows them through its top. (Add 'Cap0': 'Cap' here, and bind the hat to Cap0 in
# build_model_mod.py, to try it.)
# Joints whose source bone turns about a different point than the Galaxy joint: Galaxy's Cap0 sits at the
# base of the head, the source cap bone near its top, so the cap's turn is moved to the source's pivot.
PIVOTS = {'Cap0': ('Cap', 'Head')}


def _rx(a): c, s = math.cos(a), math.sin(a); return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
def _ry(a): c, s = math.cos(a), math.sin(a); return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
def _rz(a): c, s = math.cos(a), math.sin(a); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def euler_to_mat(deg):
    a = np.radians(deg)
    return _rz(a[2]) @ _ry(a[1]) @ _rx(a[0])  # J3D order: X, then Y, then Z


def mat_to_euler(m):
    sy = max(-1.0, min(1.0, -m[2, 0]))
    b = math.asin(sy)
    if abs(sy) < 0.99999:
        a, c = math.atan2(m[2, 1], m[2, 2]), math.atan2(m[1, 0], m[0, 0])
    else:
        a, c = math.atan2(-m[1, 2], m[1, 1]), 0.0
    return np.degrees([a, b, c])


def quat_to_mat(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


class Rig:
    def __init__(self, galaxy_joints, galaxy_parents, source_order, source_bones):
        self.names = [j[0] for j in galaxy_joints]
        self.nj = len(galaxy_joints)
        self.par = galaxy_parents
        self.rest_rot = [euler_to_mat(np.array(j[2])) for j in galaxy_joints]
        self.rest_trans = [np.array(j[3], dtype=float) for j in galaxy_joints]
        self.world_rest = []
        for i in range(self.nj):
            p = self.par[i]
            self.world_rest.append((self.world_rest[p] if p >= 0 else np.eye(3)) @ self.rest_rot[i])
        self.src_order = source_order
        self.src_parent = {n: b['parent'] for n, b in source_bones.items()}
        self.src_world_rest = {n: b['mat'][:3, :3] for n, b in source_bones.items()}
        self.src_rest_pos = {n: b['mat'][:3, 3] for n, b in source_bones.items()}
        self.src_local_rest = {}
        for n in source_order:
            p = self.src_parent[n]
            r = self.src_world_rest[n]
            self.src_local_rest[n] = r if p is None else self.src_world_rest[p].T @ r
        self.map = {}
        for i, name in enumerate(self.names):
            # Joints added to Galaxy's skeleton (the custom model's face and fingers) share the source's names.
            src = GALAXY_TO_SOURCE.get(name, name)
            if src in source_bones:
                self.map[i] = src
        self.C = {i: self.src_world_rest[s].T @ self.world_rest[i] for i, s in self.map.items()}
        self.pivot = {}
        for i, name in enumerate(self.names):
            if name in PIVOTS and i in self.map:
                bone, base = PIVOTS[name]
                p = self.par[i]
                # The source pivot, relative to the head, in the Galaxy joint's parent frame.
                self.pivot[i] = self.world_rest[p].T @ (self.src_rest_pos[bone] - self.src_rest_pos[base])

    def rest_difference(self):
        """Degrees between the two rest poses for every mapped joint."""
        out = {}
        for i, s in self.map.items():
            rel = self.C[i]
            out[self.names[i]] = math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(rel) - 1) / 2))))
        return out

    def source_pose(self, bones):
        """World rotations and parent-frame translation offsets of the source bones for one pose
        (bones: name -> {'rotation_quaternion': (4,), 'location': (3,)})."""
        world, offset = {}, {}
        for n in self.src_order:
            b = bones.get(n, {})
            q = b.get('rotation_quaternion')
            loc = b.get('location')
            rq = quat_to_mat(q) if q is not None else np.eye(3)
            p = self.src_parent[n]
            world[n] = (world[p] if p else np.eye(3)) @ self.src_local_rest[n] @ rq
            offset[n] = self.src_local_rest[n] @ loc if loc is not None else np.zeros(3)
        return world, offset

    def galaxy_pose(self, bones):
        """Local rotation matrix and translation offset (from rest) of each mapped Galaxy joint."""
        world, offset = self.source_pose(bones)
        # Every joint's world rotation: mapped joints from the source, the rest from their parent and
        # their own rest rotation (CapPosition, between Head and Cap0, for one).
        gworld = []
        for i in range(self.nj):
            p = self.par[i]
            if i in self.map:
                gworld.append(world[self.map[i]] @ self.C[i])
            else:
                gworld.append((gworld[p] if p >= 0 else np.eye(3)) @ self.rest_rot[i])
        rot, trans = {}, {}
        for i, s in self.map.items():
            p = self.par[i]
            parent = gworld[p] if p >= 0 else np.eye(3)
            rot[i] = parent.T @ gworld[i]
            # The offset is in the source parent's frame; carried through the world into Galaxy's.
            sp = self.src_parent[s]
            v = (world[sp] if sp else np.eye(3)) @ offset[s]
            trans[i] = parent.T @ v
            if i in self.pivot:
                c = self.pivot[i]
                trans[i] = trans[i] + c - (rot[i] @ np.linalg.inv(self.rest_rot[i])) @ c
        return rot, trans


def _cycle_period(act):
    nf = act['nf']
    if nf < 3:
        return max(nf, 1)

    def dist(f, g):
        d = 0.0
        for b in act['bones'].values():
            for kind, a in b.items():
                x, y = a[f], a[g]
                if kind == 'rotation_quaternion' and np.dot(x, y) < 0:
                    y = -y  # q and -q are the same turn
                d += float(np.sum((x - y) ** 2))
        return d ** 0.5

    end = dist(nf - 1, 0)
    step = np.mean([dist(i + 1, i) for i in range(nf - 1)])
    # The last frame repeats the first (a closed cycle) only when it is much nearer to it than a step; in an
    # even motion (a roll) the last frame is one ordinary step before the first.
    return nf - 1 if end <= 0.5 * step else nf


def _pose_at(act, u, loop, period):
    nf = act['nf']
    f0 = math.floor(u)
    t = u - f0
    if loop:
        f0, f1 = f0 % period, (f0 + 1) % period
    else:
        f0, f1 = min(f0, nf - 1), min(f0 + 1, nf - 1)
    def mix(kind, a):
        a0, a1 = a[f0], a[f1]
        if kind == 'rotation_quaternion' and np.dot(a0, a1) < 0:
            a1 = -a1  # q and -q are the same turn; blend along the short way
        return a0 * (1 - t) + a1 * t

    return {n: {k: mix(k, a) for k, a in b.items()} for n, b in act['bones'].items()}


def sample(rig, act, dur, loop, translation_scale=1.0, hold=None, scale_root=False):
    """Galaxy Euler angles and translation offsets per frame 0..dur for one action.

    Loops map one source cycle onto the game's cycle of dur frames, so the game's own playback rate
    (which follows Mario's speed) keeps the feet in step. One-shots keep the source timing and stop at
    the game's length; a shorter source is stretched to fill it."""
    nf = act['nf']
    period = _cycle_period(act) if loop else nf - 1
    eul = np.zeros((dur + 1, rig.nj, 3))
    tr = np.zeros((dur + 1, rig.nj, 3))
    # A game loop far shorter than the source cycle is a held pose (the wall slide): the source's
    # first frame, rather than its whole cycle squeezed into a few frames.
    held = hold is not None or (loop and period > 4 * dur)
    us = []
    for f in range(dur + 1):
        if held:
            u = float(hold if hold is not None and hold >= 0 else (nf - 1 if hold is not None else 0))
        elif loop:
            u = (f / dur) * period if f < dur else 0.0
        elif nf - 1 >= dur:
            u = float(f)
        else:
            u = (f / dur) * (nf - 1)
        us.append(u)
        rot, trans = rig.galaxy_pose(_pose_at(act, u, loop, period))
        for i, m in rot.items():
            eul[f, i] = mat_to_euler(m)
            tr[f, i] = trans[i] * (translation_scale if scale_root else 1.0)
    for i in rig.map:
        # Each rotation has two Euler solutions; near a quarter-turn pitch the first one can jump by half a
        # turn between frames, which plays as a hitch between frames. Keep the one closest to the frame before.
        for f in range(1, len(eul)):
            a, b, c = eul[f, i]
            alt = np.array([a + 180.0, 180.0 - b, c + 180.0])
            prev = eul[f - 1, i]

            def dist(e):
                return np.abs((e - prev + 180.0) % 360.0 - 180.0).sum()
            if dist(alt) < dist(eul[f, i]):
                eul[f, i] = alt
        for a in range(3):
            eul[:, i, a] = np.degrees(np.unwrap(np.radians(eul[:, i, a])))
    if translation_scale != 1.0 and not scale_root:
        _scale_body_motion(rig, eul, tr, translation_scale)
    sample.source_frames = us  # the source frame each game frame was taken from
    return eul, tr, period


def _world_positions(rig, eul, tr, f):
    world, pos = [], []
    for j in range(rig.nj):
        p = rig.par[j]
        rot = euler_to_mat(eul[f, j]) if j in rig.map else rig.rest_rot[j]
        local_t = rig.rest_trans[j] + tr[f, j]
        pw = world[p] if p >= 0 else np.eye(3)
        pp = pos[p] if p >= 0 else np.zeros(3)
        world.append(pw @ rot)
        pos.append(pp + pw @ local_t)
    return world, pos


def _scale_body_motion(rig, eul, tr, scale):
    """Scales how far the body's centre moves rather than the root bone's offsets. The source rotates
    flips about the hips and moves the root to keep the turn centred; scaling the root's offsets would
    leave the body swinging round the hips, while scaling the centre's path keeps the turn centred."""
    root = rig.names.index('JointRoot')
    centre = rig.names.index('Spine1')
    parent = rig.par[root]
    full = [_world_positions(rig, eul, tr, f) for f in range(len(eul))]
    # Relative to where the rest pose holds the centre, so a source that starts crouched or lifted is
    # pulled toward Galaxy's own height too.
    rest_eul = np.zeros_like(eul[:1])
    for i in rig.map:
        rest_eul[0, i] = mat_to_euler(rig.rest_rot[i])
    c0 = _world_positions(rig, rest_eul, np.zeros_like(tr[:1]), 0)[1][centre]
    for f, (world, pos) in enumerate(full):
        shift = (scale - 1.0) * (pos[centre] - c0)
        tr[f, root] += world[parent].T @ shift
