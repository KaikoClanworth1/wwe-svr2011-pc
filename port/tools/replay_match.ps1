# Replays a match recording (docs/MATCH_RECORDINGS.md) in a hidden test game:
#   .\replay_match.ps1 -Recording <logs\matches\<date>_<people>> -GameDir <game folder> [-UserData <test saves>]
# The match is set up from the recording's match.json - its wrestlers (superstar ids, CAW slots), attires,
# controllers, rule, arena and rand() seed - with the entrances off, through the scripted route to a match
# (SVR2011_ROUTE=match, SVR2011_TEST_MATCH, SVR2011_TEST_RULE). From the bell the game plays the recording's pads
# frame by frame (SVR2011_REPLAY). When they run out, the replay's own recording gets replay.txt: second by second,
# how far it is from the recording's trail.txt, and where the two part. Frame shots (-Shots, one every -ShotEvery
# seconds of the match) go into the replay's folder as PNGs. Prints the replay's folder and replay.txt's first lines.
# Approximate (format 1 recordings): the start places, the game's random table and the loading pace aren't replayed.
# The pre-bell intro (a sneak attack or standing) comes out either way: a replay whose bell finds the people in
# other motions than the recording's is stopped and started again (-Tries times).
param([Parameter(Mandatory)][string]$Recording, [Parameter(Mandatory)][string]$GameDir, [string]$UserData = "",
      [int]$Shots = 6, [int]$ShotEvery = 10, [int]$Tries = 4, [int]$P2pPort = 36547, [string]$Exe = "svr2011.exe")
$env:SVR2011_AUDIO_MUTE = "1"  # (test games stay muted even if their command line is lost)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "test_guard.ps1")

$Recording = (Resolve-Path $Recording).Path
$m = Get-Content -Raw -Encoding UTF8 (Join-Path $Recording "match.json") | ConvertFrom-Json
$wrestlers = @($m.people | Where-Object { $_.person -lt 6 } | Sort-Object person)
if (-not $wrestlers.Count) { throw "no wrestlers in $Recording\match.json" }
# (a Created Superstar is its save slot, ids below 50: SVR2011_TEST_MATCH's CAW<n>)
$ids = $wrestlers | ForEach-Object { if ($_.superstar -lt 50) { "CAW$($_.superstar)" } else { "$($_.superstar)" } }
$attires = $wrestlers | ForEach-Object { if ($_.attire) { $_.attire } else { 1 } }
$ports = $wrestlers | ForEach-Object { if ($null -ne $_.port) { $_.port } else { -1 } }
$seed = if ($m.seeds -and $m.seeds.rand) { "0x$($m.seeds.rand)" } else { "" }

# (the wrestlers' and the referee's motions and places (in 4s) at the bell: trail.txt's first line - "person x y z
# motion" parts)
function Get-BellMotions([string]$trail) {
    $first = Get-Content $trail -ErrorAction SilentlyContinue | Where-Object { $_ -match '^0 \|' } | Select-Object -First 1
    if (-not $first) { return $null }
    return (($first -split '\|' | Select-Object -Skip 1 | ForEach-Object { $f = $_.Trim() -split '\s+'
        if ([int]$f[0] -le 6) { "$($f[0]):$($f[4])@$([math]::Round([double]$f[1] / 4)),$([math]::Round([double]$f[3] / 4))" } }) -join ' ')
}
$wantBell = Get-BellMotions (Join-Path $Recording "trail.txt")

$runs = Join-Path (Split-Path $PSScriptRoot) "runs"
New-Item -ItemType Directory -Force $runs | Out-Null
$stem = "replay_" + (Split-Path $Recording -Leaf)
$log = Join-Path $runs "$stem.log"
$shotDir = Join-Path $runs "$stem.shots"
Remove-Item $shotDir -Recurse -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $shotDir | Out-Null

Assert-NoPlayerGame
$inputFile = Join-Path $runs "$stem.input.txt"  # (the scripted controller runs - and the route with it - with an input file)
Set-Content $inputFile "" -NoNewline
$set = @{
    SVR2011_INPUT_FILE           = $inputFile
    SVR2011_ROUTE                = "match"
    SVR2011_TEST_MATCH           = "people=$($ids -join ',') attires=$($attires -join ',') ports=$($ports -join ',') arena=$($m.arena.id) exact=1"
    SVR2011_TEST_RULE            = "{0:X2}" -f [int]$m.rule.id
    SVR2011_TEST_ENTRANCES       = "0"
    SVR2011_TEST_RAND_SEED       = $seed
    SVR2011_REPLAY               = $Recording
    SVR2011_TEST_FRAME_SHOT      = Join-Path $shotDir "shot.ppm"
    SVR2011_TEST_FRAME_SHOTS     = "$Shots"
    SVR2011_TEST_FRAME_SHOT_AT   = "60"
    SVR2011_TEST_FRAME_SHOT_EVERY = "$($ShotEvery * 60)"
}
if ($UserData) { $set.SVR2011_USER_DATA = $UserData }
foreach ($k in $set.Keys) { if ($set[$k]) { Set-Item "Env:$k" $set[$k] } }
"replay: $($wrestlers.name -join ' vs ') - rule $($m.rule.name), arena $($m.arena.name); $($m.frames) frames, seed $seed"
"  SVR2011_TEST_MATCH=$($set.SVR2011_TEST_MATCH)"
$gameArgs = @("--log_file=`"$log`"", "--audio_mute=true", "--fullscreen=false", "--monitor=2", "--p2p_port=$P2pPort")
$dir = $null
try {
    for ($try = 1; $try -le $Tries -and -not $dir; $try++) {
        Remove-Item $log -ErrorAction SilentlyContinue
        Get-ChildItem $shotDir -Filter *.ppm -ErrorAction SilentlyContinue | Remove-Item
        $p = Start-Process (Join-Path $GameDir $Exe) -WorkingDirectory $GameDir -ArgumentList $gameArgs -PassThru -WindowStyle Minimized
        try {
            # (the route to the match, the match until the recording's frames are played - and room for slow loads)
            $limit = [DateTime]::Now.AddSeconds(240 + [int]($m.frames / 30))
            $line = $null; $checked = $false
            while (-not $p.HasExited -and [DateTime]::Now -lt $limit) {
                Start-Sleep 2
                if (-not $checked) {  # (the bell: the same intro as the recording's?)
                    $rec = Select-String -Path $log -Pattern "record: this match in (.*)$" -ErrorAction SilentlyContinue | Select-Object -Last 1
                    $got = if ($rec) { Get-BellMotions (Join-Path $rec.Matches[0].Groups[1].Value.Trim() "trail.txt") }
                    if ($got) {
                        $checked = $true
                        if ($wantBell -and $got -ne $wantBell) {
                            "  try ${try}: the bell found $got, the recording's $wantBell (the pre-bell intro) - again"
                            break
                        }
                    }
                }
                $line = Select-String -Path $log -Pattern "replay: .* - replay\.txt in (.*)$" -ErrorAction SilentlyContinue |
                    Select-Object -Last 1
                if ($line) { break }
            }
            if ($line) { $dir = $line.Matches[0].Groups[1].Value.Trim(); Start-Sleep 2 }  # (the last frame shots)
            elseif (-not $checked -or $got -eq $wantBell -or -not $wantBell) {
                throw "no replay report (game $(if ($p.HasExited) { 'exited' } else { 'timed out' })); log: $log"
            }
        } finally {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            Wait-Process -Id $p.Id -Timeout 15 -ErrorAction SilentlyContinue
        }
    }
} finally {
    foreach ($k in $set.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
}
if (-not $dir) { throw "no try's bell matched the recording's ($wantBell) in $Tries tries" }

# The frame shots (binary PPM, P6) as PNGs into the replay's folder.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.IO; using System.Runtime.InteropServices;
public static class SvrPpm {
  public static void ToPng(string ppm, string png) {
    byte[] b = File.ReadAllBytes(ppm);
    int i = 0; string[] f = new string[4];
    for (int k = 0; k < 4;) {  // (P6, width, height, max: whitespace apart, # comments)
      while (char.IsWhiteSpace((char)b[i])) i++;
      if (b[i] == '#') { while (b[i] != 10) i++; continue; }
      int s = i; while (!char.IsWhiteSpace((char)b[i])) i++;
      f[k++] = System.Text.Encoding.ASCII.GetString(b, s, i - s);
    }
    i++;
    int w = int.Parse(f[1]), h = int.Parse(f[2]);
    using (var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb)) {
      var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, bmp.PixelFormat);
      byte[] row = new byte[d.Stride];
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {  // (RGB -> BGR)
          int o = i + (y * w + x) * 3;
          row[x * 3] = b[o + 2]; row[x * 3 + 1] = b[o + 1]; row[x * 3 + 2] = b[o];
        }
        Marshal.Copy(row, 0, new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), d.Stride);
      }
      bmp.UnlockBits(d);
      bmp.Save(png, ImageFormat.Png);
    }
  }
}
"@
$n = 0
foreach ($f in Get-ChildItem $shotDir -Filter *.ppm | Sort-Object Name) {
    [SvrPpm]::ToPng($f.FullName, (Join-Path $dir ("shot_{0:D2}.png" -f $n))); $n++
}
Remove-Item $shotDir, $inputFile -Recurse -ErrorAction SilentlyContinue
"replay folder: $dir ($n frame shots)"
Get-Content (Join-Path $dir "replay.txt") -TotalCount 12
