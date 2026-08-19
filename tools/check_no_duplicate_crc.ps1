# check_no_duplicate_crc.ps1 -- the CI grep check ROADMAP.md M2's own
# checklist calls for:
#
#   CI grep: no CRC or byte-stuffing implementation outside `CommonFW`
#
# firmware/CommonFW's kilnlink component (kilnlink_crc.c / kilnlink_frame.c)
# is the canonical, single-source implementation of the link's CRC-16 and
# byte-stuffing framing. This script's job is to make sure a second,
# independently-written copy of that logic doesn't silently grow somewhere
# else in the tree and drift from it unnoticed.
#
# Signal chosen deliberately narrow, matching this repo's other CI grep
# scripts (see firmware/SaftyFW/tools/check_isolation.ps1): grep for the
# literal CRC-16/CCITT-FALSE polynomial constant 0x1021 used by
# kilnlink_crc.c. That polynomial is specific enough that matching it is a
# reliable signal of "this file computes the same CRC as kilnlink", and
# generic enough (independent of language: C, Python, ...) to catch a
# reimplementation in any of this repo's tools. It deliberately does NOT
# grep bare "crc"/"stuff" identifiers -- lodepng, frogfs, mDNS, and the
# SaftyFW bootloader's image-integrity CRC32 all have legitimate,
# unrelated CRCs of their own (different polynomial, different purpose:
# file/image integrity, not link framing) and would be pure noise.
#
# KNOWN PRE-EXISTING DEBT (as of 2026-08-18, updated same day):
# firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.c has been migrated
# to delegate its CRC/framing to kilnlink_crc16_ccitt_false/kilnlink_stuff
# (proven byte-identical against the old local implementation by
# firmware/CommonFW/test/test_uart_protocol_delegate.c; see ROADMAP.md M2's
# "KilnFW delegating framing and CRC" item, now done) and no longer matches
# the 0x1021 polynomial grep, so it has been removed from the allowlist below.
#
# firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.c was
# NOT migrated in that pass: it is a stale fork of the KilnFW file (missing
# BROADCAST handling, the retry-log rate limiter, and the register_task retry
# loop the current KilnFW file has), not a literal mirror, so treating it as
# "the same file, done twice" would be wrong. It stays allowlisted below
# until it is either resynced with KilnFW's file or migrated on its own
# terms.
#
# Two duplicates remain allowlisted:
#   - firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.c
#     (stale fork of the now-migrated KilnFW file, used for host unit tests --
#     see note above)
#   - tools/PcTools/src/kilnctrl/protocol.py and its UnitTestFw mirror
#     firmware/UnitTestFw/UnitTest/pc_tools/src/uart_control/protocol.py
#     (a deliberate, documented pure-Python port -- see its own docstring,
#     "Bit-for-bit port of crc16_ccitt_false() in uart_protocol.c" -- since
#     pc_tools can't link a C library; tracked by the "pc_tools consuming
#     the same vectors as the third implementation" item)
#
# DESIGN CHOICE: this check does NOT fail on that known, already-tracked
# debt today. Making it fail loudly right now would just be a second,
# redundant way of saying what ROADMAP.md M2 already says explicitly, and
# would leave the script permanently red (and therefore ignorable) until
# two unrelated, larger migration items land. Instead the known files are
# an explicit allowlist below, so:
#   - this check PASSES today, and stays wired to CI-able ahead of time
#   - it FAILS the moment anyone adds a *fifth* file matching the CRC
#     polynomial, which is the actual failure mode this check exists to
#     catch (an undetected, un-tracked new duplicate)
#   - deleting an allowlist entry is required as part of finishing the
#     migration items above, so the allowlist can't quietly go stale: once
#     uart_protocol.c stops computing its own CRC, its grep hit disappears
#     and the entry becomes dead weight that the next run of this reasoning
#     would flag in review.
#
# Usage: powershell -File tools\check_no_duplicate_crc.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

# Directories never scanned: build output, vendored/managed deps, venvs.
$excludeDirs = @(
    '\\build\\',
    '\\managed_components\\',
    '\\\.venv\\',
    '\\node_modules\\',
    '\\components\\lvgl\\'
)

# Known, already-tracked pre-migration duplicates (see header above).
# Paths are relative to repo root, forward-slash, case-insensitive compare.
$allowlist = @(
    'firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.c',
    'tools/PcTools/src/kilnctrl/protocol.py',
    'firmware/UnitTestFw/UnitTest/pc_tools/src/uart_control/protocol.py'
)

$searchExtensions = @('*.c', '*.h', '*.cpp', '*.hpp', '*.py')

$files = Get-ChildItem -Path $root -Recurse -File -Include $searchExtensions -ErrorAction SilentlyContinue |
    Where-Object {
        $full = $_.FullName
        if ($full -like (Join-Path $root "firmware\CommonFW") + "*") { return $false }
        foreach ($ex in $excludeDirs) {
            if ($full -match $ex) { return $false }
        }
        return $true
    }

$violations = @()
foreach ($f in $files) {
    $matched = Select-String -Path $f.FullName -Pattern '0x1021' -SimpleMatch:$false -Quiet
    if ($matched) {
        $rel = $f.FullName.Substring($root.Length + 1) -replace '\\', '/'
        if ($allowlist -notcontains $rel) {
            $violations += $rel
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "DUPLICATE CRC CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v -- matches the kilnlink CRC-16/CCITT-FALSE polynomial (0x1021) and is not on the known-debt allowlist" -ForegroundColor Red
    }
    throw "$($violations.Count) untracked duplicate CRC/framing implementation(s) found -- see ROADMAP.md M2 and tools/check_no_duplicate_crc.ps1's header"
}

Write-Host "Duplicate CRC check passed: no untracked CRC-16/CCITT-FALSE (0x1021) implementation outside firmware/CommonFW."
Write-Host "  ($($allowlist.Count) known pre-migration duplicate(s) allowlisted -- see script header; tracked by ROADMAP.md M2)"
exit 0
