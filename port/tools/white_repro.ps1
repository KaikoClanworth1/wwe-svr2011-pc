# Screenshots the load from the title screen into the training ring (the
# first ring load), several a second, to catch white or blank frames.
#   white_repro.ps1 [-Backend vulkan|d3d12] [-Seconds 40]
# Prints each shot's mean brightness (0-255).
param([string]$Backend = "vulkan", [int]$Seconds = 40, [string]$Name = "white")
$ErrorActionPreference = "Continue"
$t = Join-Path $PSScriptRoot "session.ps1"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$dir = Join-Path $runs "white_$Backend"
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir | Out-Null
& $t start -Name $Name "--gpu_backend=$Backend" | Out-Null
$log = Join-Path $runs "$Name.log"
# The title screen is up about a minute after the start (intro movies).
Start-Sleep 62
& $t input "press START 200" "wait 1500" "press START 200" | Out-Null
$start = Get-Date
Add-Type -AssemblyName System.Drawing
for ($i = 0; ((Get-Date) - $start).TotalSeconds -lt $Seconds; $i++) {
    $stem = "white_$Backend\" + $i.ToString("000")
    & $t shot $stem -Name $Name | Out-Null
    if ($i -in 8, 16, 24) { & $t input "press A 200" | Out-Null }  # (Load Successful)
    $f = Join-Path $runs "$stem.png"
    if (Test-Path $f) {
        $b = [System.Drawing.Bitmap]::FromFile($f); $sum = 0; $n = 0
        for ($x = 40; $x -lt $b.Width; $x += 97) { for ($y = 40; $y -lt $b.Height; $y += 61) { $c = $b.GetPixel($x, $y); $sum += ($c.R + $c.G + $c.B) / 3; $n++ } }
        $b.Dispose()
        "{0:000} {1,5:N1}s {2,4:N0}" -f $i, ((Get-Date) - $start).TotalSeconds, ($sum / $n)
    }
}
"shots in $dir"

# (the test game ends with the capture)
Get-CimInstance Win32_Process -Filter "Name='svr2011.exe'" | Where-Object { $_.CommandLine -match 'port\\runs' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
