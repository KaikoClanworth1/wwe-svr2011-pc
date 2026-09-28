# Boots (test saves) into CREATE AN ENTRANCE -> Alicia Fox -> Easy Creation ->
# FINALIZE and keeps the session there (screenshot runs\<Name>_fin.png).
#   cae_start.ps1 -Name <run>
param([Parameter(Mandatory)][string]$Name)
$s = Join-Path $PSScriptRoot "session.ps1"
function Step([string[]]$k, [int]$sec) { & $s input @k | Out-Null; Start-Sleep $sec }
& $s start -Name $Name | Out-Null
Start-Sleep 50
Step @("press START 200") 40; Step @("press A 200") 5; Step @("press START 200") 8
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 5    # CREATE MODES
for ($i = 0; $i -lt 3; $i++) { Step @("press DOWN 150") 1 }; Step @("press A 200") 15   # CREATE AN ENTRANCE
Step @("press A 200") 8; Step @("press A 200") 15                                       # SUPERSTAR, Alicia Fox
Step @("press UP 150") 1; Step @("press A 200") 20                                      # Easy Creation
for ($i = 0; $i -lt 5; $i++) { Step @("press RB 150") 3 }; Start-Sleep 5                # FINALIZE
& $s shot "${Name}_fin" | Out-Null
