#!/bin/sh
# Builds the svrmod test CLI (tools-only, no CMake): port/out/modmaker/svrmod.exe
here=$(dirname "$0")
export PATH="/c/Program Files/LLVM/bin:$PATH"
mkdir -p "$here/../out/modmaker"
clang++ -std=c++20 -O2 -Wall -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS \
  -I "$here/../../recomp/rexglue-sdk/thirdparty/stb" \
  -o "$here/../out/modmaker/svrmod.exe" "$here/svrmod_cli.cpp" "$here"/svrfmt/*.cpp \
  -lole32 -lwindowscodecs
