# Benchmark: boot, script into the training ring, report its average FPS.
#   .\bench_ring.ps1 -Label base [-Extra "--vsync=false", ...]
# Runs in the background like session.ps1 (muted, unfocused, off-screen).
param([string]$Label = "bench", [string[]]$Extra = @(), [int]$RingSeconds = 30)
$ErrorActionPreference = "Stop"
$session = Join-Path $PSScriptRoot "session.ps1"
$log = Join-Path (Split-Path $PSScriptRoot -Parent) "runs\bench_$Label.log"

& $session start -Name "bench_$Label" @Extra | Out-Null
Start-Sleep 35                                   # logos -> title screen
& $session input "press START 200" "wait 5000" "press A 200" "wait 4000" "press A 200" | Out-Null
Start-Sleep (20 + $RingSeconds)                  # menus -> ring, then measure
& $session shot "bench_$Label" | Out-Null
& $session stop | Out-Null

# The last $RingSeconds of fps lines are all in the ring.
$n = [int]($RingSeconds / 5)
$fps = Get-Content $log | Select-String "fps: ([\d.]+) avg, worst frame ([\d.]+)" | Select-Object -Last $n
$avg = ($fps | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Measure-Object -Average).Average
$worst = ($fps | ForEach-Object { [double]$_.Matches[0].Groups[2].Value } | Measure-Object -Maximum).Maximum
"{0,-24} ring fps {1,5:N1}   worst frame {2,5:N1} ms   ({3})" -f $Label, $avg, $worst, ($Extra -join " ")
