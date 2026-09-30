# CPU per thread of the game on the connected phone (USB debugging), from
# Android's top over -Seconds: which threads (audio, GPU commands, the game's)
# cost what there. Leave the game on the title screen or in a match.
#   phone_threads.ps1 [-Seconds 10] [-Top 16]
param([int]$Seconds = 10, [int]$Top = 16)
$adb = "D:\Android\sdk\platform-tools\adb.exe"
if ($env:ANDROID_HOME -and (Test-Path (Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"))) {
    $adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
}
$pkg = "io.github.kaikoclanworth1.svr2011"
if (-not ((& $adb devices) -match "`tdevice")) { "no phone connected (USB debugging)"; return }
$gamePid = (& $adb shell pidof $pkg)
if (-not $gamePid) { "the game isn't running (tools\phone_session.ps1 start)"; return }
# Two samples: the second is the CPU use over -Seconds (sorted by %CPU;
# 100 = one core; thread names are cut to 15 characters).
$out = & $adb shell top -H -b -d $Seconds -n 2 -m $Top -p $gamePid.Trim()
$start = ($out | Select-String -Pattern "^Threads:" | Select-Object -Last 1).LineNumber
$out[($start - 1)..($out.Count - 1)] | Where-Object { $_ -notmatch "Mem:|Swap:" }
