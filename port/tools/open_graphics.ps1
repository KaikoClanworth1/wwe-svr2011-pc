# Starts a test game (test settings and saves) and opens MY WWE -> OPTIONS ->
# GRAPHICS, one press per step (the menus drop presses sent too quickly).
#   open_graphics.ps1 [-Name gfx] [-Extra --gpu_backend=vulkan]
param([string]$Name = "gfx", [string[]]$Extra = @(), [string]$Config = "test_config.toml")
$ErrorActionPreference = "Continue"
$runs = Join-Path (Split-Path $PSScriptRoot -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs $Config  # (the page saves its changes there)
$s = Join-Path $PSScriptRoot "session.ps1"
$nav = Join-Path $PSScriptRoot "nav.ps1"
& $s start -Name $Name @Extra | Out-Null
Start-Sleep 62                                     # intro movies -> title
function Key($k, $w) { & $nav -Shot "${Name}_nav" -Keys $k -Wait $w | Out-Null }
Key START 8; Key START 8                           # title -> load save
Key A 6                                            # Load Successful
Key START 4                                        # main menu
foreach ($i in 1..5) { Key DOWN 1 }                # MY WWE
Key A 2
foreach ($i in 1..3) { Key DOWN 1 }                # OPTIONS
Key A 2
foreach ($i in 1..5) { Key DOWN 1 }                # GRAPHICS
Key A 2
& $s shot "${Name}_page" | Out-Null
"shot: runs\${Name}_page.png"
