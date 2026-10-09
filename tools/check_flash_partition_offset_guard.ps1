# check_flash_partition_offset_guard.ps1 -- release gate for the
# flash_firmware() partition-offset guard.
#
# HISTORY. Originally (2026-09-16) this guarded a HARDCODED module-level
# APP_FLASH_OFFSET constant against drifting from the board's own live
# table. That constant itself was the root cause of a real incident: it
# pointed at the 1,966,080-byte `recovery`/factory slot at 0xA10000 instead
# of the `app` (ota_0) partition, so flash_firmware() was writing the main
# application image into the wrong region. The fix (see
# docs/OTA_SINGLE_SLOT_PLAN.md) replaced the hardcoded constant with
# per-flash resolution: _resolve_app_flash_target() reads
# <kiln_fw_root>/partitions.csv and returns the entry actually named `app`,
# by name (not by (type, subtype), which would have matched `recovery` too
# -- both are factory-subtype). There is now exactly one source of truth
# per flash -- the `app_target` PartitionEntry resolved once and threaded
# through the size check, the board-offset guard, and the OpenOCD command
# -- rather than a hand-kept constant that could silently diverge from the
# repo's own partition table.
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
#   2. flash_firmware() resolves app_target via _resolve_app_flash_target()
#      (partitions.csv, not a hardcoded constant) and threads that SAME
#      app_target into both the guard call and the `program_esp
#      build/KilnCtrl.bin ...` write offset -- i.e. there is exactly one
#      source of truth for "what offset are we about to write", not two
#      hand-kept copies that could drift apart from each other silently
#      (the same "reset one side of a pair" class CLAUDE.md's project
#      memory describes, applied to a resolved value instead of a runtime
#      state pair).
#   3. allow_partition_offset_mismatch is a real, named parameter of
#      flash_firmware() -- not a guard with no override, which would
#      eventually get deleted out of frustration by someone who hit it
#      legitimately (e.g. mid-migration) with no sanctioned way through.
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
    if ($body -notmatch '=\s*_check_app_flash_offset_matches_chip\s*\(\s*partition_guard_host\s*,\s*app_target\s*\)') {
        $failures += "flash_firmware() no longer calls _check_app_flash_offset_matches_chip(partition_guard_host, app_target) and assigns its result -- the guard function may still exist and be named in the docstring, but the real call site is gone, so a board/tool offset mismatch would go undetected again."
    }
    # The refusal branch itself: a mutation like `if False and partition_mismatch:` keeps
    # every name above present while neutering the guard, so require the bare branch.
    if ($body -notmatch '(?m)^\s*if\s+partition_mismatch\s*:\s*$') {
        $failures += "flash_firmware() has no bare 'if partition_mismatch:' refusal branch -- the guard result is computed but may no longer be able to refuse (e.g. short-circuited with 'False and ...')."
    }
    if ($body -notmatch 'app_target\s*=\s*_resolve_app_flash_target\s*\(') {
        $failures += "flash_firmware() no longer resolves app_target via _resolve_app_flash_target() -- the write offset must be derived from partitions.csv, not a hardcoded constant (this is the exact class of bug that put the app image in the wrong partition originally)."
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

# 3. Exactly one source of truth for the write offset: the value passed to
#    `program_esp build/KilnCtrl.bin ...` must come from the SAME app_target
#    object resolved by _resolve_app_flash_target(), not a second hand-kept
#    hex literal that could silently drift apart from it (see this script's
#    header comment and CLAUDE.md's "reset one side of a pair" bug class).
if ($null -ne $flashFnStart) {
    $flashFnEnd2 = $lines.Count - 1
    for ($i = $flashFnStart + 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^def\s+\w+\s*\(') { $flashFnEnd2 = $i - 1; break }
    }
    $body2 = ($lines[$flashFnStart..$flashFnEnd2]) -join "`n"
    if (-not ($body2 -match 'program_esp build/KilnCtrl\.bin')) {
        $failures += "could not find the 'program_esp build/KilnCtrl.bin ...' line inside flash_firmware()."
    } elseif ($body2 -notmatch 'program_esp build/KilnCtrl\.bin 0x\{app_target\.offset') {
        $failures += "flash_firmware()'s 'program_esp build/KilnCtrl.bin ...' line does not write 0x{app_target.offset:x} -- either it uses a second, independently hand-kept literal/constant that could drift from the partitions.csv-resolved value, or app_target has been renamed. Update this check or restore a single source of truth for the write offset."
    }

    # Also require the size pre-flight check: the build output must be
    # refused before OpenOCD if it does not fit app_target's own size, per
    # partitions.csv -- this is the check that closes the original
    # overflow-into-the-next-partition hazard.
    if ($body2 -notmatch 'app_bin_size\s*>\s*app_target\.size') {
        $failures += "flash_firmware() no longer refuses an oversized build (comparing against app_target.size) before flashing -- the pre-flight size guard against overflowing into the next partition appears to have been removed."
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
