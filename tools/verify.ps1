# One command that answers "is the tree good?" -- and one short answer.
#
# Before this, verifying a change meant four separate invocations (KilnFW host
# tests, SaftyFW host tests, run_all_checks, lint_pages),
# each printing hundreds of lines that a human or an agent then had to skim for
# a verdict. That skimming is where this repository has repeatedly gone wrong:
# a build tool reported "OK in 4.8s" over a log containing "ninja: build
# stopped: subcommand failed", and it was believed twice, because reading the
# tail and hoping is not verification.
#
# So the contract here is narrow and blunt:
#   - Exit code is 0 if and only if every stage that ran passed.
#   - Normal output is one line per stage. Nothing else.
#   - On failure, only the decisive lines of that stage are printed, followed
#     by the path to its full log. The full log is always written, pass or
#     fail, so a green run can still be audited afterwards.
#
# The stages are independent and run concurrently as separate processes. On a
# 24-core machine the wall time is the slowest single stage rather than the sum.
#
# WHAT THIS DELIBERATELY DOES NOT DO: the firmware target builds. Those go
# through the MCP `build_kilnfw` / `build_saftyfw` tools, which own the
# toolchain invocation (a plain `idf.py build` fails here on a python-env
# mismatch). Reimplementing the build command in a second place is a second
# thing to drift out of sync, and this repo has been bitten by exactly that
# often enough. Run the build tool separately; run this for everything else.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -Only kilnfw,checks
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -ListOnly
#
# NOTE ON EXIT CODES: PowerShell does not propagate a native command's exit
# code out of a -File script by itself -- that bit us in workbench.py, where a
# failing build surfaced as success. Every stage below reads the process's
# .ExitCode explicitly, and this script ends with an explicit exit.

param(
    # Run only the named stages. Names are the keys in $Stages below.
    [string[]]$Only,

    # Print the plan and run nothing.
    [switch]$ListOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
# Unique per invocation, not a fixed shared path: many parallel sessions run
# this script on this machine, and a fixed "$env:TEMP\kilnctl-verify" path let
# one session's run overwrite another's log out from under it -- a session has
# already read another session's failure log this way. PID + timestamp keeps
# concurrent runs from colliding.
$logDir = Join-Path $env:TEMP "kilnctl-verify-$PID-$(Get-Date -Format 'yyyyMMdd_HHmmss_fff')"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

# Each stage names the command to run and how to recognise its failure lines.
# Grepping for the decisive line is what keeps the output short without hiding
# anything -- the full log path is always printed on failure.
$Stages = [ordered]@{
    "kilnfw"  = @{
        Desc     = "KilnFW host tests"
        Exe      = "powershell"
        Args     = @("-ExecutionPolicy", "Bypass", "-File", "$repoRoot\firmware\KilnFW\App\test\build_host_tests.ps1")
        Decisive = '^\s*FAIL |FAILURE\(S\)|FAILED executables|error:'
        Cwd      = "$repoRoot\firmware\KilnFW\App\test"
    }
    "saftyfw" = @{
        Desc     = "SaftyFW host tests"
        Exe      = "powershell"
        Args     = @("-ExecutionPolicy", "Bypass", "-File", "$repoRoot\firmware\SaftyFW\test\build_host_tests.ps1")
        # Anchored on a leading "FAIL " so a test's NAME cannot match: the
        # first run of this script pointed at a banner line that merely
        # contained the word FAULT and hid the real 15 failures below it.
        Decisive = '^\s*FAIL |FAILURE\(S\)|FAILED executables|error:'
        Cwd      = "$repoRoot\firmware\SaftyFW\test"
    }
    "checks"  = @{
        Desc     = "Guard scripts (run_all_checks)"
        Exe      = "powershell"
        Args     = @("-ExecutionPolicy", "Bypass", "-File", "$repoRoot\tools\run_all_checks.ps1")
        Decisive = 'FAILED|FAIL:|below the floor'
        Cwd      = $repoRoot
    }
    "lint"    = @{
        Desc     = "Web page lint"
        Exe      = "powershell"
        # Delegates to tools/check_lint_pages.ps1, which now also runs as
        # part of tools/run_all_checks.ps1 (globbed by its check_*.ps1 name)
        # -- this keeps exactly ONE invocation of lint_pages.js rather than
        # two divergent ones (this stage used to call node directly).
        Args     = @("-ExecutionPolicy", "Bypass", "-File", "$repoRoot\tools\check_lint_pages.ps1")
        Decisive = 'FAIL|problem|error'
        Cwd      = $repoRoot
    }
}

# -File passes every argument as a plain string, so `-Only checks,lint` arrives
# as the single element "checks,lint" rather than two. Split it ourselves; the
# alternative is a flag that silently matches nothing and reports success.
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

$unknown = @($Only | Where-Object { -not $Stages.Contains($_) })
if ($unknown.Count -gt 0) {
    Write-Host "Unknown stage(s): $($unknown -join ', '). Known: $($Stages.Keys -join ', ')" -ForegroundColor Red
    exit 2
}

$selected = @($Stages.Keys | Where-Object { -not $Only -or $Only -contains $_ })
if ($selected.Count -eq 0) {
    Write-Host "No stages matched -Only. Known stages: $($Stages.Keys -join ', ')" -ForegroundColor Red
    exit 2
}

if ($ListOnly) {
    Write-Host "$($selected.Count) stage(s) would run:"
    foreach ($n in $selected) { Write-Host ("  {0,-10} {1}" -f $n, $Stages[$n].Desc) }
    Write-Host ""
    Write-Host "Firmware target builds are NOT covered here -- use the build_kilnfw / build_saftyfw tools."
    exit 0
}

$results = [ordered]@{}
$procs = @{}
$swAll = [Diagnostics.Stopwatch]::StartNew()

foreach ($name in $selected) {
    $s = $Stages[$name]
    $out = Join-Path $logDir "$name.log"
    $err = Join-Path $logDir "$name.err"
    $proc = Start-Process -FilePath $s.Exe -ArgumentList $s.Args -WorkingDirectory $s.Cwd `
        -RedirectStandardOutput $out -RedirectStandardError $err `
        -NoNewWindow -PassThru
    # Touch .Handle before the process exits. Without this the OS handle is
    # released on exit and .ExitCode comes back EMPTY -- which formats as a
    # blank in "(exit )" and, worse, compares unequal to 0, so every stage
    # reports FAIL. A verification script whose failure mode is "everything
    # failed" is survivable; the same bug inverted would not be.
    $null = $proc.Handle
    $procs[$name] = @{
        Proc = $proc
        Out  = $out
        Err  = $err
        SW   = [Diagnostics.Stopwatch]::StartNew()
    }
}

foreach ($name in $selected) {
    $p = $procs[$name]
    $p.Proc.WaitForExit()
    $p.SW.Stop()
    $results[$name] = @{
        Code    = $p.Proc.ExitCode
        Seconds = [math]::Round($p.SW.Elapsed.TotalSeconds, 1)
        Log     = $p.Out
        ErrLog  = $p.Err
    }
}

$swAll.Stop()

Write-Host ""
$failed = @()
foreach ($name in $results.Keys) {
    $r = $results[$name]
    $desc = $Stages[$name].Desc
    if ($r.Code -eq 0) {
        Write-Host ("  PASS  {0,-32} {1,6}s" -f $desc, $r.Seconds) -ForegroundColor Green
    }
    else {
        Write-Host ("  FAIL  {0,-32} {1,6}s  (exit {2})" -f $desc, $r.Seconds, $r.Code) -ForegroundColor Red
        $failed += $name
    }
}

foreach ($name in $failed) {
    $r = $results[$name]
    $pattern = $Stages[$name].Decisive
    Write-Host ""
    Write-Host "--- $name : decisive lines ---" -ForegroundColor Yellow
    $lines = @()
    foreach ($f in @($r.Log, $r.ErrLog)) {
        if (Test-Path $f) {
            $lines += @(Get-Content $f | Where-Object { $_ -match $pattern })
        }
    }
    if ($lines.Count -eq 0) {
        # A non-zero exit with nothing matching means the pattern is wrong, not
        # that the failure is imaginary. Say so rather than printing nothing --
        # silence here would read as "it failed but there is nothing wrong".
        Write-Host "  (no line matched /$pattern/ -- the stage failed anyway; tail follows)" -ForegroundColor Yellow
        $lines = @(Get-Content $r.Log -Tail 15)
    }
    $lines | Select-Object -First 25 | ForEach-Object { Write-Host "  $_" }
    Write-Host "  full log: $($r.Log)"
}

Write-Host ""
$wall = [math]::Round($swAll.Elapsed.TotalSeconds, 1)
if ($failed.Count -eq 0) {
    Write-Host "ALL PASS ($($results.Count) stages, ${wall}s wall)" -ForegroundColor Green
    Write-Host "logs: $logDir"
    exit 0
}
else {
    Write-Host "FAILED: $($failed -join ', ')  ($($results.Count) stages, ${wall}s wall)" -ForegroundColor Red
    exit 1
}
