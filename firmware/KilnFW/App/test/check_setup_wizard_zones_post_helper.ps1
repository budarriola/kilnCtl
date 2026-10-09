# check_setup_wizard_zones_post_helper.ps1 -- every POST /api/zones from
# setup_wizard_page.html must go through submitZonesConfig()/
# zoneToPostParams(), never a second hand-built params list.
#
# WHY THIS EXISTS. zones_http_post_parse.c is a WHOLE-PAGE-SUBMIT parser:
# most per-zone fields (relay_mask, kp/ki/kd, ramp, sanity, mode, maxtemp,
# mintemp, window, minon, minoff, timingprofile) are REQUIRED on every POST,
# and several more -- critically z%u_k/z%u_tau/z%u_deadtime, the measured
# plant-identification model -- are OPTIONAL but OMIT-DELETES: absence is
# read as "no model" and zeroes a value that took real bench time to
# measure (see that file's own comment on those three fields). Steps 2 and
# 3 of the setup wizard were originally found (2026-09-08 hazard report)
# hand-building a short params list containing only the field or two each
# screen actually edits (z%u_name/z%u_thermo_mask for step 2,
# z%u_tctype/z%u_cal for step 3) and POSTing that alone -- against a real
# board this would 400 on the first missing REQUIRED field, or worse,
# silently wipe every other zone's model identification, relay mask, PID
# gains, etc. the moment an operator ran the wizard after a bench tune.
#
# THE FIX applied in the same pass this check was added: steps 2/3 now
# build a `mergedZones` array (GET-response zone objects with only the
# screen's own edited fields overridden via Object.assign, the same pattern
# steps 4/5/6 already used) and call submitZonesConfig(dataCopy, mergedZones)
# -- the ONE helper that echoes every REQUIRED and OMIT-DELETES field back
# from the live GET via zoneToPostParams(), so no submission path has to
# reason field-by-field about which keys are safe to leave out.
#
# WHAT THIS CHECKS. Counts every occurrence of `fetch('/api/zones'` in
# setup_wizard_page.html (the only file this wizard's JS lives in -- it is
# not split across modules). There must be EXACTLY ONE, and it must fall
# inside the body of function submitZonesConfig(...) -- i.e. between that
# function's opening `{` and the file's next top-level `function ` after it.
# A second occurrence anywhere else in the file means some step reverted to
# (or newly introduced) its own hand-built whole-page POST, bypassing the
# helper and reopening this exact hazard.
#
# Usage: bash-run via firmware/KilnFW/App/test/build_host_tests.ps1's own
# convention, or directly: powershell -File check_setup_wizard_zones_post_helper.ps1
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$pageFile = Join-Path $testDir "..\drivers\http\setup_wizard_page.html"
if (-not (Test-Path $pageFile)) {
    throw "check_setup_wizard_zones_post_helper: $pageFile not found -- has setup_wizard_page.html moved? This check is now blind."
}
$pageFileResolved = (Resolve-Path $pageFile).Path
$lines = Get-Content -Path $pageFileResolved

$fetchLineIdxs = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "fetch\('/api/zones'") {
        $fetchLineIdxs += $i
    }
}

if ($fetchLineIdxs.Count -eq 0) {
    throw "check_setup_wizard_zones_post_helper: found ZERO fetch('/api/zones' calls in $pageFileResolved -- has submitZonesConfig() been renamed or removed? This check would pass vacuously if it just skipped an empty result."
}

if ($fetchLineIdxs.Count -gt 1) {
    $where = ($fetchLineIdxs | ForEach-Object { "line $($_ + 1)" }) -join ", "
    throw "check_setup_wizard_zones_post_helper: found $($fetchLineIdxs.Count) fetch('/api/zones' call sites ($where) -- expected exactly 1, inside submitZonesConfig(). A step is hand-building its own whole-page POST again, bypassing zoneToPostParams()/submitZonesConfig() and reopening the omit-deletes/omit-required hazard on z%u_k/z%u_tau/z%u_deadtime and friends. Route it through submitZonesConfig() instead."
}

# Confirm the single call site is actually inside submitZonesConfig(), not
# some other function that happens to share the fetch pattern.
$fetchIdx = $fetchLineIdxs[0]
$funcStart = -1
for ($i = $fetchIdx; $i -ge 0; $i--) {
    if ($lines[$i] -match "^function\s+(\w+)\s*\(") {
        $funcStart = $i
        break
    }
}
if ($funcStart -lt 0) {
    throw "check_setup_wizard_zones_post_helper: could not find an enclosing top-level function for the fetch('/api/zones' call at line $($fetchIdx + 1) -- this check's function-boundary scan has gone blind."
}
if ($lines[$funcStart] -notmatch "^function\s+submitZonesConfig\s*\(") {
    throw "check_setup_wizard_zones_post_helper: the sole fetch('/api/zones' call at line $($fetchIdx + 1) is inside '$($lines[$funcStart])', not submitZonesConfig() -- the helper has been renamed/restructured in a way this check no longer recognizes, or a call was moved out of the shared helper."
}

Write-Host "Setup wizard zones-POST helper check passed: exactly 1 fetch('/api/zones' call site, inside submitZonesConfig() (line $($fetchIdx + 1))."
exit 0
