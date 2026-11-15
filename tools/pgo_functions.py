#!/usr/bin/env python3
"""tw2de (2026-10-26): the profiling build's functions (pgo/functions.txt), so a profile fits a release built in
another folder.

    python tools/pgo_functions.py <profiling build folder> <out file>     (build_switch.py --pgo generate runs it)

Why: GCC finds a function in a profile by an id. For an exported function the id is a hash of its name alone; for one
that is not visible outside its file (static, in an anonymous namespace, a lambda, a file's static initializers) the
hash also holds its source's whole path and its object's name (gcc/coverage.cc, coverage_compute_profile_id), so a
profile made in one folder is "not found in profile data" for all of those in another. GCC's own way out
(--param=profile-func-internal-id=1, numbered functions) crashes it at -fprofile-use on any file with a C++ thunk
(profile.cc, read_thunk_profile: no function to read the number from), so the release keeps GCC's ids, and
tools/pgo_unpack.py gives every function of the profile the id the release will look for, from this list.

The list: the profiling build is compiled with -ftest-coverage (a .gcno beside each object: each function's id, name
and source path); per object, its profile's name, its folder and its functions (id, exported or not, name, source).
"""
import gzip
import os
import re
import struct
import sys

TAG_FUNCTION = 0x01000000


# GCC's crc32_byte / crc32_string (gcc/tree.cc: crc32_unsigned_n with 0x04c11db7, most significant bit first) and
# coverage_checksum_string (gcc/coverage.cc), as GCC computes a function's id.
def _table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) if c & 0x80000000 else (c << 1)
        t.append(c & 0xFFFFFFFF)
    return t


_T = _table()


def crc32_string(chksum, s):
    for b in s + b'\0':  # GCC's loop includes the terminating NUL
        chksum = ((chksum << 8) & 0xFFFFFFFF) ^ _T[((chksum >> 24) ^ b) & 0xFF]
    return chksum


def _hex(c):
    return 48 <= c <= 57 or 65 <= c <= 70


def checksum_string(chksum, s):
    """coverage_checksum_string: a "_GLOBAL__..._<8 hex>_<8 hex>" random part zeroed first."""
    b = bytearray(s)
    i = 0
    while i < len(b):
        offset = 11 if b.startswith(b'_GLOBAL__N_', i) else 9 if b.startswith(b'_GLOBAL__', i) else 0
        if offset:
            i += offset
            while i < len(b):
                if b[i] == 0x5F:
                    ok = all(i + y < len(b) and _hex(b[i + y]) for y in range(1, 9)) and \
                        i + 9 < len(b) and b[i + 9] == 0x5F and \
                        all(i + y < len(b) and _hex(b[i + y]) for y in range(10, 18))
                    if ok:
                        for y in range(10, 18):
                            b[i + y] = 0x30
                i += 1
            break
        i += 1
    return crc32_string(chksum, bytes(b))


def profile_id(name, exported, source, base):
    """coverage_compute_profile_id (the default, --param=profile-func-internal-id=0)."""
    if exported:
        c = checksum_string(0, name)
    else:
        c = checksum_string(0, source) if source else 0
        c = checksum_string(c, name)
        c = checksum_string(c, base)
    c &= 0x7FFFFFFF
    return c + (not c)


def read_gcno(path):
    """[(id, name, source, line)] of a .gcno (GCC 12+: magic, version, stamp, checksum, working folder, a flag; records of
    tag, length in bytes, payload; strings as length (with the NUL) and bytes)."""
    with open(path, 'rb') as f:
        d = f.read()
    pos = 16
    n, = struct.unpack_from('<I', d, pos)
    pos += 4 + n + 4
    out = []
    while pos + 8 <= len(d):
        tag, length = struct.unpack_from('<II', d, pos)
        pos += 8
        if tag == TAG_FUNCTION:
            p = pos
            ident, = struct.unpack_from('<I', d, p)
            p += 12
            n, = struct.unpack_from('<I', d, p)
            name = d[p + 4:p + 4 + n].rstrip(b'\0')
            p += 4 + n + 4  # the name, the "artificial" flag
            n, = struct.unpack_from('<I', d, p)
            source = d[p + 4:p + 4 + n].rstrip(b'\0')
            line, = struct.unpack_from('<I', d, p + 4 + n)
            out.append((ident, name, source, line))
        if tag == 0:
            break
        pos += length
    return out


def symbols(path):
    """(global or weak names, local names) an ELF64 object defines (.symtab); only its headers and symbol tables are
    read."""
    glob, loc = set(), set()
    with open(path, 'rb') as f:
        head = f.read(64)
        if head[:4] != b'\x7fELF' or head[4] != 2:
            return glob, loc
        shoff, = struct.unpack_from('<Q', head, 0x28)
        shentsize, shnum = struct.unpack_from('<HH', head, 0x3A)
        f.seek(shoff)
        table = f.read(shentsize * shnum)
        sections = [struct.unpack_from('<IIQQQQIIQQ', table, i * shentsize) for i in range(shnum)]
        for s in sections:
            if s[1] != 2:  # SHT_SYMTAB
                continue
            f.seek(s[4])
            syms = f.read(s[5])
            strtab = sections[s[6]]
            f.seek(strtab[4])
            strings = f.read(strtab[5])
            for off in range(0, len(syms), 24):
                st_name, st_info, _, st_shndx = struct.unpack_from('<IBBH', syms, off)
                if not st_shndx or (st_info & 0xF) != 2:  # defined functions (STT_FUNC)
                    continue
                name = strings[st_name:strings.index(b'\0', st_name)]
                (glob if (st_info >> 4) in (1, 2) else loc).add(name)
    return glob, loc


def project_root(build):
    """The project's folder as CMake gave it to the compiler (the cache's app folder, one up), '' if not configured."""
    try:
        with open(os.path.join(build, 'CMakeCache.txt'), encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('CMAKE_HOME_DIRECTORY:'):
                    return line.split('=', 1)[1].strip().rsplit('/', 1)[0]
    except OSError:
        pass
    return ''


def collect(build, out):
    """Every .gcno of the profiling build, with its object, into the list (gzip). Lines: R <project folder>, then per
    object O <its name inside the build folder>, then per function F <id> <E exported | L local | ? not in the object
    (inlined everywhere: either)> <line> <name> <source>."""
    build = os.path.abspath(build)
    objects = functions = 0
    lines = ['# tw2de functions 1', 'R\t' + project_root(build)]
    for folder, _, names in os.walk(build):
        for n in sorted(names):
            if not n.endswith('.gcno'):
                continue
            gcno = os.path.join(folder, n)
            stem = gcno[:-len('.gcno')]
            obj = stem + '.obj' if os.path.isfile(stem + '.obj') else stem + '.o'
            glob, loc = symbols(obj) if os.path.isfile(obj) else (set(), set())
            lines.append('O\t' + os.path.relpath(stem, build).replace('\\', '/'))
            objects += 1
            for ident, name, source, line in read_gcno(gcno):
                kind = 'E' if name in glob else 'L' if name in loc else '?'
                lines.append('F\t%d\t%s\t%d\t%s\t%s' % (ident, kind, line, name.decode('utf-8', 'replace'),
                                                        source.decode('utf-8', 'replace')))
                functions += 1
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with gzip.open(out, 'wt', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('pgo_functions: %d functions of %d objects -> %s' % (functions, objects, out))
    return objects


# tw2de (2026-10-29): the project's rename (tools/rename_to_tw2de.py). A profile made before it holds the old
# name in its objects' names, its sources' paths and its symbols (a namespace, a class): the same rules map them, so a
# release built after the rename still finds every function. The old name is split so the rename leaves it here.
_OLD = 'ff' + 'xiii'
_OLD_UP, _OLD_CAP = _OLD.upper(), _OLD.capitalize()
_IDENT_UP = re.compile(r'(?<=[A-Za-z0-9_])%s|%s(?=[A-Za-z0-9_])' % (_OLD_UP, _OLD_UP))
_ROOT_IN_KEY = re.compile(r'(\.dir#)(.+?)(#(?:sdk|app)#)')
_IDENT = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')


def renamed(s):
    """tools/rename_to_tw2de.py's rules."""
    for a in (_OLD + '_nx', _OLD + '-nx', _OLD):
        s = s.replace(a, 'tw2de')
    for a in (_OLD_UP + '_NX', _OLD_UP + '-NX'):
        s = s.replace(a, 'TW2DE')
    s = s.replace(_OLD_CAP, 'Tw2de')
    return _IDENT_UP.sub('TW2DE', s)


def renamed_key(key):
    """An object's profile name, renamed but for the project folder a few of them hold (".dir#<folder>#sdk#"), which
    the moves below handle."""
    m = _ROOT_IN_KEY.search(key)
    if not m:
        return renamed(key)
    return renamed(key[:m.start(2)]) + m.group(2) + renamed(key[m.end(2):])


def renamed_symbol(name):
    """A symbol (bytes): an Itanium-mangled one's identifiers renamed with their lengths, another one as text."""
    s = name.decode('utf-8', 'replace')
    if _OLD not in s.lower():
        return name
    if not s.startswith('_Z'):
        return renamed(s).encode('utf-8')
    edits = []
    for m in re.finditer(r'\d+', s):
        run = m.group(0)
        for k in range(len(run)):  # a length can follow an identifier's last digits ("ab16<old>...")
            n = int(run[k:])
            ident = s[m.end():m.end() + n]
            if n and len(ident) == n and _IDENT.fullmatch(ident) and renamed(ident) != ident:
                new = renamed(ident)
                edits.append((m.start() + k, m.end() + n, str(len(new)) + new))
                break
    for a, b, text in sorted(edits, reverse=True):
        s = s[:a] + text + s[b:]
    return s.encode('utf-8')


def renamed_source(source, roots):
    """A source's path (bytes): renamed below the project folder (the folder itself as it is)."""
    for r in roots:
        if r and source.lower().startswith(r.lower().encode() + b'/'):
            n = len(r.encode())
            return source[:n] + renamed(source[n:].decode('utf-8', 'replace')).encode('utf-8')
    return renamed(source.decode('utf-8', 'replace')).encode('utf-8')


def load(path):
    """({object name with '#' for '/', as its profile is named: [(id, kind, line, name, source)]}, the project folder)."""
    objects, root, current = {}, '', None
    with gzip.open(path, 'rt', encoding='utf-8') as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if p[0] == 'R':
                root = p[1]
            elif p[0] == 'O':
                current = objects.setdefault(renamed_key(p[1].replace('/', '#')), [])  # tw2de (2026-10-29)
            elif p[0] == 'F' and current is not None:
                current.append((int(p[1]), p[2], int(p[3]), renamed_symbol(p[4].encode('utf-8')), p[5].encode('utf-8')))
    return objects, root


def _mangled(root):
    """A folder as CMake writes it into an object's name ("D:/a/b" -> "D_/a/b")."""
    return root[0] + '_' + root[2:] if len(root) > 1 and root[1] == ':' else root.lstrip('/')


def new_ids(functions, key, old_root, new_root):
    """{old id: ([the ids the release will look for], its lineno checksum)} of one object (key: its name with '#', as in the list)."""
    base = key.replace('#', '/')
    moved = old_root and new_root and old_root.lower() != new_root.lower()
    if moved and _mangled(old_root) in base:
        base = base.replace(_mangled(old_root), _mangled(new_root), 1)
    out = {}
    for ident, kind, line, name, source in functions:
        if moved and source.lower().startswith(old_root.lower().encode() + b'/'):
            source = new_root.encode() + source[len(old_root):]
        source = renamed_source(source, (new_root, old_root))  # tw2de (2026-10-29)
        ids = []
        if kind in 'E?':
            ids.append(profile_id(name, True, source, base.encode()))
        if kind in 'L?':
            ids.append(profile_id(name, False, source, base.encode()))
        # coverage_compute_lineno_checksum: the line, the source, the name (a mismatch is only a warning). A C++ thunk
        # (_ZTh, _ZTv, _ZTc) is read with 0 (profile.cc, read_thunk_profile).
        if name[:4] in (b'_ZTh', b'_ZTv', b'_ZTc'):
            lineno = 0
        elif source:
            lineno = checksum_string(checksum_string(line, source), name)
        else:
            lineno = checksum_string(line, name)
        out[ident] = (ids, lineno)
    return out


def remap(image, ids):
    """A .gcda image with each function's id replaced by the release's (a function under two ids: its records twice,
    unless the second id is another function's)."""
    head, blocks, pos = image[:16], [], 16
    while pos + 8 <= len(image):
        tag, length = struct.unpack_from('<Ii', image, pos)
        end = pos + 8 + max(length, 0)
        if tag == TAG_FUNCTION or not blocks:
            blocks.append([tag, bytearray(image[pos:end])])
        else:
            blocks[-1][1] += image[pos:end]
        pos = end
    taken = set()
    for tag, data in blocks:
        if tag == TAG_FUNCTION and len(data) >= 12:
            taken.update(ids.get(struct.unpack_from('<I', data, 8)[0], ([], 0))[0][:1])
    out = bytearray(head)
    for tag, data in blocks:
        if tag != TAG_FUNCTION or len(data) < 12:
            out += data
            continue
        mine = ids.get(struct.unpack_from('<I', data, 8)[0])
        if not mine:
            out += data
            continue
        struct.pack_into('<I', data, 12, mine[1])
        for n, ident in enumerate(mine[0]):
            if n and ident in taken:
                continue
            taken.add(ident)
            struct.pack_into('<I', data, 8, ident)
            out += data
    return bytes(out)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    collect(sys.argv[1], sys.argv[2])
