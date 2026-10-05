# Per-thread CPU use of a running game, without stopping it (unlike the
# sampler): each thread's CPU time over -Seconds, as % of one core, with its
# name (guest threads: "<name> (F80000xx)").
#   thread_cpu.ps1 -Id <pid> [-Seconds 10] [-Top 16]
param([Parameter(Mandatory)][int]$Id, [int]$Seconds = 10, [int]$Top = 16)
if (-not ("ThreadNames" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class ThreadNames {
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(int access, bool inherit, int id);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern int GetThreadDescription(IntPtr h, out IntPtr d);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
  public static string Name(int id) {
    IntPtr h = OpenThread(0x0800, false, id);  // THREAD_QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) return "";
    IntPtr d; string s = "";
    if (GetThreadDescription(h, out d) >= 0 && d != IntPtr.Zero) { s = Marshal.PtrToStringUni(d); LocalFree(d); }
    CloseHandle(h); return s;
  }
}
"@
}
$p = Get-Process -Id $Id
$before = @{}
foreach ($t in $p.Threads) { try { $before[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
$t0 = Get-Date
Start-Sleep -Seconds $Seconds
$p.Refresh()
$wall = ((Get-Date) - $t0).TotalMilliseconds
$rows = foreach ($t in $p.Threads) {
    try { $now = $t.TotalProcessorTime.TotalMilliseconds } catch { continue }
    $was = if ($before.ContainsKey($t.Id)) { $before[$t.Id] } else { 0 }
    [pscustomobject]@{ Tid = $t.Id; Core = [math]::Round(100 * ($now - $was) / $wall, 1); Prio = $t.PriorityLevel; Name = [ThreadNames]::Name($t.Id) }
}
$total = ($rows | Measure-Object Core -Sum).Sum
"process: {0:N0}% of a core over {1:N1} s" -f $total, ($wall / 1000)
$rows | Sort-Object Core -Descending | Select-Object -First $Top | Format-Table -AutoSize | Out-String -Width 200
