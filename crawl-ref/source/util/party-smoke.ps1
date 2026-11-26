# End-to-end smoke tests of hotseat party play in a REAL game (Windows,
# Debug Console build). Starts a new character with party_size = 2 in wizard
# mode and lets util/party-smoke-<scenario>.lua.rc play:
#   stairs  - both players wait a turn, then player 1 takes stairs and the
#             script checks the party arrived together.
#   travel  - Ctrl+G to the next floor: player 2 follows player 1 to the
#             stairs without being prompted and the party arrives together.
#   explore - group autoexplore: player 1 explores, player 2 must follow
#             without being prompted, stay close, and never swap places.
#   rest    - group rest: player 2 rests along until everyone is healed;
#             then an enemy near the inactive player must block resting
#             without crashing.
#   talisman - player 1 transforms with a talisman; player 2 must not.
#   autofight - Tab in a hallway with a teammate in front: no swap, just
#             "No reachable target".
#   corpse  - a goblin kills player 1; a monster on the body must still be
#             drawn (check the final screen).
#   characters - player 2 picks their own character from the new-game menus
#             (the runner types the keys from the scenario's MENU-KEYS line).
#
# Real stairs need a real save file, which the headless -test harness lacks,
# so this is the check for level changes.
#
# Usage (from crawl-ref/source):
#   pwsh util/party-smoke.ps1 [-Scenario stairs|travel|explore|rest|characters|talisman|autofight|corpse|all] [-TimeoutSec 60] [-Exe path\to\crawl.exe]
# Exit code: 0 = PASS, 1 = FAIL, 2 = crash/timeout. Prints the script's log
# and the final screen (read with util/peek-console.ps1).

# -Exe: a console crawl.exe built elsewhere (when source/crawl.exe is a tiles
# build or locked by a running game); it's pointed at source/ for its data.
param([string]$Scenario = "all", [int]$TimeoutSec = 60, [string]$Exe = "")

if ($Scenario -eq "all") {
    $worst = 0
    foreach ($s in "stairs", "travel", "explore", "rest", "characters", "talisman", "autofight", "corpse") {
        "##### scenario: $s"
        & pwsh -NoProfile -File $PSCommandPath -Scenario $s -TimeoutSec $TimeoutSec -Exe $Exe
        $worst = [Math]::Max($worst, $LASTEXITCODE)
    }
    exit $worst
}

$src = Split-Path -Parent $PSScriptRoot
$exe = if ($Exe) { $Exe } else { Join-Path $src "crawl.exe" }
$dirArg = if ($Exe) { "-dir `"$src`" " } else { "" }
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
# (Not "$scenario": PowerShell names are case-insensitive, so that would be
# the [string] $Scenario parameter and the lines would get joined into one.)
$scenarioLines = @(Get-Content (Join-Path $PSScriptRoot "party-smoke-$Scenario.lua.rc"))
# Keys to type into player 2's character menus, if the scenario wants any.
$menuKeys = ($scenarioLines | Select-String "^# MENU-KEYS: (\S+)\s*$" | Select-Object -First 1)
$menuKeys = if ($menuKeys) { $menuKeys.Matches[0].Groups[1].Value.Trim() } else { "" }
# Without menu keys, nobody can answer player 2's character menus: make the
# extra players copies instead.
if (-not $menuKeys) { $header += "party_choose_characters = false" }
Set-Content $rc ($header + $scenarioLines)

# cmd /c gives crawl its own console; the script logs via crawl.stderr.
$cmdline = "`"`"$exe`" $dirArg-wizard -rc `"$rc`" > `"$log`" 2>&1`""
$p = Start-Process cmd.exe -ArgumentList "/c", $cmdline -WorkingDirectory $src `
        -WindowStyle Minimized -PassThru

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$menuDone = ($menuKeys -eq "")
$lastLen = -1; $lastGrowth = Get-Date; $escapes = 0
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    if (-not $menuDone) {
        $c = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
             Where-Object Name -eq "crawl.exe"
        if ($c) {
            $screen = (pwsh -NoProfile -File (Join-Path $PSScriptRoot "peek-console.ps1") -ProcessId $c.ProcessId) -join "`n"
            if ($screen -match "Please select your species") {
                # One key per menu; give each menu a moment to appear.
                foreach ($ch in $menuKeys.ToCharArray()) {
                    pwsh -NoProfile -File (Join-Path $PSScriptRoot "send-console-keys.ps1") -ProcessId $c.ProcessId -Keys "$ch" | Out-Null
                    Start-Sleep -Milliseconds 1500
                }
                $menuDone = $true
            }
        }
    }
    if ((Test-Path $log) -and
        (Select-String -Path $log -Pattern "SMOKE DONE|SMOKE FAIL|We crashed|GAVE UP" -Quiet)) { break }

    # Random levels sometimes open a menu the script can't answer (e.g.
    # walking onto a shop). If the log stops growing for a while, press Esc.
    # (A genuinely stuck game still times out: Esc won't unstick a loop.)
    if ($menuDone -and (Test-Path $log)) {
        $len = (Get-Item $log).Length
        if ($len -ne $lastLen) { $lastLen = $len; $lastGrowth = Get-Date }
        elseif (((Get-Date) - $lastGrowth).TotalSeconds -gt 20) {
            $c = Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
                 Where-Object Name -eq "crawl.exe"
            if ($c) {
                pwsh -NoProfile -File (Join-Path $PSScriptRoot "send-console-keys.ps1") -ProcessId $c.ProcessId -Keys "{ESC}" | Out-Null
                $escapes++
            }
            $lastGrowth = Get-Date
        }
    }
}
if ($escapes) { "(pressed Esc $escapes time(s) to leave menus the script couldn't answer)" }

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
