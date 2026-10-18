"""Yaz0, RARC archives and the disc file system table of a Nebula game.pak."""
import struct


def yaz0_decode(d):
    if d[:4] != b'Yaz0':
        return bytes(d)
    n = struct.unpack_from('>I', d, 4)[0]
    out = bytearray()
    p = 16
    while len(out) < n:
        code = d[p]; p += 1
        for i in range(8):
            if len(out) >= n:
                break
            if code & (0x80 >> i):
                out.append(d[p]); p += 1
            else:
                b1, b2 = d[p], d[p + 1]; p += 2
                dist = ((b1 & 15) << 8 | b2) + 1
                ln = b1 >> 4
                if ln == 0:
                    ln = d[p] + 18; p += 1
                else:
                    ln += 2
                s = len(out) - dist
                for k in range(ln):
                    out.append(out[s + k])
    return bytes(out)


def yaz0_encode(src, chain=96):
    """Greedy LZ with one step of lazy matching over 3-byte hash chains."""
    n = len(src)
    out = bytearray(b'Yaz0' + struct.pack('>I', n) + b'\0' * 8)
    head = {}
    prev = [0] * n

    def insert(i):
        if i + 2 < n:
            k = src[i:i + 3]
            prev[i] = head.get(k, -1)
            head[k] = i

    def find(i):
        if i + 2 >= n:
            return 0, 0
        j = head.get(src[i:i + 3], -1)
        best = bd = 0
        c = chain
        maxl = min(273, n - i)
        while j >= 0 and i - j <= 4096 and c > 0:
            if best < maxl and src[j + best] == src[i + best]:
                l = 3
                while l < maxl and src[j + l] == src[i + l]:
                    l += 1
                if l > best:
                    best, bd = l, i - j
                if l == maxl:
                    break
            j = prev[j]
            c -= 1
        return best, bd

    i = 0
    while i < n:
        cpos = len(out)
        out.append(0)
        code = 0
        for bit in range(8):
            if i >= n:
                break
            l, dd = find(i)
            if l >= 3 and i + 1 < n and find(i + 1)[0] > l + 1:
                l = 0
            if l >= 3:
                d1 = dd - 1
                if l >= 18:
                    out += bytes([d1 >> 8, d1 & 0xFF, l - 18])
                else:
                    out += bytes([((l - 2) << 4) | (d1 >> 8), d1 & 0xFF])
                for k in range(l):
                    insert(i + k)
                i += l
            else:
                code |= 0x80 >> bit
                out.append(src[i])
                insert(i)
                i += 1
        out[cpos] = code
    return bytes(out)


def _rarc_nodes(d):
    base = 0x20
    nd, dno, nf, fno, _, sso = struct.unpack_from('>6I', d, 0x20)

    def name(off):
        s = base + sso + off
        return d[s:d.index(b'\0', s)].decode('shift-jis', 'replace')

    files = []
    for j in range(nf):
        o = base + fno + j * 20
        fid, _, typ, nameoff, dptr, dsz = struct.unpack_from('>HHHHII', d, o)
        nm = name(nameoff)
        if fid == 0xFFFF or (typ >> 8) & 0x02 or nm in ('.', '..'):
            continue
        files.append((o, nm, dptr, dsz))
    return base + struct.unpack_from('>I', d, 0x0C)[0], files


def rarc_files(d):
    """name -> bytes for every file of a (decompressed) RARC; names are unique in the archives used here."""
    start, files = _rarc_nodes(d)
    return {nm: bytes(d[start + p:start + p + s]) for _, nm, p, s in files}


def rarc_replace(d, replace):
    """The RARC with the named files' contents replaced; directory and node tables are kept."""
    d = bytearray(d)
    start, files = _rarc_nodes(d)
    missing = set(replace) - {f[1] for f in files}
    if missing:
        raise KeyError('not in the archive: %s' % ', '.join(sorted(missing)))
    out = bytearray()
    placed = {}
    for o, nm, dptr, dsz in sorted(files, key=lambda f: f[2]):
        if dptr not in placed:
            data = replace.get(nm, bytes(d[start + dptr:start + dptr + dsz]))
            placed[dptr] = (len(out), len(data))
            out += data
            out += b'\0' * ((-len(out)) % 32)
        struct.pack_into('>II', d, o + 8, *placed[dptr])
    res = bytearray(bytes(d[:start]) + bytes(out))
    struct.pack_into('>I', res, 4, len(res))
    struct.pack_into('>II', res, 0x10, len(out), len(out))
    return bytes(res)


class Pak:
    """A Nebula game.pak (the game's data partition, Wii offsets in words)."""

    def __init__(self, path):
        import mmap
        self.f = open(path, 'rb')
        self.m = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        self.fst_offset = struct.unpack_from('>I', self.m, 0x424)[0] << 2
        fo = self.fst_offset
        n = struct.unpack_from('>I', self.m, fo + 8)[0]
        names = fo + n * 12
        self.files = {}

        def nm(off):
            s = names + off
            return self.m[s:self.m.find(b'\0', s)].decode('shift-jis', 'replace')

        def walk(i, end, path):
            while i < end:
                a, off, ln = struct.unpack_from('>III', self.m, fo + i * 12)
                if a >> 24:
                    walk(i + 1, ln, path + nm(a & 0xFFFFFF) + '/')
                    i = ln
                else:
                    self.files[path + nm(a & 0xFFFFFF)] = (off << 2, ln, fo + i * 12)
                    i += 1

        walk(1, n, '')

    def read(self, name):
        off, ln, _ = self.files[name]
        return bytes(self.m[off:off + ln])

    def filler_area(self, size, align=0x100000, start=0):
        """An offset at or after start in the unused area between the FST and the first file whose
        size bytes hold one repeated filler value."""
        first = min(off for off, _, _ in self.files.values())
        start = (max(start, self.fst_offset + 0x400000) + align - 1) // align * align
        for off in range(start, first - size, align):
            chunk = self.m[off:off + size]
            if chunk.count(chunk[:1]) == len(chunk):
                return off
        raise RuntimeError('no unused area of %d bytes in game.pak' % size)
