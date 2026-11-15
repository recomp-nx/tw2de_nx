#!/usr/bin/env python3
"""tw2de (2026-10-25): the profiling build's runs (pgo/run_*.fxpg in the project, copied from /switch/tw2de_nx/pgo on the SD
card) unpacked into the .gcda files GCC's -fprofile-use reads.

    python tools/pgo_unpack.py <folder with run_*.fxpg> <output folder> [--gcov-tool <aarch64-none-elf-gcov-tool>]

A .fxpg file (app/src/pgo_switch.cpp): "FXPG", version 2, object count, then per object its profile file name, as the
profiling build named it (/switch/tw2de_nx/pgo/<object's path inside the build folder, '/' as '#'>.gcda), and the
.gcda image libgcov would have written for it; u32s little-endian. Each file holds one whole run. One run: its images
are written as they are. Several: each run is unpacked into a folder of its own and the folders merged with
gcov-tool merge (the counts added). build_switch.py --pgo use runs this itself.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pgo_functions  # noqa: E402

PROFILE_DIR = '/switch/tw2de_nx/pgo/'


def read_run(path):
    """A run's objects, with its summary. tw2de (2026-10-26): with its prepass part beside it
    (run_<start>.prepass.fxpg: the counts as the shader pre-pass left them, app/src/pgo_switch.h), that part is taken
    out of the run and added back scaled down so its largest count is no larger than the rest's: the compiler keeps
    its branch layout, the game's code keeps the hot / cold scale it would have alone."""
    objects = load_run(path)
    part = path[:-len('.fxpg')] + '.prepass.fxpg'
    if os.path.isfile(part):
        objects = without_prepass(objects, load_run(part), os.path.basename(part))
    objects, top = with_summary(objects)
    print('pgo_unpack: %s: largest count %d' % (os.path.basename(path), top))
    return objects


def load_run(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'FXPG':
        raise ValueError('%s: not a .fxpg file' % path)
    version, count = struct.unpack_from('<II', data, 4)
    # tw2de (2026-10-26): version 1: a profiling build from before functions were numbered inside their file
    # (app/CMakeLists.txt, TW2DE_PGO_ID): its ids fit no build of today.
    if version == 1:
        raise ValueError('%s: made by an older profiling build (its functions are named in a way this build no longer '
                         'reads): make new runs with the current one (pgo_generate_switch_batched.bat), or take it out of pgo/'
                         % os.path.basename(path))
    if version != 2:
        raise ValueError('%s: version %d (this tool reads 2)' % (path, version))
    pos = 12
    objects = []
    for _ in range(count):
        (n,) = struct.unpack_from('<I', data, pos)
        name = data[pos + 4:pos + 4 + n].decode('utf-8', 'replace')
        pos += 4 + n
        (m,) = struct.unpack_from('<I', data, pos)
        image = data[pos + 4:pos + 4 + m]
        if len(image) != m:
            raise ValueError('%s: cut short' % path)
        pos += 4 + m
        objects.append((pgo_functions.renamed_key(name), image))  # tw2de (2026-10-29): the old name's profiles
    return objects


def arcs(image):
    """The image's arc counter records: (payload offset, count of u64s) for the stored ones, (None, n) for a record of
    zeros (negative length)."""
    out = []
    pos = 16
    while pos + 8 <= len(image):
        tag, length = struct.unpack_from('<Ii', image, pos)
        pos += 8
        if tag == TAG_ARCS:
            out.append((pos if length >= 0 else None, abs(length) // 8))
        pos += max(length, 0)
    return out


def without_prepass(objects, prepass, part_name):
    """The run's counts minus its prepass part, plus that part divided by k (k: its largest count over the rest's
    largest, at least 1; a count that was not 0 stays at least 1). The same build wrote both: the same objects, the
    same records."""
    by_name = dict(prepass)
    if len(by_name) != len(objects) or any(n not in by_name for n, _ in objects):
        print('pgo_unpack: %s: not the same objects as its run: left out' % part_name)
        return objects
    top_game = top_part = 0
    pairs = []
    for name, image in objects:
        mine, theirs = arcs(image), arcs(by_name[name])
        if [n for _, n in mine] != [n for _, n in theirs]:
            print('pgo_unpack: %s: %s differs from its run: left out' % (part_name, name))
            return objects
        pairs.append((name, image, by_name[name], mine, theirs))
        for (a, n), (b, _) in zip(mine, theirs):
            for i in range(n):
                total = struct.unpack_from('<Q', image, a + 8 * i)[0] if a is not None else 0
                early = struct.unpack_from('<Q', by_name[name], b + 8 * i)[0] if b is not None else 0
                top_game = max(top_game, total - min(early, total))
                top_part = max(top_part, early)
    k = max(1, -(-top_part // max(top_game, 1))) if top_game else 1
    out = []
    for name, image, part, mine, theirs in pairs:
        image = bytearray(image)
        for (a, n), (b, _) in zip(mine, theirs):
            if a is None:
                continue  # zeros in the whole run: zeros in its part too
            for i in range(n):
                total = struct.unpack_from('<Q', image, a + 8 * i)[0]
                early = struct.unpack_from('<Q', part, b + 8 * i)[0] if b is not None else 0
                early = min(early, total)
                scaled = early // k if early // k or not early else 1
                struct.pack_into('<Q', image, a + 8 * i, total - early + scaled)
        out.append((name, bytes(image)))
    print('pgo_unpack: %s: its counts divided by %d (largest %d, the rest\'s %d)' % (part_name, k, top_part,
                                                                                    top_game))
    return out


TAG_SUMMARY = 0xA1000000
TAG_ARCS = 0x01A10000


def records(image):
    """(offset of the record's payload, tag, payload length) of a .gcda image (GCC 12+: a 16-byte header -- magic,
    version, stamp, checksum -- then records of tag, length in bytes, payload; a negative length is a counter record
    of that many zero bytes, not stored)."""
    pos = 16
    while pos + 8 <= len(image):
        tag, length = struct.unpack_from('<Ii', image, pos)
        pos += 8
        n = max(length, 0)
        yield pos, tag, n
        pos += n


def run_max(objects):
    """The largest arc count of the run, over every object: libgcov's sum_max of a run."""
    top = 0
    for _, image in objects:
        for pos, tag, n in records(image):
            if tag == TAG_ARCS:
                for i in range(pos, pos + n - 7, 8):
                    lo, hi = struct.unpack_from('<II', image, i)
                    top = max(top, lo | (hi << 32))
    return top


def with_summary(objects):
    """__gcov_info_to_gcda writes no object summary (libgcov's own dump adds it): each image gets the one libgcov writes
    for one run, right after the header (runs=1, sum_max = the run's largest arc count). GCC's -fprofile-use reads it
    (no summary: no profile_info, the counts unused for hot/cold); gcov-tool merge adds them up across runs."""
    top = min(run_max(objects), 0xFFFFFFFF)
    out = []
    for name, image in objects:
        image = bytearray(image)
        summary = [pos for pos, tag, n in records(image) if tag == TAG_SUMMARY and n >= 8]
        if summary:
            struct.pack_into('<II', image, summary[0], 1, top)
        elif len(image) >= 16:
            image[16:16] = struct.pack('<IIII', TAG_SUMMARY, 8, 1, top)
        out.append((name, bytes(image)))
    return out, top


# tw2de (2026-10-26): where the project is now, as GCC writes a path into a profile name ('/' and '\\' as '#', a
# drive's ':' as '_'). A few objects (61 of 44,826: SDK sources compiled into a target of another folder) are named
# by CMake after their source's whole path, so their names hold the folder the profiling build was made in
# ("rexcore.dir#D_#projects#tw2de#sdk#src#..."); that part is replaced by this project's folder, so a
# profile made in another folder (or on another PC) still finds them. Every other name is relative to the build folder.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT_IN_NAME = re.compile(r'(\.dir#)(.+?)(#(?:sdk|app)#)')


def mangled_root():
    path = ROOT.replace('\\', '/')
    if re.match(r'^[A-Za-z]:', path):
        path = path[0] + '_' + path[2:]
    return path.strip('/').replace('/', '#')


def relative_name(name):
    name = name.replace('\\', '/')
    prefix = PROFILE_DIR
    name = name[len(prefix):] if name.startswith(prefix) else name.rsplit('/', 1)[-1]
    name = ROOT_IN_NAME.sub(lambda m: m.group(1) + mangled_root() + m.group(3), name, count=1)
    parts = [p for p in name.split('/') if p not in ('', '.', '..')]
    return os.path.join(*parts)


def write_run(objects, out, ids=None):
    os.makedirs(out, exist_ok=True)
    for name, image in objects:
        if not image:
            continue
        if ids is not None:  # tw2de (2026-10-26): the release's ids (tools/pgo_functions.py)
            key = name.replace('\\', '/')
            key = key[len(PROFILE_DIR):] if key.startswith(PROFILE_DIR) else key.rsplit('/', 1)[-1]
            key = key[:-len('.gcda')] if key.endswith('.gcda') else key
            if key in ids:
                image = pgo_functions.remap(image, ids[key])
        path = os.path.join(out, relative_name(name))
        os.makedirs(os.path.dirname(path) or out, exist_ok=True)
        with open(path, 'wb') as f:
            f.write(image)


def scaled(objects, k):
    """The arc counts divided by k (one that was not 0 stays at least 1)."""
    if k <= 1:
        return objects
    out = []
    for name, image in objects:
        image = bytearray(image)
        for pos, n in arcs(image):
            if pos is None:
                continue
            for i in range(n):
                v = struct.unpack_from('<Q', image, pos + 8 * i)[0]
                struct.pack_into('<Q', image, pos + 8 * i, v // k if v // k or not v else 1)
        out.append((name, bytes(image)))
    return out


FUNCTIONS = 'functions.txt.gz'  # tw2de (2026-10-26): the profiling build's functions (tools/pgo_functions.py)


def release_ids(src, new_root):
    """{object: {profile's id: [the release's ids]}} from the profiling build's list in the runs' folder."""
    path = os.path.join(src, FUNCTIONS)
    if not os.path.isfile(path):
        raise ValueError('no %s in %s: the profiling build writes it (pgo_generate_switch_batched.bat: build it again, the '
                         'same sources, no new runs needed), copy it beside the runs' % (FUNCTIONS, src))
    objects, old_root = pgo_functions.load(path)
    ids = {key: pgo_functions.new_ids(fns, key, old_root, new_root) for key, fns in objects.items()}
    print('pgo_unpack: %s: %d objects, profiling build in %s%s' % (FUNCTIONS, len(ids), old_root or '?',
                                                                  '' if not new_root or old_root.lower() ==
                                                                  new_root.lower() else ', this one in ' + new_root))
    return ids


def unpack(src, out, gcov_tool=None, new_root=''):
    names = sorted(os.listdir(src))
    runs = [os.path.join(src, n) for n in names if n.endswith('.fxpg') and not n.endswith('.prepass.fxpg')]
    # tw2de (2026-10-26): a prepass part with no run of its own (a run closed right after the shader pre-pass,
    # renamed run_<start>.prepass.fxpg; or one whose run is missing): counts of the compiler alone, scaled down to a
    # sixteenth of the largest count of the other runs, so they keep the game's hot / cold scale.
    lone = [os.path.join(src, n) for n in names if n.endswith('.prepass.fxpg')
            and n[:-len('.prepass.fxpg')] + '.fxpg' not in names]
    if not runs and not lone:
        raise ValueError('no run_*.fxpg in %s' % src)
    ids = release_ids(src, new_root or ROOT.replace('\\', '/'))
    if os.path.isdir(out):
        shutil.rmtree(out)
    sets = [(os.path.basename(r), read_run(r)) for r in runs]
    top_game = max([run_max(objects) for _, objects in sets] or [0])
    for part in lone:
        objects = load_run(part)
        top = run_max(objects)
        k = max(1, -(-top * 16 // top_game)) if top_game else 1
        objects, _ = with_summary(scaled(objects, k))
        print('pgo_unpack: %s: a pre-pass alone, its counts divided by %d (largest %d, the runs\' %d)' %
              (os.path.basename(part), k, top, top_game))
        sets.append((os.path.basename(part), objects))
    if len(sets) == 1:
        write_run(sets[0][1], out, ids)
        print('pgo_unpack: %s: %d objects -> %s' % (sets[0][0], len(sets[0][1]), out))
        return
    if not gcov_tool:
        raise ValueError('%d runs to merge: gcov-tool needed (--gcov-tool)' % len(sets))
    with tempfile.TemporaryDirectory(prefix='pgo_unpack_') as tmp:
        merged = None
        for i, (name, objects) in enumerate(sets):
            folder = os.path.join(tmp, 'run%d' % i)
            write_run(objects, folder, ids)
            print('pgo_unpack: %s: %d objects' % (name, len(objects)))
            if merged is None:
                merged = folder
                continue
            target = os.path.join(tmp, 'merged%d' % i)
            r = subprocess.run([gcov_tool, 'merge', '-o', target, merged, folder])
            if r.returncode != 0:
                raise ValueError('gcov-tool merge failed (exit %d)' % r.returncode)
            merged = target
        shutil.copytree(merged, out)
    print('pgo_unpack: %d runs merged -> %s' % (len(sets), out))


def main():
    args = sys.argv[1:]
    tool = None
    if '--gcov-tool' in args:
        i = args.index('--gcov-tool')
        tool = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        print(__doc__)
        return 2
    try:
        unpack(args[0], args[1], tool)
    except (ValueError, OSError) as e:
        print('pgo_unpack: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
