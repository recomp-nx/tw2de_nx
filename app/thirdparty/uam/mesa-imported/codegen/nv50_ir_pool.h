/*
 * tw2de (2026-10-23): small blocks for nv50_ir's own objects from per-thread free lists.
 *
 * A compile made ~100,000 operator new / delete calls of its own (callgrind over the game's shaders, a fast pass):
 * the CFG and interference graphs' edges, each instruction's two std::deque (a map and a node each), a value's use set
 * and definition list, the DLList items. They now come from 64 KB chunks of this thread, a free list per 16-byte size
 * class: an allocation is a pop, a free a push, no lock and no search. Blocks above 1 KB go to operator new as before.
 * poolTrim() (the end of a compile) gives the chunks back once none of their blocks is in use. The code a compile makes
 * does not depend on where its objects are (the same DKSH for every shader of the game under different allocators).
 */
#ifndef __NV50_IR_POOL_H__
#define __NV50_IR_POOL_H__

#include <cstddef>

namespace nv50_ir {

void *poolAllocate(size_t size);
void poolRelease(void *p, size_t size) noexcept;
void poolTrim() noexcept;

template <class T>
struct PoolAllocator {
   typedef T value_type;
   PoolAllocator() noexcept {}
   template <class U> PoolAllocator(const PoolAllocator<U> &) noexcept {}
   T *allocate(size_t n) { return static_cast<T *>(poolAllocate(n * sizeof(T))); }
   void deallocate(T *p, size_t n) noexcept { poolRelease(p, n * sizeof(T)); }
   template <class U> bool operator==(const PoolAllocator<U> &) const noexcept { return true; }
   template <class U> bool operator!=(const PoolAllocator<U> &) const noexcept { return false; }
};

// A class's own new / delete from the pool.
#define NV50_IR_POOLED_CLASS \
   static void *operator new(size_t n) { return ::nv50_ir::poolAllocate(n); } \
   static void operator delete(void *p, size_t n) noexcept { ::nv50_ir::poolRelease(p, n); }

} // namespace nv50_ir

#endif // __NV50_IR_POOL_H__
