@echo off
rem tw2de (2026-11-02): the PowerPC instruction tests as a Switch homebrew, out\ppc_tests\tw2de_ppc_tests.nro
rem (tools\ppc_tests\build_ppc_tests.py: Xenia's tests and this project's own, tools\ppc_tests\fnxf). First the code
rem generator (rexglue) is brought up to date from sdk\ into out\host, as the Switch build does (only what changed is
rem compiled), so the tests check the generator you have now. Tools: DEVKITPRO set, git, cmake, ninja, python and a
rem C compiler (clang, gcc or cc) on PATH. Copy the NRO to the SD card's /switch/ and start it from the homebrew
rem menu; the failures are in sdmc:/switch/tw2de_nx_ppc_tests.txt. Examples:
rem   build_ppc_tests.bat
rem   build_ppc_tests.bat --jobs 8
rem One line: cmd reads a batch file again after each command.
setlocal
python -c "import os, sys; sys.path.insert(0, r'%~dp0.'); import build_switch as b; b.build_codegen_tool(os.cpu_count() or 4)" && python "%~dp0tools\ppc_tests\build_ppc_tests.py" --rexglue "%~dp0out\host\bin\rexglue.exe" %* && (echo. & echo build_ppc_tests finished: out\ppc_tests\tw2de_ppc_tests.nro & pause & exit /b 0) || (echo. & echo build_ppc_tests failed: the reason is above. & pause & exit /b 1)
