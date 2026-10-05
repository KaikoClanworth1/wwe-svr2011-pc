# Paint Tool logos changed outside the game while the Created Superstar editor
# runs (src/paint_pages.cpp: the logo list is made again when the picker
# opens), in the limits test game (lim_session.ps1): opens HEAD -> TATTOOS ->
# PAINT TOOL DATA (runs\<Name>_picker1.png), closes it, empties logo 1 of the
# test saves' 00PaintTool.pt as the launcher would (temp file + rename),
# opens the picker again (runs\<Name>_picker2.png). The .pt is restored.
#   paint_reload_caw.ps1 [-Name prc]
param([string]$Name = "prc")
$tools = $PSScriptRoot
$runs = Join-Path (Split-Path $tools -Parent) "runs"
$env:SVR2011_CONFIG = Join-Path $runs "test_config.toml"
$env:SVR2011_USER_DATA = Join-Path $runs "test_userdata_lim"
$pt = Join-Path $runs "test_userdata_lim\Saves\00PaintTool.pt"
$backup = Join-Path $runs "$Name.pt.bak"
Copy-Item $pt $backup -Force
try {
  & (Join-Path $tools "lim_session.ps1") start -Name $Name | Out-Null
  $nav = Join-Path $tools "lim_nav.ps1"
  & $nav -Keys "BACK@45,START@40,A@6,START@10" | Out-Null                    # title -> main menu
  & $nav -Keys "DOWN@1.5,DOWN@1.5,DOWN@1.5,A@3,DOWN@1.5,A@4" | Out-Null     # CREATE MODES -> CREATE A SUPERSTAR
  & $nav -Keys "DOWN@1.5,A@10,A@5,A@5,A@5" | Out-Null                       # EDIT -> Superstar 1 -> EDIT -> ORIGINAL -> EDIT
  & $nav -Keys "A@45" | Out-Null                                            # EDIT (loads the editor)
  & $nav -Keys "UP@2,A@5,UP@2,A@6" -Shot "${Name}_picker1" | Out-Null       # HEAD -> TATTOOS -> PAINT TOOL DATA
  & $nav -Keys "B@4" -Shot "${Name}_closed" | Out-Null
  python (Join-Path $tools "pt_tool.py") clear $pt "$pt.tmp" 1 | Out-Null
  Move-Item "$pt.tmp" $pt -Force
  "emptied logo 1 outside the game"
  & $nav -Keys "A@6" -Shot "${Name}_picker2" | Out-Null                     # PAINT TOOL DATA again
  Select-String -Path (Join-Path $runs "$Name.log") -Pattern "paint pages" | ForEach-Object { $_.Line.Substring(26) }
} finally {
  & (Join-Path $tools "lim_session.ps1") stop | Out-Null
  Copy-Item $backup $pt -Force
}
