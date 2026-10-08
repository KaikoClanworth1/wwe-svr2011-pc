# Refresh "..\Game Files" - a complete, runnable install:
#   the disc files (from "..\Extract GameFiles") + the port's program files.
# User data there (svr2011.toml, UserData\, launcher.ini) is left alone.
# -WithModMaker: the Mod Maker too (left out while it isn't ready for players).
param([string]$Build = "", [switch]$WithModMaker)
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
# The mods that come with the port ("Bundled Mods" at the top of the project,
# or two levels up from a release copy): the launcher installs new ones.
$bundled = Join-Path $top "Bundled Mods"
if (-not (Test-Path $bundled)) { $bundled = Join-Path (Split-Path $top -Parent) "Bundled Mods" }
if (Get-ChildItem $bundled -Filter "*.svrmod" -ErrorAction SilentlyContinue) {
    New-Item -ItemType Directory -Force (Join-Path $game "Bundled Mods") | Out-Null
    Get-ChildItem $bundled -Filter "*.svrmod" | Copy-Item -Destination (Join-Path $game "Bundled Mods") -Force
}
if ($LASTEXITCODE -ge 8) { throw "copying the disc files failed" }
# Menu text: "Xbox LIVE" -> "Online" (rebuilt from the disc's string.pac).
python (Join-Path $root "tools\patch_strings.py") $disc $game
if ($LASTEXITCODE) { throw "patching the menu text failed" }
# MY WWE -> OPTIONS gets a GRAPHICS entry (the port opens its own page).
python (Join-Path $root "tools\patch_menu.py") $disc $game
if ($LASTEXITCODE) { throw "patching the menus failed" }
# PlayStation / keyboard button pictures, from the PS3 version's files
# (SVR2011_PS3_GAME = its PS3_GAME\USRDIR; tools/make_pad_icons.py).
$ps3 = $env:SVR2011_PS3_GAME
if ($ps3 -and (Test-Path (Join-Path $ps3 "pac"))) {
    python (Join-Path $root "tools\make_pad_icons.py") --ps3 $ps3 --xbox $disc --out $game | Out-Host
    if ($LASTEXITCODE) { throw "making the PlayStation button pictures failed" }
} elseif (-not (Test-Path (Join-Path $game "pad_icons\pad_icons.txt"))) {
    Write-Warning "no PlayStation button pictures (set SVR2011_PS3_GAME to the PS3 game's USRDIR)"
}
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
$files = @(Get-ChildItem $build -File | Where-Object { $_.Extension -in ".exe", ".dll" } |
           Where-Object { $WithModMaker -or $_.Name -ne "SvR2011 Mod Maker.exe" })
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
    # (and the Direct3D 11 backend's DXBC: older GPUs; .dxbc4 for feature level 10_x)
    Get-ChildItem (Join-Path (Split-Path $shaders -Parent) "dxbc") -Filter "*.dxbc*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $game "native_shaders") -Force
}
# The known pipelines, built ahead in the menus (tools/merge_pipelines.py).
$plist = Join-Path $root "dist\pipelines.list"
if (Test-Path $plist) {
    New-Item -ItemType Directory -Force (Join-Path $game "native_shaders") | Out-Null
    Copy-Item $plist (Join-Path $game "native_shaders") -Force
}
# The shaders in one file per API (one read instead of a thousand: tools/pack_shaders.py).
if (Test-Path (Join-Path $game "native_shaders")) {
    python (Join-Path $root "tools\pack_shaders.py") (Join-Path $game "native_shaders") | Out-Host
}
# The Android app, for the launcher's Create APK Package (tools/build_apk.py).
$apk = Join-Path $root "out\android\SvR2011.apk"
if (Test-Path $apk) {
    New-Item -ItemType Directory -Force (Join-Path $game "Android") | Out-Null
    Copy-Item $apk (Join-Path $game "Android") -Force
}

"Deployed to $game :"
$files | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
