# Makes the release zip: the launcher and the game program, with no game data.
#   .\package.ps1 [-Version 0.2.0]    -> port\out\SvR2011-PC-v<version>.zip (default: port\VERSION)
# Build first (build.ps1). The player installs the game data from their own
# disc image with the launcher's Install tab, which copies these files beside it.
param([string]$Version = "", [string]$Build = "")
$ErrorActionPreference = "Stop"
$port  = Split-Path $PSScriptRoot -Parent
if (-not $Version) { $Version = (Get-Content (Join-Path (Split-Path $PSScriptRoot -Parent) "VERSION.txt") -TotalCount 1).Trim() }
$build = if ($Build) { $Build } else { Join-Path $port "out\build\SourceRelease" }
$name  = "SvR2011-PC-v$Version"
$stage = Join-Path $port "out\package\$name"
$zip   = Join-Path $port "out\$name.zip"

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
$files = @("svr2011.exe", "SvR2011 Launcher.exe", "rexruntime.dll", "rexgpu-xenos.dll")
foreach ($f in $files) {
    $src = Join-Path $build $f
    if (-not (Test-Path $src)) { throw "$f is missing from $build - build first" }
    Copy-Item $src $stage
}
Get-ChildItem $build -Filter "*.dll" | Where-Object { $files -notcontains $_.Name } | Copy-Item -Destination $stage
Copy-Item (Join-Path $port "dist\Read Me.txt") $stage
# The native renderer's shaders (converted from the game's by
# tools/convert_shaders.py; without them it draws nothing).
$shaders = Join-Path $port "runs\shaders_native\dxil"
if (Test-Path (Join-Path $shaders "present.vs.dxil")) {
    New-Item -ItemType Directory -Force (Join-Path $stage "native_shaders") | Out-Null
    Get-ChildItem $shaders -File | Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $stage "native_shaders")
    # (and the Vulkan backend's SPIR-V beside them)
    Get-ChildItem (Join-Path (Split-Path $shaders -Parent) "spirv") -Filter "*.spv" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $stage "native_shaders")
} else {
    Write-Warning "no native shaders in $shaders (tools\convert_shaders.py) - this package uses the emulated renderer"
}
# The Android app, for the launcher's Create APK Package (tools/build_apk.py).
$apk = Join-Path $port "out\android\SvR2011.apk"
if (Test-Path $apk) {
    New-Item -ItemType Directory -Force (Join-Path $stage "Android") | Out-Null
    Copy-Item $apk (Join-Path $stage "Android")
} else {
    Write-Warning "no $apk (tools\build_apk.py) - this package has no Android app"
}

Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
"Packaged $zip"
Get-ChildItem $stage | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
