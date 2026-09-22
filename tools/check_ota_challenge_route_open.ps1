# check_ota_challenge_route_open.ps1 -- pins GET /api/ota/challenge to
# ROUTE_TIER_OPEN in route_tier_table.h.
#
# app.js's kcOtaGetChallenge() (firmware/KilnFW/App/drivers/http/app.js)
# calls this route with no session/credential and depends on it answering
# OPEN -- the login modal's OTA-auth retry path (see app.js's own comment on
# kcOtaGetChallenge()/kcOtaAuthedFetch()) fetches a fresh nonce+salt BEFORE a
# caller has any way to be authenticated yet. route_tier_table.h's own
# comment on this row explains why OPEN is safe here (the nonce plus the
# administrator record's salt/iteration count are useless without the
# administrator password itself, plan section 1 item 2b) -- this check exists
# only to catch a future edit that tightens the row to ADMIN (or removes it)
# without anyone noticing the web client silently breaks, since
# check_route_tier_coverage.ps1 only proves every route HAS a tier, never
# that it has the RIGHT one.
#
# Same comment-stripping/regex idiom as check_route_tier_coverage.ps1
# (reused here rather than reimplemented) -- dot-source it for
# Get-CodeOnlyLines/Get-TieredKeys.
param(
    [string]$TableFile
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$coverageScript = Join-Path $root "check_route_tier_coverage.ps1"
. $coverageScript

if (-not $TableFile) {
    $TableFile = Join-Path $root "..\firmware\KilnFW\App\drivers\http\route_tier_table.h"
}
if (-not (Test-Path $TableFile)) {
    throw "check_ota_challenge_route_open.ps1: -TableFile '$TableFile' does not exist."
}
$TableFile = (Resolve-Path $TableFile).Path

# Production entry point -- also called directly by the negative test
# (dot-sourced) against a synthetic file, same convention as
# Invoke-RouteTierCoverageScan above.
function Invoke-OtaChallengeRouteOpenScan {
    param(
        [Parameter(Mandatory)][string]$TableFile
    )
    $tieredKeys = Get-TieredKeys -Path $TableFile
    $key = "HTTP_GET /api/ota/challenge"
    if (-not $tieredKeys.ContainsKey($key)) {
        throw "check_ota_challenge_route_open.ps1: no ROUTE_TIER row found for $key in $TableFile at all -- has it been renamed or removed? app.js's kcOtaGetChallenge() depends on this route answering with no credential."
    }
    return $tieredKeys[$key]
}

if ($MyInvocation.InvocationName -ne '.') {
    $tier = Invoke-OtaChallengeRouteOpenScan -TableFile $TableFile

    Write-Host "OTA challenge route tier check: GET /api/ota/challenge is $tier in $TableFile."

    if ($tier -ne "ROUTE_TIER_OPEN") {
        throw "check_ota_challenge_route_open.ps1: GET /api/ota/challenge is $tier, not ROUTE_TIER_OPEN -- app.js's kcOtaGetChallenge() (firmware/KilnFW/App/drivers/http/app.js) calls this route with no session and the login modal's OTA-auth retry depends on it staying reachable with no credential (route_tier_table.h's own comment on this row explains why OPEN is safe: the nonce plus the administrator record's salt/iteration count are useless without the administrator password itself). If this tier was tightened deliberately, update app.js's caller and this check together."
    }

    Write-Host "OTA challenge route tier check passed."
    exit 0
}
