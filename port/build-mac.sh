#!/usr/bin/env bash
# Build the SvR 2011 port natively for Apple Silicon Macs (arm64).
# The macOS counterpart of build.ps1.
#
#   ./port/build-mac.sh                  fetch + patch the SDK and plume, codegen, build
#   ./port/build-mac.sh --deploy DIR     ...then copy the program into DIR, a game
#                                        folder installed by the Windows launcher
#
# Run from the repository root. Needs:
#   - Xcode 16 or newer (AppleClang 16+, for C++23):  xcode-select --install
#   - Homebrew:  brew install cmake ninja vulkan-loader molten-vk python
#   - port/assets/default.xex, copied from your own game install
#
# The recompiled game code is generated on your machine from your own
# default.xex; none of it is in the repository.
set -euo pipefail

SDK_COMMIT=c94f5ebdcb3c9d1a460ca48e04f9758448f8d518   # ReXGlue v0.10.0 (as build.ps1)
PLUME_COMMIT=d723793                                    # as port/CMakeLists.txt

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="$ROOT/port"
RECOMP="$ROOT/recomp"
SDK="$RECOMP/rexglue-sdk"
PLUME="$RECOMP/plume"
CONFIG="${CONFIG:-Release}"
ARCH_FLAGS="-mcpu=apple-m1"   # every Apple Silicon chip runs this (M1 baseline)
DEPLOY=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deploy) DEPLOY="$2"; shift 2 ;;
    --debug)  CONFIG=Debug; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# -- Checks -------------------------------------------------------------------
[[ "$(uname -s)" == Darwin ]] || die "this script is for macOS"
[[ "$(uname -m)" == arm64 ]]  || die "this script builds for Apple Silicon (arm64)"
for tool in cmake ninja git clang++ python3; do
  command -v "$tool" >/dev/null || die "$tool not found (see the header of this script)"
done
CLANG_MAJOR="$(clang++ --version | sed -nE 's/.*clang version ([0-9]+).*/\1/p' | head -1)"
[[ -n "$CLANG_MAJOR" && "$CLANG_MAJOR" -ge 16 ]] || die "AppleClang 16+ (Xcode 16+) needed, found: $(clang++ --version | head -1)"
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
[[ -e "$BREW_PREFIX/lib/libMoltenVK.dylib" ]] || die "MoltenVK not found: brew install molten-vk vulkan-loader"
[[ -f "$PORT/assets/default.xex" ]] || die "copy default.xex from your game install into port/assets/ first"

# -- ReXGlue SDK at the port's commit, with its fixes --------------------------
mkdir -p "$RECOMP"
if [[ ! -d "$SDK/.git" ]]; then
  say "Fetching the ReXGlue SDK"
  git clone https://github.com/rexglue/rexglue-sdk "$SDK"
fi
if [[ ! -f "$SDK/.svr2011-patched" ]]; then
  say "Patching the SDK (port/patches/rexglue-sdk-svr2011.patch)"
  git -C "$SDK" checkout -q "$SDK_COMMIT"
  git -C "$SDK" submodule update --init --recursive
  git -C "$SDK" apply --whitespace=nowarn "$PORT/patches/rexglue-sdk-svr2011.patch"
  touch "$SDK/.svr2011-patched"
fi

# -- plume (the native renderer's graphics layer) -----------------------------
if [[ ! -d "$PLUME/.git" ]]; then
  say "Fetching plume"
  git clone https://github.com/renderbag/plume "$PLUME"
fi
if [[ ! -f "$PLUME/.svr2011-patched" ]]; then
  say "Patching plume (port/patches/plume-svr2011.patch)"
  git -C "$PLUME" checkout -q "$PLUME_COMMIT"
  git -C "$PLUME" submodule update --init --recursive
  git -C "$PLUME" apply --whitespace=nowarn "$PORT/patches/plume-svr2011.patch"
  touch "$PLUME/.svr2011-patched"
fi

COMMON_ARGS=(
  -G Ninja
  -DCMAKE_BUILD_TYPE="$CONFIG"
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_OSX_ARCHITECTURES=arm64
  -DCMAKE_C_FLAGS="$ARCH_FLAGS" -DCMAKE_CXX_FLAGS="$ARCH_FLAGS"
  -DCMAKE_PREFIX_PATH="$BREW_PREFIX"
)

# -- The codegen tool (Windows uses the prebuilt rexglue.exe; here it's built) --
REXGLUE_BIN="$SDK/out/build/mac-tool/src/rexglue/rexglue"
if [[ ! -x "$REXGLUE_BIN" ]]; then
  say "Building the ReXGlue codegen tool"
  cmake -S "$SDK" -B "$SDK/out/build/mac-tool" "${COMMON_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$SDK/out/build/mac-tool" --target rexglue
  [[ -x "$REXGLUE_BIN" ]] || REXGLUE_BIN="$(find "$SDK/out/build/mac-tool" -type f -name rexglue -perm -u+x | head -1)"
  [[ -x "$REXGLUE_BIN" ]] || die "built the codegen tool but can't find it"
fi

# -- Generate the game's C++ from your default.xex ----------------------------
if [[ ! -f "$PORT/generated/rexglue.cmake" || "$PORT/assets/default.xex" -nt "$PORT/generated/rexglue.cmake" ]]; then
  say "Generating C++ from default.xex (about a minute)"
  (cd "$PORT" && "$REXGLUE_BIN" codegen svr2011_manifest.toml)
fi

# -- Build ---------------------------------------------------------------------
BUILD="$PORT/out/build/Mac$CONFIG"
if [[ ! -f "$BUILD/build.ninja" ]]; then
  say "Configuring ($CONFIG, arm64)"
  cmake -S "$PORT" -B "$BUILD" "${COMMON_ARGS[@]}" \
    -DREXSDK_DIR="$SDK" -DREXGLUE_USE_VULKAN=ON -DPLUME_DIR="$PLUME"
fi
say "Building (the first build takes a while)"
cmake --build "$BUILD"

# -- Deploy into a game folder -------------------------------------------------
if [[ -n "$DEPLOY" ]]; then
  [[ -d "$DEPLOY" ]] || die "no such folder: $DEPLOY"
  say "Copying the program into $DEPLOY"
  cp -f "$BUILD/svr2011" "$DEPLOY/"
  find "$BUILD" -maxdepth 1 -name '*.dylib' -exec cp -f {} "$DEPLOY/" \;
  # MoltenVK and the Vulkan loader, as the SDK staged them beside the program
  # (vulkan/lib, vulkan/share/vulkan/icd.d): the runtime finds them there itself.
  [[ -d "$BUILD/vulkan" ]] && rsync -a "$BUILD/vulkan/" "$DEPLOY/vulkan/"
  cat > "$DEPLOY/Play SvR2011.command" <<'SH'
#!/bin/sh
# Double-click to play (Vulkan through MoltenVK). The log shows in this window.
cd "$(dirname "$0")"
exec ./svr2011 --gpu_backend=vulkan "$@"
SH
  chmod +x "$DEPLOY/Play SvR2011.command" "$DEPLOY/svr2011"
  xattr -dr com.apple.quarantine "$DEPLOY" 2>/dev/null || true
  say "Done. Double-click \"Play SvR2011.command\" in $DEPLOY"
else
  say "Built: $BUILD/svr2011   (deploy with: ./port/build-mac.sh --deploy \"/path/to/game folder\")"
fi
