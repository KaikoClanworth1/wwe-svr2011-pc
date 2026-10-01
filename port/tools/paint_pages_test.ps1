# Paint Tool pages (src/paint_pages.h), in the limits test game (lim_session.ps1:
# runs\lim_game, saves runs\test_userdata_lim): boots to Create A Superstar ->
# PAINT TOOL and leaves the game on the grid (runs\<Name>_grid.png), then
# presses -Then keys one at a time with a screenshot after each
# (runs\<Name>_1.png ...).
#   paint_pages_test.ps1 [-Name pp] [-Then RB,RB,LB]
param([string]$Name = "pp", [string[]]$Then = @())
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$nav = Join-Path $tools "lim_nav.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                         # title -> main menu
& $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@3,DOWN@1.5,A@4" | Out-Null          # CREATE MODES -> CREATE A SUPERSTAR
& $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@20" -Shot "${Name}_grid" | Out-Null  # PAINT TOOL
$i = 1
foreach ($k in $Then) { & $nav -Keys "$k@5" -Shot "${Name}_$i" | Out-Null; $i++ }
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "paint pages" | ForEach-Object { $_.Line.Substring(26) }
