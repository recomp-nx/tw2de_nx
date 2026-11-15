/**
 * @file    gpu/render/upload_ring.cpp
 */
#include "renut_engine/gpu/render/upload_ring.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "renut_engine/gpu/tw2de_renut_tuning.h"  // tw2de: tw2de_renut_memory_policy
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/log.h"

namespace renut::gpu {

bool UploadRing::Init(plume::RenderDevice *device, const char *name, bool constant_sets, bool host_cached) {
  device_ = device;
  name_ = name;
  constant_sets_ = constant_sets;
  host_cached_ = host_cached;
  chunks_.clear();
  current_ = 0;
  offset_ = 0;
  if (!AddChunk(kChunkSize))
    return false;
  // tw2de (2026-11-05): kKeepChunks at once (the others only if the heap gives them: the first is enough to run).
  while (chunks_.size() < kKeepChunks && AddChunk(kChunkSize)) {
  }
  return true;
}

// tw2de (2026-10-24): a chunk size the heap could not give, per ring, and the ring's resets left before it is
// tried again. Every allocation that needed a new chunk tried 32 MB again (the game after the tutorial battle: a failed
// createBuffer every 10 ms on the render thread, and the draws without their constants).
namespace {
struct ChunkBackoff {
  const UploadRing *ring = nullptr;
  uint64_t failed_size = 0;
  uint32_t resets_left = 0;
};
ChunkBackoff g_chunk_backoff[4];
constexpr uint32_t kChunkBackoffResets = 300;

ChunkBackoff &BackoffOf(const UploadRing *ring) {
  for (ChunkBackoff &b : g_chunk_backoff)
    if (b.ring == ring)
      return b;
  for (ChunkBackoff &b : g_chunk_backoff)
    if (!b.ring) {
      b.ring = ring;
      return b;
    }
  return g_chunk_backoff[0];  // more rings than slots: they share one (only a back-off)
}
}  // namespace

bool UploadRing::AddChunk(uint64_t min_size) {
  // tw2de (2026-10-24): 32 MB (or the allocation's size), and when the heap cannot give that, 16 MB (a size freed
  // chunks are reused for): the heap had hundreds of MB free in pieces of less than 32 MB. The 32 MB try is skipped
  // for a while after it failed; the last size is always tried (without a chunk the draws lose their constants).
  ChunkBackoff &backoff = BackoffOf(this);
  const uint64_t sizes[2] = {std::max(kChunkSize, min_size), std::max<uint64_t>(16u << 20, min_size)};
  std::unique_ptr<plume::RenderBuffer> buffer;
  uint64_t size = 0;
  for (size_t i = 0; i < 2; ++i) {
    const uint64_t candidate = sizes[i];
    if (candidate == size)
      continue;
    size = candidate;
    if (i == 0 && backoff.resets_left && size >= backoff.failed_size)
      continue;
    // Guest shaders on Vulkan read their constants with vk::RawBufferLoad from
    // these allocations: CONSTANT is what gives plume's Vulkan buffer a device
    // address (see AddressableBufferFlags).
    buffer = device_->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        size, plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::VERTEX |
                  plume::RenderBufferFlag::INDEX | plume::RenderBufferFlag::DEVICE_ADDRESSABLE |
                  (host_cached_ ? plume::RenderBufferFlag::HOST_CACHED : plume::RenderBufferFlag::NONE)));
    if (buffer)
      break;
    if (i == 0) {
      backoff.failed_size = size;
      backoff.resets_left = kChunkBackoffResets;
    }
    static uint32_t logged = 0;
    if (logged < 16) {
      ++logged;
      RGPU_ERROR("Upload ring '{}': createBuffer({} bytes) failed", name_, size);
    }
  }
  if (!buffer)
    return false;
  void *mapped = buffer->map();
  if (!mapped) {
    RGPU_ERROR("Upload ring '{}': map failed", name_);
    return false;
  }
  buffer->setName(std::string(name_) + " #" + std::to_string(chunks_.size()));
  Chunk chunk;
  chunk.address = buffer->getDeviceAddress();
  chunk.mapped = static_cast<uint8_t *>(mapped);
  chunk.size = size;
  if (constant_sets_) {
    // One dynamic uniform buffer each, the layout of sets 5-7 of the draw layout (render/pipeline.cpp).
    auto make_set = [&](uint32_t range) {
      plume::RenderDescriptorSetBuilder builder;
      builder.begin();
      builder.addConstantBufferDynamic(0);
      builder.end();
      std::unique_ptr<plume::RenderDescriptorSet> set = builder.create(device_);
      if (set)
        set->setBuffer(0, buffer.get(), range);
      return set;
    };
    chunk.float_constants_set = make_set(kFloatConstantsUboBytes);
    chunk.shared_constants_set = make_set(kSharedConstantsUboBytes);
    if (!chunk.float_constants_set || !chunk.shared_constants_set) {
      RGPU_ERROR("Upload ring '{}': constant descriptor sets failed", name_);
      return false;
    }
  }
  chunk.buffer = std::move(buffer);
  chunks_.push_back(std::move(chunk));
  return true;
}

void UploadRing::Reset() {
  if (ChunkBackoff &backoff = BackoffOf(this); backoff.resets_left)
    --backoff.resets_left;  // tw2de (2026-10-24)
  window_chunks_ = std::max(window_chunks_, current_ + 1);
  // tw2de: tw2de_renut_memory_policy's trim interval (kTrimResets without it).
  if (++window_resets_ >= tw2de::renut_gpu::UploadTrimResets(kTrimResets, kFramesInFlight)) {
    // A large chunk made for one big allocation goes too; the first one stays.
    if (chunks_.size() > std::max(window_chunks_, kKeepChunks))
      chunks_.resize(std::max<size_t>(window_chunks_, kKeepChunks));  // tw2de (2026-11-05): kKeepChunks stay
    window_chunks_ = 1;
    window_resets_ = 0;
  }
  current_ = 0;
  offset_ = 0;
  for (Chunk &chunk : chunks_)
    chunk.written = chunk.published = 0;
}

void UploadRing::Publish() {
  if (!host_cached_)
    return;
  for (size_t i = 0; i <= current_ && i < chunks_.size(); ++i) {
    Chunk &chunk = chunks_[i];
    if (chunk.written > chunk.published) {
      // tw2de (2026-10-19, render-thread proposal 23): what the submit cleans, for the report (whether moving the
      // clean to another core is worth it once the constants are pushed).
      tw2de::renut_gpu::CountRingPublished(chunk.written - chunk.published);
      const plume::RenderRange range(chunk.published, chunk.written);
      chunk.buffer->unmap(0, &range);  // plume: flushes the range of a persistently mapped buffer
      chunk.published = chunk.written;
    }
  }
}

void UploadRing::HandOff(std::vector<Range> &out) {
  if (!host_cached_)
    return;
  for (size_t i = 0; i <= current_ && i < chunks_.size(); ++i) {
    Chunk &chunk = chunks_[i];
    if (chunk.written > chunk.published) {
      tw2de::renut_gpu::CountRingPublished(chunk.written - chunk.published);
      out.push_back({chunk.buffer.get(), chunk.published, chunk.written});
      chunk.published = chunk.written;
    }
  }
}

uint64_t UploadRing::Bytes() const {
  uint64_t bytes = 0;
  for (const Chunk &chunk : chunks_)
    bytes += chunk.size;
  return bytes;
}

UploadAllocation UploadRing::Allocate(uint64_t size, uint64_t alignment) {
  UploadAllocation out;
  if (size == 0 || chunks_.empty())
    return out;
  alignment = std::max<uint64_t>(alignment, 16);
  for (;;) {
    Chunk &chunk = chunks_[current_];
    const uint64_t aligned = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned + size <= chunk.size) {
      out.ref = plume::RenderBufferReference(chunk.buffer.get(), aligned);
      out.cpu = chunk.mapped + aligned;
      out.gpu_address = chunk.address ? chunk.address + aligned : 0;
      out.size = static_cast<uint32_t>(size);
      out.chunk_size = chunk.size;  // tw2de
      out.float_constants_set = chunk.float_constants_set.get();
      out.shared_constants_set = chunk.shared_constants_set.get();
      offset_ = aligned + size;
      chunk.written = offset_;
      return out;
    }
    // Next chunk, growing the ring if this frame outgrew every existing one.
    if (current_ + 1 >= chunks_.size() && !AddChunk(size))
      return out;
    ++current_;
    offset_ = 0;
  }
}

UploadAllocation UploadRing::AllocateWithRoom(uint64_t size, uint64_t room, uint64_t alignment) {
  UploadAllocation out;
  if (size == 0 || chunks_.empty())
    return out;
  room = std::max(room, size);
  alignment = std::max<uint64_t>(alignment, 16);
  for (;;) {
    Chunk &chunk = chunks_[current_];
    const uint64_t aligned = (offset_ + alignment - 1) & ~(alignment - 1);
    if (aligned + room <= chunk.size) {
      out.ref = plume::RenderBufferReference(chunk.buffer.get(), aligned);
      out.cpu = chunk.mapped + aligned;
      out.gpu_address = chunk.address ? chunk.address + aligned : 0;
      out.size = static_cast<uint32_t>(size);
      out.chunk_size = chunk.size;
      out.float_constants_set = chunk.float_constants_set.get();
      out.shared_constants_set = chunk.shared_constants_set.get();
      offset_ = aligned + size;
      chunk.written = offset_;
      return out;
    }
    if (current_ + 1 >= chunks_.size() && !AddChunk(room))
      return out;
    ++current_;
    offset_ = 0;
  }
}

UploadAllocation UploadRing::Upload(const void *data, uint64_t size, uint64_t alignment) {
  UploadAllocation out = Allocate(size, alignment);
  if (out && data)
    std::memcpy(out.cpu, data, size);
  return out;
}

} // namespace renut::gpu
