# check_csrf_origin_wiring.ps1 -- source-level proof that the CSRF origin check
# (ROUTE_TIER_REVIEW MED-1, http_origin_check.h) is actually WIRED where it
# matters. The decision helpers have host tests; the recovery image
# (firmware/KilnFW_recovery) has no host harness, so its wiring is checked here.
# The main image's pre-handler wiring is host-tested (test_readiness_crash_disclosure.c,
# test_csrf_prehandler_wiring). Docs: docs/audits/HOST_TEST_GAP_AUDIT_2026-10-09.md gap 5.
#
# Asserts, comments stripped:
#   recovery_http.c: origin_guard() calls http_origin_request_is_cross_origin(),
#     refuses with 403 for methods other than GET/HEAD, and every registered
#     route is wrapped (`wrapped.handler = origin_guard`).
#   http_auth_http.c: kiln_http_prehandler() gates on request_is_cross_origin()
#     for non-GET/HEAD, before http_auth_check().
param([string]$Root)
$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Join-Path $PSScriptRoot ".." }
$Root = (Resolve-Path $Root).Path
function Get-Code($rel) {
    $p = Join-Path $Root $rel
    if (-not (Test-Path $p)) { throw "missing $rel" }
    $t = [IO.File]::ReadAllText($p)
    $t = [regex]::Replace($t, '/\*.*?\*/', '', 'Singleline')
    return [regex]::Replace($t, '//[^\r\n]*', '')
}
$fail = @()
$rec = Get-Code "firmware\KilnFW_recovery\main\recovery_http.c"
$m = [regex]::Match($rec, 'static\s+esp_err_t\s+origin_guard\s*\([^)]*\)\s*\{(?<b>.*?)\r?\n\}', 'Singleline')
if (-not $m.Success) { $fail += "recovery_http.c: origin_guard() not found" }
else {
    $b = $m.Groups['b'].Value
    if ($b -notmatch 'http_origin_request_is_cross_origin\s*\(') { $fail += "origin_guard() does not call http_origin_request_is_cross_origin()" }
    if ($b -notmatch 'HTTP_GET' -or $b -notmatch 'HTTP_HEAD') { $fail += "origin_guard() does not exempt exactly GET/HEAD" }
    if ($b -notmatch '403') { $fail += "origin_guard() does not answer 403" }
}
if ($rec -notmatch 'wrapped\.handler\s*=\s*origin_guard\s*;') { $fail += "recovery_http.c: routes are not wrapped with origin_guard" }
$auth = Get-Code "firmware\KilnFW\App\drivers\http\http_auth_http.c"
$pm = [regex]::Match($auth, 'kiln_http_prehandler\s*\(httpd_req_t\s*\*req\)\s*\{(?<b>.*)', 'Singleline')
if (-not $pm.Success) { $fail += "http_auth_http.c: kiln_http_prehandler() not found" }
else {
    $b = $pm.Groups['b'].Value
    $i = $b.IndexOf('request_is_cross_origin(req)')
    $j = $b.IndexOf('http_auth_check(')
    if ($i -lt 0) { $fail += "kiln_http_prehandler() never calls request_is_cross_origin()" }
    elseif ($j -ge 0 -and $i -gt $j) { $fail += "kiln_http_prehandler(): cross-origin check comes after http_auth_check()" }
    if ($b -notmatch 'req->method\s*!=\s*HTTP_GET\s*&&\s*req->method\s*!=\s*HTTP_HEAD') { $fail += "kiln_http_prehandler(): method gate is not 'not GET and not HEAD'" }
}
if ($fail.Count -gt 0) { $fail | ForEach-Object { Write-Host "FAIL: $_" }; exit 1 }
Write-Host "PASS: CSRF origin check is wired in the auth pre-handler and the recovery origin_guard"
exit 0
