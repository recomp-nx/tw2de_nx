#!/usr/bin/env python3
"""tw2de (2026-10-24): independent check of tools/store_cards.py's shared cards.
  python tools/verify_shared_cards.py <ref gen dir: per-store cards> <new gen dir: shared cards>
The reference is the same generated code with every store carded on its own (the pass before shared cards)."""
# Independent check of the shared cards: every store that lost its own card is followed, in straight-line code with
# no write to its base register, by a _CB through the same register whose card page covers all its bytes.
import os, re, sys
ref_dir, new_dir = sys.argv[1], sys.argv[2]
ST = re.compile(r'REX_STORE_U(8|16|32|64)(?:_C\(|_CB\((\d+), |\()(?:ctx\.)?(r\d+)\.u32(?: \+ (-?\d+))?,')
BAD = re.compile(r'^\s*\w+:\s*$|\bgoto\b|\breturn\b|\bcase\b|\(ctx, base\)|^\s*[{}]|REX_CALL|\bswitch\b|\bif\b|\belse\b|__sync|REX_CARD|simde_mm_store|memset|memcpy|longjmp|setjmp')
bad = checked = 0
for name in os.listdir(new_dir):
    if not name.startswith('tw2de_recomp_'): continue
    r = open(os.path.join(ref_dir, name)).read().split('\n'); n = open(os.path.join(new_dir, name)).read().split('\n')
    assert len(r) == len(n), name
    for k, (a, b) in enumerate(zip(r, n)):
        if a == b: continue
        ma, mb = ST.search(a), ST.search(b)
        assert ma and mb and '_C(' in a, (name, k, a, b)
        w, reg, d = int(mb.group(1)), mb.group(3), int(mb.group(4) or 0)
        if '_CB(' in b:
            card = d - int(mb.group(2))
            checked += 1
            if not (card <= d and d + w // 8 - card <= 4096): bad += 1; print('own', name, k + 1)
            continue
        ok = False
        for j in range(k + 1, len(n)):
            l = n[j]
            m = ST.search(l)
            if m and m.group(3) == reg and '_CB(' in l:
                card = int(m.group(4) or 0) - int(m.group(2))
                ok = card <= d and d + w // 8 - card <= 4096
                break
            if BAD.search(l) or re.search(r'\b%s(?:\.\w+)?\s*[-+*/|&^<>]*=[^=]' % reg, l): break
        checked += 1
        if not ok:
            bad += 1
            if bad < 10: print('uncovered', name, k + 1, b.strip())
print('checked', checked, 'bad', bad)
