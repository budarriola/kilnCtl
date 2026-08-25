# check_event_seq_monotonic.ps1 -- enforces the event ring's sequence counter
# staying monotonic for the whole boot (firmware) / process (virtual_simfw)
# lifetime.
#
# THE INVARIANT: an event's `seq` identifies it uniquely across every run in
# that lifetime. docs/PROTOCOL.md's RESET_SIM section is the contract; the
# reasoning lives at sim_engine.c's apply_reset().
#
# WHY IT NEEDS A GUARD RATHER THAN A HOST TEST: sim_engine.c pulls in
# FreeRTOS and pico-sdk headers and is not in build_host_tests.ps1's source
# list, so no host test can call apply_reset() and observe the counter. The
# host test that exists (test_sim_engine_event_seq_monotonic.c) pins a MIRROR
# of the logic and would keep passing if someone put the reset back in the
# real file. This script is the half that watches the real file. Same
# two-layer pattern as tools/check_bridge_reject_reason.ps1 in KilnFW.
#
# WHY IT REGRESSES SO EASILY: zeroing the counter on reset is the obvious,
# tidy-looking thing to write, and it was what the code did until 2026-08-24.
# The defect it caused was silent -- an EVT frame still in flight when a run
# ended was renumbered into a plausible member of the NEXT run and attributed
# to it. The bench only caught it because a straggler happened to land on a
# seq that showed up as a gap; a straggler numbered 0 or 1 would not have.
#
# COMMENTS ARE STRIPPED BEFORE MATCHING, deliberately: both checked files
# document the removed reset in prose, at length, right beside the code that
# no longer does it. A scan that did not strip comments would match its own
# subject's history and pass on a file that had genuinely regressed -- this
# repo has shipped eight structurally-unfailable checks that way.
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$simfwDir = Split-Path -Parent $toolsDir

function Get-CodeOnly {
    param([string]$Path)
    $raw = Get-Content -Raw -Path $Path
    $s = $raw -replace '(?s)/\*.*?\*/', ' '
    $s = $s -replace '(?m)//.*$', ' '
    return $s
}

$violations = @()

# --- firmware: src/tasks/sim_engine.c -------------------------------------
# s_ring_next_seq may be assigned 0 in exactly ONE place: its own definition.
# Anywhere else (apply_reset() being the historical offender) is a violation.
$enginePath = Join-Path $simfwDir "src\tasks\sim_engine.c"
$engine = Get-CodeOnly $enginePath
$engineZeroing = [regex]::Matches($engine, 's_ring_next_seq\s*=\s*0')
$engineDecl = [regex]::Matches($engine, 'static\s+uint32_t\s+s_ring_next_seq\s*=\s*0')
if ($engineDecl.Count -ne 1) {
    $violations += "sim_engine.c: expected exactly 1 definition of s_ring_next_seq initialized to 0, found $($engineDecl.Count) -- this check can no longer tell a definition from a reset, so it is not trustworthy as written"
}
if ($engineZeroing.Count -gt $engineDecl.Count) {
    $extra = $engineZeroing.Count - $engineDecl.Count
    $violations += "sim_engine.c: $extra assignment(s) of s_ring_next_seq to 0 outside its definition. The event ring's sequence counter must stay monotonic for the boot lifetime -- see apply_reset()'s comment and docs/PROTOCOL.md's RESET_SIM section."
}
# The counter must still be incremented somewhere, or 'monotonic' is vacuous.
if ($engine -notmatch 's_ring_next_seq\+\+') {
    $violations += "sim_engine.c: s_ring_next_seq is never incremented -- the monotonicity this check enforces would be vacuously true."
}

# --- reference model: tools/virtual_simfw/src/virtual_simfw.c -------------
# ring_next_seq starts at 0 only because g_dev is a zero-initialized static;
# there is no legitimate assignment of it to 0 anywhere.
$vsPath = Join-Path $simfwDir "tools\virtual_simfw\src\virtual_simfw.c"
$vs = Get-CodeOnly $vsPath
$vsZeroing = [regex]::Matches($vs, 'ring_next_seq\s*=\s*0')
if ($vsZeroing.Count -gt 0) {
    $violations += "virtual_simfw.c: $($vsZeroing.Count) assignment(s) of ring_next_seq to 0. The reference model must mirror the firmware's monotonic counter -- it starts at 0 because g_dev is a zero-initialized static, not because anything resets it."
}
if ($vs -match 'reset_client_evt_cursors') {
    $violations += "virtual_simfw.c: reset_client_evt_cursors() is back. It only existed to resync client cursors after the producer counter was zeroed; with a monotonic counter a cursor can never sit above the producer, and resetting cursors would replay history."
}
if ($vs -notmatch 'ring_next_seq\+\+') {
    $violations += "virtual_simfw.c: ring_next_seq is never incremented -- the monotonicity this check enforces would be vacuously true."
}

if ($violations.Count -gt 0) {
    foreach ($v in $violations) { Write-Host "  VIOLATION: $v" -ForegroundColor Red }
    throw "$($violations.Count) event-seq monotonicity violation(s)"
}

Write-Host "Event-seq monotonicity OK (sim_engine.c, virtual_simfw.c)." -ForegroundColor Green
exit 0
