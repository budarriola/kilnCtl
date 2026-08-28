# Runs every standing guard script in the repository and returns a single
# verdict.
#
# ROADMAP.md M10 has carried this item for a while: "All of tools/check_*.ps1
# and firmware/*/tools/check_*.ps1 are standalone and manual today. Every one of
# them has been proven able to fail, which is the hard part; being run
# automatically is the easy part nobody has done."
#
# DISCOVERY, NOT A LIST. The scripts are found by globbing, deliberately. A
# hardcoded list is this repository's single most repeated defect -- the HTTP
# route cap fell behind the real route count four separate times, each time
# surfacing as "one page is broken", and a comment saying "keep this ahead of
# the count" had already failed three times before the count was made to
# recompute itself. A new check_*.ps1 must be run by this script the moment it
# is written, without anyone remembering to come here.
#
# The trap that discovery introduces instead is the opposite one: a glob that
# matches nothing reports "all passed" in a cheerful green, which is worse than
# a failure because it looks like evidence. $MinimumChecks below is the floor
# that makes that case loud. It is not a target to keep bumping -- it is a
# tripwire for a broken glob, a moved directory, or a wrong working directory.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1
#   powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1 -ListOnly
#
# Exit code is 0 only if every discovered check passed and at least
# $MinimumChecks of them were found.

param(
    # Print what would run, run nothing. For confirming the glob sees what you
    # expect after moving a directory.
    [switch]$ListOnly,

    # Skip the discovery floor. Only for a deliberate partial tree.
    [switch]$AllowFewerChecks
)

$ErrorActionPreference = "Stop"

# The repository root is this script's parent's parent -- derived, never
# assumed from the caller's working directory, so the script gives the same
# answer whether it is run from the root, from an editor, or from a hook.
$repoRoot = Split-Path -Parent $PSScriptRoot

# Every check_*.ps1 anywhere in the tree, excluding build output and any
# vendored/third-party directory that might ship its own.
$checks = Get-ChildItem -Path $repoRoot -Filter "check_*.ps1" -Recurse -File |
    Where-Object {
        $_.FullName -notmatch '\\build\\' -and
        $_.FullName -notmatch '\\node_modules\\' -and
        # Any dotted directory: .venv, .git, and -- the one that actually bit
        # here -- .claude\worktrees\, which holds leftover per-agent copies of
        # the whole tree. Without this the first run of this script found 24
        # scripts where the repository has 11, and would have been reporting
        # the pass/fail state of an abandoned worktree alongside the real one.
        $_.FullName -notmatch '\\\.[^\\]+\\'
    } |
    Sort-Object FullName

# As of 2026-08-28 there are several: some under tools/, two under
# firmware/SaftyFW/tools/, and the floor is set below the real count on
# purpose. It exists to catch "the glob found nothing", not to assert an
# exact inventory; setting it equal to the count would turn every legitimate
# deletion into a failure and teach people to edit this number, which is how
# the route cap got into trouble.
$MinimumChecks = 5

if ($checks.Count -lt $MinimumChecks -and -not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: found only $($checks.Count) check scripts under $repoRoot," -ForegroundColor Red
    Write-Host "        which is below the floor of $MinimumChecks. This almost certainly means" -ForegroundColor Red
    Write-Host "        the glob is broken or a directory moved -- NOT that the repository is" -ForegroundColor Red
    Write-Host "        clean. Investigate before trusting any green result from this script." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
}

if ($ListOnly) {
    Write-Host "$($checks.Count) check scripts discovered:"
    foreach ($c in $checks) {
        Write-Host "  $($c.FullName.Substring($repoRoot.Length + 1))"
    }
    exit 0
}

Write-Host ""
Write-Host "Running $($checks.Count) guard scripts from $repoRoot"
Write-Host ""

$failed = @()
$passed = @()

foreach ($c in $checks) {
    $rel = $c.FullName.Substring($repoRoot.Length + 1)

    # Each check is run from ITS OWN directory's parent project, because
    # several resolve paths relative to $PSScriptRoot and at least one
    # (check_uri_handler_cap.ps1) recounts from source trees it locates that
    # way. Running them all from the repository root would have worked today
    # and broken silently the first time one of them changed how it resolves.
    $checkDir = Split-Path -Parent $c.FullName

    # Run in a child powershell so that a check calling `exit` cannot terminate
    # this aggregator, and so $ErrorActionPreference = "Stop" inside one check
    # cannot leak out. The exit code is the whole contract.
    Push-Location $checkDir
    try {
        # $ErrorActionPreference is dropped to Continue for exactly this call.
        # Under "Stop", ANY line a child process writes to stderr is promoted to
        # a terminating NativeCommandError -- so the first failing check would
        # abort this aggregator and the remaining checks would never run, while
        # the output still looked like a report. A runner that stops at the
        # first failure is a runner that hides every failure after it.
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $c.FullName 2>&1
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prev
    } finally {
        Pop-Location
    }

    if ($code -eq 0) {
        $passed += $rel
        Write-Host "  PASS  $rel" -ForegroundColor Green
    } else {
        $failed += [pscustomobject]@{ Path = $rel; Code = $code; Output = ($output | Out-String) }
        Write-Host "  FAIL  $rel (exit $code)" -ForegroundColor Red
    }
}

Write-Host ""

if ($failed.Count -gt 0) {
    Write-Host "$($failed.Count) of $($checks.Count) checks FAILED:" -ForegroundColor Red
    foreach ($f in $failed) {
        Write-Host ""
        Write-Host "--- $($f.Path) (exit $($f.Code)) ---" -ForegroundColor Red
        Write-Host $f.Output.TrimEnd()
    }
    Write-Host ""
    exit 1
}

Write-Host "All $($checks.Count) checks passed." -ForegroundColor Green
exit 0
