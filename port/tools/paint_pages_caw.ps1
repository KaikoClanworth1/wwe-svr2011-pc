# Paint Tool pages in the Created Superstar logo picker (src/paint_pages.h), in
# the limits test game (lim_session.ps1): boots to Create A Superstar -> EDIT
# -> Superstar 1 -> HEAD -> TATTOOS -> PAINT TOOL DATA (runs\<Name>_picker.png),
# then presses -Then keys one at a time with a screenshot after each.
#   paint_pages_caw.ps1 [-Name ppc] [-Then RB,RB,LB]
param([string]$Name = "ppc", [string[]]$Then = @())
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$nav = Join-Path $tools "lim_nav.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                    # title -> main menu
& $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@3,DOWN@1.5,A@4" | Out-Null     # CREATE MODES -> CREATE A SUPERSTAR
& $nav -Keys "DOWN@1.5,A@10,A@5,A@5" | Out-Null                           # EDIT -> Superstar 1 -> ORIGINAL
& $nav -Keys "A@45" | Out-Null                                            # EDIT (loads the editor)
$log = Join-Path $runs "$Name.log"
& $nav -Keys "UP@2,A@5,UP@2,A@6" -Shot "${Name}_picker" | Out-Null        # HEAD -> TATTOOS -> PAINT TOOL DATA
$i = 1
foreach ($k in $Then) { & $nav -Keys "$k@6" -Shot "${Name}_$i" | Out-Null; $i++ }
Select-String -Path $log -Pattern "paint pages" | ForEach-Object { $_.Line.Substring(26) }
