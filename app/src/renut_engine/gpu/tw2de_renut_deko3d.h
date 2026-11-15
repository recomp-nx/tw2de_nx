// tw2de (2026-10-17): Renut's renderer on plume's deko3d back end (renut_gpu_backend = "deko3d",
// docs/deko3d-renderer.md). The game shaders' GLSL from the shader store the console translates (nx_shaders/), and the
// DKSH cache of compiled variants on the SD card.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plume {
struct RenderDevice;
struct RenderShader;
}  // namespace plume

namespace tw2de::renut_gpu::deko3d {

// The deko3d shader (plume::Deko3DShaderBlob) of a game shader by its container hash, or null (logged once).
std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *device, uint64_t hash);

// The boolean specialization constants (ids 1000..1255) a game shader has, as ShaderBoolSpecMask gives them.
bool BoolSpecMask(uint64_t hash, uint32_t mask[8]);

// The DKSH cache next to the NRO (deko3d_shaders.bin): what it holds is read at start; variants the console compiles
// are added to it (written by Save, and by a periodic save while the game runs). Installed into plume before the
// device is made.
void InstallShaderCache();
void SaveShaderCache();

// tw2de (2026-10-21): the shader pre-pass's view of the DKSH cache (opened by whichever comes first).
bool CacheHas(uint64_t key);
// tw2de (2026-10-21): the key's record is a fast pass's (compiled in play), which the pre-pass compiles again.
bool CacheIsFast(uint64_t key);
void CacheAdd(uint64_t key, const std::vector<uint8_t> &dksh);

// One line for the renderer's report: compiles, cache hits, memory.
std::string Report();

}  // namespace tw2de::renut_gpu::deko3d
