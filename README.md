# tw2de_nx
Static Recompilation of The Witcher 2: Definitive Edition 0.0.1.2 for Nintendo Switch

No game files are in this repository.

## Additions over the Xbox 360 release

A settings menu at the first start (smash A button after launching homebrew to show it again), with separate graphics settings for handheld and docked:

- Frame interval: 1 to 4 display refreshes a frame (2, the default, is 30 FPS; 1 is 60 FPS at 60 Hz)
- Resolution of the 3D scene: 1024x576, 1280x672 (the game's own, the default), 1280x720, 1600x900, 1920x1080
- Anti-aliasing: off, FXAA (the game's own, the default) or AMD FSR 3.1
- Upscaler: bilinear (the default), Lanczos or AMD FSR 1
- Dynamic resolution
- Camera speed: 30 to 100 %
- The list of achievements
- 1920x1080 output when docked on a 1080p TV
- The Switch's buttons in the game's interface (the button pictures and the controller picture)

## Known bugs:
- Game may randomly not start when docked to 1080p TV
- Achievements texts are extracted only from language you choose first time, to fix
- Disc 2 not supported (to fix once someone will send issue report with save before prompt)

## Performance

With the default settings and clocks of 1020 MHz (CPU), 460 MHz (GPU) and 1331 MHz (RAM), the tutorial's dock runs at 30 FPS. It is recommended though to use higher clocks for getting locked 30 FPS experience.

## Required game files

The Xbox 360 release with Title ID `4E4D0855` and Media ID `6FEB834A`, with title update 1.

This release has audio in English, German and French, and subtitles in English, German, French, Spanish, Italian and Polish. You can add more languages from other Xbox 360 releases. More below.

## Tools (Windows)
```
    winget install Kitware.CMake
    winget install Ninja-build.Ninja
    winget install Python.Python.3.14
    winget install LLVM.LLVM --version 20.1.0
    winget install Microsoft.VisualStudio.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Git (`winget install Git.Git`) is needed only by `build_ppc_tests.bat`.

devkitPro is not in winget. Install it with its installer (https://github.com/devkitPro/installer/releases,
"Switch Development"), then in the devkitPro MSYS2 shell:

    pacman -S switch-dev switch-zlib switch-zstd

Set `DEVKITPRO` to devkitPro's folder in its Windows form, then open a new command prompt:

    setx DEVKITPRO C:/devkitPro

## Building

1. Put `default.xex` (Disc 1) and `default.xexp` (the extracted title update 1, `disc001/default.xexp`) into
   `app/assets/`.
2. Run `build_switch_batched.bat`. The result is the folder `out/tw2de_nx`.
3. Copy `out/tw2de_nx` to the SD card's `/switch`.

## Game data

On the SD card, into `/switch/tw2de_nx/assets`:

1. Copy the contents of Disc 1.
2. Copy the contents of Disc 2, skipping every file that is already there under the same name.
3. Copy `default.xexp` from the extracted title update 1 (`disc001/default.xexp`).

To add another audio language, extract its `.w2speech` file (for example `pl0.w2speech`) from an Xbox 360 release
that has that audio and copy it to `assets/Data`.

The first start compiles the game's shaders on the console.

## Best performance

1. Build with `build_switch_batched.bat`, start it on the console and let the shader pre-pass finish.
2. Build with `pgo_generate_switch_batched.bat` and start `tw2de_nx_pgo.nro` on the console.
3. Let the intro video play for 30 seconds, go through the main menu to a new game, choose the tutorial, play the
   whole tutorial, then close the game.
4. Copy the `run_*.fxpg` files from the SD card's `/switch/tw2de_nx/pgo` to `pgo/` in the project's root.
5. Build with `pgo_use_switch_o3_lto_batched.bat`. The final NRO is `out/tw2de_nx/tw2de_nx_o3_lto.nro`.

## Third-party code

- [reNut](https://github.com/masterspike52/reNut): the project this one started from, and its renderer
- [renut_nx_ultimate_base](https://github.com/ChanseyIsTheBest/renut_nx_ultimate_base): optimization passes
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (`sdk/` is a fork of it), itself derived from [Xenia](https://github.com/xenia-project/xenia)
- [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (reblue's fork): shader translation (modified version is stored in this repository)
- [plume](https://github.com/renderbag/plume): the rendering back end (modified version is stored in this repository)
- [uam](https://github.com/devkitPro/uam), with Mesa's GLSL compiler: the deko3d shader compiler (modified version is stored in this repository)
- AMD FidelityFX Super Resolution 3.1 (modded version) and 1.0
- PowerPC instruction tests of [Xenia Canary](https://github.com/xenia-canary/xenia-canary)
- FFmpeg, fmt, spdlog, toml++, inja, nlohmann/json, CLI11, SIMDe, libmspack, o1heap, xxHash, zstd, stb, utfcpp,
  tiny-AES-c, aes_128, smol-v, volk, Vulkan-Headers, VulkanMemoryAllocator
- devkitPro's devkitA64, libnx and deko3d (not in this repository)

Each keeps its own licence in its folder.

## Thanks

- masterspike52 for reNut
- Tom Clay and the ReXGlue contributors
- the Xenia and Xenia Canary teams
- hedge-dev for XenonRecomp and XenosRecomp
- renderbag for plume
- fincs for deko3d and uam
- devkitPro and switchbrew for devkitA64 and libnx
- StevenSND for the Horizon layer of the ReXGlue fork
- AMD for FidelityFX Super Resolution
- Claude for porting this
