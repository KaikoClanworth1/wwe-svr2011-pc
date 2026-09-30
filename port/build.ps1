# Build the SvR 2011 port and optionally deploy to "Game Files".
#   .\build.ps1            configure + build against the ReXGlue SDK source tree
#                          (..\recomp\rexglue-sdk with patches\rexglue-sdk-svr2011.patch
#                          applied: the runtime fixes this game needs)
#   .\build.ps1 -Prebuilt  build against the unpatched prebuilt SDK (reference only)
#   .\build.ps1 -Trace     developer build with the D3D call census
#                          (out\build\SourceReleaseTrace; see src\d3d_trace.h)
#   .\build.ps1 -Deploy    ... then copy the program files into ..\Game Files
param([switch]$Deploy, [switch]$Prebuilt, [switch]$Trace, [string]$Config = "Release")
# Native tools write progress to stderr, so failures are judged by exit code.
$ErrorActionPreference = "Continue"

$root = $PSScriptRoot
$env:PATH = "C:\Program Files\LLVM\bin;$(Join-Path $root '..\recomp\bin');$env:PATH"
if (-not $Prebuilt) {
    $build   = Join-Path $root ("out\build\Source$Config" + $(if ($Trace) { "Trace" } else { "" }))
    # (with the SDK's Vulkan backend too: --gpu_backend=vulkan, the native renderer's Vulkan path)
    $sdkArgs = @(("-DREXSDK_DIR=" + (Resolve-Path (Join-Path $root "..\recomp\rexglue-sdk"))), "-DREXGLUE_USE_VULKAN=ON")
    if ($Trace) { $sdkArgs += "-DSVR2011_D3D_TRACE=ON" }
} else {
    $build   = Join-Path $root "out\build\$Config"
    $sdkArgs = @("-DCMAKE_PREFIX_PATH=" + (Resolve-Path (Join-Path $root "..\recomp\sdk\win-amd64")))
}

if (-not (Test-Path "$build\build.ninja")) {
    cmake -S $root -B $build -G Ninja `
        "-DCMAKE_BUILD_TYPE=$Config" `
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" @sdkArgs
    if ($LASTEXITCODE) { throw "configure failed" }
}
cmake --build $build 2>&1 | Out-Host
if ($LASTEXITCODE) { throw "build failed" }

if ($Deploy) { & (Join-Path $root "deploy.ps1") -Build $build }
