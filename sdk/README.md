# ReXGlue SDK fork with the Horizon (Nintendo Switch) layer

This is a fork of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 (commit `c94f5eb`) used by the
Mass Effect Xbox 360 to Nintendo Switch static-recompilation port. It adds:

- **Horizon layer** (`src/**/*_switch.*`, `src/ui/switch_*.cpp`, `src/input/switch/`, `src/audio/switch/`,
  `cmake/rexglue_switch.cmake`, `switch_compat/`): guest memory, exception handling, clock, threading, filesystem,
  input, audio, presentation and performance profile for devkitA64 + libnx. The Horizon layer is by StevenSND.
- **Codegen options** of the `rexglue` tool that shrink and speed up the recompiled C++ (arguments passed in
  registers, "diet" lowerings of single PPC instructions, registers as C++ locals, sharded registration, and so on).
  They are selected from the game's TOML configuration; see `src/codegen/codegen_flags.*` and
  `include/rex/codegen/config.h`.
- Console tuning of the Vulkan graphics backend, XMA audio, the VFS read caches and the kernel I/O path. The cvars
  added by the fork are prefixed `masseffect_` (for example `masseffect_io_ranges_mb`).

## Builds (tw2de, 2026-10-29)

Only two builds remain; the PC builds of a game (SDL3, Vulkan, D3D12, their shader tools, Tracy, the tests' Catch2)
are gone, with their sources:

- the Switch runtime, through the title's CMake (`app/` of this project; `build_switch.py` configures it with
  `tools/switch/cmake/switch-devkitA64.cmake`);
- the host code generator, `rexglue` (Windows or Linux):

      cmake -S . -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DREXGLUE_CODEGEN_ONLY=ON \
            -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
      cmake --build out/host --target rexglue

`thirdparty/` holds only the files these two builds use (and each library's licence).

## Licensing

BSD-3-Clause, see `LICENSE`. All upstream and third-party copyright notices are kept intact. The original upstream
README is in `README.upstream.md`.
