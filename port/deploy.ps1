# Refresh "..\Game Files" - a complete, runnable install:
#   the disc files (from "..\Extract GameFiles") + the port's program files.
# User data there (svr2011.toml, UserData\, launcher.ini) is left alone.
param([string]$Build = "")
$ErrorActionPreference = "Stop"

$root  = $PSScriptRoot
$top   = Split-Path $root -Parent
# Default: the build against the SDK source tree (build.ps1's normal build);
# out\build\Release is the unpatched prebuilt-SDK reference build.
$build = if ($Build) { $Build } else { Join-Path $root "out\build\SourceRelease" }
$game  = Join-Path $top "Game Files"
$disc  = Join-Path $top "Extract GameFiles"

New-Item -ItemType Directory -Force $game | Out-Null

# Disc files: only copies what is new or changed.
robocopy $disc $game /E /XO /NFL /NDL /NJH /NP /R:1 /W:1 | Out-Host
if ($LASTEXITCODE -ge 8) { throw "copying the disc files failed" }
# Menu text: "Xbox LIVE" -> "Online" (rebuilt from the disc's string.pac).
python (Join-Path $root "tools\patch_strings.py") $disc $game
if ($LASTEXITCODE) { throw "patching the menu text failed" }
# MY WWE -> OPTIONS gets a GRAPHICS entry (the port opens its own page).
python (Join-Path $root "tools\patch_menu.py") $disc $game
if ($LASTEXITCODE) { throw "patching the menus failed" }
$global:LASTEXITCODE = 0

# Program files. The SDK's DLLs reach the build folder only when the game
# relinks, so a rebuilt SDK DLL is taken from the SDK's output when newer.
$sdkOut = Join-Path $top "recomp\rexglue-sdk\out\win-amd64"
foreach ($dll in @(Get-ChildItem $build -File -Filter "rex*.dll")) {
    $fresh = Join-Path $sdkOut $dll.Name
    if ((Test-Path $fresh) -and (Get-Item $fresh).LastWriteTime -gt $dll.LastWriteTime) {
        Copy-Item $fresh $dll.FullName -Force
    }
}
$files = @(Get-ChildItem $build -File | Where-Object { $_.Extension -in ".exe", ".dll" })
foreach ($f in $files) { Copy-Item $f.FullName $game -Force }
Copy-Item (Join-Path $root "dist\Read Me.txt") $game -Force -ErrorAction SilentlyContinue
# The native renderer's shaders, as in the release zip.
$shaders = Join-Path $root "runs\shaders_native\dxil"
if (Test-Path (Join-Path $shaders "present.vs.dxil")) {
    New-Item -ItemType Directory -Force (Join-Path $game "native_shaders") | Out-Null
    Get-ChildItem $shaders -File | Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $game "native_shaders") -Force
    # (and the Vulkan backend's SPIR-V beside them)
    Get-ChildItem (Join-Path (Split-Path $shaders -Parent) "spirv") -Filter "*.spv" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $game "native_shaders") -Force
}
# The Android app, for the launcher's Create APK Package (tools/build_apk.py).
$apk = Join-Path $root "out\android\SvR2011.apk"
if (Test-Path $apk) {
    New-Item -ItemType Directory -Force (Join-Path $game "Android") | Out-Null
    Copy-Item $apk (Join-Path $game "Android") -Force
}

"Deployed to $game :"
$files | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
