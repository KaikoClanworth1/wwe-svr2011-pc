@echo off
rem The debug / test suite build (docs/DEBUG_SUITE_PLAN.md): out\build\DebugSuite\svr2011_dbg.exe - never a release.
rem Configured as SourceRelease (clang, Ninja, Release, x86-64-v2) plus SVR2011_DEBUG_SUITE=ON; built at low
rem priority, 2 jobs. Usage: tools\dev\build_dbg.cmd [ninja target, default svr2011]
setlocal
set PORT=%~dp0..\..
set B=%PORT%\out\build\DebugSuite
set T=%~1
if "%T%"=="" set T=svr2011
set PATH=C:\Program Files\LLVM\bin;%PORT%\..\recomp\bin;%PATH%
if not exist "%B%\build.ninja" (
  cmake -S "%PORT%" -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DREXSDK_DIR="%PORT%\..\recomp\rexglue-sdk" ^
    -DSVR2011_DEBUG_SUITE=ON -DREXGLUE_USE_VULKAN=ON || exit /b 1
)
start "" /low /wait /b ninja -C "%B%" -j 2 %T%
