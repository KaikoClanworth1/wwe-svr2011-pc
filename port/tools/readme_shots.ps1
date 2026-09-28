# Screenshots for the README (background, muted, test saves):
# title, main menu, CREATE AN ENTRANCE -> MUSIC -> USER PLAYLIST list,
# MOVIE list with user movies. Output: runs\readme_*.png
param([string]$Music, [string]$Movies)
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w = 2) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
function Shot($n) { & $s shot "readme_$n" | Out-Null }
if ($Music) { $env:SVR2011_MUSIC = $Music }
if ($Movies) { $env:SVR2011_MOVIES = $Movies }
& $s start -Name readme | Out-Null
Start-Sleep 50
Shot "title"
P START 40; P A 5; P START 8
Shot "mainmenu"
P DOWN 1; P DOWN 1; P DOWN 1; P A 5                      # CREATE MODES
P DOWN 1; P DOWN 1; P DOWN 1; P A 15                     # CREATE AN ENTRANCE
P A 8; P A 15                                            # SUPERSTAR, Alicia Fox
P UP 1; P A 20                                           # Easy Creation
for ($i = 0; $i -lt 5; $i++) { P RB 3 }; Start-Sleep 5    # FINALIZE
P DOWN 1.5; P DOWN 1.5; P A 5                           # MOVIE (no MOTION row: saved entrance)
Shot "movies"
P B 3; P DOWN 1.5; P A 5                                 # MUSIC
P UP 1.5; P UP 1.5; P A 4; P A 5                         # USER PLAYLIST, notice
Shot "userplaylist"
& $s stop | Out-Null
