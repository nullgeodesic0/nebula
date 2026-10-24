"""Which host addresses a rollback snapshot depends on (hot join, docs/HOT_JOIN.md).

    py -3 tools/agent/snapshot_scan.py <file.scan> [--detail N]

Reads a GALAXY_SNAPSHOT_SCAN dump (native_runtime.cpp rollback_write_scan): every guest thread's native stack as
the snapshot holds it, this process's module list, named anchor objects, the VirtualQuery map and the busy heap
blocks. Every aligned 8-byte word of every stack is classified by where it points: a stack of the pool (itself or
another), a module image (code or data), guest memory (the fixed flat view, or a host view of a section), a named
anchor object, another heap block, other private memory, or nowhere (not an address). The words that point
somewhere a second process would not have the same thing are what a snapshot file must not carry as they are.
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess
import sys

MEM_COMMIT, MEM_RESERVE = 0x1000, 0x2000
MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE = 0x20000, 0x40000, 0x1000000


def read_scan(path):
    data = open(path, 'rb').read()
    if data[:8] != b'NBSCAN1\0':
        raise SystemExit('%s: not a snapshot scan' % path)
    at = 8

    def take(fmt):
        nonlocal at
        values = struct.unpack_from(fmt, data, at)
        at += struct.calcsize(fmt)
        return values

    stacks = []
    for _ in range(take('<I')[0]):
        slot, sp, size = take('<iQQ')
        stacks.append((slot, sp, data[at:at + size]))
        at += size
    labels = []
    for _ in range(take('<I')[0]):
        lo, hi, n = take('<QQI')
        labels.append((lo, hi, data[at:at + n].decode()))
        at += n
    regions = [take('<QQQIII') for _ in range(take('<I')[0])]
    blocks = [take('<QQ') for _ in range(take('<I')[0])]
    frames = {}   # slot: [(rip, rsp, [where the caller's rbx rbp rsi rdi r12-r15 were saved])]
    for slot, _, _ in stacks:
        if slot < 0 or at >= len(data):
            continue
        frames[slot] = []
        for _ in range(take('<I')[0]):
            words = take('<10Q')
            frames[slot].append((words[0], words[1], [w for w in words[2:] if w]))
    return stacks, labels, regions, blocks, frames


class Classifier:
    def __init__(self, stacks, labels, regions, blocks):
        self.labels = sorted(labels)
        self.regions = sorted(regions)
        self.region_starts = [r[0] for r in self.regions]
        self.blocks = sorted(blocks)
        self.block_starts = [b[0] for b in self.blocks]
        # A stack's whole allocation (its guard page to its top), from the region holding its saved sp.
        self.stack_allocations = {}
        for slot, sp, data in stacks:
            if slot < 0:
                continue
            region = self.region(sp)
            if region:
                self.stack_allocations[slot] = (region[2], sp + len(data))

    def region(self, value):
        i = bisect.bisect_right(self.region_starts, value) - 1
        if i >= 0:
            base, size = self.regions[i][0], self.regions[i][1]
            if base <= value < base + size:
                return self.regions[i]
        return None

    def label(self, value, prefix):
        for lo, hi, name in self.labels:
            if name.startswith(prefix) and lo <= value < hi:
                return name, value - lo
        return None

    def block(self, value):
        i = bisect.bisect_right(self.block_starts, value) - 1
        if i >= 0 and self.blocks[i][0] <= value < self.blocks[i][0] + max(self.blocks[i][1], 1):
            return self.blocks[i]
        return None

    def classify(self, value):
        """(class, detail) for a word: where it points in this process."""
        for slot, (lo, hi) in self.stack_allocations.items():
            if lo <= value <= hi:
                return 'stack', 'slot %d' % slot
        hit = self.label(value, 'guest:')
        if hit:
            return 'guest', hit[0]
        hit = self.label(value, 'module:')
        if hit:
            name, offset = hit
            region = self.region(value)
            executable = region is not None and (region[5] & 0xF0) != 0
            return ('code' if executable else 'image-data'), '%s+0x%x' % (name[7:], offset)
        hit = self.label(value, 'anchor:')
        if hit:
            return 'anchor', '%s+0x%x' % (hit[0][7:], hit[1])
        block = self.block(value)
        if block:
            return 'heap', 'block 0x%x (%d bytes) +0x%x' % (block[0], block[1], value - block[0])
        region = self.region(value)
        if region and region[3] == MEM_COMMIT:
            kind = {MEM_PRIVATE: 'private', MEM_MAPPED: 'mapped', MEM_IMAGE: 'image'}.get(region[4], 'other')
            return kind, 'allocation 0x%x region 0x%x+0x%x' % (region[2], region[0], value - region[0])
        if region:
            return 'reserved', 'allocation 0x%x' % region[2]
        return None, None


def image_base(path):
    data = open(path, 'rb').read(4096)
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<Q', data, pe + 24 + 24)[0]


class Symbols:
    """Names for code addresses in the runtime and the game module (llvm-symbolizer with the run folder's PDB and
    the module's exports), looked up in one batch."""

    def __init__(self, c, run_folder, addresses):
        self.names = {}
        symbolizer = r'C:\Program Files\LLVM\bin\llvm-symbolizer.exe'
        for module in ('NebulaRuntime.exe', 'RMGE01_game.dll'):
            path = os.path.join(run_folder, module)
            hits = []
            for value in sorted(set(addresses)):
                kind, detail = c.classify(value)
                if kind == 'code' and detail.startswith(module + '+'):
                    hits.append((value, int(detail.split('+0x')[1], 16)))
            if not hits or not os.path.exists(path):
                continue
            base = image_base(path)
            text = '\n'.join('0x%x' % (base + rva) for _, rva in hits)
            out = subprocess.run([symbolizer, '--obj=' + path, '--functions=short', '--no-inlines'], input=text,
                                 capture_output=True, text=True).stdout.split('\n\n')
            for (value, rva), result in zip(hits, out):
                name = result.strip().split('\n')[0]
                self.names[value] = name if name and name != '??' else '%s+0x%x' % (module, rva)

    def __call__(self, c, value):
        if value in self.names:
            return self.names[value]
        kind, detail = c.classify(value)
        return detail if kind else '0x%x' % value


def frame_of(frames, address):
    """Index of the frame a stack address belongs to: frame i spans [rsp_i, rsp_i+1). -1: coop_context_switch's
    saved state below the first frame."""
    owner = -1
    for i, (_, rsp, _) in enumerate(frames):
        if address >= rsp:
            owner = i
    return owner


def analyse(c, stacks, frames):
    """Rows (slot, address, value, kind, detail, frame index, saved-register slot?) for every classified word."""
    rows = []
    for slot, sp, data in stacks:
        stack_frames = frames.get(slot, [])
        saved = {a for _, _, slots in stack_frames for a in slots}
        for offset in range(0, len(data) - 7, 8):
            value = struct.unpack_from('<Q', data, offset)[0]
            kind, detail = c.classify(value)
            if kind is None:
                continue
            address = sp + offset
            rows.append((slot, address, value, kind, detail, frame_of(stack_frames, address), address in saved))
    return rows


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('scan')
    parser.add_argument('--detail', type=int, default=40, help='words to list per class that need care')
    parser.add_argument('--walk', type=int, default=None, help="list one stack slot's frames and words, named")
    parser.add_argument('--run', default=r'C:\nb\run-hj', help='the run folder the scan was made with (symbols)')
    args = parser.parse_args(argv)
    stacks, labels, regions, blocks, frames = read_scan(args.scan)
    c = Classifier(stacks, labels, regions, blocks)
    rows = analyse(c, stacks, frames)
    symbols = Symbols(c, args.run, [r[2] for r in rows] + [f[0] for fs in frames.values() for f in fs])

    if args.walk is not None:
        stack_frames = frames.get(args.walk, [])
        for i, (rip, rsp, _) in enumerate(stack_frames):
            print('frame %3d rsp 0x%016x %s' % (i, rsp, symbols(c, rip)))
        print()
        for slot, address, value, kind, detail, frame, reg in rows:
            if slot == args.walk:
                name = symbols(c, stack_frames[frame][0]) if frame >= 0 else 'coop_context_switch'
                print('  0x%016x 0x%016x %-10s %-3s f%-3d %-40s %s' % (address, value, kind, 'reg' if reg else '',
                                                                       frame, symbols(c, value)[:60], name[:50]))
        return 0

    print('%d stacks, %d labels, %d regions, %d heap blocks' % (len(stacks), len(labels), len(regions), len(blocks)))
    for slot, sp, data in stacks:
        counts = collections.Counter(r[3] for r in rows if r[0] == slot)
        depth = len(frames.get(slot, []))
        print('slot %3d sp 0x%016x %8d bytes %4d frames: %s' % (
            slot, sp, len(data), depth, ', '.join('%s %d' % kv for kv in sorted(counts.items()))))
    print('\nall stacks: %s' % ', '.join('%s %d' % kv for kv in sorted(collections.Counter(r[3] for r in rows).items())))
    print('saved-register slots: %s' % ', '.join(
        '%s %d' % kv for kv in sorted(collections.Counter(r[3] for r in rows if r[6]).items())))

    targets = collections.defaultdict(collections.Counter)
    for r in rows:
        kind, detail = r[3], r[4]
        key = detail.split('+')[0] if kind in ('code', 'image-data', 'anchor', 'guest') else detail.split(' +')[0]
        targets[kind][key] += 1
    for kind in sorted(targets):
        print('\n%s targets:' % kind)
        for key, n in targets[kind].most_common(20):
            print('  %6d  %s' % (n, key))

    # Words a second process would not have as they are, with the function whose frame holds them.
    for kind in ('heap', 'private', 'mapped', 'reserved', 'image-data', 'code'):
        picked = [r for r in rows if r[3] == kind and not (kind in ('code', 'image-data') and (
            r[4].startswith('NebulaRuntime.exe') or r[4].startswith('RMGE01_')))]
        if not picked:
            continue
        print('\n%s words (%d, %d in saved-register slots; first %d):' % (
            kind, len(picked), sum(1 for r in picked if r[6]), args.detail))
        for slot, address, value, _, detail, frame, reg in picked[:args.detail]:
            name = symbols(c, frames[slot][frame][0]) if frame >= 0 and slot in frames else '-'
            print('  slot %2d 0x%016x = 0x%016x %-3s %-50s in %s' % (slot, address, value, 'reg' if reg else '',
                                                                   detail[:50], name[:60]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
