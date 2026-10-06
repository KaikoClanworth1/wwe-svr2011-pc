# WWE '13 character work: a muted background match on its own game
# (runs\opt_w13c_game, own saves runs\test_userdata_w13c, own session state).
# Shots runs\<Name>_<i>.png every -Every seconds; -Force "att,tgt,secs,motion,..."
# sets SVR2011_TEST_FORCE_MOVE (match_types.cpp); -MoveLog ids logs motion lookups.
#   w13c_match.ps1 -Name mk1 [-People "MANKIND,RANDY ORTON"] [-Shots 30] [-Every 2] [-Force ...] [-MoveLog ...] [-Rule 0D]
param([string]$Name = "w13c", [string]$People = "MANKIND,RANDY ORTON", [int]$Shots = 30, [double]$Every = 2,
      [string]$Force = "", [string]$MoveLog = "", [string]$Rule = "", [string[]]$Acts = @(), [int]$Settle = 0)
$runs = "D:\Xbox Games Ports\SvR2011 Arenas\port\runs"
$sess = Join-Path $PSScriptRoot "w13c_session.ps1"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_w13c"
robocopy (Join-Path $runs "test_userdata_arena_snap") $env:SVR2011_USER_DATA /MIR /XJ /NFL /NDL /NJH /NJS /NP | Out-Null
$env:SVR2011_ROUTE = "match"; $env:SVR2011_TEST_MATCH = "people=$People arena=1"; $env:SVR2011_TEST_RUN = "1"
$env:SVR2011_TEST_FORCE_MOVE = $Force; $env:SVR2011_TEST_MOVE_LOG = $MoveLog; $env:SVR2011_TEST_RULE = $Rule
try {
    & $sess start -Name $Name | Out-Null
    # -Acts: script inputs (src/script_input.h), one queued before each shot once -Settle seconds have passed
    for ($i = 0; $i -lt $Shots; $i++) {
        Start-Sleep -Milliseconds ([int]($Every * 1000))
        if ($Acts.Count -and ($i * $Every) -ge $Settle) { & $sess input $Acts[$i % $Acts.Count] | Out-Null }
        & $sess shot "${Name}_$i" | Out-Null
    }
} catch { "ERR $_" } finally { & $sess stop | Out-Null }
foreach ($v in "SVR2011_ROUTE", "SVR2011_TEST_MATCH", "SVR2011_TEST_RUN", "SVR2011_TEST_FORCE_MOVE", "SVR2011_TEST_MOVE_LOG", "SVR2011_TEST_RULE") {
    [Environment]::SetEnvironmentVariable($v, $null, "Process")
}
$log = Join-Path $runs "$Name.log"
"crash lines: " + (Select-String -Path $log -Pattern "crash:" | Measure-Object).Count
Select-String -Path $log -Pattern "move packs: (m\.pac|mpsp|merg|\d+ move)|superstar mods: Mankind|starts motion|match: rule 00, arena 1|move log: key" |
    Select-Object -First 20 | ForEach-Object { $_.Line.Substring([Math]::Min(40, $_.Line.Length)) }
