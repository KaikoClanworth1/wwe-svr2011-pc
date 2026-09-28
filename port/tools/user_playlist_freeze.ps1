# Repro for the CAE finalize freeze with a USER PLAYLIST song (background,
# muted, test saves): Alicia Fox -> Easy Creation -> FINALIZE -> Music ->
# USER PLAYLIST -> preview (X) and pick the song, then SAVE -> YES. Screenshot
# runs\<Name>4.png is after YES (frozen: the entrance stage with only
# "A SELECT"; fine: back in CREATE AN ENTRANCE).
# -MusicRow: rows below the top one to Music (3, or 4 when the tab has a
# MOTION row) - check <Name>1.png if it drifts.
#   .\user_playlist_freeze.ps1 -Music <folder> [-Name uf] [-LogLevel info]
param([Parameter(Mandatory)][string]$Music, [string]$Name = "uf", [string]$LogLevel = "info", [switch]$SaveGameMusic, [int]$MusicRow = 3)
$s = Join-Path $PSScriptRoot "session.ps1"
$env:SVR2011_MUSIC = $Music
& $s start -Name $Name -LogLevel $LogLevel | Out-Null
function Step([string[]]$k, [int]$sec) { & $s input @k | Out-Null; Start-Sleep $sec }
function Shot([string]$n) { & $s shot "$Name$n" | Out-Null }
Start-Sleep 50
Step @("press START 200") 40; Step @("press A 200") 5; Step @("press START 200") 8
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 5    # CREATE MODES
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 15   # CREATE AN ENTRANCE
Step @("press A 200") 8; Step @("press A 200") 15                                       # SUPERSTAR, Alicia Fox
Step @("press UP 150") 1; Step @("press A 200") 20                                      # Easy Creation
for ($i = 0; $i -lt 5; $i++) { Step @("press RB 150") 3 }; Shot 1                       # FINALIZE
for ($i = 0; $i -lt $MusicRow; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 4  # Music
if ($SaveGameMusic) {                                                                   # ALICIA FOX (the game's)
    for ($i = 0; $i -lt 3; $i++) { Step @("press UP 150") 1 }
    Step @("press DOWN 150") 1; Step @("press DOWN 150") 1; Step @("press A 200") 6; Shot 2
} else {
    Step @("press UP 150") 1; Step @("press UP 150") 1; Step @("press A 200") 4         # USER PLAYLIST
    Step @("press A 200") 4; Step @("press X 200") 3; Step @("press A 200") 6; Shot 2   # notice, preview, pick
}
Step @("press DOWN 150") 1; Step @("press A 200") 5; Shot 3                             # SAVE
Step @("press LEFT 150") 1; Step @("press A 200") 15; Shot 4                            # YES
& $s stop | Out-Null
