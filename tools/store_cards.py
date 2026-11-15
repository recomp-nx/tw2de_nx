#!/usr/bin/env python3
"""tw2de (2026-10-19, CPU proposal 1 of docs/cpu-codegen-proposals-2026-10-19.md): the store cards.

Renut keeps converted copies of the game's vertex and index data and, with tw2de_renut_exact_streams, proves each copy
by hashing its guest bytes again at its first use after every kick (16 MB a frame in gameplay, a quarter of the render
thread on the console). With the cards it hashes only data whose memory was written since the copy was proven: every
guest store outside the stack also zeroes its 4 KB page's card, the byte at guest address ea >> 12 (the first MB of the
guest window, reserved for them in Memory::Initialize), and Renut sets the cards of the pages it checks back to 1.

This step renames the stores that may write outside the stack to card-marking forms:
    REX_STORE_D_U32(r31.u32, 8, x);         ->  REX_STORE_D_U32_C(r31.u32, 8, x);
    REX_STORE_U32(ea, x);                   ->  REX_STORE_U32_C(ea, x);
    simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), ...);   ->  REX_CARD(ea), simde_mm_store_si128(...);
    memset((void*)REX_RAW_ADDR(ea), 0, 128);                      ->  REX_CARD(ea), memset(...);   (dcbz, dcbzl)
    REX_GUEST_CAS(reinterpret_cast<...>(REX_RAW_ADDR(ea)), ...)   ->  REX_GUEST_CAS_C(ea, reinterpret_cast<...>...)
    REX_STVLX(ea, v); REX_STVRX(ea, v); REX_MM_STORE_U32(a, v);    ->  the _C forms
Stores through the guest stack pointer (r1 or its local mirrors as the base, or an ea computed from it) are left as
they are: the stack is never vertex or index data. A store marks the card of its first byte's page: a vector store,
dcbz, stvlx and stvrx stay in one aligned block, but an unaligned store of 2-8 bytes may run into the next page, which
it does not mark, so Renut counts a page as written when its card or the card of the page before it is (one card
store per guest store instead of two).

The inline memcpy / memset of tools/inline_memcalls.py need no cards: they never write the physical views
(rex::ppc::PlainRange), where all GPU data lives; the CRT hooks that do (sdk/src/kernel/crt/memory.cpp) mark their
ranges themselves, as do the other host writers of guest memory.

Shared cards (2026-10-24): Renut also counts a page as written when the card of the page before it is (above), so one
card at base + dmin covers every byte in [base + dmin, base + dmin + 4096). Stores through the same guest register
(ctx.rN.u32 + d) in straight-line code, with no write to that register between them and all their bytes inside one such
4 KB span, share one card: the earlier ones become plain stores and the last one marks the card of the lowest address,
REX_STORE_Un_CB(back, ea, v) (back = its offset minus the lowest). The card stays after every store it answers for. A
label, branch, call, return or any line this step does not know as register-only ends every open group.

Each _C / _CB form evaluates its address once: the stores are volatile and, with -fno-strict-aliasing, may change ctx,
so an address written twice was loaded from ctx and added again for the card (2026-10-24: 2.3 MB of .text, ~250k
extra ldr / add).

REX_STORE_CARDS_OFF (compile time) makes every card a no-op. Idempotent; --restore puts the plain forms back.
  python tools/store_cards.py --gen app/generated [--restore] [--check]
"""
import argparse
import glob
import os
import re
import sys

R1 = re.compile(r'(?<![\w.])(?:ctx\.r1|r1l|l_r1|ll_r1|r1)\.[us](?:32|64)\b')
STORE = re.compile(r'\b(REX_STORE_D_U(?:8|16|32|64)|REX_STORE_U(?:8|16|32|64)|REX_STVLX|REX_STVRX|'
                   r'REX_MM_STORE_U(?:8|16|32|64)|TW2DE_HOT_STORE_(?:D_)?U(?:8|16|32|64))\(')
VEC = 'simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea),'
DCBZ = re.compile(r'(?<![\w,] )\bmemset\(\(void\*\)REX_RAW_ADDR\(ea\), 0, (?:32|128)\);')
CAS = 'REX_GUEST_CAS(reinterpret_cast<'
# tw2de (2026-10-24): stwcx. / stdcx. as the generator writes them now (the builtin, not REX_GUEST_CAS).
SYNC_CAS = '__sync_bool_compare_and_swap(reinterpret_cast<'
EA_SET = re.compile(r'^\s*ea = (.*);\s*$')
INSN = re.compile(r'^\s*// ')

MARKER = '// tw2de: the store cards (tools/store_cards.py)'
END = '// (end of the macros of tools/store_cards.py)\n'
MACROS = MARKER + '''
// A guest store outside the stack also zeroes its 4 KB page's card: the byte at guest address ea >> 12, in the first
// MB of the guest window (reserved for them). Renut proves its cached vertex and index copies by their pages' cards
// instead of hashing them at every kick (tw2de_renut_store_cards). A store marks its first byte's page; one that runs
// into the next page (unaligned) does not mark that one, so Renut reads a page's card together with the page before's.
#ifndef REX_CARD
#if defined(REX_STORE_CARDS_OFF)
#define REX_CARD(ea) ((void)0)
#else
#define REX_CARD(ea) ((void)(*(u8*)(base + ((u32)(ea) >> 12)) = 0))
#endif
#define REX_STORE_D_U8_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U8(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_D_U16_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U16(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_D_U32_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U32(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_D_U64_C(r, d, y) \
  ({ const u32 rex_card_r_ = (u32)(r); REX_STORE_D_U64(rex_card_r_, d, y); REX_CARD(rex_card_r_ + (u32)(d)); })
#define REX_STORE_U8_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U8(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U16_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U16(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U32_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U32(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U64_C(x, y) ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U64(rex_card_ea_, y); REX_CARD(rex_card_ea_); })
#define REX_STORE_U8_CB(back, x, y) \
  ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U8(rex_card_ea_, y); REX_CARD(rex_card_ea_ - (u32)(back)); })
#define REX_STORE_U16_CB(back, x, y) \
  ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U16(rex_card_ea_, y); REX_CARD(rex_card_ea_ - (u32)(back)); })
#define REX_STORE_U32_CB(back, x, y) \
  ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U32(rex_card_ea_, y); REX_CARD(rex_card_ea_ - (u32)(back)); })
#define REX_STORE_U64_CB(back, x, y) \
  ({ const u32 rex_card_ea_ = (u32)(x); REX_STORE_U64(rex_card_ea_, y); REX_CARD(rex_card_ea_ - (u32)(back)); })
#define REX_STVLX_C(ea, v) do { REX_STVLX(ea, v); REX_CARD(ea); } while (0)
#define REX_STVRX_C(ea, v) do { REX_STVRX(ea, v); REX_CARD(ea); } while (0)
#define REX_MM_STORE_U8_C(a, v) do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U8(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_MM_STORE_U16_C(a, v) do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U16(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_MM_STORE_U32_C(a, v) do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U32(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_MM_STORE_U64_C(a, v) do { const u32 rex_card_ea_ = (u32)(a); REX_MM_STORE_U64(rex_card_ea_, v); REX_CARD(rex_card_ea_); } while (0)
#define REX_GUEST_CAS_C(ea, p, e, d) (REX_CARD(ea), REX_GUEST_CAS(p, e, d))
#define REX_SYNC_CAS_C(ea, p, e, d) (REX_CARD(ea), __sync_bool_compare_and_swap(p, e, d))
#endif  // REX_CARD
''' + END


def first_arg(text, start):
    """The first argument of the call whose '(' is at text[start - 1]: up to the first top-level comma."""
    depth = 0
    i = start
    while i < len(text):
        c = text[i]
        if c in '([':
            depth += 1
        elif c in ')]':
            if depth == 0:
                return text[start:i]
            depth -= 1
        elif c == ',' and depth == 0:
            return text[start:i]
        i += 1
    return text[start:]


def stack_ea(lines, k):
    """Whether the ea a store on lines[k] uses was computed from r1 by its own instruction (the lines since the
    instruction's comment). Not found: not known to be the stack."""
    j = k
    while j >= 0:
        m = EA_SET.match(lines[j])
        if m:
            return bool(R1.search(m.group(1)))
        if j < k and INSN.match(lines[j]):
            return False
        j -= 1
    return False


def stack_arg(arg, lines, k):
    arg = arg.strip()
    if arg == 'ea' or arg.startswith('ea ') or arg.startswith('(ea'):
        return stack_ea(lines, k)
    return bool(R1.search(arg))


def apply_line(lines, k, counts):
    line = lines[k]
    if INSN.match(line):
        return line
    out = []
    pos = 0
    for m in STORE.finditer(line):
        name = m.group(1)
        arg = first_arg(line, m.end())
        if stack_arg(arg, lines, k):
            counts['stack'] += 1
            continue
        out.append(line[pos:m.start()])
        out.append(name + '_C(')
        pos = m.end()
        key = 'TW2DE_HOT' if name.startswith('TW2DE_HOT') else (name.split('_U')[0] if '_U' in name else name)
        counts[key] += 1
    out.append(line[pos:])
    line = ''.join(out)
    if VEC in line and 'REX_CARD(ea), ' + VEC not in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line.replace(VEC, 'REX_CARD(ea), ' + VEC)
            counts['vector'] += 1
    m = DCBZ.search(line)
    if m and 'REX_CARD(ea), memset' not in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line[:m.start()] + 'REX_CARD(ea), ' + line[m.start():]
            counts['dcbz'] += 1
    if CAS in line and 'REX_RAW_ADDR(ea)' in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line.replace(CAS, 'REX_GUEST_CAS_C(ea, reinterpret_cast<')
            counts['cas'] += 1
    if SYNC_CAS in line and 'REX_RAW_ADDR(ea)' in line:
        if stack_ea(lines, k):
            counts['stack'] += 1
        else:
            line = line.replace(SYNC_CAS, 'REX_SYNC_CAS_C(ea, reinterpret_cast<')
            counts['cas'] += 1
    return line


# Shared cards: see the docstring.
# tw2de (2026-10-25): a base register may be a C++ local (non_volatile_as_local, non_argument_as_local: r31.u32 for
# ctx.r31.u32); in one function a register is either a local or in ctx, so the register's name is the group's key.
GROUP_STORE = re.compile(r'^(\s*)REX_STORE_U(8|16|32|64)_C\((?:ctx\.)?(r\d+)\.u32(?: \+ (-?\d+))?, (.*)\);\s*$')
PLAIN_STORE = re.compile(r'^\s*REX_STORE_U(?:8|16|32|64)\(.*\);\s*$')
ASSIGN = re.compile(r'^\s*(?:(?:ctx\.)?(\w+)(?:\.\w+)?|ea)\s*(?:[-+*/|&^]|<<|>>)?=\s*(.*);\s*$')
COMPARE = re.compile(r'^\s*(?:ctx\.)?cr\d\.compare<\w+>\((.*)\);\s*$')
CALL = re.compile(r'\b(\w+)\s*(?:<[^<>()]*>)?\s*\(')
PURE_CALLS = re.compile(r'^(?:REX_LOAD_(?:D_)?[UF]\d+|__builtin_\w+|u?int(?:8|16|32|64)_t|float|double|'
                        r'u8|u16|u32|u64|s8|s16|s32|s64)$')


def pure(expr):
    return all(PURE_CALLS.match(m.group(1)) for m in CALL.finditer(expr))


def group_cards(lines, counts):
    """Lets runs of card-marking stores through one register share a card (see the docstring)."""
    groups = {}  # register -> [dmin, dend, [(line index, width, d)]]

    def close(reg):
        g = groups.pop(reg)
        stores = g[2]
        if len(stores) < 2:
            return
        for k, w, d in stores[:-1]:
            lines[k] = lines[k].replace('REX_STORE_U%s_C(' % w, 'REX_STORE_U%s(' % w, 1)
        k, w, d = stores[-1]
        lines[k] = lines[k].replace('REX_STORE_U%s_C(' % w, 'REX_STORE_U%s_CB(%d, ' % (w, d - g[0]), 1)
        counts['shared'] += len(stores) - 1
        counts['groups'] += 1

    def close_all():
        for reg in list(groups):
            close(reg)

    for k, line in enumerate(lines):
        m = GROUP_STORE.match(line)
        if m and pure(m.group(5)):
            w, reg, d = m.group(2), m.group(3), int(m.group(4) or 0)
            end = d + int(w) // 8
            g = groups.get(reg)
            if g is not None and max(g[1], end) - min(g[0], d) > 4096:
                close(reg)
                g = None
            if g is None:
                groups[reg] = [d, end, [(k, w, d)]]
            else:
                g[0], g[1] = min(g[0], d), max(g[1], end)
                g[2].append((k, w, d))
            continue
        s = line.strip()
        if not s or s.startswith('//'):
            continue
        if PLAIN_STORE.match(line) and 'REX_STORE_U' in line and '_C' not in line.split('(')[0]:
            if pure(line[line.index('(') + 1:]):
                continue  # a stack store (left plain above): writes no register
        m = COMPARE.match(line)
        if m and pure(m.group(1)):
            continue
        m = ASSIGN.match(line)
        if m and pure(m.group(2)):
            if m.group(1) in groups:
                close(m.group(1))
            continue
        close_all()
    close_all()
    return lines


UNDO = [
    (re.compile(r'\b(REX_STORE_D_U(?:8|16|32|64)|REX_STORE_U(?:8|16|32|64)|REX_STVLX|REX_STVRX|'
                r'REX_MM_STORE_U(?:8|16|32|64)|TW2DE_HOT_STORE_(?:D_)?U(?:8|16|32|64))_C\('), r'\1('),
    (re.compile(r'\b(REX_STORE_U(?:8|16|32|64))_CB\(\d+, '), r'\1('),
    (re.compile(r'REX_CARD\(ea\), (?=simde_mm_store_si128|memset\()'), ''),
    (re.compile(r'REX_GUEST_CAS_C\(ea, '), 'REX_GUEST_CAS('),
    (re.compile(r'REX_SYNC_CAS_C\(ea, '), '__sync_bool_compare_and_swap('),
]


def restore_text(text):
    for rx, rep in UNDO:
        text = rx.sub(rep, text)
    return text


# tw2de (2026-10-24): the switch Renut reads (g_tw2de_store_cards), defined true next to the code that marks the
# cards, overriding the app's weak false (app/src/tw2de_renderer_shared.cpp).
INIT_MARK = '// tw2de: tools/store_cards.py ran on this code: its stores mark the cards.'
INIT_BLOCK = '\n' + INIT_MARK + '\nextern "C" const bool g_tw2de_store_cards = true;\n'


def set_init_flag(gen, on, check):
    inits = glob.glob(os.path.join(gen, '*_init.cpp'))
    if len(inits) != 1:
        sys.exit('ERROR (post-codegen patch): store_cards: no single *_init.cpp in %s' % gen)
    text = open(inits[0], encoding='utf-8').read()
    new = text.replace(INIT_BLOCK, '')
    if on:
        new += INIT_BLOCK
    if new != text and not check:
        open(inits[0], 'w', encoding='utf-8', newline='\n').write(new)


def files(gen):
    return (sorted(glob.glob(os.path.join(gen, '*_recomp[._]*.cpp'))) +
            sorted(glob.glob(os.path.join(gen, 'hot', 'n_*.h'))))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp, hot/ and *_pch.h)')
    ap.add_argument('--restore', action='store_true', help='put the plain stores back (and drop the macros)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    counts = {k: 0 for k in ('REX_STORE_D', 'REX_STORE', 'REX_STVLX', 'REX_STVRX', 'REX_MM_STORE', 'TW2DE_HOT',
                             'vector', 'dcbz', 'cas', 'stack', 'shared', 'groups')}
    changed = 0
    for path in files(a.gen):
        text = open(path, encoding='utf-8').read()
        base = restore_text(text)
        if a.restore:
            new = base
        else:
            lines = base.split('\n')
            carded = [apply_line(lines, k, counts) for k in range(len(lines))]
            # STORE_CARDS_NO_SHARE=1: every store with its own card (the reference tools/verify_shared_cards.py takes)
            new = '\n'.join(carded if os.environ.get('STORE_CARDS_NO_SHARE') else group_cards(carded, counts))
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): store_cards: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    new = text
    if MARKER in new:
        start = new.index(MARKER)
        new = new[:start] + new[new.index(END, start) + len(END):]
    if not a.restore:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in new:
            sys.exit('ERROR (post-codegen patch): store_cards: no REX_LOAD_U8 in %s' % pchs[0])
        new = new.replace(anchor, MACROS + anchor, 1)
    if new != text and not a.check:
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(new)
    set_init_flag(a.gen, not a.restore, a.check)
    if a.restore:
        print('store_cards: plain stores restored (%d files)' % changed)
    else:
        print('store_cards%s: cards on %d D-form, %d X-form, %d vector, %d dcbz, %d compare-and-swap, %d stvlx, %d '
              'stvrx, %d MMIO-capable and %d localized-copy stores; %d stack stores left plain (%d files)' %
              (' (check only)' if a.check else '', counts['REX_STORE_D'], counts['REX_STORE'], counts['vector'],
               counts['dcbz'], counts['cas'], counts['REX_STVLX'], counts['REX_STVRX'], counts['REX_MM_STORE'],
               counts['TW2DE_HOT'], counts['stack'], changed))
        print('store_cards: %d stores share the card of another (%d groups)' % (counts['shared'], counts['groups']))


if __name__ == '__main__':
    main()
