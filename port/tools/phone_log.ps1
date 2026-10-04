# Live log from the game on an Android phone or tablet, as it is written.
# Follows the newest log in the phone's games/WWE SmackDown vs. Raw 2011/logs
# folder and moves on to the next one when the game is started again.
# Read-only: it never starts, stops or changes the game.
#   phone_log.ps1                          the connected phone (USB debugging)
#   phone_log.ps1 -Connect 192.168.1.50:41234
#                                          over Wi-Fi (Wireless debugging; pair once
#                                          with: adb pair <ip>:<pairing port> <code>)
#   phone_log.ps1 -Device RFGL105N6GJ      pick one of several connected devices
#   phone_log.ps1 -Filter "match end|fps:" only lines matching a regex
#   phone_log.ps1 -Lines 200               lines shown from the current log first (default 50)
#   phone_log.ps1 -Save                    also write everything to port\runs\phone_live_<time>.log
# Ctrl+C stops it.
param([string]$Device = "", [string]$Connect = "", [string]$Filter = "", [int]$Lines = 50, [switch]$Save)
$ErrorActionPreference = "Continue"
$adb = "D:\Android\sdk\platform-tools\adb.exe"
if ($env:ANDROID_HOME -and (Test-Path (Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"))) {
    $adb = Join-Path $env:ANDROID_HOME "platform-tools\adb.exe"
}
$env:MSYS_NO_PATHCONV = "1"
$logs = "/storage/emulated/0/games/WWE SmackDown vs. Raw 2011/logs"

if ($Connect) {
    $r = (& $adb connect $Connect) -join " "
    $r
    if ($r -notmatch "connected") { return }
    if (-not $Device) { $Device = $Connect }
}
$sel = if ($Device) { @("-s", $Device) } else { @() }
$devices = @((& $adb devices) | Select-String "`tdevice$")
if (-not $devices) { "no device connected (USB debugging, or -Connect <ip>:<port> for Wireless debugging)"; return }
if (-not $Device -and $devices.Count -gt 1) {
    "several devices connected - pick one with -Device:"; $devices | ForEach-Object { "  " + ($_.Line -split "`t")[0] }; return
}

# On the phone: tail the newest log; when a newer one appears (the game was
# started again), say so and follow that one instead.
$script = @"
d='$logs'; f=''; p=''
# (one left by an earlier run, whose PC end is gone: end it and its tail)
for q in `$(pgrep -f svr2011_phone_log.sh); do [ "`$q" != "`$`$" ] && kill `$q 2>/dev/null; done
trap '[ -n "`$p" ] && kill `$p 2>/dev/null; exit' EXIT HUP INT TERM PIPE
while true; do
  n=`$(ls -t "`$d" 2>/dev/null | grep '\.log`$' | head -1)
  if [ -n "`$n" ] && [ "`$n" != "`$f" ]; then
    [ -n "`$p" ] && kill `$p 2>/dev/null
    if [ -z "`$f" ]; then k=$Lines; else k=+1; fi
    f=`$n; echo "===== `$f ====="
    tail -n `$k -f "`$d/`$f" & p=`$!
  fi
  sleep 2
done
"@ -replace "`r", ""

$out = $null
if ($Save) {
    $runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
    New-Item -ItemType Directory -Force $runs | Out-Null
    $out = Join-Path $runs ("phone_live_{0:yyyyMMdd_HHmmss}.log" -f (Get-Date))
    "saving to $out"
}
# (pushed as a file: Windows PowerShell drops the quotes inside native arguments)
$tmp = Join-Path $env:TEMP "svr2011_phone_log.sh"
[IO.File]::WriteAllText($tmp, $script + "`n")
& $adb @sel push $tmp "/data/local/tmp/svr2011_phone_log.sh" 2>&1 | Out-Null
$writer = if ($out) { [IO.StreamWriter]::new($out, $false, [Text.UTF8Encoding]::new($false)) } else { $null }
try {
    & $adb @sel shell "sh /data/local/tmp/svr2011_phone_log.sh" | ForEach-Object {
        if ($writer) { $writer.WriteLine($_); $writer.Flush() }
        if (-not $Filter -or $_ -match $Filter -or $_ -like "===== *") { $_ }
    }
} finally {
    if ($writer) { $writer.Dispose() }
}
