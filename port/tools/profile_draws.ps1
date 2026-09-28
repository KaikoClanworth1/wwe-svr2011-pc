# Profiles the native renderer's thread during a draw-heavy scene (a Fatal-4-Way
# ladder entrance): boots a test game, starts the match, waits until the renderer
# logs a frame with at least -MinDraws draws, then samples that thread and prints
# the hottest source lines (llvm-symbolizer) inside svr2011.exe.
param([int]$MinDraws = 1800, [int]$Seconds = 15, [string]$Name = "pdraw")
$top  = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$s    = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w) { & $s input "press $b 150" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& $s start -Name $Name --native_renderer=main | Out-Null
Start-Sleep 45; P START 40; P A 5; P START 10
P A 3.5; foreach ($i in 1..3) { P DOWN 1 }; P A 3.5; foreach ($i in 1..3) { P DOWN 1 }; P A 3.5
foreach ($k in 1..16) { P A 2.5 }
$log = Join-Path $top "port\runs\$Name.log"
$pid_ = (Get-Content (Join-Path $top "port\runs\session.json") | ConvertFrom-Json).pid
$tid = $null
for ($t = 0; $t -lt 120; $t++) {
    Start-Sleep 1
    $l = Select-String -Path $log -Pattern "native perf: .* draws (\d+)" | Select-Object -Last 1
    if ($l -and [int]$l.Matches[0].Groups[1].Value -ge $MinDraws) {
        $tid = [int]($l.Line -replace '.*\[t(\d+)\].*', '$1'); break
    }
}
if (-not $tid) { "no heavy scene reached"; & $s stop | Out-Null; return }
"heavy scene: " + ($l.Line -replace '.*native perf: ', '')
$prof = Join-Path $top "port\runs\$Name`_prof.txt"
& (Join-Path $PSScriptRoot "sampler\sampler.exe") $pid_ $Seconds $prof 40 $tid | Out-Null
& $s stop | Out-Null
python (Join-Path $PSScriptRoot "prof_lines.py") $prof (Join-Path $top "port\out\build\SourceRelease\svr2011.exe")
