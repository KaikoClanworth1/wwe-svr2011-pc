# visual checks V1-V3, one game at a time (below-normal priority)
$h = $PSScriptRoot
$env:H_ACT_DELAY = "1"
$steps = (((1..12 | ForEach-Object { "DOWN" }) -join ",") + "|cat;A|rows;" + ((1..8 | ForEach-Object { "DOWN" }) -join ",") + "|row")
$runs = @(
  @{ n = "v14"; env = "props"; acts = "snap 5;snap 1;snap 1;snap 1;snap 1" },
  @{ n = "v13"; acts = "sleep 2;press X;snap 1;snap 1;snap 1" }
)
foreach ($r in $runs) {
  powershell -ExecutionPolicy Bypass -File "$h\h_row.ps1" -Hctp -ModDir "$h\mod_$($r.n)" -Name $r.n -Steps $steps -Shots 3 -Every 2 -Acts $r.acts -Env "SVR2011_TEST_HCTP_LOG=1;SVR2011_TEST_HCTP_ROOF=$(if ($r.env) { $r.env } else { 1 });SVR2011_TEST_STATE_LOG=212" | Select-String "access violation|game is not running"
  $log = Get-Content "D:\Xbox Games Ports\SvR2011 HCTP\port\runs\$($r.n).log"; $i = ($log | Select-String "starts at").LineNumber
  "== $($r.n)"
  $log[$i..($log.Count-1)] | Select-String "hctp: [a-z]|ring state: 0|crash" | Where-Object { $_.Line -notmatch "motion key|T22 search|wall switch" } | ForEach-Object { $_.Line.Substring(40) } | Select-Object -First 60
}
