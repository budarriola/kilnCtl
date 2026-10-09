# checkcache: ok
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
# WHAT THIS CHECKS, per heat-commanding module (profile_executor*.c,
# autotune_engine.c):
#
# 2026-09-01 update: profile_executor.c (4962 lines) was split into
# profile_executor.c plus eight siblings (profile_executor_run.c,
# _relay_io.c, _firing_stats.c, _config_reload.c, _status.c, _start.c,
# _feedforward.c, _pid_tick.c). The acquire/release call sites moved with
# their functions -- heat_enable_acquire() now lives in
# profile_executor_run.c and profile_executor_status.c, heat_enable_release()
# in profile_executor.c, profile_executor_relay_io.c and
# profile_executor_status.c. Scanning only profile_executor.c after that
# split would still find a release call (one is left there) and go on
# reporting green while missing that the acquire call left the file
# entirely -- silently blind to the one defect this script exists to catch.
# So the profile_executor module is scanned as the whole profile_executor*.c
# file set, not a single hardcoded name, the same fix already applied to
# check_bridge_reject_reason.ps1 for the uart_bridge.c split. autotune_engine.c
# was not split and stays a single file.
#   1. it calls heat_enable_acquire() or heat_enable_acquire_since() at
#      least once;
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
#        (-DriversDir <path> to smoke-test against a simulated tree)
param(
    [string]$DriversDir
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
if ($DriversDir) {
    $driversDir = $DriversDir
} else {
    $driversDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers"
}

if (-not (Test-Path $driversDir)) {
    throw "check_heat_enable_wiring: $driversDir not found -- the tree moved and this check has gone blind."
}
$driversDir = (Resolve-Path $driversDir).Path

$violations = @()

# ---- checks 1-3: the heat-commanding modules ------------------------------
# Each entry is a module name paired with the glob that resolves its file
# set. profile_executor uses a glob (see the 2026-09-01 split note above).
# autotune_engine.c (also 1500+ lines) was split the same way on 2026-09-04
# into autotune_engine.c plus autotune_engine_coupling.c/_guard.c/_relay.c/
# _step_identify.c; heat_enable_release() moved to autotune_engine_guard.c
# while heat_enable_acquire() stayed in autotune_engine.c. Scanning only
# autotune_engine.c after that split would still find the acquire call and
# report green while missing that the release call left the file -- the same
# blindness this script's header already documents for profile_executor's
# split, so autotune_engine gets the same glob treatment now.
$heatModules = @(
    @{ Name = "profile_executor"; Glob = "profile_executor*.c"; MinFiles = 2 },
    @{ Name = "autotune_engine";  Glob = "autotune_engine*.c";  MinFiles = 2 }
)
foreach ($mod in $heatModules) {
    $name = $mod.Name
    $files = @(Get-ChildItem -Path $driversDir -Filter $mod.Glob -File -Recurse)
    if ($files.Count -lt $mod.MinFiles) {
        $violations += "$name -- expected at least $($mod.MinFiles) file(s) matching '$($mod.Glob)' under $driversDir, found $($files.Count). It was renamed, removed, or merged back down and this check has gone blind."
        continue
    }

    $acquireCalls = @()
    $releaseCalls = @()
    $directCalls  = @()
    $hasInclude   = $false
    foreach ($f in $files) {
        # Calls only, never the word inside a comment: require the open
        # paren and exclude a line whose first non-space characters are a
        # comment marker.
        $lines = Get-Content -Path $f.FullName
        # heat_enable_acquire() and heat_enable_acquire_since() both count:
        # since 2026-09-15 the epoch-checked form is the one the firing paths
        # actually call (heat_enable_acquire() is now a thin wrapper that
        # samples an epoch and spends it). Matching only the bare name would
        # have reported both modules unwired the moment the call sites moved
        # to the _since spelling -- a false alarm, but the same regex would
        # equally have gone blind had the wrapper been the one retired.
        $acquireCalls += @($lines | Where-Object { $_ -match 'heat_enable_acquire(_since)?\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })
        $releaseCalls += @($lines | Where-Object { $_ -match 'heat_enable_release\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })
        $directCalls  += @($lines | Where-Object { $_ -match 'safety_link_request_enable\s*\(' -and $_ -notmatch '^\s*(\*|//|/\*)' })
        if ((Get-Content -Raw -Path $f.FullName) -match '#include\s+"heat_enable\.h"') {
            $hasInclude = $true
        }
    }

    if ($acquireCalls.Count -lt 1) {
        $violations += "$name (scanned $($files.Count) file(s) matching '$($mod.Glob)') -- no heat_enable_acquire()/heat_enable_acquire_since() call. This module can command heat; without the request, K4 on the safety processor never closes and NO ELEMENT CURRENT FLOWS, while the run looks completely normal. This is the exact defect of 2026-08-29."
    }
    if ($releaseCalls.Count -lt 1) {
        $violations += "$name (scanned $($files.Count) file(s) matching '$($mod.Glob)') -- no heat_enable_release() call. Every exit path (completion, operator stop, pause, fault/trip) must give K4 back; a request left standing outlives the run that made it."
    }
    if ($directCalls.Count -ge 1) {
        $violations += "$name -- calls safety_link_request_enable() directly ($($directCalls.Count) site(s)) across $($mod.Glob). Go through heat_enable.c instead: checking the return value, tracking this side's own outstanding request, and releasing on every exit are three things that have each been got wrong once already, and they belong in one place."
    }
    if (-not $hasInclude) {
        $violations += "$name -- no file matching '$($mod.Glob)' includes heat_enable.h."
    }
}

# ---- check 4: the blindness guard -----------------------------------------
$heMatches = @(Get-ChildItem -Path $driversDir -Filter "heat_enable.c" -File -Recurse)
if ($heMatches.Count -eq 0) {
    throw "check_heat_enable_wiring: heat_enable.c not found anywhere under $driversDir -- the module this check is built around is gone. Update this check before trusting its result."
}
if ($heMatches.Count -gt 1) {
    $paths = ($heMatches | ForEach-Object { $_.FullName }) -join ", "
    throw "check_heat_enable_wiring: heat_enable.c matched more than one file under $driversDir ($paths) -- cannot tell which one is the real module."
}
$hePath = $heMatches[0].FullName
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
    Write-Host "  See firmware/KilnFW/App/drivers/control/heat_enable.h." -ForegroundColor Red
    throw "$($violations.Count) heat-enable wiring violation(s)"
}

Write-Host "Heat-enable wiring check passed: profile_executor.c and autotune_engine.c both acquire and release through heat_enable.c ($($heTrue.Count) enable / $($heFalse.Count) release wire call(s) there), and neither calls safety_link_request_enable() directly."
exit 0
