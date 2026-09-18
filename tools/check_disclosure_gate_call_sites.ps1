# check_disclosure_gate_call_sites.ps1 -- 2026-09-17 adversarial review of
# 6de75575, finding 2's residual gap.
#
# BACKGROUND. 6de75575 fixed two ROUTE_TIER_OPEN disclosure leaks
# (wifi_provision_http.c's GET /status, readiness_http.c's crash-report
# checklist item) and added 661 lines of host tests. A negative test proved
# those tests could not detect the production gate being deleted: replacing
# `bool may_disclose = !http_auth_policy_web_enabled() ||
# http_auth_caller_is_admin(req);` with `bool may_disclose = true;` at
# EITHER call site rebuilt all 54/54 host-test executables clean and
# passing. Root cause: both extracted helpers are pure and fully covered,
# but the tests compose `may_disclose` BY HAND inside the test file rather
# than exercising the real call site -- and neither handler's .c file can be
# host-compiled at all (wifi_provision_http.c: twelve GCC-only
# asm("_binary_...") blob externs plus <sys/socket.h>; readiness_http.c: two
# more asm("_binary_...") externs), so no host test can execute either call
# site directly, no matter how it's written.
#
# FIX, PART 1 (behavioural, not this script): the disjunction now lives in
# its own tiny, host-compilable translation unit,
# firmware/KilnFW/App/drivers/http/http_auth_disclosure_gate.c, exposing
# http_auth_may_disclose(httpd_req_t*). It is linked for REAL (never
# stubbed) into both kilnctl_host_tests_wifi_prov_status_disclosure.exe and
# kilnctl_host_tests_readiness_crash_disclosure.exe, so those tests now call
# the exact object code either handler calls, closing the "hand-composed
# copy in the test file" half of the gap. See that header's own comment for
# the full writeup.
#
# FIX, PART 2 (this script). That still leaves the actual call site inside
# wifi_provision_http.c/readiness_http.c unreachable by any host test --
# nothing stops a future edit from replacing
# `bool may_disclose = http_auth_may_disclose(req);` with a hardcoded
# `true` (or a re-inlined, silently reshaped disjunction) at either site,
# and no behavioural test anywhere in this tree can ever observe that,
# because the enclosing files are not host-compiled. This script is
# honestly a TEXT check, not a behavioural one -- it exists because, with
# both handlers closed to host compilation, no behavioural check can reach
# the call site at all. It is deliberately narrow (this exact pair of files,
# this exact call), same discipline as check_no_duplicate_crc.ps1: grep for
# a specific, meaningful token, not a broad heuristic that would also flag
# unrelated code.
#
# Usage: powershell -File tools\check_disclosure_gate_call_sites.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

# (repo-relative path, required substring) -- both must appear, verbatim,
# in the file for the gate to still be wired up.
$expectations = @(
    @{
        Path = "firmware/KilnFW/App/drivers/http/wifi_provision_http.c"
        Needle = 'bool may_disclose = http_auth_may_disclose(req);'
    },
    @{
        Path = "firmware/KilnFW/App/drivers/http/readiness_http.c"
        Needle = 'bool may_disclose = http_auth_may_disclose(req);'
    }
)

$failures = @()

foreach ($e in $expectations) {
    $full = Join-Path $root ($e.Path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        $failures += "$($e.Path): file not found"
        continue
    }
    $content = Get-Content -LiteralPath $full -Raw
    if ($content -notlike "*$($e.Needle)*") {
        $failures += "$($e.Path): expected call site `"$($e.Needle)`" not found -- the disclosure gate " +
            "may have been deleted, replaced with a hardcoded value, or re-inlined. See " +
            "http_auth_disclosure_gate.h's header comment."
        continue
    }
    # Belt-and-suspenders: also refuse an obviously-sabotaged hardcoded
    # `true`/`false` assignment to the same variable name anywhere in the
    # file, in case a future edit adds a SECOND, unguarded assignment
    # alongside a still-present real call (e.g. dead code, or an early
    # return path that bypasses the gate).
    if ($content -match 'bool\s+may_disclose\s*=\s*(true|false)\s*;') {
        $failures += "$($e.Path): found a hardcoded `"bool may_disclose = true/false;`" assignment -- " +
            "the disclosure gate must always be composed via http_auth_may_disclose(req)."
    }
}

if ($failures.Count -gt 0) {
    Write-Host "DISCLOSURE GATE CALL-SITE CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) disclosure-gate call site violation(s) found -- see tools\check_disclosure_gate_call_sites.ps1's header"
}

Write-Host "Disclosure gate call-site check passed: both wifi_provision_http.c and readiness_http.c still compose may_disclose via the real http_auth_may_disclose(req)."
Write-Host "  NOTE: this is a source-text check, not a behavioural one -- see this script's header comment for why (neither handler's .c file can be host-compiled)."

exit 0
