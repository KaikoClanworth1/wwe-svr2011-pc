# Online tests: starts an online session (online_session.ps1) and walks it
# to the ONLINE menu (screenshot runs\<Name>_online.png). Extra arguments go
# to the game.
#   .\online_to_menu.ps1 -Name on6 [-LogLevel debug] [-NoOnline]
param([string]$Name = "online", [string]$LogLevel = "info", [switch]$NoOnline,
      [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$s = Join-Path $PSScriptRoot "online_session.ps1"
function P($b, $w) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& $s start -Name $Name -LogLevel $LogLevel @Rest
Start-Sleep 60
P START 8; P START 45; P A 6; P START 10           # demo -> title -> (loading) -> main menu
& $s shot "$Name`_main" | Out-Null
if ($NoOnline) { return }
P DOWN 1.2; P DOWN 1.2; P DOWN 1.2; P DOWN 1.2; P A 12  # ONLINE
& $s shot "$Name`_online"
