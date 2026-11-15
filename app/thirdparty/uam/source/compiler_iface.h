#pragma once
#include <memory>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "tgsi/tgsi_text.h"
#include "tgsi/tgsi_dump.h"

#include "codegen/nv50_ir_driver.h"

#include "glsl_frontend.h"

#include "nv_attributes.h"
#include "nv_shader_header.h"
#include "dksh.h"

class DekoCompiler
{
	pipeline_stage m_stage;
	glsl_program m_glsl;
	const struct tgsi_token* m_tgsi;
	struct tgsi_token* m_tgsiOwn = nullptr;  // tw2de (2026-10-23): a specialised copy (tgsi_spec.h), freed with this
	unsigned int m_tgsiNumTokens;
	nv50_ir_prog_info m_info;
	void* m_code;
	uint32_t m_codeSize;
	void* m_data;
	uint32_t m_dataSize;
	// tw2de (2026-10-23): what the codegen needs of the GLSL program besides its TGSI, kept here (copied from the
	// program, or from a front end's bytes: late specialisation): the vertex inputs' locations, the constant buffer,
	// the compute shared size. The codegen reads these, not the program.
	int8_t m_inLoc[32] = {};
	std::vector<uint8_t> m_dataOwn;
	unsigned m_smem = 0;

	NvShaderHeader m_nvsh;
	DkshProgramHeader m_dkph;

	void RetrieveAndPadCode();
	void GenerateHeaders();
	bool CompileGlslUnguarded(const char* glsl);
	bool CompileTgsiOfUnguarded(const DekoCompiler& other);
	void CaptureProgram();
	bool GenerateUnguarded();
	bool FrontEndUnguarded(const char* glsl);
	bool LoadFrontEndUnguarded(const uint8_t* data, size_t size, unsigned buffer, const uint32_t* values, unsigned count,
		bool earlyZ);

public:
	DekoCompiler(pipeline_stage stage, int optLevel = 3);
	~DekoCompiler();

	bool CompileGlsl(const char* glsl);
	void OutputDksh(const char* dkshFile);
	void WriteDksh(std::vector<uint8_t>& out) const; // SwitchWakerHD patch 2: OutputDksh's bytes, in memory

	// SwitchWakerHD patch 5: true keeps Mesa's built-in functions and type tables between compilers
	// (built once, on the first compile) instead of releasing them in every destructor
	static void SetFrontendResident(bool resident);

	// tw2de (2026-10-18): compiles the GLSL; for a fragment shader with tw2de flag 16 (choose), also without
	// flag 8 (the input interpolation cache), without 32 (value numbering over the dominator tree), without both and
	// without 512 (texture fetches clustered) (the codegen again from the same TGSI), and keeps the best: fewer GPRs
	// in steps of 8 (the per-thread allocation unit, which sets how many warps an SM holds), then less code. nullptr
	// when it does not compile.
	static std::unique_ptr<DekoCompiler> CompileBest(pipeline_stage stage, const char* glsl, int optLevel = 3);
	static std::unique_ptr<DekoCompiler> TryDrops(std::unique_ptr<DekoCompiler> best, pipeline_stage stage, int optLevel, unsigned flags);
	static std::unique_ptr<DekoCompiler> TryRescue(std::unique_ptr<DekoCompiler> best, pipeline_stage stage, int optLevel, unsigned flags);

	// tw2de (2026-10-18): the codegen of another compiler's TGSI (its GLSL program must outlive this one, or be
	// handed over: TakeGlslProgram).
	bool CompileTgsiOf(const DekoCompiler& other);
	void TakeGlslProgram(DekoCompiler& other)
	{
		m_glsl = other.m_glsl;
		other.m_glsl = nullptr;
		// tw2de (2026-10-23): and its specialised TGSI (this one's codegen read it)
		if (other.m_tgsiOwn == m_tgsi) { m_tgsiOwn = other.m_tgsiOwn; other.m_tgsiOwn = nullptr; }
	}
	// tw2de (2026-10-23, late specialisation): the front end alone (GLSL to TGSI), and its result as bytes (the
	// TGSI and what the codegen needs besides); then, later and elsewhere, a compiler made from those bytes with
	// constant buffer 'buffer' read as values (tgsi_spec.h) and the codegen run on it.
	bool FrontEnd(const char* glsl);
	void WriteFrontEnd(std::vector<uint8_t>& out) const;
	static std::unique_ptr<DekoCompiler> CompileBestSpecialized(pipeline_stage stage, const uint8_t* fe, size_t size,
		unsigned buffer, const uint32_t* values, unsigned count, bool earlyZ, int gprCap, int optLevel = 3);
	void OutputRawCode(const char* rawFile);
	void OutputTgsi(const char* tgsiFile);
};
