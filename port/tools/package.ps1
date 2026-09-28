# Makes the release zip: the launcher and the game program, with no game data.
#   .\package.ps1 -Version 0.1.0      -> port\out\SvR2011-PC-v<version>.zip
# Build first (build.ps1). The player installs the game data from their own
# disc image with the launcher's Install tab, which copies these files beside it.
param([Parameter(Mandatory)][string]$Version, [string]$Build = "")
$ErrorActionPreference = "Stop"
$port  = Split-Path $PSScriptRoot -Parent
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

Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
"Packaged $zip"
Get-ChildItem $stage | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
