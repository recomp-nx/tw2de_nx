/* tw2de (2026-11-07): force-included (/FI) in the MSVC-ABI Windows build of uam (clang-cl: the uam PC tool,
 * build_switch.py build_host_uam). windows.h once, first, then its macros that are names in Mesa and nv50_ir (near,
 * far, small, interface; min/max through NOMINMAX) undefined: later includes of windows.h are no-ops (its guard), so
 * they stay undefined. And POSIX's strtok_r, which the MSVC library calls strtok_s. */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef near
#undef far
#undef small
#undef interface
#include <string.h>
#ifndef strtok_r
#define strtok_r strtok_s
#endif
