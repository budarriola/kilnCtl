# check_skip_fast_classification.ps1 -- negative-test-shaped unit test for
# run_all_checks.ps1's SKIP-FAST classification (2a1c5c96, opus advisory 3).
#
# WHAT THIS PROVES. run_all_checks.ps1's Complete-CheckResult keys the
# SKIP-FAST bucket off a literal "SKIP-FAST: ..." line in a check's own
# stdout/stderr, not off the exit code (both SKIP and SKIP-FAST use the same
# reserved exit code 3) and not off $env:KILNCTL_CHECKS_FAST itself. This
# test proves that classification mechanically, end to end, by running a
# real nested run_all_checks.ps1 pass over two throwaway dummy check_*.ps1
# files it writes into the tree for the duration of the test:
#   - one that always prints "SKIP-FAST: dummy" and exits 3 -- a run
#     filtered to just this check must exit 0 (SKIP-FAST is non-fatal by
#     itself, with no -AllowSkips needed).
#   - one that always prints "SKIP: dummy" and exits 3 -- a run filtered to
#     just this check must exit 1 (a plain SKIP still fails the run by
#     default, unchanged by 2a1c5c96).
#
# This does not re-derive whether -Fast itself is passed to the OUTER
# run_all_checks.ps1 invocation that runs this very file -- it drives a
# separate, INNER invocation of run_all_checks.ps1 with -Only pointed at its
# own dummy files, so the classification is exercised independent of
# whatever flags the outer/real suite run happens to use.
#
# SCRATCH FILES. Named with $PID per the fixed-shared-scratch-path race
# class documented at the top of run_all_checks.ps1 -- two concurrent copies
# of this very check must not collide. Written under tools/ (not $env:TEMP)
# because run_all_checks.ps1's discovery glob only walks the repo tree,
# and removed in a finally block whether the test passes or not.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$runAllChecks = Join-Path $repoRoot "tools\run_all_checks.ps1"

$fastDummyName = "check_skipfast_classification_dummy_fast_$PID.ps1"
$plainDummyName = "check_skipfast_classification_dummy_plain_$PID.ps1"
$fastDummyPath = Join-Path $repoRoot "tools\$fastDummyName"
$plainDummyPath = Join-Path $repoRoot "tools\$plainDummyName"

$fail = @()

try {
    Set-Content -Path $fastDummyPath -Value @(
        'Write-Host "SKIP-FAST: dummy, unconditional, for check_skip_fast_classification.ps1"'
        'exit 3'
    ) -Encoding ascii

    Set-Content -Path $plainDummyPath -Value @(
        'Write-Host "SKIP: dummy, unconditional, for check_skip_fast_classification.ps1"'
        'exit 3'
    ) -Encoding ascii

    # Case 1: a check that prints "SKIP-FAST: ..." must be filed into the
    # non-fatal skipfast bucket and the run must exit 0 -- no -AllowSkips,
    # no -Fast needed on this OUTER call, since the dummy's own SKIP-FAST
    # line is unconditional and Complete-CheckResult's classification does
    # not consult $env:KILNCTL_CHECKS_FAST at all, only the printed text.
    $fastOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runAllChecks `
        -Only ([regex]::Escape($fastDummyName)) -AllowFewerChecks 2>&1 | Out-String
    $fastCode = $LASTEXITCODE
    if ($fastCode -ne 0) {
        $fail += "SKIP-FAST case: expected exit 0, got $fastCode. Output:`n$fastOut"
    } elseif ($fastOut -notmatch 'SKIP-FAST\s+tools[\\/]' ) {
        $fail += "SKIP-FAST case: exited 0 but output has no SKIP-FAST line for the dummy. Output:`n$fastOut"
    }

    # Case 2: a check that prints a plain "SKIP: ..." must still fail the
    # run by default (unchanged by 2a1c5c96) -- same dummy shape, same
    # exit code 3, different printed word.
    $plainOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runAllChecks `
        -Only ([regex]::Escape($plainDummyName)) -AllowFewerChecks 2>&1 | Out-String
    $plainCode = $LASTEXITCODE
    if ($plainCode -eq 0) {
        $fail += "plain SKIP case: expected a non-zero (fatal) exit, got 0. Output:`n$plainOut"
    } elseif ($plainOut -notmatch '(?<!-FAST)\bSKIP\s+tools[\\/]') {
        $fail += "plain SKIP case: exited $plainCode but output has no plain SKIP line for the dummy. Output:`n$plainOut"
    }
} finally {
    Remove-Item -ErrorAction SilentlyContinue $fastDummyPath, $plainDummyPath
}

if ($fail.Count -gt 0) {
    Write-Host ""
    Write-Host "FAILED: check_skip_fast_classification -- SKIP-FAST vs SKIP classification broke:" -ForegroundColor Red
    foreach ($f in $fail) {
        Write-Host $f -ForegroundColor Red
        Write-Host ""
    }
    exit 1
}

Write-Host "check_skip_fast_classification: PASS (SKIP-FAST is non-fatal, plain SKIP still fails by default)"
exit 0
