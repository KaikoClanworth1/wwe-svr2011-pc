# The release guard (docs/DEBUG_SUITE_PLAN.md 3): a package must hold nothing of the debug / test suite.
#   . .\release_guard.ps1; Assert-NoDebugSuite <staged release folder> [<apk>...]
# Fails (throws) when a file is named *_dbg* or sits under a debug_suite / tools\dev folder, or when an exe, dll,
# so or apk (its lib/*.so inside) holds the suite's marker bytes "SVRDBG:" or its "--dbg_" options.
# tools\dev\test_release_guard.ps1 checks that it does.

Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
if (-not ("SvrGuard" -as [type])) {
    Add-Type @"
using System; using System.IO;
public static class SvrGuard {
  // Whether `stream` holds any of the patterns (read in 4 MB chunks, overlapping by the longest pattern).
  public static string Find(Stream s, byte[][] pats) {
    int keep = 0; foreach (var p in pats) keep = Math.Max(keep, p.Length);
    byte[] buf = new byte[(4 << 20) + keep]; int have = 0, n;
    while ((n = s.Read(buf, have, buf.Length - have)) > 0) {
      have += n;
      foreach (var p in pats)
        for (int i = 0; i + p.Length <= have; ++i) {
          if (buf[i] != p[0]) continue;
          int k = 1; while (k < p.Length && buf[i + k] == p[k]) ++k;
          if (k == p.Length) return System.Text.Encoding.ASCII.GetString(p);
        }
      int tail = Math.Min(keep - 1, have);
      Buffer.BlockCopy(buf, have - tail, buf, 0, tail); have = tail;
    }
    return null;
  }
}
"@
}

function Assert-NoDebugSuite([string]$Dir, [string[]]$Apks = @()) {
    $pats = [byte[][]]@([Text.Encoding]::ASCII.GetBytes("SVRDBG:"), [Text.Encoding]::ASCII.GetBytes("--dbg_"))
    $bad = @()
    foreach ($f in Get-ChildItem $Dir -Recurse -File -ErrorAction SilentlyContinue) {
        $rel = $f.FullName.Substring((Resolve-Path $Dir).Path.TrimEnd('\').Length + 1)
        if ($f.Name -like "*_dbg*" -or $rel -match '(^|\\)debug_suite(\\|$)' -or $rel -match '(^|\\)tools\\dev(\\|$)') { $bad += "$rel (a debug-suite file)"; continue }
        if ($f.Extension -in ".exe", ".dll", ".so", ".pyd") {
            $s = [IO.File]::OpenRead($f.FullName)
            try { $hit = [SvrGuard]::Find($s, $pats) } finally { $s.Close() }
            if ($hit) { $bad += "$rel (holds `"$hit`": a debug-suite build)" }
        }
        if ($f.Extension -eq ".apk") { $Apks += $f.FullName }
    }
    foreach ($apk in $Apks | Select-Object -Unique) {
        if (-not (Test-Path $apk)) { continue }
        $z = [IO.Compression.ZipFile]::OpenRead($apk)
        try {
            foreach ($e in $z.Entries) {
                if ($e.FullName -notlike "lib/*.so") { continue }
                $s = $e.Open()
                try { $hit = [SvrGuard]::Find($s, $pats) } finally { $s.Close() }
                if ($hit) { $bad += "$(Split-Path $apk -Leaf): $($e.FullName) (holds `"$hit`": a debug-suite build)" }
            }
        } finally { $z.Dispose() }
    }
    if ($bad) { throw "The package holds the debug / test suite - never in a release (build the release from out\build\SourceRelease):`n  " + ($bad -join "`n  ") }
}
