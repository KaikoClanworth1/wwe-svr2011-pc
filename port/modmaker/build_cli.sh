#!/bin/sh
# Builds the svrmod test CLI (tools-only, no CMake): port/out/modmaker/svrmod.exe
here=$(dirname "$0")
export PATH="/c/Program Files/LLVM/bin:$PATH"
mkdir -p "$here/../out/modmaker/obj"
# ufbx is big: compile it once
[ -f "$here/../out/modmaker/obj/ufbx.o" ] || clang -O2 -c -o "$here/../out/modmaker/obj/ufbx.o" "$here/third_party/ufbx/ufbx.c"
# LZX for WWE '13 files (svrfmt/wwe13): libmspack's decoder from the SDK
mspack="$here/../../sdk/rexglue-sdk/thirdparty/libmspack/libmspack/mspack"
[ -f "$here/../out/modmaker/obj/lzxd.o" ] || clang -O2 -w -D_CRT_SECURE_NO_WARNINGS -c -I "$mspack" \
  -o "$here/../out/modmaker/obj/lzxd.o" "$mspack/lzxd.c"
clang++ -std=c++20 -O2 -Wall -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS \
  -I "$here/../../sdk/rexglue-sdk/thirdparty/stb" -I "$here/third_party/ufbx" -I "$mspack" \
  -o "$here/../out/modmaker/svrmod.exe" "$here/svrmod_cli.cpp" "$here"/svrfmt/*.cpp \
  "$here/../out/modmaker/obj/ufbx.o" "$here/../out/modmaker/obj/lzxd.o" -lole32 -lwindowscodecs
