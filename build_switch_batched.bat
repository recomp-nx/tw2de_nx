@echo off
rem tw2de (2026-10-29): the Switch build with the generated code in files of about 1 MB (build_switch.py --batched).
rem The code layout of build_switch.py --batched: a few hundred files instead of one a function (a much faster
rem full build). Switching between this layout and the per-function one (build_switch.py without --batched)
rem generates the code again and compiles all of it.
setlocal
python "%~dp0build_switch.py" --batched %* && (echo. & echo build_switch_batched finished: out\tw2de_nx\tw2de_nx.nro & pause & exit /b 0) || (echo. & echo build_switch_batched failed: the reason is above. & pause & exit /b 1)
