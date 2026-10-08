# checkcache: ok
# check_bridge_reject_reason.ps1 -- KilnFW/TODO.md section 11's "a reasonless
# rejection is byte-identical to an empty success" item.
#
# uart_bridge.c's bridge_reply_reject() emits {subcmd, ok=0} and appends a
# length-prefixed reason string only when the caller supplies one. With a NULL
# reason the reply is exactly two bytes: {subcmd, 0}.
#
# Two query replies in that same file are ALSO exactly {subcmd, count} with a
# count that can legitimately be zero -- THERMO_CMD_READ_FAULTS ("no channel
# has a fault") and IO_CMD_SX_SCAN ("no expander answered"). So a reasonless
# rejection of either subcommand is byte-identical to that subcommand's honest
# empty success, and a host cannot tell "this firmware has never heard of your
# command" from "we ran it and found nothing". bridge_reply_unsupported() --
# the default: case for an unrecognized subcmd, i.e. exactly the path an older
# or differently-configured firmware build takes -- used to pass NULL, so this
# was reachable in the field, not in theory.
#
# WHY THIS SCRIPT EXISTS, and not just a host test. The fix lives in C. The
# tests that cover it are in Python (tools/PcTools/tests/
# test_bridge_reject_reply.py, UnsupportedVsEmptySuccessCollisionTests), and
# they build their own frames with a hardcoded reason string -- so they
# demonstrate that the host decoder distinguishes the two shapes, but they
# CANNOT fail if someone reverts the C side to a NULL reason. They would keep
# passing against firmware that had regressed. Per this repo's rule that a
# check nobody has seen fail is not evidence, the C-side rule needs a C-side
# guard. This is it.
#
# What this catches, in firmware/KilnFW/App/drivers/uart_bridge*.c (uart_bridge.c
# plus its per-task siblings -- see the 2026-09-01 split note below):
#   - any bridge_reply_reject() call whose reason argument is NULL
#   - bridge_reply_unsupported() being defined to pass NULL (or an empty
#     string literal) through to bridge_reply_reject()
# It deliberately does NOT flag bridge_reply_reject()'s own `if (reason && ...)`
# NULL-tolerance, which stays as defensive handling, nor prose comments
# discussing the NULL shape and its history -- comments are stripped first.
#
# Usage: powershell -File tools\check_bridge_reject_reason.ps1
param(
    [string]$DriversDir
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($DriversDir) {
    $driversDir = (Resolve-Path $DriversDir).Path
} else {
    $driversDir = Join-Path $root "..\firmware\KilnFW\App\drivers"
    $driversDir = (Resolve-Path $driversDir).Path
}

# 2026-09-01: uart_bridge.c was split (commit b268377) into per-task
# translation units -- uart_bridge.c now keeps only the shared helpers and
# the PC link watchdog; every bridge_reply_reject()/bridge_reply_unsupported()
# CALL site moved to uart_bridge_thermo.c, uart_bridge_io.c, uart_bridge_touch.c,
# uart_bridge_ui_test.c, uart_bridge_safety.c, uart_bridge_system.c and
# uart_bridge_info.c. Scanning only uart_bridge.c after that split checks
# almost nothing (2 calls left there) and would have let this exact defect
# class back in unnoticed. Scan every uart_bridge*.c file instead of a single
# hardcoded name, so a future re-split doesn't quietly blind this check again;
# uart_log_bridge.c is a different, unrelated log-shipping link and is
# excluded by the name filter below (it does not start with "uart_bridge").
$targetFiles = Get-ChildItem -Path $driversDir -Filter "uart_bridge*.c" -File -Recurse |
    Where-Object { $_.Name -notlike "uart_log_bridge*" }
if ($targetFiles.Count -lt 5) {
    throw "check_bridge_reject_reason.ps1: only $($targetFiles.Count) uart_bridge*.c file(s) found under $driversDir -- has the bridge code moved or been re-merged? Update this script's target."
}

# Same comment-stripping helper as check_uart_version_independence.ps1
# (duplicated rather than imported -- this project has no shared PowerShell
# module mechanism).
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

$failures = @()

# A call is a violation if its final argument is NULL or "". Calls are single-
# line throughout these files; a call split across lines would be missed, so
# the sanity check below asserts we still recognise a plausible number of
# them (across all uart_bridge*.c files combined, now that the split moved
# most calls out of uart_bridge.c itself).
$callPattern = 'bridge_reply_reject\s*\('
$badReasonPattern = ',\s*(NULL|"")\s*\)\s*;'
$callCount = 0
$wrapperFound = $false

foreach ($f in $targetFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]
        if ($line -match $callPattern) {
            # Skip the function's own definition/declaration lines.
            if ($line -match '^\s*static\s+void\s+bridge_reply_reject') { continue }
            $callCount++
            if ($line -match $badReasonPattern) {
                $failures += "$($f.FullName):$($i + 1): bridge_reply_reject() called with a NULL/empty reason -- $($line.Trim())"
            }
        }
        # bridge_reply_unsupported() lost its `static` when uart_bridge.c was
        # split (2026-09-01, commit b268377): callers in the new
        # uart_bridge_*.c translation units need external linkage now, so the
        # definition is `void bridge_reply_unsupported(...)`, not
        # `static void ...`. Match with an optional `static` so this keeps
        # recognising the wrapper whether or not it is file-local.
        if ($line -match '^\s*(static\s+)?void\s+bridge_reply_unsupported\s*\(') {
            $wrapperFound = $true
        }
    }
}

if (-not $wrapperFound) {
    throw "check_bridge_reject_reason.ps1: bridge_reply_unsupported() not found in any of: $($targetFiles.FullName -join ', ') -- has it moved or been renamed? Update this script's pattern."
}
if ($callCount -lt 10) {
    throw "check_bridge_reject_reason.ps1: only $callCount bridge_reply_reject() call(s) recognised across $($targetFiles.Count) uart_bridge*.c file(s), which is implausibly few -- the call style has probably changed (e.g. calls now split across lines) and this check has gone blind. Update the pattern."
}

if ($failures.Count -gt 0) {
    Write-Host "BRIDGE REJECT REASON CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "Every bridge_reply_reject() call must supply a non-empty reason. A reasonless rejection is the 2-byte {subcmd, 0} frame, which is byte-identical to THERMO_CMD_READ_FAULTS's and IO_CMD_SX_SCAN's honest empty-success reply -- a host cannot tell 'unsupported command' from 'found nothing'. See uart_bridge.c's bridge_reply_unsupported() comment and KilnFW/TODO.md section 11."
}

Write-Host "Bridge reject reason check passed: all $callCount bridge_reply_reject() call(s) supply a reason."
exit 0
