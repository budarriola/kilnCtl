# check_heat_enable_wiring.ps1 -- every path in KilnFW that can command heat
# must ASK the safety processor to permit it (heat_enable_acquire) and must
# GIVE THAT PERMISSION BACK (heat_enable_release).
#
# WHY THIS EXISTS. Found 2026-08-29, on a bench with a live heating element.
# The ONLY callers of safety_link_request_enable() in the entire ESP firmware
# were danger_mode.c (the manual diagnostics bypass) and uart_bridge.c's raw
# safety_request_enable wire command. profile_executor.c and
# autotune_engine.c -- the two paths an operator actually fires or tunes a
# kiln with -- never called it at all. Both closed their own zone relay (K1)
# through kiln_io_owner and left K4 on the safety processor open, so no
# element current could flow. Every firing and every autotune this firmware
# had ever run measured a heater that was never energized; the symptom was
# read for weeks as "this jig barely responds" and once as "the trace is flat
# or noise-dominated", i.e. as a verdict on the kiln rather than on the
# controller's own inaction.
#
# That is exactly the shape of defect this repository has now hit repeatedly
# and has a name for -- a consumer with no producer, a feature that bypasses
# its owner module -- and the reason it survived so long is that NOTHING in
# the test suite or the guard scripts could tell the difference between "the
# request was made and the kiln is slow" and "the request was never made".
# The host tests now cover the release paths and heat_enable.c's own
# semantics (test_heat_enable.c, test_profile_executor_prestart.c,
# test_autotune_engine_prestart.c). They CANNOT cover
# profile_executor_run()'s acquire, which sits past a full task/config/
# thermocouple harness those files deliberately never build. This script is
# what covers that one: a source-level assertion that the call is present.
#
# WHAT THIS CHECKS, per heat-commanding module (profile_executor.c,
# autotune_engine.c):
#   1. it calls heat_enable_acquire() at least once;
#   2. it calls heat_enable_release() at least once;
#   3. it does NOT call safety_link_request_enable() directly -- the whole
#      point of heat_enable.c is that this logic (check the result, track our
#      own request, release on every exit) exists once, not once per caller.
#      danger_mode.c and uart_bridge.c are the two deliberate exceptions and
#      are not scanned: danger_mode is the manual bypass whose entire purpose
#      is to stand outside the firing paths, and uart_bridge is the raw wire
#      command an operator/PcTools issues explicitly.
#   4. heat_enable.c itself really does call safety_link_request_enable() in
#      both directions -- the blindness guard, so a rename or a gutted module
#      cannot make checks 1-3 pass over calls that no longer reach the wire.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_heat_enable_wiring.ps1
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$driversDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers"

if (-not (Test-Path $driversDir)) {
    throw "check_heat_enable_wiring: $driversDir not found -- the tree moved and this check has gone blind."
}

$violations = @()

# ---- checks 1-3: the heat-commanding modules ------------------------------
$heatModules = @("profile_executor.c", "autotune_engine.c")
foreach ($name in $heatModules) {
    $path = Join-Path $driversDir $name
    if (-not (Test-Path $path)) {
        $violations += "$name -- file not found; it was renamed or removed and this check has gone blind."
        continue
    }
    $text = Get-Content -Raw -Path $path

    # Calls only, never the word inside a comment: require the open paren and
    # exclude a line whose first non-space characters are a comment marker.
    $lines = Get-Content -Path $path
    $acquireCalls = @($lines | Where-Object { $_ -match 'heat_enable_acquire\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })
    $releaseCalls = @($lines | Where-Object { $_ -match 'heat_enable_release\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })
    $directCalls  = @($lines | Where-Object { $_ -match 'safety_link_request_enable\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })

    if ($acquireCalls.Count -lt 1) {
        $violations += "$name -- no heat_enable_acquire() call. This module can command heat; without the request, K4 on the safety processor never closes and NO ELEMENT CURRENT FLOWS, while the run looks completely normal. This is the exact defect of 2026-08-29."
    }
    if ($releaseCalls.Count -lt 1) {
        $violations += "$name -- no heat_enable_release() call. Every exit path (completion, operator stop, pause, fault/trip) must give K4 back; a request left standing outlives the run that made it."
    }
    if ($directCalls.Count -ge 1) {
        $violations += "$name -- calls safety_link_request_enable() directly ($($directCalls.Count) site(s)). Go through heat_enable.c instead: checking the return value, tracking this side's own outstanding request, and releasing on every exit are three things that have each been got wrong once already, and they belong in one place."
    }
    if (-not ($text -match '#include\s+"heat_enable\.h"')) {
        $violations += "$name -- does not include heat_enable.h."
    }
}

# ---- check 4: the blindness guard -----------------------------------------
$hePath = Join-Path $driversDir "heat_enable.c"
if (-not (Test-Path $hePath)) {
    throw "check_heat_enable_wiring: heat_enable.c not found -- the module this check is built around is gone. Update this check before trusting its result."
}
$heLines = Get-Content -Path $hePath
$heWireCalls = @($heLines | Where-Object { $_ -match 'safety_link_request_enable\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })
$heTrue  = @($heWireCalls | Where-Object { $_ -match 'request_enable\s*\(\s*[A-Za-z0-9_.\->]+\s*,\s*true\s*\)' })
$heFalse = @($heWireCalls | Where-Object { $_ -match 'request_enable\s*\(\s*[A-Za-z0-9_.\->]+\s*,\s*false\s*\)' })
if ($heTrue.Count -lt 1) {
    $violations += "heat_enable.c -- no safety_link_request_enable(..., true) call. The module every heat path now routes through does not actually ask for heat; checks 1-3 above would pass over calls that reach nothing."
}
if ($heFalse.Count -lt 1) {
    $violations += "heat_enable.c -- no safety_link_request_enable(..., false) call. Nothing ever gives K4 back."
}

if ($violations.Count -gt 0) {
    Write-Host "HEAT-ENABLE WIRING CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "  A run that commands heat without requesting it from the safety processor" -ForegroundColor Red
    Write-Host "  is indistinguishable, from the outside, from a kiln that will not heat." -ForegroundColor Red
    Write-Host "  See firmware/KilnFW/App/drivers/heat_enable.h." -ForegroundColor Red
    throw "$($violations.Count) heat-enable wiring violation(s)"
}

Write-Host "Heat-enable wiring check passed: profile_executor.c and autotune_engine.c both acquire and release through heat_enable.c ($($heTrue.Count) enable / $($heFalse.Count) release wire call(s) there), and neither calls safety_link_request_enable() directly."
exit 0
