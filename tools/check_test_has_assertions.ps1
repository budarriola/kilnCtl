# check_test_has_assertions.ps1 -- every test function actually EXECUTED by
# a host-test suite must contain at least one assertion, and that assertion
# must not be comparing two literal values the test itself just wrote (which
# can never fail regardless of the implementation).
#
# WHY THIS EXISTS. A 2026-08-31 vacuous-test sweep found the shipped defect
# class this guards against twice in one session: a fuzzy-strength A/B test
# that compared two runs both taken at strength 0.0 (identical code path,
# could never differ), and a settle-criterion check justified against a
# constant the test itself defined (mutating the constant left the suite
# green). Both compiled, both ran, both showed up in "N/N checks passed" --
# nothing short of reading every test body caught them. This check cannot
# read intent, but it CAN catch the two purely mechanical shapes: a test
# function with NO assertion call at all, and an assertion whose both sides
# are bare numeric/string literals (a tautology no implementation change can
# flip).
#
# SCOPE. C host tests under firmware/KilnFW/App/test and firmware/SaftyFW/test
# (TEST_CHECK / TEST_CHECK_NEAR / TEST_CHECK_MSG / CHECK macros), and Python
# tests under tools/PcTools/tests (assert statements in test_* functions).
# "Executed" is approximated by requiring the function to be referenced
# elsewhere in the file (its own suite's dispatch: main()/run_test_*() call
# it by name, or pytest's own collection for *_test.py files) -- a helper
# that merely resets shared state (test_post_hooks_reset, etc.) is filtered
# out by requiring the name to start with test_ AND be called by name at
# least once elsewhere in the same file (pytest functions are always
# "called" by the framework, so every top-level test_ function in a .py file
# counts).
#
# LIMITS. This is a syntactic scan, not a compiler or a test runner: it
# cannot see assertions built dynamically, macros that expand to checks
# under a different name, or a tautology spread across two lines. It proves
# absence of the two specific shapes above, nothing more -- see the sweep
# report for the mutation-based verification that actually proves coverage.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_test_has_assertions.ps1
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

$cDirs = @(
    (Join-Path $repoRoot "firmware\KilnFW\App\test"),
    (Join-Path $repoRoot "firmware\SaftyFW\test")
)
$pyDirs = @(
    (Join-Path $repoRoot "tools\PcTools\tests")
)

$violations = @()
$scannedFiles = 0
$scannedFns = 0

# ---- C test files -----------------------------------------------------
$assertMacroPattern = 'TEST_CHECK(_NEAR|_MSG)?\s*\(|(?<![A-Za-z0-9_])CHECK\s*\('
$literalToken = '(-?\d+(\.\d+)?[fFuUlL]*|"[^"]*"|true|false|NULL)'

foreach ($dir in $cDirs) {
    if (-not (Test-Path $dir)) { continue }
    $files = Get-ChildItem -Path $dir -Filter "test_*.c" -File
    foreach ($file in $files) {
        $text = Get-Content -Raw -Path $file.FullName
        $scannedFiles++

        # Locally-defined helper functions (static ret name(...) { ) -- if a
        # test body calls one of these instead of an assertion macro
        # directly, assume (this check cannot see inside it cheaply) that
        # the helper is where the checking happens, same as compare_one()
        # in test_uart_protocol_link_delegate.c or the guard1-relaxation
        # verify_* helpers in test_autotune_engine_prestart.c. This trades
        # a false negative on a helper that itself asserts nothing for
        # avoiding a false positive on the many tests that legitimately
        # factor their assertion out.
        $localFns = New-Object System.Collections.Generic.HashSet[string]
        foreach ($lm in [regex]::Matches($text, '(?m)^static\s+[\w\*]+\s+(\w+)\s*\([^;{]*\)\s*\{')) {
            [void]$localFns.Add($lm.Groups[1].Value)
        }

        # "Executed" means dispatched BY NAME from main() or a run_test_*()
        # aggregator -- the file's own menu of what actually runs. A helper
        # that resets shared state and happens to be named test_* (called
        # only from other setup code, never from the menu) is deliberately
        # NOT in scope: it was never claiming to be a test.
        $dispatched = New-Object System.Collections.Generic.HashSet[string]
        $menuFns = [regex]::Matches($text, '(?:^|\n)\s*(?:void|int)\s+(main|run_test_\w*)\s*\([^)]*\)\s*\{')
        foreach ($mm in $menuFns) {
            $mStart = $mm.Index + $mm.Length
            $d = 1
            $k = $mStart
            while ($d -gt 0 -and $k -lt $text.Length) {
                if ($text[$k] -eq '{') { $d++ }
                elseif ($text[$k] -eq '}') { $d-- }
                $k++
            }
            $menuBody = $text.Substring($mStart, $k - $mStart - 1)
            foreach ($cm2 in [regex]::Matches($menuBody, '(test_\w+)\s*\(\s*\)\s*;')) {
                [void]$dispatched.Add($cm2.Groups[1].Value)
            }
        }

        # Every static void test_xxx(void) { ... } function body, matched by
        # brace depth so nested blocks don't truncate it early.
        $fnStarts = [regex]::Matches($text, 'static\s+void\s+(test_\w+)\s*\(\s*void\s*\)\s*\{')
        foreach ($m in $fnStarts) {
            $name = $m.Groups[1].Value
            if (-not $dispatched.Contains($name)) { continue }

            $bodyStart = $m.Index + $m.Length
            $depth = 1
            $i = $bodyStart
            while ($depth -gt 0 -and $i -lt $text.Length) {
                $c = $text[$i]
                if ($c -eq '{') { $depth++ }
                elseif ($c -eq '}') { $depth-- }
                $i++
            }
            $body = $text.Substring($bodyStart, $i - $bodyStart - 1)

            $scannedFns++
            $checks = [regex]::Matches($body, $assertMacroPattern)
            if ($checks.Count -eq 0) {
                $delegates = $false
                foreach ($cm3 in [regex]::Matches($body, '(\w+)\s*\(')) {
                    $callee = $cm3.Groups[1].Value
                    if ($callee -ne $name -and $localFns.Contains($callee)) {
                        $delegates = $true
                        break
                    }
                }
                if (-not $delegates) {
                    $violations += "$($file.Name): $name -- no assertion macro call, direct or via a local helper"
                }
                continue
            }

            # For each assertion call, pull its first argument (up to the
            # first top-level comma) and flag one that is a bare "literal ==
            # literal" / "literal != literal" comparison -- a tautology no
            # production change can affect.
            foreach ($cm in $checks) {
                $argStart = $cm.Index + $cm.Length
                $d = 1
                $j = $argStart
                while ($d -gt 0 -and $j -lt $body.Length) {
                    if ($body[$j] -eq '(') { $d++ }
                    elseif ($body[$j] -eq ')') { $d-- }
                    if ($d -eq 1 -and $body[$j] -eq ',') { break }
                    $j++
                }
                $firstArg = $body.Substring($argStart, $j - $argStart).Trim()
                if ($firstArg -match "^$literalToken\s*(==|!=)\s*$literalToken$") {
                    $violations += "$($file.Name): $name -- assertion `"$firstArg`" compares two literals, cannot fail"
                }
            }
        }
    }
}

# ---- Python test files -------------------------------------------------
# Scoped to the one mechanical shape that is unambiguous in this codebase's
# idiom: an `assert` statement whose both sides are bare literals (a
# tautology). Deliberately NOT flagging "no assert at all" here -- this
# suite leans heavily and legitimately on pytest.raises(...)/no-exception-
# means-pass and helper-function assertions the caller re-uses, so a bare
# per-function assert count would be almost all noise, not signal.
$pyLiteralToken = '(-?\d+(\.\d+)?|"[^"]*"|''[^'']*''|True|False|None)'
foreach ($dir in $pyDirs) {
    if (-not (Test-Path $dir)) { continue }
    $files = Get-ChildItem -Path $dir -Filter "test_*.py" -File
    foreach ($file in $files) {
        $lines = Get-Content -Path $file.FullName
        $scannedFiles++
        for ($ln = 0; $ln -lt $lines.Count; $ln++) {
            $line = $lines[$ln]
            if ($line -match "^\s*assert\s+$pyLiteralToken\s*(==|!=)\s*$pyLiteralToken\s*(,.*)?$") {
                $violations += "$($file.Name):$($ln + 1) -- assertion `"$($line.Trim())`" compares two literals, cannot fail"
            }
        }
        $scannedFns++
    }
}

if ($scannedFiles -eq 0 -or $scannedFns -eq 0) {
    throw "check_test_has_assertions: scanned $scannedFiles file(s) / $scannedFns test function(s) -- discovery found nothing, this check would pass vacuously."
}

if ($violations.Count -gt 0) {
    Write-Host "TEST-HAS-ASSERTIONS CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A test with no assertion, or an assertion comparing two literals," -ForegroundColor Red
    Write-Host "  passes regardless of what the production code does." -ForegroundColor Red
    throw "$($violations.Count) vacuous-shaped test function(s) found"
}

Write-Host "Test-has-assertions check passed: all $scannedFns executed test function(s) across $scannedFiles file(s) contain a real, non-tautological assertion."
exit 0
