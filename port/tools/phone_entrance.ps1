# Plays into a match on the connected phone (test saves, scripted controller,
# muted): title -> training ring -> main menu -> Play -> One on One -> Normal
# Match -> John Cena vs. Randy Orton -> the entrances. Then -Profile records a
# simpleperf CPU profile (runs\phone_perf.data, reported per thread and
# function) or, without it, per-thread CPU (phone_threads.ps1).
#   phone_entrance.ps1 [-Profile] [-Seconds 10] [-P1 "RIGHT RIGHT"] [-P2 "LEFT"] [-Args_ ...]
# -P1 / -P2: cursor moves before each pick (as in capture_entrance.ps1: player
# 1 starts on John Cena, the computer on Randy Orton; -P1 "RIGHT RIGHT" Batista).
# -Args_ "--native_record_thread=false": extra settings for the run.
param([switch]$Profile, [int]$Seconds = 10, [string]$P1 = "none", [string]$P2 = "none", [string]$Args_ = "")
$ErrorActionPreference = "Continue"
$s = Join-Path $PSScriptRoot "phone_session.ps1"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
function K($k, $w) { & $s key $k -Wait $w | Out-Null }
$env:SVR2011_PHONE_ARGS = $Args_
$st = & $s start
if (-not ($st -match "started")) { $st; return }
Start-Sleep 62                                   # intro movies -> title
K START 8; K START 10                            # title -> load the save
K A 6; K A 6; K A 8                              # notices (DLC, Universe) -> ring
K START 6; K A 4; K A 6; K A 12                  # main menu -> Play -> One on One -> Normal
K A 4                                            # player 1 joins (cursor on John Cena)
foreach ($k in ($P1 -split "\s+" | Where-Object { $_ -and $_ -ne "none" })) { K $k 2 }
K A 4; K A 4                                     # superstar 1, ready (the computer's cursor on Randy Orton)
foreach ($k in ($P2 -split "\s+" | Where-Object { $_ -and $_ -ne "none" })) { K $k 2 }
K A 4; K A 16                                    # superstar 2, ready -> versus
K A 22                                           # Play -> entrances
& $s shot phone_entrance | Out-Null
if ($Profile) {
    $pkg = "io.github.kaikoclanworth1.svr2011"
    & $adb shell "/data/local/tmp/simpleperf record --app $pkg --duration $Seconds -f 2000 -o /data/local/tmp/perf.data" 2>&1 | Select-Object -Last 1
    & $adb shell "/data/local/tmp/simpleperf report -i /data/local/tmp/perf.data --sort comm 2>/dev/null | sed -n '7,20p'"
} else {
    & (Join-Path $PSScriptRoot "phone_threads.ps1") -Seconds $Seconds -Top 12
}
& $s log 2
& $s stop | Out-Null                             # (a running game keeps heating the phone)
