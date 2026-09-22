# check_kiln_auth_config_isolation.ps1 -- mechanically enforces
# docs/WEB_AUTH_PLAN.md item 12b: "The credential record lives in its own
# namespace on its own partition (item 2), and no config operation reads or
# writes it."
#
# This is deliberately ONE property, not a list of per-case rules (item
# 12b's own "One property, not several rules" subsection): credentials live
# in NVS namespace `kiln_auth` on the default `nvs` partition, under keys
# `web_auth`/`lcd_auth`/`auth_policy` (item 2), and nothing that touches
# config -- save/clone/apply/rename/delete a slot, export/import a package,
# a whole-board backup/restore, a zones/prefs/profiles write, the `cfg`
# LittleFS dual-write, or any of the four factory-reset scopes -- may name
# any of those four identifiers. If that single fact holds, every
# case-by-case outcome item 12b lists (survives a slot switch, absent from a
# backup, untouched by a restore, untouched by every factory-reset scope)
# follows for free; if it stops holding, every one of those outcomes is at
# risk simultaneously, which is exactly why this check looks for the one
# property instead of enumerating the cases.
#
# 2026-09-16: as of this writing item 2 (credential storage itself) has not
# been implemented by any of the other parallel slices of this plan -- a
# repo-wide search for these four identifiers returns zero hits anywhere
# under firmware/. That is the expected, PASSING state today. This check
# exists so that changes on either side of that boundary in the future -- a
# config path that starts touching kiln_auth, OR the credential module
# itself landing without being added to the allowlist below -- fail loudly
# instead of silently.
#
# FAIL-CLOSED SHAPE, ON PURPOSE: this scans EVERY *.c file under
# firmware/KilnFW/App/drivers (the same tree check_uri_handler_cap.ps1 and
# check_route_tier_coverage.ps1 scan), not a hardcoded list of "the config
# files". A brand new config/backup/restore/factory-reset file added later
# is automatically in scope with zero maintenance here -- the failure mode
# this project has shipped before (check_uri_handler_cap.ps1's own header
# comment) is a hardcoded list quietly not covering a new file. The only way
# a file may reference one of the four identifiers is to be named, by
# basename, in $Allowlist below -- which is where item 2's own
# credential-storage module belongs once it exists (e.g. a future
# web_auth_store.c). Adding a basename there is a deliberate, reviewed
# step, never a default.
#
# Usage: powershell -File tools\check_kiln_auth_config_isolation.ps1 [-DriversDir <path>]
# -DriversDir is for smoke-testing against a scratch tree (see this script's
# own negative-test procedure, run by hand and never checked in as an
# automated negative test against production source -- see
# docs/WEB_AUTH_PLAN.md item 12b's acceptance/negative-test notes).
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

# The property's four identifiers, exactly as docs/WEB_AUTH_PLAN.md item 2
# spells them: the NVS namespace itself, plus its three blob keys. Checked
# as plain substrings (word-boundary regex below) rather than requiring an
# exact `nvs_open(..., "kiln_auth", ...)` call shape, because the property
# this check enforces is "no config file even NAMES these identifiers" --
# narrowing the pattern to one call shape would miss a `#define` alias, a
# comment-adjacent reference inside a string, or a namespace constant handed
# to hal_kv_open() through a macro, all of which land on config storage in
# spirit even if this check's own comment-stripper (below) exempts a bare
# comment from counting.
$forbiddenIdentifiers = @("kiln_auth", "web_auth", "lcd_auth", "auth_policy")

# Basenames explicitly permitted to reference the identifiers above --
# because they ARE the credential-storage module item 2 describes, not a
# config path. Empty today: item 2 has not landed in this tree yet (see
# header comment). When it does, its file(s) belong here, added
# deliberately and reviewed, never as a default outcome of this check
# failing to find them.
#
# web_auth_store.c/.h: this IS item 2's credential-storage module (docs/
# WEB_AUTH_PLAN.md secs 2/3/11) -- kiln_auth is its own dedicated NVS
# namespace, deliberately distinct from kiln_nvs/wifi_nvs/profiles_nvs (the
# only three factory-reset ever erases), and web_auth/lcd_auth/auth_policy
# are its three blob keys. No config/backup/restore/factory-reset path may
# reference them; this file is the one place that legitimately does.
$Allowlist = @("web_auth_store.c", "web_auth_store.h")

# Per-line escape hatch for a REFUSAL, not a real reference: a line carrying
# this exact marker comment is read as "this line names the identifier only
# to reject/deny it" -- e.g. nvs_keys_get_handler()'s deliberate 403 on the
# kiln_auth namespace in diagnostics_http.c (`if (strcmp(ns_raw, "kiln_auth")
# == 0) { /* kiln_auth-isolation: refusal */`). This is narrower than
# allowlisting the whole file (which would blind the check to a real future
# leak anywhere else in that same file) and smaller/less gameable than
# parsing the strcmp/httpd_resp_send_err call shape structurally. The marker
# is matched against the RAW source line, not the comment-stripped code
# line below (comments are stripped before the identifier scan runs, so the
# marker itself has to be read from the original text) -- moving or
# duplicating a refusal without carrying the marker across the copy is
# exactly what turns it back into an ordinary finding.
$RefusalMarker = "kiln_auth-isolation: refusal"

# Same comment-stripping helper as check_uri_handler_cap.ps1 /
# check_bridge_reject_reason.ps1 / check_uart_version_independence.ps1
# (duplicated rather than imported -- this project has no shared PowerShell
# module mechanism).
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

$sourceFiles = Get-ChildItem -Path $driversDir -Filter "*.c" -File -Recurse

# Blindness floor, same shape as check_uri_handler_cap.ps1's 80-route floor:
# if this ever counts fewer than 50 .c files, the tree has moved or the
# glob is broken, and a clean pass on zero files scanned must not read as
# "the property holds".
if ($sourceFiles.Count -lt 50) {
    throw "check_kiln_auth_config_isolation.ps1: only found $($sourceFiles.Count) .c file(s) under $driversDir -- implausibly low (59+ expected as of 2026-09-16). The drivers tree has probably moved; update this script's target directory before trusting its result."
}

$findings = @()
foreach ($f in $sourceFiles) {
    if ($Allowlist -contains $f.Name) {
        continue
    }
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    $rawLines = Get-Content -Path $f.FullName
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($rawLines[$i] -match [regex]::Escape($RefusalMarker)) {
            continue
        }
        foreach ($id in $forbiddenIdentifiers) {
            # Word-boundary match so e.g. a hypothetical "kiln_auth_test"
            # symbol still counts (it is still naming the identifier), but
            # nothing shorter or unrelated coincidentally matches.
            if ($codeLines[$i] -match "\b$([regex]::Escape($id))\b") {
                $findings += [PSCustomObject]@{
                    File = $f.FullName
                    Line = $i + 1
                    Identifier = $id
                    Text = $codeLines[$i].Trim()
                }
            }
        }
    }
}

Write-Host "kiln_auth config isolation check: scanned $($sourceFiles.Count) .c file(s) under $driversDir for {$($forbiddenIdentifiers -join ', ')}, $($Allowlist.Count) file(s) allowlisted."

if ($findings.Count -gt 0) {
    Write-Host "KILN_AUTH CONFIG ISOLATION CHECK FAILED:" -ForegroundColor Red
    Write-Host "  docs/WEB_AUTH_PLAN.md item 12b: credentials must live outside all config storage --" -ForegroundColor Red
    Write-Host "  no config, backup, restore, or factory-reset path may reference the kiln_auth" -ForegroundColor Red
    Write-Host "  namespace or its web_auth/lcd_auth/auth_policy keys. Found:" -ForegroundColor Red
    foreach ($finding in $findings) {
        Write-Host ("    {0}:{1}: references '{2}' -- {3}" -f $finding.File, $finding.Line, $finding.Identifier, $finding.Text) -ForegroundColor Red
    }
    throw "kiln_auth config isolation violated by $($findings.Count) reference(s) above. If this file IS item 2's credential-storage module (not a config path), add its basename to `$Allowlist in this script deliberately; otherwise remove the reference from the config path that added it."
}

Write-Host "kiln_auth config isolation check passed: no config path under $driversDir references kiln_auth/web_auth/lcd_auth/auth_policy."
exit 0
