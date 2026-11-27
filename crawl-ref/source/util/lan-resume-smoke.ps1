# LAN save/load test (Tiles build), two copies of Crawl on this computer:
#   run 1: host (-lan-host <port>) and client play a round, the host saves
#          and goes back to its start menu: the client must be told so
#          (and so go back to its own start menu), not be left waiting.
#   run 2: the host loads the save with no LAN option at all: because player
#          2 was on another computer, it must host by itself (default port
#          31337) and wait; a new client joins, gets player 2 back, and the
#          round only completes if player 2 is played from the client.
#
#   pwsh util/lan-resume-smoke.ps1 [-TimeoutSec 90] [-Port 31556]
#
# Host scripts: util/lan-resume-smoke-1.lua.rc and -2.lua.rc. Nothing else
# may be using port 31337 (e.g. your own hosted game).

param([int]$TimeoutSec = 90, [int]$Port = 31556)

$src = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $src "crawl.exe"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("crawl-lan-resume-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force "$work\saves", "$work\morgue" | Out-Null
$common = @(
    "show_more = false",
    "restart_after_game = false",
    "tile_full_screen = false",
    "tile_skip_title = true",
    "tile_window_width = 1024",
    "tile_window_height = 700"
)
$clientRc = "$work\client.rc"
Set-Content $clientRc ($common + @(
    "save_dir = $($work.Replace('\','/'))/client-saves",
    "morgue_dir = $($work.Replace('\','/'))/client-morgue"))

function Stop-Tree($p) {
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}

function Invoke-Run([int]$run, [string]$hostArgs, [string]$joinAt) {
    $rc = "$work\host$run.rc"
    $log = "$work\host$run.log"
    Set-Content $rc (@(
        "name = LanResume",
        "species = Minotaur",
        "background = Gladiator",
        "weapon = flail",
        "party_choose_characters = false",
        "save_dir = $($work.Replace('\','/'))/saves",
        "morgue_dir = $($work.Replace('\','/'))/morgue") + $common +
        # Run 1: back to the start menu after saving, as in real play.
        @("restart_after_game = $(if ($run -eq 1) { 'true' } else { 'false' })") +
        (Get-Content (Join-Path $PSScriptRoot "lan-resume-smoke-$run.lua.rc")))
    $env:CRAWL_STDERR_FILE = $log
    $env:CRAWL_LAN_AUTOSTART = "2"
    $h = Start-Process $exe -ArgumentList (@("-wizard", "-rc", "`"$rc`"") + $hostArgs.Split(" ", [StringSplitOptions]::RemoveEmptyEntries)) `
            -WorkingDirectory $src -WindowStyle Minimized -PassThru
    $env:CRAWL_LAN_AUTOSTART = $null
    $clog = "$work\client$run.log"
    $env:CRAWL_STDERR_FILE = $clog
    $c = Start-Process $exe -ArgumentList "-rc", "`"$clientRc`"", "-lan-join", $joinAt, "-lan-script", "s" `
            -WorkingDirectory $src -WindowStyle Minimized -PassThru
    $env:CRAWL_STDERR_FILE = $null
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline -and -not $h.HasExited) {
        Start-Sleep -Milliseconds 500
        if ((Test-Path $log) -and
            (Select-String -Path $log -Pattern "SMOKE DONE|SMOKE FAIL|We crashed|GAVE UP" -Quiet)) { break }
        # Run 1: the host has saved and told the client.
        if ($run -eq 1 -and (Test-Path $clog) -and
            (Select-String -Path $clog -Pattern "LAN client:" -Quiet)) { break }
    }
    Start-Sleep -Milliseconds 500
    $c.Refresh()
    $clientUp = -not $c.HasExited
    Stop-Tree $h
    Start-Sleep -Seconds 1
    Stop-Tree $c
    if (Test-Path $log) { Get-Content $log | Where-Object { $_ -ne "" } }
    if (Test-Path $clog) { Get-Content $clog | Where-Object { $_ -match "^LAN client:" } }
    if ($clientUp) { "CLIENT STILL RUNNING" }
}

$one = @(Invoke-Run 1 "-lan-host $Port" "127.0.0.1:$Port")
"=== run 1 (host + client, save) ==="
$one
$two = @(Invoke-Run 2 "" "127.0.0.1")
"=== run 2 (load: hosts again by itself) ==="
$two

function Stop-Test($result, $code) {
    if ($code) { "files: $work" } else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
    "RESULT: $result"; exit $code
}
if (($one + $two) -match "We crashed") { Stop-Test "CRASH" 2 }
if (($one + $two) -match "SMOKE FAIL|GAVE UP") { Stop-Test "FAIL" 1 }
if (-not ($one -match "^LAN client: The host saved the game") -or
    -not ($one -match "CLIENT STILL RUNNING")) {
    Stop-Test "FAIL (the client wasn't sent back to its menu when the host saved)" 1
}
$before = ($one | Select-String "^SMOKE BEFORE (.*)$" | Select-Object -First 1)
$after = ($two | Select-String "^SMOKE AFTER (.*)$" | Select-Object -First 1)
if (-not $before -or -not $after -or -not ($two -match "SMOKE PASS")) { Stop-Test "TIMEOUT" 2 }
if ($before.Matches[0].Groups[1].Value -ne $after.Matches[0].Groups[1].Value) {
    Stop-Test "FAIL (the loaded party differs from the saved one)" 1
}
Stop-Test "PASS" 0
