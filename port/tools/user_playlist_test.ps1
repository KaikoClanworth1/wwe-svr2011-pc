# Background check of Create An Entrance -> Music -> USER PLAYLIST (muted,
# off-screen): title -> main menu -> CREATE MODES -> CREATE AN ENTRANCE ->
# SUPERSTAR (Alicia Fox) -> Easy Creation -> FINALIZE -> Music -> USER
# PLAYLIST, then preview the first playlist and back out. Screenshots
# runs\<Name>N.png; the music folder is -Music (SVR2011_MUSIC).
#   .\user_playlist_test.ps1 -Music <folder> [-Name up]
param([Parameter(Mandatory)][string]$Music, [string]$Name = "up", [switch]$Save, [switch]$Plain)
$s = Join-Path $PSScriptRoot "session.ps1"
$env:SVR2011_MUSIC = $Music
$env:SVR2011_AUDIO_LEVEL = "1"
& $s start -Name $Name -LogLevel info | Out-Null
function Step([string[]]$k, [int]$sec) { & $s input @k | Out-Null; Start-Sleep $sec }
function Shot([string]$n) { & $s shot "$Name$n" | Out-Null }
Start-Sleep 50
Step @("press START 200") 40; Step @("press A 200") 5; Step @("press START 200") 8
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 5    # CREATE MODES
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 15   # CREATE AN ENTRANCE
Step @("press A 200") 8                                                                 # SUPERSTAR
Step @("press A 200") 15; Step @("press UP 150") 1; Step @("press A 200") 20            # Alicia Fox, Easy
for ($i = 0; $i -lt 5; $i++) { Step @("press RB 150") 3 }                               # FINALIZE
if ($Plain) {                                                                           # control: SAVE, no user playlist
    for ($i = 0; $i -lt 4; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 8; Shot 6
    Step @("press LEFT 150") 1; Step @("press A 200") 10; Shot 7
    Step @("press A 200") 10; Shot 8
    Step @("press A 200") 10; Shot 9
    & $s stop | Out-Null
    return
}
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 4    # Music
Step @("press UP 150") 1; Step @("press UP 150") 1; Step @("press A 200") 4             # USER PLAYLIST
Shot 1
Step @("press A 200") 5; Shot 2                                                         # the notice
Step @("press X 200") 10; Shot 3                                                        # preview
Step @("press B 200") 8; Shot 4                                                         # back (stops it)
Step @("press A 200") 5; Step @("press A 200") 5                                        # USER PLAYLIST, notice
Step @("press A 200") 6; Shot 5                                                         # pick MY THEME
if ($Save) {                                                                            # FINALIZE -> SAVE
    Step @("press DOWN 150") 1; Step @("press A 200") 8; Shot 6
    Step @("press LEFT 150") 1; Step @("press A 200") 10; Shot 7                        # YES
    Step @("press A 200") 10; Shot 8
    Step @("press A 200") 10; Shot 9
} else {
    Step @("press X 200") 45; Shot 6                                                    # full entrance preview
    Step @("press B 200") 10; Shot 7
}
& $s stop | Out-Null
$log = Join-Path (Split-Path $PSScriptRoot -Parent) "runs\$Name.log"
Select-String -Path $log -Pattern "user music|XamCreateEnumerator|XMPPlayUser|XMPStop|XMPPause|XMPContinue|Unimplemented XMP|audio level" |
    ForEach-Object { $_.Line.Substring(12) } | Select-Object -Last 30
