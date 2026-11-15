// tw2de (2026-10-23): late specialisation. A run-time variant of a shader (its specialization constants given
// other values than the defaults) was compiled from its GLSL with the values defined in front: Mesa's whole front end
// again for every variant, two thirds of a fast pass. Now the pre-pass runs the front end once per shader on a form
// of its GLSL whose constants are read from a uniform buffer (LateSpecBase) and keeps the result (front_end_store.h);
// a variant is compiled from that with the values filled in and the branches they decide cut (uam's
// compileSpecialized, tgsi_spec.h): the codegen alone, about a quarter of the work. The PC tool over 800 variants of
// 400 of the game's shaders: 2.5% fewer instructions than compiling each variant from its GLSL, and every variant gives
// the same pixels as its shader with the constants as uniforms (a TGSI interpreter over random pixels).
//
// The constants: "const uint NAME = SPIRV_CROSS_CONSTANT_ID_<id>;" (xenos_glsl / XenosRecomp) becomes
// "#define NAME (dk_spec.v[slot >> 2].<xyzw>)", slot id - 900 for 900..915 and 16 + id - 1000 for the booleans
// (1000..1303), in "layout(binding = 13, std140) uniform DkSpec { uvec4 v[80]; } dk_spec;" (TGSI's constant buffer
// 14). Constant 909 (early depth test: an #ifdef in the text) is not one of them: a variant with it gets the early
// fragment tests from the specialisation (the only thing it changes in Mesa's output). 910 (32 registers at most) is
// uam's own (a GPR cap).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace renut::nxs {

constexpr unsigned kLateSpecBinding = 13;  // the GLSL's uniform binding; uam's TGSI buffer is this + 1
constexpr unsigned kLateSpecSlots = 320;   // vec4 v[80]

// The base form of a shader's GLSL; its constants' default values by slot. False when the text has a constant used
// some other way than through its const (the shader then has no late path: its variants compile from GLSL).
bool LateSpecBase(const std::string &glsl, std::string &base, std::vector<std::pair<uint32_t, uint32_t>> &slot_defaults);

// What the store keeps for a shader: the slot defaults and uam's front-end bytes.
void LateSpecPack(const std::vector<std::pair<uint32_t, uint32_t>> &slot_defaults, const std::vector<uint8_t> &front_end,
                  std::vector<uint8_t> &out);

// A variant's constants (id, value: those that differ from the default) as the buffer's slots, from a packed blob.
// False when the variant has a constant the late path cannot give (the caller compiles it from GLSL).
struct LateSpecInputs {
  std::vector<uint32_t> slots;  // kLateSpecSlots words
  bool early_z = false;
  int gpr_cap = 0;
  const uint8_t *front_end = nullptr;
  size_t front_end_size = 0;
};
bool LateSpecPrepare(const std::vector<uint8_t> &blob, const std::vector<std::pair<uint32_t, uint32_t>> &values,
                     LateSpecInputs &in);

}  // namespace renut::nxs
