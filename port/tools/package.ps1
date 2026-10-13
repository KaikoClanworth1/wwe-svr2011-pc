# Makes the release files, with no game data: the PC zip (the launcher and the
# game program) and the Android app on its own (released beside it: all an
# Android player needs - it installs the game from their disc image).
#   .\package.ps1 [-Version 0.2.0]    -> port\out\SvR2011-PC-v<version>.zip and
#                                       port\out\SvR2011-Android-v<version>.apk (default: port\VERSION)
# Build first (build.ps1). The player installs the game data from their own
# disc image with the launcher's Install tab, which copies these files beside it.
# -WithModMaker: the Mod Maker too (left out while it isn't ready for players).
param([string]$Version = "", [string]$Build = "", [switch]$WithModMaker)
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
# The Mod Maker (the launcher's Mods tab opens it), when built and asked for.
$modmaker = Join-Path $build "SvR2011 Mod Maker.exe"
if ($WithModMaker) {
    if (Test-Path $modmaker) { Copy-Item $modmaker $stage } else { Write-Warning "no SvR2011 Mod Maker.exe in $build" }
    # the converters its Other games page runs (Python 3 + numpy + Pillow on the player's side)
    $mmtools = Join-Path $stage "Mod Maker Tools"
    New-Item -ItemType Directory -Force $mmtools | Out-Null
    foreach ($t in @("svrfmt.py", "jboy.py", "ch_tool.py", "svr10_char.py", "svr10_bruise.py", "svr10_moves.py",
                     "svr10_superstar.py", "movepack.py", "w13_char.py", "svr08_stage.py", "svr08_gimmick.py",
                     "discfs.py", "arena_port.py",
                     # the Port page's converters (Mod Maker 3 contract) and what they import / read
                     "superstar_port.py", "abilities_port.py", "ratings_port.py", "ps2_records.py", "svr08_char.py", "entrance_port.py", "ymks.py", "svr08_entrance.py",
                     "hctp_entrance.py", "hctp_motion.py", "svr10_entrance.py", "wwe13_entrance.py", "w13_retarget.py",
                     "w13_skeletons.py", "yanm_decode.py", "belt_port.py", "weapon_port.py", "reskin_port.py",
                     "moves_port.py", "ymbs.py", "ymbs_retarget.py", "attire_port.py", "pro_slots.json", "announcer_calls.json", "bink2_convert.py",
                     "wwe13_pack.py", "arena_tool.py",  # (imports checked transitively 13 Oct: arena_port needs these two)
                     "ps2_entrance.py", "sym_entrance.py", "svr06_entrance.py",  # (entrance_port: PS2 discs, SYM, SvR 2006)
                     "hand_tracks.py", "svr06_hands.py", "finger_retarget.py",  # (entrance_port: finger tracks)
                     "entrance_ring.py",  # (Simple mode: an entrance's tracks for the preview)
                     "model_budget.py", "xact.py", "svr07_char.py", "wwise.py",
                     "svr11_build.py", "svr11_skeleton.json", "rig_maps.json", "rig_maps.py", "svr11_skeleton.py",
                     "moveset_maps.py", "moveset_maps.json", "moveset_port.py", "fix_model.py", "gear_port.py",
                     "ps2_char.py", "ps2_port.py")) {  # (FBX import: builder + skeleton / rig maps; ps2_char: PS2 superstar models, imports discfs + the builder)
        $f = Join-Path $PSScriptRoot $t
        if (Test-Path $f) { Copy-Item $f $mmtools } else { Write-Warning "Mod Maker Tools: no $t yet" }
    }
    $data = Join-Path $PSScriptRoot "data"  # (reskin_port.py's stock texture table)
    if (Test-Path $data) { Copy-Item $data (Join-Path $mmtools "data") -Recurse -Force }
    # vgmstream (the Wwise Vorbis theme decoder wwise.py runs; port\third_party\vgmstream, not in git:
    # the official release, version and SHA-256 in its VERSION.txt) -> Mod Maker Tools\vgmstream
    $vgm = Join-Path $port "third_party\vgmstream"
    if (Test-Path (Join-Path $vgm "vgmstream-cli.exe")) {
        $vdst = Join-Path $mmtools "vgmstream"
        New-Item -ItemType Directory -Force $vdst | Out-Null
        Get-ChildItem $vgm -File | Copy-Item -Destination $vdst   # (VERSION.txt too: where it came from + its SHA-256)
    } else { Write-Warning "no $vgm\vgmstream-cli.exe - WWE '12+ themes won't convert" }
    # (and svrmod.exe, which svrfmt.py runs to compress pac entries: modmaker/build_cli.sh)
    $cli = Join-Path $port "out\modmaker\svrmod.exe"
    if (Test-Path $cli) { Copy-Item $cli $mmtools } else { Write-Warning "no $cli (modmaker\build_cli.sh) - the converters can't compress" }
    # the guide (pictures, diagrams, tutorials): docs/guide -> "Mod Maker Guide"
    $guide = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "docs\guide"
    if (Test-Path $guide) { Copy-Item $guide (Join-Path $stage "Mod Maker Guide") -Recurse }
}
Get-ChildItem $build -Filter "*.dll" | Where-Object { $files -notcontains $_.Name } | Copy-Item -Destination $stage
# The Visual C++ runtime the programs use (dynamic CRT), app-local beside them
# (Microsoft's redistributable files, from the build machine's Visual Studio
# Redist folder - the toolset the programs were linked with or newer): players
# without the VC++ redistributable can start the game, nothing to install.
$vcrt = @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140_atomic_wait.dll")
$redist = @("${env:ProgramFiles}\Microsoft Visual Studio", "${env:ProgramFiles(x86)}\Microsoft Visual Studio") |
    Where-Object { Test-Path $_ } |
    ForEach-Object { Get-ChildItem $_ -Directory -Recurse -Depth 8 -Filter "Microsoft.VC14*.CRT" -ErrorAction SilentlyContinue } |
    Where-Object { $_.FullName -match "\\Redist\\MSVC\\[0-9.]+\\x64\\" -and $_.FullName -notmatch "onecore|debug" } |
    Sort-Object { [version]($_.FullName -replace '.*\\Redist\\MSVC\\([0-9.]+)\\.*', '$1') } -Descending |
    Select-Object -First 1
if (-not $redist) { throw "no Visual Studio Redist\MSVC\<version>\x64\Microsoft.VC14x.CRT folder - the release needs the VC++ runtime DLLs" }
foreach ($f in $vcrt) {
    $src = Join-Path $redist.FullName $f
    if (-not (Test-Path $src)) { throw "$f is missing from $($redist.FullName)" }
    Copy-Item $src $stage
}
Write-Host "VC++ runtime from $($redist.FullName)"
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
    # (and the Direct3D 11 backend's DXBC: older GPUs; .dxbc4 for feature level 10_x)
    Get-ChildItem (Join-Path (Split-Path $shaders -Parent) "dxbc") -Filter "*.dxbc*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "dbg_*" -and $_.Name -notlike "debug_*" } |
        Copy-Item -Destination (Join-Path $stage "native_shaders")
} else {
    Write-Warning "no native shaders in $shaders (tools\convert_shaders.py) - this package uses the emulated renderer"
}
# PlayStation / keyboard button pictures (made by deploy.ps1 into the game
# folder, tools/make_pad_icons.py).
$icons = Join-Path (Split-Path $port -Parent) "Game Files\pad_icons"
if (-not (Test-Path (Join-Path $icons "pad_icons.txt"))) {  # (a release copy in _release\port)
    $icons = Join-Path (Split-Path (Split-Path $port -Parent) -Parent) "Game Files\pad_icons"
}
if (Test-Path (Join-Path $icons "pad_icons.txt")) {
    New-Item -ItemType Directory -Force (Join-Path $stage "pad_icons") | Out-Null
    Get-ChildItem $icons -File | Where-Object { $_.Extension -in ".dds", ".txt", ".bin" } |
        Copy-Item -Destination (Join-Path $stage "pad_icons")
} else {
    Write-Warning "no pad_icons in $icons - the package has Xbox button pictures only"
}

# Mods that come with the port (the launcher installs them into the game):
# the .svrmod files in "Bundled Mods" at the top of the project (outside git;
# a release copy in _release\port looks two levels up).
$bundled = Join-Path (Split-Path $port -Parent) "Bundled Mods"
if (-not (Test-Path $bundled)) { $bundled = Join-Path (Split-Path (Split-Path $port -Parent) -Parent) "Bundled Mods" }
if (Get-ChildItem $bundled -Filter "*.svrmod" -ErrorAction SilentlyContinue) {
    New-Item -ItemType Directory -Force (Join-Path $stage "Bundled Mods") | Out-Null
    Get-ChildItem $bundled -Filter "*.svrmod" | Copy-Item -Destination (Join-Path $stage "Bundled Mods")
} else {
    Write-Warning "no Bundled Mods\*.svrmod - the package comes with no mods"
}

# The known pipelines, built ahead in the menus (tools/merge_pipelines.py).
$plist = Join-Path $port "dist\pipelines.list"
if (Test-Path $plist) {
    New-Item -ItemType Directory -Force (Join-Path $stage "native_shaders") | Out-Null
    Copy-Item $plist (Join-Path $stage "native_shaders") -Force
}
# The shaders in one file per API (one read instead of a thousand: tools/pack_shaders.py).
if (Test-Path (Join-Path $stage "native_shaders")) {
    python (Join-Path $port "tools\pack_shaders.py") (Join-Path $stage "native_shaders") --remove-loose | Out-Host
}
# The Android app, for the launcher's Create APK Package (tools/build_apk.py).
$apk = Join-Path $port "out\android\SvR2011.apk"
if (Test-Path $apk) {
    New-Item -ItemType Directory -Force (Join-Path $stage "Android") | Out-Null
    Copy-Item $apk (Join-Path $stage "Android")
    # (the release's own Android download; the app's updater looks for this name)
    Copy-Item $apk (Join-Path $port "out\SvR2011-Android-v$Version.apk") -Force
} else {
    Write-Warning "no $apk (tools\build_apk.py) - this package has no Android app"
}

Remove-Item $zip -ErrorAction SilentlyContinue
# Entry names with "/": Compress-Archive in Windows PowerShell writes "\",
# which Linux extractors such as the Steam Deck's Ark turn into files named
# "native_shaders\x" instead of folders (no shaders: no native renderer).
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    $root = (Resolve-Path $stage).Path.TrimEnd('\') + '\'
    foreach ($f in Get-ChildItem $stage -Recurse -File) {
        $entry = $f.FullName.Substring($root.Length).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
            [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
"Packaged $zip"
# The bundled mods on their own, for the Android app (it downloads this asset
# and installs them as the PC launcher does - a phone has no PC zip).
$modsZip = Join-Path $port "out\SvR2011-Mods-v$Version.zip"
Remove-Item $modsZip -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $stage "Bundled Mods")) {
    $archive = [IO.Compression.ZipFile]::Open($modsZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in Get-ChildItem (Join-Path $stage "Bundled Mods") -File) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, "Bundled Mods/" + $f.Name,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $archive.Dispose() }
    "Packaged $modsZip (upload it to the release too: the Android app's bundled mods)"
}
if (Test-Path $apk) { "Packaged " + (Join-Path $port "out\SvR2011-Android-v$Version.apk") + " (upload it to the release too)" }
Get-ChildItem $stage | ForEach-Object { "  $($_.Name)  $([math]::Round($_.Length / 1MB, 1)) MB" }
