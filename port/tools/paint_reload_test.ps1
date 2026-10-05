# Paint Tool logos changed outside the game while it runs (src/paint_pages.cpp),
# in the limits test game (lim_session.ps1): opens Create A Superstar ->
# PAINT TOOL (runs\<Name>_grid1.png), leaves it (B, OK), changes the test
# saves' 00PaintTool.pt as the launcher would (slots 1 and 3 swapped, written
# through a temp file + rename), opens PAINT TOOL again (runs\<Name>_grid2.png).
# The .pt is restored afterwards.
#   paint_reload_test.ps1 [-Name prl] [-Inside]
#   -Inside: the change is made while the grid is open, then the grid is left
#            and opened again (runs\<Name>_live.png: the grid 4 s after the change).
param([string]$Name = "prl", [switch]$Inside)
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$env:SVR2011_TEST_PAINT_TRACE = "1"
$pt = Join-Path $runs "test_userdata_lim\Saves\00PaintTool.pt"
$backup = Join-Path $runs "$Name.pt.bak"
Copy-Item $pt $backup -Force
function Change {
  python (Join-Path $tools "pt_tool.py") swap $pt "$pt.tmp" 1 3 | Out-Null
  Move-Item "$pt.tmp" $pt -Force
  "changed the .pt outside the game"
}
try {
  & (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
  $env:SVR2011_TEST_PAINT_TRACE = $null
  $nav = Join-Path $tools "lim_nav.ps1"
  & $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                         # title -> main menu
  & $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@3,DOWN@1.5,A@4" | Out-Null          # CREATE MODES -> CREATE A SUPERSTAR
  & $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@20" -Shot "${Name}_grid1" | Out-Null  # PAINT TOOL
  if ($Inside) { Change; Start-Sleep 4; & (Join-Path $tools "lim_session.ps1") shot "${Name}_live" | Out-Null }
  & $nav -Keys "B@8" -Shot "${Name}_left" | Out-Null                              # leave (the game saves)
  & $nav -Keys "A@25" -Shot "${Name}_title" | Out-Null                            # OK (-> the title screen)
  if (-not $Inside) { Change }
  & $nav -Keys "START@6" -Shot "${Name}_menu" | Out-Null                          # main menu
  & $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@3,DOWN@1.5,A@4" -Shot "${Name}_caw" | Out-Null
  & $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@20" -Shot "${Name}_grid2" | Out-Null  # PAINT TOOL again
  "the .pt after the game left the Paint Tool:"
  python (Join-Path $tools "pt_tool.py") info $pt
  Select-String -Path (Join-Path $runs "$Name.log") -Pattern "paint trace|paint pages" | ForEach-Object { $_.Line.Substring(26) }
} finally {
  & (Join-Path $tools "lim_session.ps1") stop | Out-Null
  Copy-Item $backup $pt -Force
}
