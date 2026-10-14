# HCTP backstage room test: installs <ModDir> as Mods\Backstage\hctp_test in runs\opt_wwe13b_game
# (removed again at the end), plays rule -Rule (1B parking room) via route match; shots runs\<Name>_*.png
param([Parameter(Mandatory = $true)][string]$ModDir, [string]$Name = "h1", [int]$Shots = 12, [int]$Every = 5,
      [int]$Settle = 120, [string]$People = "JOHN CENA,RANDY ORTON", [string[]]$Acts = @(), [string]$Rule = "1B", [switch]$NoWait)
$port = "D:\Xbox Games Ports\SvR2011 Arenas\port"
$runs = Join-Path $port "runs"
$game = Join-Path $runs "opt_wwe13b_game"
if (-not $NoWait) { while (Get-Process svr2011, svr2011_trace -ErrorAction SilentlyContinue) { Start-Sleep 10 } }
$mods = Join-Path $game "Mods\Backstage"
New-Item -ItemType Directory -Force $mods | Out-Null
$dst = Join-Path $mods "hctp_test"
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
Copy-Item -Recurse $ModDir $dst
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_wwe13b"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$env:SVR2011_ROUTE = "match"
$env:SVR2011_TEST_MATCH = "people=$People"
$env:SVR2011_TEST_RULE = $Rule
$s = "C:\Users\Ranger\AppData\Local\Temp\claude\D--Xbox-Games-Ports-WWE-Smackdown-Vs-Raw-2011\2d590f87-f85a-4a47-af22-6d217b451ada\scratchpad\wwe13\b_session.ps1"
try {
    & $s start -Name $Name | Out-Null
    for ($i = 0; $i -lt [int]($Settle / 10); $i++) { Start-Sleep 10; & $s shot "${Name}_e$i" | Out-Null }
    $acts = @("stick L 32767 0 1500 RB 0 0", "press X", "press A", "stick L -32767 0 600 B 0 0",
              "stick L 0 32767 1500 RB 0 0", "press X", "stick L -32767 0 1500 RB 0 0", "press A", "stick L 0 -32767 1500 RB 0 0")
    if ($Acts.Count) { $acts = $Acts }
    for ($i = 0; $i -lt $Shots; $i++) {
        & $s input $acts[$i % $acts.Count] | Out-Null
        Start-Sleep $Every
        & $s shot "${Name}_m$i" | Out-Null
    }
} finally {
    & $s stop | Out-Null
    Remove-Item -Recurse -Force $dst -ErrorAction SilentlyContinue
    $env:SVR2011_ROUTE = $null; $env:SVR2011_TEST_MATCH = $null; $env:SVR2011_TEST_RULE = $null
}
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "match: rule|test match|arena mods|BG78|access violation|\[critical\]" | Select-Object -First 25 | ForEach-Object { $_.Line.Substring([Math]::Min(26, $_.Line.Length)) }
