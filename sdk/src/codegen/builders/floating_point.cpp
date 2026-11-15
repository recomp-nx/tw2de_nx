/**
 * @file        rexcodegen/builders/floating_point.cpp
 * @brief       PPC floating point instruction code generation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "builder_context.h"
#include "helpers.h"

namespace rex::codegen {

// exact_fp_log (tw2de 2026-11-01, diagnostic): around the host's code of an arithmetic instruction, the operands
// kept (the result may overwrite one), then rex::ppc::exact::check / check_single (rex/ppc/fpu_exact.h) with the
// exact_fp helper of the same name: a report where the 360's result would differ. Nothing without the option.
namespace {
class ExactFpCheck {
 public:
  ExactFpCheck(BuilderContext& ctx, const char* op, int operands, bool single)
      : ctx_(ctx), op_(op), n_(operands), single_(single), exact_(ctx.exact_fp_log()), origin_(ctx.fp_origin_log()),
        on_(exact_ || origin_) {
    if (!on_)
      return;
    ctx_.println("\t{{");
    for (int i = 0; i < n_; ++i)
      ctx_.println("\t\tconst double xfp{} = {}.f64;", i, ctx_.f(ctx_.insn.operands[1 + i]));
  }
  ~ExactFpCheck() {
    if (!on_)
      return;
    std::string args;
    for (int i = 0; i < n_; ++i)
      args += fmt::format("{}xfp{}", i ? ", " : "", i);
    const std::string head = fmt::format("0x{:08X}u, 0x{:08X}u, \"{}\", {}.f64, [&] {{ return rex::ppc::exact::{}({}); }}",
                                         ctx_.base, ctx_.fn.base(), op_, ctx_.f(ctx_.insn.operands[0]), op_, args);
    const int b = n_ > 1 ? 1 : 0, c = n_ > 2 ? 2 : b;
    if (exact_ && single_)
      ctx_.println("\t\trex::ppc::exact::check_single({}, xfp0, xfp{}, xfp{});", head, b, c);
    else if (exact_)
      ctx_.println("\t\trex::ppc::exact::check({});", head);
    if (origin_)
      ctx_.println("\t\trex::ppc::exact::origin(0x{:08X}u, 0x{:08X}u, \"{}\", {}.f64, xfp0, xfp{}, xfp{}, "
                   "uint32_t(ctx.lr), ctx.r1.u32, base);",
                   ctx_.base, ctx_.fn.base(), op_, ctx_.f(ctx_.insn.operands[0]), b, c);
    ctx_.println("\t}}");
  }

 private:
  BuilderContext& ctx_;
  const char* op_;
  int n_;
  bool single_;
  bool exact_;
  bool origin_;
  bool on_;
};
}  // namespace

//=============================================================================
// Sign Manipulation
//=============================================================================

bool build_fabs(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_move();
  ctx.println("\t{}.u64 = {}.u64 & ~0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fnabs(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_move();
  ctx.println("\t{}.u64 = {}.u64 | 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fneg(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_move();
  ctx.println("\t{}.u64 = {}.u64 ^ 0x8000000000000000;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Move and Conversion
//=============================================================================

bool build_fmr(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_move();
  ctx.println("\t{}.f64 = {}.f64;", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fcfid(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_modeless();
  ctx.println("\t{}.f64 = double({}.s64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctid(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  // tw2de (2026-11-01): always the helper (rex/ppc/diet_ops.h, what fcti_inline / fctiwz_inline gave): the 360's
  // values, and the conversion instruction itself, so that the host raises the flags FPSCR takes (XX for an inexact
  // result, VX for NaN and out of range: the record forms' CR1 in Xenia's hardware-recorded tests).
  ctx.println("\t{0}.s64 = rex::ppc::fctid({1}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctidz(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_modeless();
  // tw2de (2026-11-01): always the helper (rex/ppc/diet_ops.h, what fcti_inline / fctiwz_inline gave): the 360's
  // values, and the conversion instruction itself, so that the host raises the flags FPSCR takes (XX for an inexact
  // result, VX for NaN and out of range: the record forms' CR1 in Xenia's hardware-recorded tests).
  ctx.println("\t{0}.s64 = rex::ppc::fctidz({1}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiw(BuilderContext& ctx) {
  ctx.emit_set_flush_mode(false);
  // tw2de (2026-11-01): always the helper (rex/ppc/diet_ops.h, what fcti_inline / fctiwz_inline gave): the 360's
  // values, and the conversion instruction itself, so that the host raises the flags FPSCR takes (XX for an inexact
  // result, VX for NaN and out of range: the record forms' CR1 in Xenia's hardware-recorded tests).
  ctx.println("\t{0}.s64 = rex::ppc::fctiw({1}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fctiwz(BuilderContext& ctx) {
  ctx.emit_flush_mode_for_modeless();
  // tw2de (2026-11-01): always the helper (rex/ppc/diet_ops.h, what fcti_inline / fctiwz_inline gave): the 360's
  // values, and the conversion instruction itself, so that the host raises the flags FPSCR takes (XX for an inexact
  // result, VX for NaN and out of range: the record forms' CR1 in Xenia's hardware-recorded tests).
  ctx.println("\t{0}.s64 = rex::ppc::fctiwz({1}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsp(BuilderContext& ctx) {
  if (ctx.compare_any_mode()) {
    ctx.println("\t{}.f64 = rex::ppc::RoundToSingleAnyMode({}.f64);", ctx.f(ctx.insn.operands[0]),
                ctx.f(ctx.insn.operands[1]));
    return true;
  }
  ctx.println("\t{}.f64 = double(float({}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Comparison
//=============================================================================

bool build_fcmpu(BuilderContext& ctx) {
  if (ctx.compare_any_mode()) {
    ctx.println("\trex::ppc::CompareAnyMode({}, {}.f64, {}.f64);", ctx.cr(ctx.insn.operands[0]),
                ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ctx.println("\t{}.compare({}.f64, {}.f64);", ctx.cr(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fcmpo(BuilderContext& ctx) {
  // fcmpo is identical to fcmpu for recompilation purposes.
  // The difference is that fcmpo sets FPSCR exception flags for SNaN operands,
  // which we don't need to emulate.
  return build_fcmpu(ctx);
}

//=============================================================================
// Addition
//=============================================================================

bool build_fadd(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fadd({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fadd", 2, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 + {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fadds(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fadds({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fadds", 2, true);
  if (ctx.arithmetic_any_mode()) {
    ctx.println("\t{}.f64 = rex::ppc::AddSingleAnyMode({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
                ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ctx.println("\t{}.f64 = double(float({}.f64 + {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Subtraction
//=============================================================================

bool build_fsub(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fsub({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fsub", 2, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 - {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fsubs(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fsubs({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fsubs", 2, true);
  if (ctx.arithmetic_any_mode()) {
    ctx.println("\t{}.f64 = rex::ppc::SubSingleAnyMode({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
                ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ctx.println("\t{}.f64 = double(float({}.f64 - {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Multiplication
//=============================================================================

bool build_fmul(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fmul({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fmul", 2, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 * {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fmuls(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fmuls({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fmuls", 2, true);
  if (ctx.arithmetic_any_mode()) {
    ctx.println("\t{}.f64 = rex::ppc::MulSingleAnyMode({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
                ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ctx.println("\t{}.f64 = double(float({}.f64 * {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Division
//=============================================================================

bool build_fdiv(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fdiv({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fdiv", 2, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = {}.f64 / {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

bool build_fdivs(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fdivs({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ExactFpCheck check(ctx, "fdivs", 2, true);
  if (ctx.arithmetic_any_mode()) {
    ctx.println("\t{}.f64 = rex::ppc::DivSingleAnyMode({}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
                ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
    return true;
  }
  ctx.println("\t{}.f64 = double(float({}.f64 / {}.f64));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]));
  return true;
}

//=============================================================================
// Fused Multiply-Add
//=============================================================================

bool build_fmadd(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fmadd({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fmadd", 3, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = std::fma({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmadds(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fmadds({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fmadds", 3, true);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(std::fma({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsub(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fmsub({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fmsub", 3, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = std::fma({}.f64, {}.f64, -{}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fmsubs(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fmsubs({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fmsubs", 3, true);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(std::fma({}.f64, {}.f64, -{}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

// fnmadd / fnmsub: rex::ppc::fnmadd / fnmsub (rex/ppc/fpu_fma.h). tw2de (2026-11-01): the generated
// -std::fma(...) let GCC fold the negation into AArch64 FNMADD / FMSUB, which give +0 where the PowerPC gives -0, and
// negated NaN results (the PowerPC keeps their sign).
bool build_fnmadd(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fnmadd({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fnmadd", 3, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = rex::ppc::fnmadd({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmadds(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fnmadds({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fnmadds", 3, true);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(rex::ppc::fnmadd({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsub(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fnmsub({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fnmsub", 3, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = rex::ppc::fnmsub({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

bool build_fnmsubs(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fnmsubs({}.f64, {}.f64, {}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]), ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ExactFpCheck check(ctx, "fnmsubs", 3, true);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(rex::ppc::fnmsub({}.f64, {}.f64, {}.f64)));",
              ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

//=============================================================================
// Reciprocal and Square Root
//=============================================================================

bool build_fres(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h (the 360's flags)
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fres({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
    return true;
  }
  ExactFpCheck check(ctx, "fres", 1, false);
  ctx.emit_set_flush_mode(false);
  // rex::ppc::fres (rex/ppc/fpu_est.h): the hardware's table estimate on AArch64, double(float(1.0 / x)) elsewhere.
  ctx.println("\t{}.f64 = rex::ppc::fres({}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_frsqrte(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h (the 360's flags)
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::frsqrte({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
    return true;
  }
  ExactFpCheck check(ctx, "frsqrte", 1, false);
  ctx.emit_set_flush_mode(false);
  // rex::ppc::frsqrte (rex/ppc/fpu_est.h): the hardware's table estimate on AArch64, the old expression elsewhere.
  ctx.println("\t{}.f64 = rex::ppc::frsqrte({}.f64);", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrt(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fsqrt({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
    return true;
  }
  ExactFpCheck check(ctx, "fsqrt", 1, false);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = sqrt({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
  return true;
}

bool build_fsqrts(BuilderContext& ctx) {
  if (ctx.exact_fp()) {  // tw2de (2026-11-01): rex/ppc/fpu_exact.h
    ctx.emit_set_flush_mode(false);
    ctx.println("\t{}.f64 = rex::ppc::exact::fsqrts({}.f64);", ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]));
    return true;
  }
  ExactFpCheck check(ctx, "fsqrts", 1, true);
  ctx.emit_set_flush_mode(false);
  ctx.println("\t{}.f64 = double(float(sqrt({}.f64)));", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]));
  return true;
}

//=============================================================================
// Selection
//=============================================================================

bool build_fsel(BuilderContext& ctx) {
  // exact_fp (tw2de 2026-11-01): the bit test also for a signaling NaN, which a quiet compare still flags.
  if (ctx.config().fpComparesModeless || ctx.exact_fp()) {
    // The test on the bits: the same truth table as `>= 0.0` with the flush mode off, in either mode.
    ctx.println("\t{}.f64 = rex::ppc::GeZeroAnyMode({}.f64) ? {}.f64 : {}.f64;",
                ctx.f(ctx.insn.operands[0]), ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
                ctx.f(ctx.insn.operands[3]));
    return true;
  }
  ctx.emit_set_flush_mode(false);
  // tw2de (2026-11-01): a quiet compare (fcmp; `>=` is fcmpe, which raises invalid for a NaN: fsel raises nothing
  // on the 360, Xenia's hardware-recorded fsel. cases). The same truth table.
  ctx.println("\t{}.f64 = __builtin_isgreaterequal({}.f64, 0.0) ? {}.f64 : {}.f64;", ctx.f(ctx.insn.operands[0]),
              ctx.f(ctx.insn.operands[1]), ctx.f(ctx.insn.operands[2]),
              ctx.f(ctx.insn.operands[3]));
  return true;
}

}  // namespace rex::codegen
