# End-to-end smoke tests of hotseat party play in a REAL game (Windows,
# Debug Console build). Starts a new character with party_size = 2 in wizard
# mode and lets util/party-smoke-<scenario>.lua.rc play:
#   stairs  - both players wait a turn, then player 1 takes stairs and the
#             script checks the party arrived together.
#   explore - group autoexplore: player 1 explores, player 2 must follow
#             without being prompted, stay close, and never swap places.
#   rest    - group rest: player 2 rests along until everyone is healed;
#             then an enemy near the inactive player must block resting
#             without crashing.
#
# Real stairs need a real save file, which the headless -test harness lacks,
# so this is the check for level changes.
#
# Usage (from crawl-ref/source):
#   pwsh util/party-smoke.ps1 [-Scenario stairs|explore|rest|all] [-TimeoutSec 60]
# Exit code: 0 = PASS, 1 = FAIL, 2 = crash/timeout. Prints the script's log
# and the final screen (read with util/peek-console.ps1).

param([string]$Scenario = "all", [int]$TimeoutSec = 60)

if ($Scenario -eq "all") {
    $worst = 0
    foreach ($s in "stairs", "explore", "rest") {
        "##### scenario: $s"
        & pwsh -NoProfile -File $PSCommandPath -Scenario $s -TimeoutSec $TimeoutSec
        $worst = [Math]::Max($worst, $LASTEXITCODE)
    }
    exit $worst
}

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
Set-Content $rc ($header + (Get-Content (Join-Path $PSScriptRoot "party-smoke-$Scenario.lua.rc")))

# cmd /c gives crawl its own console; the script logs via crawl.stderr.
$cmdline = "`"`"$exe`" -wizard -rc `"$rc`" > `"$log`" 2>&1`""
$p = Start-Process cmd.exe -ArgumentList "/c", $cmdline -WorkingDirectory $src `
        -WindowStyle Minimized -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    if ((Test-Path $log) -and
        (Select-String -Path $log -Pattern "SMOKE DONE|SMOKE FAIL|We crashed|GAVE UP" -Quiet)) { break }
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
