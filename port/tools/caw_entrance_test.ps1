# Background check of a CAW (test saves, 2nd created superstar) in an
# exhibition One on One vs Randy Orton, entrances NOT skipped: screenshots
# runs\<Name>_e<N>.png every -Every s while the entrances play.
#   caw_entrance_test.ps1 [-Name ce] [-Shots 12] [-Every 6]
param([string]$Name = "ce", [int]$Shots = 12, [int]$Every = 6, [switch]$Keep)
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w = 2) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& $s start -Name $Name "--native_renderer=main" | Out-Null
Start-Sleep 45
P START 40; P A 5; P START 10; P A 5                      # title -> main menu -> PLAY
P A 3; P A 10                                             # ONE ON ONE -> NORMAL
P UP 1.5; P LEFT 1.5; P A 4                               # (1P picks a tile)
P DOWN 1.5; P DOWN 1.5; P RIGHT 1.5; P RIGHT 1.5; P LEFT 1.5; P LEFT 1.5; P RIGHT 1.5
P A 4; P DOWN 8; P A 4                                    # CREATED SUPERSTARS -> 2nd CAW
P UP 2; P A 6; P A 5                                      # COM: Randy Orton, ready
& $s shot "${Name}_sel" | Out-Null
P A 5; P A 5                                              # PLAY -> start
for ($i = 1; $i -le $Shots; $i++) { Start-Sleep $Every; & $s shot "${Name}_e$i" | Out-Null }
if (-not $Keep) { & $s stop | Out-Null }
