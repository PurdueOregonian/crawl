# Run crawl's headless Lua tests on Windows (Console debug build).
#
# The Windows console build has no headless mode, so crawl needs a real
# console: this launches it in its own minimized console window and
# captures output to a log. Crawl prints nothing on success; on failure
# it prints "N tests, X succeeded, Y failed" plus the errors.
#
# Usage (from crawl-ref/source):
#   pwsh util/run-tests.ps1                       # all tests except test/big/
#   pwsh util/run-tests.ps1 party_baseline        # selected tests
#   pwsh util/run-tests.ps1 party_baseline,bounce -TimeoutSec 300
#
# Exit code: 0 = all passed, 1 = failures, 2 = timeout / crashed.

param(
    [string]$Tests = "",
    [int]$TimeoutSec = 600
)

$src = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $src "crawl.exe"
if (-not (Test-Path $exe)) { Write-Error "crawl.exe not found in $src (build Debug Console first)"; exit 2 }

$log = Join-Path ([System.IO.Path]::GetTempPath()) ("crawl-test-{0}.log" -f [guid]::NewGuid())
$crawlArgs = @("-test")
if ($Tests) { $crawlArgs += $Tests }

# cmd /c gives crawl a console of its own; stdout+stderr go to the log.
# The whole command is wrapped in an extra pair of quotes because cmd /c
# strips the first and last quote when the line starts with one.
$cmdline = "`"$exe`" $($crawlArgs -join ' ') > `"$log`" 2>&1"
$p = Start-Process -FilePath cmd.exe -ArgumentList "/c", "`"$cmdline`"" `
        -WorkingDirectory $src -WindowStyle Minimized -PassThru

if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id)" |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    $p.Kill()
    if (Test-Path $log) { Get-Content $log }
    Write-Host "TIMEOUT after $TimeoutSec s"
    exit 2
}

$out = if (Test-Path $log) { Get-Content $log } else { @() }
$out
Remove-Item $log -ErrorAction SilentlyContinue

if ($out -match "We crashed!") { Write-Host "RESULT: CRASH"; exit 2 }
if ($out -match "\d+ tests?, \d+ succeeded, [1-9]\d* failed") { Write-Host "RESULT: FAIL"; exit 1 }
if ($p.ExitCode -ne 0) { Write-Host "RESULT: FAIL (exit $($p.ExitCode))"; exit 1 }
Write-Host "RESULT: PASS"
exit 0
