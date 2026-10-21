# The debug / test suite on an Android phone (docs/DEBUG_SUITE_PLAN.md): the debug-suite APK
# (python tools/build_apk.py --debug-suite -> out/android-dbg/SvR2011-dbg.apk, never a release) started over adb
# with the suite's cues - straight into a match or a menu page, shots / state dumps at given seconds, and a quit -
# then its shots, states and log pulled to port\runs\dbg_android\<Name>.
#   svrtest_android.ps1 -Boot match -Match "people=EDGE,KANE cpu=all" -Shot 5,10 -Quit 15 [-Name m1]
#   svrtest_android.ps1 -Boot OPTIONS -From start -Shot 50 -Quit 55
#   svrtest_android.ps1 ... -Install out\android-dbg\SvR2011-dbg.apk   (adb install -r: never an uninstall)
# The phone's own data stays out of it: the test's saves are the PC tests' (test_run/userdata, as
# phone_session.ps1), the game is muted, and the dbg folder on the phone is removed after the pull.
param([string]$Boot = "", [string]$Match = "", [string]$Shot = "", [string]$State = "", [double]$Quit = 30,
      [string]$From = "", [int]$Speed = 0, [string]$Extra = "", [string]$Name = "dbg", [string]$Install = "",
      [string]$Serial = "RFGL105N6GJ", [int]$Timeout = 300)
$ErrorActionPreference = "Continue"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
if ($env:ANDROID_HOME -and (Test-Path (Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"))) {
    $adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
}
$env:MSYS_NO_PATHCONV = "1"
$pkg = "io.github.kaikoclanworth1.svr2011"
$game = "/storage/emulated/0/games/WWE SmackDown vs. Raw 2011"
$run = "$game/test_run"
$dbg = "$game/dbg"
$runs = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "runs"
$out = Join-Path $runs "dbg_android\$Name"

function Sh([string]$cmd) { & $adb -s $Serial shell $cmd 2>$null }
function Q([string]$p) { "'" + $p + "'" }

if (-not ((& $adb devices) -match "$Serial`tdevice")) { "no phone $Serial (USB debugging)"; exit 1 }
if (((Sh "dumpsys window | grep isKeyguardShowing=true") -join "")) { "the phone is locked - unlock it first"; exit 1 }
if ($Install) {
    "installing $Install (install -r: the phone's data stays)"
    & $adb -s $Serial install -r $Install | Select-Object -Last 1
}
# (the release guard's marker: refuse a phone without the debug build - its cues would do nothing)
$apk = ((Sh "pm path $pkg") -join "") -replace '^package:', ''
if (-not $apk) { "the game isn't installed on $Serial"; exit 1 }

Sh "am force-stop $pkg" | Out-Null
Sh "rm -rf $(Q $dbg); mkdir -p $(Q $dbg) $(Q "$run/userdata")" | Out-Null
if (-not (Sh "ls $(Q "$run/userdata/Saves") 2>/dev/null")) {
    $src = Join-Path $runs "test_userdata"
    foreach ($d in "Saves", "achievements") { & $adb -s $Serial push (Join-Path $src $d) "$run/userdata/" | Out-Null }
}
$a = @("--audio_mute=true", "--dbg_out=`"$dbg`"")
if ($Boot) { $a += "--dbg_boot=`"$Boot`"" }
if ($Match) { $a += "--dbg_match=`"$Match`"" }
if ($Shot) { $a += "--dbg_shot=$Shot" }
if ($State) { $a += "--dbg_state=$State" }
if ($Quit -gt 0) { $a += "--dbg_quit=$Quit" }
if ($From) { $a += "--dbg_from=$From" }
if ($Speed -gt 1) { $a += "--dbg_speed=$Speed" }
if ($Extra) { $a += $Extra }
$argline = $a -join " "
# (a script on the phone: the arguments' quotes reach the app as they are - GameActivity keeps a quoted value whole)
$script = "am start -n $pkg/.InstallActivity --es SVR2011_USER_DATA '$run/userdata' --es args '$argline'`n"
$tmp = Join-Path $env:TEMP "svrdbg_start.sh"
[IO.File]::WriteAllText($tmp, $script)
& $adb -s $Serial push $tmp /data/local/tmp/svrdbg_start.sh | Out-Null
"start: $argline"
Sh "sh /data/local/tmp/svrdbg_start.sh" | Out-Null

# Until the game quits itself (dbg_quit) or the time is up.
$t0 = Get-Date
Start-Sleep 5
while (((Get-Date) - $t0).TotalSeconds -lt $Timeout -and ((Sh "pidof $pkg") -join "")) { Start-Sleep 3 }
$alive = [bool]((Sh "pidof $pkg") -join "")
if ($alive) { "still running at $Timeout s - stopped"; Sh "am force-stop $pkg" | Out-Null }

New-Item -ItemType Directory -Force $out | Out-Null
& $adb -s $Serial pull "$dbg/." $out | Select-Object -Last 1
$log = ((Sh "ls -t $(Q "$game/logs") | head -1") -join "").Trim()
if ($log) { & $adb -s $Serial pull "$game/logs/$log" (Join-Path $out "game.log") | Out-Null }
Sh "rm -rf $(Q $dbg) /data/local/tmp/svrdbg_start.sh" | Out-Null
"pulled to $out"
if (Test-Path (Join-Path $out "game.log")) {
    Select-String -Path (Join-Path $out "game.log") -Pattern "SVRDBG|Unhandled guest access|world update stuck" |
        Select-Object -Last 15 | ForEach-Object { $_.Line.Substring([Math]::Min(27, $_.Line.Length)) }
}
if (-not (Select-String -Path (Join-Path $out "game.log") -Pattern "SVRDBG" -Quiet -ErrorAction SilentlyContinue)) {
    "no SVRDBG lines: the installed APK is not the debug-suite build (build_apk.py --debug-suite)"
}
