# End-to-end smoke test of hotseat party play in a REAL game (Windows,
# Debug Console build). Starts a new character with party_size = 2 in wizard
# mode and lets util/party-smoke.lua.rc play: both players wait a turn, then
# player 1 takes stairs and the script checks the party arrived together.
#
# Real stairs need a real save file, which the headless -test harness lacks,
# so this is the check for level changes.
#
# Usage (from crawl-ref/source):  pwsh util/party-smoke.ps1 [-TimeoutSec 60]
# Exit code: 0 = PASS, 1 = FAIL, 2 = crash/timeout. Prints the script's log
# and the final screen (read with util/peek-console.ps1).

param([int]$TimeoutSec = 60)

$src = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $src "crawl.exe"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("crawl-party-smoke-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force "$work\saves", "$work\morgue" | Out-Null
$log = "$work\smoke.log"
$rc = "$work\smoke.rc"

$header = @(
    "name = PartySmoke",
    "species = Minotaur",
    "background = Gladiator",
    "weapon = flail",
    "party_size = 2",
    "show_more = false",
    "restart_after_game = false",
    "save_dir = $($work -replace '\\','/')/saves",
    "morgue_dir = $($work -replace '\\','/')/morgue",
    ""
)
Set-Content $rc ($header + (Get-Content (Join-Path $PSScriptRoot "party-smoke.lua.rc")))

# cmd /c gives crawl its own console; the script logs via crawl.stderr.
$cmdline = "`"`"$exe`" -wizard -rc `"$rc`" > `"$log`" 2>&1`""
$p = Start-Process cmd.exe -ArgumentList "/c", $cmdline -WorkingDirectory $src `
        -WindowStyle Minimized -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path $log) -and
        (Select-String -Path $log -Pattern "SMOKE DONE|We crashed|GAVE UP" -Quiet)) { break }
}

$crawl = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
         Where-Object Name -eq "crawl.exe"
$out = if (Test-Path $log) { Get-Content $log | Where-Object { $_ -ne "" } } else { @() }
"=== script log ==="
$out
if ($crawl) {
    "=== final screen ==="
    pwsh -NoProfile -File (Join-Path $PSScriptRoot "peek-console.ps1") -ProcessId $crawl.ProcessId |
        Where-Object { $_ -ne "" }
    Stop-Process -Id $crawl.ProcessId -Force -ErrorAction SilentlyContinue
}
if (-not $p.HasExited) { $p.Kill() }
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue

if ($out -match "We crashed") { "RESULT: CRASH"; exit 2 }
if ($out -match "SMOKE PASS")  { "RESULT: PASS"; exit 0 }
if ($out -match "SMOKE FAIL")  { "RESULT: FAIL"; exit 1 }
"RESULT: TIMEOUT"; exit 2
