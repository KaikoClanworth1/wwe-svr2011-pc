# D3D census: run the SVR2011_D3D_TRACE build (Game Files\svr2011_trace.exe,
# from .\build.ps1 -Trace) in the background from boot into the training
# ring, keeping the call counts of three phases:
#   runs\d3dtrace\calls_title.csv   title screen
#   runs\d3dtrace\calls_menu.csv    menus
#   runs\d3dtrace\calls_ring.csv    training ring
# plus every shader container created (runs\d3dtrace\xsc\, xsc.csv).
$ErrorActionPreference = "Stop"
$session = Join-Path $PSScriptRoot "session.ps1"
$d = Join-Path (Split-Path $PSScriptRoot -Parent) "runs\d3dtrace"
Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
$env:SVR2011_D3D_TRACE_DIR = $d

& $session start -Name d3dtrace -Exe svr2011_trace.exe | Out-Null
Start-Sleep 38
Copy-Item "$d\calls.csv" "$d\calls_title.csv"
& $session input "press START 200" "wait 6000" "press A 200" | Out-Null
Start-Sleep 12
Copy-Item "$d\calls.csv" "$d\calls_menu.csv"
& $session input "press A 200" | Out-Null
Start-Sleep 30
& $session input "press A 200" | Out-Null
Start-Sleep 25
& $session shot d3dtrace | Out-Null
Copy-Item "$d\calls.csv" "$d\calls_ring.csv"
& $session stop | Out-Null
"census done: $d ($((Get-ChildItem "$d\xsc").Count) shader containers)"
