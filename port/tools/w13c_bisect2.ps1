# Two-stage bisect for a PAIR of moves.txt lines that only break things together
# (SVR2011_TEST_MOVE_LOG=missing count above -Limit): with half A always on,
# narrow half B to one line; then with that line on, narrow half A.
param([Parameter(Mandatory = $true)][string]$Moves, [int]$Limit = 200)
$mod = "D:\Xbox Games Ports\SvR2011 Arenas\port\runs\opt_w13c_game\Mods\Superstars\mankind\moves.txt"
$lines = @(Get-Content $Moves | Where-Object { $_ -match '^0x' })
$script:n = 0
function Bad($set) {
    $script:n++
    $set | Set-Content -Encoding ascii $mod
    & (Join-Path $PSScriptRoot "w13c_match.ps1") -Name "bs2_$($script:n)" -Shots 45 -Every 2 -MoveLog "missing" | Out-Null
    $m = (Select-String -Path "D:\Xbox Games Ports\SvR2011 Arenas\port\runs\bs2_$($script:n).log" -Pattern "move log: motion").Count
    "  run $($script:n): $($set.Count) lines -> $m misses"
    return $m -gt $Limit
}
function Narrow($fixed, $pool) {
    while ($pool.Count -gt 1) {
        $half = [int][Math]::Ceiling($pool.Count / 2)
        $a = @($pool[0..($half - 1)]); $b = @($pool[$half..($pool.Count - 1)])
        $r = Bad (@($fixed) + $a)
        if ($r[-1]) { $pool = $a } else { $pool = $b }
        $r[0..($r.Count - 2)]
    }
    return , $pool
}
$h = [int][Math]::Ceiling($lines.Count / 2)
$A = @($lines[0..($h - 1)]); $B = @($lines[$h..($lines.Count - 1)])
"stage 1: half A on, narrowing half B ($($B.Count) lines)"
$res = Narrow $A $B; $lb = $res[-1]
"culprit in B: $lb"
"stage 2: that line on, narrowing half A ($($A.Count) lines)"
$res = Narrow $lb $A; $la = $res[-1]
"culprit in A: $la"
Copy-Item -Force $Moves $mod
