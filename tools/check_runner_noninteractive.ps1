# check_runner_noninteractive.ps1 -- run_all_checks.ps1 must launch every check child with
# -NonInteractive and stdin from a file at EOF, so a prompt (Read-Host, Remove-Item "Confirm?")
# fails fast instead of hanging a suite run. Part 1 asserts the launch line in the runner source;
# part 2 launches a Read-Host fixture exactly the way the runner does and requires a quick non-zero exit.
# checkcache: ok
$ErrorActionPreference = "Stop"
$runner = Join-Path $PSScriptRoot "run_all_checks.ps1"
$src = Get-Content -LiteralPath $runner -Raw
$fails = @()
if ($src -notmatch '"-NoProfile",\s*"-NonInteractive",\s*"-ExecutionPolicy",\s*"Bypass",\s*"-File"') { $fails += "runner does not launch .ps1 checks with -NoProfile -NonInteractive -ExecutionPolicy Bypass -File" }
if ($src -notmatch '-RedirectStandardInput \$inFile') { $fails += "runner does not redirect child stdin (-RedirectStandardInput)" }
foreach ($p in 1800, 900) { if ($src -notmatch "-PerCheckTimeoutSec $p ") { $fails += "runner phase lacks -PerCheckTimeoutSec $p" } }
if ($src -notmatch 'check timed out') { $fails += "runner timeout FAIL text lacks 'timed out'" }

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("noninter_" + $PID)
New-Item -ItemType Directory -Path $tmp -Force | Out-Null
try {
    $fx = Join-Path $tmp "fixture_prompt.ps1"
    Set-Content -LiteralPath $fx -Value '$ErrorActionPreference = "Stop"; Read-Host "never answered"; exit 0' -Encoding ascii
    $in = Join-Path $tmp "in.txt"; [IO.File]::WriteAllText($in, "")
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath powershell -ArgumentList @("-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", $fx) `
        -RedirectStandardInput $in -RedirectStandardOutput (Join-Path $tmp "o.txt") -RedirectStandardError (Join-Path $tmp "e.txt") -PassThru -NoNewWindow
    $null = $proc.Handle
    if (-not $proc.WaitForExit(60000)) {
        & taskkill /PID $proc.Id /T /F | Out-Null
        $fails += "Read-Host fixture still running after 60s (would hang a suite run)"
    } elseif ($proc.ExitCode -eq 0) {
        $fails += "Read-Host fixture exited 0 (prompt was answered/ignored)"
    } else {
        Write-Host "fixture failed fast: exit $($proc.ExitCode) in $([int]$sw.Elapsed.TotalSeconds)s"
    }
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($fails.Count) { $fails | ForEach-Object { Write-Host "FAIL: $_" }; exit 1 }
Write-Host "PASS: runner launches checks non-interactively with stdin at EOF and per-phase timeouts"
exit 0
