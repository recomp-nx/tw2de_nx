// tw2de (2026-11-06): memmove of a large range in parts that can run on several cores at once, with memmove's
// result also when the ranges overlap.
//
// Why: the game's heap compaction (sub_8279AB20 -> sub_8279A0B8, cpu/tw2de_mover_diag.cpp) moves 26-46 MB in one
// main-thread frame at an area entry and in the later bursts; run 49a8a667: 190 memmoves, 134 of them overlapping,
// 27.1 ms on one core (memory-bound, ~2.5 GB/s).
//
// The range is cut into 'parts' pieces of c bytes (the last one takes the rest). Without overlap every piece is a plain
// copy. With overlap at distance t (t = |dst - src|, required here: t <= c / 2):
//   down (dst < src): piece k writes [src + kc - t, src + (k+1)c - t), so it overwrites the LAST t bytes of piece k-1's
//     source, and nothing else of another piece. Those t bytes of every piece but the last are saved first (one pass,
//     P * t bytes). Piece k then moves its first c - t bytes (memmove: its own overlap) and copies its saved tail after
//     them; the last piece moves all of its bytes.
//   up (dst > src): the mirror image: piece k overwrites the FIRST t bytes of piece k+1's source. Those of every piece
//     but the first are saved; piece k moves its last c - t bytes, then copies its saved head before them; the first
//     piece moves all of its bytes.
// Within a piece the order is fixed (the move, then the saved bytes: they land where the move read). Two pieces never
// touch the same byte after the saving pass, so they may run in any order or at once (tools/native_tests/
// parallel_memmove_test.cpp: against memmove, every shape, pieces in random orders and on threads).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tw2de::parallel_memmove {

// true when n bytes at distance t can be cut into 'parts' pieces this way (else one memmove).
inline bool Splittable(size_t t, size_t n, uint32_t parts) {
  if (parts < 2 || n < parts * size_t(2))
    return false;
  return t >= n || t <= (n / parts) / 2;
}

// run(count, piece): calls piece(i) once for each i < count, in any order, possibly at once; returns when all are done.
// 'saved' is scratch (resized here).
template <class Run>
void Move(uint8_t *dst, const uint8_t *src_c, size_t n, uint32_t parts, std::vector<uint8_t> &saved, Run &&run) {
  uint8_t *src = const_cast<uint8_t *>(src_c);
  if (dst == src || n == 0)
    return;
  const size_t t = dst > src ? size_t(dst - src) : size_t(src - dst);
  if (!Splittable(t, n, parts)) {
    std::memmove(dst, src, n);
    return;
  }
  const size_t c = n / parts;
  auto begin = [&](uint32_t k) { return size_t(k) * c; };
  auto size = [&](uint32_t k) { return k + 1 == parts ? n - begin(k) : c; };
  if (t >= n) {  // no overlap
    run(parts, [&](uint32_t k) { std::memcpy(dst + begin(k), src + begin(k), size(k)); });
    return;
  }
  saved.resize(size_t(parts) * t);
  if (dst < src) {
    // The last t bytes of each piece but the last.
    for (uint32_t k = 0; k + 1 < parts; ++k)
      std::memcpy(saved.data() + size_t(k) * t, src + begin(k) + size(k) - t, t);
    run(parts, [&](uint32_t k) {
      if (k + 1 == parts) {
        std::memmove(dst + begin(k), src + begin(k), size(k));
        return;
      }
      std::memmove(dst + begin(k), src + begin(k), size(k) - t);
      std::memcpy(dst + begin(k) + size(k) - t, saved.data() + size_t(k) * t, t);
    });
  } else {
    // The first t bytes of each piece but the first.
    for (uint32_t k = 1; k < parts; ++k)
      std::memcpy(saved.data() + size_t(k) * t, src + begin(k), t);
    run(parts, [&](uint32_t k) {
      if (k == 0) {
        std::memmove(dst, src, size(0));
        return;
      }
      std::memmove(dst + begin(k) + t, src + begin(k) + t, size(k) - t);
      std::memcpy(dst + begin(k), saved.data() + size_t(k) * t, t);
    });
  }
}

// tw2de_parallel_memmove.cpp: a move of kLargeBytes or more done this way by the caller and two copy threads, in
// pieces of about kPieceBytes. false: not done (too small, a shape this cannot cut, another large move running): the
// caller's own memmove then.
constexpr size_t kLargeBytes = 8u << 20;
constexpr size_t kPieceBytes = 2u << 20;
bool Large(uint8_t *dst, const uint8_t *src, size_t n);

}  // namespace tw2de::parallel_memmove
