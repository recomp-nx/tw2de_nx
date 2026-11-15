#!/usr/bin/env python3
"""tw2de (2026-11-12): renamed the FNXF XIII-LR project copy this project started from to TW2DE NX, once
(already run; kept as the record of the rules).

    python3 tools/rename_to_fnxf_xiii_lr.py [--dry-run]

    <old>_2 -> <old>_lr, <OLD>_2 -> <OLD>_LR, FnxfXiii2 -> FnxfXiiiLr (in identifiers, cvars, comments), the title
    "FNXF XIII-2" -> "FNXF XIII-LR", the NRO / SD folder name <old>-2 -> <old>-lr, and in file and folder names the
    same (<old> = the FNXF XIII project's short name).
Skipped: _claude/, out/, app/generated/, app/assets/, pgo/, __pycache__/, and files that are not text.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELF = os.path.abspath(__file__)
SKIP = {'_claude', 'out', 'pgo', '__pycache__', '.git', '_to_delete'}
SKIP_PATHS = {os.path.join('app', 'generated'), os.path.join('app', 'assets')}
OLD = 'fnxf' + '_xiii'  # split, so this file reads the same after the rename
RULES = [
    (re.compile(OLD + r'_lr(?![0-9A-Za-z])'), 'tw2de'),
    (re.compile(OLD + r'-lr(?![0-9A-Za-z])'), 'tw2de_nx'),
    (re.compile(OLD.upper() + r'_LR(?![0-9A-Za-z])'), 'TW2DE'),
    (re.compile('Fnxf' + 'XiiiLr' + r'(?![0-9A-Za-z])'), 'Tw2de'),
    (re.compile('FNXF' + r' XIII-LR(?![0-9A-Za-z])'), 'TW2DE NX'),
]


def renamed(s):
    for rx, new in RULES:
        s = rx.sub(new, s)
    return s


def main():
    dry = '--dry-run' in sys.argv
    files = changed = 0
    moves = []
    for dp, dns, fns in os.walk(ROOT):
        rel = os.path.relpath(dp, ROOT)
        dns[:] = [d for d in dns if d not in SKIP and os.path.normpath(os.path.join(rel, d)) not in SKIP_PATHS]
        for d in dns:
            if renamed(d) != d:
                moves.append(os.path.join(dp, d))
        for f in fns:
            p = os.path.join(dp, f)
            if os.path.abspath(p) == SELF:
                continue
            data = open(p, 'rb').read()
            text = None
            if b'\0' not in data[:65536]:
                try:
                    text = data.decode('utf-8')
                except UnicodeDecodeError:
                    pass
            files += 1
            if text is not None and renamed(text) != text:
                changed += 1
                if not dry:
                    open(p, 'wb').write(renamed(text).encode('utf-8'))
            if renamed(f) != f:
                moves.append(p)
    for p in sorted(moves, key=len, reverse=True):  # deepest first
        d, n = os.path.split(p)
        target = os.path.join(d, renamed(n))
        if os.path.exists(target):
            print('skip (exists)', os.path.relpath(target, ROOT))
            continue
        print('rename', os.path.relpath(p, ROOT), '->', renamed(n))
        if not dry:
            os.rename(p, target)
    print(f'{files} files, {changed} changed, {len(moves)} renamed')


if __name__ == '__main__':
    main()
