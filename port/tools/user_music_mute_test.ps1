# Background check that game audio comes back after a USER PLAYLIST song
# (muted, off-screen, test saves): CREATE AN ENTRANCE -> Alicia Fox -> Easy
# Creation -> FINALIZE -> Music -> USER PLAYLIST -> preview (X), back out to
# the practice arena, with the game's output level logged every 5 s
# (SVR2011_AUDIO_LEVEL) - it should be back above 0 after "user music: stop".
#   .\user_music_mute_test.ps1 -Music <folder> [-Name mu] [-MusicRow 4]
param([Parameter(Mandatory)][string]$Music, [string]$Name = "mu", [int]$MusicRow = 4, [string]$LogLevel = "info", [switch]$GameSong, [switch]$NoPlay, [switch]$NoneSong, [switch]$Sample)
$s = Join-Path $PSScriptRoot "session.ps1"
$env:SVR2011_MUSIC = $Music
$env:SVR2011_AUDIO_LEVEL = "1"
& $s start -Name $Name -LogLevel $LogLevel | Out-Null
function Step([string[]]$k, [int]$sec) { & $s input @k | Out-Null; Start-Sleep $sec }
Start-Sleep 50
Step @("press START 200") 40; Step @("press A 200") 5; Step @("press START 200") 8
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 5    # CREATE MODES
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 15   # CREATE AN ENTRANCE
Step @("press A 200") 8; Step @("press A 200") 15                                       # SUPERSTAR, Alicia Fox
Step @("press UP 150") 1; Step @("press A 200") 20                                      # Easy Creation
for ($i = 0; $i -lt 5; $i++) { Step @("press RB 150") 3 }; Start-Sleep 5                # FINALIZE
for ($i = 0; $i -lt $MusicRow; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 4
if ($NoneSong) {                                                                        # control: NONE
    Step @("press UP 150") 1; Step @("press X 200") 12
} elseif ($GameSong) {                                                                  # control: BATISTA
    Step @("press DOWN 150") 1; Step @("press X 200") 12
} else {
    Step @("press UP 150") 1; Step @("press UP 150") 1; Step @("press A 200") 4         # USER PLAYLIST
    if ($NoPlay) { Step @("press A 200") 12 }                                           # notice, just look
    else { Step @("press A 200") 4; Step @("press X 200") 12 }                          # notice, preview
}
Step @("press B 200") 4; Step @("press B 200") 4; Start-Sleep 12                        # back (stops it)
for ($i = 0; $i -lt 4; $i++) { Step @("press B 200") 6 }; Start-Sleep 12                # out to the arena
& $s shot "${Name}_end" | Out-Null
if ($Sample) {
    $sp = (Get-Content (Join-Path (Split-Path $PSScriptRoot -Parent) "runs\session.json") | ConvertFrom-Json).pid
    & (Join-Path $PSScriptRoot "sampler\sampler.exe") $sp 4 (Join-Path (Split-Path $PSScriptRoot -Parent) "runs\${Name}_sample.txt") 24 | Out-Null
}
& $s stop | Out-Null
$log = Join-Path (Split-Path $PSScriptRoot -Parent) "runs\$Name.log"
Select-String -Path $log -Pattern "audio level|user music: (playing|stop)" |
    ForEach-Object { $l = $_.Line; $l.Substring(12, [Math]::Min(80, $l.Length - 12)) } | Select-Object -Last 24
