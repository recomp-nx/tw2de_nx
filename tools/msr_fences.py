#!/usr/bin/env python3
"""TW2 (2026-11-12): the full fence the generator writes for every mfmsr / mtmsrd, only where the guest lock needs it.

The generator writes `mfmsr` / `mtmsrd` (the 360 kernel's interrupt-off bracket around an interlocked operation:
mfmsr r9; mtmsrd r13,1; lwarx ... stwcx.; mtmsrd r9,1) as
        // mfmsr r9
        std::atomic_thread_fence(std::memory_order_seq_cst);
        ... = REX_CHECK_GLOBAL_LOCK();
and the same fence before REX_ENTER_GLOBAL_LOCK / REX_LEAVE_GLOBAL_LOCK for mtmsrd: a `dmb ish` each, four around every
such interlocked increment (the job system's index claim sub_82FD4E70: 8 % of the game's main thread at stock
clocks). On the PowerPC neither instruction orders memory (they are not storage barriers; the game orders its memory
with sync / lwsync, which the generator translates separately) and the lwarx / stwcx. pair is a sequentially
consistent compare-and-swap here (tools/guest_cas.py); the fence matters only with the SDK's global guest lock on
(rex::thread::guest_atomic_lock_mode 2 or 3: a real lock entered and left there). So the fence becomes
REX_MSR_FENCE(): the same fence in those modes, nothing in mode 0 (the default). REX_MSR_FENCE_FULL (compile time)
gives the fence back everywhere.

  python tools/msr_fences.py --gen app/generated/default [--check]
Idempotent.
"""
import argparse
import os
import re
import sys

FENCE = '\tstd::atomic_thread_fence(std::memory_order_seq_cst);'
NEW = '\tREX_MSR_FENCE();'
COMMENT = re.compile(r'^\t// (mfmsr|mtmsrd)\b')
MARKER = '// TW2: the mfmsr / mtmsrd fence only with the guest lock on (tools/msr_fences.py)'
MACRO = MARKER + '''
#ifndef REX_MSR_FENCE
#if defined(REX_MSR_FENCE_FULL)
#define REX_MSR_FENCE() std::atomic_thread_fence(std::memory_order_seq_cst)
#else
#define REX_MSR_FENCE() \\
  do { if (rex::thread::guest_atomic_lock_mode >= 2) std::atomic_thread_fence(std::memory_order_seq_cst); } while (0)
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp(\.\d+|_[0-9A-F]{8})\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): msr_fences: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    sites = done = changed = 0
    for f in files:
        path = os.path.join(a.gen, f)
        text = open(path, encoding='utf-8').read()
        if '// mfmsr' not in text and '// mtmsrd' not in text:
            continue
        lines = text.split('\n')
        n = 0
        for i in range(1, len(lines)):
            if lines[i] == NEW and COMMENT.match(lines[i - 1]):
                done += 1
            elif lines[i] == FENCE and COMMENT.match(lines[i - 1]):
                lines[i] = NEW
                n += 1
        if n:
            sites += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text and not a.check:
        anchor = '#define REX_CHECK_GLOBAL_LOCK()'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): msr_fences: no REX_CHECK_GLOBAL_LOCK in %s' % pch)
        open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACRO + anchor, 1))
    print('msr_fences%s: %d mfmsr / mtmsrd fences made conditional in %d files (%d already)' %
          (' (check only)' if a.check else '', sites, changed, done))


if __name__ == '__main__':
    main()
