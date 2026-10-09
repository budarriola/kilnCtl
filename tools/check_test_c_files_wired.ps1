# check_test_c_files_wired.ps1 -- every *.c file under firmware/KilnFW/App/test/
# and firmware/SaftyFW/test/ must be reachable by SOME automated build path,
# or this check fails.
#
# WHY THIS EXISTS. 2026-09-10 opus review: check_no_orphaned_checks.ps1 closes
# the "test exists but nothing runs it" class for check_*.ps1|.py and
# test_*.ps1|.py -- but it enumerates only those four filename shapes, so a
# .c test source with its own main() is invisible to it. Two were found
# orphaned this way: test_flash_worker_boot_order.c (its own main(), never
# named in build_host_tests.ps1, referenced nowhere but a prose mention in a
# planning doc) and sim_credibility_gate_closedloop.c (600 lines, never
# built by anything). check_c_files_in_cmakelists.ps1 explicitly excludes
# test/ from its scope (it exists to catch a completely different class --
# a real-target source missing from CMakeLists -- see its own header), so no
# existing check covers this class for .c files at all.
#
# WHAT COUNTS AS WIRED. A test .c file is wired if ANY of:
#   1. Its basename appears literally (as text) in the build_host_tests.ps1
#      that lives in the same test/ directory -- the normal case: named in
#      a `cl` command line, in an Invoke-HostTestExe -BuildCmd string, or
#      passed to a linker directly.
#   2. It is #include-d by basename inside another *.c file in the same
#      test/ directory -- test_adaptive_tune.c's own
#      `#include "test_adaptive_tune_dwell.c"` (and five siblings) is the
#      standing convention: those six files never appear by name in
#      build_host_tests.ps1 at all, only test_adaptive_tune.c does, but they
#      are compiled as part of it. This check does not require the
#      including file itself be wired via rule 1 -- in every observed case
#      here it also is, but requiring it would add a recursion this check
#      does not need to prove today.
#   3. Its basename appears literally in a check_*.py or check_*.ps1 file in
#      the same test/ directory -- the drift-check convention
#      (heater_output_pwm_drift_check.py builds and runs
#      heater_output_pwm_drift_harness.c itself, entirely outside
#      build_host_tests.ps1; pid_fuzzy_drift_check.py/harness.c is the same
#      shape). Those check_*.py/.ps1 files are themselves covered by
#      check_no_orphaned_checks.ps1, so this is coverage-by-a-covered-check,
#      not a dead end.
#
# A file satisfying none of the three is an orphan: nothing in this repo's
# automated tooling ever compiles it, so it cannot even prove it still
# builds, let alone that it passes.
#
# EXCLUSIONS: files whose own name already matches check_*.ps1/.py or
# test_*.ps1/.py are out of scope here -- those are check_no_orphaned_checks.ps1's
# job, not this one (a .c file's job is being COMPILED, not being invoked as
# a script). Build output under */build/ is not source.
#
# NEGATIVE-TESTED: proven to fail against a deliberately orphaned .c file
# dropped in firmware/KilnFW/App/test/, then removed by hand (see the commit
# this shipped in for the transcript) -- an empty git diff on every touched
# production/test file after the negative test is the proof this check can
# actually fail, not just pass by construction.
#
# Usage: powershell -File tools\check_test_c_files_wired.ps1
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

$testDirs = @(
    (Join-Path $repoRoot "firmware\KilnFW\App\test"),
    (Join-Path $repoRoot "firmware\SaftyFW\test")
)

# Strip comments before matching so a basename mentioned only in prose, a
# commented-out block, or a skip-list comment does not count as "wired" --
# rules 1 and 3 used to `-match` the raw file text, so a paragraph of prose
# (this repo has one in front of nearly every build recipe) could satisfy
# them with no actual build/include directive behind it. Strips PowerShell/
# Python "#" line comments and PowerShell "<# ... #>" block comments; good
# enough for this check's purpose (real build directives are code, not
# comments) without needing a full tokenizer.
function Strip-ScriptComments {
    param([string]$Text)
    $noBlock = $Text -replace '(?s)<#.*?#>', ''
    $lines = $noBlock -split "`r?`n" | ForEach-Object {
        if ($_ -match '^\s*#') { '' } else { $_ -replace '(?<!\S)#.*$', '' }
    }
    return ($lines -join "`n")
}

$orphans = @()
$totalConsidered = 0

foreach ($testDir in $testDirs) {
    if (-not (Test-Path $testDir)) {
        throw "check_test_c_files_wired: $testDir not found -- has it moved? This check is now blind for that firmware."
    }

    $buildScript = Join-Path $testDir "build_host_tests.ps1"
    if (-not (Test-Path $buildScript)) {
        throw "check_test_c_files_wired: $buildScript not found -- has it moved or been renamed? This check is now blind for $testDir."
    }
    $buildScriptText = Strip-ScriptComments (Get-Content -Raw -Path $buildScript)

    # All *.c files directly under this test/ dir (one level -- neither test
    # tree nests .c sources deeper than this today; build/ output lives
    # under test/build/ and is excluded).
    $cFiles = Get-ChildItem -Path $testDir -File -Filter "*.c" |
        Where-Object { $_.FullName -notmatch '[\\/]build[\\/]' } |
        Sort-Object Name

    if ($cFiles.Count -lt 5) {
        throw "check_test_c_files_wired: found implausibly few .c files ($($cFiles.Count)) directly under $testDir -- has the tree shape changed? This check has gone blind rather than the tree having gone empty."
    }

    # Rule 3 sources: every .py / .ps1 script in the same directory (not
    # just a check_*/test_* named one -- this repo's drift-check convention
    # names the SCRIPT after the SUBJECT with a _check.py suffix, e.g.
    # heater_output_pwm_drift_check.py, not check_heater_output_pwm_drift.py
    # -- that naming is exactly why check_no_orphaned_checks.ps1's own
    # check_*/test_* glob does not reach these files by name either; the
    # thin check_*.ps1 wrapper that DOES exist, e.g.
    # check_heater_output_pwm_drift.ps1, is a separate file that invokes
    # this one, so scanning both/either finds the harness reference).
    $driftCheckFiles = Get-ChildItem -Path (Join-Path $testDir "*") -File -Include "*.py", "*.ps1" -ErrorAction SilentlyContinue
    $driftCheckText = ""
    foreach ($f in $driftCheckFiles) {
        $driftCheckText += (Strip-ScriptComments (Get-Content -Raw -Path $f.FullName))
        $driftCheckText += "`n"
    }

    # Rule 2 sources: every OTHER *.c file's own text, to find #include
    # "basename.c" references.
    $allCText = @{}
    foreach ($f in $cFiles) {
        $allCText[$f.Name] = Get-Content -Raw -Path $f.FullName
    }

    foreach ($f in $cFiles) {
        $totalConsidered++
        $name = $f.Name
        $wired = $false

        if ($buildScriptText -match [regex]::Escape($name)) {
            $wired = $true
        }

        if (-not $wired) {
            foreach ($otherName in $allCText.Keys) {
                if ($otherName -eq $name) { continue }
                if ($allCText[$otherName] -match ('#include\s*"' + [regex]::Escape($name) + '"')) {
                    $wired = $true
                    break
                }
            }
        }

        if (-not $wired -and $driftCheckText -match [regex]::Escape($name)) {
            $wired = $true
        }

        if (-not $wired) {
            $rel = $f.FullName.Substring($repoRoot.Length + 1) -replace '\\', '/'
            $orphans += $rel
        }
    }
}

if ($orphans.Count -gt 0) {
    Write-Host ""
    Write-Host "FAILED: found $($orphans.Count) test .c file(s) that no automated build path" -ForegroundColor Red
    Write-Host "        reaches -- not named in their directory's build_host_tests.ps1, not" -ForegroundColor Red
    Write-Host "        #include-d by a sibling test .c, and not named in a same-directory" -ForegroundColor Red
    Write-Host "        drift-check script:" -ForegroundColor Red
    foreach ($o in $orphans) {
        Write-Host "  $o" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "        Wire it into build_host_tests.ps1 (see any Invoke-HostTestExe block" -ForegroundColor Red
    Write-Host "        for the pattern), or delete it if it is genuinely dead." -ForegroundColor Red
    exit 1
}

Write-Host "check_test_c_files_wired: PASS ($totalConsidered test .c files across $($testDirs.Count) test/ trees all reachable by an automated build path)"
exit 0
