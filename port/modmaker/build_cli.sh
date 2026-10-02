#!/bin/sh
# Builds the svrmod test CLI (tools-only, no CMake): port/out/modmaker/svrmod.exe
here=$(dirname "$0")
export PATH="/c/Program Files/LLVM/bin:$PATH"
mkdir -p "$here/../out/modmaker/obj"
# ufbx is big: compile it once
[ -f "$here/../out/modmaker/obj/ufbx.o" ] || clang -O2 -c -o "$here/../out/modmaker/obj/ufbx.o" "$here/third_party/ufbx/ufbx.c"
clang++ -std=c++20 -O2 -Wall -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS \
  -I "$here/../../sdk/rexglue-sdk/thirdparty/stb" -I "$here/third_party/ufbx" \
  -o "$here/../out/modmaker/svrmod.exe" "$here/svrmod_cli.cpp" "$here"/svrfmt/*.cpp \
  "$here/../out/modmaker/obj/ufbx.o" -lole32 -lwindowscodecs
