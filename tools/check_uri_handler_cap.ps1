# checkcache: ok
# check_uri_handler_cap.ps1 -- keeps wifi_provision_http.c's
# config.max_uri_handlers ahead of the real number of httpd_uri_t routes this
# firmware can register in one boot.
#
# esp_http_server allocates hd->hd_calls = calloc(config.max_uri_handlers,
# sizeof(httpd_uri_t *)) once, at httpd_start() (esp-idf
# components/esp_http_server/src/httpd_main.c, httpd_create()). Every
# registration past that cap is refused with ESP_ERR_HTTPD_HANDLERS_FULL --
# logged by this file's REGISTER_OR_LOG macro and by every other *_http.c
# module's own ESP_LOGE-and-continue path, but NOT fatal, so the boot
# continues and the only visible symptom is that one route 404s with no
# obvious cause. This exact bug reached the bench on 2026-08-24
# (commissioning/bench_preset 404, commit 750dc33) because the cap
# (max_uri_handlers = 84) had fallen behind the tree's real route count --
# the FOURTH time this has happened (previous bumps: 59->72, 72->80, 80->84,
# all in this same file's comment block above the assignment). A comment
# saying "keep this ahead of the real count" has now failed three times in a
# row; this script is the fourth attempt, and it is a script instead of prose
# specifically because prose has a 100% failure rate on this exact bug so far.
#
# What "the real number of routes" means here: every httpd_uri_t literal
# (`.uri = "..."`) anywhere under firmware/KilnFW/App/drivers/*.c. That is
# deliberately every ROUTE FILE in the directory, not a hardcoded list of
# "the *_http.c modules" -- board_temps.c, factory_reset.c and sim_backend.c
# register routes too and do not follow the *_http.c naming convention, and a
# hardcoded file list would silently stop covering a brand new module the same
# way the old comment-only approach stopped covering brand new routes. This
# also deliberately counts EVERY `.uri = "..."` regardless of #if/#ifdef --
# including sim_backend.c's 2 routes, which only compile in under
# CONFIG_KILNCTL_SIM_PLANT -- because the cap is a single compile-time array
# size shared by every build configuration; the guard must hold for the
# worst-case build (SIM_PLANT enabled), not merely the default one. Comments
# are stripped first (same helper as check_bridge_reject_reason.ps1 /
# check_uart_version_independence.ps1), so a comment that merely shows or
# discusses a `.uri = "..."` line -- as this project's own comment history
# does more than once -- is not counted as a route.
#
# Usage: powershell -File tools\check_uri_handler_cap.ps1 [-DriversDir <path>]
# -DriversDir is for smoke-testing against a simulated post-layer-move tree
# (or any other alternate root); normal use omits it and gets the real repo
# path.
param(
    [string]$DriversDir
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($DriversDir) {
    $driversDir = (Resolve-Path $DriversDir).Path
} else {
    $driversDir = Join-Path $root "..\firmware\KilnFW\App\drivers"
    $driversDir = (Resolve-Path $driversDir).Path
}
# Resolve a bare basename to a file anywhere under $driversDir. Fails loud (not
# silently picks one) if a name is missing or ambiguous -- same shape as the
# Resolve-DriverFile helper in check_host_embed_symbols_defined.ps1 and
# check_nvs_write_guard_coverage.ps1 (duplicated rather than imported -- this
# project has no shared PowerShell module mechanism).
function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "check_uri_handler_cap.ps1: expected cap file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? Update this script's path."
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_uri_handler_cap.ps1: '$BaseName' matched more than one file under $DriversDir ($paths) -- this script cannot tell which one is the cap file. Disambiguate."
    }
    return $found[0].FullName
}

$capFile = Resolve-DriverFile -DriversDir $driversDir -BaseName "wifi_provision_http.c"

# Same comment-stripping helper as check_bridge_reject_reason.ps1 and
# check_uart_version_independence.ps1 (duplicated rather than imported -- this
# project has no shared PowerShell module mechanism).
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $result += $code
    }
    return $result
}

$uriPattern = '\.uri\s*=\s*"'
$sourceFiles = Get-ChildItem -Path $driversDir -Filter "*.c" -File -Recurse
if ($sourceFiles.Count -lt 5) {
    throw "check_uri_handler_cap.ps1: only $($sourceFiles.Count) .c file(s) found under $driversDir -- has the routing code moved? Update this script's target directory."
}

$totalRoutes = 0
$perFile = @{}
foreach ($f in $sourceFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $count = 0
    foreach ($line in $codeLines) {
        $count += ([regex]::Matches($line, $uriPattern)).Count
    }
    if ($count -gt 0) {
        $perFile[$f.Name] = $count
        $totalRoutes += $count
    }
}

# Sanity floor: this tree carries 85+ routes today (2026-08-24 recount). If the
# pattern above ever stops matching real registrations -- httpd_uri_t
# literals move to a different shape, get built via a macro/table instead of
# individual structs, etc. -- this check must go loud rather than quietly
# start passing on an undercount.
if ($totalRoutes -lt 80) {
    throw "check_uri_handler_cap.ps1: only counted $totalRoutes total '.uri = ""..""' routes under $driversDir, which is implausibly low (85+ expected as of 2026-08-24) -- the registration style has probably changed and this script has gone blind. Update its pattern before trusting its result."
}

$capLines = Get-CodeOnlyLines -Path $capFile
$capDefinePattern = 'config\.max_uri_handlers\s*=\s*(\d+)\s*;'
$capValue = $null
foreach ($line in $capLines) {
    if ($line -match $capDefinePattern) {
        $capValue = [int]$Matches[1]
        break
    }
}
if ($null -eq $capValue) {
    throw "check_uri_handler_cap.ps1: no 'config.max_uri_handlers = <N>;' assignment found in $capFile -- has it been renamed or restructured? Update this script's pattern."
}

# ROUTE_TIER_REVIEW_2026-10-09 LOW-2: http_auth_http.c's KILN_HTTP_MAX_ROUTES
# (the wrapper table) must be >= max_uri_handlers, else routes the httpd
# would accept are refused by the wrapper.
$authFile = Resolve-DriverFile -DriversDir $driversDir -BaseName "http_auth_http.c"
$maxRoutes = $null
foreach ($line in (Get-CodeOnlyLines -Path $authFile)) {
    if ($line -match '^\s*#\s*define\s+KILN_HTTP_MAX_ROUTES\s+(\d+)') { $maxRoutes = [int]$Matches[1]; break }
}
if ($null -eq $maxRoutes) {
    throw "check_uri_handler_cap.ps1: no '#define KILN_HTTP_MAX_ROUTES <N>' found in $authFile -- update this script's pattern."
}
if ($maxRoutes -lt $capValue -or $maxRoutes -lt $totalRoutes) {
    throw "URI HANDLER CAP CHECK FAILED: KILN_HTTP_MAX_ROUTES ($maxRoutes in $authFile) must be >= max_uri_handlers ($capValue) and >= registered routes ($totalRoutes)."
}

Write-Host "URI handler cap check: $totalRoutes route(s) registered across $($perFile.Count) file(s) under drivers/, cap (max_uri_handlers) = $capValue."

if ($capValue -lt $totalRoutes) {
    Write-Host "URI HANDLER CAP CHECK FAILED:" -ForegroundColor Red
    Write-Host "  max_uri_handlers = $capValue in $capFile" -ForegroundColor Red
    Write-Host "  but $totalRoutes routes are registered across drivers/*.c:" -ForegroundColor Red
    foreach ($name in ($perFile.Keys | Sort-Object)) {
        Write-Host "    $name : $($perFile[$name])" -ForegroundColor Red
    }
    throw "max_uri_handlers ($capValue) is below the real worst-case route count ($totalRoutes) -- httpd_register_uri_handler() will silently fail (ESP_ERR_HTTPD_HANDLERS_FULL, logged but non-fatal) for whichever routes register after the table fills, and the user-visible symptom is an unexplained 404. Raise config.max_uri_handlers in $capFile to at least $totalRoutes plus headroom, same as every previous bump documented in the comment block above that assignment."
}

$headroom = $capValue - $totalRoutes
if ($headroom -lt 4) {
    Write-Host "URI handler cap check passed, but headroom is thin ($headroom spare slots for $totalRoutes routes against a cap of $capValue) -- the next route added anywhere under drivers/*.c may need another bump soon." -ForegroundColor Yellow
} else {
    Write-Host "URI handler cap check passed: $headroom spare slot(s)."
}
exit 0
