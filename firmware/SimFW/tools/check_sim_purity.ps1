# check_sim_purity.ps1 -- enforces docs/PLAN.md section 9's repo-layout
# promise and section 13.1's testing-strategy premise for firmware/SimFW/src/sim/:
#
#   "Model is pure C, no RTOS dependencies, host-testable with the same
#   MSVC/CMake harness pattern SaftyFW/test uses." (PLAN.md section 4.3)
#
#   "everything in src/sim/ is pure and runs on the PC" (PLAN.md section 13.1)
#
# This is what lets the SAME source files compile both into the RP2040
# firmware image and into the MSVC/CMake host test suite (~4955 checks as of
# this pass) with zero divergence between "what shipped" and "what got
# tested". It is fragile: a single #include of a FreeRTOS or pico-sdk header
# silently breaks the MSVC build (those headers don't exist there), and a
# single GCC-only construct (inline asm, statement expressions, etc.) breaks
# it in the opposite direction by compiling on-target but not on the host.
# Both failure directions have real precedent in this project -- an inline-
# asm memory barrier written GCC-only once broke the MSVC host build (see
# tc_fault_state.c's own header comment for the fix, an #if defined(_MSC_VER)
# split using _ReadWriteBarrier() on MSVC and asm volatile("":::"memory") on
# GCC/Clang) -- so this check covers both directions, not just the RTOS-
# include one.
#
# Two things this script checks:
#   1. No FreeRTOS/pico-sdk/TinyUSB #include anywhere under src/sim/.
#   2. Every raw GCC-only inline-asm use is behind an `#if defined(_MSC_VER)`
#      / `#else` split with an MSVC-side equivalent -- i.e. never unguarded.
#      This is a best-effort structural check (PLAN.md section 13's own
#      "if practical" scope for this class of check), not a full compiler;
#      it looks for the specific pattern this project has already been
#      burned by, not every conceivable non-portable construct.
#
# Usage: powershell -File firmware\SimFW\tools\check_sim_purity.ps1
$ErrorActionPreference = "Stop"

$simfwRoot = Split-Path -Parent $PSScriptRoot
$simRoot = Join-Path $simfwRoot "src\sim"
# Test-only override so this script's own behavior can be exercised against
# a throwaway fixture directory without touching the real src/sim/ tree.
if ($env:SIMFW_SIM_ROOT_OVERRIDE) {
    $simRoot = $env:SIMFW_SIM_ROOT_OVERRIDE
}

if (-not (Test-Path $simRoot)) {
    throw "check_sim_purity.ps1: expected pure-C tree not found at $simRoot"
}

$files = Get-ChildItem -Path $simRoot -Recurse -File -Include "*.c", "*.h" -ErrorAction SilentlyContinue

if ($files.Count -eq 0) {
    throw "check_sim_purity.ps1: no .c/.h files found under $simRoot -- check looks broken, not passing"
}

# --- Same comment-stripping helper as the repo's other CI grep checks ------
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $result += $code
    }
    return $result
}

# --- Rule 1: forbidden includes --------------------------------------------
# FreeRTOS headers, pico-sdk headers (pico/*, hardware/*), and TinyUSB. Note
# this deliberately checks CODE lines (comments stripped) -- this file's own
# header above, and other files' doc comments, may name these headers in
# prose without tripping the check.
$forbiddenIncludePattern = '#include\s*["<](FreeRTOS\.h|task\.h|queue\.h|semphr\.h|timers\.h|event_groups\.h|portable\.h|projdefs\.h|pico/|hardware/|tusb\.h)'

$failures = @()

foreach ($f in $files) {
    $rel = "src/sim/$($f.Name)"
    $codeLines = Get-CodeOnlyLines -Path $f.FullName

    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($codeLines[$i] -match $forbiddenIncludePattern) {
            $failures += "$($rel):$($i + 1): forbidden FreeRTOS/pico-sdk/TinyUSB include in src/sim/ -- $($codeLines[$i].Trim())"
        }
    }

    # --- Rule 2: unguarded GCC-only inline asm ------------------------------
    # Walk the file tracking #if/#else/#endif nesting for any branch keyed on
    # _MSC_VER. An asm use is "guarded" only if it sits in the non-MSVC side
    # of such a split (either `#if !defined(_MSC_VER)` / `#ifndef _MSC_VER`
    # directly, or the `#else` of an `#if defined(_MSC_VER)` / `#ifdef
    # _MSC_VER`). Anything else -- no split at all, or asm on the MSVC side --
    # is flagged: this project's real incident was exactly "asm with no
    # split", so that is the failure mode this rule exists to catch.
    $stack = New-Object System.Collections.Generic.List[bool]  # true = current branch is the non-MSVC (asm-safe) side
    $pendingElseIsGuard = New-Object System.Collections.Generic.List[bool]

    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]

        if ($line -match '^\s*#\s*if(?:def|ndef)?\b(.*)$') {
            $cond = $Matches[1]
            if ($line -match '#\s*ifndef\s+_MSC_VER') {
                $stack.Add($true)
                $pendingElseIsGuard.Add($false)
            } elseif ($cond -match '!\s*defined\s*\(\s*_MSC_VER\s*\)') {
                $stack.Add($true)
                $pendingElseIsGuard.Add($false)
            } elseif ($line -match '#\s*ifdef\s+_MSC_VER' -or $cond -match 'defined\s*\(\s*_MSC_VER\s*\)') {
                $stack.Add($false)
                $pendingElseIsGuard.Add($true)  # the matching #else IS the guarded/asm-safe branch
            } else {
                # Unrelated #if -- inherit nothing special; not asm-safe by itself.
                $stack.Add($false)
                $pendingElseIsGuard.Add($false)
            }
            continue
        }
        if ($line -match '^\s*#\s*else\b') {
            if ($stack.Count -gt 0) {
                $idx = $stack.Count - 1
                $stack[$idx] = $pendingElseIsGuard[$idx]
            }
            continue
        }
        if ($line -match '^\s*#\s*endif\b') {
            if ($stack.Count -gt 0) {
                $stack.RemoveAt($stack.Count - 1)
                $pendingElseIsGuard.RemoveAt($pendingElseIsGuard.Count - 1)
            }
            continue
        }

        if ($line -match '__asm\b|\basm\s*(volatile)?\s*\(') {
            $currentlyGuarded = ($stack.Count -gt 0) -and $stack[$stack.Count - 1]
            if (-not $currentlyGuarded) {
                $failures += "$($rel):$($i + 1): GCC-only inline asm not behind an '#if !defined(_MSC_VER)' / '#else of #if defined(_MSC_VER)' split -- breaks the MSVC host-test build (see tc_fault_state.c for the correct pattern) -- $($line.Trim())"
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "SIM PURITY CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) src/sim/ purity violation(s) found -- see docs/PLAN.md sections 4.3/9/13.1"
}

Write-Host "Sim purity check passed: $($files.Count) file(s) under src/sim/ have no FreeRTOS/pico-sdk/TinyUSB includes, and every GCC-only inline-asm use is behind an _MSC_VER split."
exit 0
