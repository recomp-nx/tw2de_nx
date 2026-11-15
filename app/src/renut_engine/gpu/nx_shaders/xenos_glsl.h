// tw2de (2026-10-21): the Xbox 360 shader translation on the Switch itself. XenosRecomp's GLSL mode (the deko3d
// contract: shader_common_glsl.h, the tw2de-dk line) compiled into the NRO, so no shader the game ships is in the
// release: the pre-pass (shader_prepass.h) translates the containers it finds in the game files, the renderer the ones
// the game creates at run time (default.xex's), and uam compiles the GLSL on the console.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "renut_engine/gpu/shaders/shader_cache.h"

namespace renut::nxs {

struct TranslatedShader {
  uint64_t hash = 0;  // XXH3 of the container (virtual part, then physical part): the renderer's key
  bool pixel = false;
  ShaderCacheEntry entry{};  // the metadata the PC's shader cache has; dxil* zero, spirvSize = the GLSL's size
  std::vector<ShaderVertexInput> inputs;
  std::vector<ShaderLiteral> literals;
  uint32_t boolMask[8] = {};  // the boolean specialization constants (ids 1000..1255)
  std::string glsl;
};

// A container's size (virtual + physical) from its header, or 0 when it is not one XenosRecomp takes.
size_t ContainerSize(const uint8_t *container, size_t available);

// Translates one container (big-endian, as the game has it). Thread-safe (one recompiler per thread).
bool Translate(const uint8_t *container, size_t size, TranslatedShader &out);

// shader_common_glsl.h, which every translation has in it (the store's compression dictionary).
std::string_view GlslHeader();

// Changes when the translation does: the header's text and kTranslatorVersion. A store or a DKSH cache made by
// another translation is thrown away.
uint64_t TranslatorTag();

uint64_t Fnv1a64(const void *data, size_t size);

// The GLSL without the function definitions nothing in it reaches (xenos_glsl.cpp; Translate applies it).
std::string TrimUnusedFunctions(const std::string &glsl);

}  // namespace renut::nxs
