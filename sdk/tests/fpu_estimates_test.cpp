// fres / frsqrte (rex/ppc/fpu_est.h) against the results recorded on real PowerPC hardware in Xenia's test suite
// (src/xenia/cpu/ppc/testing/instr__gen_fres.s, instr__gen_frsqrte.s). tw2de (2026-10-24): frsqrte is the hardware's
// table (xenia-edge's) and must match every case; fres is xenia-edge's 1 / float(x), not the hardware's 12-bit table:
// its differences are counted, not failed. AArch64 build, e.g.:
//   aarch64-linux-gnu-g++ -std=gnu++23 -O2 -static -Iinclude tests/fpu_estimates_test.cpp && qemu-aarch64 ./a.out
#include <rex/ppc/fpu_est.h>
#include <cstdio>
struct Case { bool rsqrt; uint64_t in, out; };
static const Case kCases[] = {
    {false, 0x0000000000000000, 0x7FF0000000000000}, {false, 0x8000000000000000, 0xFFF0000000000000},
    {false, 0x0000000000000001, 0x7FF0000000000000}, {false, 0x000FFFFFFFFFFFFF, 0x7FF0000000000000},
    {false, 0x3FF0000000000000, 0x3FEFE00000000000}, {false, 0xBFF0000000000000, 0xBFEFE00000000000},
    {false, 0xC1E0000000000000, 0xBDFFE00000000000}, {false, 0x41DFFFFFFFC00000, 0x3E00000000000000},
    {false, 0x7FF0000000000000, 0x0000000000000000}, {false, 0xFFF0000000000000, 0x8000000000000000},
    {false, 0xFFF8000000000000, 0xFFF8000000000000}, {false, 0xFFF4000000000000, 0xFFFC000000000000},
    {true, 0x0000000000000000, 0x7FF0000000000000},  {true, 0x8000000000000000, 0xFFF0000000000000},
    {true, 0x0000000000000001, 0x617F100000000000},  {true, 0x000FFFFFFFFFFFFF, 0x5FE0800000000000},
    {true, 0x3FF0000000000000, 0x3FEF100000000000},  {true, 0xBFF0000000000000, 0x7FF8000000000000},
    {true, 0xC1E0000000000000, 0x7FF8000000000000},  {true, 0x41DFFFFFFFC00000, 0x3EF7000000000000},
    {true, 0x7FF0000000000000, 0x0000000000000000},  {true, 0xFFF0000000000000, 0x7FF8000000000000},
    {true, 0xFFF8000000000000, 0xFFF8000000000000},  {true, 0xFFF4000000000000, 0xFFFC000000000000},
};
int main() {
  int bad = 0;
  int rsqrt_total = 0, res_total = 0, res_differ = 0;
  for (const Case& t : kCases) {
    const double x = rex::ppc::fpu_est::from_bits(t.in);
    const uint64_t r = rex::ppc::fpu_est::bits(t.rsqrt ? rex::ppc::frsqrte(x) : rex::ppc::fres(x));
    (t.rsqrt ? rsqrt_total : res_total)++;
    if (r != t.out) {
      if (t.rsqrt) {
        ++bad;
        std::printf("frsqrte %016llX: %016llX, hardware %016llX\n", (unsigned long long)t.in, (unsigned long long)r,
                    (unsigned long long)t.out);
      } else {
        ++res_differ;
      }
    }
  }
  std::printf("frsqrte: %d/%d as the hardware; fres: %d/%d as the hardware\n", rsqrt_total - bad, rsqrt_total,
              res_total - res_differ, res_total);
  return bad != 0;
}
