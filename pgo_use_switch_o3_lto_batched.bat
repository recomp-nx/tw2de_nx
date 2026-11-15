@echo off
rem tw2de (2026-10-30): the release with the profile in pgo/ at -O3 with link-time optimisation, the generated code in files of about 1 MB (a much faster full
rem build). A profile fits only the layout it was made with: use the _batched profiling and release files together.
rem Switching between batched and per-function regenerates the code and compiles all of it.
setlocal
python "%~dp0build_switch.py" --batched --pgo use --opt O3 --lto %* && (echo. & echo pgo_use_switch_o3_lto_batched finished: out\tw2de_nx\tw2de_nx_o3_lto.nro & pause & exit /b 0) || (echo. & echo pgo_use_switch_o3_lto_batched failed: the reason is above. & pause & exit /b 1)
