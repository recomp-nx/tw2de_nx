// tw2de (2026-10-21): profile-guided optimisation on the console. An NRO built with TW2DE_PGO_GENERATE
// (-fprofile-generate, cmake -DTW2DE_PGO=generate) counts every branch and call; the counts of the whole run are
// written to one file on the SD card, /switch/tw2de_nx/pgo/run_<start time>.fxpg, when the game closes (pgo_switch.cpp,
// 2026-10-25; 2026-10-26: no longer every 2 minutes too). The folder is copied back to the PC (pgo/ in the project) and the release built with
// -fprofile-use (build_switch.py --pgo use unpacks the runs into .gcda files first).
#pragma once

#if TW2DE_PGO_GENERATE
namespace tw2de {

// The counts so far, to /switch/tw2de_nx/pgo/run_<start time>.fxpg; with 'part', to run_<start time>.<part>.fxpg
// (2026-10-26: "prepass", after the shader pre-pass: tools/pgo_unpack.py takes that part out of the run's counts and
// adds it back scaled down, so the compiler's counts never set the scale of the game's hot code).
void PgoDump(const char *part = nullptr);
void StartPgoDumper();

}  // namespace tw2de
#endif
