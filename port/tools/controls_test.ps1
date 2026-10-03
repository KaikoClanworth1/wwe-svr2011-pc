# CONTROLS tab test (graphics_page.cpp), in the limits test game
# (lim_session.ps1, its own config copy): MY WWE -> OPTIONS -> GRAPHICS, RB
# twice to CONTROLS; A on "A" then the key G (SVR2011_TEST_PAGE_KEYS) ->
# keybind_a = G; DOWN, X -> keybind_b cleared; Y then Shift+H -> added; UP
# (wraps) to RESET TO DEFAULTS, A. Screenshots runs\<Name>_*.png, the log's
# "controls:" lines and the config's keybind_ lines.
#   controls_test.ps1 [-Name ctl]
param([string]$Name = "ctl")
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$cfg = Join-Path $runs "test_config_controls.toml"
Copy-Item (Join-Path $runs "test_config.toml") $cfg -Force
$env:SVR2011_CONFIG = $cfg
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$keys = Join-Path $runs "${Name}_keys.txt"; Set-Content $keys "" -NoNewline
$env:SVR2011_TEST_PAGE_KEYS = $keys
& (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
$env:SVR2011_TEST_PAGE_KEYS = $null
$nav = Join-Path $tools "lim_nav.ps1"
$s = Join-Path $tools "lim_session.ps1"
& $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null              # main menu
& $nav -Keys "DOWN@1,DOWN@1,DOWN@1,DOWN@1,DOWN@1,A@2" | Out-Null      # MY WWE
& $nav -Keys "DOWN@1,DOWN@1,DOWN@1,A@2" | Out-Null                    # OPTIONS
& $nav -Keys "DOWN@1,DOWN@1,DOWN@1,DOWN@1,DOWN@1,A@2" | Out-Null      # GRAPHICS
& $nav -Keys "RB@1,RB@1" | Out-Null                                   # CONTROLS
& $s shot "${Name}_tab" | Out-Null
& $nav -Keys "A@1" | Out-Null
& $s shot "${Name}_wait" | Out-Null
Set-Content $keys "G"; Start-Sleep 2                                    # G
& $s shot "${Name}_a" | Out-Null
& $nav -Keys "DOWN@1,X@1" | Out-Null                                  # B: clear
& $nav -Keys "Y@1" | Out-Null
Set-Content $keys "Shift+H"; Start-Sleep 2                              # Shift+H added
& $s shot "${Name}_b" | Out-Null
& $nav -Keys "UP@1,UP@1,A@1" | Out-Null                               # RESET TO DEFAULTS
& $s shot "${Name}_reset" | Out-Null
Select-String -Path (Join-Path $runs "$Name.log") -Pattern "controls:" | ForEach-Object { $_.Line.Substring(50) }
Select-String -Path $cfg -Pattern "^keybind_(a|b) " | ForEach-Object { $_.Line }
& $s stop | Out-Null
