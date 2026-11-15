/**
 * @file    gpu/d3d/hooks/shader.cpp
 * @brief   Shader creation. The renderer owns the shader objects: each is a
 *          record keyed to its XenosRecomp cache entry by container hash.
 *
 * The ~150 shaders N&B compiles from HLSL at boot (hcg*ShaderCreateByHlsl)
 * reach D3DDevice_CreateVertexShader/CreatePixelShader too, so a cache miss
 * here is captured to disk for the next shader cache build.
 *
 * Shaders loaded with asset bundles are D3D objects the bundle already holds,
 * set up in place by XGRegisterVertexShader/XGRegisterPixelShader. Those stay
 * guest objects; the host side is kept per object address, keyed by the same
 * container hash.
 */
#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <xxhash.h>  // tw2de (2026-11-08)

#include "renut_engine/gpu/tw2de_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/guest/resources.h"
#include "renut_engine/gpu/render/pipeline.h"
#include "renut_engine/gpu/render/shader_library.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/backend/lookahead.h"  // TW2 (2026-11-14, tw2de_lookahead)
#include "renut_engine/gpu/tw2de_renut_tuning.h"  // tw2de (2026-11-05, seen18): the prefetch thread

using namespace renut::gpu;

namespace {

// Where D3DDevice_CreateVertexShader / CreatePixelShader (0x8264E8B0 /
// 0x8264E6A8) lay out a shader object: the container's virtual part copied
// after the D3D header, and a pointer to its physical part (literal constants
// and microcode). Objects in bundles have the same layout.
constexpr u32 kVertexShaderContainer = 0x368;
constexpr u32 kVertexShaderPhysical = 0x20;
constexpr u32 kPixelShaderContainer = 0x28;
constexpr u32 kPixelShaderPhysical = 0x18;

// Guest shader objects registered in place, by object address. The render
// thread's (State().mutex).
std::unordered_map<u32, std::unique_ptr<ShaderRecord>> g_guest_shaders;

// Recent FindShaderLocked results (two lookups a draw otherwise, one under a
// shared lock). An entry goes when its shader does (ForgetShader): both kinds
// are destroyed on the render thread, the only user, so a memo'd record is
// alive. (Checking the record registry's generation instead missed whenever
// any record came or went, which busy levels do all the time.) State().mutex.
// A miss is kept too (record null): draws keep binding addresses that are no
// shader of the kind (depth-only passes), and those cost a registry lookup and
// a map search a draw. A shader made at an address forgets it first (both are
// in order with the draws on the render thread). va 0 is an empty entry
// (FindShaderLocked never looks it up).
struct ShaderMemo {
  u32 va = 0;
  ShaderRecord *record = nullptr;
};
// A table per kind, a set of four entries per cache line, the most recent
// first. A single direct-mapped table for both kinds had the shaders of a busy
// level (Showdown Town binds hundreds a frame) evicting each other every frame,
// and each miss was a registry lookup and a map search.
constexpr u32 kShaderMemoWays = 4;
constexpr u32 kShaderMemoSetBits = 10;
struct alignas(64) ShaderMemoSet {
  ShaderMemo way[kShaderMemoWays];
};
static_assert(sizeof(ShaderMemoSet) == 64);
ShaderMemoSet g_shader_memo[2][1u << kShaderMemoSetBits];

u32 KindIndex(RecordKind kind) {
  return kind == RecordKind::PixelShader ? 1 : 0;
}

ShaderMemoSet &ShaderMemoSetFor(u32 kind_index, u32 shader_va) {
  return g_shader_memo[kind_index][(shader_va * 0x9E3779B1u) >> (32 - kShaderMemoSetBits)];
}

// The last shader found of each kind: consecutive draws mostly keep their
// shaders, and this is one compare where the memo is a cache miss.
ShaderMemo g_last_shader[2];

void ForgetShader(u32 shader_va) {
  for (u32 kind = 0; kind < 2; ++kind) {
    for (ShaderMemo &memo : ShaderMemoSetFor(kind, shader_va).way) {
      if (memo.va == shader_va)
        memo = {};
    }
    if (g_last_shader[kind].va == shader_va)
      g_last_shader[kind] = {};
  }
}

} // namespace

namespace renut::gpu {

// tw2de (2026-11-05, seen18): host shaders made when the game creates or registers a shader, on a background thread,
// not at its first draw. Run acc7f3ca: at an area entry the render thread spent 10.9 ms of one 113 ms frame in
// GetHostShader (the shaders the load created, each made at its first draw) while the game waited for it. The thread
// takes the records in order; GetHostShader still makes one itself when it comes first (both under the record's
// host_mutex: whichever is first builds it, the other finds it). CreateCachedShader is already called off the render
// thread by the pipeline prebuild workers. Lifetime: a record is taken off the queue and its mutex taken once before it
// is freed (DetachHostShaderPrefetch): the thread takes a record's host_mutex while it still holds the queue's lock, so
// after that nothing of it is touched again.
namespace {
class HostShaderPrefetch {
public:
  void Add(ShaderRecord *record) {
    if (!record || !record->entry)
      return;
    {
      std::lock_guard lock(mutex_);
      if (!started_) {
        started_ = true;
        tw2de::renut_gpu::StartLongLivedThread([] { Instance().Loop(); }, 64 << 10);
      }
      queue_.push_back(record);
    }
    wake_.notify_one();
  }
  void Forget(ShaderRecord *record) {
    std::lock_guard lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();)
      it = *it == record ? queue_.erase(it) : it + 1;
  }
  static HostShaderPrefetch &Instance() {
    static HostShaderPrefetch p;
    return p;
  }

private:
  void Loop() {
    tw2de::renut_gpu::OnBackgroundThreadStart("host shader prefetch");
    for (;;) {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [this] { return !queue_.empty(); });
      ShaderRecord *record = queue_.front();
      queue_.pop_front();
      std::unique_lock host_lock(record->host_mutex);
      lock.unlock();
      if (!record->host && record->entry) {
        if (plume::RenderDevice *device = Video::Device()) {
          auto host = CreateCachedShader(device, *record->entry);
          if (host) {  // a failure is left to GetHostShader (its log, and no retry every draw)
            record->host = std::move(host);
            record->host_ready.store(record->host.get(), std::memory_order_release);
          }
        }
      }
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<ShaderRecord *> queue_;
  bool started_ = false;
};

void PrefetchHostShader(ShaderRecord *record) {
  HostShaderPrefetch::Instance().Add(record);
}

// Before a record is freed: the prefetch thread neither has it queued nor is building it.
void DetachHostShaderPrefetch(ShaderRecord *record) {
  if (!record)
    return;
  HostShaderPrefetch::Instance().Forget(record);
  std::lock_guard host_lock(record->host_mutex);
}
}  // namespace

ShaderRecord *FindShaderLocked(u32 shader_va, RecordKind kind) {
  if (!shader_va)
    return nullptr;
  const u32 kind_index = KindIndex(kind);
  ShaderMemo &last = g_last_shader[kind_index];
  if (last.va == shader_va)
    return last.record;
  ShaderMemo *ways = ShaderMemoSetFor(kind_index, shader_va).way;
  u32 hit = 0;
  while (hit < kShaderMemoWays && ways[hit].va != shader_va)
    ++hit;
  ShaderMemo found;
  if (hit < kShaderMemoWays) {
    found = ways[hit];
  } else {
    hit = kShaderMemoWays - 1;  // the least recent goes
    ++rtw::g_shader_misses;  // tw2de (2026-11-06, seen20, [rtwork])
    ShaderRecord *record = FindRecord<ShaderRecord>(shader_va, kind);
    if (!record) {
      auto it = g_guest_shaders.find(shader_va);
      record = it != g_guest_shaders.end() && it->second->kind == kind ? it->second.get() : nullptr;
    }
    found = {shader_va, record};
  }
  for (u32 i = hit; i > 0; --i)
    ways[i] = ways[i - 1];
  ways[0] = found;
  last = found;
  return found.record;
}

void ReleaseGuestShaderLocked(u32 shader_va) {
  if (auto it = g_guest_shaders.find(shader_va); it != g_guest_shaders.end())
    DetachHostShaderPrefetch(it->second.get());  // tw2de (2026-11-05, seen18)
  if (g_guest_shaders.erase(shader_va))
    ForgetShader(shader_va);
}

void BuildDeclarationLayout(VertexDeclarationRecord &declaration) {
  declaration.layout =
      BuildHostVertexLayout(declaration.elements.data(), u32(declaration.elements.size()));
  // FNV-1a over what the input layout depends on.
  uint64_t hash = 0xCBF29CE484222325ull;
  auto mix = [&hash](uint64_t v) { hash = (hash ^ v) * 0x100000001B3ull; };
  for (const plume::RenderInputElement &e : declaration.layout.input_elements) {
    mix(e.location);
    mix(uint64_t(e.format));
    mix(e.slotIndex);
    mix(e.alignedByteOffset);
  }
  for (u32 s = 0; s < kMaxVertexStreams; ++s)
    mix(declaration.layout.streams[s].host_stride);
  declaration.layout_hash = hash;
}

const VertexDeclarationRecord *ShaderFetchDeclarationLocked(ShaderRecord &vs) {
  if (vs.fetch_declaration_built)
    return vs.fetch_declaration.get();
  vs.fetch_declaration_built = true;
  if (!vs.entry || !vs.entry->vertexInputCount)
    return nullptr;
  auto declaration = std::make_unique<VertexDeclarationRecord>();
  const ShaderVertexInput *inputs = g_shaderVertexInputs + vs.entry->vertexInputOffset;
  for (u32 i = 0; i < vs.entry->vertexInputCount; ++i) {
    const ShaderVertexInput &input = inputs[i];
    // Stream n's fetch constant is Xenos index 95 - n (d3d::StreamFetchIndex).
    const u32 first_stream_fetch = d3d::kFirstVertexFetchIndex;
    if (input.fetchIndex < first_stream_fetch || input.fetchIndex > 95)
      continue;
    const u32 stream = 95 - input.fetchIndex;
    const u32 type = FetchDeclType(input.format, input.flags & 1, input.flags & 2);
    if (type == kDeclTypeUnused)
      continue;
    GuestVertexElement element{};
    element.Stream = u16(stream);
    element.Offset = u16(input.offsetDwords * 4);
    element.Type = type;
    element.Method = 0;
    element.Usage = input.usage;
    element.UsageIndex = input.usageIndex;
    declaration->elements.push_back(element);
    if (stream < d3d::kMaxStreams && !vs.fetch_strides[stream])
      vs.fetch_strides[stream] = input.strideDwords * 4u;
  }
  if (declaration->elements.empty())
    return nullptr;
  BuildDeclarationLayout(*declaration);
  vs.fetch_declaration = std::move(declaration);
  return vs.fetch_declaration.get();
}

// tw2de (2026-11-08, real-time shadows): the shadow maps' casters are drawn with no vertex declaration set: the
// game bound their vertex shaders with D3DVertexShader_Bind (native, not hooked), which rewrites the shader's vertex
// fetches in its microcode (the physical part) for the declaration: format, stride and offset, in the declaration's
// order (the first a full fetch, the rest mini fetches), each keeping its element's destination register. The
// renderer's shader is translated from the unbound microcode (format, stride and offset 0), so those draws had no
// vertex input: every vertex at one point, nothing rasterized, the shadow maps all far (no real-time shadows).
// The object's layout, from the game's XGRegisterVertexShader (sub_82D091B8): the physical part's address at +32, bind
// slot 0 at +40 (word 0 -1 until bound), the unbound copies of the element fetches from +40 + 28, 12 bytes each, in
// element order; the container at +872, its shader at + shaderOffset (+24): microcode offset +0, field18 +24,
// element count +28, the elements from +36 + 4 field18 (address 12 bits, usage 4, usage index 4). Seen on the console
// (2026-11-08): copy (dst r1, r3, r4, r0) for elements at 8-11; bound microcode at 8-11: r3 full, r4, r1, r0 mini.
// Each element is matched to the bound fetch with its destination register.
const VertexDeclarationRecord *GuestBoundDeclarationLocked(ShaderRecord &vs) {
  if (vs.kind != RecordKind::VertexShader || !vs.self_va)
    return nullptr;
  const u8 *object = GuestPtr<const u8>(vs.self_va);
  if (!object)
    return nullptr;
  auto word = [](const u8 *p) { return u32(*reinterpret_cast<const be_u32 *>(p)); };
  const u8 *slot = object + 40;
  if (word(slot) == 0xFFFFFFFFu)
    return nullptr;  // never bound
  const u32 shader_offset = word(object + kVertexShaderContainer + 24);
  if (shader_offset < 0x24 || shader_offset > 0x10000)
    return nullptr;
  const u8 *shader = object + kVertexShaderContainer + shader_offset;
  const u32 field18 = word(shader + 24), count = word(shader + 28);
  if (count == 0 || count > 32 || field18 > 1024)
    return nullptr;
  const u32 physical_va = word(object + kVertexShaderPhysical);
  const u8 *physical = physical_va < 0x20000000u ? GuestPhysicalPtr<const u8>(physical_va)
                                                 : GuestPtr<const u8>(physical_va);
  if (!physical)
    return nullptr;
  const u8 *microcode = physical + word(shader);
  // The bound fetches at the elements' addresses, in address order: each one's fetch constant and stride (a mini
  // fetch takes the last full fetch's).
  struct Bound {
    u32 address, d0, d1, d2, fetch, stride;
  };
  Bound bound[32];
  for (u32 i = 0; i < count; ++i) {
    const u32 address = word(shader + 36 + 4 * (field18 + i)) & 0xFFF;
    const u8 *instr = microcode + 12 * address;
    bound[i] = {address, word(instr), word(instr + 4), word(instr + 8), 0, 0};
  }
  std::sort(bound, bound + count, [](const Bound &a, const Bound &b) { return a.address < b.address; });
  uint64_t key = XXH3_64bits(bound, sizeof(Bound) * count) ^ (uint64_t(physical_va) << 32) ^ count;
  if (vs.guest_bound_key == key)
    return vs.guest_bound_declaration.get();
  vs.guest_bound_key = key;
  vs.guest_bound_declaration.reset();
  std::memset(vs.guest_bound_strides, 0, sizeof(vs.guest_bound_strides));
  bool have_full = false;
  u32 fetch = 0, stride = 0;
  for (u32 i = 0; i < count; ++i) {
    if (!((bound[i].d1 >> 30) & 1)) {
      fetch = ((bound[i].d0 >> 20) & 0x1F) * 3 + ((bound[i].d0 >> 25) & 3);
      stride = bound[i].d2 & 0xFF;
      have_full = true;
    }
    bound[i].fetch = have_full ? fetch : 0;
    bound[i].stride = stride;
  }
  auto declaration = std::make_unique<VertexDeclarationRecord>();
  for (u32 i = 0; i < count; ++i) {
    const u32 element_word = word(shader + 36 + 4 * (field18 + i));
    const u32 dst = (word(slot + 28 + 12 * i) >> 12) & 0x3F;  // the element's register, from its unbound copy
    const Bound *b = nullptr;
    for (u32 j = 0; j < count && !b; ++j)
      if (((bound[j].d0 >> 12) & 0x3F) == dst)
        b = &bound[j];
    if (!b || b->fetch < d3d::kFirstVertexFetchIndex || b->fetch > 95)
      continue;
    u32 type = FetchDeclType((b->d1 >> 16) & 0x3F, (b->d1 >> 12) & 1, (b->d1 >> 13) & 1);
    if (type == kDeclTypeUnused)
      continue;
    // tw2de (2026-11-10, LR's GUI): the bound fetch's destination swizzle (dword 1 bits 0-11, 3 bits a component:
    // 0-3 a fetched component, 4 zero, 5 one, 7 not written), as the bind wrote it for the declaration's type (a
    // D3DCOLOR's byte order, for one): the declaration type carries it the same way. Without it the GUI's colours had
    // their channels rotated (the alpha taken from another byte: see-through dialog windows).
    for (u32 c = 0; c < 4; ++c) {
      const u32 sw = (b->d1 >> (3 * c)) & 7;
      if (sw != 7 && sw != 6)
        type = (type & ~(7u << (10 + 3 * c))) | (sw << (10 + 3 * c));
    }
    const int32_t offset = int32_t((b->d2 >> 8) << 9) >> 9;  // 23 bits, signed
    const u8 usage = u8((element_word >> 12) & 0xF), usage_index = u8((element_word >> 16) & 0xF);
    bool known = false;
    for (const GuestVertexElement &e : declaration->elements)
      known |= u8(e.Usage) == usage && u8(e.UsageIndex) == usage_index;
    if (known)
      continue;
    const u32 stream = 95 - b->fetch;
    GuestVertexElement element{};
    element.Stream = u16(stream);
    element.Offset = u16(offset * 4);
    element.Type = type;
    element.Method = 0;
    element.Usage = usage;
    element.UsageIndex = usage_index;
    declaration->elements.push_back(element);
    if (stream < d3d::kMaxStreams && !vs.guest_bound_strides[stream])
      vs.guest_bound_strides[stream] = b->stride * 4u;
  }
  static uint32_t logged = 0;
  if (logged++ < 12) {
    std::string line;
    for (const GuestVertexElement &e : declaration->elements)
      line += fmt::format(" [usage {} {} stream {} offset {} type {:08X}]", u32(e.Usage), u32(e.UsageIndex),
                          u32(e.Stream), u32(e.Offset), u32(e.Type));
    RGPU_INFO("[bind] vertex shader {:016X} at {:08X}: {} of {} elements from the game's bind:{}", vs.hash, vs.self_va,
              declaration->elements.size(), count, line);
  }
  if (declaration->elements.empty())
    return nullptr;
  BuildDeclarationLayout(*declaration);
  vs.guest_bound_declaration = std::move(declaration);
  return vs.guest_bound_declaration.get();
}

plume::RenderShader *GetHostShader(ShaderRecord *shader) {
  if (!shader || !shader->entry)
    return nullptr;
  // Built already: draws call this under State().mutex, which destruction
  // (DestroyShaderRecord) takes too, and only this function sets 'host'.
  if (plume::RenderShader *host = shader->host_ready.load(std::memory_order_acquire))
    return host;  // tw2de (2026-11-05, seen18): built (here or by the prefetch thread)
  std::lock_guard lock(shader->host_mutex);
  if (!shader->host) {
    if (!shader->entry)
      return nullptr;
    shader->host = CreateCachedShader(Video::Device(), *shader->entry);
    if (!shader->host) {
      RGPU_ERROR("Creating shader {:016X} failed", shader->hash);
      shader->entry = nullptr;  // don't retry every draw
      return nullptr;
    }
  }
  shader->host_ready.store(shader->host.get(), std::memory_order_release);
  return shader->host.get();
}

void DestroyVertexDeclarationRecord(VertexDeclarationRecord *declaration) {
  if (!declaration)
    return;
  std::lock_guard lock(State().mutex);
  ForgetVertexDeclarationLocked(declaration->self_va);
  DestroyRecord(declaration);
}

void DestroyShaderRecord(ShaderRecord *shader) {
  if (!shader)
    return;
  // Pipelines built from it keep their own compiled copy; only the module goes.
  {
    auto &s = State();
    std::lock_guard lock(s.mutex);
    ForgetShader(shader->self_va);
    HostShaderPrefetch::Instance().Forget(shader);  // tw2de (2026-11-05, seen18): then its mutex (below)
    std::lock_guard shader_lock(shader->host_mutex);
    shader->host_ready.store(nullptr, std::memory_order_release);
    shader->host.reset();
  }
  DestroyRecord(shader);
}

} // namespace renut::gpu

namespace {

u32 CreateShaderRecord(const be_u32 *function, bool pixel) {
  if (!function)
    return 0;
  const auto *container = reinterpret_cast<const uint8_t *>(function);
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  if ((u32(header->flags) & 0xFFFFFF00u) != 0x102A1100u) {
    RGPU_ERROR("Create{}Shader: not a shader container (flags {:08X})", pixel ? "Pixel" : "Vertex",
               u32(header->flags));
    return 0;
  }

  auto *shader = CreateRecord<ShaderRecord>(pixel ? RecordKind::PixelShader
                                                  : RecordKind::VertexShader);
  if (!shader)
    return 0;
  const auto type = pixel ? d3d::ResourceType::PixelShader : d3d::ResourceType::VertexShader;
  shader->x360.resource.Common = kCommonD3DCreated | u32(type);
  shader->x360.resource.ReferenceCount = 1u;

  shader->hash = HashShaderContainer(container);
  shader->fetch_filters = ScanFetchFilters(container);
  lookahead::NoteShaderFetches(shader->self_va, shader->fetch_filters.fetched);  // TW2 (2026-11-14, tw2de_lookahead)
  shader->entry = FindShaderEntry(shader->hash);
  if (!shader->entry)
    shader->entry = CaptureMissingShader(container, pixel, shader->hash);
  PipelineShaderLoaded(shader->entry);
  renut::gpu::PrefetchHostShader(shader);  // tw2de (2026-11-05, seen18)
  // Draws before it may have found no shader at the address (FindShaderLocked).
  render_thread::Queue([va = shader->self_va] {
    const ::renut::gpu::rtw::Scope rtw_misc(::renut::gpu::rtw::kMiscItems);  // tw2de (2026-11-05, [rtwork])
    ForgetShader(va);
  });
  return shader->self_va;
}

// The host side of a shader object the guest set up in place: its container is
// the virtual part inside the object followed by the physical part.
void RegisterGuestShader(u32 shader_va, u32 physical_va, bool pixel) {
  const u32 container_va = shader_va + (pixel ? kPixelShaderContainer : kVertexShaderContainer);
  const auto *header = GuestPtr<const d3d::ShaderContainer>(container_va);
  const u8 *physical = physical_va < 0x20000000u ? GuestPhysicalPtr<const u8>(physical_va)
                                                 : GuestPtr<const u8>(physical_va);
  if (!header || !physical)
    return;
  const u32 flags = header->flags;
  const u32 virtual_size = header->virtualSize;
  const u32 physical_size = header->physicalSize;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || ((flags & 1) != 0) == pixel ||
      virtual_size < sizeof(d3d::ShaderContainer) || virtual_size > 0x10000 ||
      physical_size == 0 || physical_size > 0x100000) {
    RGPU_ERROR("XGRegister{}Shader {:08X}: no shader container at {:08X} (flags {:08X})",
               pixel ? "Pixel" : "Vertex", shader_va, container_va, flags);
    return;
  }
  std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
  std::memcpy(container.data(), header, virtual_size);
  std::memcpy(container.data() + virtual_size, physical, physical_size);
  const uint64_t hash = HashShaderContainer(container.data());
  const RecordKind kind = pixel ? RecordKind::PixelShader : RecordKind::VertexShader;
  // Captured here, where its container is at hand, only when the cache lacks it.
  if (!FindShaderEntry(hash))
    CaptureMissingShader(container.data(), pixel, hash);

  const FetchFilterOverrides fetch_filters = ScanFetchFilters(container.data());  // tw2de (2026-10-31)
  lookahead::NoteShaderFetches(shader_va, fetch_filters.fetched);  // TW2 (2026-11-14, tw2de_lookahead)
  // In order with the draws that use it (render thread).
  render_thread::Queue([shader_va, pixel, hash, kind, fetch_filters] {
    const ::renut::gpu::rtw::Scope rtw_shader(::renut::gpu::rtw::kShaderMake);  // tw2de (2026-11-05, [rtwork])
    auto &shader = g_guest_shaders[shader_va];
    if (shader && shader->hash == hash && shader->kind == kind)
      return;
    // A different shader in the same memory: pipelines keep their own compiled
    // copy of the old one, so its module can go now. Draws before it may also
    // have found none there (FindShaderLocked).
    ForgetShader(shader_va);
    DetachHostShaderPrefetch(shader.get());  // tw2de (2026-11-05, seen18): before the old record goes
    shader = std::make_unique<ShaderRecord>();
    shader->kind = kind;
    shader->self_va = shader_va;
    shader->hash = hash;
    shader->fetch_filters = fetch_filters;
    shader->entry = FindShaderEntry(hash);
    PipelineShaderLoaded(shader->entry);
    PrefetchHostShader(shader.get());  // tw2de (2026-11-05, seen18)
  });
}

// void XGRegisterVertexShader(D3DVertexShader *pShader, void *pPhysicalPart) [0x8264E7B8]
// As the original: the header (a vertex shader of one reference), and for
// each of its bind slots (two when the container's flag 0x20 is set) the
// vertex fetch instructions copied from the microcode into the slot.
// tw2de: the game's own XGRegisterVertexShader / XGRegisterPixelShader set the object up (the same code as N&B's,
// sub_82D091B8 / sub_82D08828), then the renderer registers it.
REX_EXTERN(__imp__sub_82D091B8);
REX_EXTERN(__imp__sub_82D08828);
void XGRegisterVertexShader_hook(u32 shader, u32 physical) {
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D091B8, shader, physical);
  RegisterGuestShader(shader, physical, false);
}

void XGRegisterPixelShader_hook(u32 shader, u32 physical) {
  rex::ppc::GuestToHostFunction<void>(__imp__sub_82D08828, shader, physical);
  RegisterGuestShader(shader, physical, true);
}

// D3DVertexShader *D3DDevice_CreateVertexShader(const DWORD *pFunction) [0x8264E8B0]
u32 D3DDevice_CreateVertexShader_hook(mapped_u32 function) {
  return CreateShaderRecord(static_cast<const be_u32 *>(function), false);
}

// D3DPixelShader *D3DDevice_CreatePixelShader(const DWORD *pFunction) [0x8264E6A8]
u32 D3DDevice_CreatePixelShader_hook(mapped_u32 function) {
  return CreateShaderRecord(static_cast<const be_u32 *>(function), true);
}

// D3DVertexDeclaration *D3DDevice_CreateVertexDeclaration(
//     const D3DVERTEXELEMENT9 *pVertexElements) [0x8264EA90]
u32 D3DDevice_CreateVertexDeclaration_hook(rex::MappedPtr<GuestVertexElement> elements) {
  if (!elements)
    return 0;
  auto *declaration = CreateRecord<VertexDeclarationRecord>(RecordKind::VertexDeclaration);
  if (!declaration)
    return 0;
  declaration->x360.resource.Common =
      kCommonD3DCreated | u32(d3d::ResourceType::VertexDeclaration);
  declaration->x360.resource.ReferenceCount = 1u;

  const GuestVertexElement *element = elements;
  for (u32 i = 0; i < 64 && u32(element[i].Type) != kDeclTypeUnused; ++i)
    declaration->elements.push_back(element[i]);
  BuildDeclarationLayout(*declaration);
  return declaration->self_va;
}

// void D3DVertexShader_Bind(D3DVertexShader *pShader, DWORD Flags,
//     D3DVertexDeclaration *pDecl, const DWORD *pStreamStrides, DWORD Index) [0x82657048]
// Patches a CVertexShader's vertex fetches to a declaration and the strides of
// streams 0..(highest stream in the declaration). Draws take their input layout
// from a declaration record instead, so nothing is patched; the declaration and
// strides are kept for draws that rely on the bind: N&B binds its shaders when
// it creates them and then draws with no declaration set, or with a zero
// SetStreamSource stride.
void D3DVertexShader_Bind_hook(u32 shader_va, u32 /*flags*/, u32 declaration_va,
                               mapped_u32 stream_strides, u32 /*index*/) {
  // The declaration and strides as they are now; the shader record is the
  // render thread's (draws read it), so it is updated there, in order.
  const auto *declaration =
      FindRecord<VertexDeclarationRecord>(declaration_va, RecordKind::VertexDeclaration);
  if (!declaration)
    return;
  auto bound = std::make_shared<VertexDeclarationRecord>(*declaration);
  std::vector<std::pair<u32, u32>> strides;  // stream, stride
  if (stream_strides) {
    const auto *guest_strides = static_cast<const be_u32 *>(stream_strides);
    for (const GuestVertexElement &element : declaration->elements) {
      const u32 stream = element.Stream;
      if (stream < d3d::kMaxStreams)
        strides.emplace_back(stream, u32(guest_strides[stream]));
    }
  }
  render_thread::Queue([shader_va, bound, strides = std::move(strides)] {
    const ::renut::gpu::rtw::Scope rtw_misc(::renut::gpu::rtw::kMiscItems);  // tw2de (2026-11-05, [rtwork])
    auto *shader = FindShaderLocked(shader_va, RecordKind::VertexShader);
    if (!shader)
      return;
    shader->bound_declaration = std::make_unique<VertexDeclarationRecord>(*bound);
    for (const auto &[stream, stride] : strides)
      shader->bound_strides[stream] = stride;
  });
}

} // namespace

// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_8264E8B0, D3DDevice_CreateVertexShader_hook);  // D3DDevice_CreateVertexShader
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_8264E6A8, D3DDevice_CreatePixelShader_hook);  // D3DDevice_CreatePixelShader
// tw2de: not hooked (native objects; the draw reads them, NativeDeclarationLocked) // TW2DE_RENUT_HOOK(sub_826C8140, D3DDevice_CreateVertexDeclaration_hook)  /* N&B sub_8264EA90 */;  // D3DDevice_CreateVertexDeclaration
// tw2de: not hooked (no match in the game): TW2DE_RENUT_HOOK(sub_82657048, D3DVertexShader_Bind_hook);  // D3DVertexShader_Bind
// tw2de: XGRegisterVertexShader (N&B sub_8264E7B8) / XGRegisterPixelShader (N&B sub_8264E670). With the native
// renderer, the game's own code runs and the shader is still captured (tw2de_capture_shaders_native), so a play
// session on the PC's reference renderer collects the shader containers the Switch package is built from.
REXCVAR_DEFINE_BOOL(tw2de_capture_shaders_native, true, "tw2de",
                    "With tw2de_renderer = native, still write each new shader container to renut_shader_capture_dir")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
namespace {
void CaptureRegisteredShader(u32 shader_va, u32 physical_va, bool pixel) {
  using namespace renut::gpu;
  const u32 container_va = shader_va + (pixel ? kPixelShaderContainer : kVertexShaderContainer);
  const auto *header = GuestPtr<const d3d::ShaderContainer>(container_va);
  const u8 *physical = physical_va < 0x20000000u ? GuestPhysicalPtr<const u8>(physical_va)
                                                 : GuestPtr<const u8>(physical_va);
  if (!header || !physical)
    return;
  const u32 flags = header->flags, virtual_size = header->virtualSize, physical_size = header->physicalSize;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || ((flags & 1) != 0) == pixel ||
      virtual_size < sizeof(d3d::ShaderContainer) || virtual_size > 0x10000 || physical_size == 0 ||
      physical_size > 0x100000)
    return;
  std::vector<uint8_t> container(size_t(virtual_size) + physical_size);
  std::memcpy(container.data(), header, virtual_size);
  std::memcpy(container.data() + virtual_size, physical, physical_size);
  const uint64_t hash = HashShaderContainer(container.data());
  if (!FindShaderEntry(hash))
    CaptureMissingShader(container.data(), pixel, hash);
}
}  // namespace
extern "C" REX_FUNC(sub_82D091B8) {
  if (::tw2de::renut_gpu::Active()) {
    ::rex::ppc::HostToGuestFunction<XGRegisterVertexShader_hook>(ctx, base);
    return;
  }
  const u32 shader = ctx.r3.u32, physical = ctx.r4.u32;
  __imp__sub_82D091B8(ctx, base);
  if (REXCVAR_GET(tw2de_capture_shaders_native))
    CaptureRegisteredShader(shader, physical, false);
}
extern "C" REX_FUNC(sub_82D08828) {
  if (::tw2de::renut_gpu::Active()) {
    ::rex::ppc::HostToGuestFunction<XGRegisterPixelShader_hook>(ctx, base);
    return;
  }
  const u32 shader = ctx.r3.u32, physical = ctx.r4.u32;
  __imp__sub_82D08828(ctx, base);
  if (REXCVAR_GET(tw2de_capture_shaders_native))
    CaptureRegisteredShader(shader, physical, true);
}
