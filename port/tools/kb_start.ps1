# Boots (test saves) into Create A Superstar and opens the NAME keyboard of the
# roster form (the game's on-screen keyboard), leaving the session running.
#   kb_start.ps1 -Name <run>    then: session.ps1 input "type ..." / "key BACK"
param([Parameter(Mandatory)] [string]$Name)
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& (Join-Path $PSScriptRoot "caw_start.ps1") -Name $Name
P RB 6; P RB 6; P RB 6                  # HEAD -> BODY -> CLOTHING -> OTHER
P UP 3; P A 5                           # FINALIZE
P LEFT 3; P A 8                         # "no changes ... continue?" YES -> roster form
P A 5                                   # NAME -> keyboard
& $s shot "$Name`_kb" | Out-Null
