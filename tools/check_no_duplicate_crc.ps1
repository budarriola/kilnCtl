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
# KNOWN PRE-EXISTING DEBT (as of 2026-08-18, updated 2026-08-23):
# firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.c has been migrated
# to delegate its CRC/framing to kilnlink_crc16_ccitt_false/kilnlink_stuff
# (proven byte-identical against the old local implementation by
# firmware/CommonFW/test/test_uart_protocol_delegate.c; see ROADMAP.md M2's
# "KilnFW delegating framing and CRC" item, now done) and no longer matches
# the 0x1021 polynomial grep, so it has been removed from the allowlist below.
#
# firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.c (a
# stale fork of the KilnFW file, never migrated) and its pc_tools mirror
# firmware/UnitTestFw/UnitTest/pc_tools/src/uart_control/protocol.py used to
# be allowlisted here too, then were removed from the allowlist when
# `firmware/UnitTestFw` was deleted (the bench-fixture firmware that had
# replaced it is now gone in turn, and `firmware/UnitTestFw` was restored
# from history 2026-08-28). Re-allowlisted 2026-08-28 -- see the allowlist
# entries below for why migrating them instead is not worth doing for a
# restore-only fixture.
#
# One duplicate remains allowlisted:
#   - tools/PcTools/src/kilnctrl/crc16.py
#     (a deliberate, documented pure-Python port -- see its own docstring,
#     "Bit-for-bit port of crc16_ccitt_false() in uart_protocol.c" -- since
#     pc_tools can't link a C library; tracked by the "pc_tools consuming
#     the same vectors as the third implementation" item)
#
#     kilnctrl.protocol used to carry this port directly; it now imports
#     crc16_ccitt_false from kilnctrl.crc16 and re-exports it.
#
# DESIGN CHOICE: this check does NOT fail on that known, already-tracked
# debt today. Making it fail loudly right now would just be a second,
# redundant way of saying what ROADMAP.md M2 already says explicitly, and
# would leave the script permanently red (and therefore ignorable) until
# that one remaining item lands. Instead the known file is an explicit
# allowlist below, so:
#   - this check PASSES today, and stays wired to CI-able ahead of time
#   - it FAILS the moment anyone adds a *second* file matching the CRC
#     polynomial, which is the actual failure mode this check exists to
#     catch (an undetected, un-tracked new duplicate)
#   - deleting an allowlist entry is required as part of finishing the
#     migration item above, so the allowlist can't quietly go stale: once
#     protocol.py stops computing its own CRC, its grep hit disappears
#     and the entry becomes dead weight that the next run of this reasoning
#     would flag in review.
#
# Usage: powershell -File tools\check_no_duplicate_crc.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

# Directories never scanned: build output, vendored/managed deps, venvs,
# and throwaway agent worktrees. `.claude/worktrees/` holds full, disposable
# copies of the repo used by background coding agents; they can contain
# stale copies of files that have since been changed or deleted upstream; a
# CRC hit inside one is not a real source hit and would just make this
# check's output misleading noise.
$excludeDirs = @(
    '\\build\\',
    '\\managed_components\\',
    '\\\.venv\\',
    '\\node_modules\\',
    '\\components\\lvgl\\',
    '\\\.claude\\worktrees\\'
)

# Known, already-tracked pre-migration duplicates (see header above).
# Paths are relative to repo root, forward-slash, case-insensitive compare.
$allowlist = @(
    'tools/PcTools/src/kilnctrl/crc16.py',

    # 2026-08-27. This one is a deliberate, permanent second implementation,
    # and it is the opposite of the hazard this check exists for. The file is
    # the byte-identity proof for uart_protocol.c's delegation to CommonFW: it
    # embeds a frozen copy of the CRC/stuffing code that used to live in
    # uart_protocol.c and diffs it byte-for-byte against
    # kilnlink_crc16_ccitt_false()/kilnlink_stuff() over the payloads that
    # actually distinguish framing implementations (empty, containing 0x7E,
    # containing 0x7D, both adjacent, maximum length). A test that proves two
    # implementations agree necessarily contains two implementations. Removing
    # this entry means the proof was deleted, which is the thing to ask about
    # -- not a migration finishing.
    'firmware/KilnFW/App/test/test_uart_protocol_link_delegate.c',

    # 2026-08-28: re-entered, not new. See the header's "KNOWN PRE-EXISTING
    # DEBT" note above -- these two were allowlisted here before, removed
    # when firmware/UnitTestFw was deleted, and the restore explicitly left
    # them off the list ("Not fixed here; restore-only"). UnitTestFw is a
    # standalone legacy fixture restored from git history and deliberately
    # not built or maintained forward, so migrating its fork of
    # uart_protocol.c to delegate to kilnlink (the way KilnFW's real one
    # does) would mean actively maintaining code this project chose not to
    # carry forward. Deleting either entry is required if UnitTestFw is ever
    # migrated or removed again, same discipline as every other entry here.
    'firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.c',
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
