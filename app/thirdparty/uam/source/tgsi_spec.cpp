// tw2de (2026-10-23): late specialisation (tgsi_spec.h).
#include "tgsi_spec.h"

#include <cstring>
#include <iterator>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <cstdlib>
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_build.h"
extern "C" {
#include "tgsi/tgsi_transform.h"  // no extern "C" of its own
}

namespace {

// A temporary's components: known (a value) or not.
struct Known {
	bool known[4] = {};
	uint32_t v[4] = {};
};

struct State {
	std::unordered_map<unsigned, Known> temps;
	void forget(unsigned index) { temps.erase(index); }
	void forgetAll() { temps.clear(); }
	bool get(unsigned index, unsigned c, uint32_t& out) const
	{
		auto it = temps.find(index);
		if (it == temps.end() || !it->second.known[c])
			return false;
		out = it->second.v[c];
		return true;
	}
	// What both have (a merge after an if / else).
	void meet(const State& o)
	{
		for (auto it = temps.begin(); it != temps.end();) {
			auto jt = o.temps.find(it->first);
			bool any = false;
			for (int c = 0; c < 4; ++c) {
				if (it->second.known[c] && (jt == o.temps.end() || !jt->second.known[c] || jt->second.v[c] != it->second.v[c]))
					it->second.known[c] = false;
				any |= it->second.known[c];
			}
			it = any ? std::next(it) : temps.erase(it);
		}
	}
};

enum class Region { Normal, TakeThen, TakeElse, Loop, Switch };

struct Frame {
	Frame(Region k, bool outer) : kind(k), skipping_outer(outer) {}
	Region kind;
	bool skipping_outer;  // the frame around this one was skipping
	bool in_else = false;
	State at_if;          // Normal: the state at the IF
	State then_end;       // Normal: the state at the ELSE
};

struct Ctx {
	tgsi_transform_context base;
	unsigned buffer;
	const uint32_t* values;
	unsigned count;
	// the buffer's vec4 slots read directly, and the immediate each became
	std::map<unsigned, unsigned> slotImm;
	unsigned immCount = 0;
	// per loop / switch (by the index of its first instruction): the temporaries written inside it
	std::map<unsigned, std::unordered_set<unsigned>> regionWrites;
	bool regionWritesAll[1] = {};
	std::unordered_set<unsigned> indirectRegions;  // regions with an indirect temporary write: forget all
	unsigned insn = 0;
	State state;
	std::vector<Frame> stack;
	bool skipping = false;
	bool dead = false;  // after an unconditional discard at the top level
	unsigned folded = 0;
	// instructions whose result is known: the first pass collects their values, the second writes each as a MOV of
	// an immediate (the immediates go before the instructions)
	bool earlyZ = false;  // the early fragment tests property added (constant 909)
	bool collect = true;
	std::vector<uint32_t> folds;  // 4 words a fold
	size_t foldNext = 0;
	unsigned foldBase = 0;
	unsigned movs = 0;
};

uint32_t SpecValue(const Ctx* c, unsigned slot, unsigned comp)
{
	const unsigned k = slot * 4 + comp;
	return k < c->count ? c->values[k] : 0u;
}

bool IsSpecSrc(const Ctx* c, const tgsi_full_src_register& s)
{
	return s.Register.File == TGSI_FILE_CONSTANT && s.Register.Dimension && !s.Dimension.Indirect &&
		s.Dimension.Index == int(c->buffer) && !s.Register.Indirect;
}

// The value of a source's component c (after the swizzle), when known.
bool SrcValue(const Ctx* c, const tgsi_full_src_register& s, unsigned comp, uint32_t& out, const std::vector<uint32_t>* imms)
{
	if (s.Register.Negate || s.Register.Absolute)
		return false;
	const unsigned swz[4] = { s.Register.SwizzleX, s.Register.SwizzleY, s.Register.SwizzleZ, s.Register.SwizzleW };
	const unsigned sc = swz[comp];
	switch (s.Register.File) {
	case TGSI_FILE_IMMEDIATE:
		if (s.Register.Indirect || !imms || size_t(s.Register.Index) * 4 + sc >= imms->size())
			return false;
		out = (*imms)[s.Register.Index * 4 + sc];
		return true;
	case TGSI_FILE_TEMPORARY:
		if (s.Register.Indirect)
			return false;
		return c->state.get(s.Register.Index, sc, out);
	default:
		return false;
	}
}

thread_local std::vector<uint32_t> g_imms;  // the immediates' words in order (one thread compiles a shader at a time: per call)

bool Evaluate(unsigned op, const uint32_t* a, unsigned n, uint32_t& r)
{
	switch (op) {
	case TGSI_OPCODE_MOV: r = a[0]; return true;
	case TGSI_OPCODE_AND: r = a[0] & a[1]; return true;
	case TGSI_OPCODE_OR: r = a[0] | a[1]; return true;
	case TGSI_OPCODE_XOR: r = a[0] ^ a[1]; return true;
	case TGSI_OPCODE_NOT: r = ~a[0]; return true;
	case TGSI_OPCODE_USEQ: r = a[0] == a[1] ? ~0u : 0u; return true;
	case TGSI_OPCODE_USNE: r = a[0] != a[1] ? ~0u : 0u; return true;
	case TGSI_OPCODE_USGE: r = a[0] >= a[1] ? ~0u : 0u; return true;
	case TGSI_OPCODE_USLT: r = a[0] < a[1] ? ~0u : 0u; return true;
	case TGSI_OPCODE_ISGE: r = int32_t(a[0]) >= int32_t(a[1]) ? ~0u : 0u; return true;
	case TGSI_OPCODE_ISLT: r = int32_t(a[0]) < int32_t(a[1]) ? ~0u : 0u; return true;
	case TGSI_OPCODE_UCMP: r = a[0] ? a[1] : a[2]; return true;
	case TGSI_OPCODE_SHL: r = a[0] << (a[1] & 31); return true;
	case TGSI_OPCODE_USHR: r = a[0] >> (a[1] & 31); return true;
	case TGSI_OPCODE_ISHR: r = uint32_t(int32_t(a[0]) >> (a[1] & 31)); return true;
	case TGSI_OPCODE_UADD: r = a[0] + a[1]; return true;
	case TGSI_OPCODE_UMUL: r = a[0] * a[1]; return true;
	case TGSI_OPCODE_INEG: r = uint32_t(-int32_t(a[0])); return true;
	default: (void)n; return false;
	}
}

bool ComponentWise(unsigned op)
{
	switch (op) {
	case TGSI_OPCODE_MOV: case TGSI_OPCODE_AND: case TGSI_OPCODE_OR: case TGSI_OPCODE_XOR: case TGSI_OPCODE_NOT:
	case TGSI_OPCODE_USEQ: case TGSI_OPCODE_USNE: case TGSI_OPCODE_USGE: case TGSI_OPCODE_USLT: case TGSI_OPCODE_ISGE:
	case TGSI_OPCODE_ISLT: case TGSI_OPCODE_UCMP: case TGSI_OPCODE_SHL: case TGSI_OPCODE_USHR: case TGSI_OPCODE_ISHR:
	case TGSI_OPCODE_UADD: case TGSI_OPCODE_UMUL: case TGSI_OPCODE_INEG:
		return true;
	default:
		return false;
	}
}

// What a component-wise instruction (one destination) computes in each written component, when known: the mask.
unsigned Results(const Ctx* c, const tgsi_full_instruction& in, uint32_t out[4])
{
	const unsigned op = in.Instruction.Opcode;
	if (in.Instruction.NumDstRegs != 1 || in.Instruction.Saturate || !ComponentWise(op) || in.Instruction.NumSrcRegs > 3 ||
		in.Dst[0].Register.Indirect)
		return 0;
	unsigned mask = 0;
	for (unsigned comp = 0; comp < 4; ++comp) {
		if (!(in.Dst[0].Register.WriteMask & (1u << comp)))
			continue;
		uint32_t a[3] = {};
		bool have[3] = {};
		bool all = true;
		for (unsigned s = 0; s < in.Instruction.NumSrcRegs; ++s) {
			have[s] = SrcValue(c, in.Src[s], comp, a[s], &g_imms);
			all = all && have[s];
		}
		if (all && Evaluate(op, a, in.Instruction.NumSrcRegs, out[comp])) {
			mask |= 1u << comp;
			continue;
		}
		// one known source can be enough: x & 0, x | ~0, x * 0, a select whose condition is known
		if ((op == TGSI_OPCODE_AND || op == TGSI_OPCODE_UMUL) && ((have[0] && a[0] == 0) || (have[1] && a[1] == 0))) {
			out[comp] = 0;
			mask |= 1u << comp;
		} else if (op == TGSI_OPCODE_OR && ((have[0] && a[0] == ~0u) || (have[1] && a[1] == ~0u))) {
			out[comp] = ~0u;
			mask |= 1u << comp;
		} else if (op == TGSI_OPCODE_UCMP && have[0] && have[a[0] ? 1 : 2]) {
			out[comp] = a[a[0] ? 1 : 2];
			mask |= 1u << comp;
		}
	}
	return mask;
}

// The temporaries an instruction writes become what it computes (when known), or unknown.
void Track(Ctx* c, const tgsi_full_instruction& in)
{
	uint32_t v[4] = {};
	const unsigned mask = Results(c, in, v);
	for (unsigned d = 0; d < in.Instruction.NumDstRegs; ++d) {
		const tgsi_full_dst_register& dst = in.Dst[d];
		if (dst.Register.File != TGSI_FILE_TEMPORARY)
			continue;
		if (dst.Register.Indirect) {
			c->state.forgetAll();
			continue;
		}
		Known k;
		auto it = c->state.temps.find(dst.Register.Index);
		if (it != c->state.temps.end())
			k = it->second;
		for (unsigned comp = 0; comp < 4; ++comp) {
			if (!(dst.Register.WriteMask & (1u << comp)))
				continue;
			k.known[comp] = d == 0 && (mask & (1u << comp)) != 0;
			k.v[comp] = v[comp];
		}
		c->state.temps[dst.Register.Index] = k;
	}
}

void ForgetRegion(Ctx* c, unsigned start)
{
	if (c->indirectRegions.count(start)) {
		c->state.forgetAll();
		return;
	}
	auto it = c->regionWrites.find(start);
	if (it != c->regionWrites.end())
		for (unsigned t : it->second)
			c->state.forget(t);
}

void TransformImmediate(tgsi_transform_context* tc, tgsi_full_immediate* imm)
{
	Ctx* c = reinterpret_cast<Ctx*>(tc);
	for (unsigned i = 0; i < 4; ++i)
		g_imms.push_back(i < imm->Immediate.NrTokens - 1 ? imm->u[i].Uint : 0u);
	c->immCount++;
	tc->emit_immediate(tc, imm);
}

void TransformDeclaration(tgsi_transform_context* tc, tgsi_full_declaration* decl)
{
	Ctx* c = reinterpret_cast<Ctx*>(tc);
	// the buffer itself goes: nothing reads it any more
	if (decl->Declaration.File == TGSI_FILE_CONSTANT && decl->Declaration.Dimension && decl->Dim.Index2D == c->buffer)
		return;
	tc->emit_declaration(tc, decl);
}

void Prolog(tgsi_transform_context* tc)
{
	Ctx* c = reinterpret_cast<Ctx*>(tc);
	if (c->earlyZ) {
		tgsi_full_property prop = tgsi_default_full_property();
		prop.Property.PropertyName = TGSI_PROPERTY_FS_EARLY_DEPTH_STENCIL;
		prop.Property.NrTokens = 2;
		prop.u[0].Data = 1;
		tc->emit_property(tc, &prop);
	}
	// one immediate per slot of the buffer that is read: its four values
	for (std::map<unsigned, unsigned>::iterator it = c->slotImm.begin(); it != c->slotImm.end(); ++it) {
		const unsigned slot = it->first;
		unsigned& index = it->second;
		tgsi_full_immediate imm = tgsi_default_full_immediate();
		imm.Immediate.NrTokens = 1 + 4;
		imm.Immediate.DataType = TGSI_IMM_UINT32;
		for (unsigned i = 0; i < 4; ++i) {
			imm.u[i].Uint = SpecValue(c, slot, i);
			g_imms.push_back(imm.u[i].Uint);
		}
		index = c->immCount++;
		tc->emit_immediate(tc, &imm);
	}
	c->foldBase = c->immCount;
	if (!c->collect) {
		for (size_t f = 0; f + 4 <= c->folds.size(); f += 4) {
			tgsi_full_immediate imm = tgsi_default_full_immediate();
			imm.Immediate.NrTokens = 1 + 4;
			imm.Immediate.DataType = TGSI_IMM_UINT32;
			for (unsigned i = 0; i < 4; ++i) {
				imm.u[i].Uint = c->folds[f + i];
				g_imms.push_back(imm.u[i].Uint);
			}
			c->immCount++;
			tc->emit_immediate(tc, &imm);
		}
	}
}

tgsi_full_src_register ImmSrc(unsigned index)
{
	tgsi_full_src_register s;
	std::memset(&s, 0, sizeof(s));
	s.Register.File = TGSI_FILE_IMMEDIATE;
	s.Register.Index = index;
	s.Register.SwizzleX = TGSI_SWIZZLE_X;
	s.Register.SwizzleY = TGSI_SWIZZLE_Y;
	s.Register.SwizzleZ = TGSI_SWIZZLE_Z;
	s.Register.SwizzleW = TGSI_SWIZZLE_W;
	return s;
}

bool SameRegister(const tgsi_full_dst_register& d, const tgsi_full_src_register& s)
{
	return d.Register.File == s.Register.File && d.Register.Index == s.Register.Index;
}

// An instruction whose result is known as a MOV of it; a UCMP whose condition is known as MOVs of what it picks.
// True: emitted (the original is not).
bool Fold(Ctx* c, tgsi_transform_context* tc, const tgsi_full_instruction& in)
{
	uint32_t v[4] = {};
	const unsigned mask = Results(c, in, v);
	const unsigned write = in.Instruction.NumDstRegs == 1 ? in.Dst[0].Register.WriteMask : 0;
	if (write && mask == write && in.Instruction.Opcode != TGSI_OPCODE_MOV) {
		if (c->collect) {
			c->folds.insert(c->folds.end(), v, v + 4);
			return false;
		}
		tgsi_full_instruction mov = in;
		mov.Instruction.Opcode = TGSI_OPCODE_MOV;
		mov.Instruction.NumSrcRegs = 1;
		mov.Src[0] = ImmSrc(c->foldBase + unsigned(c->foldNext++));
		tc->emit_instruction(tc, &mov);
		c->movs++;
		return true;
	}
	if (in.Instruction.Opcode == TGSI_OPCODE_UCMP && write && !in.Dst[0].Register.Indirect && !in.Instruction.Saturate) {
		unsigned pick1 = 0, pick2 = 0;
		for (unsigned comp = 0; comp < 4; ++comp) {
			if (!(write & (1u << comp)))
				continue;
			uint32_t cond;
			if (!SrcValue(c, in.Src[0], comp, cond, &g_imms))
				return false;
			(cond ? pick1 : pick2) |= 1u << comp;
		}
		if (pick1 && pick2 && (SameRegister(in.Dst[0], in.Src[1]) || SameRegister(in.Dst[0], in.Src[2])))
			return false;  // the first MOV would change what the second reads
		for (int k = 0; k < 2; ++k) {
			const unsigned m = k == 0 ? pick1 : pick2;
			if (!m)
				continue;
			tgsi_full_instruction mov = in;
			mov.Instruction.Opcode = TGSI_OPCODE_MOV;
			mov.Instruction.NumSrcRegs = 1;
			mov.Src[0] = in.Src[k == 0 ? 1 : 2];
			mov.Dst[0].Register.WriteMask = m;
			tc->emit_instruction(tc, &mov);
		}
		c->movs++;
		return true;
	}
	return false;
}

void TransformInstruction(tgsi_transform_context* tc, tgsi_full_instruction* in)
{
	Ctx* c = reinterpret_cast<Ctx*>(tc);
	const unsigned at = c->insn++;
	const unsigned op = in->Instruction.Opcode;
	if (c->dead) {
		if (op == TGSI_OPCODE_END)
			tc->emit_instruction(tc, in);
		return;
	}

	// structure first: it decides whether this instruction is kept
	switch (op) {
	case TGSI_OPCODE_IF:
	case TGSI_OPCODE_UIF: {
		Frame f{ Region::Normal, c->skipping };
		if (!c->skipping) {
			for (unsigned s = 0; s < in->Instruction.NumSrcRegs; ++s)
				if (IsSpecSrc(c, in->Src[s])) {
					in->Src[s].Register.File = TGSI_FILE_IMMEDIATE;
					in->Src[s].Register.Index = c->slotImm[in->Src[s].Register.Index];
					in->Src[s].Register.Dimension = 0;
				}
			uint32_t v;
			if (SrcValue(c, in->Src[0], 0, v, &g_imms)) {
				bool taken;
				if (op == TGSI_OPCODE_IF) {
					float fv;
					std::memcpy(&fv, &v, 4);
					taken = fv != 0.0f;
				} else {
					taken = v != 0;
				}
				f.kind = taken ? Region::TakeThen : Region::TakeElse;
				c->folded++;
			}
		}
		if (f.kind == Region::Normal) {
			f.at_if = c->state;
			c->stack.push_back(f);
			if (!c->skipping)
				tc->emit_instruction(tc, in);
			return;
		}
		c->stack.push_back(f);
		if (f.kind == Region::TakeElse)
			c->skipping = true;
		return;  // the IF goes
	}
	case TGSI_OPCODE_ELSE: {
		Frame& f = c->stack.back();
		f.in_else = true;
		if (f.kind == Region::Normal) {
			if (!f.skipping_outer) {
				f.then_end = c->state;
				c->state = f.at_if;
				tc->emit_instruction(tc, in);
			}
		} else if (!f.skipping_outer) {
			c->skipping = f.kind == Region::TakeThen;
		}
		return;
	}
	case TGSI_OPCODE_ENDIF: {
		Frame f = c->stack.back();
		c->stack.pop_back();
		if (f.kind == Region::Normal) {
			if (!f.skipping_outer) {
				if (f.in_else)
					c->state.meet(f.then_end);
				else
					c->state.meet(f.at_if);
				tc->emit_instruction(tc, in);
			}
		} else {
			c->skipping = f.skipping_outer;
		}
		return;
	}
	case TGSI_OPCODE_BGNLOOP:
	case TGSI_OPCODE_SWITCH:
		c->stack.push_back(Frame{ op == TGSI_OPCODE_BGNLOOP ? Region::Loop : Region::Switch, c->skipping });
		if (!c->skipping)
			ForgetRegion(c, at);  // what the body writes is not known at its start (a loop's next pass)
		break;
	case TGSI_OPCODE_ENDLOOP:
	case TGSI_OPCODE_ENDSWITCH: {
		c->stack.pop_back();
		break;
	}
	default:
		break;
	}
	if (c->skipping)
		return;
	for (unsigned s = 0; s < in->Instruction.NumSrcRegs; ++s)
		if (IsSpecSrc(c, in->Src[s])) {
			in->Src[s].Register.File = TGSI_FILE_IMMEDIATE;
			in->Src[s].Register.Index = c->slotImm[in->Src[s].Register.Index];
			in->Src[s].Register.Dimension = 0;
		}
	if (op == TGSI_OPCODE_ENDLOOP || op == TGSI_OPCODE_ENDSWITCH) {
		// after it: what the body wrote is not known (it may have run any number of times, or a case of it)
		c->state.forgetAll();
	} else if (op == TGSI_OPCODE_BRK || op == TGSI_OPCODE_CONT) {
		// nothing to track
	} else if (op == TGSI_OPCODE_KILL_IF) {
		// kills when a component is below zero: known, and none is, it goes
		bool never = true, always = false;
		tgsi_full_src_register src = in->Src[0];
		src.Register.Negate = 0;
		src.Register.Absolute = 0;
		for (unsigned comp = 0; comp < 4; ++comp) {
			uint32_t v;
			float f;
			if (!SrcValue(c, src, comp, v, &g_imms)) {
				never = false;
				continue;
			}
			std::memcpy(&f, &v, 4);
			if (in->Src[0].Register.Absolute)
				f = f < 0.0f ? -f : f;
			if (in->Src[0].Register.Negate)
				f = -f;
			if (f < 0.0f) {
				never = false;
				always = true;
			}
		}
		if (never)
			return;
		if (always) {
			// an unconditional discard; at the top level nothing after it matters (the pixel is gone)
			tgsi_full_instruction kill = *in;
			kill.Instruction.Opcode = TGSI_OPCODE_KILL;
			kill.Instruction.NumSrcRegs = 0;
			tc->emit_instruction(tc, &kill);
			if (c->stack.empty())
				c->dead = true;
			return;
		}
	} else {
		const bool done = Fold(c, tc, *in);
		Track(c, *in);
		if (done)
			return;
	}
	tc->emit_instruction(tc, in);
}

// Before the transform: the buffer's slots read, and each loop's or switch's writes.
void Scan(Ctx& c, const tgsi_token* tokens)
{
	tgsi_parse_context p;
	if (tgsi_parse_init(&p, tokens) != TGSI_PARSE_OK)
		return;
	unsigned insn = 0;
	std::vector<unsigned> open;  // the loops / switches around
	while (!tgsi_parse_end_of_tokens(&p)) {
		tgsi_parse_token(&p);
		if (p.FullToken.Token.Type != TGSI_TOKEN_TYPE_INSTRUCTION)
			continue;
		const tgsi_full_instruction& in = p.FullToken.FullInstruction;
		const unsigned op = in.Instruction.Opcode;
		if (op == TGSI_OPCODE_BGNLOOP || op == TGSI_OPCODE_SWITCH)
			open.push_back(insn);
		for (unsigned s = 0; s < in.Instruction.NumSrcRegs; ++s)
			if (IsSpecSrc(&c, in.Src[s]))
				c.slotImm[in.Src[s].Register.Index] = 0;
		for (unsigned d = 0; d < in.Instruction.NumDstRegs; ++d)
			if (in.Dst[d].Register.File == TGSI_FILE_TEMPORARY)
				for (unsigned r : open) {
					if (in.Dst[d].Register.Indirect)
						c.indirectRegions.insert(r);
					else
						c.regionWrites[r].insert(in.Dst[d].Register.Index);
				}
		if (op == TGSI_OPCODE_ENDLOOP || op == TGSI_OPCODE_ENDSWITCH)
			if (!open.empty())
				open.pop_back();
		++insn;
	}
	tgsi_parse_free(&p);
}

}  // namespace

tgsi_token* uam_tgsi_specialize(const tgsi_token* tokens, unsigned buffer, const uint32_t* values, unsigned count,
	unsigned* numTokens, unsigned* folded, bool earlyZ)
{
	// two passes: the first finds the instructions whose result is known, the second writes the tokens
	std::vector<uint32_t> folds;
	tgsi_token* out = nullptr;
	int n = 0;
	unsigned cut = 0;
	for (int pass = 0; pass < 2; ++pass) {
		Ctx c{};
		c.buffer = buffer;
		c.values = values;
		c.count = count;
		c.collect = pass == 0;
		c.earlyZ = earlyZ;
		c.folds = folds;
		g_imms.clear();
		Scan(c, tokens);
		const unsigned in = tgsi_num_tokens(tokens);
		// a UCMP can become two MOVs: room for twice the instructions
		const unsigned max = in * 2 + 64 + (unsigned(c.slotImm.size()) + unsigned(folds.size() / 4)) * 8;
		free(out);
		out = static_cast<tgsi_token*>(malloc(max * sizeof(tgsi_token)));
		if (!out)
			return nullptr;
		c.base.transform_instruction = TransformInstruction;
		c.base.transform_declaration = TransformDeclaration;
		c.base.transform_immediate = TransformImmediate;
		c.base.prolog = Prolog;
		n = tgsi_transform_shader(tokens, out, max, &c.base);
		g_imms.clear();
		if (n <= 0 || !c.stack.empty() || (pass == 1 && c.foldNext * 4 != folds.size())) {
			free(out);
			return nullptr;
		}
		folds = c.folds;
		cut = c.folded;
	}
	if (numTokens)
		*numTokens = unsigned(n);
	if (folded)
		*folded = cut;
	return out;
}
