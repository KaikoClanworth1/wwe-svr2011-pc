# CPU use per thread during a match entrance (test settings, not the
# player's): runs capture_entrance.ps1 in the background, waits for a frame
# with at least -MinDraws draws, then measures each thread's CPU time over
# -Seconds and samples stacks (runs\<Name>_prof.txt).
#   perf_entrance.ps1 [-Name pent] [-Seconds 10] [-Extra --gpu_backend=d3d12]
param([string]$Name = "pent", [int]$Seconds = 10, [int]$MinDraws = 1500, [string[]]$Extra = @())
$ErrorActionPreference = "Continue"
$port = Split-Path $PSScriptRoot -Parent
$runs = Join-Path $port "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$log = Join-Path $runs "$Name.log"
Remove-Item $log -ErrorAction SilentlyContinue
$job = Start-Job -ArgumentList $PSScriptRoot, $Name, $Extra, $env:SVR2011_CONFIG {
    param($dir, $name, $extra, $cfg)
    $env:SVR2011_CONFIG = $cfg
    Set-Location $dir
    & .\capture_entrance.ps1 -Name $name -Shots 30 -Extra $extra
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
& (Join-Path $PSScriptRoot "sampler\sampler.exe") $pid_ $Seconds $prof 30 | Out-Null
$p = Get-Process -Id $pid_
$rows = foreach ($th in $p.Threads) {
    try { $ms = $th.TotalProcessorTime.TotalMilliseconds - $before[$th.Id] } catch { continue }
    [pscustomobject]@{ tid = $th.Id; cpu_pct = [math]::Round(100 * $ms / ($Seconds * 1000), 1) }
}
"CPU per thread over ${Seconds}s (100 = one core):"
$rows | Sort-Object cpu_pct -Descending | Select-Object -First 12 | Format-Table -AutoSize | Out-String
"total: " + [math]::Round(($rows | Measure-Object cpu_pct -Sum).Sum, 0) + "% of one core"
Select-String -Path $log -Pattern "native perf|fps: " | Select-Object -Last 4 | ForEach-Object { $_.Line -replace '^\[[^\]]+\] ', '' }
Wait-Job $job -Timeout 200 | Out-Null
Remove-Job $job -Force
Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match 'port\\runs' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
