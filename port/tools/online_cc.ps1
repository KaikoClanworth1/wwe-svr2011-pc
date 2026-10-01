# Online tests: boots an online session (online_to_menu.ps1) into ONLINE ->
# COMMUNITY CREATIONS (screenshot runs\<Name>_cc.png). Declines the paint
# data upload prompt; the in-game agreement must have been accepted in this
# test save before.
#   .\online_cc.ps1 -Name cc1
param([string]$Name = "cc", [string]$LogLevel = "info")
$s = Join-Path $PSScriptRoot "online_session.ps1"
function P($b, $w) { & $s input "press $b 150" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& (Join-Path $PSScriptRoot "online_to_menu.ps1") -Name $Name -LogLevel $LogLevel | Out-Null
P A 20                                  # "upload paint data now?" -> NO
P DOWN 1.5; P DOWN 1.5; P A 20          # COMMUNITY CREATIONS
& $s shot "$Name`_cc"
