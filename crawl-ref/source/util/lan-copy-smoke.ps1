# LAN copies-of-the-save test (Tiles build): three "computers" (copies of
# Crawl on this one, each with its own save directory), A, B and C.
# Each player has their own name (A "LanCopy", B "Bee", C "Cee").
#   run 1: A hosts (-lan-host <port>), B and C join; a round, then A saves.
#          B and C must each be sent a copy of the save, as their player:
#          saved under their own name (so their character has it, and the
#          copy has them as its main player).
#   run 2: C loads its copy: it must host by itself (port 31337) as its own
#          player. B joins first, then A: each must get their own player back
#          (by joining order alone, B would get player 1), and the round must
#          complete with everyone playing from their own computer.
#   run 3: A loads its save and B joins; A deletes the game from the waiting
#          screen (D, D). A's save and B's copy must both be gone.
#
#   pwsh util/lan-copy-smoke.ps1 [-TimeoutSec 150] [-Port 31557]
#
# Host scripts: util/lan-copy-smoke-1.lua.rc and -2.lua.rc. Nothing else may
# be using port 31337 (e.g. your own hosted game).

param([int]$TimeoutSec = 150, [int]$Port = 31557)

$src = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $src "crawl.exe"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("crawl-lan-copy-" + [guid]::NewGuid())
$computers = "A", "B", "C"
foreach ($c in $computers) { New-Item -ItemType Directory -Force "$work\$c\saves", "$work\$c\morgue" | Out-Null }
$names = @{ A = "LanCopy"; B = "Bee"; C = "Cee" }
function Save-Of($c) { "$work\$c\saves\$($names[$c]).cs" }
$common = @(
    "show_more = false",
    "restart_after_game = false",
    "tile_full_screen = false",
    "tile_skip_title = true",
    "tile_window_width = 1024",
    "tile_window_height = 700"
)

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CopySmokeWin {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
"@

function Dir-Of($c) { "$($work.Replace('\','/'))/$c" }

# A copy of Crawl on computer $c: a client (joining $join), or the host
# (loading or making the game "LanCopy", with the given Lua script).
function Start-Crawl($c, $log, [string]$join = "", [string]$script = "", [int]$me = 0,
                     [string[]]$extra = @()) {
    $rc = "$work\$c-$([guid]::NewGuid()).rc"
    $lines = @("name = $($names[$c])", "save_dir = $(Dir-Of $c)/saves",
               "morgue_dir = $(Dir-Of $c)/morgue") + $common
    if ($script) {
        $lines = @("species = Minotaur", "background = Gladiator",
                   "weapon = flail", "party_choose_characters = false") + $lines +
                 @("<", "smoke_me = $me", ">") +
                 (Get-Content (Join-Path $PSScriptRoot $script))
    }
    Set-Content $rc $lines
    $argv = @("-rc", "`"$rc`"")
    if ($join) { $argv += @("-lan-join", $join, "-lan-script", "ssssssss") } else { $argv += "-wizard" }
    $env:CRAWL_STDERR_FILE = $log
    $p = Start-Process $exe -ArgumentList ($argv + $extra) -WorkingDirectory $src `
            -WindowStyle Minimized -PassThru
    $env:CRAWL_STDERR_FILE = $null
    $p
}

function Stop-Tree($p) {
    if (-not $p) { return }
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
}

function Read-Log($log, $prefix) {
    if (Test-Path $log) { Get-Content $log | Where-Object { $_ -ne "" } | ForEach-Object { "$prefix$_" } }
}

# Wait until $test is true (or the host exits, or time runs out).
function Wait-For($hostProc, [scriptblock]$test) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline -and -not $hostProc.HasExited) {
        if (& $test) { return $true }
        Start-Sleep -Milliseconds 500
    }
    & $test
}

function Has($log, $pattern) { (Test-Path $log) -and (Select-String -Path $log -Pattern $pattern -Quiet) }

function Connections([string]$state) {
    @(Get-NetTCPConnection -LocalPort 31337 -State $state -ErrorAction SilentlyContinue).Count
}

function Stop-Test($result, $code) {
    if ($code) { "files: $work" } else { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
    "RESULT: $result"; exit $code
}

if ((Connections Listen) -gt 0) { Stop-Test "SKIPPED (port 31337 is in use)" 2 }

# ---- run 1: A hosts, B and C join, A saves.
$env:CRAWL_LAN_AUTOSTART = "3"
$a = Start-Crawl A "$work\A1.log" -script "lan-copy-smoke-1.lua.rc" -me 1 -extra @("-lan-host", "$Port")
$env:CRAWL_LAN_AUTOSTART = $null
$b = Start-Crawl B "$work\B1.log" -join "127.0.0.1:$Port"
$c = Start-Crawl C "$work\C1.log" -join "127.0.0.1:$Port"
$null = Wait-For $a { (Has "$work\A1.log" "SMOKE FAIL|GAVE UP|We crashed") -or
                      ((Has "$work\B1.log" "copy of the game now") -and (Has "$work\C1.log" "copy of the game now")) }
Start-Sleep -Seconds 1
Stop-Tree $a; Stop-Tree $b; Stop-Tree $c
"=== run 1 (A hosts, B and C join, A saves) ==="
Read-Log "$work\A1.log" "A: "; Read-Log "$work\B1.log" "B: "; Read-Log "$work\C1.log" "C: "
$one = @(Read-Log "$work\A1.log" "") + @(Read-Log "$work\B1.log" "") + @(Read-Log "$work\C1.log" "")
if ($one -match "We crashed") { Stop-Test "CRASH" 2 }
if ($one -match "SMOKE FAIL|GAVE UP") { Stop-Test "FAIL" 1 }
$before = ($one | Select-String "^SMOKE BEFORE (.*)$" | Select-Object -First 1)
if (-not $before) { Stop-Test "TIMEOUT (run 1)" 2 }
$bWas = [int](Select-String -Path "$work\B1.log" -Pattern "playing as Player (\d)" | Select-Object -First 1).Matches[0].Groups[1].Value
$cWas = [int](Select-String -Path "$work\C1.log" -Pattern "playing as Player (\d)" | Select-Object -First 1).Matches[0].Groups[1].Value
foreach ($x in "B", "C") {
    if (-not (Test-Path (Save-Of $x))) { Stop-Test "FAIL ($x got no copy of the save under its own name)" 1 }
}
"(B was player $bWas, C was player $cWas)"

# ---- run 2: C hosts from its copy; B joins, then A.
$c = Start-Crawl C "$work\C2.log" -script "lan-copy-smoke-2.lua.rc" -me $cWas
$listening = Wait-For $c { (Connections Listen) -gt 0 }
$b = Start-Crawl B "$work\B2.log" -join "127.0.0.1"
$bIn = Wait-For $c { (Connections Established) -ge 1 }
$a = Start-Crawl A "$work\A2.log" -join "127.0.0.1"
$null = Wait-For $c { Has "$work\C2.log" "SMOKE DONE|SMOKE FAIL|GAVE UP|We crashed" }
Stop-Tree $c; Start-Sleep -Seconds 1; Stop-Tree $a; Stop-Tree $b
"=== run 2 (C hosts from its copy; B joins, then A) ==="
"(C hosting: $listening, B in before A: $bIn)"
Read-Log "$work\C2.log" "C: "; Read-Log "$work\B2.log" "B: "; Read-Log "$work\A2.log" "A: "
$two = @(Read-Log "$work\C2.log" "")
if ($two -match "We crashed") { Stop-Test "CRASH" 2 }
if ($two -match "SMOKE FAIL|GAVE UP") { Stop-Test "FAIL" 1 }
$after = ($two | Select-String "^SMOKE AFTER (.*)$" | Select-Object -First 1)
if (-not $after -or -not ($two -match "SMOKE PASS")) { Stop-Test "TIMEOUT (run 2)" 2 }
if ($before.Matches[0].Groups[1].Value -ne $after.Matches[0].Groups[1].Value) {
    Stop-Test "FAIL (C's copy differs from the saved game)" 1
}
if (-not (Has "$work\A2.log" "playing as Player 1$")) { Stop-Test "FAIL (A didn't get player 1 back)" 1 }
if (-not (Has "$work\B2.log" "playing as Player $bWas$")) { Stop-Test "FAIL (B didn't get player $bWas back)" 1 }

# ---- run 3: A loads its save, B joins, A deletes the game.
$a = Start-Crawl A "$work\A3.log" -script "lan-copy-smoke-2.lua.rc" -me 1
$null = Wait-For $a { (Connections Listen) -gt 0 }
$b = Start-Crawl B "$work\B3.log" -join "127.0.0.1"
$null = Wait-For $a { (Connections Established) -ge 1 }
Start-Sleep -Seconds 2
$a.Refresh()
$hw = $a.MainWindowHandle
foreach ($i in 1, 2) {
    # D: WM_KEYDOWN / WM_KEYUP, with the scancode (SDL needs it).
    [CopySmokeWin]::PostMessage($hw, 0x100, [IntPtr]0x44, [IntPtr](0x20 -shl 16)) | Out-Null
    [CopySmokeWin]::PostMessage($hw, 0x101, [IntPtr]0x44, [IntPtr]((0x20 -shl 16) -bor 0xC0000001)) | Out-Null
    Start-Sleep -Milliseconds 500
}
$null = Wait-For $a { (Has "$work\B3.log" "deleted") -and -not (Test-Path (Save-Of A)) }
Start-Sleep -Seconds 1
Stop-Tree $a; Stop-Tree $b
"=== run 3 (A loads, B joins, A deletes the game) ==="
Read-Log "$work\A3.log" "A: "; Read-Log "$work\B3.log" "B: "
if (Test-Path (Save-Of A)) { Stop-Test "FAIL (A's save wasn't deleted)" 1 }
if (Test-Path (Save-Of B)) { Stop-Test "FAIL (B's copy wasn't deleted)" 1 }
if (-not (Test-Path (Save-Of C))) { Stop-Test "FAIL (C's copy went too, though C wasn't there)" 1 }
Stop-Test "PASS" 0
