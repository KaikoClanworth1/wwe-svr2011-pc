# Screenshot of the Mod Maker without taking focus: started with --behind (a
# no-activate tool window kept under every other window, not on the taskbar),
# captured with PrintWindow. Never shown in front: the user may be playing.
#   modmaker_shot.ps1 [-Exe <Mod Maker exe>] [-Game <game folder>] [-Out runs\modmaker.png] [-Wait 6]
#                     [-Extra "--editor 4 --editor-view 0"]
param([string]$Exe = "", [string]$Game = "", [string]$Out = "", [int]$Wait = 6, [string]$Extra = "")
$port = Split-Path $PSScriptRoot -Parent
if (-not $Exe) { $Exe = Join-Path $port "out\build\SourceArenas\SvR2011 Mod Maker.exe" }
if (-not $Game) { $Game = Join-Path $port "runs\opt_arena_game" }
if (-not $Out) { $Out = Join-Path $port "runs\modmaker.png" }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class MW {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][MW]::SetProcessDPIAware()
$al = @("--game", "`"$Game`"", "--behind")
if ($Extra) { $al += $Extra }
$p = Start-Process $Exe -ArgumentList $al -PassThru -WindowStyle Hidden
# (a tool window has no MainWindowHandle: find it by class)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class MWF { [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string c, string t); }
"@
$h = [IntPtr]::Zero
for ($t = 0; $t -lt 100 -and $h -eq [IntPtr]::Zero; $t++) { Start-Sleep -Milliseconds 100; $h = [MWF]::FindWindowW("SvR2011ModMaker", $null) }
[void][MW]::SetWindowPos($h, [IntPtr]1, 0, 0, 0, 0, 0x0010 -bor 0x0002 -bor 0x0001)  # HWND_BOTTOM, NOACTIVATE | NOMOVE | NOSIZE
Start-Sleep $Wait
$r = New-Object MW+RECT; [void][MW]::GetWindowRect($h, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
[void][MW]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
$Out
