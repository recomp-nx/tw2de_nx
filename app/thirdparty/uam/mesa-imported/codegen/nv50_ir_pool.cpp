/*
 * tw2de (2026-10-23): nv50_ir_pool.h.
 */
#include "codegen/nv50_ir_pool.h"

#include <new>

namespace nv50_ir {

namespace {

const size_t kGrain = 16, kMax = 1024, kClasses = kMax / kGrain, kChunk = 64 * 1024;

struct FreeBlock {
   FreeBlock *next;
};

struct Chunk {
   Chunk *next;
};

struct ThreadPool {
   FreeBlock *free[kClasses] = {};
   Chunk *chunks = nullptr;
   char *at = nullptr, *end = nullptr;  // the newest chunk's unused part
   long live = 0;                       // blocks given and not returned (by this thread)

   void *allocate(size_t size)
   {
      const size_t c = (size + kGrain - 1) / kGrain - 1;
      ++live;
      if (FreeBlock *b = free[c]) {
         free[c] = b->next;
         return b;
      }
      const size_t bytes = (c + 1) * kGrain;
      if (size_t(end - at) < bytes) {
         Chunk *k = static_cast<Chunk *>(::operator new(kChunk));
         k->next = chunks;
         chunks = k;
         at = reinterpret_cast<char *>(k) + kGrain;  // the chunk's link, then 16-byte aligned blocks
         end = reinterpret_cast<char *>(k) + kChunk;
      }
      void *p = at;
      at += bytes;
      return p;
   }

   void release(void *p, size_t size)
   {
      const size_t c = (size + kGrain - 1) / kGrain - 1;
      FreeBlock *b = static_cast<FreeBlock *>(p);
      b->next = free[c];
      free[c] = b;
      --live;
   }

   void trim()
   {
      if (live != 0)
         return;  // a block still in use (or one another thread gave back): the chunks stay
      while (chunks) {
         Chunk *k = chunks;
         chunks = k->next;
         ::operator delete(k);
      }
      for (size_t i = 0; i < kClasses; ++i)
         free[i] = nullptr;
      at = end = nullptr;
   }
};

thread_local ThreadPool pool;

} // namespace

void *poolAllocate(size_t size)
{
   if (size == 0)
      size = 1;
   if (size > kMax)
      return ::operator new(size);
   return pool.allocate(size);
}

void poolRelease(void *p, size_t size) noexcept
{
   if (!p)
      return;
   if (size == 0)
      size = 1;
   if (size > kMax) {
      ::operator delete(p);
      return;
   }
   pool.release(p, size);
}

void poolTrim() noexcept
{
   pool.trim();
}

} // namespace nv50_ir
