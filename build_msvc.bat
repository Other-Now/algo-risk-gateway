@echo off
rem Build every target with MSVC directly (no CMake needed). Output in build\.
call "%~dp0msvc_env.bat"
if not exist "%~dp0build" mkdir "%~dp0build"
cd /d "%~dp0build"
set FLAGS=-nologo -std:c++20 -O2 -EHsc -W4 -permissive- -D_CRT_SECURE_NO_WARNINGS -I..\include
cl %FLAGS% ..\tests\arg_tests.cpp  -Fe:arg_tests.exe  -Fo:arg_tests.obj  || exit /b 1
cl %FLAGS% ..\src\sim_main.cpp     -Fe:arg_sim.exe    -Fo:arg_sim.obj    || exit /b 1
cl %FLAGS% ..\src\replay_main.cpp  -Fe:arg_replay.exe -Fo:arg_replay.obj || exit /b 1
cl %FLAGS% ..\src\recon_main.cpp   -Fe:arg_recon.exe  -Fo:arg_recon.obj  || exit /b 1
cl %FLAGS% ..\bench\arg_bench.cpp  -Fe:arg_bench.exe  -Fo:arg_bench.obj  || exit /b 1
