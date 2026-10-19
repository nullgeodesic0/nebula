"""Reader for Switch BFRES skeletal animations (FSKA, file version 0.8): per bone, per frame local scale,
Euler XYZ rotation (radians) and translation."""
import math
import struct


class Bfres:
    def __init__(self, data):
        assert data[:8] == b'FRES    '
        self.d = data

    def u16(self, o): return struct.unpack_from('<H', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.d, o)[0]
    def u64(self, o): return struct.unpack_from('<Q', self.d, o)[0]
    def f32(self, o): return struct.unpack_from('<f', self.d, o)[0]

    def string(self, o):
        n = self.u16(o)
        return self.d[o + 2:o + 2 + n].decode('utf8', 'replace')

    def skeletal_anims(self):
        base, count = self.u64(0x38), self.u16(0xBE)
        return [SkeletalAnim(self, base + i * 0x60) for i in range(count)]


class SkeletalAnim:
    def __init__(self, f, o):
        assert f.d[o:o + 4] == b'FSKA'
        self.name = f.string(f.u64(o + 0x10))
        bone_array = f.u64(o + 0x30)
        self.flags = f.u32(o + 0x48)
        self.frames = struct.unpack_from('<i', f.d, o + 0x4C)[0]
        self.loop = bool(self.flags & 4)
        self.euler = bool(self.flags & 0x1000)
        nbones = f.u16(o + 0x58)
        self.bones = [BoneAnim(f, bone_array + i * 0x28) for i in range(nbones)]


# Offsets of the animated values in a bone's data: scale xyz, translate xyz, rotate xyzw.
_TARGETS = {0x4: ('s', 0), 0x8: ('s', 1), 0xC: ('s', 2), 0x10: ('t', 0), 0x14: ('t', 1), 0x18: ('t', 2),
            0x20: ('r', 0), 0x24: ('r', 1), 0x28: ('r', 2), 0x2C: ('r', 3)}


class BoneAnim:
    def __init__(self, f, o):
        self.name = f.string(f.u64(o))
        curves, base = f.u64(o + 8), f.u64(o + 0x10)
        flags = f.u32(o + 0x18)
        ncurve = f.d[o + 0x1E]
        self.base = {'s': [1.0, 1.0, 1.0], 'r': [0.0, 0.0, 0.0, 1.0], 't': [0.0, 0.0, 0.0]}
        p = base
        if flags & 0x8:
            self.base['s'] = [f.f32(p + 4 * k) for k in range(3)]; p += 12
        if flags & 0x10:
            self.base['r'] = [f.f32(p + 4 * k) for k in range(4)]; p += 16
        if flags & 0x20:
            self.base['t'] = [f.f32(p + 4 * k) for k in range(3)]; p += 12
        self.curves = [Curve(f, curves + i * 0x30) for i in range(ncurve)]

    def sample(self, frame):
        v = {k: list(x) for k, x in self.base.items()}
        for c in self.curves:
            key = _TARGETS.get(c.target)
            if key:
                v[key[0]][key[1]] = c.eval(frame)
        return v


class Curve:
    def __init__(self, f, o):
        frames_at, keys_at = f.u64(o), f.u64(o + 8)
        flags, n = f.u16(o + 0x10), f.u16(o + 0x12)
        self.target = f.u32(o + 0x14)
        self.start, self.end, scale, offset, delta = struct.unpack_from('<5f', f.d, o + 0x18)
        ftype, ktype, self.kind = flags & 3, (flags >> 2) & 3, (flags >> 4) & 7
        if ftype == 0:
            self.frames = list(struct.unpack_from('<%df' % n, f.d, frames_at))
        elif ftype == 1:
            self.frames = [v / 32.0 for v in struct.unpack_from('<%dh' % n, f.d, frames_at)]
        else:
            self.frames = list(struct.unpack_from('<%dB' % n, f.d, frames_at))
        per = {0: 4, 1: 2, 2: 1}.get(self.kind, 1)
        fmt = {0: 'f', 1: 'h', 2: 'b'}[ktype]
        raw = struct.unpack_from('<%d%s' % (n * per, fmt), f.d, keys_at)
        if ktype == 0:
            vals = list(raw)
            # Float keys hold the values themselves, without the scale applied.
            self.keys = [vals[i * per:(i + 1) * per] for i in range(n)]
            if self.kind == 0 or self.kind == 1:
                self.keys = [[k[0] * 1.0] + k[1:] for k in self.keys]
        else:
            self.keys = [[raw[i * per + j] * scale for j in range(per)] for i in range(n)]
        for k in self.keys:
            k[0] += offset

    def eval(self, frame):
        fr = self.frames
        if frame <= fr[0]:
            return self.keys[0][0]
        if frame >= fr[-1]:
            k = self.keys[-1]
            if self.kind == 0:  # the last segment's end
                return sum(k) if len(fr) > 1 and frame > fr[-1] else k[0]
            return k[0] + (k[1] if self.kind == 1 and frame > fr[-1] else 0.0)
        i = 0
        while i + 1 < len(fr) and fr[i + 1] <= frame:
            i += 1
        t = (frame - fr[i]) / (fr[i + 1] - fr[i]) if i + 1 < len(fr) else 0.0
        k = self.keys[i]
        if self.kind == 0:
            return k[0] + k[1] * t + k[2] * t * t + k[3] * t * t * t
        if self.kind == 1:
            return k[0] + k[1] * t
        return k[0]


def euler_xyz_matrix(r):
    """Rotation matrix for NintendoWare Euler XYZ angles (radians): R = Rz * Ry * Rx."""
    x, y, z = r[:3]
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
            [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
            [-sy, sx * cy, cx * cy]]
