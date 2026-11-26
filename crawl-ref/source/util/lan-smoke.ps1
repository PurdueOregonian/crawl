# Real two-copy LAN test (Tiles build): starts a host and a client on this
# computer, plays player 1 from a script on the host and player 2 from a
# script on the client, and saves what each window shows.
#
#   pwsh util/lan-smoke.ps1 [-TimeoutSec 90] [-Port 31555] [-Out dir]
#
# Host: crawl -wizard -rc <rc> -lan-host <port>, with util/lan-smoke-host.lua.rc
#       (party of 2, player 2 a copy) logging "SMOKE ..." lines to stderr.
# Client: crawl -lan-join 127.0.0.1:<port> -lan-script <keys> -lan-dump <png>
# Screenshots: <Out>/client.png (what the client was sent),
# <Out>/client-window.png (the client's window) and <Out>/host.png (the
# host's window).

param([int]$TimeoutSec = 90, [int]$Port = 31555, [string]$Out = "",
      [string]$ClientKeys = "ss",
      # Player 2 picks their own character on the client (ClientKeys then
      # starts with the menu keys).
      [switch]$Choose)

$src = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $src "crawl.exe"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("crawl-lan-smoke-" + [guid]::NewGuid())
if (-not $Out) { $Out = $work }
New-Item -ItemType Directory -Force "$work\saves", "$work\morgue", $Out | Out-Null
$hostLog = "$work\host.log"
$clientLog = "$work\client.log"
$rc = "$work\host.rc"

$header = @(
    "name = LanSmoke",
    "species = Minotaur",
    "background = Gladiator",
    "weapon = flail",
    "party_size = 2",
    "party_choose_characters = $(if ($Choose) { 'true' } else { 'false' })",
    "show_more = false",
    "restart_after_game = false",
    "tile_full_screen = false",
    "tile_skip_title = true",
    "tile_window_width = 1024",
    "tile_window_height = 700",
    "save_dir = $($work -replace '\\','/')/saves",
    "morgue_dir = $($work -replace '\\','/')/morgue",
    ""
)
Set-Content $rc ($header + (Get-Content (Join-Path $PSScriptRoot "lan-smoke-host.lua.rc")))

# Tiles can't redirect stderr on Windows; crawl.stderr also writes here.
$env:CRAWL_STDERR_FILE = $hostLog
$h = Start-Process cmd.exe -ArgumentList "/c", "`"`"$exe`" -wizard -rc `"$rc`" -lan-host $Port`"" `
        -WorkingDirectory $src -WindowStyle Minimized -PassThru
$env:CRAWL_STDERR_FILE = $null
$c = Start-Process cmd.exe -ArgumentList "/c", "`"`"$exe`" -lan-join 127.0.0.1:$Port -lan-script $ClientKeys -lan-dump `"$Out\client.bmp`" > `"$clientLog`" 2>&1`"" `
        -WorkingDirectory $src -WindowStyle Minimized -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path $hostLog) -and
        (Select-String -Path $hostLog -Pattern "SMOKE DONE|SMOKE FAIL|We crashed|GAVE UP" -Quiet)) { break }
}
Start-Sleep -Seconds 2   # let the last frames arrive and be saved

# Screenshot of the host's window.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
function Save-Window($launcher, $png) {
    $crawl = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($launcher.Id)" | Where-Object Name -eq "crawl.exe"
    if (-not $crawl) { return }
    $p = Get-Process -Id $crawl.ProcessId -ErrorAction SilentlyContinue
    if (-not $p -or $p.MainWindowHandle -eq 0) { return }
    $r = New-Object Win+RECT
    [Win]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    [Win]::PrintWindow($p.MainWindowHandle, $dc, 2) | Out-Null   # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}
Save-Window $h "$Out\host.png"
# What the client's window actually shows (client.png is what it was sent).
Save-Window $c "$Out\client-window.png"

if (Test-Path "$Out\client.bmp") {
    $img = [System.Drawing.Image]::FromFile("$Out\client.bmp")
    $img.Save("$Out\client.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $img.Dispose()
}

foreach ($p in @($h, $c)) {
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (-not $p.HasExited) { $p.Kill() }
}
Start-Sleep -Milliseconds 500

$hostLines = if (Test-Path $hostLog) { Get-Content $hostLog | Where-Object { $_ -ne "" } } else { @() }
"=== host log ==="
$hostLines | Select-Object -Last 40
"=== client log ==="
if (Test-Path $clientLog) { Get-Content $clientLog }
"screenshots: $Out"
if ($Out -ne $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }

if ($hostLines -match "We crashed") { "RESULT: CRASH"; exit 2 }
if ($hostLines -match "SMOKE FAIL")  { "RESULT: FAIL"; exit 1 }
if ($hostLines -match "SMOKE PASS")  { "RESULT: PASS"; exit 0 }
"RESULT: TIMEOUT"; exit 2
