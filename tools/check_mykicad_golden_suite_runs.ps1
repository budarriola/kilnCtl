# check_mykicad_golden_suite_runs.ps1 -- fail loudly if the mykicadMcp golden
# test suite (tests/test_parsers_golden.py and its real-board siblings) stops
# actually running against the real kiln board.
#
# WHY THIS EXISTS. The 2026-08-28 hardware/firmware/tools split moved the
# KiCad project from the repo root to hardware/mainBoard/. tests/conftest.py's
# `kiln_project_path` fixture only ever walked direct ancestors looking for
# `kiln.kicad_pro`, so after the move it could not find the project, silently
# fell through to `pytest.skip(...)`, and every real-board test (the golden
# component/net counts, connector/bus/critical-net golden checks) went
# vacuous -- ~19 tests reported SKIPPED, which `pytest -q`'s summary line
# still shows as green. Nobody noticed for over a week. See the fix in
# tests/conftest.py (walks for hardware/mainBoard/kiln.kicad_pro too, and a
# `git show HEAD:./name` path fix) and the negative-test note in that file's
# history: temporarily breaking the discovered path reproduces the failure
# this check exists to catch.
#
# WHAT THIS CHECKS. Runs the mykicadMcp test suite and fails if:
#   - it can't be run at all (missing venv / pytest not installed / collection
#     error), or
#   - it collects and runs zero tests (a broken pytest invocation reporting
#     "no tests ran" is exactly as vacuous as an unnoticed skip), or
#   - any test in the golden/real-board files is SKIPPED (their whole point
#     is to run against the real, committed kiln.kicad_pcb; a skip there means
#     the `kiln_project_path` fixture fell back to "board not found" again).
#
# This does not re-derive golden constants -- that's a human/agent job when
# the board legitimately changes (see test_parsers_golden.py's own docstring
# for the current snapshot and why net-based checks are pinned at zero nets).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_mykicad_golden_suite_runs.ps1
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$mcpDir = Join-Path $repoRoot "tools\mykicadMcp"
$venvPython = Join-Path $mcpDir ".venv\Scripts\python.exe"

if (-not (Test-Path $venvPython)) {
    throw "check_mykicad_golden_suite_runs: venv python not found at $venvPython -- cannot run the suite at all."
}

# The real-board golden files: anything using the `kiln_project_path` fixture.
$goldenFiles = @(
    "tests\test_parsers_golden.py",
    "tests\test_bus_corridor.py",
    "tests\test_board_local_and_layers.py",
    "tests\test_writer_roundtrip.py",
    "tests\test_critical_nets.py",
    "tests\test_connectors.py"
) | Where-Object { Test-Path (Join-Path $mcpDir $_) }

if ($goldenFiles.Count -eq 0) {
    throw "check_mykicad_golden_suite_runs: none of the expected golden test files were found under $mcpDir -- discovery is broken, this check would pass vacuously."
}

Push-Location $mcpDir
try {
    $output = & $venvPython -m pytest $goldenFiles -q -rs 2>&1 | Out-String
} finally {
    Pop-Location
}

Write-Host $output

if ($output -match "no tests ran" -or $output -notmatch "(\d+) passed") {
    throw "check_mykicad_golden_suite_runs: the golden suite reported zero tests run. This is exactly the vacuous-skip failure mode this check exists to catch."
}

$passedMatch = [regex]::Match($output, "(\d+) passed")
$passedCount = [int]$passedMatch.Groups[1].Value
if ($passedCount -lt 8) {
    throw "check_mykicad_golden_suite_runs: only $passedCount test(s) passed (expected at least 8 from test_parsers_golden.py alone) -- some golden file likely failed to collect."
}

# A fixture that hard-fails (pytest.fail, e.g. the "board not found" case in
# conftest.py) surfaces as "N passed, M error(s)" or "N failed" in the
# summary, NOT as "no tests ran" -- checked separately from the zero-tests
# case above so a broken-fixture run can't slip through just because some
# unrelated tests in the same file still passed.
$failedMatch = [regex]::Match($output, "(\d+) failed")
$errorMatch = [regex]::Match($output, "(\d+) error")
$failedCount = if ($failedMatch.Success) { [int]$failedMatch.Groups[1].Value } else { 0 }
$errorCount = if ($errorMatch.Success) { [int]$errorMatch.Groups[1].Value } else { 0 }
if ($failedCount -gt 0 -or $errorCount -gt 0) {
    throw "check_mykicad_golden_suite_runs: $failedCount failed / $errorCount errored -- the golden suite is not cleanly green (see output above, likely kiln_project_path failing to find the real board)."
}

$skipMatches = [regex]::Matches($output, "SKIPPED \[\d+\] [^\r\n]*")
$boardSkips = $skipMatches | Where-Object {
    $_.Value -match "kiln board" -or $_.Value -match "conftest\.py"
}
if ($boardSkips.Count -gt 0) {
    Write-Host "GOLDEN SUITE SKIPPED AGAINST THE REAL BOARD:" -ForegroundColor Red
    foreach ($s in $boardSkips) { Write-Host "  $($s.Value)" -ForegroundColor Red }
    throw "check_mykicad_golden_suite_runs: $($boardSkips.Count) golden test(s) skipped because the real kiln board could not be found -- kiln_project_path is broken again."
}

Write-Host "check_mykicad_golden_suite_runs passed: $passedCount golden/real-board test(s) ran against the real kiln board, zero skipped."
exit 0
