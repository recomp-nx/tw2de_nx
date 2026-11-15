// tw2de (2026-10-23): late specialisation. A shader's specialization constants read from a uniform buffer (the
// GLSL compiled once, without their values) become immediates in its TGSI, and the branches they decide are cut: the
// codegen of a variant from TGSI the front end made once (uam_api.h compileSpecialized).
#pragma once

#include <cstdint>

struct tgsi_token;

// The tokens with constant buffer 'buffer' read as values[] (vec4 slot k, component c: values[k * 4 + c]; past
// count: 0), the IF / UIF whose condition is then known cut down to the side taken; earlyZ: the early fragment tests
// property added (GLSL's layout(early_fragment_tests), which is all it changes in Mesa's output). A new token array (free() it), or
// nullptr. folded: the IFs cut.
tgsi_token* uam_tgsi_specialize(const tgsi_token* tokens, unsigned buffer, const uint32_t* values, unsigned count,
	unsigned* numTokens, unsigned* folded, bool earlyZ = false);
