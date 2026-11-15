#pragma once
// The renderer's view of uam (devkitPro's deko3d shader compiler, vendored here with the patches in
// PATCHES.md): GLSL source in, DKSH bytes out (the same bytes the uam command line tool writes).
//
// NOT thread-safe and NOT reentrant: Mesa's GLSL frontend inside uam keeps one static gl_context and
// global tables, so init() and every compile() must run on ONE thread (the renderer's compile worker).
// That thread needs a large stack (Mesa's parser and nv50_ir recurse deeply: 8 MB on the Switch).
#include <cstdint>
#include <string>
#include <vector>

namespace uam {

enum class Stage { Vertex, TessCtrl, TessEval, Geometry, Fragment, Compute };

struct Result {
    bool ok = false;
    std::vector<uint8_t> dksh;  // a whole .dksh file (header, program header, code, constants); empty on failure
    std::string log;            // everything uam reported for this shader (errors, warnings); may be set when ok
};

// once, before the first compile(). resident_frontend keeps Mesa's built-in GLSL function library and
// type tables alive between compiles (built on the first compile, a few MB); false rebuilds and frees
// them around every compile, as the uam tool does.
void init(bool resident_frontend = true);
// frees what init() and the compiles kept; compile() works again after another init()
void shutdown();

// compiles one stage of a separable program (uam has no linking; every binding must be explicit).
// A compile error, and any fatal error inside Mesa or nv50_ir, returns ok = false with the reason in
// log; it never exits the process.
Result compile(Stage stage, const char* glsl);

// tw2de (2026-10-18): tw2de's codegen passes (nv50_ir_peephole.cpp): 1 PRMT byte moves, 2 the storage buffer
// offset fold, 4 the shift distribution, 8 the fragment input interpolation cache, 16 choose per fragment shader
// whether 8 pays (default all). Before compiling; the DKSH cache must be one made with the same.
void setFNXF_XIII_LRFlags(unsigned flags);
// tw2de (2026-10-21): the calling thread's flags only (its later compiles); the other threads keep theirs. The
// renderer's run-time compiles go without 16 (one backend run: the fast pass), its background upgrades with it.
void setThreadFNXF_XIII_LRFlags(unsigned flags);

// tw2de (2026-10-23, late specialisation; source/tgsi_spec.h). A shader whose specialization constants are read
// from uniform buffer 'binding' (in GLSL; TGSI buffer binding + 1) goes through Mesa's front end once (frontEnd: its
// TGSI and what the codegen needs, as bytes to keep); a variant is then compiled from those bytes with the buffer's
// vec4 slot k, component c read as values[k * 4 + c] and the branches that decides cut (compileSpecialized): the
// codegen alone, about a quarter of a compile from GLSL. earlyZ: the early fragment tests; gprCap: 32 (constant 910)
// or 0.
bool frontEnd(Stage stage, const char* glsl, std::vector<uint8_t>& out, std::string& log);
Result compileSpecialized(Stage stage, const uint8_t* frontEnd, size_t size, unsigned buffer, const uint32_t* values,
                          unsigned count, bool earlyZ, int gprCap);

}  // namespace uam
