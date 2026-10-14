# Row test: installs <ModDir> as Mods\Backstage\hctp_test, route exhibition, steps given by -Steps (list of
# "keys|shotname"), log line checks. Own game runs\opt_wwe13b_game.
param([Parameter(Mandatory = $true)][string]$ModDir, [string]$Name = "r1", [string]$Steps = "", [int]$Shots = 8, [int]$Every = 5, [string]$Acts = "", [switch]$Hctp, [string]$Env = "")
$port = if ($Hctp) { "D:\Xbox Games Ports\SvR2011 HCTP\port" } else { "D:\Xbox Games Ports\SvR2011 Arenas\port" }
$runs = Join-Path $port "runs"
$game = Join-Path $runs $(if ($Hctp) { "opt_hctp_game" } else { "opt_wwe13b_game" })
$dst = Join-Path $game "Mods\Backstage\hctp_test"
if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
Copy-Item -Recurse $ModDir $dst
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_wwe13b"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
foreach ($kv in ($Env -split ";")) { if ($kv -match "^(\w+)=(.*)$") { Set-Item "env:$($matches[1])" $matches[2] } }
$env:SVR2011_ROUTE = "exhibition"; $env:SVR2011_TEST_ROW_LOG = "1"; $env:SVR2011_FPS_PROBE = "2"
$s = if ($Hctp) { Join-Path $PSScriptRoot "h_session.ps1" } else { "C:\Users\Ranger\AppData\Local\Temp\claude\D--Xbox-Games-Ports-WWE-Smackdown-Vs-Raw-2011\2d590f87-f85a-4a47-af22-6d217b451ada\scratchpad\wwe13\b_session.ps1" }
$log = Join-Path $runs "$Name.log"
try {
    & $s start -Name $Name | Out-Null
    for ($i = 0; $i -lt 80; $i++) { Start-Sleep -Seconds 3; if (Select-String -Path $log -Pattern "all steps done" -Quiet) { break } }
    Start-Sleep -Seconds 4
    $k = 0
    foreach ($st in ($Steps -split ";")) {
        $keys, $shot = $st -split '\|'
        foreach ($key in ($keys -split ',')) {
            if ($key -match '^wait(\d+)$') { Start-Sleep -Seconds $matches[1]; continue }
            & $s input "press $key" | Out-Null; Start-Sleep -Milliseconds 900
        }
        Start-Sleep -Seconds 2
        if ($shot) { & $s shot "${Name}_$shot" | Out-Null }
        $k++
    }
    for ($i = 0; $i -lt 15; $i++) { if (Select-String -Path $log -Pattern "match: rule 1B" -Quiet) { break }; & $s input "press A" | Out-Null; Start-Sleep 4 }
    if ($Acts) {
        Start-Sleep $(if ($env:H_ACT_DELAY) { [int]$env:H_ACT_DELAY } else { 6 })
        $k = 0
        foreach ($a in ($Acts -split ";")) {
            if ($a -match '^sleep (\d+)$') { Start-Sleep $matches[1]; continue }
            if ($a -match '^snap (\d+)$') { Start-Sleep $matches[1]; & $s shot ("{0}_a{1:D2}" -f $Name, $k) | Out-Null; $k++; continue }
            $parts = @($a -split '\+'); & $s input @parts | Out-Null; Start-Sleep -Milliseconds 2500; & $s shot ("{0}_a{1:D2}" -f $Name, $k) | Out-Null; $k++
        }
    }
    for ($i = 0; $i -lt $Shots; $i++) { Start-Sleep $Every; & $s shot "${Name}_m$i" | Out-Null }
} catch { $_.Exception.Message } finally {
    & $s stop | Out-Null
    Remove-Item -Recurse -Force $dst -ErrorAction SilentlyContinue
    foreach ($kv in ($Env -split ";")) { if ($kv -match "^(\w+)=") { Set-Item "env:$($matches[1])" $null } }
    $env:SVR2011_ROUTE = $null; $env:SVR2011_TEST_ROW_LOG = $null; $env:SVR2011_FPS_PROBE = $null
}
Select-String -Path $log -Pattern "row picked|backstage area|gimmick|match: rule|BG78|access violation|crash:" | Select-Object -First 30 | ForEach-Object { $_.Line.Substring([Math]::Min(26, $_.Line.Length)) }
