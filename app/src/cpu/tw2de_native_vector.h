// tw2de (2026-11-05): the game's std::vector<unsigned char>::_M_fill_insert (sub_825A76F8) done natively on guest
// memory.
//
// Run 05626fcf / profiler 3f11c561: 22-29% of the main thread's samples in the slow frames of the load and of the 20 fps
// stretch after it fall in sub_825A76F8, 0.95 s in the load alone. The game's function inserts n copies of a byte one
// byte at a time; its append loop even reloads and stores the vector's end pointer for every byte (3 guest memory
// operations and a store card per byte). Its arguments (generated/default/tw2de_recomp_823AD1A8.cpp):
//   r3: the vector {begin @0, end @4, capacity end @8}, guest u32s, big-endian
//   r4: the position, r5: n, r6: the byte's guest address (read for every byte it stores)
// Its three paths:
//   n == 0: nothing.
//   capacity - begin < size + n: a new buffer from the game's allocator (NewBuffer below; a resize of a new vector
//     always comes here), the game's own allocator functions called as the game calls them.
//   pos == end: n bytes stored at end, end advanced by n (the game skips the store while end is 0; we leave that case to
//     it).
//   else: [pos, end) moved up by n (a backward byte copy: memmove), [pos, pos + n) filled, end advanced by n.
// The byte is read once here, after the move, so a byte address inside a range that is written goes to the game's own
// function (its result then depends on the byte-by-byte order). Everything else matches it byte for byte (tools: the PC
// harness, the notes' "native vector" entry). The game's function also clobbers volatile registers (r4-r11, ctr); no
// caller may read them after a call (the ABI), as with the CRT hooks.
#pragma once

#include <cstdint>
#include <cstring>

#include "tw2de_native_crt.h"

namespace tw2de::native_vector {

inline uint32_t LoadU32(const uint8_t *base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline void StoreU32(uint8_t *base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
  native_crt::Wrote(base, a, 4);
}
// [a, a + an) and [b, b + bn) share a byte (64-bit sums: no wrap).
inline bool Touches(uint32_t a, uint32_t an, uint32_t b, uint64_t bn) {
  return uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}

// The game's allocator: the object at *(0x82EB131C + 4), alloc(obj, bytes) at its vtable + 4, free(obj, p) at + 16
// (lis r10,-32021; addi r26,r10,4892). tw2de (2026-11-10): FF XIII-2's function (sub_825A76F8) is FF XIII's
// (sub_823AD1A8) but for these three: the global (FF XIII 0x82C177FC) and the free slot (FF XIII + 12).
// tw2de (2026-11-10): LR's (lis r10,-31979; addi r26,r10,-11796), and the two return addresses below LR's
// (sub_825A76F8 + 0xB4 / + 0x124): FF XIII-2's were left here and the first load crashed (a read of 0x480059C1).
constexpr uint32_t kAllocatorGlobal = 0x8314D1ECu;
constexpr uint32_t kFreeSlot = 16;

// The new-buffer path, the game's own steps with its own allocator (a resize of a new vector always takes it):
//   oldcap = cap - begin; want = oldcap + oldcap / 2; if want < size + n: want = size + n; bytes = (want + 3) & ~3
//   p = alloc(bytes); copy [begin, pos) to p; fill n bytes at p + (pos - begin); copy [pos, end) after them;
//   free(begin) when begin != 0; begin = p, end = p + (end - begin) + n, cap = p + bytes.
// 'resolve' (guest address -> host function or nullptr) finds the allocator's functions; both are looked up before
// anything is changed, so a false return still leaves everything to the game's function. The calls are made as the
// game makes them: r1 160 bytes down with its back chain stored, lr the game's return address, r3/r4 the arguments.
template <class Ctx, class Resolve>
bool NewBuffer(Ctx &ctx, uint8_t *base, Resolve &&resolve, uint32_t vec, uint32_t pos, uint32_t n, uint32_t val,
               uint32_t begin, uint32_t end, uint32_t cap) {
  if (pos < begin || pos > end || end < begin) return false;
  const uint32_t holder = LoadU32(base, kAllocatorGlobal + 4);
  const uint32_t vtable = LoadU32(base, holder);
  auto *alloc = resolve(LoadU32(base, vtable + 4));
  auto *release = resolve(LoadU32(base, vtable + kFreeSlot));
  if (!alloc || (begin && !release)) return false;
  const uint32_t used = end - begin;            // r27 (the game takes it as is, begin 0 or not)
  const uint32_t size = begin ? used : 0u;
  const uint32_t oldcap = cap - begin;
  uint32_t want = (oldcap >> 1) + oldcap;
  if (want < size + n) want = size + n;
  const uint32_t bytes = (want + 3u) & ~3u;
  const uint32_t at = pos - begin;
  const uint32_t r1 = ctx.r1.u32;
  ctx.r1.u64 = uint32_t(r1 - 160u);
  StoreU32(base, ctx.r1.u32, r1);
  ctx.r3.u64 = holder;
  ctx.r4.u64 = bytes;
  ctx.lr = 0x825A77ACu;
  alloc(ctx, base);
  const uint32_t p = ctx.r3.u32;
  // The byte's value: the game reads it during its fill, after the first copy; that copy writes only the new buffer.
  if (Touches(p, bytes, val, 1)) {
    // A byte inside the new buffer (cannot happen for a value on the stack or in the old buffer): as the game does.
    native_crt::Memmove(base, p, begin, at);
    for (uint32_t i = 0; i < n; ++i)
      if (p + at + i) {
        base[p + at + i] = base[val];
        native_crt::Wrote(base, p + at + i, 1);
      }
  } else {
    native_crt::Memmove(base, p, begin, at);
    native_crt::Memset(base, p + at, base[val], n);
  }
  native_crt::Memmove(base, p + at + n, begin + at, used - at);
  if (begin) {
    // The game reads the allocator again for its free.
    const uint32_t holder2 = LoadU32(base, kAllocatorGlobal + 4);
    auto *release2 = resolve(LoadU32(base, LoadU32(base, holder2) + kFreeSlot));
    ctx.r3.u64 = holder2;
    ctx.r4.u64 = begin;
    ctx.lr = 0x825A781Cu;
    (release2 ? release2 : release)(ctx, base);
  }
  StoreU32(base, vec + 0, p);
  StoreU32(base, vec + 8, p + bytes);
  StoreU32(base, vec + 4, p + used + n);
  ctx.r1.u64 = r1;
  return true;
}

// true: done; false: the caller runs the game's own function (nothing was changed).
template <class Ctx, class Resolve> bool CallFillInsert(Ctx &ctx, uint8_t *base, Resolve &&resolve) {
  const uint32_t vec = ctx.r3.u32, pos = ctx.r4.u32, n = ctx.r5.u32, val = ctx.r6.u32;
  if (n == 0) return true;
  const uint32_t begin = LoadU32(base, vec + 0), end = LoadU32(base, vec + 4), cap = LoadU32(base, vec + 8);
  // The game's test, in its 32-bit arithmetic: (cap - begin) < (begin ? end - begin : 0) + n means a new buffer.
  const uint32_t size = begin ? end - begin : 0u;
  if (uint32_t(cap - begin) < uint32_t(size + n)) return NewBuffer(ctx, base, resolve, vec, pos, n, val, begin, end, cap);
  if (end == 0 || begin == 0 || end < begin) return false;
  // The vector itself must not lie in what is written (the game reloads end from it while appending).
  if (pos == end) {
    if (Touches(end, n, val, 1) || Touches(end, n, vec, 12)) return false;
    if (uint64_t(end) + n > 0x100000000ull) return false;
    native_crt::Memset(base, end, base[val], n);
    StoreU32(base, vec + 4, end + n);
    return true;
  }
  if (pos < begin || pos > end) return false;  // not a position inside the vector: the game's own behavior
  const uint32_t tail = end - pos;
  if (uint64_t(end) + n > 0x100000000ull) return false;
  // Written: [pos, end + n). The byte is read during the fill, after the move.
  if (Touches(pos, tail + n, val, 1) || Touches(pos, tail + n, vec, 12)) return false;
  native_crt::Memmove(base, pos + n, pos, tail);
  native_crt::Memset(base, pos, base[val], n);
  StoreU32(base, vec + 4, end + n);
  return true;
}

}  // namespace tw2de::native_vector
