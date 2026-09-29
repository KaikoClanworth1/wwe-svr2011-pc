# Creates a superstar from a template and saves it, in a scratch copy of the
# saves (SVR2011_USER_DATA), then lists the save packages. Muted, in the
# background; never touches the player's saves.
#   caw_save_test.ps1 -Name <run> -Seed <folder with the profile dir> [-NoUtilityDrive] [-Renderer main|off]
param([Parameter(Mandatory)] [string]$Name, [Parameter(Mandatory)] [string]$Seed, [switch]$NoUtilityDrive,
      [string]$Renderer = "main")

$ErrorActionPreference = "Stop"
$port = Split-Path $PSScriptRoot -Parent
$s = Join-Path $PSScriptRoot "session.ps1"
$ud = Join-Path $port "runs\$Name`_userdata"
if (Test-Path $ud) { throw "$ud exists; use a new -Name" }
New-Item -ItemType Directory $ud | Out-Null
Copy-Item -Recurse (Join-Path $Seed "*") $ud
# The installed DLC (only read), so the game sees the same content as the player's.
$dlc = Join-Path (Split-Path $port -Parent) "Game Files\UserData\0000000000000000"
if (Test-Path $dlc) { cmd /c mklink /J "$ud\0000000000000000" "$dlc" | Out-Null }

$env:SVR2011_USER_DATA = $ud
if ($NoUtilityDrive) { $env:SVR2011_NO_UTILITY_DRIVE = "1" }
& $s start -Name $Name "--native_renderer=$Renderer" | Out-Null
Remove-Item Env:SVR2011_USER_DATA, Env:SVR2011_NO_UTILITY_DRIVE -ErrorAction SilentlyContinue

function Press($b, $wait) { & $s input "press $b 200" | Out-Null; Start-Sleep -Milliseconds ([int]($wait * 1000)) }
Start-Sleep 45
& $s shot "$Name`_0title" | Out-Null
Press START 40; & $s shot "$Name`_1" | Out-Null
Press A 5; Press START 10                                 # title -> main menu
& $s shot "$Name`_2menu" | Out-Null
Press DOWN 1; Press DOWN 1; Press DOWN 1; Press A 3       # CREATE MODES
Press DOWN 1; Press A 3; Press A 30                       # CREATE A SUPERSTAR -> NEW
& $s shot "$Name`_3editor" | Out-Null
Press A 4; Press A 8                                      # template: SUPERSTAR, the first
Press RB 1.5; Press RB 1.5; Press RB 1.5                  # OTHER
Press UP 1.5; Press A 4                                   # FINALIZE
Press LEFT 1; Press A 6                                   # default gear: YES
Press UP 1.5; & $s shot "$Name`_form" | Out-Null
Press A 25                                                # SUBMIT APPLICATION
& $s shot "$Name`_after" | Out-Null
& $s stop | Out-Null

Get-ChildItem $ud -Recurse -File | Where-Object { $_.FullName -notmatch '\\0000000000000000\\' } |
    ForEach-Object { "{0,12}  {1:HH:mm:ss}  {2}" -f $_.Length, $_.LastWriteTime, $_.FullName.Substring($ud.Length + 1) }
