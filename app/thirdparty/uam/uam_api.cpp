// uam_api.h on top of uam's DekoCompiler (one thread only, see the header)
#include "uam_api.h"

#include "compiler_iface.h"
#include "uam_log.h"

namespace nv50_ir { void poolTrim() noexcept; }  // tw2de (2026-10-23): nv50_ir_pool.h

namespace uam {
namespace {

void append_log(const char* msg, void* user) {
    static_cast<std::string*>(user)->append(msg);
}

pipeline_stage to_pipeline_stage(Stage s) {
    switch (s) {
        case Stage::Vertex: return pipeline_stage_vertex;
        case Stage::TessCtrl: return pipeline_stage_tess_ctrl;
        case Stage::TessEval: return pipeline_stage_tess_eval;
        case Stage::Geometry: return pipeline_stage_geometry;
        case Stage::Fragment: return pipeline_stage_fragment;
        case Stage::Compute: return pipeline_stage_compute;
    }
    return pipeline_stage_fragment;
}

bool g_resident = false;

}  // namespace

void init(bool resident_frontend) {
    glsl_frontend_init();
    g_resident = resident_frontend;
    DekoCompiler::SetFrontendResident(resident_frontend);
}

void shutdown() {
    if (g_resident) glsl_frontend_exit();
    g_resident = false;
    DekoCompiler::SetFrontendResident(false);
}

extern "C" void nv50_ir_set_tw2de_flags(unsigned flags);
extern "C" void nv50_ir_set_tw2de_flags_default(unsigned flags);

void setFNXF_XIII_LRFlags(unsigned flags) {
    nv50_ir_set_tw2de_flags_default(flags);  // tw2de (patch 9): every compile thread's
    nv50_ir_set_tw2de_flags(flags);
}

void setThreadFNXF_XIII_LRFlags(unsigned flags) {
    nv50_ir_set_tw2de_flags(flags);  // tw2de (2026-10-21): this thread's alone
}

Result compile(Stage stage, const char* glsl) {
    Result r;
    uam_set_log_callback(append_log, &r.log);
    {
        // tw2de (2026-10-18): CompileBest, the same choice as the PC tool's (the DKSH cache)
        std::unique_ptr<DekoCompiler> c = glsl ? DekoCompiler::CompileBest(to_pipeline_stage(stage), glsl, 3) : nullptr;
        r.ok = c != nullptr;
        if (r.ok) c->WriteDksh(r.dksh);
    }
    nv50_ir::poolTrim();  // tw2de (2026-10-23): the compile's nv50_ir blocks back (nv50_ir_pool.h)
    uam_set_log_callback(nullptr, nullptr);
    return r;
}

bool frontEnd(Stage stage, const char* glsl, std::vector<uint8_t>& out, std::string& log) {
    uam_set_log_callback(append_log, &log);
    bool ok;
    {
        DekoCompiler c(to_pipeline_stage(stage), 3);
        ok = glsl && c.FrontEnd(glsl);
        if (ok)
            c.WriteFrontEnd(out);
    }
    nv50_ir::poolTrim();
    uam_set_log_callback(nullptr, nullptr);
    return ok;
}

Result compileSpecialized(Stage stage, const uint8_t* fe, size_t size, unsigned buffer, const uint32_t* values,
                          unsigned count, bool earlyZ, int gprCap) {
    Result r;
    uam_set_log_callback(append_log, &r.log);
    {
        std::unique_ptr<DekoCompiler> c = DekoCompiler::CompileBestSpecialized(
            to_pipeline_stage(stage), fe, size, buffer, values, count, earlyZ, gprCap, 3);
        r.ok = c != nullptr;
        if (r.ok) c->WriteDksh(r.dksh);
    }
    nv50_ir::poolTrim();
    uam_set_log_callback(nullptr, nullptr);
    return r;
}

}  // namespace uam
