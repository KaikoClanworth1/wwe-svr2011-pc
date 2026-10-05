# The optimization benchmark (opt_bench.ps1) on the connected phone: the same
# route - boot and the title's demo match, the menus, Batista vs Kane with
# entrances, -MatchSecs of the match - with the phone's test saves, muted.
# The log comes back to runs\opt_<Name>.log and opt_bench.ps1 -Report makes
# the table (the phone's thermal status before and after goes in the note).
#   opt_phone_bench.ps1 -Name phone1 [-Args "--full_speed=false"]
param([string]$Name = "phone", [string]$People = "BATISTA,KANE", [int]$TitleWait = 100, [int]$MatchSecs = 60,
      [string]$Args = "")
$ErrorActionPreference = "Continue"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$ps = Join-Path $tools "phone_session.ps1"
$game = "/storage/emulated/0/games/WWE SmackDown vs. Raw 2011"
$env:MSYS_NO_PATHCONV = "1"
function Sh([string]$cmd) { & $adb shell $cmd }
function Thermal { ((Sh "dumpsys thermalservice") | Select-String "Thermal Status:" | Select-Object -First 1).Line.Trim() }
function NewestLog { (Sh "ls -t '$game/logs' | head -1").Trim() }
function LogHas($pattern) { $l = NewestLog; [int]((Sh "grep -c -E '$pattern' '$game/logs/$l'") | Select-Object -First 1) -gt 0 }
function WaitFor($pattern, $seconds) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $seconds) { if (LogHas $pattern) { return $true }; Start-Sleep -Seconds 3 }
    return $false
}

$before = Thermal
$env:SVR2011_TEST_MATCH = "people=$People"   # (no arena: forcing one froze the Fold once - src/test_match.cpp)
$env:SVR2011_PHONE_ARGS = $Args
$r = & $ps start
if ("$r" -notmatch "started") { throw "phone: $r" }
Start-Sleep -Seconds $TitleWait
& $ps shot "opt_${Name}_title" | Out-Null
& $ps input "route normal" "pressuntil A test match: people" | Out-Null
if (-not (WaitFor "\] test match: people" 240)) { & $ps shot "opt_${Name}_stuck_menu" | Out-Null; "no match after 240 s" }
elseif (-not (WaitFor "entrances over|\] match starts" 300)) { & $ps shot "opt_${Name}_stuck_entrance" | Out-Null; "no match start after 300 s" }
else { Start-Sleep -Seconds $MatchSecs; & $ps shot "opt_${Name}_match" | Out-Null }
$l = NewestLog
& $adb pull "$game/logs/$l" (Join-Path $runs "opt_$Name.log") | Out-Null
& $ps stop | Out-Null
$after = Thermal
"phone thermal: before '$before', after '$after'"
& (Join-Path $tools "opt_bench.ps1") -Name $Name -Cond phone -Report
