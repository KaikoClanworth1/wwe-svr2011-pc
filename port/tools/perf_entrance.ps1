# CPU use per thread during a match entrance (test settings, not the
# player's): runs capture_entrance.ps1 in the background, waits for a frame
# with at least -MinDraws draws, then measures each thread's CPU time over
# -Seconds and samples stacks (runs\<Name>_prof.txt).
#   perf_entrance.ps1 [-Name pent] [-Seconds 10] [-Extra --gpu_backend=d3d12]
param([string]$Name = "pent", [int]$Seconds = 10, [int]$MinDraws = 1500, [string[]]$Extra = @(), [string]$Config = "test_config.toml", [string]$Focus = "", [string]$P1 = "", [string]$P2 = "")
$ErrorActionPreference = "Continue"
$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port "runs"
$env:SVR2011_CONFIG = Join-Path $runs $Config
$log = Join-Path $runs "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$job = Start-Job -ArgumentList $PSScriptRoot, $Name, $Extra, $env:SVR2011_CONFIG, $P1, $P2 {
    param($dir, $name, $extra, $cfg, $p1, $p2)
    $env:SVR2011_CONFIG = $cfg
    Set-Location $dir
    & .\capture_entrance.ps1 -Name $name -Shots 30 -Extra $extra -P1 $p1 -P2 $p2
}
$pid_ = $null
for ($t = 0; $t -lt 400; $t++) {
    Start-Sleep 1
    $l = Select-String -Path $log -Pattern "native perf: .* draws (\d+)" -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($l -and [int]$l.Matches[0].Groups[1].Value -ge $MinDraws) { break }
}
$pid_ = (Get-Content (Join-Path $runs "session.json") | ConvertFrom-Json).pid
$p = Get-Process -Id $pid_
$before = @{}; foreach ($th in $p.Threads) { try { $before[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch {} }
$prof = Join-Path $runs "${Name}_prof.txt"
$sampler = Join-Path $PSScriptRoot "sampler\sampler.exe"
if ($Focus) {
    # -Focus "GPU Commands": sample only the threads with that in their name
    # (a quick pass finds their ids), for many more samples of them.
    $probe = Join-Path $runs "${Name}_probe.txt"
    & $sampler $pid_ 1 $probe 4 | Out-Null
    $tids = (Select-String -Path $probe -Pattern ('^==== thread (\d+) "[^"]*' + [regex]::Escape($Focus))) | ForEach-Object { $_.Matches[0].Groups[1].Value }
    & $sampler $pid_ $Seconds $prof 30 ($tids -join ",") | Out-Null
} else {
    & $sampler $pid_ $Seconds $prof 30 | Out-Null
}
$p = Get-Process -Id $pid_
$rows = foreach ($th in $p.Threads) {
    try { $ms = $th.TotalProcessorTime.TotalMilliseconds - $before[$th.Id] } catch { continue }
    [pscustomobject]@{ tid = $th.Id; cpu_pct = [math]::Round(100 * $ms / ($Seconds * 1000), 1) }
}
# Thread names from the sampler's report.
$names = @{}
foreach ($m in (Select-String -Path $prof -Pattern '^==== thread (\d+) "([^"]*)"')) { $names[[int]$m.Matches[0].Groups[1].Value] = $m.Matches[0].Groups[2].Value }
foreach ($r in $rows) { $r | Add-Member name $names[[int]$r.tid] }
"CPU per thread over ${Seconds}s (100 = one core):"
$rows | Sort-Object cpu_pct -Descending | Select-Object -First 16 | Format-Table -AutoSize | Out-String
"total: " + [math]::Round(($rows | Measure-Object cpu_pct -Sum).Sum, 0) + "% of one core"
Select-String -Path $log -Pattern "native perf|fps: " | Select-Object -Last 4 | ForEach-Object { $_.Line -replace '^\[[^\]]+\] ', '' }
Wait-Job $job -Timeout 200 | Out-Null
Remove-Job $job -Force
# (only this test's own game - session.json's: other sessions run theirs under port\runs too)
$own = (Get-Content (Join-Path $runs "session.json") | ConvertFrom-Json).pid
if ($own) { Stop-Process -Id $own -Force -ErrorAction SilentlyContinue }
