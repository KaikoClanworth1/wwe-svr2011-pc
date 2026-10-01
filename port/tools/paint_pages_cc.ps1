# Paint Tool pages in Community Creations (src/paint_pages.h), in the limits
# test game (lim_session.ps1) against the local Community Creations server
# (online_server.ps1; started by the online tests): boots to ONLINE ->
# COMMUNITY CREATIONS (runs\<Name>_cc.png), then presses -Then keys one at a
# time with a screenshot after each. Config runs\test_config_lim_online.toml
# (online_* settings, its own online_xuid), saves runs\test_userdata_lim_online
# (a copy of the online tests' saves: the user agreement accepted).
#   paint_pages_cc.ps1 [-Name ppo] [-Then DOWN,A]
param([string]$Name = "ppo", [string[]]$Then = @())
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config_lim_online.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim_online"
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$nav = Join-Path $tools "lim_nav.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                   # title -> main menu
& $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,DOWN@1.5,A@20" | Out-Null       # ONLINE
& $nav -Keys "A@20" | Out-Null                                          # "upload Paint Tool data now?" -> NO
& $nav -Keys "DOWN@1.5,DOWN@1.5,A@20" -Shot "${Name}_cc" | Out-Null      # COMMUNITY CREATIONS
$i = 1
foreach ($k in $Then) { & $nav -Keys "$k@5" -Shot "${Name}_$i" | Out-Null; $i++ }
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "paint pages" | ForEach-Object { $_.Line.Substring(26) }
