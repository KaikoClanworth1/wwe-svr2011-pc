# One menu step for the limits test game (lim_session.ps1), checked in its
# log: presses -Keys (lim_nav.ps1 syntax) and waits until a new log line
# matches -Expect; tries again up to -Tries times. Throws when it never does.
#   lim_step.ps1 -Log runs\x.log -Keys "START@2" -Expect "menu select: group 0"
param([Parameter(Mandatory)][string]$Log, [Parameter(Mandatory)][string]$Keys, [string]$Expect = "",
      [int]$Tries = 4, [int]$Wait = 8)
$nav = Join-Path $PSScriptRoot "lim_nav.ps1"
for ($t = 0; $t -lt $Tries; $t++) {
    $before = @(Get-Content $Log -ErrorAction SilentlyContinue).Count
    & $nav -Keys $Keys | Out-Null
    if (-not $Expect) { return }
    $until = (Get-Date).AddSeconds($Wait)
    while ((Get-Date) -lt $until) {
        $hit = Get-Content $Log -ErrorAction SilentlyContinue | Select-Object -Skip $before | Select-String -Pattern $Expect | Select-Object -First 1
        if ($hit) { return $hit.Line.Substring([math]::Min(40, $hit.Line.Length)) }
        Start-Sleep -Milliseconds 500
    }
}
throw "lim_step: '$Expect' never came after $Keys"
