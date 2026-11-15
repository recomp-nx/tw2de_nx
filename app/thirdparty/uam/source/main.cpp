// tw2de (2026-11-06): the uam command line tool (target uam_tool, output name 'uam') for the PC build: the host
// shaders' DKSH for the NRO's romfs (app/src/renut_engine/gpu/gpu.cmake), made by this uam (the console's) instead of
// devkitPro's. devkitPro's uam 1.1.0 made wrong code for FSR 3's accumulate and prepare_inputs passes (constant
// buffer values read as 0 in some half-warps, a different set every frame; the console's uam is right), so the build
// uses the compiler the console uses, the same way: uam::compile (CompileBest) with the tw2de codegen flags at 0,
// as plume compiles a host shader (Deko3DShaderBlob::StagePlainUam) at run time. Same bytes as a console compile,
// except for a shader that names its own flags (a '// tw2de-uam-flags N' line: see main below).
//
//   uam -s <vert|tess_ctrl|tess_eval|geom|frag|comp> -o <out.dksh> <in.glsl>
//
// devkitPro's tool's command line for the options gpu.cmake uses; exit code 0 on success, 1 on a compile error (the
// log on stderr), 2 on a bad command line or a file error.
#include "uam_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

bool ParseStage(const char *s, uam::Stage &out) {
    struct Name {
        const char *name;
        uam::Stage stage;
    };
    static const Name kNames[] = {{"vert", uam::Stage::Vertex},       {"vertex", uam::Stage::Vertex},
                                  {"tess_ctrl", uam::Stage::TessCtrl}, {"tess_eval", uam::Stage::TessEval},
                                  {"geom", uam::Stage::Geometry},      {"geometry", uam::Stage::Geometry},
                                  {"frag", uam::Stage::Fragment},      {"fragment", uam::Stage::Fragment},
                                  {"comp", uam::Stage::Compute},       {"compute", uam::Stage::Compute}};
    for (const Name &n : kNames) {
        if (!std::strcmp(s, n.name)) {
            out = n.stage;
            return true;
        }
    }
    return false;
}

int Usage() {
    std::fprintf(stderr, "usage: uam -s <vert|tess_ctrl|tess_eval|geom|frag|comp> -o <out.dksh> <in.glsl>\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    const char *stage_name = nullptr, *out_path = nullptr, *in_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if ((!std::strcmp(argv[i], "-s") || !std::strcmp(argv[i], "--stage")) && i + 1 < argc)
            stage_name = argv[++i];
        else if ((!std::strcmp(argv[i], "-o") || !std::strcmp(argv[i], "--out")) && i + 1 < argc)
            out_path = argv[++i];
        else if (argv[i][0] == '-')
            return Usage();
        else if (!in_path)
            in_path = argv[i];
        else
            return Usage();
    }
    uam::Stage stage;
    if (!stage_name || !out_path || !in_path || !ParseStage(stage_name, stage))
        return Usage();

    std::ifstream in(in_path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "uam: cannot read %s\n", in_path);
        return 2;
    }
    std::stringstream text;
    text << in.rdbuf();

    // As the console compiles a host shader: the tw2de codegen flags at 0 (unless the shader names its own), CompileBest.
    // tw2de (2026-11-07): a shader can ask for codegen flags with a line '// tw2de-uam-flags N' (tools/fsr3/
    // convert_fsr3.py: FSR 3's accumulate passes, 2048 | 2097152: the register-saving passes, 105 -> 63 registers);
    // UAM_TW2DE_FLAGS in the environment overrides it (tests).
    unsigned flags = 0;
    const std::string src = text.str();
    static const char kTag[] = "// tw2de-uam-flags ";
    const size_t tag = src.find(kTag);
    if (tag != std::string::npos)
        flags = unsigned(std::strtoul(src.c_str() + tag + sizeof(kTag) - 1, nullptr, 0));
    if (const char *flags_env = std::getenv("UAM_TW2DE_FLAGS"))
        flags = unsigned(std::strtoul(flags_env, nullptr, 0));
    uam::setFNXF_XIII_LRFlags(flags);
    uam::init(false);
    const uam::Result r = uam::compile(stage, src.c_str());
    uam::shutdown();
    if (!r.log.empty())
        std::fprintf(stderr, "%s", r.log.c_str());
    if (!r.ok) {
        std::fprintf(stderr, "uam: %s failed to compile\n", in_path);
        return 1;
    }
    std::FILE *out = std::fopen(out_path, "wb");
    if (!out || std::fwrite(r.dksh.data(), 1, r.dksh.size(), out) != r.dksh.size() || std::fclose(out) != 0) {
        std::fprintf(stderr, "uam: cannot write %s\n", out_path);
        return 2;
    }
    return 0;
}
