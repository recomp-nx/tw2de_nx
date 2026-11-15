/**
 * @file    gpu/render/shader_library.cpp
 */
#include "renut_engine/gpu/render/shader_library.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
REXCVAR_DECLARE(bool, log_diagnostics);  // tw2de (2026-10-25): sdk/src/system/xobject.cpp
#include <smolv.h>
#include <xxhash.h>
#include <zstd.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/tw2de_renut_tuning.h"
#if defined(PLUME_DEKO3D)
#include "renut_engine/gpu/tw2de_renut_deko3d.h"  // tw2de (2026-10-17): the deko3d back end's GLSL package
#endif
#if defined(RENUT_SHADER_STORE)
#include "renut_engine/gpu/nx_shaders/shader_store.h"  // tw2de (2026-10-21): the shaders the console translated
#include "renut_engine/gpu/nx_shaders/container_cache.h"  // tw2de (2026-10-23)
#include "renut_engine/gpu/nx_shaders/xenos_glsl.h"
#endif

REXCVAR_DEFINE_STRING(renut_shader_capture_dir, "shader_capture", "Nuts&Bolts/Graphics",
                      "Where shaders missing from the shader cache are written; the next build "
                      "recompiles them into the cache");

namespace renut::gpu {

namespace {

struct DecompressedCache {
  std::once_flag once;
  std::vector<uint8_t> bytes;
};

DecompressedCache g_spirv;
#if defined(_WIN32)
DecompressedCache g_dxil;
#endif
std::atomic<uint64_t> g_decompressed_bytes{0};  // ShaderCacheHostBytes

const uint8_t *Decompress(DecompressedCache &cache, const uint8_t *compressed,
                          size_t compressed_size, size_t decompressed_size, const char *name) {
  std::call_once(cache.once, [&] {
    cache.bytes.resize(decompressed_size);
    const size_t n = ZSTD_decompress(cache.bytes.data(), decompressed_size, compressed,
                                     compressed_size);
    if (ZSTD_isError(n) || n != decompressed_size) {
      RGPU_ERROR("{} shader cache decompression failed", name);
      cache.bytes.clear();
      cache.bytes.shrink_to_fit();
    }
    g_decompressed_bytes.fetch_add(cache.bytes.capacity(), std::memory_order_relaxed);
  });
  return cache.bytes.empty() ? nullptr : cache.bytes.data();
}

std::mutex g_capture_mutex;
std::unordered_set<uint64_t> g_captured;  // CaptureMissingShader

} // namespace

} // namespace renut::gpu

#if defined(RENUT_SHADER_CACHE_FILE)
ShaderCacheEntry *g_shaderCacheEntries = nullptr;
size_t g_shaderCacheEntryCount = 0;
ShaderVertexInput *g_shaderVertexInputs = nullptr;
ShaderLiteral *g_shaderLiterals = nullptr;
uint32_t g_shaderCacheLayout = 0;
const uint8_t *g_compressedDxilCache = nullptr;
size_t g_dxilCacheCompressedSize = 0;
size_t g_dxilCacheDecompressedSize = 0;
const uint8_t *g_compressedSpirvCache = nullptr;
size_t g_spirvCacheCompressedSize = 0;
size_t g_spirvCacheDecompressedSize = 0;

REXCVAR_DEFINE_STRING(renut_shader_cache_file, "renut_shaders.bin", "Nuts&Bolts/Graphics",
                      "tw2de: the shader cache (next to the executable unless absolute), made by the build "
                      "(tools/renut_shader_pack.cpp)");

namespace {
std::vector<uint8_t> g_pack;  // the whole file; the globals point into it
}  // namespace

bool LoadShaderCacheFile() {
  static std::once_flag once;
  static bool loaded = false;
  std::call_once(once, [] {
    const std::filesystem::path path =
        tw2de::renut_gpu::ShaderDataPath(REXCVAR_GET(renut_shader_cache_file));
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
      RGPU_ERROR("Shader cache {} not found: every shader is missing (put the build's renut_shaders.bin next to "
                 "the executable)", path.string());
      return;
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    // 8-byte aligned storage for the entries' 64-bit hashes.
    g_pack.resize(size_t(size) + 8);
    const size_t base = (8 - (reinterpret_cast<uintptr_t>(g_pack.data()) & 7)) & 7;
    uint8_t *const data = g_pack.data() + base;
    if (!in.read(reinterpret_cast<char *>(data), size) || size_t(size) < sizeof(ShaderPackHeader)) {
      RGPU_ERROR("Shader cache {}: could not be read", path.string());
      return;
    }
    ShaderPackHeader h;
    std::memcpy(&h, data, sizeof(h));
    if (std::memcmp(h.magic, "RNSHPAK1", 8) != 0 || h.entrySize != sizeof(ShaderCacheEntry) ||
        h.vertexInputSize != sizeof(ShaderVertexInput) || h.literalSize != sizeof(ShaderLiteral)) {
      RGPU_ERROR("Shader cache {}: not a shader pack of this renderer", path.string());
      return;
    }
    size_t at = (sizeof(h) + 7) & ~size_t(7);
    auto take = [&](size_t bytes) -> uint8_t * {
      if (at + bytes > size_t(size))
        return nullptr;
      uint8_t *p = data + at;
      at = (at + bytes + 7) & ~size_t(7);
      return p;
    };
    auto *entries = take(size_t(h.entryCount) * sizeof(ShaderCacheEntry));
    auto *inputs = take(size_t(h.vertexInputCount) * sizeof(ShaderVertexInput));
    auto *literals = take(size_t(h.literalCount) * sizeof(ShaderLiteral));
    auto *dxil = take(size_t(h.dxilCompressedSize));
    auto *spirv = take(size_t(h.spirvCompressedSize));
    if (!entries || !inputs || !literals || !dxil || !spirv) {
      RGPU_ERROR("Shader cache {}: truncated", path.string());
      return;
    }
    g_shaderCacheEntries = reinterpret_cast<ShaderCacheEntry *>(entries);
    g_shaderCacheEntryCount = h.entryCount;
    g_shaderVertexInputs = reinterpret_cast<ShaderVertexInput *>(inputs);
    g_shaderLiterals = reinterpret_cast<ShaderLiteral *>(literals);
    g_compressedDxilCache = dxil;
    g_dxilCacheCompressedSize = size_t(h.dxilCompressedSize);
    g_dxilCacheDecompressedSize = size_t(h.dxilDecompressedSize);
    g_compressedSpirvCache = spirv;
    g_spirvCacheCompressedSize = size_t(h.spirvCompressedSize);
    g_spirvCacheDecompressedSize = size_t(h.spirvDecompressedSize);
    g_shaderCacheLayout = h.layout;
    loaded = true;
    RGPU_INFO("Shader cache {}: {} shaders, {:.1f} MB", path.string(), h.entryCount, double(size) / (1 << 20));
  });
  return loaded;
}
#endif

#if defined(RENUT_SHADER_STORE)
// tw2de (2026-10-21): the Switch reads its shaders from the store (nx_shaders/shader_store.h): the pre-pass fills
// it from the game files before the game starts, and CaptureMissingShader translates the ones the game creates at run
// time into it. No shader of the game is in the release.
ShaderCacheEntry *g_shaderCacheEntries = nullptr;
size_t g_shaderCacheEntryCount = 0;
ShaderVertexInput *g_shaderVertexInputs = nullptr;
ShaderLiteral *g_shaderLiterals = nullptr;
uint32_t g_shaderCacheLayout = 0;
// No DXIL or SPIR-V on the Switch (deko3d takes the store's GLSL).
const uint8_t *g_compressedDxilCache = nullptr;
size_t g_dxilCacheCompressedSize = 0;
size_t g_dxilCacheDecompressedSize = 0;
const uint8_t *g_compressedSpirvCache = nullptr;
size_t g_spirvCacheCompressedSize = 0;
size_t g_spirvCacheDecompressedSize = 0;

bool LoadShaderCacheFile() {
  static std::once_flag once;
  static bool loaded = false;
  std::call_once(once, [] {
    auto &store = renut::nxs::ShaderStore::Get();
    const std::string path = tw2de::renut_gpu::ShaderDataPath("tw2de_shaders.bin").string();
    if (!store.Open(path)) {
      RGPU_ERROR("Shader store {} cannot be opened: every shader is missing", path);
      return;
    }
    g_shaderVertexInputs = const_cast<ShaderVertexInput *>(store.VertexInputs());
    g_shaderLiterals = const_cast<ShaderLiteral *>(store.Literals());
    g_shaderCacheEntryCount = store.Count();
    g_shaderCacheLayout = kShaderCacheLayout;
    loaded = true;
    if (REXCVAR_GET(log_diagnostics)) RGPU_INFO("Shader store {}: {} shaders", path, store.Count());
  });
  return loaded;
}
#endif

namespace renut::gpu {

uint64_t HashShaderContainer(const uint8_t *container) {
  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  return XXH3_64bits(container, size);
}

const ShaderCacheEntry *FindShaderEntry(uint64_t hash) {
#if defined(RENUT_SHADER_STORE)
  if (!LoadShaderCacheFile())
    return nullptr;
  const renut::nxs::StoredShader *stored = renut::nxs::ShaderStore::Get().Find(hash);
  return stored ? &stored->entry : nullptr;
#endif
#if defined(RENUT_SHADER_CACHE_FILE)
  if (!LoadShaderCacheFile())
    return nullptr;
#endif
  if (g_shaderCacheLayout != kShaderCacheLayout) {
    static bool logged = false;
    if (!logged) {
      RGPU_ERROR("Shader cache layout {} does not match the renderer's {}; rebuild it",
                 g_shaderCacheLayout, kShaderCacheLayout);
      logged = true;
    }
    return nullptr;
  }
  const ShaderCacheEntry *begin = g_shaderCacheEntries;
  const ShaderCacheEntry *end = begin + g_shaderCacheEntryCount;
  const ShaderCacheEntry *it = std::lower_bound(
      begin, end, hash, [](const ShaderCacheEntry &e, uint64_t h) { return e.hash < h; });
  return (it != end && it->hash == hash) ? it : nullptr;
}

void ShaderBoolSpecMask(const ShaderCacheEntry &entry, uint32_t mask[8]) {
  std::fill(mask, mask + 8, 0u);
#if defined(PLUME_DEKO3D)
  // tw2de: the GLSL package lists them (the SPIR-V cache need not be unpacked for deko3d).
  if (IsDeko3D()) {
    tw2de::renut_gpu::deko3d::BoolSpecMask(entry.hash, mask);
    return;
  }
#endif
  if (!IsVulkan() || entry.spirvSize == 0)
    return;
  const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                    g_spirvCacheDecompressedSize, "SPIR-V");
  if (!cache)
    return;
  const uint8_t *smol = cache + entry.spirvOffset;
  std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
  if (spirv.size() < 20 || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size()))
    return;
  // After the five header words, each instruction's first word is (word count << 16) | opcode. OpDecorate (71):
  // target, decoration, literals; SpecId is decoration 1.
  const auto *words = reinterpret_cast<const uint32_t *>(spirv.data());
  const size_t count = spirv.size() / 4;
  for (size_t i = 5; i < count;) {
    const uint32_t length = words[i] >> 16, opcode = words[i] & 0xFFFF;
    if (length == 0)
      break;
    if (opcode == 71 && length >= 4 && i + 3 < count && words[i + 2] == 1) {
      const uint32_t id = words[i + 3];
      if (id >= 1000 && id < 1256)
        mask[(id - 1000) / 32] |= 1u << ((id - 1000) % 32);
    }
    i += length;
  }
}

std::unique_ptr<plume::RenderShader> CreateCachedShader(plume::RenderDevice *device,
                                                        const ShaderCacheEntry &entry) {
  if (!device)
    return nullptr;

#if defined(PLUME_DEKO3D)
  // tw2de (2026-10-17): the deko3d back end's GLSL for uam, from the package in the NRO.
  if (IsDeko3D())
    return tw2de::renut_gpu::deko3d::CreateGameShader(device, entry.hash);
#endif

  if (IsVulkan()) {
    const uint8_t *cache = Decompress(g_spirv, g_compressedSpirvCache, g_spirvCacheCompressedSize,
                                      g_spirvCacheDecompressedSize, "SPIR-V");
    if (!cache || entry.spirvSize == 0)
      return nullptr;
    const uint8_t *smol = cache + entry.spirvOffset;
    std::vector<uint8_t> spirv(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (spirv.empty() || !smolv::Decode(smol, entry.spirvSize, spirv.data(), spirv.size())) {
      RGPU_ERROR("SPIR-V decode failed for shader {:016X}", entry.hash);
      return nullptr;
    }
    return device->createShader(spirv.data(), spirv.size(), "main",
                                plume::RenderShaderFormat::SPIRV);
  }

#if defined(_WIN32)
  const uint8_t *cache = Decompress(g_dxil, g_compressedDxilCache, g_dxilCacheCompressedSize,
                                    g_dxilCacheDecompressedSize, "DXIL");
  if (!cache || entry.dxilSize == 0)
    return nullptr;
  return device->createShader(cache + entry.dxilOffset, entry.dxilSize, "main",
                              plume::RenderShaderFormat::DXIL);
#else
  return nullptr;
#endif
}

uint64_t ShaderCacheHostBytes() {
  return g_decompressed_bytes.load(std::memory_order_relaxed);
}

const ShaderCacheEntry *CaptureMissingShader(const uint8_t *container, bool pixel_shader, uint64_t hash) {
#if defined(RENUT_SHADER_STORE)
  // tw2de (2026-10-21): translated here and now (default.xex's shaders, which no game file has), into the store
  // for the next starts; uam compiles it when a draw first needs it.
  if (LoadShaderCacheFile()) {
    auto &store = renut::nxs::ShaderStore::Get();
    if (const renut::nxs::StoredShader *have = store.Find(hash))
      return &have->entry;
    const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
    const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
    {
      std::lock_guard lock(g_capture_mutex);
      if (g_captured.count(hash))
        return nullptr;  // it did not translate before
    }
    // tw2de (2026-10-23): kept for the next translation too (nx_shaders/container_cache.h).
    {
      auto &cache = renut::nxs::ContainerCache::Get();
      if (cache.Open(tw2de::renut_gpu::ShaderDataPath("tw2de_shader_containers").string())) {
        cache.Add(hash, container, size);
        cache.Sync();
      }
    }
    renut::nxs::TranslatedShader t;
    if (renut::nxs::Translate(container, size, t)) {
      if (const renut::nxs::StoredShader *added = store.Add(t)) {
        store.Sync();
        RGPU_INFO("Shader {:016X} ({}) translated at run time into the shader store", hash, pixel_shader ? "pixel" : "vertex");
        return &added->entry;
      }
    }
    std::lock_guard lock(g_capture_mutex);
    g_captured.insert(hash);
    RGPU_ERROR("Shader {:016X} ({}) did not translate", hash, pixel_shader ? "pixel" : "vertex");
    return nullptr;
  }
#endif
  std::lock_guard lock(g_capture_mutex);
  if (!g_captured.insert(hash).second)
    return nullptr;

  const auto *header = reinterpret_cast<const d3d::ShaderContainer *>(container);
  const size_t size = size_t(u32(header->virtualSize)) + u32(header->physicalSize);
  // tw2de: next to the executable (on the Switch, the NRO's folder), not the working directory.
  const std::filesystem::path dir = tw2de::renut_gpu::NextToExecutable(REXCVAR_GET(renut_shader_capture_dir));
  const std::filesystem::path path =
      dir / fmt::format("{}_{:016x}.bin", pixel_shader ? "ps" : "vs", hash);

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(path, std::ios::binary);
  if (out)
    out.write(reinterpret_cast<const char *>(container), std::streamsize(size));
  if (!out) {
    RGPU_WARN("Shader {:016X} is not in the shader cache and could not be captured to {}", hash,
              path.string());
    return nullptr;
  }
  RGPU_WARN("Shader {:016X} is not in the shader cache: captured to {}, the next build adds it",
            hash, path.string());
  return nullptr;
}

} // namespace renut::gpu
