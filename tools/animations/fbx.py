"""Minimal binary FBX reader: the bone hierarchy and rest transforms of an armature."""
import struct
import zlib


def _read_node(d, o, wide):
    if wide:
        end, nprops, plen = struct.unpack_from('<QQQ', d, o)
        o += 24
    else:
        end, nprops, plen = struct.unpack_from('<III', d, o)
        o += 12
    nlen = d[o]
    name = d[o + 1:o + 1 + nlen].decode('ascii', 'replace')
    o += 1 + nlen
    if end == 0:
        return None, o
    props = []
    for _ in range(nprops):
        t = chr(d[o]); o += 1
        if t in 'YCIFDL':
            fmt, size = {'Y': ('<h', 2), 'C': ('<?', 1), 'I': ('<i', 4), 'F': ('<f', 4),
                         'D': ('<d', 8), 'L': ('<q', 8)}[t]
            props.append(struct.unpack_from(fmt, d, o)[0]); o += size
        elif t in 'SR':
            n, = struct.unpack_from('<I', d, o); o += 4
            v = d[o:o + n]; o += n
            props.append(v.decode('utf8', 'replace') if t == 'S' else v)
        else:
            n, enc, clen = struct.unpack_from('<III', d, o); o += 12
            raw = d[o:o + clen]; o += clen
            if enc:
                raw = zlib.decompress(raw)
            fmt = {'f': 'f', 'd': 'd', 'l': 'q', 'i': 'i', 'b': '?'}[t]
            props.append(list(struct.unpack('<%d%s' % (n, fmt), raw)))
    children = []
    sentinel = 25 if wide else 13
    while o < end:
        if end - o == sentinel and not any(d[o:end]):
            o = end
            break
        child, o = _read_node(d, o, wide)
        if child is None:
            break
        children.append(child)
    return {'name': name, 'props': props, 'children': children}, end


def read(path):
    d = open(path, 'rb').read()
    assert d.startswith(b'Kaydara FBX Binary')
    version, = struct.unpack_from('<I', d, 23)
    wide = version >= 7500
    o, top = 27, []
    while o < len(d):
        node, o = _read_node(d, o, wide)
        if node is None:
            break
        top.append(node)
    return {n['name']: n for n in top}


def bones(path):
    """{name: {'parent': name or None, 't': (x,y,z), 'r': (x,y,z) degrees, 's': (x,y,z), 'pre': (x,y,z)}}"""
    root = read(path)
    models = {}
    for n in root['Objects']['children']:
        if n['name'] != 'Model':
            continue
        uid, name, kind = n['props'][0], n['props'][1].split('\x00')[0], n['props'][2]
        rec = {'kind': kind, 'name': name, 't': (0.0, 0.0, 0.0), 'r': (0.0, 0.0, 0.0),
               's': (1.0, 1.0, 1.0), 'pre': (0.0, 0.0, 0.0), 'parent': None}
        for c in n['children']:
            if c['name'] != 'Properties70':
                continue
            for p in c['children']:
                key = p['props'][0]
                vals = tuple(p['props'][4:7])
                if key == 'Lcl Translation': rec['t'] = vals
                elif key == 'Lcl Rotation': rec['r'] = vals
                elif key == 'Lcl Scaling': rec['s'] = vals
                elif key == 'PreRotation': rec['pre'] = vals
        models[uid] = rec
    for c in root['Connections']['children']:
        if c['props'][0] == 'OO' and c['props'][1] in models and c['props'][2] in models:
            models[c['props'][1]]['parent'] = models[c['props'][2]]['name']
    return {m['name']: m for m in models.values() if m['kind'] == 'LimbNode'}
