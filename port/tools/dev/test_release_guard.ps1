# The release guard's own test (docs/DEBUG_SUITE_PLAN.md 3): a clean fake package passes; each kind of
# debug-suite leak fails it. Prints PASS / FAIL per case; exit 1 when any case is wrong.
#   powershell -File tools\dev\test_release_guard.ps1 [-Dbg out\build\DebugSuite\svr2011_dbg.exe] [-Release out\build\SourceRelease\svr2011.exe]
param([string]$Dbg = "", [string]$Release = "")
$ErrorActionPreference = "Stop"
. (Join-Path (Split-Path $PSScriptRoot -Parent) "release_guard.ps1")
$root = Join-Path ([IO.Path]::GetTempPath()) ("svr_guard_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$wrong = 0
function Case([string]$Name, [bool]$ShouldFail, [scriptblock]$Setup) {
    $dir = Join-Path $root $Name
    New-Item -ItemType Directory -Force $dir | Out-Null
    Set-Content (Join-Path $dir "readme.txt") "a release file"
    [IO.File]::WriteAllBytes((Join-Path $dir "svr2011.exe"), [Text.Encoding]::ASCII.GetBytes("MZ a clean game " + ("x" * 5000000)))
    & $Setup $dir
    $failed = $false
    try { Assert-NoDebugSuite $dir } catch { $failed = $true; $why = ($_.Exception.Message -split "`n")[1] }
    $ok = $failed -eq $ShouldFail
    if (-not $ok) { $script:wrong++ }
    "{0}  {1}{2}" -f ($(if ($ok) { "PASS" } else { "FAIL" })), $Name, $(if ($failed) { " - refused:$why" } else { " - accepted" })
}
Case "clean" $false {}
Case "dbg_exe_name" $true { param($d) Set-Content (Join-Path $d "svr2011_dbg.exe") "x" }
Case "debug_suite_folder" $true { param($d) New-Item -ItemType Directory (Join-Path $d "debug_suite") | Out-Null; Set-Content (Join-Path $d "debug_suite\a.txt") "x" }
Case "tools_dev_folder" $true { param($d) New-Item -ItemType Directory -Force (Join-Path $d "tools\dev") | Out-Null; Set-Content (Join-Path $d "tools\dev\svrtest.ps1") "x" }
Case "marker_in_exe" $true { param($d)
    # (the marker straddling a 4 MB chunk boundary: the overlap has to find it)
    $b = [Text.Encoding]::ASCII.GetBytes(("y" * ((4 -shl 20) - 3)) + "SVRDBG: x" + ("y" * 100))
    [IO.File]::WriteAllBytes((Join-Path $d "svr2011.exe"), $b) }
Case "dbg_option_in_dll" $true { param($d) [IO.File]::WriteAllBytes((Join-Path $d "rexruntime.dll"), [Text.Encoding]::ASCII.GetBytes("zz --dbg_boot=match zz")) }
Case "marker_in_apk_so" $true { param($d)
    New-Item -ItemType Directory (Join-Path $d "Android") | Out-Null
    $apk = Join-Path $d "Android\SvR2011.apk"
    $z = [IO.Compression.ZipFile]::Open($apk, [IO.Compression.ZipArchiveMode]::Create)
    $e = $z.CreateEntry("lib/arm64-v8a/libmain.so")
    $w = $e.Open(); $bytes = [Text.Encoding]::ASCII.GetBytes("ELF ... SVRDBG: ..."); $w.Write($bytes, 0, $bytes.Length); $w.Close()
    $z.Dispose() }
# the real binaries, when given: the dbg build must be refused, the release one accepted
if ($Dbg) { Case "real_dbg_exe" $true { param($d) Copy-Item $Dbg (Join-Path $d "svr2011.exe") } }
if ($Release) { Case "real_release_exe" $false { param($d) Copy-Item $Release (Join-Path $d "svr2011.exe") } }
Remove-Item -Recurse -Force $root
if ($wrong) { "$wrong case(s) wrong"; exit 1 }
"all cases right"
