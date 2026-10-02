# Is an arena file held open by the arena test game? Tries an exclusive open
# of runs\opt_arena_game\pac\bg\<File> at points in the ONE ON ONE flow:
# title screen, main menu, match settings, match loaded.
#   arena_open_probe.ps1 [-File bg17.pac]
param([string]$File = "bg17.pac")
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$path = Join-Path $runs "opt_arena_game\pac\bg\$File"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_arena"
$s = Join-Path $tools "arena_session.ps1"
$nav = Join-Path $tools "arena_nav.ps1"
function Probe($when) {
    try { $fs = [IO.File]::Open($path, 'Open', 'Read', 'None'); $fs.Close(); "${when}: not open" }
    catch { "${when}: OPEN by the game" }
}
& $s start -Name probe | Out-Null
Start-Sleep 20; Probe "boot+20s"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null; Probe "main menu"
& $nav -Keys "A@3,A@3,A@8,A@3,A@4,A@4,A@5,A@6" | Out-Null; Probe "match settings"
& $nav -Keys "A@1" | Out-Null
Start-Sleep 25; Probe "loading/entrance"
Start-Sleep 30; Probe "match +55s"
& $s stop | Out-Null
