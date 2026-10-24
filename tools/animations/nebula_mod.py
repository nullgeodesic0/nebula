"""Packs built custom content into one Nebula mod folder (runtime/include/galaxy/mods.h): the archives as
files/ in the disc's layout and the voice clips as voice/. Nebula's mods folder then loads them like any other
mod (Settings > Mods), so the disc needs no patched file table.

    py -3 tools/animations/nebula_mod.py --out "%LOCALAPPDATA%\\Nebula\\mods\\Odyssey Mario" ^
        <content>\\mods\\model <content>\\mods\\animations <content>\\mods\\voice

Each input is a folder one of the build scripts wrote: a disc mod (manifest.txt, from build_animation_mod.py,
build_model_mod.py, build_odyssey_model_mod.py or build_odyssey_luigi_mod.py) or the voice clips (voices.txt,
from build_voice_mod.py). A disc mod's manifest names each archive's disc path on its file-table line and the
built file at the offset that line points it to. Later inputs win where two give the same file. An animation mod's
luigi_swap.txt (Luigi's animation swap table) goes to the mod's top folder, where the game reads it. Everything is the
player's own build and stays on their PC: never commit or share the result.
"""
import argparse
import os
import re
import shutil
import sys

# "0x63F710 12 fst_Mario.bin  # ObjectData/Mario.arc: 0xFF711F40 -> 0x4000000"
FST_LINE = re.compile(r'^0x[0-9A-Fa-f]+\s+12\s+\S+\s+#\s*(\S+):\s*0x[0-9A-Fa-f]+\s*->\s*(0x[0-9A-Fa-f]+)')
# "0x4000000 460832 Mario.arc.yaz0"
DATA_LINE = re.compile(r'^(0x[0-9A-Fa-f]+)\s+(\d+)\s+(\S+)')


def disc_files(folder):
    """The archives of a built disc mod: {disc path: built file}."""
    targets = {}
    data = {}
    with open(os.path.join(folder, 'manifest.txt'), encoding='utf-8') as manifest:
        for line in manifest:
            line = line.strip()
            fst = FST_LINE.match(line)
            if fst:
                targets[int(fst.group(2), 16)] = fst.group(1)
                continue
            item = DATA_LINE.match(line)
            if item:
                data[int(item.group(1), 16)] = os.path.join(folder, item.group(3))
    files = {}
    for offset, path in targets.items():
        if offset not in data:
            sys.exit('%s: no data for %s at 0x%X' % (folder, path, offset))
        files[path] = data[offset]
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True, help='the mod folder to write (inside Nebula\'s mods folder)')
    parser.add_argument('inputs', nargs='+', help='built folders: disc mods (manifest.txt) or voices (voices.txt)')
    args = parser.parse_args()

    files = {}
    voice = None
    luigi_swap = None
    for folder in args.inputs:
        if os.path.isfile(os.path.join(folder, 'manifest.txt')):
            files.update(disc_files(folder))
            if os.path.isfile(os.path.join(folder, 'luigi_swap.txt')):
                luigi_swap = os.path.join(folder, 'luigi_swap.txt')
        elif os.path.isfile(os.path.join(folder, 'voices.txt')):
            voice = folder
        else:
            sys.exit('%s: neither manifest.txt nor voices.txt' % folder)

    os.makedirs(args.out, exist_ok=True)
    for path, source in sorted(files.items()):
        target = os.path.join(args.out, 'files', *path.split('/'))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(source, target)
        print('files/%s  (%d bytes)' % (path, os.path.getsize(target)))
    if voice is not None:
        target = os.path.join(args.out, 'voice')
        if os.path.isdir(target):
            shutil.rmtree(target)
        shutil.copytree(voice, target)
        print('voice/  (%d clips)' % sum(1 for name in os.listdir(target) if name.lower().endswith('.wav')))
    if luigi_swap is not None:
        shutil.copyfile(luigi_swap, os.path.join(args.out, 'luigi_swap.txt'))
        print('luigi_swap.txt')
    with open(os.path.join(args.out, 'readme.txt'), 'w', newline='\n') as note:
        note.write('Built on this PC by tools/animations from the player\'s own files. Not for sharing.\n')


if __name__ == '__main__':
    main()
