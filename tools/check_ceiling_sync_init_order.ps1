# check_ceiling_sync_init_order.ps1 -- safety_ceiling_sync_init() must be
# called in main_control_bringup() BEFORE safety_link_start().
#
# WHY THIS EXISTS. safety_ceiling_sync.c's two mutexes
# (s_divergence_state_lock, s_reconcile_lock) are statically allocated and
# created once, from safety_ceiling_sync_init(). Every take/give site in
# that file is guarded by `if (lock)`, so a handle that is still NULL does
# not assert -- it silently runs the critical section UNLOCKED. That makes a
# mis-ordered init invisible: no crash, no log, just no serialization.
#
# WHY IT MATTERS. safety_link_start() creates safety_poll_task, which calls
# safety_ceiling_sync_reconcile_on_link_up_nonblocking() on the first tick
# the link is up -- main_control_bringup.c's own heat-hook comment already
# makes this point about that same window. kiln_cfg_swap_worker reaches the
# blocking entry point from another task, on the other core. If init runs
# after safety_link_start(), those first ticks run with both handles NULL,
# which is exactly the unserialized state the static-mutex fix (2026-09-22)
# was written to remove. The fix shipped that way once and was caught in
# review.
#
# WHAT THIS CHECKS. In firmware/KilnFW/App/main_control_bringup.c, with
# comments stripped, a call to safety_ceiling_sync_init() must exist and
# must appear on an earlier line than the call to safety_link_start().
#
# Usage: powershell -File tools\check_ceiling_sync_init_order.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root "firmware\KilnFW\App\main_control_bringup.c"
if (-not (Test-Path $src)) {
    throw "check_ceiling_sync_init_order: $src not found -- has it moved or been split? This check is now blind."
}

# Comment stripper -- duplicated from tools/check_volatile_ceiling_write_callers.ps1
# (this project has no shared PowerShell module mechanism). Both call names
# are discussed at length in comments in this very file, so stripping is not
# optional here.
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $result = @()
    foreach ($line in (Get-Content -Path $Path)) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) { $code = $code.Substring($endIdx + 2); $inBlockComment = $false }
            else { $result += ""; continue }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) { $code = $code.Substring(0, $lineCommentIdx) }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) { $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2) }
            else { $code = $code.Substring(0, $startIdx); $inBlockComment = $true; break }
        }
        $result += $code
    }
    return $result
}

$lines = Get-CodeOnlyLines -Path $src
$initLine = -1
$startLine = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($initLine -lt 0 -and $lines[$i] -match "safety_ceiling_sync_init\s*\(") { $initLine = $i + 1 }
    if ($startLine -lt 0 -and $lines[$i] -match "safety_link_start\s*\(") { $startLine = $i + 1 }
}

$failures = @()
if ($initLine -lt 0) {
    $failures += "safety_ceiling_sync_init() is not called in main_control_bringup.c at all -- safety_ceiling_sync.c's two mutexes would stay NULL for the whole boot, running every critical section unlocked and silently."
}
if ($startLine -lt 0) {
    $failures += "safety_link_start() is not called in main_control_bringup.c -- this check can no longer locate the task-creating call it orders against, so it is blind rather than passing."
}
if ($initLine -ge 0 -and $startLine -ge 0 -and $initLine -gt $startLine) {
    $failures += "safety_ceiling_sync_init() is called at line $initLine, AFTER safety_link_start() at line $startLine. safety_link_start() creates safety_poll_task, which reaches safety_ceiling_sync's reconcile entry point on its first link-up tick -- those ticks would run with both mutex handles still NULL (every take site is `if (lock)`-guarded, so this fails silently, not loudly). Move the safety_ceiling_sync_init() call above safety_link_start()."
}

if ($failures.Count -gt 0) {
    Write-Output "CEILING SYNC INIT ORDER CHECK: FAILED"
    foreach ($f in $failures) { Write-Output "  $f" }
    exit 1
}

Write-Output "CEILING SYNC INIT ORDER CHECK: PASSED (safety_ceiling_sync_init() at line $initLine, safety_link_start() at line $startLine)"
exit 0
