# Background check for more than 2 High Resolution logos (test saves hold three
# 256x256 Paint Tool logos): CAW editor -> HEAD tattoo logo 1, BODY TORSO logo 2,
# BODY BACK logo 3 (the game's own limit is 2). Screenshots runs\<Name>_1..3.
#   caw_logos_test.ps1 [-Name cl] [-Keep]
param([string]$Name = "cl", [switch]$Keep, [int]$BackLogo = 3)
$s = Join-Path $PSScriptRoot "session.ps1"
function P($b, $w = 4) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($w * 1000)) }
& (Join-Path $PSScriptRoot "caw_start.ps1") -Name $Name
P UP; P A 5; P UP; P A 5                                  # HEAD -> TATTOOS -> PAINT TOOL DATA
P A; P LEFT; P A 6                                        # logo 1, notice YES
& $s shot "${Name}_1" | Out-Null
P A; P B; P RB 5; P UP; P A 5; P UP; P A 5; P A 5         # BODY -> TATTOOS -> PAINT TOOL DATA -> TORSO
P DOWN; P A 8                                             # logo 2
& $s shot "${Name}_2" | Out-Null
P A; P DOWN; P A 5                                        # BACK
for ($i = 1; $i -lt $BackLogo; $i++) { P DOWN 1.5 }
P A 12                                                    # logo 3 (or -BackLogo 1/2)
& $s shot "${Name}_3" | Out-Null
$log = Join-Path (Split-Path $PSScriptRoot -Parent) "runs\$Name.log"
Select-String -Path $log -Pattern "caw logos|TEMP |violation|FatalError" | ForEach-Object { $_.Line.Substring(12) } | Select-Object -Last 40
if (-not $Keep) { & $s stop | Out-Null }
