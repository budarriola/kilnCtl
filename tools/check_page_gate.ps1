# check_page_gate.ps1 -- KG-01 (owner 2026-10-19): with web auth on, the ESP must not
# serve any HTML page other than the dashboard (+ login + shared assets) to a request
# without a session. The gate is ONE place: kiln_http_prehandler() (http_auth_http.c)
# answers a denied page-shell GET with send_page_gate_redirect() (302 to /?next=<page>).
# This check keeps a new page route from slipping past it:
#   A. every non-/api/ GET row tiered OPEN in route_tier_table.h is on the reviewed
#      list of dashboard/login/shared-asset URIs (a new OPEN page = a leak);
#   B. every other non-/api/ GET row is a kPageShellUris entry (so the gate redirects
#      it) unless it is a reviewed non-page route; every kPageShellUris entry has a
#      non-OPEN GET row;
#   C. the pre-handler's two DENY branches call send_page_gate_redirect() and no
#      is_page_shell_get() branch runs the real handler any more.
param(
    [string]$TableFile,
    [string]$PrehandlerFile
)
$ErrorActionPreference = "Stop"
$http = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\drivers\http"
if (-not $TableFile) { $TableFile = Join-Path $http "route_tier_table.h" }
if (-not $PrehandlerFile) { $PrehandlerFile = Join-Path $http "http_auth_http.c" }

# Reviewed OPEN non-API GET URIs: the dashboard, the login page, the shared assets the
# dashboard loads, and /status (Wi-Fi state JSON, not a page).
$openAllow = @("/", "/login", "/app.js", "/nav.js", "/theme.css", "/status")
# Reviewed non-page, non-OPEN GET routes (JSON / script, not a page shell).
$nonPageAllow = @("/scan", "/networks", "/commissioning_shared.js")

function Get-Code([string]$Path) {
    $t = [IO.File]::ReadAllText((Resolve-Path $Path).Path)
    # One pass, leftmost wins, so a "/*" inside a // comment (or vice versa) cannot
    # swallow real code.
    return [regex]::Replace($t, '(?s)//[^\r\n]*|/\*.*?\*/', '')
}

$fail = New-Object System.Collections.Generic.List[string]
$table = Get-Code $TableFile
$rows = [regex]::Matches($table, 'ROUTE_TIER\(\s*"([^"]*)"\s*,\s*(HTTP_[A-Za-z0-9_]+)\s*,\s*(ROUTE_TIER_[A-Za-z0-9_]+)\s*\)')
$shells = @([regex]::Matches($table, 'PAGE_SHELL_URI\(\s*"(/[^"]*)"\s*\)') | ForEach-Object { $_.Groups[1].Value })
if ($rows.Count -lt 100) { $fail.Add("parsed only $($rows.Count) ROUTE_TIER rows -- check is blind") }
if ($shells.Count -lt 10) { $fail.Add("parsed only $($shells.Count) PAGE_SHELL_URI entries -- check is blind") }

$getTier = @{}
foreach ($m in $rows) {
    if ($m.Groups[2].Value -eq "HTTP_GET") { $getTier[$m.Groups[1].Value] = $m.Groups[3].Value }
}
foreach ($uri in $getTier.Keys) {
    if ($uri.StartsWith("/api/")) { continue }
    $tier = $getTier[$uri]
    if ($tier -eq "ROUTE_TIER_OPEN") {
        if ($openAllow -notcontains $uri) {
            $fail.Add("A: GET $uri is OPEN but not a reviewed dashboard/login/shared-asset URI -- a page served to logged-out viewers (KG-01)")
        }
    } elseif ($shells -notcontains $uri -and $nonPageAllow -notcontains $uri) {
        $fail.Add("B: GET $uri ($tier) is not in kPageShellUris -- add PAGE_SHELL_URI(`"$uri`") so the gate redirects it, or list it as a reviewed non-page route in this check")
    }
}
foreach ($s in $shells) {
    if (-not $getTier.ContainsKey($s)) { $fail.Add("B: kPageShellUris entry $s has no GET row") }
    elseif ($getTier[$s] -eq "ROUTE_TIER_OPEN") { $fail.Add("B: kPageShellUris entry $s is OPEN (nothing to gate)") }
}

$pre = Get-Code $PrehandlerFile
$calls = [regex]::Matches($pre, 'if\s*\(\s*is_page_shell_get\(ctx\)\s*\)\s*\{\s*return\s+send_page_gate_redirect\(req,\s*ctx\)\s*;')
if ($calls.Count -lt 2) {
    $fail.Add("C: expected both DENY branches to 'if (is_page_shell_get(ctx)) { return send_page_gate_redirect(req, ctx); }', found $($calls.Count)")
}
if ([regex]::IsMatch($pre, 'is_page_shell_get\(ctx\)\s*\)\s*\{[^}]{0,200}real_handler')) {
    $fail.Add("C: an is_page_shell_get() branch still runs the real handler -- a denied page would be served")
}
if (-not [regex]::IsMatch($pre, 'send_page_gate_redirect\(httpd_req_t[^)]*\)\s*\{(?s:.{0,600}?)"Location"')) {
    $fail.Add("C: send_page_gate_redirect() no longer sets a Location header")
}

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    Write-Host "check_page_gate: $($fail.Count) problem(s)"
    exit 1
}
Write-Host "check_page_gate: OK ($($getTier.Count) GET rows, $($shells.Count) page shells)"
exit 0
