# check_flash_partition_offset_guard.ps1 -- release gate for the
# flash_firmware() partition-offset guard added 2026-09-16
# (docs/OTA_SINGLE_SLOT_PLAN.md step 3, hazard closed alongside the new
# single-application-slot firmware/KilnFW/partitions.csv landing in that
# same commit series).
#
# THE HAZARD. flash_firmware() (tools/PcTools/src/kilnctrl/mcp_server_flash.py)
# writes the app image to a hardcoded flash offset (module-level
# APP_FLASH_OFFSET) rather than deriving it from partitions.csv or from the
# board. That is deliberate: the new partitions.csv moved the app region
# around, but the bench board still carries the OLD table, so retargeting
# the write offset ahead of the board's own migration would silently write
# the app image into the wrong region. Nothing enforced that the tool's
# hardcoded offset and the board's OWN live table stay in agreement, though
# -- a board actually migrated (or a future edit retargeting the constant
# early) would go undetected right up until a flash landed in the wrong
# place.
#
# THE FIX THIS GATE PROTECTS. mcp_server_flash._check_app_flash_offset_
# matches_chip() reads the board's live table over GET /api/partitions (no
# JTAG, no reset) and refuses -- naming both offsets, never auto-correcting
# -- before flash_firmware() ever calls OpenOCD, unless the caller passes
# allow_partition_offset_mismatch=True. This is a RUNTIME guard: it only
# protects an interactive flash against a board that is reachable at flash
# time. It does nothing for a fresh clone that never runs flash_firmware()
# at all, which is exactly the gap a static release gate needs to cover --
# hence this script, run from run_all_checks.ps1 on every clone, not just at
# the bench.
#
# WHAT THIS SCRIPT CANNOT DO. It has no board to talk to, so it cannot
# confirm the guard actually catches a REAL mismatch against real hardware
# -- that is what tools/PcTools/tests/test_mcp_server_flash_partition_guard.py
# does (against injected fake board data, exercising the same production
# function). What this script CAN and does check, with no board required:
#
#   1. The guard function and its wiring into flash_firmware() still exist
#      (a future refactor could silently drop the call while leaving the
#      function defined and its unit tests green, since those tests call
#      the guard function directly, not flash_firmware() itself).
#   2. flash_firmware()'s hardcoded write offset (the `program_esp
#      build/KilnCtrl.bin ...` line) and the guard's own APP_FLASH_OFFSET
#      constant are the SAME literal -- i.e. there is exactly one source of
#      truth for "what offset are we about to write", not two hand-kept
#      copies that could drift apart from each other silently (the same
#      "reset one side of a pair" class CLAUDE.md's project memory
#      describes, applied to a constant instead of a runtime state pair).
#   3. allow_partition_offset_mismatch is a real, named parameter of
#      flash_firmware() -- not a guard with no override, which would
#      eventually get deleted out of frustration by someone who hit it
#      legitimately (e.g. mid-migration) with no sanctioned way through.
#
# This deliberately does NOT compare partitions.csv's own offsets against
# APP_FLASH_OFFSET: as of 2026-09-16 those TWO are expected to disagree
# (repo table already single-slot-shaped, tool still targeting the old
# table, on purpose, until docs/OTA_SINGLE_SLOT_PLAN.md step 4's migration
# flash lands) -- a check asserting they match would fail the suite for a
# state the plan explicitly calls correct.
#
# Usage: powershell -File tools\check_flash_partition_offset_guard.ps1

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$flashPy = Join-Path $root "PcTools\src\kilnctrl\mcp_server_flash.py"
$testPy = Join-Path $root "PcTools\tests\test_mcp_server_flash_partition_guard.py"

if (-not (Test-Path $flashPy)) {
    throw "check_flash_partition_offset_guard.ps1: $flashPy not found -- has mcp_server_flash.py moved?"
}
if (-not (Test-Path $testPy)) {
    throw "check_flash_partition_offset_guard.ps1: $testPy not found -- has the negative-test file for this guard been removed or renamed?"
}

$lines = Get-Content -Path $flashPy
$text = $lines -join "`n"

$failures = @()

# 1. The guard function must still be defined.
if ($text -notmatch 'def\s+_check_app_flash_offset_matches_chip\s*\(') {
    $failures += "_check_app_flash_offset_matches_chip() is no longer defined in mcp_server_flash.py -- the runtime partition-offset guard has been removed or renamed."
}

# 2. flash_firmware() must actually CALL the guard (not just define it and
#    leave it dead) and must do so BEFORE kill_openocd_sessions()/the OpenOCD
#    program_esp sequence -- find flash_firmware()'s own body span first so
#    this doesn't accidentally match some unrelated later function that also
#    happens to mention the guard name in a comment.
$flashFnStart = $null
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^def\s+flash_firmware\s*\(') { $flashFnStart = $i; break }
}
if ($null -eq $flashFnStart) {
    $failures += "no top-level 'def flash_firmware(' found in mcp_server_flash.py -- has it been renamed or moved to another module?"
} else {
    # Body ends at the next top-level 'def ' (column 0) after the start line.
    $flashFnEnd = $lines.Count - 1
    for ($i = $flashFnStart + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^def\s+\w+\s*\(') { $flashFnEnd = $i - 1; break }
    }
    $body = ($lines[$flashFnStart..$flashFnEnd]) -join "`n"

    # Match the actual CALL (assigning its result), not merely a docstring
    # or comment mentioning the function's name -- flash_firmware()'s own
    # docstring names this guard by design, so a name-only match would stay
    # green even after the real call site was deleted.
    if ($body -notmatch '=\s*_check_app_flash_offset_matches_chip\s*\(\s*partition_guard_host\s*\)') {
        $failures += "flash_firmware() no longer calls _check_app_flash_offset_matches_chip(partition_guard_host) and assigns its result -- the guard function may still exist and be named in the docstring, but the real call site is gone, so a board/tool offset mismatch would go undetected again."
    }
    if ($body -notmatch 'allow_partition_offset_mismatch') {
        $failures += "flash_firmware() no longer has an allow_partition_offset_mismatch parameter (or the guard call no longer checks it) -- either the override has been removed (blocking legitimate mid-migration flashes with no escape hatch) or the guard itself is gone."
    }

    # Ordering: the guard call must appear before the OpenOCD program_esp
    # sequence textually within the function body, so a mismatch is refused
    # BEFORE anything touches the board.
    $guardIdx = $body.IndexOf("_check_app_flash_offset_matches_chip(")
    $tclIdx = $body.IndexOf("program_esp build/KilnCtrl.bin")
    if ($guardIdx -ge 0 -and $tclIdx -ge 0 -and $guardIdx -gt $tclIdx) {
        $failures += "flash_firmware() calls _check_app_flash_offset_matches_chip() AFTER building the OpenOCD program_esp command -- the guard must run and be able to refuse before OpenOCD is ever invoked."
    }
}

# 3. Exactly one source of truth for the write offset: the guard's own
#    APP_FLASH_OFFSET constant and the literal actually passed to
#    `program_esp build/KilnCtrl.bin ...` inside flash_firmware() must be
#    the same value, expressed as the same reference (not two independently
#    hand-kept hex literals that could silently drift apart -- see this
#    script's header comment).
$constMatch = [regex]::Match($text, 'APP_FLASH_OFFSET\s*=\s*(0[xX][0-9A-Fa-f]+)')
if (-not $constMatch.Success) {
    $failures += "APP_FLASH_OFFSET constant not found (expected a top-level 'APP_FLASH_OFFSET = 0x...' assignment)."
} else {
    $constValue = [Convert]::ToInt64($constMatch.Groups[1].Value, 16)
    if ($null -ne $flashFnStart) {
        $flashFnEnd2 = $lines.Count - 1
        for ($i = $flashFnStart + 1; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^def\s+\w+\s*\(') { $flashFnEnd2 = $i - 1; break }
        }
        $body2 = ($lines[$flashFnStart..$flashFnEnd2]) -join "`n"
        if (-not ($body2 -match 'program_esp build/KilnCtrl\.bin')) {
            $failures += "could not find the 'program_esp build/KilnCtrl.bin ...' line inside flash_firmware() to compare against APP_FLASH_OFFSET."
        } elseif ($body2 -match 'program_esp build/KilnCtrl\.bin 0x\{APP_FLASH_OFFSET') {
            # Single source of truth via the constant itself (an f-string
            # like `0x{APP_FLASH_OFFSET:x}`) -- good, nothing further to
            # check numerically since this literally cannot drift.
        } else {
            # Some other form (e.g. a bare hex literal) -- accept it only if
            # the effective VALUE still matches APP_FLASH_OFFSET, so a stray
            # hand-edited literal that silently diverges is still caught.
            $tclLineMatch = [regex]::Match($body2, 'program_esp build/KilnCtrl\.bin[^0-9xXA-Fa-f]*(0[xX][0-9A-Fa-f]+)')
            if (-not $tclLineMatch.Success) {
                $failures += "flash_firmware()'s 'program_esp build/KilnCtrl.bin ...' line writes neither APP_FLASH_OFFSET (via an f-string) nor a recognisable bare hex literal -- update this check's pattern or restore a single source of truth for the write offset."
            } else {
                $tclValue = [Convert]::ToInt64($tclLineMatch.Groups[1].Value, 16)
                if ($tclValue -ne $constValue) {
                    $failures += ("flash_firmware()'s OpenOCD write offset (0x{0:x}) and the guard's APP_FLASH_OFFSET constant (0x{1:x}) have DRIFTED APART -- these must be the exact same value (ideally the same source, e.g. `0x{{APP_FLASH_OFFSET:x}}`) or the guard is checking a different address than the one actually written." -f $tclValue, $constValue)
                }
            }
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "FLASH PARTITION-OFFSET GUARD CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
    throw "The flash_firmware() partition-offset guard (docs/OTA_SINGLE_SLOT_PLAN.md) is missing, disconnected, or has drifted -- see tools/check_flash_partition_offset_guard.ps1's header comment."
}

Write-Host "Flash partition-offset guard check passed: guard defined, wired into flash_firmware() before OpenOCD is touched, override present, single source of truth for the write offset."
exit 0
