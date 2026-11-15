// tw2de (2026-10-23): a deko3d GLSL shader's specialization constants and their defaults, as plume's loadVariant
// and the pre-pass's variant list (nx_shaders/variant_list.cpp VariantSource) both read them (the same, or the keys
// differ).
//
// Every "#define SPIRV_CROSS_CONSTANT_ID_<id> <value>" in a part of the text the stage compiles, sorted by id. The
// shaders' common header (XenosRecomp shader_common_glsl.h) has both stages' constants, each in an
// "#ifdef TW2DE_STAGE_VERTEX" / "#ifdef TW2DE_STAGE_PIXEL" section, and the text defines its own stage: before, a
// pixel shader counted the vertex shaders' constant 904 (given to nearly every draw) and a vertex shader the pixel
// shaders' 900-903, 906 and 909, and a draw that gave one of those made a "variant" of the same code (compiled and
// stored again under its own key; the same DKSH, checked with uam). Only the TW2DE_STAGE_* conditions are evaluated:
// any other #if / #ifdef / #ifndef counts as compiled (its #else too), as before.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace plume {

inline std::vector<std::pair<uint32_t, uint32_t>> Deko3DParseSpecDefaults(const std::string &glsl) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    // The stage the text is: its "#define TW2DE_STAGE_PIXEL" / "_VERTEX" line (none: every section counts).
    const bool pixel = glsl.find("#define TW2DE_STAGE_PIXEL") != std::string::npos;
    const bool vertex = glsl.find("#define TW2DE_STAGE_VERTEX") != std::string::npos;
    const bool known = pixel != vertex;
    // Per open conditional: 0 not about the stage, 1 a stage section compiled, 2 one not compiled.
    std::vector<uint8_t> stack;
    int off = 0;  // open sections not compiled
    static const char kDefine[] = "#define SPIRV_CROSS_CONSTANT_ID_";
    size_t line = 0;
    while (line < glsl.size()) {
        size_t end = glsl.find('\n', line);
        if (end == std::string::npos)
            end = glsl.size();
        size_t s = line;
        while (s < end && (glsl[s] == ' ' || glsl[s] == '\t'))
            ++s;
        if (s < end && glsl[s] == '#') {
            auto starts = [&](const char *w) { return glsl.compare(s, std::strlen(w), w) == 0; };
            if (starts("#ifdef ") || starts("#ifndef ")) {
                const bool neg = starts("#ifndef ");
                size_t n = s + (neg ? 8 : 7);
                while (n < end && glsl[n] == ' ')
                    ++n;
                size_t e = n;
                while (e < end && glsl[e] != ' ' && glsl[e] != '\t' && glsl[e] != '\r')
                    ++e;
                const std::string name = glsl.substr(n, e - n);
                uint8_t kind = 0;
                if (known && (name == "TW2DE_STAGE_PIXEL" || name == "TW2DE_STAGE_VERTEX")) {
                    const bool defined = name == "TW2DE_STAGE_PIXEL" ? pixel : vertex;
                    kind = (defined != neg) ? 1 : 2;
                }
                stack.push_back(kind);
                off += kind == 2;
            }
            else if (starts("#if ") || starts("#if(")) {
                stack.push_back(0);
            }
            else if (starts("#else") || starts("#elif")) {
                if (!stack.empty() && stack.back() != 0) {
                    off -= stack.back() == 2;
                    // #else: the other side; #elif after a compiled section: not compiled, after one not compiled:
                    // its own condition, not evaluated (compiled)
                    stack.back() = stack.back() == 1 ? 2 : (starts("#elif") ? 0 : 1);
                    off += stack.back() == 2;
                }
            }
            else if (starts("#endif")) {
                if (!stack.empty()) {
                    off -= stack.back() == 2;
                    stack.pop_back();
                }
            }
            else if (off == 0 && starts(kDefine)) {
                const char *p = glsl.c_str() + s + (sizeof(kDefine) - 1);
                char *idEnd = nullptr;
                const uint32_t id = uint32_t(std::strtoul(p, &idEnd, 10));
                const uint32_t value = uint32_t(std::strtoul(idEnd, nullptr, 0));
                out.emplace_back(id, value);
            }
        }
        line = end + 1;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &b) { return a.first < b.first; });
    return out;
}

}  // namespace plume
