# Bisects a superstar mod's moves.txt for lines that make the match lose
# generic motions (SVR2011_TEST_MOVE_LOG=missing count above -Limit).
#   w13c_bisect.ps1 -Moves <full moves.txt> [-Limit 200]
param([Parameter(Mandatory = $true)][string]$Moves, [int]$Limit = 200)
$mod = "D:\Xbox Games Ports\SvR2011 Arenas\port\runs\opt_w13c_game\Mods\Superstars\mankind\moves.txt"
$lines = @(Get-Content $Moves | Where-Object { $_ -match '^0x' })
function Misses($set, $tag) {
    $set | Set-Content -Encoding ascii $mod
    & (Join-Path $PSScriptRoot "w13c_match.ps1") -Name "bis_$tag" -Shots 45 -Every 2 -MoveLog "missing" | Out-Null
    return (Select-String -Path "D:\Xbox Games Ports\SvR2011 Arenas\port\runs\bis_$tag.log" -Pattern "move log: motion").Count
}
$bad = $lines
$n = 0
while ($bad.Count -gt 1) {
    $half = [int][Math]::Ceiling($bad.Count / 2)
    $a = $bad[0..($half - 1)]
    $m = Misses $a ("a" + $n)
    "lines $($a.Count) (first half of $($bad.Count)): $m misses"
    if ($m -gt $Limit) { $bad = $a } else {
        $b = $bad[$half..($bad.Count - 1)]
        $m2 = Misses $b ("b" + $n)
        "lines $($b.Count) (second half): $m2 misses"
        if ($m2 -gt $Limit) { $bad = $b } else { "neither half alone: it's the total (or a pair)"; break }
    }
    $n++
}
"suspect lines:"; $bad
Copy-Item -Force $Moves $mod
