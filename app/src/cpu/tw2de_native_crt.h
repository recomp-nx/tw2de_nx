// tw2de (2026-10-23): native versions of the game's C runtime memory and string functions, on guest memory (base:
// the guest window, the arguments and results guest addresses). The C library's own functions do the work; the compare
// functions' results are made what the game's own return (tools: the PC harness compares each with the game's
// recompiled function over random inputs before it is used: tw2de_native_crt.cpp lists the proven ones).
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>

#include <rex/system/xmemory.h>  // tw2de (2026-10-24): the store cards

namespace tw2de::native_crt {

inline uint8_t *P(uint8_t *base, uint32_t a) { return base + a; }
inline uint32_t G(uint8_t *base, const void *p) {
  return uint32_t(reinterpret_cast<const uint8_t *>(p) - base);
}
inline int32_t Sign(int r) { return (r > 0) - (r < 0); }
// tw2de (2026-10-24): a host write of guest memory marks its pages' store cards (rex/system/xmemory.h), as the
// game's own stores do (tools/store_cards.py): the cards are the guest window's first MB, at base.
inline void Wrote(uint8_t *base, uint32_t d, uint32_t n) { rex::memory::MarkStoreCardsAt(base, d, n); }

// memcpy and its kin: memmove (an overlap gives what memmove gives; the game's copies agree with it where they do not
// overlap, which is what the C standard defines).
inline uint32_t Memmove(uint8_t *base, uint32_t d, uint32_t s, uint32_t n) {
  std::memmove(P(base, d), P(base, s), n);
  Wrote(base, d, n);
  return d;
}
inline uint32_t Memset(uint8_t *base, uint32_t d, uint32_t c, uint32_t n) {
  std::memset(P(base, d), int(c & 0xFF), n);
  Wrote(base, d, n);
  return d;
}
inline uint32_t Memchr(uint8_t *base, uint32_t p, uint32_t c, uint32_t n) {
  const void *r = std::memchr(P(base, p), int(c & 0xFF), n);
  return r ? G(base, r) : 0;
}
inline int32_t MemcmpSign(uint8_t *base, uint32_t a, uint32_t b, uint32_t n) {
  return Sign(std::memcmp(P(base, a), P(base, b), n));
}
inline uint32_t Strlen(uint8_t *base, uint32_t s) { return uint32_t(std::strlen(reinterpret_cast<char *>(P(base, s)))); }
inline uint32_t Strcpy(uint8_t *base, uint32_t d, uint32_t s) {
  const size_t n = std::strlen(reinterpret_cast<char *>(P(base, s))) + 1;
  std::memmove(P(base, d), P(base, s), n);
  Wrote(base, d, uint32_t(n));
  return d;
}
inline uint32_t Strncpy(uint8_t *base, uint32_t d, uint32_t s, uint32_t n) {
  std::strncpy(reinterpret_cast<char *>(P(base, d)), reinterpret_cast<char *>(P(base, s)), n);
  Wrote(base, d, n);
  return d;
}
inline uint32_t Strcat(uint8_t *base, uint32_t d, uint32_t s) {
  const size_t at = std::strlen(reinterpret_cast<char *>(P(base, d)));
  std::strcat(reinterpret_cast<char *>(P(base, d)), reinterpret_cast<char *>(P(base, s)));
  Wrote(base, d + uint32_t(at), uint32_t(std::strlen(reinterpret_cast<char *>(P(base, d + uint32_t(at)))) + 1));
  return d;
}
inline uint32_t Strchr(uint8_t *base, uint32_t s, uint32_t c) {
  const char *r = std::strchr(reinterpret_cast<char *>(P(base, s)), int(char(c)));
  return r ? G(base, r) : 0;
}
inline uint32_t Strrchr(uint8_t *base, uint32_t s, uint32_t c) {
  const char *r = std::strrchr(reinterpret_cast<char *>(P(base, s)), int(char(c)));
  return r ? G(base, r) : 0;
}
inline uint32_t Strstr(uint8_t *base, uint32_t h, uint32_t n) {
  const char *r = std::strstr(reinterpret_cast<char *>(P(base, h)), reinterpret_cast<char *>(P(base, n)));
  return r ? G(base, r) : 0;
}
inline int32_t StrcmpSign(uint8_t *base, uint32_t a, uint32_t b) {
  return Sign(std::strcmp(reinterpret_cast<char *>(P(base, a)), reinterpret_cast<char *>(P(base, b))));
}
inline int32_t StrncmpSign(uint8_t *base, uint32_t a, uint32_t b, uint32_t n) {
  return Sign(std::strncmp(reinterpret_cast<char *>(P(base, a)), reinterpret_cast<char *>(P(base, b)), n));
}

// --- The guest calls, as the overrides (tw2de_native_crt.cpp) make them -------------------------------------------
// Each takes the guest's arguments from r3.. and returns false when the call must go to the game's own function
// instead. Only what was proven equal is done natively; the rest is left to the original:
//   - the copies the harness proved equal to memcpy, not memmove (sub_82D21E80 and four more): they copy forwards, so
//     with overlapping ranges they give something else; those calls go to the original;
//   - strncpy: proven with separate buffers; overlapping ones go to the original.
// r3 is left as it is when the function returns its first argument (the original does not change it either); a found
// pointer is the guest address, zero-extended (null: 0). Templates on the context, so the PC harness tests these same
// lines.

inline bool Overlap(uint32_t a, uint64_t an, uint32_t b, uint64_t bn) {
  return an && bn && uint64_t(a) < uint64_t(b) + bn && uint64_t(b) < uint64_t(a) + an;
}

template <class Ctx> bool CallMemcpy(Ctx &ctx, uint8_t *base) {
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  if (Overlap(d, n, s, n))
    return false;
  std::memcpy(P(base, d), P(base, s), n);
  Wrote(base, d, n);
  return true;
}
template <class Ctx> bool CallMemmove(Ctx &ctx, uint8_t *base) {
  std::memmove(P(base, ctx.r3.u32), P(base, ctx.r4.u32), ctx.r5.u32);
  Wrote(base, ctx.r3.u32, ctx.r5.u32);
  return true;
}
template <class Ctx> bool CallMemset(Ctx &ctx, uint8_t *base) {
  std::memset(P(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), ctx.r5.u32);
  Wrote(base, ctx.r3.u32, ctx.r5.u32);
  return true;
}
template <class Ctx> bool CallStrncpy(Ctx &ctx, uint8_t *base) {
  const uint32_t d = ctx.r3.u32, s = ctx.r4.u32, n = ctx.r5.u32;
  const size_t len = strnlen(reinterpret_cast<const char *>(P(base, s)), n);
  if (Overlap(d, n, s, len < n ? len + 1 : n))
    return false;
  std::strncpy(reinterpret_cast<char *>(P(base, d)), reinterpret_cast<const char *>(P(base, s)), n);
  Wrote(base, d, n);
  return true;
}
// The game's strchr (sub_82D21E40) compares each byte with the whole 32-bit c (cmpw), not with (char)c as C does: a
// c above 255, a sign-extended char among them (0xFFFFFFE9), finds nothing. 0 finds the terminator, as in C.
template <class Ctx> bool CallStrchr(Ctx &ctx, uint8_t *base) {
  const uint32_t c = ctx.r4.u32;
  ctx.r3.u64 = c > 0xFF ? 0 : Strchr(base, ctx.r3.u32, c);
  return true;
}
template <class Ctx> bool CallStrstr(Ctx &ctx, uint8_t *base) {
  ctx.r3.u64 = Strstr(base, ctx.r3.u32, ctx.r4.u32);
  return true;
}

}  // namespace tw2de::native_crt
