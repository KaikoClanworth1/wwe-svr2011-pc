param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][string]$Commands, [string[]]$Arg = @(),
      [string]$Backend = "vulkan", [int]$Slot = 9, [int]$HoldAt = 25, [int]$WaitSecs = 900, [string]$Out = "")
# scene_test - one held match for comparing settings on the same frame (SVR2011_TEST_SCENE, native_renderer.cpp). For
# us only: tools\dev is never packaged.
#
#   scene_test.ps1 -Tag t1 -Arg "--arena_crowd=off" `
#                  -Commands "wait 60;shot E:\x\on.ppm;gpu 120 on;set native_shadows false;wait 45;shot E:\x\off.ppm;gpu 120 off"
#
# A normal match through svrtest (hidden, muted, scripted input, private data), held -HoldAt seconds after it starts
# (the free camera: F6, then F7 hides the UI and F8 holds the world); then <Commands> (';' between lines) go to the
# scene file. The game is stopped by its PID at the end and the log's "scene" lines are printed. Keep the crowd off
# (--arena_crowd=off): its animation goes on while the match is held - everything else is the same frame, pixel for
# pixel (tried 12 Nov, Vulkan and Direct3D 12). GPU times need native_record_thread false (passed here).
$port = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
$w = if ($Out) { $Out } else { Join-Path $port "runs\scene" }
New-Item -ItemType Directory -Force $w | Out-Null
$script = "$w\scene_$Tag.txt"
[IO.File]::WriteAllLines($script, [string[]]@("route match"))
$scene = "$w\scene_$Tag.cmd"
[IO.File]::WriteAllText($scene, "")
$env:SVR2011_TEST_SCENE = $scene
$a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSScriptRoot\svrtest.ps1`"", "script", "`"$script`"",
       "--backend", $Backend, "--slot", $Slot, "--name", "scene_$Tag", "--keep", "--arg", "--native_record_thread=false")
foreach ($x in $Arg) { $a += "--arg", $x }
$start = Get-Date
$drv = Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Hidden
$log = $null
try {
  $deadline = (Get-Date).AddSeconds($WaitSecs)
  $sent = $false; $held = $false; $matchAt = $null
  $want = @($Commands -split ';' | Where-Object { $_ -match '^\s*gpu ' }).Count
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $d = Get-ChildItem "$port\runs\svrtest" -Directory -Filter "*scene_$Tag" | Where-Object { $_.LastWriteTime -ge $start.AddSeconds(-5) } |
         Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $d -or -not (Test-Path "$($d.FullName)\game.log")) { continue }
    $log = "$($d.FullName)\game.log"
    $t = Get-Content $log -ErrorAction SilentlyContinue
    if (-not $held -and -not $matchAt -and ($t | Select-String -SimpleMatch "script input: all steps done")) { $matchAt = Get-Date }
    if (-not $held -and $matchAt -and ((Get-Date) - $matchAt).TotalSeconds -ge $HoldAt) {
      # (a hotkey runs when its line is read: added to the game's input file now, with the match running;
      # F8 needs the free camera on, which happens a frame after F6)
      [IO.File]::AppendAllLines("$($d.FullName)\input.txt", [string[]]@("hotkey F6"))
      Start-Sleep -Seconds 2
      [IO.File]::AppendAllLines("$($d.FullName)\input.txt", [string[]]@("hotkey F7", "hotkey F8"))
      $held = $true
    }
    if (-not $sent -and ($t | Select-String -SimpleMatch "free camera: match paused")) {
      Start-Sleep -Seconds 2
      [IO.File]::WriteAllLines($scene, [string[]]($Commands -split ';' | ForEach-Object { $_.Trim() }))
      $sent = $true
    }
    if ($sent -and @($t | Select-String -SimpleMatch "scene gpu:").Count -ge $want -and (Get-Item $scene).Length -eq 0) {
      Start-Sleep 3
      break
    }
  }
  if (-not $sent) { "$Tag : the match was never held" }
  "log=$log"
  if ($log) {
    Get-Content $log | Select-String "scene|free camera: match|online_enabled false" |
      ForEach-Object { $_.Line.Substring([Math]::Min(46, $_.Line.Length)) }
  }
} finally {
  Remove-Item Env:SVR2011_TEST_SCENE -ErrorAction SilentlyContinue
  Stop-Process -Id $drv.Id -Force -ErrorAction SilentlyContinue
  Get-CimInstance Win32_Process -Filter "name='svr2011.exe'" | Where-Object { $_.CommandLine -match "scene_$Tag" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped game $($_.ProcessId)" }
}
