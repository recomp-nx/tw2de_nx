#!/usr/bin/env python3
"""tw2de (2026-10-23): the whole Switch build in one run, from your own default.xex to the NRO.

    python build_switch.py                  everything (each step skips what is already up to date)
    python build_switch.py --gaps LOG ...   first: add the missing functions a crash log names (see below)
    python build_switch.py -j 8             compile jobs (also --jobs; default: every core)
    python build_switch.py --no-post-passes (2026-11-01, diagnostic) the code generated again and built without the
                                            post-codegen passes (step 3), to tell whether one of them changes what
                                            the game does; the next build without it runs them again
    python build_switch.py --portable-fp   (2026-11-01, diagnostic) the vector and float instruction helpers in their
                                            portable form (as on x86 / Xenia) instead of the AArch64 ones
    python build_switch.py --regen          generate the code from default.xex again even when its stamp says it is
                                            up to date (rexglue codegen --ignore-stamp)
    python build_switch.py --pgo generate   the profiling build (out/switch-pgo -> out/tw2de_nx/tw2de_nx_pgo.nro): play it; it
                                            writes its counts (one run_<time>.fxpg file per run) to
                                            /switch/tw2de_nx/pgo on the SD card when closed.
                                            Copy that folder's files to pgo/ (in the project).
    python build_switch.py --codegen-only   steps 1-3 only (below): the generated code the Switch build compiles, from
                                            app/assets/default.xex; DEVKITPRO not needed
    python build_switch.py --batched        the generated code in files of about 1 MB (many functions each) instead
                                            of one file a function: far fewer files to compile. Kept until a build
                                            without it (see "Code layout" below)
    python build_switch.py --pgo use        the release built with the counts in pgo/ (out/switch, as always): the
                                            runs are unpacked into out/pgo-gcda first (several are merged)
    python build_switch.py --opt O2 --lto   (2026-10-30) every target at -O2 (or --opt O3) and/or link-time
                                            optimisation (with -fipa-pta): a build of its own in out/switch-o2-lto
                                            (out/switch-o3, out/switch-lto ...: the options in the name) ->
                                            out/tw2de_nx/tw2de_nx_o2_lto.nro beside the usual tw2de_nx.nro (both
                                            can be on the SD card in /switch/tw2de_nx and share its data).
                                            Without them: -O3, no LTO, as always. With PGO: an -O3 profile (pgo/,
                                            --pgo generate) fits --opt O3 --lto with --pgo use
                                            (pgo_use_switch_o3_lto_batched.bat); -O2 has profiles of its own:
                                            --pgo generate --opt O2 (its runs go to /switch/tw2de_nx/pgo-o2 on the
                                            SD card, into pgo-o2/ in the project), then --pgo use --opt O2 --lto.

Steps:
  1. The code generator (rexglue), a PC program, built from sdk/ into out/host (only the first time, or when its
     sources change): the SDK's REXGLUE_CODEGEN_ONLY mode, the tool and the part of the runtime it loads a XEX with
     (no SDL3, FFmpeg or graphics). Works on a Windows host and on a Linux one (WSL included).
  2. Codegen: app/assets/default.xex (yours; never part of the project) -> app/generated/default.
  3. The post-codegen passes proven exact for the Switch (tools/post_codegen_switch.py).
  4. The Switch build: devkitA64 into out/switch, then out/tw2de_nx/tw2de_nx.nro.
  Then out/tw2de_nx is the SD card's /switch/tw2de_nx as the port needs it (2026-10-26): the NRO (and lang/, a
  project's language packs, when it has any). The project's tex/ (the controller pictures) is inside the NRO, in its
  romfs (2026-11-06). Copy it to the SD card's /switch (the game's files, shaders and saves there are not touched).

Nothing is looked up at a fixed path: the tools come from your environment.
  DEVKITPRO        devkitPro's root (devkitA64, libnx, portlibs). Required.
  cmake, ninja     on PATH.
  CC / CXX         the PC compiler for step 1 (optional: CMake's own detection otherwise; on Windows run this from a
                   shell where your compiler is set up, e.g. a Visual Studio developer prompt, or set CC/CXX to clang).
The game's shaders are not in the NRO (the console translates and compiles them); neither are Renut's own: their
GLSL (app/src/renut_engine/gpu/shaders/deko3d) is compiled on the console too. So no shader tool is needed here.

Code layout (tw2de 2026-10-29): by default each recompiled function is a file of its own
(generated/default/tw2de_recomp_<address>.cpp, about 40,000): a function's file and lines stay the same when others
change, so a profile (PGO) stays valid for every function whose code did not change, and a change recompiles only its
files. --batched writes them in files of about 1 MB (tw2de_recomp.<n>.cpp, a few hundred): far fewer compiler
starts, a much faster full build, for when the code is settled. Switching regenerates the code and compiles all of it.
A profile follows the layout of the build that made it: runs made with the per-function profiling build do not fit a
--batched release (its objects have other names): for a --batched release with PGO, make its runs with
--pgo generate --batched (and keep only those in pgo/). The project's .bat files (build_switch_batched.bat,
pgo_generate_switch_batched.bat, pgo_use_switch_o3_lto_batched.bat) all build with --batched.

Missing functions (the game stops with "Call to invalid or unregistered function at guest address 0x..."):
  python build_switch.py --gaps <path to tw2de_XXX.log> [more logs]
runs tools/runtime_gap_resolver.py on the logs first; it adds to app/overrides.toml the one address the log proves
missing (an address inside a stretch of code no function claims), then the run regenerates and rebuilds. When a log
names several, it lists them and adds none: pick one with --gap-address 0x... (the resolver's rule: one per run).
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))  # this script is at the project's root
APP = os.path.join(ROOT, 'app')
SDK = os.path.join(ROOT, 'sdk')
OUT = os.path.join(ROOT, 'out')
SD_OUT = os.path.join(OUT, 'tw2de_nx')        # tw2de (2026-10-26): /switch/tw2de_nx on the SD card
SD_FOLDERS = ('lang',)                         # the project's folders copied there with the NRO (tw2de, 2026-11-06:
                                               # tex/ is in the NRO's romfs now, app/CMakeLists.txt)
HOST_BUILD = os.path.join(OUT, 'host')
HOST_BIN = os.path.join(HOST_BUILD, 'bin')
SWITCH_BUILD = os.path.join(OUT, 'switch')
PGO_BUILD = os.path.join(OUT, 'switch-pgo')  # tw2de (2026-10-24): --pgo generate
PGO_DIR = os.path.join(ROOT, 'pgo')          # the counts copied back from the SD card (--pgo use): an input, so in the
                                             # project, not in out/ (tw2de, 2026-10-26; was out/pgo)
PGO_GCDA = os.path.join(OUT, 'pgo-gcda')     # tw2de (2026-10-25): pgo/'s run_*.fxpg unpacked (tools/pgo_unpack.py)
XEX = os.path.join(APP, 'assets', 'default.xex')
GEN = os.path.join(APP, 'generated', 'default')
TOOLCHAIN = os.path.join(ROOT, 'tools', 'switch', 'cmake', 'switch-devkitA64.cmake')
EXE = '.exe' if os.name == 'nt' else ''
# tw2de (2026-10-26): the code generator already built (a kit: tools/make_kit.py puts it here): used as it is,
# step 1 skipped, so no PC compiler is needed.
PREBUILT_REXGLUE = os.path.join(ROOT, 'bin', 'rexglue' + EXE)


CMAKE_POSIX = False  # an MSYS2 / Cygwin cmake (set by check_environment): it takes /d/x, not D:/x


def cmake_path(path):
    """A path in the form the cmake on PATH understands. A native cmake takes C:/x; an MSYS2 or Cygwin one (a
    /usr/bin/cmake, Git Bash's or MSYS2's) reads C:/x as a relative path and needs /c/x (Cygwin: /cygdrive/c/x)."""
    if not path.startswith('/') and not re.match(r'^[A-Za-z]:[\\/]', path):
        path = os.path.abspath(path)
    m = re.match(r'^([A-Za-z]):[\\/](.*)$', path)
    if CMAKE_POSIX and m:
        rest = m.group(2).replace('\\', '/')
        prefix = '/cygdrive/' if CMAKE_POSIX == 'cygwin' else '/'
        return prefix + m.group(1).lower() + '/' + rest
    return path.replace('\\', '/')


def cmake_flavour():
    """'native', 'msys' or 'cygwin': what CMAKE_HOST_SYSTEM_NAME the cmake on PATH reports."""
    script = os.path.join(OUT, 'cmake_host_probe.cmake')
    os.makedirs(OUT, exist_ok=True)
    with open(script, 'w') as f:
        f.write('message(STATUS "host=${CMAKE_HOST_SYSTEM_NAME}")\n')
    r = subprocess.run(['cmake', '-P', os.path.basename(script)], cwd=OUT, capture_output=True, text=True)
    text = (r.stdout + r.stderr).lower()
    if 'host=msys' in text or 'host=mingw' in text:
        return 'msys'
    if 'host=cygwin' in text:
        return 'cygwin'
    return 'native'


def step(text):
    print('\n=== ' + text, flush=True)


def fail(text):
    print('\nbuild_switch: ' + text, file=sys.stderr, flush=True)
    sys.exit(1)


def run(cmd, cwd=None, env=None):
    print('> ' + ' '.join('"%s"' % c if ' ' in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd, cwd=cwd, env=env)
    if r.returncode != 0:
        fail('failed (exit %d): %s' % (r.returncode, ' '.join(cmd)))


def check_environment(need_devkitpro=True):
    step('Checking the environment')
    missing = [t for t in ('cmake', 'ninja') if shutil.which(t) is None]
    if missing:
        fail('not on PATH: ' + ', '.join(missing))
    global CMAKE_POSIX
    flavour = cmake_flavour()
    if flavour != 'native':
        CMAKE_POSIX = flavour
        print('cmake on PATH is an %s build: paths are given to it as %s' % (flavour.upper(), cmake_path(ROOT)))
    dkp = os.environ.get('DEVKITPRO')
    if not dkp and need_devkitpro:
        fail('DEVKITPRO is not set (devkitPro\'s root folder)')
    if not need_devkitpro:
        pass  # tw2de (2026-10-26): --codegen-only builds nothing for the Switch
    elif os.name == 'nt' and dkp.startswith('/') and not os.path.isdir(dkp):
        # devkitPro's MSYS2 shell sets DEVKITPRO=/opt/devkitpro: its Windows form is in DEVKITPRO's MSYS2 root, which
        # python on Windows cannot see; the files are checked by cmake instead.
        print('DEVKITPRO=%s is an MSYS2 path: its files are left to cmake to find' % dkp)
    else:
        gxx = os.path.join(dkp, 'devkitA64', 'bin', 'aarch64-none-elf-g++' + EXE)
        if not os.path.isfile(gxx):
            fail('no devkitA64 compiler at %s (DEVKITPRO=%s)' % (gxx, dkp))
        for need in (os.path.join(dkp, 'libnx', 'include', 'switch.h'),
                     os.path.join(dkp, 'portlibs', 'switch', 'lib', 'libz.a')):
            if not os.path.isfile(need):
                fail('missing %s (devkitPro packages: switch-dev, switch-zlib and the other portlibs the build links)'
                     % need)
    if not os.path.isfile(XEX):
        fail('no %s: put the game\'s default.xex there (yours; it is never part of the project)' % XEX)
    # tw2de (2026-11-09): FF XIII-2's zone DLL modules beside it (the manifest's [[modules]]).
    for line in open(os.path.join(APP, MANIFEST), encoding='utf-8'):
        m = re.match(r'\s*file_path\s*=\s*"(assets/[^"]+)"', line)
        if m and not os.path.isfile(os.path.join(APP, m.group(1))):
            fail('no app/%s: put the game\'s %s there (from the disc, beside default.xex)' %
                 (m.group(1), os.path.basename(m.group(1))))
    print('cmake %s, ninja %s, DEVKITPRO %s, python %s' % (shutil.which('cmake'), shutil.which('ninja'), dkp,
                                                            sys.executable))
    return dkp


def visual_studio():
    """The newest Visual Studio with the C++ tools: (installation folder, its vcvars64.bat), or None. Found with
    vswhere, which the VS Installer always keeps in its own folder."""
    installer = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Microsoft Visual Studio',
                             'Installer', 'vswhere.exe')
    if not os.path.isfile(installer):
        return None
    r = subprocess.run([installer, '-latest', '-products', '*', '-requires',
                        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                       capture_output=True, text=True)
    vs = r.stdout.strip().splitlines()[0] if r.returncode == 0 and r.stdout.strip() else ''
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvars64.bat') if vs else ''
    return (vs, vcvars) if vcvars and os.path.isfile(vcvars) else None


def vcvars_env(vcvars):
    """The environment vcvars64.bat sets up (MSVC's headers and libraries, which clang-cl uses too)."""
    r = subprocess.run('"%s" >nul && set' % vcvars, shell=True, capture_output=True, text=True)
    if r.returncode != 0:
        fail('%s failed' % vcvars)
    env = {}
    for line in r.stdout.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def llvm_from_registry():
    """LLVM for Windows installed but not on PATH (its installer adds it only when asked, winget's never): the
    installer records its folder in the registry (HKLM\\SOFTWARE\\LLVM\\LLVM, default value). Its clang-cl, or None."""
    try:
        import winreg
    except ImportError:
        return None
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                with winreg.OpenKey(hive, r'SOFTWARE\LLVM\LLVM', 0, winreg.KEY_READ | view) as key:
                    folder = winreg.QueryValueEx(key, '')[0]
            except OSError:
                continue
            exe = os.path.join(folder, 'bin', 'clang-cl.exe')
            if os.path.isfile(exe):
                print('LLVM found in the registry (not on PATH): %s' % folder)
                return exe
    return None


def clang_major(exe):
    """The Clang major version of an executable (0 when it does not say)."""
    try:
        r = subprocess.run([exe, '--version'], capture_output=True, text=True)
        m = re.search(r'clang version (\d+)', r.stdout + r.stderr)
        return int(m.group(1)) if m else 0
    except OSError:
        return 0


def host_env():
    """The environment for the PC part (the code generator's build). The SDK takes Clang 18 or newer only (sdk/
    CMakeLists.txt), with MSVC's headers and libraries on Windows. CC / CXX set: yours, as they are. Otherwise, on
    Windows, with the Visual Studio environment loaded when it is not (no INCLUDE): clang-cl from PATH, or the one
    Visual Studio installs with its "C++ Clang tools" (VC/Tools/Llvm/x64/bin), or devkitPro's MSYS2 clang in the
    MSVC ABI (what the former PC build used). Elsewhere: CMake's own choice."""
    env = dict(os.environ)
    if env.get('CC') or env.get('CXX') or os.name != 'nt':
        return env
    vs = visual_studio()
    if not env.get('INCLUDE'):
        if vs is None:
            fail('no Visual Studio with the C++ tools (its headers and libraries are what clang-cl builds against): '
                 'install Visual Studio Build Tools with the C++ workload, or set CC and CXX to your clang')
        print('loading the Visual Studio environment: %s' % vs[1])
        env = vcvars_env(vs[1])
    clang = shutil.which('clang-cl', path=env.get('PATH') or env.get('Path'))
    if not clang:
        clang = llvm_from_registry()
    if not clang and vs:
        bundled = os.path.join(vs[0], 'VC', 'Tools', 'Llvm', 'x64', 'bin', 'clang-cl.exe')
        clang = bundled if os.path.isfile(bundled) else None
    if clang:
        major = clang_major(clang)
        if major and major < 18:
            fail('%s is Clang %d; the SDK needs Clang 18 or newer (update LLVM, or set CC and CXX to a newer clang)'
                 % (clang, major))
        print('compiler for the code generator: %s (Clang %s)' % (clang, major or '?'))
        env['CC'] = env['CXX'] = clang
        return env
    # The project's former PC build: devkitPro's MSYS2 clang (msys2/clang64) driving the MSVC ABI,
    # with Visual Studio's headers and libraries and lld-link. Found from DEVKITPRO, not a fixed path.
    dkp = env.get('DEVKITPRO', '')
    bin_dir = os.path.join(dkp, 'msys2', 'clang64', 'bin') if dkp and not dkp.startswith('/') else ''
    if bin_dir and os.path.isfile(os.path.join(bin_dir, 'clang++.exe')):
        print('compiler for the code generator: %s (MSVC ABI)' % bin_dir)
        env['CC'] = os.path.join(bin_dir, 'clang.exe')
        env['CXX'] = os.path.join(bin_dir, 'clang++.exe')
        env['REX_HOST_EXTRA'] = ';'.join([
            '-DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc', '-DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc',
            '-DCMAKE_LINKER_TYPE=LLD', '-DCMAKE_RC_COMPILER=' + cmake_path(os.path.join(bin_dir, 'llvm-rc.exe')),
            '-DCMAKE_AR=' + cmake_path(os.path.join(bin_dir, 'llvm-ar.exe'))])
        return env
    fail('no Clang for the code generator (the SDK needs Clang 18 or newer): add "C++ Clang tools for Windows" in the '
         'Visual Studio Installer, or set CC and CXX to your clang')


HOST_UAM_BUILD = os.path.join(OUT, 'host-uam')
UAM_SRC = os.path.join(APP, 'thirdparty', 'uam')


def uam_host_env():
    """The environment and extra CMake arguments for uam's PC build, or (None, []). On Windows: CC / CXX if set, else
    devkitPro's MSYS2 clang64 (clang), mingw64 or ucrt64 (gcc), linked -static (none of MSYS2's DLLs needed when the
    Switch build runs it), else the code generator's clang-cl (host_env) with the static C runtime."""
    env = dict(os.environ)
    if os.name != 'nt':
        return env, []
    if env.get('CC') or env.get('CXX'):
        return env, ['-DCMAKE_EXE_LINKER_FLAGS=-static']
    dkp = env.get('DEVKITPRO', '')
    if dkp.startswith('/') and shutil.which('cygpath'):
        # DEVKITPRO=/opt/devkitpro (devkitPro's MSYS2 shell): its Windows form from MSYS2's own cygpath
        r = subprocess.run(['cygpath', '-w', dkp], capture_output=True, text=True)
        dkp = r.stdout.strip() if r.returncode == 0 else dkp
    if not dkp or dkp.startswith('/'):
        return None, []
    for sub, cc, cxx in (('clang64', 'clang', 'clang++'), ('mingw64', 'gcc', 'g++'), ('ucrt64', 'gcc', 'g++'),
                         ('clang64', 'gcc', 'g++')):
        bin_dir = os.path.join(dkp, 'msys2', sub, 'bin')
        if os.path.isfile(os.path.join(bin_dir, cxx + EXE)):
            print('compiler for uam: %s (MinGW, %s)' % (os.path.join(bin_dir, cxx + EXE), sub))
            # its own tools first on PATH (the compiler's helpers and DLLs while it runs)
            env['PATH'] = bin_dir + os.pathsep + env.get('PATH', env.get('Path', ''))
            env['CC'] = os.path.join(bin_dir, cc + EXE)
            env['CXX'] = os.path.join(bin_dir, cxx + EXE)
            return env, ['-DCMAKE_EXE_LINKER_FLAGS=-static']
    # tw2de (2026-11-07): no MinGW: the code generator's compiler (clang-cl with Visual Studio's headers; uam's
    # threads have an MSVC branch, mesa-imported/c11/threads.h), with the static C runtime (/MT): the program needs no
    # Visual Studio DLLs when the Switch build runs it outside that environment.
    try:
        env = host_env()
    except SystemExit:
        return None, []
    extra = [a for a in env.pop('REX_HOST_EXTRA', '').split(';') if a]
    return env, extra + ['-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded']


def build_host_uam(jobs):
    """tw2de (2026-11-06): the project's uam (app/thirdparty/uam: the compiler the console runs) built as a PC
    program, out/host-uam/uam(.exe): the host shaders' DKSH in the NRO's romfs are made with it (gpu.cmake,
    TW2DE_UAM), the same bytes a console compile makes. devkitPro's uam is not used: it made wrong code for FSR 3's
    accumulate and prepare_inputs passes (constant values read as 0 in some half-warps). Not fatal: without a PC
    compiler (or if it does not build) the path is '' and the console compiles those shaders at their first use."""
    step('The shader compiler for the NRO (uam, a PC program)')
    env, extra = uam_host_env()
    if env is None:
        print('build_switch: WARNING: no PC compiler for uam (MinGW, clang-cl, or CC and CXX): the host shaders are '
              'compiled on the console at their first use', flush=True)
        return ''
    exe = os.path.join(HOST_UAM_BUILD, 'uam' + EXE)
    try:
        if not os.path.isfile(os.path.join(HOST_UAM_BUILD, 'build.ninja')):
            cmd = ['cmake', '-S', cmake_path(UAM_SRC), '-B', cmake_path(HOST_UAM_BUILD), '-G', 'Ninja',
                   '-DCMAKE_BUILD_TYPE=Release']
            for var, cache in (('CC', 'CMAKE_C_COMPILER'), ('CXX', 'CMAKE_CXX_COMPILER')):
                if env.get(var):
                    cmd.append('-D%s=%s' % (cache, cmake_path(env[var]) if os.sep in env[var] or '/' in env[var]
                                            else env[var]))
            run(cmd + extra, env=env)
        # -k 0: every compile error of a failed build at once (ninja)
        run(['cmake', '--build', cmake_path(HOST_UAM_BUILD), '--target', 'uam_tool', '-j', str(jobs), '--', '-k', '0'],
            env=env)
    except SystemExit:
        print('build_switch: WARNING: uam (the PC program) did not build: the host shaders are compiled on the '
              'console at their first use', flush=True)
        return ''
    if not os.path.isfile(exe):
        print('build_switch: WARNING: built, but no %s: the host shaders are compiled on the console' % exe, flush=True)
        return ''
    return exe


def build_codegen_tool(jobs, label='1/4 The code generator (rexglue, a PC program)'):
    step(label)
    env = host_env()
    if not os.path.isfile(os.path.join(HOST_BUILD, 'build.ninja')):  # a failed configure writes a cache, not this
        # REXGLUE_CODEGEN_ONLY (sdk/cmake/rexglue_codegen_only.cmake): the code generator and the part of the runtime
        # it loads a XEX with, about 180 compile steps; no SDL3, FFmpeg, Vulkan, audio, graphics or input.
        cmd = ['cmake', '-S', cmake_path(SDK), '-B', cmake_path(HOST_BUILD), '-G', 'Ninja',
               '-DCMAKE_BUILD_TYPE=Release', '-DREXGLUE_CODEGEN_ONLY=ON', '-DREX_OUTPUT_DIR=' + cmake_path(HOST_BIN),
               '-DPython3_EXECUTABLE=' + cmake_path(sys.executable)]
        for var, cache in (('CC', 'CMAKE_C_COMPILER'), ('CXX', 'CMAKE_CXX_COMPILER')):
            if env.get(var):
                cmd.append('-D%s=%s' % (cache, cmake_path(env[var]) if os.sep in env[var] or '/' in env[var] else env[var]))
        # The SDK's byte-swap helpers (sdk/src/core/memory.cpp) use SSSE3 intrinsics: x86-64-v2, as the PC builds
        # always had (the former PC build). Added to CFLAGS / CXXFLAGS (CMake's starting flags), yours kept; clang-cl takes
        # it as clang does.
        if platform.machine().lower() in ('amd64', 'x86_64'):
            for var in ('CFLAGS', 'CXXFLAGS'):
                env[var] = (env.get(var, '') + ' -march=x86-64-v2').strip()
        cmd += [a for a in env.pop('REX_HOST_EXTRA', '').split(';') if a]
        run(cmd, env=env)
    env.pop('REX_HOST_EXTRA', None)
    run(['cmake', '--build', cmake_path(HOST_BUILD), '--target', 'rexglue', '-j', str(jobs)], env=env)
    path = os.path.join(HOST_BIN, 'rexglue' + EXE)
    if not os.path.isfile(path):
        fail('built, but no %s' % path)
    return path


MANIFEST = 'tw2de_manifest.toml'
LAYOUT_FILE = os.path.join(GEN, 'code_layout.txt')  # tw2de (2026-10-29): the layout the generated code has


def codegen(rexglue, ignore_stamp=False, batched=False):
    layout = 'batched' if batched else 'per-function'
    previous = open(LAYOUT_FILE).read().strip() if os.path.isfile(LAYOUT_FILE) else ''
    switched = bool(previous) and previous != layout
    step('2/4 Codegen from your default.xex (%s files)' % layout +
         (' (--regen: the stamp ignored)' if ignore_stamp else '') +
         (' (was %s: generated again)' % previous if switched else ''))
    manifest = MANIFEST
    if batched:
        # tw2de (2026-10-29): the manifest with one_function_per_file off, beside it (its paths are relative to its
        # folder), for this run only: the code generator then fills files of max_file_size_bytes (1 MB).
        with open(os.path.join(APP, MANIFEST), encoding='utf-8') as f:
            text = f.read()
        text, n = re.subn(r'(?m)^one_function_per_file\s*=\s*true\s*$', 'one_function_per_file = false', text)
        if n != 1:
            fail('%s: no "one_function_per_file = true" line to turn off for --batched' % MANIFEST)
        manifest = MANIFEST.replace('.toml', '.batched.toml')
        with open(os.path.join(APP, manifest), 'w', encoding='utf-8') as f:
            f.write('# Made by build_switch.py --batched from %s for one codegen run; deleted after it.\n' % MANIFEST)
            f.write(text)
    try:
        run([rexglue, 'codegen', manifest] + (['--ignore-stamp'] if ignore_stamp or switched else []), cwd=APP)
    finally:
        if manifest != MANIFEST:
            try:
                os.remove(os.path.join(APP, manifest))
            except OSError:
                pass
    if not os.path.isfile(os.path.join(GEN, 'tw2de_init.cpp')):
        fail('codegen wrote no %s' % os.path.join(GEN, 'tw2de_init.cpp'))
    with open(LAYOUT_FILE, 'w') as f:
        f.write(layout + '\n')


def post_codegen():
    step('3/4 The post-codegen passes proven for the Switch')
    # tw2de (2026-11-09): the game's DLL modules (app/generated/z0070 ...) get the same passes as default.xex's
    # code: the store cards above all (Renut trusts a page whose card is clean, whichever module wrote it).
    gen_root = os.path.dirname(GEN)
    for name in sorted(os.listdir(gen_root)):
        d = os.path.join(gen_root, name)
        if os.path.isdir(d) and any(f.endswith('_pch.h') for f in os.listdir(d)):
            run([sys.executable, os.path.join(ROOT, 'tools', 'post_codegen_switch.py'), '--gen', d])


def write_symbols(dkp, elf, out):
    """tw2de (2026-10-26): the ELF's functions as '<hex address> <name>' lines, by address (devkitA64's nm,
    demangled; the recompiled functions' __imp__ twins left out: same address as their sub_). Read by the game
    (renut_engine/gpu/backend/memory_report.cpp) at its first memory report."""
    nm = os.path.join(dkp, 'devkitA64', 'bin', 'aarch64-none-elf-nm' + ('.exe' if os.name == 'nt' else ''))
    if not os.path.isfile(elf) or not os.path.isfile(nm):
        print('symbols: no %s or no %s: none written' % (elf, nm))
        return
    r = subprocess.run([nm, '-n', '-C', '--defined-only', elf], capture_output=True, text=True, errors='replace')
    lines = []
    for line in r.stdout.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in ('T', 't', 'W', 'w') and not parts[2].startswith('__imp__'):
            lines.append('%x %s' % (int(parts[0], 16), parts[2][:160]))
    with open(out, 'w', newline='\n', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('symbols: %d functions -> %s' % (len(lines), out))


def gcov_version_name(stamp):
    """GCC's profile version word ("B61*") as a version ("16.1")."""
    if len(stamp) != 4 or not stamp[1:3].isdigit():
        return stamp
    return '%d.%s' % ((ord(stamp[0]) - ord('A')) * 10 + int(stamp[1]), stamp[2])


def profile_gcc_version(pgo_dir=PGO_DIR):
    """The GCC version the first run in pgo/ (or pgo_dir) was made with ("16.1"), or '' (none, or not readable)."""
    import struct
    for n in sorted(os.listdir(pgo_dir)) if os.path.isdir(pgo_dir) else []:
        if not n.endswith('.fxpg'):
            continue
        with open(os.path.join(pgo_dir, n), 'rb') as f:
            data = f.read(1 << 16)
        if data[:4] != b'FXPG' or len(data) < 12:
            continue
        (name_len,) = struct.unpack_from('<I', data, 12)
        image = 16 + name_len + 4  # the first object's .gcda image: magic, version, ...
        if len(data) >= image + 8:
            (word,) = struct.unpack_from('<I', data, image + 4)
            return gcov_version_name(struct.pack('>I', word).decode('ascii', 'replace'))
    return ''


def devkita64_gcov_version(dkp):
    """devkitA64's GCC as "major.minor" (the part a profile's version word holds), or ''."""
    gcc = os.path.join(dkp, 'devkitA64', 'bin', 'aarch64-none-elf-gcc' + EXE)
    try:
        v = subprocess.run([gcc, '-dumpfullversion'], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ''
    parts = v.split('.')
    return '%s.%s' % (parts[0], parts[1]) if len(parts) >= 2 else ''


def tool_version(cmd):
    """A tool's first line of --version output, or 'not found'."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        out = (r.stdout or r.stderr).strip().splitlines()
        return out[0].strip() if out else 'no output'
    except OSError:
        return 'not found'


def pacman_db(dkp):
    """devkitPro's pacman database of installed packages (one folder per package), or None."""
    for db in (os.path.join(dkp, 'pacman', 'var', 'lib', 'pacman', 'local'),
               os.path.join(dkp, 'msys2', 'var', 'lib', 'pacman', 'local'),
               os.path.join(os.path.dirname(dkp), 'msys2', 'var', 'lib', 'pacman', 'local')):
        if os.path.isdir(db):
            return db
    return None


def norm_path(p):
    return os.path.normpath(p).replace('\\', '/').lower()


def used_devkitpro_packages(dkp, build):
    """tw2de (2026-10-26): the devkitPro packages this build used, not every one installed: the owners (pacman's
    file lists) of the files under DEVKITPRO it read: the headers every object was compiled with (ninja's
    dependencies), the libraries the link named (build.ninja and its response files: paths, and -l names found in the
    library folders), and the compiler, linker, C and C++ libraries themselves. ([], why) when it cannot tell."""
    import glob
    db = pacman_db(dkp)
    if not db:
        return [], 'pacman\'s database not found under %s' % dkp
    root = norm_path(dkp)
    owner = {}
    for pkg in os.listdir(db):
        files = os.path.join(db, pkg, 'files')
        if not os.path.isfile(files):
            continue
        with open(files, encoding='utf-8', errors='replace') as f:
            for line in f:
                line = line.strip()
                if line.startswith('opt/devkitpro/') and not line.endswith('/'):
                    owner[root + '/' + line[len('opt/devkitpro/'):].lower()] = pkg
    used = set()
    # The headers: every object's dependencies as ninja recorded them.
    try:
        r = subprocess.run(['ninja', '-C', build, '-t', 'deps'], capture_output=True, text=True, errors='replace')
        for line in r.stdout.splitlines():
            if line.startswith((' ', '\t')):
                p = norm_path(line.strip())
                if p.startswith(root):
                    used.add(p)
    except OSError:
        return [], 'ninja not found'
    # The libraries: what the link lines name.
    lib_dirs = [os.path.join(dkp, 'libnx', 'lib'), os.path.join(dkp, 'portlibs', 'switch', 'lib')]
    texts = []
    for f in [os.path.join(build, 'build.ninja')] + glob.glob(os.path.join(build, 'CMakeFiles', '*.rsp')):
        if os.path.isfile(f):
            with open(f, encoding='utf-8', errors='replace') as fh:
                texts.append(fh.read())
    for text in texts:
        for tok in text.replace('"', ' ').split():
            if tok.startswith('-l') and len(tok) > 2:
                for d in lib_dirs:
                    cand = os.path.join(d, 'lib%s.a' % tok[2:])
                    if os.path.isfile(cand):
                        used.add(norm_path(cand))
            elif tok.endswith('.a') and norm_path(tok).startswith(root):
                used.add(norm_path(tok))
    # The toolchain: the compiler and linker, newlib's C library, libstdc++ and libgcc, libnx (switch.specs links it).
    a64 = os.path.join(dkp, 'devkitA64')
    for pattern in ('bin/aarch64-none-elf-gcc*', 'bin/aarch64-none-elf-ld*', 'aarch64-none-elf/lib/libc.a',
                    'aarch64-none-elf/lib/libstdc++.a', 'lib/gcc/aarch64-none-elf/*/libgcc.a'):
        used.update(norm_path(p) for p in glob.glob(os.path.join(a64, pattern)))
    used.add(norm_path(os.path.join(dkp, 'libnx', 'lib', 'libnx.a')))
    packages = sorted({owner[p] for p in used if p in owner})
    return packages, '%d files under DEVKITPRO used, %d of them in a package' % (len(used),
                                                                               sum(p in owner for p in used))


def tree_hash(paths, skip=()):
    """SHA-256 of the files under 'paths' (project-relative), names and contents, in a fixed order."""
    import hashlib
    h = hashlib.sha256()
    for top in paths:
        full = os.path.join(ROOT, top)
        files = [full] if os.path.isfile(full) else []
        for folder, dirs, names in os.walk(full):
            dirs[:] = sorted(d for d in dirs if d not in ('__pycache__', '.git'))
            files += [os.path.join(folder, n) for n in sorted(names)]
        for f in files:
            rel = os.path.relpath(f, ROOT).replace(os.sep, '/')
            if any(rel.startswith(sk) for sk in skip):
                continue
            h.update(rel.encode() + b'\0')
            with open(f, 'rb') as fh:
                h.update(fh.read())
    return h.hexdigest()


def write_build_info(dkp, path, pgo, profile_id, build):
    """tw2de (2026-10-26): <nro>.build.txt: the build's mode, the tools and libraries, and fingerprints of what it
    compiled (the sources, the game's executable, the generated code), so it can be built again the same way."""
    import datetime
    import hashlib
    import platform as _platform
    lines = ['# tw2de build info (build_switch.py): what %s was built from and with.' % os.path.basename(
        path[:-len('.build.txt')] + '.nro'),
             '# A profile (pgo/) made with a profiling build fits only code compiled the same way: the same GCC',
             '# version, the same libraries (headers inlined into the code), the same sources and default.xex.', '',
             'date: ' + datetime.datetime.now().strftime('%Y-%m-%d %H:%M'),
             'mode: ' + {'generate': 'profiling build (--pgo generate)',
                         'use': 'release with the profile (--pgo use), profile id %s' % (profile_id or '-')}.get(
                             pgo, 'release'),
             '', '[tools]',
             'host: %s %s (%s), python %s' % (_platform.system(), _platform.release(), _platform.machine(),
                                             _platform.python_version()),
             'devkitA64 gcc: ' + tool_version([os.path.join(dkp, 'devkitA64', 'bin', 'aarch64-none-elf-gcc' + EXE),
                                               '--version']),
             'devkitA64 ld: ' + tool_version([os.path.join(dkp, 'devkitA64', 'bin', 'aarch64-none-elf-ld' + EXE),
                                              '--version']),
             'cmake: ' + tool_version(['cmake', '--version']),
             'ninja: ' + tool_version(['ninja', '--version']),
             'rexglue: ' + (os.path.basename(PREBUILT_REXGLUE) + ' (prebuilt, bin/)' if os.path.isfile(PREBUILT_REXGLUE)
                            else 'built from sdk/') + ', ' + tool_version(
                                [PREBUILT_REXGLUE if os.path.isfile(PREBUILT_REXGLUE)
                                 else os.path.join(HOST_BIN, 'rexglue' + EXE), '--version']),
             '', '[devkitPro packages used]']
    packages, how = used_devkitpro_packages(dkp, build)
    lines += ['# the packages this build read files of: ' + how] + (packages or ['(could not be told)'])
    sha = hashlib.sha256()
    if os.path.isfile(XEX):
        with open(XEX, 'rb') as f:
            sha.update(f.read())
    lines += ['', '[inputs: SHA-256]',
              'default.xex: ' + (sha.hexdigest() if os.path.isfile(XEX) else 'missing'),
              'generated code (app/generated/default): ' + tree_hash(['app/generated/default']),
              'project sources (app/src, app/cmake, app/CMakeLists.txt, app/*.toml, sdk/src, sdk/include, sdk/cmake, '
              'tools/switch): ' + tree_hash(['app/src', 'app/cmake', 'app/CMakeLists.txt', 'app/tw2de_manifest.toml',
                                             'app/overrides.toml', 'sdk/src', 'sdk/include', 'sdk/cmake',
                                             'tools/switch'])]
    with open(path, 'w', newline='\n', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print('build info -> %s' % path)


def warn_profile_layout(profile):
    """tw2de (2026-10-29): a profile whose generated objects have the other layout's names (see "Code layout"):
    said, as GCC would only report each object without counts."""
    layout = open(LAYOUT_FILE).read().strip() if os.path.isfile(LAYOUT_FILE) else 'per-function'
    per_function = batched = 0
    for _, _, names in os.walk(profile):
        for n in names:
            if re.search(r'_recomp_[0-9A-Fa-f]{8}\.cpp', n):
                per_function += 1
            elif re.search(r'_recomp\.\d+\.cpp', n):
                batched += 1
    if layout == 'batched' and per_function and not batched:
        print('pgo: WARNING: the runs in pgo/ were made with the per-function code, this build is --batched: the '
              'game code gets no counts (make runs with --pgo generate --batched)')
    elif layout != 'batched' and batched and not per_function:
        print('pgo: WARNING: the runs in pgo/ were made with --batched code, this build is per-function: the game '
              'code gets no counts (make runs with --pgo generate, without --batched)')


def variant_name(opt, lto):
    """tw2de (2026-10-30): 'o2-lto', 'o3', 'lto' ...; '' for the usual build."""
    return '-'.join(([opt.lower()] if opt else []) + (['lto'] if lto else []))


def pgo_paths(opt):
    """tw2de (2026-10-30): the profile's folders for an optimisation level: the project's runs, their unpacked
    counts (out/), and the SD card's folder the profiling build writes its runs to. -O3 (the usual level, and
    --opt O3): pgo/, out/pgo-gcda, /switch/tw2de_nx/pgo, as always. -O2: pgo-o2/, out/pgo-gcda-o2,
    /switch/tw2de_nx/pgo-o2: a profile fits only code compiled at the level it was made with (GCC places its counters
    after the early optimisations, which the level changes), so the two are kept apart."""
    if opt == 'O2':
        return os.path.join(ROOT, 'pgo-o2'), os.path.join(OUT, 'pgo-gcda-o2'), '/switch/tw2de_nx/pgo-o2'
    return PGO_DIR, PGO_GCDA, '/switch/tw2de_nx/pgo'


def build_switch(dkp, jobs, configure_only=False, pgo='', opt='', lto=False, portable_fp=False, uam=''):
    pgo_dir, pgo_gcda, pgo_sd = pgo_paths(opt)
    if pgo == 'generate':
        # tw2de (2026-10-30): the profiling build at the level only: the counters are placed when each file is
        # compiled, before link-time optimisation (a profile made without LTO fits the LTO release of the same level),
        # and an instrumented whole-program link would only be slower. --opt O3 is the usual profiling build.
        if lto:
            print('pgo: --lto is not used for the profiling build (its profile fits the --lto release all the same)')
        lto = False
        opt = 'O2' if opt == 'O2' else ''
    variant = variant_name(opt, lto)
    step('4/4 The Switch build' + (' (--pgo %s)' % pgo if pgo else '') +
         (' (%s)' % variant if variant else ''))
    build = PGO_BUILD if pgo == 'generate' else SWITCH_BUILD
    # tw2de (2026-10-30): --opt / --lto in a folder of their own (the objects differ: nothing to share with the
    # usual build), out/switch-o2-lto (the profiling build: out/switch-pgo-o2).
    if variant:
        build += '-' + variant
    # tw2de (2026-10-24): the profile mode the folder was configured with (cmake's cache keeps TW2DE_PGO): a
    # different one configures it again (out/switch goes from a release with counts to one without and back).
    mode = 'generate' if pgo == 'generate' else ('use' if pgo == 'use' else '')
    # tw2de (2026-10-25): the SDK's libraries in the build folder (<build>/sdk-lib), not the shared sdk/out/switch-arm64:
    # the profiling build's instrumented libraries were linked into the release (undefined __gcov_init). The stamp
    # names the folder too, so a folder configured before this is configured again (the libraries archived again from
    # its own objects; nothing is recompiled).
    sdk_lib = os.path.join(build, 'sdk-lib')
    want = mode + ' sdk-lib ' + (variant or 'o3') + ' ' + pgo_sd
    stamp = os.path.join(build, 'tw2de_pgo_mode.txt')
    old_mode = open(stamp).read().strip() if os.path.isfile(stamp) else ''
    if pgo == 'use' and not os.path.isdir(pgo_dir):
        fail('--pgo use: no %s (copy %s from the SD card there after playing the --pgo generate%s build)'
             % (pgo_dir, pgo_sd, ' --opt O2' if opt == 'O2' else ''))
    # tw2de (2026-10-26): a profile from another GCC version (each .gcda holds its compiler's version: "B61*" is
    # GCC 16.1) would be refused file by file by this compiler (and gcov-tool could not merge it): the release is
    # built without it instead, said once.
    if pgo == 'use':
        theirs, ours = profile_gcc_version(pgo_dir), devkita64_gcov_version(dkp)
        if theirs and ours and theirs != ours:
            print('pgo: the runs in %s were made with GCC %s, this devkitA64 is GCC %s: they cannot be used; the '
                  'release is built without a profile (make new runs with this devkitA64\'s profiling build)'
                  % (pgo_dir, theirs, ours))
            pgo, mode = '', ''
            want = mode + ' sdk-lib ' + (variant or 'o3') + ' ' + pgo_sd
    profile = pgo_dir
    profile_id = ''
    if pgo == 'use' and any(n.endswith('.fxpg') for n in os.listdir(pgo_dir)):
        # tw2de (2026-10-26): unpacked (and merged) only when pgo/ changed: the runs' names and contents, and
        # the unpacker itself, make the profile's id, kept in out/pgo-gcda. The id also goes into the build's flags
        # (TW2DE_PGO_PROFILE_ID): a new profile recompiles everything with it (GCC's profiles are not in ninja's
        # dependencies: without it, a new profile reached only the files changed anyway), the same one nothing.
        import hashlib
        h = hashlib.sha1()
        for n in sorted(os.listdir(pgo_dir)):
            if n.endswith('.fxpg'):
                h.update(n.encode() + b'\n')
                with open(os.path.join(pgo_dir, n), 'rb') as f:
                    h.update(f.read())  # the contents: copied again from the SD card unchanged, the same id
        for n in ('functions.txt.gz', os.path.join('..', 'tools', 'pgo_unpack.py'),
                  os.path.join('..', 'tools', 'pgo_functions.py')):
            if os.path.isfile(os.path.join(pgo_dir, n)):
                with open(os.path.join(pgo_dir, n), 'rb') as f:
                    h.update(f.read())
        profile_id = h.hexdigest()[:16]
        id_file = os.path.join(pgo_gcda, 'profile_id.txt')
        old_id = open(id_file).read().strip() if os.path.isfile(id_file) else ''
        if old_id == profile_id:
            print('pgo: the profile in %s is up to date (id %s): not unpacked again' % (pgo_gcda, profile_id))
        else:
            sys.path.insert(0, os.path.join(ROOT, 'tools'))
            import pgo_unpack
            tool = os.path.join(dkp, 'devkitA64', 'bin', 'aarch64-none-elf-gcov-tool' + EXE)
            if not os.path.isfile(tool):
                tool = shutil.which('aarch64-none-elf-gcov-tool')
            try:
                sys.path.insert(0, os.path.join(ROOT, 'tools'))
                import pgo_functions
                # the folder this build's compiler sees (CMake's, from its cache; this project's when not configured)
                root = pgo_functions.project_root(SWITCH_BUILD) or ROOT.replace('\\', '/')
                pgo_unpack.unpack(pgo_dir, pgo_gcda, tool, root)
            except (ValueError, OSError) as e:
                fail('--pgo use: %s' % e)
            with open(id_file, 'w') as f:
                f.write(profile_id + '\n')
            print('pgo: a new profile (id %s): everything is compiled again with it' % profile_id)
        profile = pgo_gcda
        warn_profile_layout(profile)
    want += ' ' + profile + (' ' + profile_id if profile_id else '')
    want += ' portable-fp' if portable_fp else ''  # tw2de (2026-11-01): a change reconfigures
    want += ' uam=' + uam  # tw2de (2026-11-06): the PC uam (build_host_uam), or none: a change reconfigures
    if not os.path.isfile(os.path.join(build, 'build.ninja')) or want != old_mode:
        run(['cmake', '-S', cmake_path(APP), '-B', cmake_path(build), '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TOOLCHAIN_FILE=' + cmake_path(TOOLCHAIN),
             '-DDEVKITPRO=' + cmake_path(dkp), '-DREXSDK_DIR=' + cmake_path(SDK),
             '-DPython3_EXECUTABLE=' + cmake_path(sys.executable), '-DPYTHON_EXECUTABLE=' + cmake_path(sys.executable),
             '-DTW2DE_PGO=' + mode, '-DTW2DE_PGO_DIR=' + cmake_path(profile),
             '-DTW2DE_PGO_PROFILE_ID=' + profile_id,
             '-DTW2DE_OPT=' + (opt or ''), '-DTW2DE_LTO=' + ('ON' if lto else 'OFF'),
             '-DTW2DE_PORTABLE_FP=' + ('ON' if portable_fp else 'OFF'),
             '-DTW2DE_PGO_RUN_DIR=' + pgo_sd,
             '-DTW2DE_UAM=' + (cmake_path(uam) if uam else ''),
             '-DREX_OUTPUT_DIR=' + cmake_path(sdk_lib)])
        os.makedirs(build, exist_ok=True)
        open(stamp, 'w').write(want + '\n')
    if configure_only:
        print('\nConfigured: %s (--configure-only)' % build)
        return
    run(['cmake', '--build', cmake_path(build), '-j', str(jobs)])
    nro = os.path.join(build, 'tw2de.nro')
    if not os.path.isfile(nro):
        fail('the build finished but wrote no %s' % nro)
    os.makedirs(SD_OUT, exist_ok=True)
    final = os.path.join(SD_OUT, ('tw2de_nx_pgo' if pgo == 'generate' else 'tw2de_nx') +
                         ('_' + variant.replace('-', '_') if variant else '') + '.nro')
    shutil.copyfile(nro, final)
    # tw2de (2026-10-26): the ELF's function names beside the NRO (<nro>.sym), for the game's memory report: it names
    # the places of its large heap blocks itself (log_diagnostics), so the log alone says where the memory goes.
    write_symbols(dkp, os.path.join(build, 'tw2de.elf'), os.path.splitext(final)[0] + '.sym')
    # tw2de (2026-10-26): what this NRO was built from and with (<nro>.build.txt), to build it again the same way
    # (a profile made with it fits only code compiled the same way).
    write_build_info(dkp, os.path.splitext(final)[0] + '.build.txt', pgo, profile_id, build)
    # tw2de (2026-10-26): the profiling build's functions (tools/pgo_functions.py) beside the runs in pgo/: with
    # them the release finds every function of the profile wherever it is built (in another folder too).
    if pgo == 'generate':
        sys.path.insert(0, os.path.join(ROOT, 'tools'))
        import pgo_functions
        pgo_functions.collect(build, os.path.join(pgo_dir, 'functions.txt.gz'))
    # tw2de (2026-10-26): the language packs beside it, as on the SD card. tw2de (2026-11-06): tex/ is in the
    # NRO now: the old copy goes from out/tw2de_nx.
    if os.path.isdir(os.path.join(SD_OUT, 'tex')):
        shutil.rmtree(os.path.join(SD_OUT, 'tex'))
    for name in SD_FOLDERS:
        src, dst = os.path.join(ROOT, name), os.path.join(SD_OUT, name)
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
            print('%s -> %s (%d files)' % (name, dst, sum(len(f) for _, _, f in os.walk(dst))))
    print('\nDone: %s (%.1f MB)' % (final, os.path.getsize(final) / 1048576.0))


def resolve_gaps(logs, address):
    step('Missing functions from the log(s)')
    cmd = [sys.executable, os.path.join(ROOT, 'tools', 'runtime_gap_resolver.py')]
    for log in logs:
        cmd += ['--log', log]
    cmd.append('--apply')
    if address:
        cmd += ['--address', address]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        fail('nothing was added to app/overrides.toml (the resolver said why, above); nothing else was built')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4, help='compile jobs (default: every core)')
    ap.add_argument('--gaps', nargs='+', metavar='LOG', help='crash logs: add the missing function they prove first')
    ap.add_argument('--gap-address', help='with --gaps: the one address to add when a log names several')
    ap.add_argument('--configure-only', action='store_true', help='stop after configuring the Switch build')
    ap.add_argument('--codegen-only', action='store_true',
                    help='steps 1-3 only: the code generator, the code from app/assets/default.xex and its passes '
                         '(app/generated/default): what the Switch build compiles; no Switch build, no DEVKITPRO')
    ap.add_argument('--regen', action='store_true',
                    help='generate the code from default.xex again even if up to date (rexglue --ignore-stamp)')
    ap.add_argument('--batched', action='store_true',
                    help='the generated code in files of about 1 MB instead of one a function (faster full builds)')
    ap.add_argument('--pgo', choices=('generate', 'use'),
                    help='profile-guided optimisation: the profiling build, or the release with its counts (pgo/)')
    ap.add_argument('--opt', choices=('O2', 'O3'),
                    help='every target at this optimisation level (a build of its own: out/switch-o2...)')
    ap.add_argument('--no-post-passes', action='store_true',
                    help='diagnostic: the code generated again and built without the post-codegen passes (step 3); '
                         'the next build without it runs them again')
    ap.add_argument('--portable-fp', action='store_true',
                    help='diagnostic: the vector / float instruction helpers in their portable (x86, Xenia-like) form '
                         'instead of the AArch64 ones (rex/ppc vmx_fp.h, fpu_est.h, intrinsics.h, context.h); the '
                         'whole game code compiles again')
    ap.add_argument('--lto', action='store_true',
                    help='link-time optimisation of the whole NRO (a build of its own: out/switch-...-lto)')
    a = ap.parse_args()
    dkp = check_environment(need_devkitpro=not a.codegen_only)
    if os.path.isfile(PREBUILT_REXGLUE):
        step('1/4 The code generator: already built (%s)' % PREBUILT_REXGLUE)
        rexglue = PREBUILT_REXGLUE
    else:
        rexglue = build_codegen_tool(a.jobs)
    if a.gaps:
        # The resolver measures the CURRENT generated code: make sure there is one (the first run has none yet).
        if not os.path.isfile(os.path.join(GEN, 'tw2de_init.cpp')):
            codegen(rexglue, batched=a.batched)
        resolve_gaps(a.gaps, a.gap_address)
    # tw2de (2026-11-01): --no-post-passes: a fresh codegen (the tree on disk has had the passes), left as generated.
    codegen(rexglue, a.regen or a.no_post_passes, a.batched)
    if a.no_post_passes:
        step('3/4 The post-codegen passes: skipped (--no-post-passes)')
    else:
        post_codegen()
    if a.codegen_only:
        print('\nDone: %s holds the generated code; a build without --codegen-only compiles the Switch '
              'release from it' % GEN)
        return
    uam = build_host_uam(a.jobs)
    build_switch(dkp, a.jobs, a.configure_only, a.pgo or '', a.opt or '', a.lto, a.portable_fp, uam)


if __name__ == '__main__':
    main()
