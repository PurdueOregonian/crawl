# Party save/load test in a real game (Tiles build): run 1 makes a party of
# two different characters and saves on player 2's turn; run 2 loads that
# save and must find the same party (species, HP, XL, positions, whose turn),
# then play on.
#
#   pwsh util/party-save-smoke.ps1 [-TimeoutSec 60]
#
# Scripts: util/party-save-smoke-1.lua.rc and -2.lua.rc, logging "SMOKE ..."
# lines (Tiles can't redirect stderr; crawl.stderr also writes to
# $CRAWL_STDERR_FILE).

param([int]$TimeoutSec = 60)

$src = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $src "crawl.exe"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("crawl-party-save-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force "$work\saves", "$work\morgue" | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class SaveSmokeWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
# What the game's window shows (for when a run gets stuck).
function Save-Window($proc, $png) {
    $proc.Refresh()
    if ($proc.HasExited) { return }
    $hw = $proc.MainWindowHandle
    if ($hw -eq 0) { return }
    $r = New-Object SaveSmokeWin+RECT
    [SaveSmokeWin]::GetWindowRect($hw, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [SaveSmokeWin]::PrintWindow($hw, $dc, 2) | Out-Null
    $g.ReleaseHdc($dc)
    try { $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) } catch {}
    $g.Dispose(); $bmp.Dispose()
}

function Invoke-Run([int]$run) {
    $rc = "$work\run$run.rc"
    $log = "$work\run$run.log"
    $header = @(
        "name = SaveSmoke",
        "species = Minotaur",
        "background = Gladiator",
        "weapon = flail",
        "party_size = 1",
        "show_more = false",
        "restart_after_game = false",
        "tile_full_screen = false",
        "tile_skip_title = true",
        "tile_window_width = 1024",
        "tile_window_height = 700",
        "save_dir = $($work.Replace('\','/'))/saves",
        "morgue_dir = $($work.Replace('\','/'))/morgue",
        ""
    )
    Set-Content $rc ($header + (Get-Content (Join-Path $PSScriptRoot "party-save-smoke-$run.lua.rc")))
    $env:CRAWL_STDERR_FILE = $log
    $p = Start-Process $exe -ArgumentList "-wizard", "-rc", "`"$rc`"" `
            -WorkingDirectory $src -WindowStyle Minimized -PassThru
    $env:CRAWL_STDERR_FILE = $null
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $log) -and
            (Select-String -Path $log -Pattern "SMOKE DONE|SMOKE FAIL|We crashed|GAVE UP" -Quiet)) { break }
    }
    if (-not $p.HasExited) {
        Start-Sleep -Milliseconds 500
        Save-Window $p "$work
un$run.png"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $log) { Get-Content $log | Where-Object { $_ -ne "" } } else { @() }
}

$one = @(Invoke-Run 1)
"=== run 1 (new game, save) ==="
$one
$two = @(Invoke-Run 2)
"=== run 2 (load) ==="
$two

function Stop-Test($result, $code) {
    # Keep the saves, logs and screenshots of a failed run to look at.
    if ($code) { "files: $work" } else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
    "RESULT: $result"; exit $code
}
if (($one + $two) -match "We crashed") { Stop-Test "CRASH" 2 }
if (($one + $two) -match "SMOKE FAIL|GAVE UP") { Stop-Test "FAIL" 1 }
$before = ($one | Select-String "^SMOKE BEFORE (.*)$" | Select-Object -First 1)
$after = ($two | Select-String "^SMOKE AFTER (.*)$" | Select-Object -First 1)
if (-not $before -or -not $after -or -not ($two -match "SMOKE ROUND OK")) { Stop-Test "TIMEOUT" 2 }
if ($before.Matches[0].Groups[1].Value -ne $after.Matches[0].Groups[1].Value) {
    Stop-Test "FAIL (the loaded party differs from the saved one)" 1
}
Stop-Test "PASS" 0
